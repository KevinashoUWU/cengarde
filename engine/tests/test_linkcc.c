/* linkcc.h: each link's own rate.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "linkcc.h"
#include "test.h"

#define MS 1000u
#define MBIT 125000ull /* bytes/s */

/* STARTUP doubles toward what the link delivers, stops when bw no longer
 * grows by a quarter, drains, then cruises; the window covers the round
 * trip and the queue target. */
static void test_lc_startup(void)
{
	struct cg_lc c;
	uint64_t t = 0, bw = 4 * MBIT;

	cg_lc_init(&c);
	CHECK_EQ(cg_lc_bw(&c), CG_LC_START);
	CHECK_EQ(c.mode, CG_LC_STARTUP_M);
	CHECK_EQ(cg_lc_rate(&c), 2 * CG_LC_START);
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

void test_linkcc(void)
{
	test_lc_startup();
	test_lc_loss();
	test_lc_rtt();
}
