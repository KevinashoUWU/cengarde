/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "capacity.h"
#include "test.h"

#define MS 1000u /* microseconds per millisecond */
#define MBIT 1000000ull

static const struct cg_cap_cfg CFG = { .window_ms = 10000, .sat_us = 20 * MS };

/* A link carrying 1000-byte packets: the sender's counters, what the other
 * end received, and its delay; one report every 100 ms. */
struct sim {
	struct cg_cap_link l;
	struct cg_hlink h;
	uint64_t t, tx_pkts, tx_bytes;
	uint32_t rx;
	double backlog; /* packets sent and not yet delivered */
	int stale;      /* its delay reports stopped: no queue measured */
	uint32_t jitter_us; /* added to the delay, as a cellular link's swings */
};

static void sim_start(struct sim *s)
{
	memset(s, 0, sizeof(*s));
	cg_cap_reset(&s->l);
	s->t = 1000;
	s->rx = 0xfffff000u; /* wraps within the first seconds */
}

/* 100 ms: send at send_mbit, deliver up to cap_mbit (the rest queues), the
 * delay = base + queue, and a report. */
static void sim_step(struct sim *s, double send_mbit, double cap_mbit)
{
	double sent = send_mbit * MBIT / 8 / 1000 / 10, can = cap_mbit * MBIT / 8 / 1000 / 10, out;

	s->t += 100;
	s->tx_pkts += (uint64_t)sent;
	s->tx_bytes += (uint64_t)sent * 1000;
	s->backlog += (double)(uint64_t)sent;
	out = s->backlog < can ? s->backlog : can;
	s->backlog -= (double)(uint32_t)out;
	s->rx += (uint32_t)out;
	s->h.owd = 30000u + s->jitter_us + (uint32_t)(s->backlog / can * 100 * MS); /* queue: backlog over the rate */
	s->h.have_owd = 1;
	s->h.fresh_until_ms = s->t + 3000;
	cg_cap_tick(&s->l, s->t, s->t, s->rx, s->tx_pkts, s->tx_bytes, s->stale ? NULL : &s->h, &CFG);
}

static int near(uint64_t bps, double mbit, double tol)
{
	double v = (double)bps / MBIT;

	return v > mbit * (1 - tol) && v < mbit * (1 + tol);
}

static void run(struct sim *s, int steps, double send_mbit, double cap_mbit)
{
	for (int i = 0; i < steps; i++)
		sim_step(s, send_mbit, cap_mbit);
}

/* A link sent below its capacity delivers what it is sent; above it, its
 * capacity; the capacity holds while the load is low: only a queue says a
 * link carries less. */
static void test_capacity_rates(void)
{
	static struct sim s;

	sim_start(&s);
	run(&s, 40, 30, 50);
	CHECK(s.l.have_rate && near(s.l.rate_bps, 30, 0.02) && near(cg_cap_bps(&s.l), 30, 0.02));
	CHECK(s.l.size == 1000);

	/* 10 for 6 s: it delivers 10, and still carries 30 */
	run(&s, 60, 10, 50);
	CHECK(near(s.l.rate_bps, 10, 0.1) && near(cg_cap_bps(&s.l), 30, 0.05));
	/* a minute of 10: still 30, it was never full */
	run(&s, 600, 10, 50);
	CHECK(near(s.l.rate_bps, 10, 0.05) && near(cg_cap_bps(&s.l), 30, 0.05));
}

/* Above the link's rate the queue grows: past sat_us the capacity is what
 * it delivers, even below an older and larger maximum. */
static void test_capacity_saturated(void)
{
	static struct sim s;

	sim_start(&s);
	run(&s, 40, 40, 50); /* 40 delivered, no queue */
	CHECK(near(cg_cap_bps(&s.l), 40, 0.02));
	run(&s, 80, 40, 25); /* the link drops to 25: a queue builds */
	CHECK(s.l.queue_us > (int32_t)(20 * MS));
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* one bad half second while the queue lasts does not sink it */
	run(&s, 5, 40, 2);
	run(&s, 5, 40, 25);
	CHECK(cg_cap_bps(&s.l) > 15 * MBIT);
	run(&s, 30, 40, 25);
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* the queue empties: more delivered than sent in an interval, and no
	 * new start for that; then the link back at 50: the capacity follows */
	run(&s, 6, 0, 50);
	CHECK(s.l.have_rate && s.l.rate_bps > 20 * MBIT);
	run(&s, 34, 0, 50);
	run(&s, 80, 45, 50);
	CHECK(near(s.l.rate_bps, 45, 0.05) && cg_cap_bps(&s.l) >= 44 * MBIT);
}

/* Senders speeding up into a link of 50 (TCP's slow start, many flows at
 * once): its queue passes sat_us while the smoothed delivery rate still
 * carries the samples from before. Full, it is measured by what it
 * delivers while full: 50, not the 16-27 of the way up (story 013: with 16
 * flows down, a link of 72 fell to 27). */
static void test_capacity_ramp(void)
{
	static struct sim s;

	sim_start(&s);
	run(&s, 40, 5, 50); /* sent 5: it delivers 5 */
	CHECK(near(cg_cap_bps(&s.l), 5, 0.05));
	run(&s, 10, 80, 50); /* two samples full */
	CHECK(s.l.queue_us > (int32_t)(20 * MS));
	CHECK(s.l.sat_n >= CG_CAP_SAT_N);
	CHECK(near(cg_cap_bps(&s.l), 50, 0.1));
	run(&s, 30, 80, 50);
	CHECK(near(cg_cap_bps(&s.l), 50, 0.05));
}

/* One sample whose delay swings over sat_us is not a full link: the
 * capacity holds (a lightly loaded link took what it was sent, 10). Two in
 * a row are. */
static void test_capacity_swing(void)
{
	static struct sim s;

	sim_start(&s);
	run(&s, 40, 40, 50);
	run(&s, 60, 10, 50);
	CHECK(near(cg_cap_bps(&s.l), 40, 0.05));
	s.jitter_us = 30 * MS;
	run(&s, 5, 10, 50);
	CHECK_EQ(s.l.sat_n, 1);
	CHECK(near(cg_cap_bps(&s.l), 40, 0.05));
	s.jitter_us = 0;
	run(&s, 5, 10, 50);
	CHECK_EQ(s.l.sat_n, 0);
	CHECK(near(cg_cap_bps(&s.l), 40, 0.05));
}

/* The rescue's weight (cg_cap_rescue_w): all of it while the queue is low
 * and still; less as the queue grows, before it is over the target (its
 * slope and its slope's slope look a second ahead); none once full. */
static void test_capacity_rescue_w(void)
{
	static struct sim s;
	int fell_early = 0;

	sim_start(&s);
	CHECK_EQ(cg_cap_rescue_w(&s.l, 20 * MS), CG_CAP_W_ONE); /* nothing measured */
	run(&s, 50, 30, 50);
	CHECK_EQ(cg_cap_rescue_w(&s.l, 20 * MS), CG_CAP_W_ONE);
	/* sent 55 into 50: the queue grows 10 ms each second */
	for (int i = 0; i < 40; i++) {
		sim_step(&s, 55, 50);
		if (s.l.ql_us < (int32_t)(20 * MS) && cg_cap_rescue_w(&s.l, 20 * MS) < CG_CAP_W_ONE)
			fell_early = 1;
	}
	CHECK(fell_early); /* it gave less before the queue passed the target */
	CHECK(s.l.s1 > 0);
	run(&s, 100, 70, 50); /* far over: full */
	CHECK_EQ(cg_cap_rescue_w(&s.l, 20 * MS), 0);
	/* the load stops: the queue drains, and the weight comes back */
	run(&s, 100, 0, 50);
	run(&s, 50, 10, 50);
	CHECK_EQ(cg_cap_rescue_w(&s.l, 20 * MS), CG_CAP_W_ONE);
}

/* A link that stops delivering what it is sent, while its delay says
 * nothing (its replies stopped coming in time): after two samples of less
 * than half, its capacity is what it delivers. A link ramping up, its
 * delivery a sample behind, is not taken for one. */
static void test_capacity_short(void)
{
	static struct sim s;

	sim_start(&s);
	run(&s, 40, 30, 50); /* 30 delivered */
	CHECK(near(cg_cap_bps(&s.l), 30, 0.02));
	/* the link drops to 3, and its delay goes stale: no queue measured */
	s.l.queue_us = 2000; /* the last queue it measured, from before */
	s.stale = 1;
	run(&s, 40, 30, 3);
	CHECK(s.l.queue_us == 2000);
	CHECK(s.l.short_n >= CG_CAP_SHORT_N);
	CHECK(cg_cap_bps(&s.l) < 10 * MBIT);
	/* a fresh start: 0 to 40 at once on a link of 50 */
	sim_start(&s);
	run(&s, 40, 10, 50);
	for (int i = 0; i < 40; i++) {
		sim_step(&s, 40, 50);
		CHECK_EQ(s.l.short_n, 0);
	}
	CHECK(cg_cap_bps(&s.l) > 30 * MBIT);
}

/* Samples span CG_CAP_MIN_MS; counters from another run start again. */
static void test_capacity_edges(void)
{
	struct cg_cap_link l;
	struct cg_hlink h;
	uint64_t t = 5000;

	memset(&h, 0, sizeof(h));
	cg_cap_reset(&l);
	CHECK(cg_cap_bps(&l) == 0 && l.queue_us == -1);
	cg_cap_tick(&l, t, 0, 0, 0, 0, &h, &CFG); /* no report yet */
	CHECK(!l.have);
	cg_cap_tick(&l, t, t, 100, 100, 100000, &h, &CFG);
	CHECK(l.have && !l.have_rate);
	cg_cap_tick(&l, t + 300, t + 300, 300, 300, 300000, &h, &CFG); /* 300 ms: too short */
	CHECK(!l.have_rate);
	cg_cap_tick(&l, t + 500, t + 500, 600, 600, 600000, &h, &CFG); /* 500 packets of 1000 B in 500 ms */
	CHECK(l.have_rate && l.rate_bps == 8 * MBIT);
	/* the same report again: nothing */
	cg_cap_tick(&l, t + 900, t + 500, 600, 900, 900000, &h, &CFG);
	CHECK(l.rate_bps == 8 * MBIT && l.t_ms == t + 500);
	/* twice the rate: a quarter of the way there */
	cg_cap_tick(&l, t + 1000, t + 1000, 1600, 1600, 1600000, &h, &CFG);
	CHECK(l.rate_bps == 10 * MBIT && cg_cap_bps(&l) == 10 * MBIT);
	/* the other end's counters went back (a new session): start again */
	cg_cap_tick(&l, t + 1700, t + 1700, 5, 2000, 2000000, &h, &CFG);
	CHECK(l.have && !l.have_rate && l.rx == 5 && cg_cap_bps(&l) == 0);
	/* more delivered than sent since that start: the same */
	cg_cap_tick(&l, t + 2300, t + 2300, 5000, 2100, 2100000, &h, &CFG);
	CHECK(!l.have_rate && l.rx == 5000);
	/* nothing sent nor delivered: 0 */
	cg_cap_tick(&l, t + 2900, t + 2900, 5000, 2100, 2100000, &h, &CFG);
	CHECK(l.have_rate && l.rate_bps == 0);
}

/* A link measured at rate r Mbit/s with queue q ms. */
static void measured(struct cg_cap_link *l, double r, int q)
{
	cg_cap_reset(l);
	l->have = l->have_rate = 1;
	l->rate_bps = l->max_cur = l->max_prev = (uint64_t)(r * MBIT);
	l->queue_us = q < 0 ? -1 : q * (int32_t)MS;
}

/* A queue at a light load: a hiccup of the radio (1 s with a 20th of its
 * capacity) leaves the capacity as it was; the same lasting 4 s lowers it
 * to what the link delivers. */
static void test_capacity_hiccup(void)
{
	struct sim s;

	sim_start(&s);
	run(&s, 20, 30, 40); /* measured at 30 */
	CHECK(near(cg_cap_bps(&s.l), 30, 0.05));
	run(&s, 20, 3, 40); /* an 8 Mbit/s stream's share: 3 */
	run(&s, 10, 3, 1.5); /* the radio stalls 1 s */
	CHECK(s.l.queue_us > (int32_t)(20 * MS));
	CHECK(near(cg_cap_bps(&s.l), 30, 0.05));
	CHECK(s.l.full_ms == 0);
	run(&s, 10, 3, 40);
	CHECK(near(cg_cap_bps(&s.l), 30, 0.05));
	run(&s, 40, 3, 1.5); /* and now for good */
	CHECK(cg_cap_bps(&s.l) < 3 * MBIT);
	CHECK(s.l.full_ms != 0);
}

/* Fast start: a link measured at 50, then found full at 10, comes back to
 * half its best (25) after CG_CAP_REST_MS without being full, never before;
 * the best is forgotten after CG_CAP_BEST_MS. */
static void test_capacity_fast_start(void)
{
	struct sim s;

	sim_start(&s);
	run(&s, 20, 50, 60); /* 2 s at 50 */
	CHECK(near(cg_cap_bps(&s.l), 50, 0.05) && near(cg_cap_best_bps(&s.l), 50, 0.05));
	run(&s, 30, 12, 10); /* full at 10, for longer than CG_CAP_EP_MS: a */
	run(&s, 300, 10.2, 10); /* slower link, not a contention (no fast return) */
	CHECK(near(cg_cap_bps(&s.l), 10, 0.1));
	CHECK(s.l.back_ms == 0);
	run(&s, 50, 0.5, 10); /* idle 5 s: not yet */
	CHECK(cg_cap_bps(&s.l) < 12 * MBIT);
	run(&s, 80, 0.5, 10); /* past 10 s: half its best */
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* full again at 10 for long: it measures 10 again, and holds it 10 s */
	run(&s, 30, 12, 10);
	run(&s, 300, 10.2, 10);
	CHECK(near(cg_cap_bps(&s.l), 10, 0.1));
	run(&s, 50, 0, 10);
	CHECK(cg_cap_bps(&s.l) < 12 * MBIT);
	/* five minutes later, at most 10 for longer than CG_CAP_BEST_MS: the
	 * 50 is forgotten */
	run(&s, 3200, 0.5, 10);
	CHECK(cg_cap_best_bps(&s.l) < 30 * MBIT);
}

/* The probe: 5/4 and 1/16 of the largest; twice while under half its best. */
static void test_capacity_probe(void)
{
	struct cg_cap_link l;

	measured(&l, 20, 2);
	CHECK(cg_cap_probe(&l, 20 * MBIT, 160 * MBIT) == 35 * MBIT);
	l.best_cur = 50 * MBIT; /* 20 under 25 */
	CHECK(cg_cap_probe(&l, 20 * MBIT, 160 * MBIT) == 50 * MBIT);
	l.best_cur = 0;
	l.best_prev = 40 * MBIT; /* 20 not under 20 */
	CHECK(cg_cap_probe(&l, 20 * MBIT, 160 * MBIT) == 35 * MBIT);
}

/* The weights: shares as the capacities, 5/4 below the probe queue, the
 * unmeasured at the top, the floor, dup0 and the governor's scale. */
static void test_capacity_weights(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	uint16_t w[CG_MAX_LINKS], gov[CG_MAX_LINKS];

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	cg_cap_weights(l, 0, NULL, 10 * MS, 0, w); /* nothing measured: all at the top */
	CHECK(w[0] == CG_GOV_MAX && w[15] == CG_GOV_MAX);

	measured(&l[0], 200, 50); /* full: no probe */
	measured(&l[1], 20, 50);
	measured(&l[2], 100, 2); /* room: 125 */
	measured(&l[3], 1, 50);  /* below the floor */
	cg_cap_weights(l, 0, NULL, 10 * MS, 0, w);
	/* 100 with room: 125 and 1/16 of 200, 137.5 */
	CHECK(w[0] == CG_GOV_MAX && w[1] == 100 && w[2] == 687 && w[3] == CG_GOV_MIN);
	CHECK(w[4] == CG_GOV_MAX); /* not measured */
	measured(&l[6], 0, 900); /* measured, delivering nothing: the floor */
	cg_cap_weights(l, 0, NULL, 10 * MS, 0, w);
	CHECK(w[6] == CG_GOV_MIN && w[4] == CG_GOV_MAX);
	/* a probing link above the others sets the scale */
	measured(&l[2], 200, 2);
	cg_cap_weights(l, 0, NULL, 10 * MS, 0, w);
	CHECK(w[2] == CG_GOV_MAX && w[0] == 761 && w[1] == 76);
	/* dup0 keeps 0 and does not set the scale */
	measured(&l[5], 1000, 50);
	cg_cap_weights(l, 1u << 5, NULL, 10 * MS, 0, w);
	CHECK(w[5] == 0 && w[2] == CG_GOV_MAX);
	/* the governor scales each share */
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		gov[i] = CG_GOV_MAX;
	gov[0] = 500;
	gov[1] = 10;
	cg_cap_weights(l, 1u << 5, gov, 10 * MS, 0, w);
	CHECK(w[0] == 380 && w[1] == CG_GOV_MIN && w[2] == CG_GOV_MAX);
	/* a link left at 0.1 Mbit/s by a queue that has gone: 1/16 of the
	 * largest keeps it in the split, so that it can show more */
	measured(&l[1], 0.1, 2);
	cg_cap_weights(l, 1u << 5, NULL, 10 * MS, 0, w);
	CHECK(w[1] > 40 && w[1] < 60);
}

/* Two links of 30 and 8 Mbit/s take 30 between them, split by these
 * weights, each behind a FIFO queue; reports every 100 ms. Equal weights
 * give the slow one 15 and its queue runs away; from the capacities it
 * ends with about its share and both queues stay short. */
static void test_capacity_converges(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_hlink h[CG_MAX_LINKS];
	uint16_t w[CG_MAX_LINKS];
	const double cap[2] = { 30.0, 8.0 }, load = 30.0;
	double q[2] = { 0, 0 }, maxq_late = 0, share1 = 0; /* queued Mbit; ms */
	uint64_t tx_p[2] = { 0, 0 }, tx_b[2] = { 0, 0 }, t = 1000;
	uint32_t rx[2] = { 0, 0 };

	memset(h, 0, sizeof(h));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	for (int k = 0; k < 600; k++) { /* 60 s */
		double tot;

		cg_cap_weights(l, 0xfffc, NULL, CFG.sat_us / 2, 0, w); /* links 2-15 out (dup0) */
		tot = (double)w[0] + w[1];
		t += 100;
		for (int i = 0; i < 2; i++) {
			double in = load * w[i] / tot * 0.1, out; /* Mbit this tick */
			uint64_t pk = (uint64_t)(in * MBIT / 8 / 1000);

			/* whole packets, so that no more arrive than were sent */
			q[i] += (double)pk * 8000 / MBIT;
			out = q[i] < cap[i] * 0.1 ? q[i] : cap[i] * 0.1;
			out = (double)(uint64_t)(out * MBIT / 8000) * 8000 / MBIT;
			q[i] -= out;
			tx_p[i] += pk;
			tx_b[i] += pk * 1000;
			rx[i] += (uint32_t)(out * MBIT / 8000 + 0.5);
			h[i].owd = 20000u + (uint32_t)(q[i] / cap[i] * 1000 * MS);
			h[i].have_owd = 1;
			h[i].fresh_until_ms = t + 3000;
			cg_cap_tick(&l[i], t, t, rx[i], tx_p[i], tx_b[i], &h[i], &CFG);
			if (k >= 300 && q[i] / cap[i] * 1000 > maxq_late)
				maxq_late = q[i] / cap[i] * 1000;
		}
		if (k >= 300)
			share1 += w[1] / tot / 300.0;
	}
	/* 8 of 38: 0.21 of the load, give or take the probing */
	CHECK(share1 > 0.12 && share1 < 0.30);
	CHECK(maxq_late < 100.0);
}

/* The shaper's rate: none until a queue under load, then 9/10 of what is
 * delivered, up 1/32 a tick while full and without a queue, held while
 * idle. */
static void test_capacity_total(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_cap_total t;

	memset(&t, 0, sizeof(t));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	cg_cap_total_tick(&t, l, 0xffff, 20 * MS);
	CHECK(cg_cap_shape_bps(&t) == 0);
	measured(&l[0], 60, 5);
	measured(&l[1], 40, 5);
	measured(&l[2], 500, 500); /* not in the mask */
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(cg_cap_shape_bps(&t) == 0); /* no queue: no shaping */
	/* queues on an idle tunnel (delays swing): nothing */
	measured(&l[0], 0.5, 50);
	measured(&l[1], 1, 50);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(cg_cap_shape_bps(&t) == 0);
	/* a queue on the link that carries less: for the weights, not here */
	measured(&l[0], 60, 5);
	measured(&l[1], 40, 50);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(cg_cap_shape_bps(&t) == 0);
	measured(&l[0], 60, 50);
	/* queues on both: 9/10 of 100 */
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(t.bps == 90 * MBIT);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS); /* still: not higher than before */
	CHECK(t.bps == 90 * MBIT);
	/* behind the shaper: 90 delivered, no queue: up 1/32 a tick */
	measured(&l[0], 54, 5);
	measured(&l[1], 36, 5);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(t.bps == 90 * MBIT + 90 * MBIT / 32);
	for (int i = 0; i < 2; i++)
		cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(t.bps > 98 * MBIT && t.bps < 100 * MBIT);
	/* idle: held */
	measured(&l[0], 5, 0);
	measured(&l[1], 5, 0);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(t.bps > 98 * MBIT && t.bps < 100 * MBIT);
	/* a queue at less: down to 9/10 of it */
	measured(&l[0], 30, 50);
	measured(&l[1], 20, 50);
	cg_cap_total_tick(&t, l, 0x3, 20 * MS);
	CHECK(t.bps == 45 * MBIT);
}

/* A tunnel of 30 Mbit/s that becomes 50 after 30 s, which a flow would
 * fill to 80, behind the shaper: the rate follows it up (the probing) and
 * settles just under 50, and the queue stays short, where without it the
 * links' queues would grow without end. */
static void test_capacity_shapes(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_cap_total tot;
	double q = 0, maxq_late = 0, rate_late = 0;
	uint64_t tx_p = 0, tx_b = 0, t = 1000;
	uint32_t rx = 0;

	memset(h, 0, sizeof(h));
	memset(&tot, 0, sizeof(tot));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	for (int k = 0; k < 1200; k++) { /* 120 s */
		double off = 80.0, out, cap = k < 300 ? 3.0 : 5.0; /* Mbit a tick */
		uint64_t pk;

		if (tot.bps && off > (double)tot.bps / MBIT)
			off = (double)tot.bps / MBIT; /* the shaper holds the rest, ahead of the link */
		off *= 0.1;
		pk = (uint64_t)(off * MBIT / 8 / 1000);
		q += off;
		out = q < cap ? q : cap;
		q -= out;
		t += 100;
		tx_p += pk;
		tx_b += pk * 1000;
		rx += (uint32_t)(out * MBIT / 8 / 1000);
		h[0].owd = 20000u + (uint32_t)(q / (cap * 10) * 1000 * MS);
		h[0].have_owd = 1;
		h[0].fresh_until_ms = t + 3000;
		cg_cap_tick(&l[0], t, t, rx, tx_p, tx_b, &h[0], &CFG);
		cg_cap_total_tick(&tot, l, 0x1, CFG.sat_us);
		if (k >= 600) {
			if (q / 50.0 * 1000 > maxq_late)
				maxq_late = q / 50.0 * 1000;
			rate_late += (double)tot.bps / MBIT / 600;
		}
	}
	CHECK(rate_late > 40 && rate_late < 52);
	CHECK(maxq_late < 100);
}

/* What a split would offer a link (the probe, roughly): up to load, 5/4 of
 * its capacity while its queue is short, 9/10 of it while it holds one. */
static double offer(const struct sim *s, double load)
{
	double c = (double)cg_cap_bps(&s->l) / MBIT, o;

	if (!s->l.have_rate)
		return load;
	o = s->l.queue_us >= 0 && s->l.queue_us < (int32_t)(10 * MS) ? c * 1.25 : c * 0.9;
	return o < load ? o : load;
}

/* Steps until the capacity is back to at least mbit (-1: not in steps). */
static int back_in(struct sim *s, int steps, double load, double cap_mbit, double mbit, uint64_t *peak)
{
	int at = -1;

	for (int i = 0; i < steps; i++) {
		sim_step(s, offer(s, load), cap_mbit);
		if (at < 0 && cg_cap_bps(&s->l) >= (uint64_t)(mbit * MBIT))
			at = i + 1;
		if (peak && cg_cap_bps(&s->l) > *peak)
			*peak = cg_cap_bps(&s->l);
	}
	return at;
}

/* Fast return (story 013, 2026-10-16): a link carrying 40 of its 50 has
 * its cell loaded by someone else for 10 s (it gives 13): it is found full
 * and brought down; once the load goes and its queue has stayed short for
 * a second, its capacity from before is back at once, not after the 10 s
 * of the fast start and the probing's ~20 (the field). The same for
 * bursts of 2 s. An episode of over CG_CAP_EP_MS is a slower link: no. */
static void test_capacity_fast_return(void)
{
	struct sim s;
	uint64_t peak = 0;
	int at;

	sim_start(&s);
	for (int i = 0; i < 200; i++)
		sim_step(&s, offer(&s, 40), 50);
	CHECK(near(cg_cap_bps(&s.l), 40, 0.05));
	for (int i = 0; i < 100; i++)
		sim_step(&s, offer(&s, 40), 13);
	CHECK(cg_cap_bps(&s.l) < 16 * MBIT);
	CHECK(s.l.ep_ms != 0 && near(s.l.pre_bps, 40, 0.05));
	at = back_in(&s, 200, 40, 50, 32, &peak);
	CHECK(at > 0 && at <= 25); /* 80 % within 2.5 s */
	CHECK(s.l.back_ms != 0 && s.l.ep_ms == 0);
	CHECK(peak < 42 * MBIT); /* and not more than before */
	/* bursts of 2 s, 8 s apart */
	for (int b = 0; b < 4; b++) {
		for (int i = 0; i < 20; i++)
			sim_step(&s, offer(&s, 40), 13);
		at = back_in(&s, 80, 40, 50, 32, NULL);
		CHECK(at > 0 && at <= 25);
	}
	/* 40 s at 13: a slower link, it waits for the fast start */
	for (int i = 0; i < 400; i++)
		sim_step(&s, offer(&s, 40), 13);
	s.l.back_ms = 0;
	at = back_in(&s, 50, 40, 50, 32, NULL);
	CHECK(at < 0);
	CHECK(s.l.back_ms == 0);
}

/* A link that really got slower (50 to 13 for good): probed at 5/4, its
 * queue is never calm for a second, and it does not come back to its old
 * capacity by this path (the fast start's half best, every CG_CAP_REST_MS,
 * is the only bounce, as before). */
static void test_capacity_no_bounce(void)
{
	struct sim s;
	int restores = 0;
	uint64_t last = 0;

	sim_start(&s);
	for (int i = 0; i < 200; i++)
		sim_step(&s, offer(&s, 40), 50);
	for (int i = 0; i < 1800; i++) { /* 3 min at 13 */
		sim_step(&s, offer(&s, 40), 13);
		if (s.l.back_ms != last) {
			restores++;
			last = s.l.back_ms;
		}
	}
	CHECK(restores <= 1);
	CHECK(cg_cap_bps(&s.l) <= 20 * MBIT + MBIT / 10);
}

/* The restore's backoff, step by step: calm for CG_CAP_CALM_MS gives the
 * capacity back; full again within CG_CAP_RETRY_MS of it is a failed
 * restore, and the next waits CG_CAP_RETRY_MS << failures; a restore that
 * held clears them. A long episode gives nothing back. */
static void test_capacity_backoff(void)
{
	struct cg_cap_link l;
	uint64_t t = 100000, back;

	measured(&l, 40, 0);
	l.ql_us = 0;
	cg_cap_back(&l, t, 10 * MS); /* no episode: nothing */
	CHECK(l.calm_ms == 0 && cg_cap_bps(&l) == 40 * MBIT);
	cg_cap_full(&l, 13 * MBIT, t);
	CHECK(l.ep_ms == t && l.pre_bps == 40 * MBIT && cg_cap_bps(&l) == 13 * MBIT);
	l.ql_us = 50 * MS; /* not calm */
	cg_cap_back(&l, t + 500, 10 * MS);
	CHECK(l.calm_ms == 0);
	l.ql_us = 2 * MS;
	l.have_s1 = 1;
	l.s1 = 20 * MS; /* short, but still rising */
	cg_cap_back(&l, t + 600, 10 * MS);
	CHECK(l.calm_ms == 0);
	l.s1 = 0;
	cg_cap_back(&l, t + 700, 10 * MS);
	CHECK(l.calm_ms == t + 700);
	cg_cap_back(&l, t + 1600, 10 * MS);
	CHECK(cg_cap_bps(&l) == 13 * MBIT);
	cg_cap_back(&l, t + 1700, 10 * MS);
	CHECK(cg_cap_bps(&l) == 40 * MBIT && l.back_ms == t + 1700 && l.ep_ms == 0);
	back = t + 1700;
	/* full again 1 s later: failed; calm again, but it waits 2 x 5 s */
	cg_cap_full(&l, 13 * MBIT, back + 1000);
	CHECK(l.fail_n == 1 && l.pre_bps == 40 * MBIT);
	for (uint64_t k = back + 1100; k < back + 10000; k += 100)
		cg_cap_back(&l, k, 10 * MS);
	CHECK(cg_cap_bps(&l) == 13 * MBIT && l.back_ms == back);
	/* the episode ended without a restore; probed back up to 40 meanwhile,
	 * a new one starts */
	l.max_cur = l.max_prev = 40 * MBIT;
	cg_cap_full(&l, 13 * MBIT, back + 10000);
	CHECK(l.fail_n == 0); /* 10 s after the restore: that one held */
	/* again within 5 s of a restore, twice: 10 s, then 20 s */
	l.fail_n = 1;
	for (uint64_t k = back + 10100; k < back + 12000; k += 100)
		cg_cap_back(&l, k, 10 * MS);
	CHECK(cg_cap_bps(&l) == 40 * MBIT && l.back_ms > back + 11000);
	back = l.back_ms;
	cg_cap_full(&l, 13 * MBIT, back + 500);
	CHECK(l.fail_n == 2);
	l.fail_n = CG_CAP_FAIL_MAX;
	for (uint64_t k = back + 600; k < back + (CG_CAP_RETRY_MS << CG_CAP_FAIL_MAX); k += 100)
		cg_cap_back(&l, k, 10 * MS);
	CHECK(cg_cap_bps(&l) == 13 * MBIT);
	/* a long episode: calm only after CG_CAP_EP_MS, nothing back */
	measured(&l, 40, 0);
	cg_cap_full(&l, 13 * MBIT, t);
	l.ql_us = 0;
	cg_cap_back(&l, t + CG_CAP_EP_MS, 10 * MS);
	cg_cap_back(&l, t + CG_CAP_EP_MS + CG_CAP_CALM_MS, 10 * MS);
	CHECK(cg_cap_bps(&l) == 13 * MBIT && l.ep_ms == 0 && l.back_ms == 0);
}

/* No overshoot from a drain (the field: after a load of 40 Mbit/s on the
 * same cell, the capacity went to 24 against ~9 for ~10 s): a link sent 8
 * of the 24 it can carry is loaded down to 3 for 10 s, its queue grows to
 * seconds, and when the load goes it empties that queue at 24. Those
 * samples deliver more than was sent: they raise neither the capacity nor
 * the average while full. */
static void test_capacity_drain(void)
{
	struct sim s;
	uint64_t peak = 0;

	sim_start(&s);
	run(&s, 200, 8, 24);
	CHECK(near(cg_cap_bps(&s.l), 8, 0.05));
	run(&s, 100, 8, 3);
	CHECK(cg_cap_bps(&s.l) < 5 * MBIT);
	CHECK(s.l.queue_us > (int32_t)(1000 * MS));
	for (int i = 0; i < 200; i++) {
		sim_step(&s, 8, 24);
		if (cg_cap_bps(&s.l) > peak)
			peak = cg_cap_bps(&s.l);
	}
	CHECK(s.l.rate_bps < 10 * MBIT); /* the drain counted for the rate */
	CHECK(peak < 9 * MBIT);
	CHECK(near(cg_cap_bps(&s.l), 8, 0.1)); /* back to before */
}

/* split_predict: the queue in a second only for a rise that lasts; under
 * the swings of a cellular delay it stays the queue now. */
static void test_capacity_predict(void)
{
	struct cg_cap_link l;
	struct cg_hlink h;
	uint32_t seed = 12345;
	uint64_t t = 1000;
	int on = 0, at = -1;

	cg_cap_reset(&l);
	memset(&h, 0, sizeof(h));
	CHECK(cg_cap_pred_us(&l) == -1);
	h.have_owd = 1;
	for (int k = 0; k < 6000; k++) { /* 10 min swinging over 30 ms */
		t += 100;
		seed = seed * 1103515245u + 12345u;
		h.owd = 30000 + (seed >> 8) % (30 * MS + 1);
		h.fresh_until_ms = t + 3000;
		cg_cap_tick(&l, t, 0, 0, 0, 0, &h, &CFG);
		CHECK(cg_cap_pred_us(&l) >= l.queue_us);
		CHECK(cg_cap_split_q(&l, 0) == l.queue_us);
		if (k > 700)
			on += cg_cap_pred_us(&l) > l.queue_us;
	}
	CHECK(on < 53); /* under 1 % */
	/* a queue growing 40 ms a second (the field's 20 Mbit/s upload) */
	for (int k = 0; k < 40; k++) {
		t += 100;
		seed = seed * 1103515245u + 12345u;
		h.owd = 30000 + (seed >> 8) % (20 * MS + 1) + (uint32_t)k * 4 * MS;
		h.fresh_until_ms = t + 3000;
		cg_cap_tick(&l, t, 0, 0, 0, 0, &h, &CFG);
		CHECK(cg_cap_pred_us(&l) <= l.queue_us + CG_CAP_PRED_MAX_US);
		if (at < 0 && cg_cap_pred_us(&l) > l.queue_us)
			at = k + 1;
	}
	CHECK(at > 0 && at <= 10);
	CHECK(cg_cap_split_q(&l, 1) == cg_cap_pred_us(&l));
	/* the clamp */
	l.s1 = 10 * 1000 * 1000;
	l.rise_n = CG_CAP_PRED_N;
	CHECK(cg_cap_pred_us(&l) == l.queue_us + CG_CAP_PRED_MAX_US);
	/* in the weights: a link whose queue is short but rising fast is not
	 * probed with predict */
	{
		struct cg_cap_link ls[CG_MAX_LINKS];
		uint16_t w[CG_MAX_LINKS];

		for (unsigned i = 0; i < CG_MAX_LINKS; i++)
			cg_cap_reset(&ls[i]);
		measured(&ls[0], 20, 2);
		measured(&ls[1], 20, 2);
		ls[1].have_s1 = 1;
		ls[1].s1 = 40 * MS;
		ls[1].rise_n = CG_CAP_PRED_N;
		cg_cap_weights(ls, 0xfffc, NULL, 10 * MS, 0, w);
		CHECK(w[0] == w[1]);
		cg_cap_weights(ls, 0xfffc, NULL, 10 * MS, 1, w);
		CHECK(w[0] == CG_GOV_MAX && w[1] == 761); /* 20 against 25 + 20 / 16 */
	}
}

void test_capacity(void)
{
	test_capacity_fast_return();
	test_capacity_no_bounce();
	test_capacity_backoff();
	test_capacity_drain();
	test_capacity_predict();
	test_capacity_short();
	test_capacity_total();
	test_capacity_shapes();
	test_capacity_converges();
	test_capacity_hiccup();
	test_capacity_fast_start();
	test_capacity_probe();
	test_capacity_weights();
	test_capacity_rates();
	test_capacity_saturated();
	test_capacity_ramp();
	test_capacity_swing();
	test_capacity_rescue_w();
	test_capacity_edges();
}
