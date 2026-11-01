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
	c.bw = c.bmax[0] = 40 * MBIT; /* as if STARTUP had found it */
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
	/* past STARTUP, queued, bw follows what the link delivers down (its
	 * mean, test_lc_mean) */
	cg_lc_owd(&c, t, 1000000);
	cg_lc_owd(&c, t, 1000000 + 50 * MS);
	for (int i = 0; i < 50; i++) {
		t += 50 * MS;
		cg_lc_acked(&c, t, 10 * MBIT / 20, 10 * MBIT, 40 * MS);
	}
	CHECK(c.bw > 9 * MBIT && c.bw <= 10 * MBIT);
	/* never under the floor */
	for (int i = 0; i < 100; i++) {
		t += 50 * MS;
		cg_lc_acked(&c, t, 50, 1000, 40 * MS);
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
	/* the queue in bytes: what is in flight over bw x rtprop */
	c.inflight = cg_lc_bw(&c) * c.rtprop / 1000000 + 5000;
	CHECK_EQ(cg_lc_queued(&c), 5000);
	c.inflight = 0;
	/* bursts: samples of 8 Mbit/s with a queue cap the 20 Mbit/s one at 10 */
	c.bw = 20 * MBIT; /* past STARTUP bw is the delivery's mean (test_lc_mean) */
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

/* satq (the VPS's download): the cap holds only while the link holds a
 * queue; without it (the router's upload) always. */
static void test_lc_satq(void)
{
	struct cg_lc c;
	uint64_t t = 1 * MS;

	cg_lc_init(&c);
	c.satq = 1;
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
	/* without satq it holds */
	c.satq = 0;
	CHECK_EQ(cg_lc_bw(&c), c.sat * 5 / 4);
}

/* MEAN: past STARTUP, bw is the delivery's mean of 0.5 s, not its bursts'
 * largest sample; queued it follows the link down, without a queue what it
 * was sent only raises it. */
static void test_lc_mean(void)
{
	struct cg_lc c;
	uint64_t t = 1 * MS;

	cg_lc_init(&c);
	c.target_us = 60 * MS;
	c.mode = CG_LC_PROBE_M;
	c.cycle = 2;
	cg_lc_owd(&c, t, 1000000);
	CHECK_EQ(cg_lc_mean(&c), 0); /* nothing yet */
	/* 40 Mbit/s on average, in bursts: 10 ms of 200, 40 of nothing, each
	 * burst sampled at 200 Mbit/s; queued (80 ms of queue) */
	for (int i = 0; i < 40; i++) {
		cg_lc_owd(&c, t, 1000000 + 80 * MS);
		cg_lc_acked(&c, t, 250000, 200 * MBIT, 40 * MS);
		t += 50 * MS;
	}
	CHECK(c.bw > 36 * MBIT && c.bw < 44 * MBIT); /* the max filter read 200 */
	/* the link slows to 20 while queued: bw follows it down */
	for (int i = 0; i < 20; i++) {
		cg_lc_owd(&c, t, 1000000 + 80 * MS);
		cg_lc_acked(&c, t, 125000, 100 * MBIT, 40 * MS);
		t += 50 * MS;
	}
	CHECK(c.bw > 18 * MBIT && c.bw < 22 * MBIT);
	/* no queue, less sent (10): bw stays; more sent (30): bw rises */
	for (int i = 0; i < 20; i++) {
		cg_lc_owd(&c, t, 1000000 + 5 * MS);
		cg_lc_acked(&c, t, 62500, 10 * MBIT, 40 * MS);
		t += 50 * MS;
	}
	CHECK(c.bw > 18 * MBIT && c.bw < 22 * MBIT);
	for (int i = 0; i < 20; i++) {
		cg_lc_owd(&c, t, 1000000 + 5 * MS);
		cg_lc_acked(&c, t, 187500, 30 * MBIT, 40 * MS);
		t += 50 * MS;
	}
	CHECK(c.bw > 27 * MBIT && c.bw < 33 * MBIT);
	/* a silence of a second starts the mean over */
	t += 1000 * MS;
	cg_lc_acked(&c, t, 1000, 1 * MBIT, 40 * MS);
	CHECK_EQ(cg_lc_mean(&c), 0);
	CHECK(c.bw > 27 * MBIT); /* kept until the mean has its 0.5 s again */
}

/* FEC v3's guard by the link's own queue once known, and a
 * tick's loss by its subflow's counters. */
static void test_lc_f3(void)
{
	struct cg_lc c;
	uint64_t ps = 0, pl = 0;

	cg_lc_init(&c);
	CHECK_EQ(cg_lc_f3_queue(&c, 7000), 7000); /* no delay of its own yet */
	c.have_owd = 1;
	c.q_us = 42000;
	CHECK_EQ(cg_lc_f3_queue(&c, 7000), 42000);
	CHECK_EQ(cg_lc_tick_loss(1000, 10, &ps, &pl), 10);    /* 10 of 1000: 10 per mille */
	CHECK_EQ(cg_lc_tick_loss(1000, 10, &ps, &pl), -1);    /* nothing sent this tick */
	CHECK_EQ(cg_lc_tick_loss(1500, 260, &ps, &pl), 500);  /* 250 of 500 */
	CHECK_EQ(cg_lc_tick_loss(1600, 500, &ps, &pl), 1000); /* more lost than sent (late SFLOSS): capped */
	CHECK_EQ(ps, 1600);
	CHECK_EQ(pl, 500);
}

void test_linkcc(void)
{
	test_lc_f3();
	test_lc_mean();
	test_lc_satq();
	test_lc_queue();
	test_lc_startup();
	test_lc_loss();
	test_lc_rtt();
}
