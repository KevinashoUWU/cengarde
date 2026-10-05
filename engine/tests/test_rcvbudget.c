/* SPDX-License-Identifier: GPL-2.0-only */
#include "rcvbudget.h"
#include "test.h"

#define MIB (1024 * 1024)

void test_rcvbudget(void)
{
	uint64_t m[3], small[3] = { 8192, 12288, 16384 }, gb1[3] = { 24576, 32768, 49152 };
	struct cg_rcvbudget b;
	uint32_t n;

	/* Parsing /proc/sys/net/ipv4/udp_mem. */
	CHECK_EQ(cg_udp_mem_parse("383415\t511221\t766830\n", m), 0);
	CHECK(m[0] == 383415 && m[1] == 511221 && m[2] == 766830);
	CHECK_EQ(cg_udp_mem_parse("1 2 3", m), 0);
	CHECK_EQ(cg_udp_mem_parse("1 2", m), -1);
	CHECK_EQ(cg_udp_mem_parse("1 2 3 4", m), -1);
	CHECK_EQ(cg_udp_mem_parse("1 x 3", m), -1);
	CHECK_EQ(cg_udp_mem_parse("1 -2 3", m), -1);
	CHECK_EQ(cg_udp_mem_parse("1 0 3", m), -1); /* nothing to share */
	CHECK_EQ(cg_udp_mem_parse("", m), -1);

	/* The kernel's default from RAM, when the sysctl is out of sight:
	 * 16 GB (MemTotal 16480972 kB here, udp_mem[1] 511221) within 1 %. */
	cg_udp_mem_estimate(16480972ull * 1024, 4096, m);
	CHECK(m[1] >= 511221 && m[1] <= 511221 + 511221 / 100);
	CHECK(m[0] == m[1] / 4 * 3 && m[2] == m[0] * 2);
	cg_udp_mem_estimate(1ull << 30, 4096, m); /* 1 GB: 32768 pages */
	CHECK_EQ(m[1], 32768);
	cg_udp_mem_estimate(1 << 20, 4096, m); /* the floor */
	CHECK(m[1] == 128 && m[0] == 96 && m[2] == 192);
	cg_udp_mem_estimate(1 << 20, 0, m);
	CHECK_EQ(m[1], 128);

	/* Who counts: the lanes of groups with a router, one WireGuard socket
	 * per router, the junk socket. */
	CHECK_EQ(cg_rcvbudget_sockets(8, 1, 1, 1), 10);
	CHECK_EQ(cg_rcvbudget_sockets(1, 1, 1, 0), 2); /* lanes = 1: one socket, no junk */
	CHECK_EQ(cg_rcvbudget_sockets(8, 4, 4, 1), 37);
	CHECK_EQ(cg_rcvbudget_sockets(8, 2, 4, 1), 21); /* two routers per group: their groups only */

	/* 16 GB (this lab's udp_mem): nothing capped. */
	CHECK_EQ(cg_udp_mem_parse("383415\t511221\t766830\n", m), 0);
	b = cg_rcvbudget(m, 4096, 10, 4 * MIB);
	CHECK_EQ(b.budget, 511221ull * 4096 / 2);
	CHECK_EQ(b.sockets, 10);
	CHECK_EQ(b.per_socket, 4 * MIB);
	CHECK_EQ(b.capped, 0);
	b = cg_rcvbudget(m, 4096, 37, 4 * MIB);
	CHECK_EQ(b.capped, 0);
	/* 32 MiB (the lab's big buffers) for one router fits; for four
	 * routers it is capped at about 13.5 MiB. */
	b = cg_rcvbudget(m, 4096, 10, 32 * MIB);
	CHECK_EQ(b.capped, 0);
	b = cg_rcvbudget(m, 4096, 37, 32 * MIB);
	CHECK(b.capped && b.per_socket == (int)(511221ull * 4096 / 2 / 74));

	/* 1 GB (udp_mem[1] = RAM / 8): one router, 3.2 MiB each, so that the
	 * ten sockets hold at most 64 MiB once the kernel doubles them. */
	b = cg_rcvbudget(gb1, 4096, 10, 4 * MIB);
	CHECK_EQ(b.budget, 64ull * MIB);
	CHECK_EQ(b.per_socket, 64 * MIB / 20);
	CHECK_EQ(b.capped, 1);
	CHECK(2ull * (uint64_t)b.per_socket * b.sockets <= b.budget);
	/* Four routers in four groups: 37 sockets of about 0.86 MiB. */
	b = cg_rcvbudget(gb1, 4096, 37, 4 * MIB);
	CHECK_EQ(b.per_socket, 64 * MIB / 74);
	CHECK(2ull * (uint64_t)b.per_socket * b.sockets <= b.budget);
	/* lanes = 1 there: two sockets, 4 MiB fits. */
	b = cg_rcvbudget(gb1, 4096, 2, 4 * MIB);
	CHECK_EQ(b.per_socket, 4 * MIB);
	CHECK_EQ(b.capped, 0);

	/* 512 MB: about 15 000 pages of pressure threshold. */
	m[0] = 11250, m[1] = 15000, m[2] = 22500;
	b = cg_rcvbudget(m, 4096, 10, 4 * MIB);
	CHECK_EQ(b.budget, 15000ull * 4096 / 2);
	CHECK_EQ(b.per_socket, (int)(15000ull * 4096 / 2 / 20));
	CHECK_EQ(b.capped, 1);

	/* The lab's udpmem scenario: 8 lanes + WireGuard + junk in 24 MiB. */
	b = cg_rcvbudget(small, 4096, 10, 4 * MIB);
	CHECK_EQ(b.budget, 24ull * MIB);
	CHECK(b.capped && 2ull * (uint64_t)b.per_socket * 10 <= 24ull * MIB);

	/* A configured value below the share stays; 0 (the kernel's default)
	 * is never capped; unknown udp_mem caps nothing; tiny shares stop at
	 * the floor. */
	b = cg_rcvbudget(gb1, 4096, 10, 1 * MIB);
	CHECK(b.per_socket == 1 * MIB && !b.capped);
	b = cg_rcvbudget(gb1, 4096, 10, 0);
	CHECK(b.per_socket == 0 && !b.capped);
	b = cg_rcvbudget(NULL, 4096, 10, 4 * MIB);
	CHECK(b.budget == 0 && b.per_socket == 4 * MIB && !b.capped && b.sockets == 10);
	m[0] = 1, m[1] = 2, m[2] = 3;
	b = cg_rcvbudget(m, 4096, 10, 4 * MIB);
	CHECK(b.per_socket == CG_RCVBUF_FLOOR && b.capped);
	/* 64 KiB pages (some ARM kernels). */
	b = cg_rcvbudget(gb1, 65536, 10, 64 * MIB);
	CHECK_EQ(b.budget, 32768ull * 65536 / 2);
	n = 10;
	CHECK(2ull * (uint64_t)b.per_socket * n <= b.budget);
}
