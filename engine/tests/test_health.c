/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "health.h"
#include "test.h"

#define MS 1000u /* microseconds */

static const struct cg_hcfg cfg = {
	.mute_behind_us = 150 * MS, .unmute_behind_us = 120 * MS, .settle_ms = 2000, .min_active = 2
};

/* Reports a raw delay for each link (UINT32_MAX: none) and evaluates. The
 * filter is reset first, so the reported value is the filtered one. */
static uint16_t step(struct cg_hlink *h, int n, const uint32_t *owd, uint16_t live, uint64_t now,
		     const struct cg_hcfg *c)
{
	for (int i = 0; i < n; i++)
		if (owd[i] != UINT32_MAX) {
			h[i].have_owd = 0;
			cg_health_report(&h[i], owd[i], now, 3000);
		}
	return cg_health_eval(h, n, live, now, c);
}

/* Runs from t0 to t1 in 100 ms steps with fixed delays; returns t1. */
static uint64_t run(struct cg_hlink *h, int n, const uint32_t *owd, uint16_t live, uint64_t t0, uint64_t t1,
		    const struct cg_hcfg *c)
{
	for (uint64_t t = t0; t <= t1; t += 100)
		step(h, n, owd, live, t, c);
	return t1;
}

static void init(struct cg_hlink *h, int n)
{
	for (int i = 0; i < n; i++)
		cg_health_reset(&h[i], 1);
}

static void test_filter(void)
{
	struct cg_hlink h;

	cg_health_reset(&h, 0);
	CHECK(!cg_health_fresh(&h, 0));
	cg_health_report(&h, 1000, 10, 3000);
	CHECK_EQ(h.owd, 1000);
	CHECK(cg_health_fresh(&h, 3010));
	CHECK(!cg_health_fresh(&h, 3011));
	cg_health_report(&h, 2000, 20, 3000);
	CHECK_EQ(h.owd, 1250); /* gain 1/4 */
	cg_health_report(&h, 0, 30, 3000);
	CHECK_EQ(h.owd, 1250 - 312);

	/* The raw delay is a clock difference: it wraps, and only differences
	 * between links count. */
	CHECK_EQ(cg_owd_diff(5, 0xfffffffbu), 10);
	CHECK_EQ(cg_owd_diff(0xfffffffbu, 5), -10);
	cg_health_reset(&h, 0);
	cg_health_report(&h, 0xfffffff0u, 0, 3000);
	cg_health_report(&h, 0x10, 0, 3000); /* +32 across the wrap */
	CHECK_EQ(h.owd, 0xfffffff8u);
}

static void test_mute_unmute(void)
{
	struct cg_hlink h[3];
	uint32_t late[3] = { 10 * MS, 50 * MS, 310 * MS }, good[3] = { 10 * MS, 50 * MS, 20 * MS };
	uint64_t t;
	uint16_t ch;

	init(h, 3);
	/* Link 2 is 300 ms behind link 0: muted after settle_ms, not before. */
	run(h, 3, late, 7, 0, 1900, &cfg);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	CHECK(h[2].have_behind && h[2].behind_us == 300 * MS);
	CHECK_EQ(h[1].behind_us, 40 * MS);
	ch = step(h, 3, late, 7, 2000, &cfg);
	CHECK_EQ(ch, 4);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK_EQ(h[2].mutes, 1);
	CHECK_EQ(h[0].state, CG_H_ACTIVE);
	CHECK_EQ(h[1].state, CG_H_ACTIVE);

	/* Back within 120 ms: unmuted after 2 x settle_ms. */
	run(h, 3, good, 7, 2100, 6000, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	ch = step(h, 3, good, 7, 6100, &cfg);
	CHECK_EQ(ch, 4);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);

	/* Late again right away: a failed unmute, the next wait doubles. */
	t = run(h, 3, late, 7, 6200, 8200, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK_EQ(h[2].backoff, 1);
	CHECK_EQ(h[2].mutes, 2);
	run(h, 3, good, 7, t + 100, t + 8000, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	step(h, 3, good, 7, t + 8100, &cfg);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	t += 8100;

	/* Flapping is capped at 2^CG_H_MAX_BACKOFF. */
	for (int k = 0; k < 6; k++) {
		uint64_t wait = (uint64_t)2 * cfg.settle_ms << (k + 2 > CG_H_MAX_BACKOFF ? CG_H_MAX_BACKOFF : k + 2);

		t = run(h, 3, late, 7, t + 100, t + 2100, &cfg);
		CHECK_EQ(h[2].state, CG_H_MUTED);
		t = run(h, 3, good, 7, t + 100, t + wait + 100, &cfg);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
	}
	CHECK_EQ(h[2].backoff, CG_H_MAX_BACKOFF);

	/* An unmute that lasts CG_H_FLAP_MS was a success: no backoff next time. */
	t = run(h, 3, good, 7, t + 100, t + CG_H_FLAP_MS, &cfg);
	t = run(h, 3, late, 7, t + 100, t + 2100, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK_EQ(h[2].backoff, 0);
}

static void test_hysteresis(void)
{
	struct cg_hlink h[3];
	uint32_t late[3] = { 0, 0, 300 * MS }, mid[3] = { 0, 0, 130 * MS }, near[3] = { 0, 0, 125 * MS };

	init(h, 3);
	/* An interruption restarts the settle time. */
	run(h, 3, late, 7, 0, 1500, &cfg);
	step(h, 3, mid, 7, 1600, &cfg); /* below the mute threshold */
	run(h, 3, late, 7, 1700, 3600, &cfg);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	step(h, 3, late, 7, 3700, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);

	/* Between the thresholds a muted link stays muted. */
	run(h, 3, mid, 7, 3800, 20000, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	run(h, 3, near, 7, 20100, 30000, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
}

static void test_min_active(void)
{
	struct cg_hlink h[4];
	uint32_t two[2] = { 0, 500 * MS }, three[4] = { 0, 400 * MS, 200 * MS, UINT32_MAX };
	struct cg_hcfg one = cfg;

	/* Two links and min_active 2: never muted, however late. */
	init(h, 2);
	run(h, 2, two, 3, 0, 10000, &cfg);
	CHECK_EQ(h[1].state, CG_H_ACTIVE);
	CHECK(h[1].holding);

	/* Two of three late: only one may go, the furthest behind. */
	init(h, 3);
	run(h, 3, three, 7, 0, 10000, &cfg);
	CHECK_EQ(h[1].state, CG_H_MUTED);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);

	/* With min_active 1 both go, the fastest never does. */
	one.min_active = 1;
	init(h, 3);
	run(h, 3, three, 7, 0, 10000, &one);
	CHECK_EQ(h[0].state, CG_H_ACTIVE);
	CHECK_EQ(h[1].state, CG_H_MUTED);
	CHECK_EQ(h[2].state, CG_H_MUTED);

	/* Equal links: nobody is behind. Delay muting off: nobody is muted. */
	{
		uint32_t eq[3] = { 7, 7, 7 }, far[3] = { 0, 0, 900 * MS };
		struct cg_hcfg off = cfg;

		init(h, 3);
		run(h, 3, eq, 7, 0, 10000, &one);
		CHECK(h[0].state == CG_H_ACTIVE && h[1].state == CG_H_ACTIVE && h[2].state == CG_H_ACTIVE);
		off.mute_behind_us = 0;
		init(h, 3);
		run(h, 3, far, 7, 0, 10000, &off);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
	}
}

static void test_promotion(void)
{
	struct cg_hlink h[4];
	uint32_t owd[4] = { 0, 10 * MS, 400 * MS, 300 * MS };
	uint16_t ch;

	init(h, 4);
	run(h, 4, owd, 15, 0, 10000, &cfg);
	CHECK(h[2].state == CG_H_MUTED && h[3].state == CG_H_MUTED);

	/* Link 0 stalls: the fastest muted link (3) carries at once. */
	ch = step(h, 4, owd, 14, 10100, &cfg);
	CHECK_EQ(ch, 8);
	CHECK_EQ(h[3].state, CG_H_ACTIVE);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK_EQ(h[3].unmuted_ms, 0);

	/* Link 0 is back: link 3 is behind again and goes, without backoff
	 * (its promotion was not a try). */
	run(h, 4, owd, 15, 10200, 12300, &cfg);
	CHECK_EQ(h[3].state, CG_H_MUTED);
	CHECK_EQ(h[3].backoff, 0);

	/* Nothing fresh to rank by: promotion falls back to the lowest index. */
	init(h, 3);
	h[1].state = h[2].state = CG_H_MUTED;
	ch = cg_health_eval(h, 3, 6, 100, &cfg);
	CHECK_EQ(ch, 6);
	CHECK(h[1].state == CG_H_ACTIVE && h[2].state == CG_H_ACTIVE);
}

static void test_off_on_reload(void)
{
	struct cg_hlink h[4];
	uint32_t owd[4] = { 0, 10 * MS, 300 * MS, 400 * MS }, good[4] = { 0, 10 * MS, 20 * MS, 400 * MS };
	struct cg_hcfg off = cfg;
	uint64_t t;
	uint16_t ch;

	/* Links 2 and 3 muted, link 2 after a failed unmute (backoff 1). */
	init(h, 4);
	t = run(h, 4, owd, 15, 0, 3000, &cfg);
	t = run(h, 4, good, 15, t + 100, t + 4100, &cfg);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	t = run(h, 4, owd, 15, t + 100, t + 2100, &cfg);
	CHECK(h[2].state == CG_H_MUTED && h[3].state == CG_H_MUTED);
	CHECK_EQ(h[2].backoff, 1);

	/* A reload turns muting off (the unmute threshold follows it to 0):
	 * both carry again at the next evaluation, link 3 too although it is
	 * not live right now, and none is muted again however late. */
	off.mute_behind_us = off.unmute_behind_us = 0;
	ch = step(h, 4, owd, 7, t + 100, &off);
	CHECK_EQ(ch, 12);
	CHECK(h[2].state == CG_H_ACTIVE && h[3].state == CG_H_ACTIVE);
	CHECK(!h[2].backoff && !h[2].unmuted_ms && !h[3].unmuted_ms);
	CHECK_EQ(cg_health_carriers(h, 4, 15, 15), 15);
	t = run(h, 4, owd, 15, t + 200, t + 60000, &off);
	CHECK(h[2].state == CG_H_ACTIVE && h[3].state == CG_H_ACTIVE);
	CHECK_EQ(h[2].mutes, 2);

	/* Back on: muted again after settle_ms, with no backoff (the way back
	 * was forced, not a try). */
	run(h, 4, owd, 15, t + 100, t + 2100, &cfg);
	CHECK(h[2].state == CG_H_MUTED && h[3].state == CG_H_MUTED);
	CHECK_EQ(h[2].backoff, 0);
}

static void test_stale(void)
{
	struct cg_hlink h[3];
	uint32_t late[3] = { 0, 0, 300 * MS }, none[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };

	/* Reports valid for 500 ms, then nothing: no decision on old data. */
	init(h, 3);
	for (int i = 0; i < 3; i++)
		cg_health_report(&h[i], late[i], 0, 500);
	cg_health_eval(h, 3, 7, 0, &cfg);
	CHECK(h[2].holding);
	run(h, 3, none, 7, 100, 500, &cfg);
	CHECK(h[2].holding);
	run(h, 3, none, 7, 600, 5000, &cfg);
	CHECK(!h[2].holding);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	CHECK(!h[2].have_behind);

	/* A link that is not live has no say and does not count as a carrier. */
	init(h, 3);
	run(h, 3, late, 3, 0, 10000, &cfg);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);
	CHECK(!h[2].holding);

	/* A late link that keeps falling silent (its queue fills, the replies
	 * stop, it drains) is still muted: a stall freezes it, it does not
	 * restart its settle time. */
	init(h, 3);
	for (uint64_t t = 0; t <= 2400; t += 100)
		step(h, 3, late, (t / 100) % 3 ? 3 : 7, t, &cfg);
	CHECK_EQ(h[2].state, CG_H_MUTED);
}

static void test_carriers(void)
{
	struct cg_hlink h[4];

	init(h, 4);
	h[1].state = CG_H_MUTED;
	CHECK_EQ(cg_health_carriers(h, 4, 15, 15), 13);
	CHECK_EQ(cg_health_carriers(h, 4, 7, 15), 5);   /* only present links */
	CHECK_EQ(cg_health_carriers(h, 4, 15, 2), 2);   /* only a muted link is live */
	CHECK_EQ(cg_health_carriers(h, 4, 15, 0), 15);  /* nothing live: everything */
	CHECK_EQ(cg_health_carriers(h, 4, 0, 0), 0);

	/* Trickle: one packet in every 3. */
	{
		int got[7];

		for (int i = 0; i < 7; i++)
			got[i] = cg_trickle(&h[1], 3);
		CHECK(!got[0] && !got[1] && got[2] && !got[3] && !got[4] && got[5] && !got[6]);
		CHECK(!cg_trickle(&h[1], 0));
	}
}

static void test_stall(void)
{
	/* Client: three unanswered probes, the oldest out for 2 RTT + slack. */
	CHECK(!cg_probes_stalled(2, 0, 10000, 40));
	CHECK(!cg_probes_stalled(3, 1000, 2080, 40));
	CHECK(cg_probes_stalled(3, 1000, 2081, 40));
	/* stalled probes, but packets still down (story 025): alive for what
	 * goes up within two round trips and slack, for a stream's DATA within
	 * a second; neither once the stall is CG_RXALIVE_MAX_MS old */
	CHECK(cg_link_rx_alive(180, 1200, 40));
	CHECK(!cg_link_rx_alive(181, 1200, 40));
	CHECK(!cg_link_rx_alive(10, CG_RXALIVE_MAX_MS, 40));
	CHECK(cg_link_rx_alive_arq(CG_RXALIVE_ARQ_MS, 1200));
	CHECK(!cg_link_rx_alive_arq(CG_RXALIVE_ARQ_MS + 1, 1200));
	CHECK(!cg_link_rx_alive_arq(10, CG_RXALIVE_MAX_MS));
	CHECK(!cg_probes_stalled(9, 1000, 3000, 500)); /* a slow link is not a dead one */
	CHECK(cg_probes_stalled(9, 1000, 3001, 500));

	/* Server: three announced intervals plus slack. */
	CHECK(cg_path_stalled(0, 5, 100));
	CHECK(!cg_path_stalled(1000, 2300, 100));
	CHECK(cg_path_stalled(1000, 2301, 100));
	CHECK(!cg_path_stalled(1000, 5000, 1000));
	CHECK(cg_path_stalled(1000, 5001, 1000));
}

/* mute_queue (story 016): the lateness the guard compares. */
static void test_late(void)
{
	int have;

	/* nothing known */
	CHECK_EQ(cg_health_late(0, 0, -1, -1, 0, &have), 0);
	CHECK(!have);
	/* behind alone, or the largest of what is known */
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, -1, 0, &have), 40 * MS);
	CHECK(have);
	CHECK_EQ(cg_health_late(1, -5 * MS, -1, -1, 0, &have), -5 * (int32_t)MS); /* ahead */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, 20 * MS, 0, &have), 280 * MS);
	CHECK_EQ(cg_health_late(1, 400 * MS, 300 * MS, 20 * MS, 0, &have), 400 * MS);
	/* a queue the fastest has as well makes no copy later */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, 300 * MS, 0, &have), 40 * MS);
	/* one queue unknown: not counted */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, -1, 0, &have), 40 * MS);
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, 0, 0, &have), 40 * MS);
	/* the unanswered probes' bound, also without a fresh report */
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, -1, 700 * MS, &have), 700 * MS);
	CHECK_EQ(cg_health_late(0, 0, -1, -1, 700 * MS, &have), 700 * MS);
	CHECK(have);

	/* the bound: the oldest unanswered probe's age over the fastest RTT */
	CHECK_EQ(cg_health_silent_us(0, 1000, 5000, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1020, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1030, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1530, 30), 500 * MS);
	CHECK_EQ(cg_health_silent_us(3, 2000, 1000, 30), 0); /* not before it was sent */
}

/* mute_queue in an evaluation: a link 100 ms behind the fastest (under
 * mute_behind_us) whose own queue is 300 ms over the fastest's is muted;
 * without the mode it is not. Back under unmute_behind_us of lateness (the
 * queue gone), it comes back after 2 x settle. */
static void test_queue_mode(void)
{
	struct cg_hlink h[3];
	struct cg_hcfg q = cfg;
	uint32_t owd[3] = { 50 * MS, 60 * MS, 150 * MS };

	q.min_active = 1;
	q.queue_mode = 1;
	init(h, 3);
	h[0].queue_us = 0;
	h[1].queue_us = 5 * MS;
	h[2].queue_us = 300 * MS;
	run(h, 3, owd, 7, 0, 3000, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK(h[2].behind_us == 100 * MS);
	CHECK(h[2].have_late && h[2].late_us == 300 * MS);
	CHECK_EQ(h[1].state, CG_H_ACTIVE);

	/* the same without the mode: 100 ms behind is not enough */
	{
		struct cg_hcfg o = q;

		o.queue_mode = 0;
		init(h, 3);
		h[0].queue_us = 0;
		h[2].queue_us = 300 * MS;
		run(h, 3, owd, 7, 0, 10000, &o);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
		CHECK(!h[2].have_late);
	}

	/* muted, its queue drains: lateness 100 ms behind, under unmute 120 */
	init(h, 3);
	h[0].queue_us = 0;
	h[2].queue_us = 300 * MS;
	run(h, 3, owd, 7, 0, 3000, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	h[2].queue_us = 0;
	run(h, 3, owd, 7, 3100, 6900, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED); /* 2 x settle not yet */
	run(h, 3, owd, 7, 7000, 7200, &q);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);

	/* muting off: the mode mutes nothing */
	{
		struct cg_hcfg o = q;

		o.mute_behind_us = o.unmute_behind_us = 0;
		init(h, 3);
		h[0].queue_us = 0;
		h[2].queue_us = 3000 * MS;
		h[2].silent_us = 3000 * MS;
		run(h, 3, owd, 7, 0, 10000, &o);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
	}
}

/* mute_queue: a late link whose reports come in bursts (its replies sit in
 * a queue of seconds) keeps holding its mute through the gaps, as long as
 * its unanswered probes say it is late; without the mode each gap starts it
 * over and it is never muted (story 016, the virtual cell). */
static void test_queue_mode_gaps(void)
{
	struct cg_hlink h[2];
	struct cg_hcfg q = cfg;
	uint32_t owd[2] = { 30 * MS, 1030 * MS }, gap[2] = { 30 * MS, UINT32_MAX };

	q.min_active = 1;
	for (int mode = 0; mode < 2; mode++) {
		q.queue_mode = (uint8_t)mode;
		init(h, 2);
		/* a report from link 1 valid 500 ms every 1.5 s, its oldest
		 * probe unanswered for up to a second meanwhile */
		for (uint64_t t = 0; t <= 6000; t += 100) {
			int rep = t % 1500 == 0;

			h[1].silent_us = rep ? 0 : (uint32_t)(t % 1500) * MS + 200 * MS;
			for (int i = 0; i < 2; i++) {
				uint32_t v = rep ? owd[i] : gap[i];

				if (v != UINT32_MAX) {
					h[i].have_owd = 0;
					cg_health_report(&h[i], v, t, 500);
				}
			}
			cg_health_eval(h, 2, 3, t, &q);
		}
		if (mode)
			CHECK_EQ(h[1].state, CG_H_MUTED);
		else
			CHECK_EQ(h[1].state, CG_H_ACTIVE);
	}

	/* a short silence (a radio hiccup) is under the threshold: no hold */
	q.queue_mode = 1;
	init(h, 2);
	{
		uint32_t ok[2] = { 30 * MS, 40 * MS };

		run(h, 2, ok, 3, 0, 1000, &q);
		h[1].silent_us = 100 * MS;
		for (uint64_t t = 1100; t <= 4000; t += 100)
			step(h, 2, gap, 3, t, &q);
		CHECK_EQ(h[1].state, CG_H_ACTIVE);
		CHECK(!h[1].holding);
	}
}

void test_health(void)
{
	test_late();
	test_queue_mode();
	test_queue_mode_gaps();
	test_filter();
	test_mute_unmute();
	test_hysteresis();
	test_min_active();
	test_promotion();
	test_off_on_reload();
	test_stale();
	test_carriers();
	test_stall();
}
