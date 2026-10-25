/* pep.h: PEP mode B's messages, destinations and tables (story 025).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "pep.h"
#include "test.h"

static void test_codec(void)
{
	uint8_t b[64], a4[4] = { 172, 30, 1, 2 }, a6[16] = { 0x20, 0x01, 0x0d, 0xb8 };
	struct cg_pep_msg m;
	struct cg_st_ack a = { .aseq = 0xbeef, .cum = 0x100000005ull, .edge = 0x100100000ull, .n = 2 }, u;
	size_t n;

	n = cg_pep_put_open(b, 0x00020003, 4, a4, 5201);
	CHECK_EQ(n, 13);
	CHECK(cg_pep_is(b, n));
	CHECK_EQ(cg_pep_parse(b, n, &m), 0);
	CHECK_EQ(m.type, CG_PEP_OPEN);
	CHECK_EQ(m.id, 0x00020003);
	CHECK_EQ(m.family, 4);
	CHECK_EQ(m.port, 5201);
	CHECK(!memcmp(m.addr, a4, 4));
	CHECK_EQ(cg_pep_parse(b, n - 1, &m), -1);
	n = cg_pep_put_open(b, 9, 6, a6, 443);
	CHECK_EQ(n, 25);
	CHECK_EQ(cg_pep_parse(b, n, &m), 0);
	CHECK(!memcmp(m.addr, a6, 16));

	n = cg_pep_put_data(b, 7, CG_PEPF_FIN | CG_PEPF_EST, 0x1234567890ull, 0xdeadbeef);
	memcpy(b + n, "hello", 5);
	CHECK_EQ(cg_pep_parse(b, n + 5, &m), 0);
	CHECK_EQ(m.type, CG_PEP_DATA);
	CHECK_EQ(m.flags, CG_PEPF_FIN | CG_PEPF_EST);
	CHECK_EQ(m.off, 0x34567890u);
	CHECK_EQ(m.ts, 0xdeadbeefu);
	CHECK_EQ(m.len, 5);
	CHECK(!memcmp(m.data, "hello", 5));

	a.s[0] = 0x100000100ull;
	a.e[0] = 0x100000200ull;
	a.s[1] = 0x100000300ull;
	a.e[1] = 0x100000400ull;
	n = cg_pep_put_ack(b, 7, CG_PEPF_EST, &a, -1234);
	CHECK_EQ(n, 21 + 16);
	CHECK(n <= CG_PEP_ACK_MAX);
	CHECK_EQ(cg_pep_parse(b, n, &m), 0);
	CHECK_EQ(m.type, CG_PEP_ACK);
	CHECK_EQ(m.aseq, 0xbeef);
	CHECK_EQ(m.owd, -1234);
	CHECK_EQ(m.n, 2);
	cg_pep_ack_of(&m, 0x100000000ull, 0x1000f0000ull, &u);
	CHECK_EQ(u.cum, a.cum);
	CHECK_EQ(u.edge, a.edge);
	CHECK_EQ(u.s[1], a.s[1]);
	CHECK_EQ(u.e[0], a.e[0]);
	b[20] = CG_ST_ACK_RANGES + 1;
	CHECK_EQ(cg_pep_parse(b, n, &m), -1);

	n = cg_pep_put_rst(b, 7, CG_PEP_R_REFUSED);
	CHECK_EQ(cg_pep_parse(b, n, &m), 0);
	CHECK_EQ(m.type, CG_PEP_RST);
	CHECK_EQ(m.reason, CG_PEP_R_REFUSED);
	CHECK(!strcmp(cg_pep_reason(CG_PEP_R_REFUSED), "refused"));

	/* an IP packet is not a stream message */
	b[0] = 0x45;
	CHECK(!cg_pep_is(b, 20));
	b[0] = 0x60;
	CHECK(!cg_pep_is(b, 40));
	b[0] = 0xF9;
	CHECK_EQ(cg_pep_parse(b, 20, &m), -1); /* no such type */
}

static void test_dest(void)
{
	uint8_t a[16];

	memcpy(a, (uint8_t[]){ 8, 8, 8, 8 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), 0);
	memcpy(a, (uint8_t[]){ 127, 0, 0, 1 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), CG_PEP_R_DENIED);
	memcpy(a, (uint8_t[]){ 169, 254, 169, 254 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), CG_PEP_R_DENIED);
	memcpy(a, (uint8_t[]){ 0, 0, 0, 0 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), CG_PEP_R_DENIED);
	memcpy(a, (uint8_t[]){ 239, 1, 1, 1 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), CG_PEP_R_DENIED);
	memcpy(a, (uint8_t[]){ 10, 79, 0, 1 }, 4);
	CHECK_EQ(cg_pep_dest_ok(4, a), 0); /* private: the VPS's own network is the operator's call */
	memset(a, 0, 16);
	CHECK_EQ(cg_pep_dest_ok(6, a), CG_PEP_R_DENIED); /* :: */
	a[15] = 1;
	CHECK_EQ(cg_pep_dest_ok(6, a), CG_PEP_R_DENIED); /* ::1 */
	a[15] = 2;
	CHECK_EQ(cg_pep_dest_ok(6, a), 0);
	a[10] = a[11] = 0xff;
	a[12] = 127;
	CHECK_EQ(cg_pep_dest_ok(6, a), CG_PEP_R_DENIED); /* ::ffff:127.x */
	a[12] = 1;
	CHECK_EQ(cg_pep_dest_ok(6, a), 0);
	memset(a, 0, 16);
	a[0] = 0xfe;
	a[1] = 0x80;
	a[15] = 1;
	CHECK_EQ(cg_pep_dest_ok(6, a), CG_PEP_R_DENIED);
	a[0] = 0xff;
	a[1] = 0x02;
	CHECK_EQ(cg_pep_dest_ok(6, a), CG_PEP_R_DENIED);
	a[0] = 0x20;
	CHECK_EQ(cg_pep_dest_ok(6, a), 0);
}

static void test_tab(void)
{
	uint64_t key[16] = { 0 };
	uint32_t val[16];
	struct cg_pep_tab t;

	cg_pep_tab_init(&t, key, val, 16);
	CHECK_EQ(cg_pep_tab_find(&t, cg_pep_key(1, 2)), CG_PEP_TAB_EMPTY);
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pep_tab_put(&t, cg_pep_key(0xabc, cg_pep_id(1, (uint16_t)i)), i), 0);
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pep_tab_find(&t, cg_pep_key(0xabc, cg_pep_id(1, (uint16_t)i))), i);
	CHECK_EQ(cg_pep_tab_find(&t, cg_pep_key(0xabd, cg_pep_id(1, 0))), CG_PEP_TAB_EMPTY);
	for (uint32_t i = 0; i < 8; i += 2)
		cg_pep_tab_del(&t, cg_pep_key(0xabc, cg_pep_id(1, (uint16_t)i)));
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_pep_tab_find(&t, cg_pep_key(0xabc, cg_pep_id(1, (uint16_t)i))),
			 i % 2 ? i : CG_PEP_TAB_EMPTY);
	/* churn: never full, never lost */
	for (uint32_t r = 0; r < 1000; r++) {
		uint64_t k = cg_pep_key(r, r * 7);

		CHECK_EQ(cg_pep_tab_put(&t, k, r), 0);
		CHECK_EQ(cg_pep_tab_find(&t, k), r);
		cg_pep_tab_del(&t, k);
		CHECK_EQ(cg_pep_tab_find(&t, k), CG_PEP_TAB_EMPTY);
	}
	for (uint32_t i = 1; i < 8; i += 2)
		CHECK_EQ(cg_pep_tab_find(&t, cg_pep_key(0xabc, cg_pep_id(1, (uint16_t)i))), i);
	CHECK_EQ(cg_pep_next_gen(0xffff), 1);
	CHECK_EQ(cg_pep_id(2, 3), 0x20003);
}

static void test_edge(void)
{
	/* the destination takes it all: the whole buffer */
	CHECK_EQ(cg_pep_edge(1000, 1500, 100000, 50000), 101000);
	/* it does not: the backlog alone, the window closes */
	CHECK_EQ(cg_pep_edge(1000, 61000, 100000, 50000), 51000);
	/* a backlog over the buffer is the buffer */
	CHECK_EQ(cg_pep_edge(1000, 61000, 100000, 200000), 101000);
	CHECK_EQ(cg_pep_backlog(1000000, 200, 65536), 200000);
	CHECK_EQ(cg_pep_backlog(1000, 200, 65536), 65536);
}

void test_pep(void)
{
	test_codec();
	test_dest();
	test_tab();
	test_edge();
}
