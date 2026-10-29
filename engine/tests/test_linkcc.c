/* linkcc.h: each link's own rate.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "linkcc.h"
#include "test.h"

#define MS 1000u
#define MBIT 125000ull /* bytes/s */

/* STARTUP grows by half toward what the link delivers, stops when bw no longer
 * grows by a quarter, drains, then cruises; the window covers the round
 * trip and the queue target. */
static void test_lc_startup(void)
{
	struct cg_lc c;
	uint64_t t = 0, bw = 4 * MBIT;

	cg_lc_init(&c);
	CHECK_EQ(cg_lc_bw(&c), CG_LC_START);
	CHECK_EQ(c.mode, CG_LC_STARTUP_M);
	CHECK_EQ(cg_lc_rate(&c), 3 * CG_LC_START / 2);
	/* the link delivers what is offered up to 40 Mbit/s */
	for (int r = 0; r < 30; r++) {
		uint64_t offered = cg_lc_rate(&c), got = offered < 40 * MBIT ? offered : 40 * MBIT;

		t += 40 * MS;
		cg_lc_acked(&c, t, 0, got, 40 * MS);
		bw = c.bw;
	}
	CHECK(bw >= 38 * MBIT && bw <= 40 * MBIT);
	CHECK(c.mode != CG_LC_STARTUP_M);
	CHECK_EQ(c.rtprop, 40 * MS);
	/* nothing in flight: DRAIN went to PROBE */
	CHECK_EQ(c.mode, CG_LC_PROBE_M);
	CHECK(cg_lc_rate(&c) >= c.bw * 3 / 4 && cg_lc_rate(&c) <= c.bw * 5 / 4);
	/* the window: twice bw x (rtprop + target) */
	CHECK_EQ(cg_lc_cwnd(&c, 50 * MS), 2 * c.bw * 90 * MS / 1000000);
	CHECK(cg_lc_room(&c, 1400, 50 * MS));
	c.inflight = cg_lc_cwnd(&c, 50 * MS);
	CHECK(!cg_lc_room(&c, 1400, 50 * MS));
}

/* What the link delivers lowers bw only after the window forgets the
 * larger samples; a round losing more than 5 % cuts it at once. */
static void test_lc_loss(void)
{
	struct cg_lc c;
	uint64_t t = 0, before;

	cg_lc_init(&c);
	cg_lc_seed(&c, 40 * MBIT);
	CHECK_EQ(c.bw, 40 * MBIT);
	cg_lc_acked(&c, t, 0, 40 * MBIT, 40 * MS);
	/* a round with 3 lost of 100: no cut */
	for (int i = 0; i < 100; i++)
		cg_lc_sent(&c, 1400);
	for (int i = 0; i < 3; i++)
		cg_lc_lost(&c, 1400);
	t += 50 * MS;
	before = c.bw;
	cg_lc_acked(&c, t, 97 * 1400, 40 * MBIT, 40 * MS);
	CHECK_EQ(c.bw, before);
	CHECK_EQ(c.cuts, 0);
	CHECK_EQ(c.inflight, 0);
	/* a round with 10 of 100 lost: 7/8, out of STARTUP */
	for (int i = 0; i < 100; i++)
		cg_lc_sent(&c, 1400);
	for (int i = 0; i < 10; i++)
		cg_lc_lost(&c, 1400);
	t += 50 * MS;
	cg_lc_acked(&c, t, 90 * 1400, 40 * MBIT, 40 * MS);
	CHECK_EQ(c.cuts, 1);
	CHECK_EQ(c.bw, 40 * MBIT * 7 / 8);
	CHECK(c.mode != CG_LC_STARTUP_M);
	/* too few packets in a round say nothing */
	for (int i = 0; i < 10; i++)
		cg_lc_sent(&c, 1400);
	for (int i = 0; i < 5; i++)
		cg_lc_lost(&c, 1400);
	t += 50 * MS;
	cg_lc_acked(&c, t, 5 * 1400, 35 * MBIT, 40 * MS);
	CHECK_EQ(c.cuts, 1);
	/* after 2 s without larger samples bw follows the delivery down */
	for (int i = 0; i < 50; i++) {
		t += 50 * MS;
		cg_lc_acked(&c, t, 0, 10 * MBIT, 40 * MS);
	}
	CHECK_EQ(c.bw, 10 * MBIT);
	/* never under the floor */
	for (int i = 0; i < 100; i++) {
		t += 50 * MS;
		cg_lc_acked(&c, t, 0, 1000, 40 * MS);
	}
	CHECK_EQ(c.bw, CG_LC_FLOOR);
	CHECK_EQ(cg_lc_rate(&c) >= CG_LC_FLOOR, 1);
}

/* rtprop is the least round trip of 10 s; a larger one after that is
 * taken; srtt follows. */
static void test_lc_rtt(void)
{
	struct cg_lc c;

	cg_lc_init(&c);
	cg_lc_acked(&c, 1 * MS, 0, 0, 50 * MS);
	cg_lc_acked(&c, 2 * MS, 0, 0, 30 * MS);
	cg_lc_acked(&c, 3 * MS, 0, 0, 80 * MS);
	CHECK_EQ(c.rtprop, 30 * MS);
	cg_lc_acked(&c, 2 * MS + CG_LC_RTPROP_US, 0, 0, 60 * MS); /* taken at 2 ms: still within */
	CHECK_EQ(c.rtprop, 30 * MS);
	cg_lc_acked(&c, 2 * MS + CG_LC_RTPROP_US + 1, 0, 0, 60 * MS);
	CHECK_EQ(c.rtprop, 60 * MS);
	CHECK(c.srtt > 30 * MS && c.srtt < 80 * MS);
}

/* The queue by the one-way delay: STARTUP ends when it passes the target,
 * DRAIN when it is under half, PROBE drains over the target and probes only
 * well under it; what a queued link delivered caps bw at 5/4. */
static void test_lc_queue(void)
{
	struct cg_lc c;
	uint64_t t = 1 * MS; /* 0 is "no round yet" */

	cg_lc_init(&c);
	CHECK_EQ(c.target_us, CG_LC_TARGET_US);
	c.bits = CG_LC_QMODE | CG_LC_SAT | CG_LC_S32 | CG_LC_QETA; /* every use of the queue */
	c.target_us = 60 * MS;
	CHECK_EQ(cg_lc_qstate(&c), 0); /* not known */
	cg_lc_owd(&c, t, 1000000 + 20 * MS); /* two clocks apart: only changes count */
	CHECK_EQ(c.q_us, 0);
	CHECK_EQ(cg_lc_qstate(&c), -1);
	/* STARTUP while bw grows; the queue passes the target: DRAIN */
	cg_lc_acked(&c, t, 0, 2 * MBIT, 40 * MS);
	t += 40 * MS;
	cg_lc_owd(&c, t, 1000000 + 90 * MS);
	CHECK_EQ(c.q_us, 70 * MS);
	CHECK_EQ(cg_lc_qstate(&c), 1);
	cg_lc_acked(&c, t, 0, 4 * MBIT, 40 * MS);
	CHECK_EQ(c.mode, CG_LC_DRAIN_M);
	/* still over half the target: DRAIN; under it: PROBE */
	t += 40 * MS;
	cg_lc_owd(&c, t, 1000000 + 60 * MS);
	cg_lc_acked(&c, t, 0, 4 * MBIT, 40 * MS);
	CHECK_EQ(c.mode, CG_LC_DRAIN_M);
	t += 40 * MS;
	cg_lc_owd(&c, t, 1000000 + 30 * MS);
	cg_lc_acked(&c, t, 0, 4 * MBIT, 40 * MS);
	CHECK_EQ(c.mode, CG_LC_PROBE_M);
	/* the probe's phase: 5/4 only well under the target */
	c.cycle = 0;
	CHECK_EQ(cg_lc_gain(&c), 320);
	cg_lc_owd(&c, t, 1000000 + 70 * MS); /* 50 ms: between half and the target */
	CHECK_EQ(cg_lc_gain(&c), CG_LC_ONE);
	cg_lc_owd(&c, t, 1000000 + 100 * MS); /* over it: drain, whatever the phase */
	CHECK_EQ(cg_lc_gain(&c), 192);
	c.cycle = 3;
	CHECK_EQ(cg_lc_gain(&c), 192);
	/* the queue in bytes is its delay at the pacing rate */
	CHECK_EQ(cg_lc_queued(&c), cg_lc_rate(&c) * 80 * MS / 1000000);
	/* bursts: samples of 8 Mbit/s with a queue cap the 20 Mbit/s one at 10 */
	cg_lc_acked(&c, t, 0, 20 * MBIT, 40 * MS);
	CHECK_EQ(c.bw, 20 * MBIT);
	for (int i = 0; i < 8; i++)
		cg_lc_acked(&c, t + (uint64_t)i * MS, 0, 8 * MBIT, 40 * MS);
	CHECK(c.sat > 7 * MBIT && c.sat < 9 * MBIT);
	CHECK_EQ(cg_lc_bw(&c), c.sat * 5 / 4);
	/* the cap lapses without queued samples for CG_LC_SAT_US */
	cg_lc_owd(&c, t, 1000000 + 25 * MS);
	cg_lc_acked(&c, t + CG_LC_SAT_US + 10 * MS, 0, 0, 40 * MS);
	CHECK_EQ(c.sat, 0);
	/* the least moves on: after two buckets a new floor counts */
	cg_lc_owd(&c, t + CG_LC_OWD_WIN_US, 1000000 + 50 * MS);
	cg_lc_owd(&c, t + 2 * CG_LC_OWD_WIN_US, 1000000 + 45 * MS);
	CHECK_EQ(c.q_us, 0);
}

/* SATQ: the cap holds only while the link holds a queue; QMAX: the split's
 * queue is the larger of in flight and the delay's. */
static void test_lc_satq_qmax(void)
{
	struct cg_lc c;
	uint64_t t = 1 * MS;

	cg_lc_init(&c);
	c.bits = CG_LC_QMODE | CG_LC_SAT | CG_LC_SATQ;
	c.target_us = 60 * MS;
	cg_lc_owd(&c, t, 1000000);
	cg_lc_acked(&c, t, 0, 100 * MBIT, 40 * MS);
	/* queued: what it delivered caps bw */
	cg_lc_owd(&c, t, 1000000 + 40 * MS);
	for (int i = 0; i < 8; i++)
		cg_lc_acked(&c, t + (uint64_t)i * MS, 0, 20 * MBIT, 40 * MS);
	CHECK(c.sat > 19 * MBIT && c.sat < 21 * MBIT);
	CHECK_EQ(cg_lc_bw(&c), c.sat * 5 / 4);
	/* the queue gone: the cap with it */
	cg_lc_owd(&c, t + 10 * MS, 1000000 + 5 * MS);
	CHECK_EQ(cg_lc_bw(&c), 100 * MBIT);
	/* without SATQ it holds */
	c.bits = CG_LC_QMODE | CG_LC_SAT;
	CHECK_EQ(cg_lc_bw(&c), c.sat * 5 / 4);
	/* QMAX: in flight over the BDP, or the delay at the rate, the larger */
	c.bits = CG_LC_QMAX;
	c.rtprop = 40 * MS;
	c.inflight = cg_lc_bw(&c) * 40 * MS / 1000000 + 100000;
	cg_lc_owd(&c, t + 20 * MS, 1000000 + 2 * MS);
	CHECK_EQ(cg_lc_queued(&c), 100000); /* in flight: the larger */
	cg_lc_owd(&c, t + 30 * MS, 1000000 + 500 * MS);
	CHECK_EQ(cg_lc_queued(&c), cg_lc_rate(&c) * 500 * MS / 1000000); /* the delay */
	c.bits = 0;
	CHECK_EQ(cg_lc_queued(&c), 100000);
}

/* One MTPT epoch on a model link: its queue reaches q_ms (binding the
 * target when at least half of it), it delivers mbit Mbit/s, and loses
 * lost of sent packets. */
static void mtpt_epoch(struct cg_lc *c, uint64_t *t, uint32_t q_ms, uint64_t mbit, uint32_t sent, uint32_t lost)
{
	for (uint32_t i = 0; i < sent; i++)
		cg_lc_sent(c, 1400);
	for (uint32_t i = 0; i < lost; i++)
		cg_lc_lost(c, 1400);
	cg_lc_owd(c, *t + 500 * MS, 1000000 + (int32_t)q_ms * (int32_t)MS);
	*t += CG_LC_MTPT_EPOCH_US;
	cg_lc_acked(c, *t, mbit * MBIT, 0, 0);
}

/* MTPT: without the bit the caller's target is the target; with it the
 * target starts there and climbs while the link delivers more, turns
 * around at the knee and stays near it, falls two steps on a lossy epoch,
 * holds while it does not bind or delivers the same, and stays between the
 * caller's target and its ceiling. */
static void test_lc_mtpt(void)
{
	struct cg_lc c;
	uint64_t t = 1000;
	uint32_t lo = UINT32_MAX, hi = 0;

	cg_lc_init(&c);
	c.bits = CG_LC_QMODE;
	cg_lc_target(&c, 60 * MS, 300 * MS);
	CHECK_EQ(c.target_us, 60 * MS);
	c.bits |= CG_LC_MTPT;
	cg_lc_target(&c, 60 * MS, 300 * MS);
	CHECK_EQ(c.target_us, 60 * MS);
	CHECK_EQ(c.mt_cap_us, 300 * MS);
	cg_lc_owd(&c, t, 1000000); /* the least one-way delay */
	cg_lc_acked(&c, t, 0, 0, 0); /* the first epoch starts */
	/* a link that gives 40 Mbit/s with 100 ms of queue or more, less with
	 * less (a modem grants more as its queue grows) */
	for (int e = 0; e < 40; e++) {
		uint64_t mbit = c.target_us >= 100 * MS ? 40 : 40 * c.target_us / (100 * MS);

		mtpt_epoch(&c, &t, c.target_us / MS, mbit, 200, 0);
		cg_lc_target(&c, 60 * MS, 300 * MS); /* the caller, each tick: the base no longer resets it */
		if (e >= 10) {
			lo = c.target_us < lo ? c.target_us : lo;
			hi = c.target_us > hi ? c.target_us : hi;
		}
	}
	CHECK(lo >= 60 * MS && hi <= 160 * MS); /* around the knee, not up to the ceiling */
	CHECK(c.mt_up > 0);
	/* an epoch losing 10 %: two steps down */
	{
		uint32_t before = c.target_us;

		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 20);
		CHECK_EQ(c.target_us, before - 2 * CG_LC_MTPT_STEP_US);
		CHECK_EQ(c.mt_dir, -1);
	}
	/* a link with less to carry than its target allows: it holds */
	{
		uint32_t before = c.target_us;

		for (int e = 0; e < 5; e++)
			mtpt_epoch(&c, &t, 5, 10, 200, 0);
		CHECK_EQ(c.target_us, before);
	}
	/* the ceiling: a slower link's (the caller's budget less its lag) */
	cg_lc_target(&c, 60 * MS, 50 * MS);
	CHECK(c.target_us <= 50 * MS);
	for (int e = 0; e < 10; e++) {
		mtpt_epoch(&c, &t, c.target_us / MS, 10 + 10 * (uint64_t)e, 200, 0); /* always more */
		cg_lc_target(&c, 60 * MS, 50 * MS);
		CHECK(c.target_us <= 50 * MS);
	}
	/* never under the caller's target, however lossy */
	cg_lc_target(&c, 60 * MS, 300 * MS);
	for (int e = 0; e < 20; e++)
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 40);
	CHECK_EQ(c.target_us, 60 * MS);
	/* flat: it stays */
	{
		uint32_t before;

		for (int e = 0; e < 3; e++)
			mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		before = c.target_us;
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		CHECK_EQ(c.target_us, before);
	}
}

void test_linkcc(void)
{
	test_lc_mtpt();
	test_lc_satq_qmax();
	test_lc_queue();
	test_lc_startup();
	test_lc_loss();
	test_lc_rtt();
}
