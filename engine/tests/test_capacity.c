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
	uint32_t buf_pkts;  /* the link's buffer, tail drop (0: unbounded) */
	uint32_t drop_every; /* one of every this many packets lost before the queue (0: none) */
	uint64_t drop_acc;
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
static void sim_step_cfg(struct sim *s, double send_mbit, double cap_mbit, const struct cg_cap_cfg *cfg);

static void sim_step(struct sim *s, double send_mbit, double cap_mbit)
{
	sim_step_cfg(s, send_mbit, cap_mbit, &CFG);
}

static void sim_step_cfg(struct sim *s, double send_mbit, double cap_mbit, const struct cg_cap_cfg *cfg)
{
	double sent = send_mbit * MBIT / 8 / 1000 / 10, can = cap_mbit * MBIT / 8 / 1000 / 10, out;

	s->t += 100;
	s->tx_pkts += (uint64_t)sent;
	s->tx_bytes += (uint64_t)sent * 1000;
	s->backlog += (double)(uint64_t)sent;
	if (s->drop_every) { /* random loss, no queue: one of every drop_every */
		s->drop_acc += (uint64_t)sent;
		s->backlog -= (double)(s->drop_acc / s->drop_every);
		s->drop_acc %= s->drop_every;
	}
	out = s->backlog < can ? s->backlog : can;
	s->backlog -= (double)(uint32_t)out;
	s->rx += (uint32_t)out;
	if (s->buf_pkts && s->backlog > s->buf_pkts) /* tail drop */
		s->backlog = s->buf_pkts;
	s->h.owd = 30000u + s->jitter_us + (uint32_t)(s->backlog / can * 100 * MS); /* queue: backlog over the rate */
	s->h.have_owd = 1;
	s->h.fresh_until_ms = s->t + 3000;
	cg_cap_tick(&s->l, s->t, s->t, s->rx, s->tx_pkts, s->tx_bytes, s->stale ? NULL : &s->h, cfg);
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
	run(&s, 30, 12, 10); /* full at 10 */
	CHECK(near(cg_cap_bps(&s.l), 10, 0.1));
	run(&s, 50, 0.5, 10); /* idle 5 s: not yet */
	CHECK(cg_cap_bps(&s.l) < 12 * MBIT);
	run(&s, 60, 0.5, 10); /* past 10 s: half its best */
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* full again at 10: it measures 10 again, and holds it 10 s */
	run(&s, 30, 12, 10);
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
	cg_cap_weights(l, 0, NULL, 10 * MS, w); /* nothing measured: all at the top */
	CHECK(w[0] == CG_GOV_MAX && w[15] == CG_GOV_MAX);

	measured(&l[0], 200, 50); /* full: no probe */
	measured(&l[1], 20, 50);
	measured(&l[2], 100, 2); /* room: 125 */
	measured(&l[3], 1, 50);  /* below the floor */
	cg_cap_weights(l, 0, NULL, 10 * MS, w);
	/* 100 with room: 125 and 1/16 of 200, 137.5 */
	CHECK(w[0] == CG_GOV_MAX && w[1] == 100 && w[2] == 687 && w[3] == CG_GOV_MIN);
	CHECK(w[4] == CG_GOV_MAX); /* not measured */
	measured(&l[6], 0, 900); /* measured, delivering nothing: the floor */
	cg_cap_weights(l, 0, NULL, 10 * MS, w);
	CHECK(w[6] == CG_GOV_MIN && w[4] == CG_GOV_MAX);
	/* a probing link above the others sets the scale */
	measured(&l[2], 200, 2);
	cg_cap_weights(l, 0, NULL, 10 * MS, w);
	CHECK(w[2] == CG_GOV_MAX && w[0] == 761 && w[1] == 76);
	/* dup0 keeps 0 and does not set the scale */
	measured(&l[5], 1000, 50);
	cg_cap_weights(l, 1u << 5, NULL, 10 * MS, w);
	CHECK(w[5] == 0 && w[2] == CG_GOV_MAX);
	/* the governor scales each share */
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		gov[i] = CG_GOV_MAX;
	gov[0] = 500;
	gov[1] = 10;
	cg_cap_weights(l, 1u << 5, gov, 10 * MS, w);
	CHECK(w[0] == 380 && w[1] == CG_GOV_MIN && w[2] == CG_GOV_MAX);
	/* a link left at 0.1 Mbit/s by a queue that has gone: 1/16 of the
	 * largest keeps it in the split, so that it can show more */
	measured(&l[1], 0.1, 2);
	cg_cap_weights(l, 1u << 5, NULL, 10 * MS, w);
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

		cg_cap_weights(l, 0xfffc, NULL, CFG.sat_us / 2, w); /* links 2-15 out (dup0) */
		tot = (double)w[0] + w[1];
		t += 100;
		for (int i = 0; i < 2; i++) {
			double in = load * w[i] / tot * 0.1, out; /* Mbit this tick */
			uint64_t pk = (uint64_t)(in * MBIT / 8 / 1000);

			q[i] += in;
			out = q[i] < cap[i] * 0.1 ? q[i] : cap[i] * 0.1;
			q[i] -= out;
			tx_p[i] += pk;
			tx_b[i] += pk * 1000;
			rx[i] += (uint32_t)(out * MBIT / 8 / 1000);
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

/* A link not loaded for CG_CAP_STALE_MS: the ETA takes its capacity as
 * twice, then twice again every CG_CAP_STALE_MS, up to the ceiling; loaded
 * again, its own. cg_cap_tickle names it due, at cg_cap_probe's rate. */
static void test_capacity_stale(void)
{
	struct sim s;
	struct cg_cap_link many[CG_MAX_LINKS];
	uint64_t c, tb[CG_MAX_LINKS], tc[CG_MAX_LINKS];
	uint16_t due;

	sim_start(&s);
	run(&s, 30, 10, 100); /* loaded: 10 of a capacity of 10 */
	c = cg_cap_bps(&s.l);
	CHECK(near(c, 10, 0.15));
	CHECK(!cg_cap_stale_p(&s.l, s.t));
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t, 64), c);
	run(&s, 20, 0.5, 100); /* 2 s at a twentieth: not stale yet */
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t, 64), c);
	run(&s, 15, 0.5, 100); /* 3.5 s */
	CHECK(cg_cap_stale_p(&s.l, s.t));
	CHECK_EQ(cg_cap_bps(&s.l), c); /* the capacity itself holds */
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t, 64), 2 * c);
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t + 3000, 64), 4 * c);
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t + 60000, 64), 40 * MBIT); /* the ceiling */
	CHECK_EQ(cg_cap_stale(&s.l, 50 * MBIT, 40 * MBIT, s.t + 60000, 64), 50 * MBIT); /* over it: as is */
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t + 60000, 2), 2 * c); /* max_mul */
	CHECK_EQ(cg_cap_stale(&s.l, c, 40 * MBIT, s.t + 60000, 1), c);     /* off */
	memset(many, 0, sizeof(many));
	many[0] = s.l;
	cg_cap_reset(&many[1]); /* not measured: never due */
	cg_cap_tickle(many, 0x3, s.t, &due, tb, tc);
	CHECK_EQ(due, 0x1);
	CHECK_EQ(tb[0], cg_cap_probe(&s.l, c, c));
	CHECK_EQ(tb[1], 0);
	CHECK_EQ(tc[0], c);
	CHECK_EQ(tc[1], 0);
	cg_cap_tickle(many, 0x2, s.t, &due, tb, tc); /* not in the mask */
	CHECK_EQ(due, 0);
	run(&s, 10, 12, 100); /* tickled: loaded again */
	CHECK(!cg_cap_stale_p(&s.l, s.t));
	CHECK(cg_cap_bps(&s.l) > c); /* and it showed more */
	CHECK_EQ(cg_cap_stale(&s.l, cg_cap_bps(&s.l), 40 * MBIT, s.t, 64), cg_cap_bps(&s.l));
}

/* capacity_loss_pct: a loaded link that drops instead of queueing (no
 * queue, 15 % lost) is full at what it delivers after CG_CAP_LOSS_N
 * samples; off, or at a light load, it is not. */
static void loss_feed(struct cg_cap_link *l, const struct cg_cap_cfg *cfg, uint64_t *t, uint32_t *rx, uint64_t *tx,
		      int samples, uint32_t sent, uint32_t delivered)
{
	struct cg_hlink h;

	memset(&h, 0, sizeof(h));
	h.have_owd = 1;
	h.owd = 30000; /* no queue at all */
	for (int i = 0; i < samples; i++) {
		*t += 500;
		*tx += sent;
		*rx += delivered;
		h.fresh_until_ms = *t + 3000;
		cg_cap_tick(l, *t, *t, *rx, *tx, *tx * 1000, &h, cfg);
	}
}

static void test_capacity_loss(void)
{
	const struct cg_cap_cfg on = { .window_ms = 10000, .sat_us = 20 * MS, .loss_pct = 5 };
	struct cg_cap_link l;
	uint64_t t = 1000, tx = 0;
	uint32_t rx = 0;

	/* 6000 packets of 1000 B per 500 ms (96 Mbit/s), all delivered */
	cg_cap_reset(&l);
	loss_feed(&l, &on, &t, &rx, &tx, 4, 6000, 6000);
	CHECK(near(cg_cap_bps(&l), 96, 0.02));
	/* then 15 % lost, no queue: cut after CG_CAP_LOSS_N samples */
	loss_feed(&l, &on, &t, &rx, &tx, CG_CAP_LOSS_N - 1, 6000, 5100);
	CHECK(l.cut == CG_CAP_CUT_NONE);
	loss_feed(&l, &on, &t, &rx, &tx, 1, 6000, 5100);
	CHECK(l.cut == CG_CAP_CUT_LOSS);
	CHECK_EQ(l.n_cut_loss, 1);
	CHECK(cg_cap_bps(&l) <= 82 * MBIT); /* what it delivered, 81.6 */
	CHECK(l.full_ms == t);             /* fast start waits */
	/* off: the same loss never cuts */
	cg_cap_reset(&l);
	loss_feed(&l, &CFG, &t, &rx, &tx, 4, 6000, 6000);
	loss_feed(&l, &CFG, &t, &rx, &tx, 6, 6000, 5100);
	CHECK(l.cut == CG_CAP_CUT_NONE && near(cg_cap_bps(&l), 96, 0.02));
	/* a light load (a tenth of its capacity) losing 15 %: the radio's, no cut */
	cg_cap_reset(&l);
	loss_feed(&l, &on, &t, &rx, &tx, 4, 6000, 6000);
	loss_feed(&l, &on, &t, &rx, &tx, 6, 600, 510);
	CHECK(l.cut == CG_CAP_CUT_NONE && l.loss_n == 0);
}

/* lossy only at the frontier: a link losing 2 % near its best is lossy; one
 * losing 2 % at a tenth of its best (a misaligned dish) is not. */
static void test_capacity_lossy_frontier(void)
{
	const struct cg_cap_cfg cf = { .window_ms = 10000, .sat_us = 20 * MS, .lossy = 1 };
	struct cg_cap_link l;
	uint64_t t = 1000, tx = 0;
	uint32_t rx = 0;

	cg_cap_reset(&l);
	loss_feed(&l, &cf, &t, &rx, &tx, 4, 6000, 6000); /* best 96 Mbit/s */
	loss_feed(&l, &cf, &t, &rx, &tx, 2, 6000, 5880);
	CHECK(l.lossy);
	cg_cap_reset(&l);
	loss_feed(&l, &cf, &t, &rx, &tx, 4, 6000, 6000);
	t += CG_CAP_LOSSY_MS;
	loss_feed(&l, &cf, &t, &rx, &tx, 6, 600, 588);
	CHECK(!l.lossy);
	t += 40000;
	loss_feed(&l, &cf, &t, &rx, &tx, 6, 600, 588);
	CHECK(!l.lossy);
	CHECK(l.peak_bps > 80000000ull);
}

/* The audit's second pass: the rescue's full bit with hysteresis, the G4
 * episode for fast start and the predicted queue, each by its mode. */
static void test_capacity_clock2(void)
{
	struct cg_cap_link l;
	uint32_t t = 20 * MS;

	/* rfull: on at 4 x, mode 1 off under 3 x only, mode 2 off at once
	 * while rising */
	cg_cap_reset(&l);
	l.ql_us = 85 * MS;
	cg_cap_rfull(&l, t, 1);
	CHECK(l.rfull);
	l.ql_us = 70 * MS; /* between 3 x and 4 x, not rising: kept */
	cg_cap_rfull(&l, t, 1);
	CHECK(l.rfull);
	CHECK(cg_cap_full(&l, t, 1));
	CHECK(!cg_cap_full(&l, t, 0)); /* the old bit: off under 4 x */
	l.ql_us = 55 * MS;
	cg_cap_rfull(&l, t, 1);
	CHECK(!l.rfull);
	l.ql_us = 85 * MS;
	cg_cap_rfull(&l, t, 2);
	CHECK(l.rfull);
	l.ql_us = 75 * MS; /* falling (s1 < 0): held */
	l.have_s1 = 1;
	l.s1 = -10000;
	l.s2 = 0;
	cg_cap_rfull(&l, t, 2);
	CHECK(l.rfull);
	l.ql_us = 65 * MS; /* rising: off at once */
	l.s1 = 5 * MS;
	cg_cap_rfull(&l, t, 2);
	CHECK(!l.rfull);

	/* episode: mode 1 always; 2 and 3 only when the capacity overshot */
	cg_cap_reset(&l);
	l.have_rate = 1;
	l.max_cur = l.max_prev = 40 * MBIT;
	l.last_tx_bps = 40 * MBIT;
	l.last_rx_bps = 38 * MBIT; /* carries what it is sent: right */
	CHECK(!cg_cap_episode(&l, 5000, 2));
	CHECK(!cg_cap_episode(&l, 5000, 0));
	CHECK(cg_cap_episode(&l, 5000, 1) && l.full_ms == 5000);
	l.full_ms = 0;
	l.last_rx_bps = 30 * MBIT; /* sent 40, carries 30: an overestimate */
	CHECK(cg_cap_episode(&l, 6000, 2) && l.full_ms == 6000);
	l.full_ms = 0;
	l.last_tx_bps = 20 * MBIT; /* not sent its capacity: says nothing */
	CHECK(!cg_cap_episode(&l, 6000, 3));
	l.last_tx_bps = 40 * MBIT;
	CHECK(cg_cap_episode(&l, 7000, 3) && l.ep_ms == 7000 && !l.full_ms);
	/* mode 3 holds fast start back CG_CAP_EP_REST_MS only */
	l.best_cur = l.best_prev = 100 * MBIT;
	l.best_ms = 7000;
	CHECK(!cg_cap_best(&l, 7000 + CG_CAP_EP_REST_MS - 1));
	CHECK(cg_cap_best(&l, 7000 + CG_CAP_EP_REST_MS));
	CHECK_EQ(l.n_g4_ep, 3);

	/* qpred: mode 0 both ways, 1 never under the queue, 2 at most twice */
	cg_cap_reset(&l);
	l.have_s1 = 1;
	l.s1 = -40000; /* draining 40 ms a second */
	l.s2 = 0;
	CHECK_EQ(cg_cap_qpred(&l, 30 * MS, 500, 0), 10 * MS);
	CHECK_EQ(cg_cap_qpred(&l, 30 * MS, 500, 1), 30 * MS);
	l.s1 = 100 * MS;
	CHECK_EQ(cg_cap_qpred(&l, 30 * MS, 500, 1), 80 * MS);
	CHECK_EQ(cg_cap_qpred(&l, 30 * MS, 500, 2), 60 * MS);
	CHECK_EQ(cg_cap_qpred(&l, 30 * MS, 0, 2), 30 * MS);
}

/* A sample's loss counts what was sent CG_CAP_LAG_MS before its report:
 * a link ramping up with 100 ms of packets in flight, none lost, is not
 * lossy (its own sent count is 20 % ahead of what arrived). */
static void test_capacity_inflight(void)
{
	const struct cg_cap_cfg cf = { .window_ms = 10000, .sat_us = 20 * MS, .lossy = 1 };
	struct cg_cap_link l;
	struct cg_hlink h;
	uint64_t t = 1000, tx = 0, sent_at[64];
	uint32_t n = 0;

	memset(&h, 0, sizeof(h));
	h.have_owd = 1;
	h.owd = 30000;
	cg_cap_reset(&l);
	/* ticks each 100 ms, a report each 500 ms; rate rising 1000 to 4000 a tick */
	for (int k = 0; k < 60; k++) {
		uint32_t rx;

		t += 100;
		tx += 1000 + 50 * (uint64_t)k;
		sent_at[k] = tx;
		rx = k ? (uint32_t)sent_at[k - 1] : 0; /* what was sent a tick (100 ms) before */
		h.fresh_until_ms = t + 3000;
		cg_cap_tick(&l, t, k % 5 == 4 ? t : 0, rx, tx, tx * 1000, &h, &cf);
		n += l.lossy;
	}
	CHECK_EQ(n, 0);
	/* the same with 5 % of it lost: lossy */
	cg_cap_reset(&l);
	n = 0;
	tx = 0;
	for (int k = 0; k < 60; k++) {
		uint32_t rx;

		t += 100;
		tx += 3000;
		sent_at[k] = tx;
		rx = k ? (uint32_t)(sent_at[k - 1] * 95 / 100) : 0;
		h.fresh_until_ms = t + 3000;
		cg_cap_tick(&l, t, k % 5 == 4 ? t : 0, rx, tx, tx * 1000, &h, &cf);
		n += l.lossy;
	}
	CHECK(n > 0);
}

/* A link 15 ms away whose sending rate falls, none lost: by CG_CAP_LAG_MS
 * (250 ms behind, against 15) every sample counts a loss; by sequence
 * (capacity_seq_lag), none. tick: the packets sent in each 100 ms tick, the
 * sequence the same count (one link). lost: per mille lost. Returns the
 * samples marked lossy, and the last state in *l. */
static unsigned seq_feed(struct cg_cap_link *l, struct cg_cap_cfg *cf, unsigned lost, int falling)
{
	struct cg_hlink h;
	uint64_t t = 100000, tx = 0, at[128];
	unsigned n = 0;

	memset(&h, 0, sizeof(h));
	h.have_owd = 1;
	h.owd = 30000;
	cg_cap_reset(l);
	for (int k = 0; k < 120; k++) {
		uint64_t arrived;

		t += 100;
		/* 3000 a tick, then falling 1 % a tick, or steady */
		at[k] = tx += falling && k > 20 ? 3000 - 15 * (uint64_t)(k - 20) : 3000;
		/* what arrived by now: what was sent 15 ms ago (between the ticks) */
		arrived = k ? at[k - 1] + (at[k] - at[k - 1]) * 85 / 100 : 0;
		cf->tx_seq = (uint32_t)tx;
		cf->rx_top = (uint32_t)arrived; /* the newest: sequence arrived - 1, + 1 for 0 none */
		h.fresh_until_ms = t + 3000;
		cg_cap_tick(l, t, k % 5 == 4 ? t : 0, (uint32_t)(arrived - arrived * lost / 1000), tx, tx * 1000, &h, cf);
		n += k > 30 && l->lossy;
	}
	return n;
}

static void test_capacity_seq_lag(void)
{
	struct cg_cap_cfg cf = { .window_ms = 10000, .sat_us = 20 * MS, .lossy = 1 };
	struct cg_cap_link l;

	/* by CG_CAP_LAG_MS: a falling rate reads as a loss */
	CHECK(seq_feed(&l, &cf, 0, 1) > 0);
	/* by sequence: none */
	cf.seq_lag = 1;
	CHECK_EQ(seq_feed(&l, &cf, 0, 1), 0);
	CHECK_EQ(seq_feed(&l, &cf, 0, 0), 0);
	CHECK(!l.lost_ms);
	/* and 1 % lost is a loss */
	CHECK(seq_feed(&l, &cf, 10, 0) > 0);
	CHECK(l.lost_ms);
	/* a slower link: behind_us moves what could have arrived back */
	cf.behind_us = 20000;
	CHECK_EQ(seq_feed(&l, &cf, 0, 0), 0);
	cf.behind_us = 0;
	/* no rx_top (an old peer): CG_CAP_LAG_MS as before */
	{
		struct cg_cap_cfg c2 = cf;
		struct cg_hlink h;
		uint64_t t = 1000, tx = 0;
		uint32_t rx = 0;

		memset(&h, 0, sizeof(h));
		cg_cap_reset(&l);
		for (int k = 0; k < 40; k++) {
			t += 100;
			tx += 3000;
			rx = (uint32_t)(tx - 3000);
			c2.tx_seq = (uint32_t)tx;
			c2.rx_top = 0;
			cg_cap_tick(&l, t, k % 5 == 4 ? t : 0, rx, tx, tx * 1000, &h, &c2);
		}
		CHECK(!l.lossy);
	}
}

/* Starlink's slot clock (slot.h): a sample sent across an edge's window
 * counts no loss; the same loss in the middle of a slot does. */
static void test_capacity_slot(void)
{
	const struct cg_slot slot = { .edge_ms = CG_SLOT_EDGE_MS, .pre_ms = CG_SLOT_PRE_MS, .post_ms = CG_SLOT_POST_MS };
	struct cg_cap_cfg cf = { .window_ms = 10000, .sat_us = 20 * MS, .lossy = 1, .seq_lag = 1 };
	struct cg_hlink h;
	struct cg_cap_link l;

	memset(&h, 0, sizeof(h));
	h.have_owd = 1;
	h.owd = 30000;
	for (int with = 0; with < 2; with++) {
		uint64_t t = 100000, tx = 0, gone = 0, lossy_n = 0;

		/* the wall clock: now_ms 100000 is :11.000 */
		cf.real_off_ms = 1700000000000ll + 11000 - 100000 - 1700000000000ll % 60000;
		cf.slot = with ? &slot : NULL;
		cg_cap_reset(&l);
		for (int k = 0; k < 400; k++) { /* 40 s: the edges at :12, :27 and :42 */
			uint64_t wall;

			t += 100;
			tx += 3000;
			wall = (uint64_t)((int64_t)t + cf.real_off_ms);
			/* 3 % of a tick lost at each switch (:12, :27...) */
			if (cg_slot_since(&slot, wall) < 100)
				gone += 90;
			cf.tx_seq = (uint32_t)tx;
			cf.rx_top = (uint32_t)(tx - 3000);
			h.fresh_until_ms = t + 3000;
			cg_cap_tick(&l, t, k % 5 == 4 ? t : 0, (uint32_t)(tx - 3000 - gone), tx, tx * 1000, &h, &cf);
			lossy_n += l.lossy;
		}
		if (with) {
			CHECK_EQ(lossy_n, 0);
			CHECK(l.n_edge >= 3);
			CHECK(!l.lost_ms);
		} else {
			CHECK(lossy_n > 0);
		}
	}
}

void test_capacity(void)
{
	test_capacity_seq_lag();
	test_capacity_slot();
	test_capacity_inflight();
	test_capacity_lossy_frontier();
	test_capacity_loss();
	test_capacity_clock2();
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
	test_capacity_stale();
}
