/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "split.h"
#include "test.h"

#define S 1000000ull /* us per second */

/* A tickle starts every every_us on the next due link, in turn, lasts
 * CG_TICKLE_WIN_US, and takes packets up to its rate. */
static void test_tickle_turns(void)
{
	struct cg_tickle t;
	uint64_t bps[CG_MAX_LINKS] = { 0 }, cap[CG_MAX_LINKS] = { 0 }, now = 10 * S;
	int got = 0;

	cg_tickle_reset(&t);
	bps[1] = 8000000;  /* 8 Mbit/s */
	bps[3] = 100000;   /* under the minimum: 1 Mbit/s */
	cg_tickle_tick(&t, now, 0, 0xa, bps, cap); /* every 0: never */
	CHECK_EQ(t.link, -1);
	cg_tickle_tick(&t, now, 2 * S, 0, bps, cap); /* nothing due */
	CHECK_EQ(t.link, -1);
	cg_tickle_tick(&t, now, 2 * S, 0xa, bps, cap);
	CHECK_EQ(t.link, 1);
	CHECK_EQ(t.bps, 8000000);
	CHECK_EQ(t.count, 1);
	/* 8 Mbit/s for 100 ms: 100 kB, 100 packets of 1000 B; a 3rd link not in set */
	CHECK_EQ(cg_tickle_take(&t, 0x5, 1000, now + 1000), -1);
	for (uint64_t u = now; u < now + 100000; u += 500)
		got += cg_tickle_take(&t, 0xf, 1000, u) == 1;
	CHECK(got >= 95 && got <= 102);
	/* past the window: over, and no new one before every_us */
	CHECK_EQ(cg_tickle_take(&t, 0xf, 1000, now + CG_TICKLE_WIN_US), -1);
	cg_tickle_tick(&t, now + CG_TICKLE_WIN_US, 2 * S, 0xa, bps, cap);
	CHECK_EQ(t.link, -1);
	cg_tickle_tick(&t, now + 2 * S, 2 * S, 0xa, bps, cap);
	CHECK_EQ(t.link, 3); /* the next in turn */
	CHECK_EQ(t.bps, CG_TICKLE_MIN_BPS);
	cg_tickle_tick(&t, now + 6 * S, 2 * S, 0xa, bps, cap);
	cg_tickle_tick(&t, now + 6 * S, 2 * S, 0xa, bps, cap);
	CHECK_EQ(t.link, 1); /* round again, after its backoff (2 s doubled) */
	/* the bucket never holds more than CG_TICKLE_BURST_US of the rate */
	got = 0;
	for (int i = 0; i < 1000; i++)
		got += cg_tickle_take(&t, 0xf, 1000, now + 6 * S + 400000) == 1;
	CHECK(got <= 21);
}

/* In the split: a tickled link takes the packets first (counted in the ETA's
 * backlog), with CG_F_SPLIT; outside the set it does not. */
static void test_tickle_split(void)
{
	struct cg_split sp;
	struct cg_eta e;
	struct cg_tickle t;
	uint16_t w[CG_MAX_LINKS];
	uint64_t bps[CG_MAX_LINKS] = { 0 }, cap[CG_MAX_LINKS] = { 0 }, now = 10 * S;
	int n0 = 0, n1 = 0;

	memset(&sp, 0, sizeof(sp));
	cg_eta_reset(&e);
	cg_tickle_reset(&t);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		w[i] = 1000;
	cg_eta_set(&e, 0, 50000000, 0, 0, now);     /* fast, no extra delay */
	cg_eta_set(&e, 1, 10000000, 20000, 0, now); /* slow, 20 ms more */
	for (int i = 0; i < 100; i++) {
		uint8_t f = 0;
		int k = cg_split_pick_eta(&sp, &e, &t, now + (uint64_t)i * 200, 0x3, 0x3, 0, w, CG_WGS_DATA, 1000, 100, &f);

		n0 += k == 0;
		n1 += k == 1;
	}
	CHECK_EQ(n1, 0); /* the ETA alone never picks the slow link here */
	CHECK_EQ(n0, 100);
	CHECK_EQ(sp.n_by[CG_SPLIT_BY_TICKLE], 0);
	bps[1] = 20000000;
	now += S;
	cg_tickle_tick(&t, now, 2 * S, 0x2, bps, cap);
	n0 = n1 = 0;
	for (int i = 0; i < 100; i++) {
		uint8_t f = 0;
		int k = cg_split_pick_eta(&sp, &e, &t, now + (uint64_t)i * 200, 0x3, 0x3, 0, w, CG_WGS_DATA, 1000, 100, &f);

		CHECK(f & CG_F_SPLIT);
		n0 += k == 0;
		n1 += k == 1;
	}
	/* 20 Mbit/s of 40 offered (1000 B every 200 us) */
	CHECK(n1 >= 45 && n1 <= 55);
	CHECK_EQ(sp.n_by[CG_SPLIT_BY_TICKLE], (uint64_t)n1); /* counted as the tickle's (status split_by) */
	CHECK(e.l[1].backlog > 0);
	/* the link left the split's set: the tickle takes nothing */
	CHECK_EQ(cg_split_pick_eta(&sp, &e, &t, now + 30000, 0x1, 0x1, 0, w, CG_WGS_DATA, 1000, 100, NULL), -1);
}

/* Backoff: a tickle after which the capacity did not grow by 1/8 doubles
 * that link's wait, up to CG_TICKLE_MAX_SHIFT; one that grew resets it. A
 * zeroed struct judges nothing. */
static void test_tickle_backoff(void)
{
	struct cg_tickle t;
	uint64_t bps[CG_MAX_LINKS] = { 0 }, cap[CG_MAX_LINKS] = { 0 }, now = 10 * S, every = 2 * S;
	int starts = 0;

	memset(&t, 0, sizeof(t)); /* as a zeroed client or session: link 0 */
	bps[2] = 4000000;
	cap[2] = 3000000;
	cg_tickle_tick(&t, now, every, 0, bps, cap);
	CHECK_EQ(t.link, -1);
	for (int i = 0; i < 20; i++) /* nothing to judge on link 0 */
		cg_tickle_tick(&t, now + (uint64_t)i * S, every, 0, bps, cap);
	CHECK_EQ(t.shift[0], 0);
	CHECK_EQ(cg_tickle_backoff(&t), 0);
	/* link 2 never grows: 2, 4, 8, ... 64 s between tickles */
	now += 30 * S;
	for (uint64_t u = now; u < now + 400 * S; u += 100000) {
		uint64_t c0 = t.count;

		cg_tickle_tick(&t, u, every, 0x4, bps, cap);
		starts += t.count != c0;
	}
	CHECK_EQ(t.shift[2], CG_TICKLE_MAX_SHIFT);
	CHECK_EQ(cg_tickle_backoff(&t), 0x4);
	CHECK(starts >= 7 && starts <= 10); /* 2+4+8+16+32+64+64+64+64...: about 9 in 400 s */
	CHECK_EQ(t.grew, 0);
	/* it grows: back to every_us */
	now += 400 * S;
	for (uint64_t u = now; u < now + 70 * S && t.link < 0; u += 100000)
		cg_tickle_tick(&t, u, every, 0x4, bps, cap);
	CHECK_EQ(t.link, 2);
	cap[2] = 6000000;
	for (uint64_t u = t.until_us; u < t.until_us + 2 * S; u += 100000)
		cg_tickle_tick(&t, u, every, 0x4, bps, cap);
	CHECK_EQ(t.shift[2], 0);
	CHECK_EQ(t.grew, 1);
	CHECK_EQ(cg_tickle_backoff(&t), 0);
}

void test_tickle(void)
{
	test_tickle_turns();
	test_tickle_split();
	test_tickle_backoff();
}
