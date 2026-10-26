/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "fec3.h"
#include "test.h"

/* When it protects: a split over two links or more, a lone link; not
 * redundancy over several. */
static void test_f3_mode(void)
{
	CHECK_EQ(cg_f3_mode_of(1, 4), CG_F3_BOND);
	CHECK_EQ(cg_f3_mode_of(1, 2), CG_F3_BOND);
	CHECK_EQ(cg_f3_mode_of(1, 1), CG_F3_SINGLE);
	CHECK_EQ(cg_f3_mode_of(0, 1), CG_F3_SINGLE);
	CHECK_EQ(cg_f3_mode_of(0, 3), CG_F3_OFF);
	CHECK_EQ(cg_f3_mode_of(1, 0), CG_F3_OFF);
}

/* The share holds: 10 % of 1400-byte DATA in repairs of 1440 bytes is one
 * every ~10.3 packets; a tail repair after the wait, one repair of debt at
 * most. */
static void test_f3_credit(void)
{
	struct cg_f3 f;
	unsigned rep = 0, tail = 0;
	uint64_t bytes = 0;

	memset(&f, 0, sizeof(f));
	CHECK(!cg_f3_due(&f, 1000, 10));
	for (unsigned i = 0; i < 10000; i++) {
		cg_f3_earn(&f, 1400, 10, 1000 + i);
		while (cg_f3_owed(&f)) {
			cg_f3_spend(&f, cg_f3_cost(1400));
			bytes += cg_f3_cost(1400);
			rep++;
		}
	}
	CHECK_EQ(cg_f3_cost(1400), 1440);
	CHECK(rep >= 965 && rep <= 975);
	CHECK(bytes * 1000 / (10000ull * 1400) >= 99 && bytes * 1000 / (10000ull * 1400) <= 100);
	/* a burst of 3 and nothing after: the tail repair at the wait, on
	 * debt; a second burst at once has no credit for another */
	memset(&f, 0, sizeof(f));
	for (unsigned i = 0; i < 3; i++)
		cg_f3_earn(&f, 1400, 10, 5000 + i);
	CHECK(!cg_f3_owed(&f));
	CHECK_EQ(cg_f3_due_at(&f, 10000), 15000);
	CHECK(!cg_f3_due(&f, 14999, 10000));
	CHECK(cg_f3_due(&f, 15000, 10000));
	cg_f3_spend(&f, 1440);
	tail++;
	CHECK(f.credit < 0);
	CHECK(!cg_f3_due(&f, 99999, 10000)); /* nothing waits */
	cg_f3_earn(&f, 1400, 10, 100000);
	CHECK(!cg_f3_due(&f, 200000, 10000)); /* in debt over a repair */
	for (unsigned i = 0; i < 6; i++)
		cg_f3_earn(&f, 1400, 10, 100001 + i);
	CHECK(!cg_f3_due(&f, 200000, 10000));
	cg_f3_earn(&f, 1400, 10, 100007); /* 3 + 8 packets: out of debt */
	CHECK(cg_f3_due(&f, 200000, 10000));
	CHECK_EQ(tail, 1);
	cg_f3_idle(&f);
	CHECK(!f.credit && !f.fresh_us);
}

/* Repairs follow the links' DATA: 3 to 1 over 400 picks; a link out of the
 * candidates takes none; with no weight yet, evenly. */
static void test_f3_pick(void)
{
	struct cg_f3 f;
	unsigned n[CG_F3_LINKS] = { 0 };

	memset(&f, 0, sizeof(f));
	CHECK_EQ(cg_f3_pick(&f, 0), -1);
	for (unsigned i = 0; i < 400; i++)
		n[cg_f3_pick(&f, 0x6)]++;
	CHECK_EQ(n[1], 200);
	CHECK_EQ(n[2], 200);
	memset(&f, 0, sizeof(f));
	memset(n, 0, sizeof(n));
	for (unsigned t = 0; t < 40; t++) { /* 30 kB and 10 kB a tick */
		cg_f3_weigh(&f, 0, 30000ull * t);
		cg_f3_weigh(&f, 3, 10000ull * t);
	}
	CHECK(f.w[0] > 29000 && f.w[0] <= 30000);
	for (unsigned i = 0; i < 400; i++)
		n[cg_f3_pick(&f, 0x9)]++;
	CHECK(n[0] >= 298 && n[0] <= 302);
	CHECK_EQ(n[0] + n[3], 400);
	for (unsigned i = 0; i < 100; i++)
		CHECK_EQ(cg_f3_pick(&f, 0x8), 3);
	/* a counter that went back (a new socket): weight from what follows */
	cg_f3_weigh(&f, 0, 5);
	CHECK(f.w[0] < 30000);
}

/* The guard: no room, or a queue past the window; none measured. */
static void test_f3_guard(void)
{
	CHECK(!cg_f3_guard(-1, 100000, 0));
	CHECK(!cg_f3_guard(100000, 100000, 0));
	CHECK(cg_f3_guard(100001, 100000, 0));
	CHECK(!cg_f3_guard(0, 0, 0));
	CHECK(cg_f3_guard(0, 300000, 1));
	CHECK(cg_f3_guard(-1, 300000, 1));
}

/* Every other repair over the window's last quarter, never under 20 ms. */
static void test_f3_span(void)
{
	struct cg_f3 f;

	memset(&f, 0, sizeof(f));
	CHECK_EQ(cg_f3_span_us(&f, 200000), 50000);
	CHECK_EQ(cg_f3_span_us(&f, 200000), 200000);
	CHECK_EQ(cg_f3_span_us(&f, 200000), 50000);
	CHECK_EQ(cg_f3_span_us(&f, 40000), 40000);
	CHECK_EQ(cg_f3_span_us(&f, 40000), 20000);
	CHECK_EQ(cg_f3_span_us(&f, 20000), 20000);
	CHECK_EQ(cg_f3_span_us(&f, 20000), 20000);
}

/* The lag: the slowest link of the set over the repair's own, never
 * negative, capped; links whose delay is not known count as the fastest. */
static void test_f3_lag(void)
{
	int32_t b[CG_F3_LINKS] = { 0, 8000, 3000, 30000 };

	CHECK_EQ(cg_f3_lag_us(b, 0xf, 0xf, 0, 50000), 30000);
	CHECK_EQ(cg_f3_lag_us(b, 0xf, 0xf, 3, 50000), 0);
	CHECK_EQ(cg_f3_lag_us(b, 0xf, 0x7, 2, 50000), 5000); /* link 3 not in the set */
	CHECK_EQ(cg_f3_lag_us(b, 0x7, 0xf, 0, 50000), 8000); /* link 3's not known */
	CHECK_EQ(cg_f3_lag_us(b, 0xf, 0xf, 0, 10000), 10000);
	b[0] = -2000;
	CHECK_EQ(cg_f3_lag_us(b, 0xf, 0x3, 0, 50000), 10000);
	CHECK_EQ(cg_f3_lag_us(b, 0, 0xf, 1, 50000), 0);
}

/* The lone link's gate: on with random loss (linksig.h's kind) and for
 * CG_F3_RAND_HOLD_MS after while it still loses; never on congestive loss
 * alone, nor in a cut (over half the repair rate); the knob over all of it. */
static void test_f3_gate(void)
{
	struct cg_f3 f;
	uint64_t t = 1000;

	memset(&f, 0, sizeof(f));
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, t)); /* clean */
	for (int i = 1; i <= 30; i++)
		cg_f3_loss(&f, 0, 10, (uint64_t)i, 80000, 80000, 0); /* 1 %, congestive: a queue of 80 ms */
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, t));
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 10, t)); /* random */
	t += 20000;
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, t)); /* held: reads congestive now */
	cg_f3_loss(&f, 0, 10, 30, 50000, 50000, 0);                             /* the same sample: once */
	t += CG_F3_RAND_HOLD_MS;
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, t)); /* past the hold */
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 10, t));
	for (int i = 31; i <= 60; i++)
		cg_f3_loss(&f, 0, 0, (uint64_t)i, 50000, 50000, 0);
	CHECK_EQ(f.loss16[0] / 16, 0);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, t + 1)); /* clean again within the hold */
	for (int i = 61; i <= 90; i++)
		cg_f3_loss(&f, 0, 600, (uint64_t)i, 50000, 50000, 0); /* a microcut, 60 % */
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 10, t + 2));
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_YES, 0, 10, t + 2));
	memset(&f, 0, sizeof(f));
	for (int i = 1; i <= 30; i++)
		cg_f3_loss(&f, 0, 10, (uint64_t)i, 50000, 50000, 0);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_NO, 1, 10, t));
	/* 1 % at 1 % of repairs: over half the rate, off; at 5 %, on */
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 1, t));
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 5, t));
	cg_f3_loss(&f, 0, 5000, 31, 50000, 50000, 0);
	CHECK(f.loss16[0] / 16 <= 1000);
	/* Starlink's 0.3 % without a queue: random, though under linksig's lossy sample */
	memset(&f, 0, sizeof(f));
	for (int i = 1; i <= 30; i++)
		cg_f3_loss(&f, 0, i % 2 ? 6 : 0, (uint64_t)i, 2000, 2000, 0);
	CHECK(f.rand_ms[0] == 29);
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, 40));
	/* the same with a queue (congestive): never */
	memset(&f, 0, sizeof(f));
	for (int i = 1; i <= 30; i++)
		cg_f3_loss(&f, 0, i % 2 ? 6 : 0, (uint64_t)i, 30000, 30000, 0);
	CHECK(!f.rand_ms[0]);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10, 40));
}

/* The share: the floor on a clean or only congested set, the floor plus
 * CG_F3_PER_LOSS x the scattered loss weighed by the links' bytes, the whole
 * share for CG_F3_HOLD_MS after a burst; fixed when the floor is at the top. */
static void test_f3_pct(void)
{
	struct cg_f3 f;
	uint64_t t;

	memset(&f, 0, sizeof(f));
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 5000), 20); /* nothing known yet: the whole share */
	CHECK_EQ(f.warm_ms, 5000 + CG_F3_WARM_MS);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 5000 + CG_F3_WARM_MS - 1), 20);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 5000 + CG_F3_WARM_MS), 0);
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1;
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 20, 1000), 20); /* fixed */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 30, 1000), 20);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 1000), 0);   /* clean: nothing */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 2, 1000), 2);   /* the floor */
	/* congestive loss (a queue of 80 ms): clean for the share */
	for (t = 1; t <= 30; t++)
		cg_f3_loss(&f, 0, 10, t, 80000, 80000, 0);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 40), 0);
	CHECK_EQ(f.bursts, 0);
	/* 1 % at random on both links, equal bytes: 25 % */
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1; /* warmed up */
	f.w[0] = f.w[1] = 1000;
	for (t = 1; t <= 30; t++) {
		cg_f3_loss(&f, 0, 10, t, 2000, 2000, 0);
		cg_f3_loss(&f, 1, 10, t, 2000, 2000, 0);
	}
	CHECK_EQ(f.bursts, 0);
	CHECK(f.srand_ms[0] == 30 && f.srand_ms[1] == 30);
	f.loss16[0] = f.loss16[1] = 10 * 16;
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 30, 0, 40), 25);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 30, 2, 40), 27);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 5, 0, 40), 5); /* at most the top */
	/* Starlink's 0.4 %: 10 % */
	f.loss16[0] = f.loss16[1] = 4 * 16;
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 40), 10);
	/* only link 0 loses 1.2 %, a third of the bytes: a third of it */
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1; /* warmed up */
	f.w[0] = 1000;
	f.w[1] = 2000;
	f.loss16[0] = 12 * 16;
	f.srand_ms[0] = 30;
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 40), 10); /* 4 per mille x 25 */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 30 + CG_F3_SRAND_FULL_MS), 10);
	/* then it fades: halved every CG_F3_SRAND_HALF_MS, gone at the end */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 30 + CG_F3_SRAND_FULL_MS + 1), 5); /* 2 per mille x 25 */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 30 + CG_F3_SRAND_FULL_MS + CG_F3_SRAND_HALF_MS + 1), 3);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 30 + CG_F3_SRAND_END_MS - 1), 0);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 30 + CG_F3_SRAND_END_MS), 0);
	/* linksig's kind says random now: counted again */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0x1, 20, 0, 30 + CG_F3_SRAND_END_MS + 2), 10);
	/* a burst on link 1 (a stall: 10 % in a sample, no queue): the whole
	 * share for the hold, also after the link is clean again */
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1; /* warmed up */
	cg_f3_loss(&f, 1, 100, 100, 3000, 3000, 0);
	CHECK_EQ(f.bursts, 1);
	for (t = 101; t <= 130; t++)
		cg_f3_loss(&f, 1, 0, t, 3000, 3000, 0);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 1000), 20);
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 0, 1000), 0); /* not a link of the set */
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 100 + CG_F3_HOLD_MS), 20);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 101 + CG_F3_HOLD_MS), 0);
	/* the same loss with its queue full (congestion), or none known: no burst */
	cg_f3_loss(&f, 0, 100, 200, CG_F3_BURST_Q_US + 1, CG_F3_BURST_Q_US + 1, 0);
	cg_f3_loss(&f, 0, 100, 201, -1, -1, 0);
	/* a TCP filling the link: its standing queue lifts the minute's minimum (no
	 * queue over it now) but the sample's top was high: congestion, no burst */
	cg_f3_loss(&f, 0, 100, 202, 2000, 90000, 0);
	CHECK_EQ(f.bursts, 1);
	/* random loss at the link's capacity: the gate's, not the share's */
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1; /* warmed up */
	f.w[0] = 1000;
	for (t = 1; t <= 30; t++)
		cg_f3_loss(&f, 0, 10, t, 2000, 2000, 1);
	CHECK(f.rand_ms[0] == 30 && !f.srand_ms[0]);
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 0, 40), 0);
	f.loss16[0] = 8 * 16;
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0x1, 20, 0, 40), 20); /* linksig says random: it counts */
	/* the lone link's gate lasts as long as the share's evidence, even when
	 * its own (later, by the instant queue) runs out first (single1) */
	memset(&f, 0, sizeof(f));
	cg_f3_loss(&f, 0, 10, 1000, 2000, 2000, 0);   /* the share's and the gate's */
	cg_f3_loss(&f, 0, 10, 1500, 2000, 40000, 1);  /* the gate's only: at its capacity */
	CHECK(f.srand_ms[0] == 1000 && f.rand_ms[0] == 1500);
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 20, 1500 + CG_F3_RAND_HOLD_MS + 1));
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 20, 1000 + CG_F3_SRAND_END_MS));
	/* a TCP that just backed off: the sample is quiet and under its capacity,
	 * the one before full with a queue: neither burst nor random loss */
	memset(&f, 0, sizeof(f));
	f.warm_ms = 1; /* warmed up */
	cg_f3_loss(&f, 0, 0, 1, 40000, 60000, 1);
	cg_f3_loss(&f, 0, 150, 2, 1000, 2000, 0);
	CHECK(!f.srand_ms[0] && !f.bursts);
	cg_f3_loss(&f, 0, 150, 3, 1000, 2000, 0); /* two quiet samples: a burst */
	CHECK(f.srand_ms[0] == 3 && f.bursts == 1);
	/* a stall on a full link without a queue: a burst, not random loss */
	cg_f3_loss(&f, 1, 0, 3, 1000, 2000, 1);
	cg_f3_loss(&f, 1, 100, 4, 1000, 2000, 1);
	CHECK(f.bursts == 2 && !f.srand_ms[1]);
}

/* The bytes loss does not hold back (the protocontroller's): their part of
 * the share asks for no repairs; variant A's middle TCP by its port. */
static void test_f3_mix(void)
{
	struct cg_f3 f;
	uint8_t v4[40] = { 0x45, 0, 0, 40, 0, 0, 0, 0, 64, 6 }, v6[60] = { 0x60 };
	int i;

	memset(&f, 0, sizeof(f));
	f.warm_ms = 1;
	cg_f3_mix_tick(&f); /* nothing sent: unchanged */
	CHECK_EQ(f.ins_q10, 0);
	for (i = 0; i < 40; i++) {
		cg_f3_mix(&f, 1000, 1);
		cg_f3_mix(&f, 1000, 0);
		cg_f3_mix_tick(&f);
	}
	CHECK(f.ins_q10 >= 500 && f.ins_q10 <= 512);
	/* a burst: half the bytes ask for the share over the floor */
	f.burst_ms[0] = 100;
	f.ins_q10 = 512;
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 0, 200), 10);
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 4, 200), 12);
	f.ins_q10 = 1024; /* all of them: the floor */
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 4, 200), 4);
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 20, 200), 20); /* a fixed share stays */
	/* random loss: on a lone link scaled, over several links whole */
	f.burst_ms[0] = 0;
	f.srand_ms[0] = f.srand_ms[1] = 150;
	f.loss16[0] = f.loss16[1] = 8 * 16;
	f.w[0] = f.w[1] = 1000;
	f.ins_q10 = 1024;
	CHECK_EQ(cg_f3_pct(&f, 0x1, 0, 20, 0, 200), 0);
	CHECK_EQ(cg_f3_pct(&f, 0x3, 0, 20, 0, 200), 20);
	/* the port: TCP's destination (or source) port, IPv4 with options, IPv6 */
	v4[22] = 0x1e;
	v4[23] = 0x79; /* destination 7801 */
	CHECK(cg_f3_tcp_port(v4, sizeof(v4), 7801, 1));
	CHECK(!cg_f3_tcp_port(v4, sizeof(v4), 7801, 0));
	CHECK(!cg_f3_tcp_port(v4, sizeof(v4), 0, 1));
	CHECK(!cg_f3_tcp_port(v4, 23, 7801, 1)); /* short */
	v4[9] = 17;                              /* UDP */
	CHECK(!cg_f3_tcp_port(v4, sizeof(v4), 7801, 1));
	v6[6] = 6;
	v6[40] = 0x1e;
	v6[41] = 0x79; /* source 7801 */
	CHECK(cg_f3_tcp_port(v6, sizeof(v6), 7801, 0));
	CHECK(!cg_f3_tcp_port(v6, sizeof(v6), 7801, 1));
	CHECK(!cg_f3_tcp_port(v6, 43, 7801, 0));
}

void test_fec3(void)
{
	test_f3_mode();
	test_f3_credit();
	test_f3_pick();
	test_f3_guard();
	test_f3_lag();
	test_f3_span();
	test_f3_gate();
	test_f3_pct();
	test_f3_mix();
}
