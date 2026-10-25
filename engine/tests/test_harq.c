/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "harq.h"
#include "test.h"

/* The hint written once the copies are decided: a packet left on one link
 * by its copies' mask says so; split, its pick; mute_trickle never alone. */
static void test_harq_only(void)
{
	CHECK_EQ(cg_harq_only(-1, 0x3, 0, 0xffff, 0), -1);   /* every carrier */
	CHECK_EQ(cg_harq_only(-1, 0x3, 0, 0x1, 0), 0);       /* copies dropped: link 0 alone */
	CHECK_EQ(cg_harq_only(-1, 0x6, 0, 0x4, 0), 2);
	CHECK_EQ(cg_harq_only(-1, 0x3, 0, 0x1, 1), -1);      /* trickle copies it elsewhere */
	CHECK_EQ(cg_harq_only(1, 0x3, 0, 0xffff, 0), 1);     /* split */
	CHECK_EQ(cg_harq_only(1, 0x7, 0x4, 0xffff, 0), -1);  /* split with a weight-0 copy */
	CHECK_EQ(cg_harq_only(1, 0x7, 0x4, 0x2, 0), 1);      /* that copy dropped */
	CHECK_EQ(cg_harq_only(-1, 0x3, 0, 0, 0), -1);        /* on none */
	CHECK_EQ(cg_harq_only(-1, 0x3, 0, 0x4, 0), -1);      /* the mask on a link that does not carry */
}

static void test_parq_wire(void)
{
	struct cg_parq_req q[CG_PARQ_MAX], r[CG_PARQ_MAX];
	uint8_t b[CG_PARQ_LEN];
	unsigned len;

	memset(q, 0, sizeof(q));
	q[0] = (struct cg_parq_req){ .kind = CG_REP_RLC, .d = 2, .base = 0xfffffffe, .map = 0x8000000000000005ull };
	q[1] = (struct cg_parq_req){ .kind = CG_REP_RS, .m = 8, .d = 1, .base = 77, .map = 0xfffff, .have = 0x85 };
	len = cg_parq_write(b, q, 2);
	CHECK_EQ(len, 1 + 2 * CG_PARQ_REQ);
	CHECK_EQ(cg_parq_read(r, b, len), 2);
	CHECK_EQ(r[0].kind, CG_REP_RLC);
	CHECK_EQ(r[0].d, 2);
	CHECK_EQ(r[0].m, 0);
	CHECK_EQ(r[0].base, 0xfffffffe);
	CHECK(r[0].map == 0x8000000000000005ull);
	CHECK_EQ(r[1].kind, CG_REP_RS);
	CHECK_EQ(r[1].m, 8);
	CHECK_EQ(r[1].have, 0x85);
	CHECK_EQ(r[1].map, 0xfffff);
	/* malformed */
	CHECK_EQ(cg_parq_read(r, b, len - 1), -1);
	CHECK_EQ(cg_parq_read(r, b, 1), -1);
	b[0] = 0;
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[0] = 2;
	b[1 + CG_PARQ_REQ + 1] = 21; /* RS m over its k of 20 */
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[1 + CG_PARQ_REQ + 1] = 8;
	b[1 + 2] = 0; /* d 0 */
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[1 + 2] = 2;
	b[1 + 15] &= 0xfe; /* map without bit 0 */
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[1 + 15] |= 1;
	b[1] = 9; /* unknown kind */
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[1] = CG_REP_RLC;
	b[1 + 16] = 1; /* RLC with a have */
	CHECK_EQ(cg_parq_read(r, b, len), -1);
	b[1 + 16] = 0;
	CHECK_EQ(cg_parq_read(r, b, len), 2);
}

/* The RS geometry leaves room above m, within k and CG_RS_MMAX; the
 * indices asked for come from the top, never one the receiver has. */
static void test_rs_geometry(void)
{
	uint8_t idx[CG_RS_MMAX];

	CHECK_EQ(cg_rs_mhdr(21, 3), 8);
	CHECK_EQ(cg_rs_mhdr(21, 4), 8);
	CHECK_EQ(cg_rs_mhdr(64, 5), 16);
	CHECK_EQ(cg_rs_mhdr(64, 20), 32);
	CHECK_EQ(cg_rs_mhdr(64, 32), 32);
	CHECK_EQ(cg_rs_mhdr(5, 3), 5);   /* within k */
	CHECK_EQ(cg_rs_mhdr(2, 2), 2);
	CHECK_EQ(cg_rs_mhdr(1, 1), 1);
	CHECK_EQ(cg_parq_rs_pick(0x5, 8, 2, idx), 2);
	CHECK_EQ(idx[0], 7);
	CHECK_EQ(idx[1], 6);
	CHECK_EQ(cg_parq_rs_pick(0xc0, 8, 3, idx), 3);
	CHECK_EQ(idx[0], 5);
	CHECK_EQ(idx[2], 3);
	CHECK_EQ(cg_parq_rs_pick(0xff, 8, 3, idx), 0); /* none left */
	CHECK_EQ(cg_parq_rs_pick(0xfd, 8, 3, idx), 1);
	CHECK_EQ(idx[0], 1);
}

/* The route of a gap: the code first, then its deficit, the packet, and
 * nothing past TCP's budget. */
static void test_harq_route(void)
{
	struct cg_harq_gap g = { .horizon_us = 100000, .eta_us = 20000 };

	/* no code: the packet */
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_NACK);
	g.here = 1;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_NONE);
	g.here = 0;
	/* a code whose repairs may still come: wait */
	g.code = 1;
	g.age_us = 10000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_WAIT);
	/* sealed: with a deficit, the parity; without, the packet */
	g.sealed = 1;
	g.deficit = 2;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_PARQ);
	g.deficit = 0;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_NACK);
	/* not sealed but out of time: the same */
	g.sealed = 0;
	g.age_us = 100000;
	g.deficit = 1;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_PARQ);
	/* TCP's budget: an answer that cannot land within it is TCP's */
	g.budget_us = 110000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_TCP);
	g.age_us = 10000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_WAIT); /* may still wait */
	g.age_us = 90000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_TCP);  /* waiting more leaves no time to ask */
	CHECK(cg_harq_hold(0));
	CHECK(!cg_harq_hold(25000));
	g.code = 0;
	g.age_us = 0;
	g.budget_us = 25000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_NACK); /* 0 + 20 within 25 */
	g.age_us = 6000;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_TCP);
	g.here = 1;
	CHECK_EQ(cg_harq_route(&g), CG_HARQ_NONE);
	/* the budget and the wait */
	CHECK_EQ(cg_harq_budget_us(0), 0);
	CHECK_EQ(cg_harq_budget_us(20000), 25000);
	CHECK_EQ(cg_harq_wait_us(0, 100000, 0), 0);
	CHECK_EQ(cg_harq_wait_us(60000, 100000, 0), 160000);
	CHECK_EQ(cg_harq_wait_us(60000, 0, 0), 60000);
	CHECK_EQ(cg_harq_wait_us(60000, 100000, 25000), 25000);
	CHECK_EQ(cg_harq_wait_us(60000, 0, 200000), 60000);
}

void test_harq(void)
{
	test_harq_only();
	test_parq_wire();
	test_rs_geometry();
	test_harq_route();
}
