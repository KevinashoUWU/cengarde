/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "govern.h"
#include "test.h"

#define MS 1000u /* microseconds per millisecond */

static const struct cg_gov_cfg CFG = { .target_us = 20 * MS, .window_ms = 10000 };

/* A delay report on link i, fresh for 3 s. */
static void delay(struct cg_hlink *h, unsigned i, uint32_t owd, uint64_t now_ms)
{
	h[i].owd = owd;
	h[i].have_owd = 1;
	h[i].fresh_until_ms = now_ms + 3000;
}

/* The windowed minimum: the lowest delay of the current and the previous
 * half window, so an old minimum is forgotten after one to two halves. */
static void test_govern_base(void)
{
	struct cg_gov_link l;
	uint64_t t = 1000;

	memset(&l, 0, sizeof(l));
	CHECK_EQ(cg_gov_base(&l, 50000, t, 10000), 50000);
	CHECK_EQ(cg_gov_base(&l, 40000, t += 100, 10000), 40000);
	CHECK_EQ(cg_gov_base(&l, 60000, t += 100, 10000), 40000);
	/* a new half window: 40000 is still the previous half's minimum */
	CHECK_EQ(cg_gov_base(&l, 70000, t += 5000, 10000), 40000);
	CHECK_EQ(cg_gov_base(&l, 65000, t += 100, 10000), 40000);
	/* and one more: forgotten */
	CHECK_EQ(cg_gov_base(&l, 68000, t += 5000, 10000), 65000);
	/* wrap-safe: raw delays around 2^32 */
	memset(&l, 0, sizeof(l));
	CHECK_EQ(cg_gov_base(&l, 0xfffff000u, t, 10000), 0xfffff000u);
	CHECK_EQ(cg_gov_base(&l, 0x00000800u, t += 100, 10000), 0xfffff000u); /* 6 ms later, past the wrap */
	CHECK_EQ(cg_gov_base(&l, 0xffffe000u, t += 100, 10000), 0xffffe000u);
}

/* Link 0 against link 1, which holds no queue (10 ms, its base): one tick. */
static void tick2(struct cg_gov *g, struct cg_hlink *h, uint32_t owd0, uint64_t t, int active)
{
	delay(h, 0, owd0, t);
	delay(h, 1, 10 * MS, t);
	cg_gov_tick(g, h, 0x3, active, t, &CFG);
}

/* The steps, on link 0's queue beyond the shortest (link 1's, 0): down by
 * 1/8 above the target, up by CG_GOV_UP below half of it, held in between;
 * the floor and the top; only while active, only for fresh candidates;
 * dup0 stays 0; all queues alike move nothing. */
static void test_govern_steps(void)
{
	struct cg_gov g;
	struct cg_hlink h[CG_MAX_LINKS];
	uint16_t w[CG_MAX_LINKS];
	uint64_t t = 1000;

	memset(h, 0, sizeof(h));
	cg_gov_reset(&g);
	cg_gov_weights(&g, 0, w);
	CHECK(w[0] == CG_GOV_MAX && w[15] == CG_GOV_MAX);

	/* link 0: base 10 ms, then 40 ms of queue: down by 1/8 per tick */
	tick2(&g, h, 10 * MS, t, 1);
	CHECK_EQ(g.l[0].queue_us, 0);
	tick2(&g, h, 50 * MS, t += 100, 1);
	CHECK_EQ(g.l[0].queue_us, 40 * MS);
	cg_gov_weights(&g, 0, w);
	CHECK(w[0] == 875 && w[1] == CG_GOV_MAX);
	for (int k = 0; k < 40; k++)
		tick2(&g, h, 50 * MS, t += 100, 1);
	cg_gov_weights(&g, 0, w);
	CHECK(w[0] == CG_GOV_MIN && w[1] == CG_GOV_MAX); /* the floor; link 1 at the top */

	/* 15 ms beyond: between half the target and the target, held */
	tick2(&g, h, 25 * MS, t += 100, 1);
	cg_gov_weights(&g, 0, w);
	CHECK_EQ(w[0], CG_GOV_MIN);
	/* 5 ms beyond: up by CG_GOV_UP per tick, to the top */
	tick2(&g, h, 15 * MS, t += 100, 1);
	cg_gov_weights(&g, 0, w);
	CHECK_EQ(w[0], CG_GOV_MIN + CG_GOV_UP);
	for (int k = 0; k < 60; k++)
		tick2(&g, h, 15 * MS, t += 100, 1);
	cg_gov_weights(&g, 0, w);
	CHECK_EQ(w[0], CG_GOV_MAX);

	/* the same queue on both: nothing beyond the shortest, nothing down */
	for (int k = 0; k < 10; k++) {
		delay(h, 0, 90 * MS, t += 100);
		delay(h, 1, 90 * MS, t);
		cg_gov_tick(&g, h, 0x3, 1, t, &CFG);
	}
	cg_gov_weights(&g, 0, w);
	CHECK(w[0] == CG_GOV_MAX && w[1] == CG_GOV_MAX);

	/* not active: the queue is measured, the weight does not move */
	tick2(&g, h, 200 * MS, t += 100, 0);
	cg_gov_weights(&g, 0, w);
	CHECK(w[0] == CG_GOV_MAX && g.l[0].queue_us > 100 * (int32_t)MS);
	/* not a candidate, or not fresh: untouched */
	cg_gov_tick(&g, h, 0x2, 1, t += 100, &CFG);
	CHECK_EQ(g.l[0].x, CG_GOV_MAX * CG_GOV_SCALE);
	cg_gov_tick(&g, h, 0x3, 1, t += 5000, &CFG); /* 5 s after the last reports */
	CHECK_EQ(g.l[0].x, CG_GOV_MAX * CG_GOV_SCALE);
	/* dup0 stays 0; a restarted link starts at the top */
	cg_gov_weights(&g, 0x8, w);
	CHECK(w[3] == 0 && w[0] == CG_GOV_MAX);
	g.l[2].x = CG_GOV_MIN * CG_GOV_SCALE;
	cg_gov_link_reset(&g, 2);
	cg_gov_weights(&g, 0, w);
	CHECK(w[2] == CG_GOV_MAX && !g.l[2].have_min);
}

/* Two links of 30 and 8 (Mbit/s, say) take 30 between them, split by the
 * weights, each behind a FIFO queue: the delay is the base plus the queue
 * over the rate. With equal weights the slow one is fed 15 and its queue
 * grows without end; the governor brings its share down to what it
 * carries and both queues to around the target. */
static void test_govern_converges(void)
{
	struct cg_gov g;
	struct cg_hlink h[CG_MAX_LINKS];
	uint16_t w[CG_MAX_LINKS];
	const double cap[2] = { 30.0, 8.0 }, load = 30.0, base[2] = { 20.0, 25.0 }; /* rates, base ms */
	double q[2] = { 0, 0 };                                                        /* queued, in Mbit */
	double maxq_late[2] = { 0, 0 }, share1 = 0;
	uint64_t t = 1000;

	memset(h, 0, sizeof(h));
	cg_gov_reset(&g);
	for (int k = 0; k < 600; k++) { /* 60 s of 100 ms ticks */
		double tot, owd_ms[2];

		cg_gov_weights(&g, 0, w);
		tot = (double)w[0] + w[1];
		for (int i = 0; i < 2; i++) {
			double in = load * w[i] / tot * 0.1, out = cap[i] * 0.1; /* Mbit in this tick */

			q[i] += in - out;
			if (q[i] < 0)
				q[i] = 0;
			owd_ms[i] = base[i] + q[i] / cap[i] * 1000.0;
			delay(h, (unsigned)i, (uint32_t)(owd_ms[i] * MS), t);
			if (k >= 300 && q[i] / cap[i] * 1000.0 > maxq_late[i])
				maxq_late[i] = q[i] / cap[i] * 1000.0;
		}
		if (k >= 300)
			share1 += w[1] / tot / 300.0;
		cg_gov_tick(&g, h, 0x3, 1, t, &CFG);
		t += 100;
	}
	/* the slow link ends with about its capacity's share (8/30 = 0.27,
	 * minus what the fast link has to spare), and nobody's queue runs away */
	CHECK(share1 > 0.12 && share1 < 0.32);
	CHECK(maxq_late[1] < 60.0);
	CHECK(maxq_late[0] < 60.0);
	/* equal weights for comparison: the slow link's queue runs away */
	{
		double qq = 0;

		for (int k = 0; k < 600; k++) {
			qq += (load / 2 - cap[1]) * 0.1;
		}
		CHECK(qq / cap[1] * 1000.0 > 50000.0);
	}
}

void test_govern(void)
{
	test_govern_base();
	test_govern_steps();
	test_govern_converges();
}
