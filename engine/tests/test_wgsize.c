/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "test.h"
#include "wgsize.h"

/* The bucket by its definition: the first edge len fits under. */
static unsigned by_edges(size_t len)
{
	unsigned i = 0;

	while (i < CG_WGS_N - 1 && len > cg_wgs_edge(i))
		i++;
	return i;
}

void test_wgsize(void)
{
	static const uint32_t edges[] = { 32, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1440 };
	struct cg_wgs w;
	uint8_t b[2048];
	unsigned bad = 0;

	/* The edges of the status (bench/fieldrec.py reads them there). */
	CHECK_EQ(sizeof(edges) / sizeof(edges[0]), CG_WGS_N - 1);
	for (unsigned i = 0; i < CG_WGS_N - 1; i++)
		CHECK_EQ(cg_wgs_edge(i), edges[i]);
	CHECK_EQ(cg_wgs_edge(CG_WGS_N - 2), CG_WGS_MAX);
	/* The table agrees with the edges for every size a datagram can have. */
	for (size_t len = 0; len <= 65535; len++)
		bad += cg_wgs_bucket(len) != by_edges(len);
	CHECK_EQ(bad, 0);
	/* Each edge belongs to its bucket, one byte more to the next. */
	CHECK_EQ(cg_wgs_bucket(32), 0); /* a keepalive */
	CHECK_EQ(cg_wgs_bucket(33), 1);
	CHECK_EQ(cg_wgs_bucket(80), 1);  /* an ACK over IPv4, no options */
	CHECK_EQ(cg_wgs_bucket(96), 1);  /* the same with timestamps */
	CHECK_EQ(cg_wgs_bucket(112), 2); /* over IPv6 with timestamps */
	CHECK_EQ(cg_wgs_bucket(1440), CG_WGS_N - 2);
	CHECK_EQ(cg_wgs_bucket(1441), CG_WGS_N - 1);
	CHECK_EQ(cg_wgs_bucket(1452), CG_WGS_N - 1); /* WireGuard's own 1420 MTU, full */

	/* Types by the first byte; data by size, packets and bytes. */
	memset(&w, 0, sizeof(w));
	memset(b, 0, sizeof(b));
	b[0] = 1;
	cg_wgs_add(&w, b, 148);
	b[0] = 2;
	cg_wgs_add(&w, b, 92);
	cg_wgs_add(&w, b, 92);
	b[0] = 3;
	cg_wgs_add(&w, b, 64);
	b[0] = 4;
	cg_wgs_add(&w, b, 32);
	cg_wgs_add(&w, b, 32);
	cg_wgs_add(&w, b, 96);
	cg_wgs_add(&w, b, 1440);
	cg_wgs_add(&w, b, 1452);
	cg_wgs_add(&w, b, 2000);
	b[0] = 0; /* not WireGuard: other, whatever its size */
	cg_wgs_add(&w, b, 1000);
	b[0] = 5;
	cg_wgs_add(&w, b, 32);
	b[0] = 0xff;
	cg_wgs_add(&w, b, 1);
	b[0] = 4;
	cg_wgs_add(&w, b, 0); /* empty: its first byte is not read */
	CHECK_EQ(w.msg[CG_WGS_INIT], 1);
	CHECK_EQ(w.msg[CG_WGS_RESPONSE], 2);
	CHECK_EQ(w.msg[CG_WGS_COOKIE], 1);
	CHECK_EQ(w.msg[CG_WGS_OTHER], 4);
	CHECK_EQ(w.data[0], 2);
	CHECK_EQ(w.data_bytes[0], 64);
	CHECK_EQ(w.data[1], 1);
	CHECK_EQ(w.data_bytes[1], 96);
	CHECK_EQ(w.data[CG_WGS_N - 2], 1);
	CHECK_EQ(w.data_bytes[CG_WGS_N - 2], 1440);
	CHECK_EQ(w.data[CG_WGS_N - 1], 2);
	CHECK_EQ(w.data_bytes[CG_WGS_N - 1], 1452 + 2000);
	for (unsigned i = 2; i < CG_WGS_N - 2; i++)
		CHECK_EQ(w.data[i], 0);
}
