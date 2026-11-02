/* protoctl.h: the protocontroller's messages, destinations and tables (story 025).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "protoctl.h"
#include "protoctlio.h"
#include "test.h"

static void test_codec(void)
{
	uint8_t b[64], a4[4] = { 172, 30, 1, 2 }, a6[16] = { 0x20, 0x01, 0x0d, 0xb8 };
	struct cg_pctl_msg m;
	struct cg_st_ack a = { .aseq = 0xbeef, .cum = 0x100000005ull, .edge = 0x100100000ull, .n = 2 }, u;
	size_t n;

	n = cg_pctl_put_open(b, 0x00020003, 4, a4, 5201);
	CHECK_EQ(n, 13);
	CHECK(cg_pctl_is(b, n));
	CHECK_EQ(cg_pctl_parse(b, n, &m), 0);
	CHECK_EQ(m.type, CG_PCTL_OPEN);
	CHECK_EQ(m.id, 0x00020003);
	CHECK_EQ(m.family, 4);
	CHECK_EQ(m.port, 5201);
	CHECK(!memcmp(m.addr, a4, 4));
	CHECK(cg_pctl_parse(b, n - 1, &m) < 0);
	n = cg_pctl_put_open(b, 9, 6, a6, 443);
	CHECK_EQ(n, 25);
	CHECK_EQ(cg_pctl_parse(b, n, &m), 0);
	CHECK(!memcmp(m.addr, a6, 16));

	n = cg_pctl_put_data(b, 7, CG_PCTLF_FIN | CG_PCTLF_EST, 0x1234567890ull, 0xdeadbeef);
	memcpy(b + n, "hello", 5);
	CHECK_EQ(cg_pctl_parse(b, n + 5, &m), 0);
	CHECK_EQ(m.type, CG_PCTL_DATA);
	CHECK_EQ(m.flags, CG_PCTLF_FIN | CG_PCTLF_EST);
	CHECK_EQ(m.off, 0x34567890u);
	CHECK_EQ(m.ts, 0xdeadbeefu);
	CHECK_EQ(m.len, 5);
	CHECK(!memcmp(m.data, "hello", 5));

	a.s[0] = 0x100000100ull;
	a.e[0] = 0x100000200ull;
	a.s[1] = 0x100000300ull;
	a.e[1] = 0x100000400ull;
	n = cg_pctl_put_ack(b, 7, CG_PCTLF_EST, &a, -1234);
	CHECK_EQ(n, 21 + 16);
	CHECK(n <= CG_PCTL_ACK_MAX);
	CHECK_EQ(cg_pctl_parse(b, n, &m), 0);
	CHECK_EQ(m.type, CG_PCTL_ACK);
	CHECK_EQ(m.aseq, 0xbeef);
	CHECK_EQ(m.owd, -1234);
	CHECK_EQ(m.n, 2);
	cg_pctl_ack_of(&m, 0x100000000ull, 0x1000f0000ull, &u);
	CHECK_EQ(u.cum, a.cum);
	CHECK_EQ(u.edge, a.edge);
	CHECK_EQ(u.s[1], a.s[1]);
	CHECK_EQ(u.e[0], a.e[0]);
	b[20] = CG_ST_ACK_RANGES + 1;
	CHECK_EQ(cg_pctl_parse(b, n, &m), -1); /* more ranges than allowed: malformed */

	n = cg_pctl_put_rst(b, 7, CG_PCTL_R_REFUSED);
	CHECK_EQ(cg_pctl_parse(b, n, &m), 0);
	CHECK(cg_pctl_parse(b, n - 1, &m) < 0);
	CHECK_EQ(m.type, CG_PCTL_RST);
	CHECK_EQ(m.reason, CG_PCTL_R_REFUSED);
	CHECK(!strcmp(cg_pctl_reason(CG_PCTL_R_REFUSED), "refused"));

	/* an IP packet is not a stream message */
	b[0] = 0x45;
	CHECK(!cg_pctl_is(b, 20));
	b[0] = 0x60;
	CHECK(!cg_pctl_is(b, 40));
	b[0] = 0xF9;
	CHECK_EQ(cg_pctl_parse(b, 20, &m), -1); /* no such type */
}

static void test_dest(void)
{
	uint8_t a[16];

	memcpy(a, (uint8_t[]){ 8, 8, 8, 8 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), 0);
	memcpy(a, (uint8_t[]){ 127, 0, 0, 1 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), CG_PCTL_R_DENIED);
	memcpy(a, (uint8_t[]){ 169, 254, 169, 254 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), CG_PCTL_R_DENIED);
	memcpy(a, (uint8_t[]){ 0, 0, 0, 0 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), CG_PCTL_R_DENIED);
	memcpy(a, (uint8_t[]){ 239, 1, 1, 1 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), CG_PCTL_R_DENIED);
	memcpy(a, (uint8_t[]){ 10, 79, 0, 1 }, 4);
	CHECK_EQ(cg_pctl_dest_ok(4, a), 0); /* private: the VPS's own network is the operator's call */
	memset(a, 0, 16);
	CHECK_EQ(cg_pctl_dest_ok(6, a), CG_PCTL_R_DENIED); /* :: */
	a[15] = 1;
	CHECK_EQ(cg_pctl_dest_ok(6, a), CG_PCTL_R_DENIED); /* ::1 */
	a[15] = 2;
	CHECK_EQ(cg_pctl_dest_ok(6, a), 0);
	a[10] = a[11] = 0xff;
	a[12] = 127;
	CHECK_EQ(cg_pctl_dest_ok(6, a), CG_PCTL_R_DENIED); /* ::ffff:127.x */
	a[12] = 1;
	CHECK_EQ(cg_pctl_dest_ok(6, a), 0);
	memset(a, 0, 16);
	a[0] = 0xfe;
	a[1] = 0x80;
	a[15] = 1;
	CHECK_EQ(cg_pctl_dest_ok(6, a), CG_PCTL_R_DENIED);
	a[0] = 0xff;
	a[1] = 0x02;
	CHECK_EQ(cg_pctl_dest_ok(6, a), CG_PCTL_R_DENIED);
	a[0] = 0x20;
	CHECK_EQ(cg_pctl_dest_ok(6, a), 0);
}

static void test_pfx(void)
{
	uint8_t p4[4] = { 10, 79, 0, 1 }, a4[4] = { 10, 79, 3, 7 }, p6[16] = { 0xfd, 0, 0, 0x79 }, a6[16];

	CHECK(cg_pctl_in_pfx(4, a4, p4, 16));
	CHECK(!cg_pctl_in_pfx(4, a4, p4, 24));
	CHECK(cg_pctl_in_pfx(4, a4, p4, 0));
	a4[1] = 78;
	CHECK(!cg_pctl_in_pfx(4, a4, p4, 16));
	CHECK(cg_pctl_in_pfx(4, a4, p4, 14)); /* 78 and 79 share 7 bits */
	CHECK(cg_pctl_in_pfx(4, a4, p4, 15));
	CHECK(!cg_pctl_in_pfx(4, a4, p4, 16));
	CHECK(cg_pctl_in_pfx(4, p4, p4, 99)); /* clamped to 32 */
	memcpy(a6, p6, 16);
	a6[15] = 2;
	CHECK(cg_pctl_in_pfx(6, a6, p6, 64));
	CHECK(!cg_pctl_in_pfx(6, a6, p6, 128));
	a6[7] = 1;
	CHECK(!cg_pctl_in_pfx(6, a6, p6, 64));
	CHECK(cg_pctl_in_pfx(6, a6, p6, 61));
}

static void test_tab(void)
{
	uint64_t key[16] = { 0 };
	uint32_t val[16];
	struct cg_pctl_tab t;

	cg_pctl_tab_init(&t, key, val, 16);
	CHECK_EQ(cg_pctl_tab_find(&t, cg_pctl_key(1, 2)), CG_PCTL_TAB_EMPTY);
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pctl_tab_put(&t, cg_pctl_key(0xabc, cg_pctl_id(1, (uint16_t)i)), i), 0);
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pctl_tab_find(&t, cg_pctl_key(0xabc, cg_pctl_id(1, (uint16_t)i))), i);
	CHECK_EQ(cg_pctl_tab_find(&t, cg_pctl_key(0xabd, cg_pctl_id(1, 0))), CG_PCTL_TAB_EMPTY);
	for (uint32_t i = 0; i < 8; i += 2)
		cg_pctl_tab_del(&t, cg_pctl_key(0xabc, cg_pctl_id(1, (uint16_t)i)));
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pctl_tab_find(&t, cg_pctl_key(0xabc, cg_pctl_id(1, (uint16_t)i))),
			 i % 2 ? i : CG_PCTL_TAB_EMPTY);
	/* churn: never full, never lost */
	for (uint32_t r = 0; r < 1000; r++) {
		uint64_t k = cg_pctl_key(r, r * 7);

		CHECK_EQ(cg_pctl_tab_put(&t, k, r), 0);
		CHECK_EQ(cg_pctl_tab_find(&t, k), r);
		cg_pctl_tab_del(&t, k);
		CHECK_EQ(cg_pctl_tab_find(&t, k), CG_PCTL_TAB_EMPTY);
	}
	for (uint32_t i = 1; i < 8; i += 2)
		CHECK_EQ(cg_pctl_tab_find(&t, cg_pctl_key(0xabc, cg_pctl_id(1, (uint16_t)i))), i);
	CHECK_EQ(cg_pctl_next_gen(0xffff), 1);
	CHECK_EQ(cg_pctl_id(2, 3), 0x20003);
}

static void test_edge(void)
{
	/* the destination takes it all: the whole buffer */
	CHECK_EQ(cg_pctl_edge(1000, 1500, 100000, 50000), 101000);
	/* it does not: the backlog alone, the window closes */
	CHECK_EQ(cg_pctl_edge(1000, 61000, 100000, 50000), 51000);
	/* a backlog over the buffer is the buffer */
	CHECK_EQ(cg_pctl_edge(1000, 61000, 100000, 200000), 101000);
	CHECK_EQ(cg_pctl_backlog(1000000, 200, 65536), 200000);
	CHECK_EQ(cg_pctl_backlog(1000, 200, 65536), 65536);
	/* the pool by the RAM: an eighth, from 64 to 1024 MB */
	CHECK_EQ(cg_pctl_mem_auto(0), 64);
	CHECK_EQ(cg_pctl_mem_auto(256ull << 20), 64);
	CHECK_EQ(cg_pctl_mem_auto(2048ull << 20), 256);
	CHECK_EQ(cg_pctl_mem_auto(7732388ull << 10), 943);
	CHECK_EQ(cg_pctl_mem_auto(64ull << 30), 1024);
}

/* What the rescue does with a kept packet a NACK names (cg_pctl_nack_kind). */
static void test_nack_kind(void)
{
	uint8_t b[256], ip[20] = { 0x45 };
	uint32_t id = 0, off = 0, ts = 0;
	struct cg_st_ack a = { .aseq = 3, .cum = 100, .edge = 200 };
	uint8_t a4[4] = { 192, 0, 2, 1 };
	size_t n;

	/* an IP packet: the rescue as always */
	CHECK_EQ(cg_pctl_nack_kind(ip, sizeof(ip), &id, &off, &ts), CG_PCTL_NACK_NOT);
	CHECK_EQ(cg_pctl_nack_kind(ip, 0, &id, &off, &ts), CG_PCTL_NACK_NOT);
	/* a DATA: its stream, offset and clock */
	n = cg_pctl_put_data(b, 0x00050007, CG_PCTLF_EST, 0x1234567890ull, 0xdeadbeef);
	memset(b + n, 0xab, 100);
	n += 100;
	CHECK_EQ(cg_pctl_nack_kind(b, n, &id, &off, &ts), CG_PCTL_NACK_DATA);
	CHECK_EQ(id, 0x00050007);
	CHECK_EQ(off, 0x34567890);
	CHECK_EQ(ts, 0xdeadbeef);
	/* too short for its header: sent again as before */
	CHECK_EQ(cg_pctl_nack_kind(b, CG_PCTL_DATA_HDR - 1, &id, &off, &ts), CG_PCTL_NACK_RESEND);
	CHECK_EQ(cg_pctl_nack_kind(b, 3, &id, &off, &ts), CG_PCTL_NACK_RESEND);
	/* an ACK: a fresh one instead */
	id = 0;
	n = cg_pctl_put_ack(b, 9, CG_PCTLF_EST, &a, 5);
	CHECK_EQ(cg_pctl_nack_kind(b, n, &id, &off, &ts), CG_PCTL_NACK_ACK);
	CHECK_EQ(id, 9);
	/* an OPEN, a RST: sent again */
	n = cg_pctl_put_open(b, 9, 4, a4, 80);
	CHECK_EQ(cg_pctl_nack_kind(b, n, &id, &off, &ts), CG_PCTL_NACK_RESEND);
	n = cg_pctl_put_rst(b, 9, CG_PCTL_R_PEER);
	CHECK_EQ(cg_pctl_nack_kind(b, n, &id, &off, &ts), CG_PCTL_NACK_RESEND);
}


/* The spread of the links a stream takes (task #77): the largest delay
 * behind the fastest among the known links of the set, capped. */
static void test_spread(void)
{
	int32_t b[16] = { 0, 40000, 300000, -5000, 590000 };

	CHECK_EQ(cg_pctl_spread(b, 0x1f, 0x1f, 16), 590000);
	CHECK_EQ(cg_pctl_spread(b, 0x0f, 0x1f, 16), 300000); /* link 4 not known */
	CHECK_EQ(cg_pctl_spread(b, 0x1f, 0x0b, 16), 40000);  /* links 0, 1, 3 carry */
	CHECK_EQ(cg_pctl_spread(b, 0x1f, 0x09, 16), 0);      /* ahead counts as none */
	CHECK_EQ(cg_pctl_spread(b, 0, 0x1f, 16), 0);
	b[5] = 5000000;
	CHECK_EQ(cg_pctl_spread(b, 0x3f, 0x3f, 16), CG_PCTL_SPREAD_MAX_US);
}

void test_protoctl(void)
{
	test_spread();
	test_codec();
	test_dest();
	test_tab();
	test_edge();
	test_pfx();
	test_nack_kind();
}
