/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "decrec.h"
#include "test.h"

#define MSUS 1000u
#define MBIT 1000000ull
#define SPREAD (30 * MSUS) /* split_spread_ms 30 */
#define SETTLE 2000        /* mute_settle_ms 2000 */

/* The ring: newest first, the oldest overwritten past CG_DEC_N, every
 * mechanism counted since the start. */
static void test_decision_ring(void)
{
	static struct cg_decring r;
	const struct cg_dec *d;

	cg_dec_reset(&r);
	CHECK_EQ(cg_dec_count(&r), 0);
	CHECK(cg_dec_get(&r, 0) == NULL);
	cg_dec_add(&r, 100, 1, CG_DEC_MUTE, 0, 1, 40000);
	cg_dec_add(&r, 200, 2, CG_DEC_LEAVE_QUEUE, CG_G4_IN, CG_G4_QLEFT, 35000);
	cg_dec_add(&r, 300, 999, CG_DEC_STRESS_ON, 0, 1, 1); /* a link out of range: the direction */
	CHECK_EQ(cg_dec_count(&r), 3);
	d = cg_dec_get(&r, 0);
	CHECK(d && d->t_ms == 300 && d->mech == CG_DEC_STRESS_ON && d->link == CG_DEC_NOLINK);
	d = cg_dec_get(&r, 2);
	CHECK(d && d->t_ms == 100 && d->link == 1 && d->from == 0 && d->to == 1 && d->why == 40000);
	CHECK(cg_dec_get(&r, 3) == NULL);
	cg_dec_add(&r, 400, 0, CG_DEC_MECHS, 0, 0, 0); /* not a mechanism: ignored */
	CHECK_EQ(cg_dec_count(&r), 3);

	/* wrap: 200 more; the newest 64 are kept, in order */
	for (int k = 0; k < 200; k++)
		cg_dec_add(&r, 1000 + (uint64_t)k, (unsigned)k % 4, k & 1 ? CG_DEC_CUT_FULL : CG_DEC_GOV_DOWN, k, k + 1,
			   k);
	CHECK_EQ(cg_dec_count(&r), CG_DEC_N);
	CHECK_EQ(r.n, 203);
	for (unsigned k = 0; k < CG_DEC_N; k++) {
		d = cg_dec_get(&r, k);
		CHECK(d != NULL);
		if (d)
			CHECK_EQ(d->why, 199 - (int)k);
	}
	CHECK(cg_dec_get(&r, CG_DEC_N) == NULL);
	CHECK_EQ(r.total[CG_DEC_MUTE], 1);
	CHECK_EQ(r.total[CG_DEC_LEAVE_QUEUE], 1);
	CHECK_EQ(r.total[CG_DEC_STRESS_ON], 1);
	CHECK_EQ(r.total[CG_DEC_CUT_FULL], 100);
	CHECK_EQ(r.total[CG_DEC_GOV_DOWN], 100);
}

/* An edge records once per change, with from and to. */
static void test_decision_edge(void)
{
	static struct cg_decring r;
	uint8_t st = 0;

	cg_dec_reset(&r);
	CHECK(!cg_dec_edge(&r, &st, 0, 10, 3, CG_DEC_STALE, CG_DEC_FRESH, 0));
	CHECK(cg_dec_edge(&r, &st, 5, 20, 3, CG_DEC_STALE, CG_DEC_FRESH, 350)); /* any non-zero is on */
	CHECK_EQ(st, 1);
	CHECK(!cg_dec_edge(&r, &st, 1, 30, 3, CG_DEC_STALE, CG_DEC_FRESH, 450));
	CHECK(cg_dec_edge(&r, &st, 0, 40, 3, CG_DEC_STALE, CG_DEC_FRESH, 20));
	CHECK_EQ(cg_dec_count(&r), 2);
	CHECK(cg_dec_get(&r, 1)->mech == CG_DEC_STALE && cg_dec_get(&r, 1)->from == 0 && cg_dec_get(&r, 1)->to == 1);
	CHECK(cg_dec_get(&r, 0)->mech == CG_DEC_FRESH && cg_dec_get(&r, 0)->from == 1 && cg_dec_get(&r, 0)->to == 0);
	CHECK_EQ(cg_dec_get(&r, 0)->why, 20);
}

/* Every mechanism has its own name and a unit for its why. */
static void test_decision_names(void)
{
	for (unsigned m = 0; m < CG_DEC_MECHS; m++) {
		CHECK(strcmp(cg_dec_name(m), "?") != 0);
		CHECK(strcmp(cg_dec_why_name(m), "why") != 0);
		for (unsigned k = 0; k < m; k++)
			CHECK(strcmp(cg_dec_name(m), cg_dec_name(k)) != 0);
	}
	CHECK(!strcmp(cg_dec_name(CG_DEC_MECHS), "?"));
	CHECK(!strcmp(cg_dec_name(CG_DEC_LEAVE_QUEUE + CG_G4_REJOIN_RECOVER), "rejoin_recover"));
	CHECK(!strcmp(cg_g4_state_name(CG_G4_QLEFT), "qleft"));
	CHECK(!strcmp(cg_g4_state_name(99), "?"));
}

static void delay(struct cg_hlink *h, unsigned link, uint32_t owd, uint64_t now_ms)
{
	h[link].owd = owd;
	h[link].have_owd = 1;
	h[link].fresh_until_ms = now_ms + 3600000;
}

/* One G4 evaluation, recorded as the ticks do. */
static uint16_t g4(struct cg_decring *r, struct cg_spread *sp, const struct cg_hlink *h, uint16_t cand,
		   uint32_t spread, uint64_t t, int rejoin, uint8_t *stress, uint8_t *recover)
{
	uint8_t before[CG_MAX_LINKS];
	uint16_t ch;

	cg_decrec_g4_before(sp, before);
	ch = cg_split_spread_eval_r(sp, h, cand, spread, SETTLE, t, rejoin);
	cg_decrec_g4(r, sp, ch, before, stress, recover, t);
	return ch;
}

/* G4's transitions, each with its reason and the states before and after. */
static void test_decision_g4(void)
{
	static struct cg_decring r;
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;
	uint8_t stress = 0, recover = 0;
	const struct cg_dec *d;

	cg_dec_reset(&r);
	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	CHECK_EQ(cg_g4_state(&sp, 0), CG_G4_OUT);
	/* 3 is a candidate without a delay: waiting */
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	delay(h, 2, 5000000 + 80 * MSUS, t); /* slow by base delay: never in */
	g4(&r, &sp, h, 0xf, SPREAD, t, 1, &stress, &recover);
	CHECK_EQ(cg_g4_state(&sp, 0), CG_G4_IN);
	CHECK_EQ(cg_g4_state(&sp, 2), CG_G4_LEFT);
	CHECK_EQ(cg_g4_state(&sp, 3), CG_G4_WAITING);
	CHECK_EQ(cg_dec_count(&r), 1);
	d = cg_dec_get(&r, 0);
	CHECK(d->mech == CG_DEC_LEAVE_BASE && d->link == 2 && d->from == CG_G4_OUT && d->to == CG_G4_LEFT);
	CHECK_EQ(d->why, 80 * MSUS);
	CHECK_EQ(sp.n_ev[2][CG_G4_LEAVE_BASE], 1);
	CHECK_EQ(sp.ev_ms[2], t);

	/* 1's queue grows: leave_queue, and the stress begins */
	delay(h, 1, 5000000 + 60 * MSUS, t);
	g4(&r, &sp, h, 0xf, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(cg_g4_state(&sp, 1), CG_G4_QLEFT);
	CHECK_EQ(cg_dec_count(&r), 3);
	d = cg_dec_get(&r, 1);
	CHECK(d->mech == CG_DEC_LEAVE_QUEUE && d->link == 1 && d->from == CG_G4_IN && d->to == CG_G4_QLEFT);
	d = cg_dec_get(&r, 0);
	CHECK(d->mech == CG_DEC_STRESS_ON && d->link == CG_DEC_NOLINK && d->why == 1);
	/* nothing changes: nothing recorded */
	g4(&r, &sp, h, 0xf, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(cg_dec_count(&r), 3);

	/* 1 drains and becomes the fastest while 0 queues: rejoin_recover */
	delay(h, 1, 5000000 - 10 * MSUS, t);
	delay(h, 0, 5000000 + 40 * MSUS, t);
	g4(&r, &sp, h, 0xf, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(sp.n_ev[1][CG_G4_REJOIN_RECOVER], 1);
	CHECK_EQ(sp.n_ev[0][CG_G4_LEAVE_QUEUE], 1);
	CHECK_EQ(r.total[CG_DEC_REJOIN_RECOVER], 1);
	CHECK_EQ(r.total[CG_DEC_RECOVER_ON], 1);
	CHECK_EQ(r.total[CG_DEC_LEAVE_QUEUE], 2);
	CHECK_EQ(cg_g4_state(&sp, 1), CG_G4_IN);

	/* 0's queue drains back within 3/4 of the spread: holding, then settle */
	delay(h, 0, 5000000 - 5 * MSUS, t);
	g4(&r, &sp, h, 0xf, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(cg_g4_state(&sp, 0), CG_G4_HOLDING);
	for (int k = 0; k < 25; k++)
		g4(&r, &sp, h, 0xf, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(cg_g4_state(&sp, 0), CG_G4_IN);
	CHECK_EQ(sp.n_ev[0][CG_G4_REJOIN_SETTLE], 1);
	CHECK_EQ(r.total[CG_DEC_REJOIN_SETTLE], 1);
	CHECK_EQ(r.total[CG_DEC_STRESS_OFF], 1);
	CHECK_EQ(stress, 0);

	/* spread to 0: every link that left is back, rejoin_off */
	g4(&r, &sp, h, 0xf, 0, t += 100, 1, &stress, &recover);
	CHECK_EQ(sp.n_ev[2][CG_G4_REJOIN_OFF], 1);
	CHECK_EQ(cg_g4_state(&sp, 2), CG_G4_IN);
	CHECK_EQ(r.total[CG_DEC_REJOIN_OFF], 1);
}

/* A stall, and every fresh link out: their reasons. */
static void test_decision_g4_stall_empty(void)
{
	static struct cg_decring r;
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;
	uint8_t stress = 0, recover = 0;

	cg_dec_reset(&r);
	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	g4(&r, &sp, h, 0x3, SPREAD, t, 1, &stress, &recover);
	g4(&r, &sp, h, 0x3, SPREAD, t += 100, 1, &stress, &recover);
	delay(h, 1, 5000000 + 310 * MSUS, t); /* jumps 300 ms in 100 ms */
	g4(&r, &sp, h, 0x3, SPREAD, t += 100, 1, &stress, &recover);
	CHECK_EQ(r.total[CG_DEC_LEAVE_STALL], 1);
	CHECK_EQ(r.total[CG_DEC_STRESS_ON], 0);
	CHECK_EQ(cg_g4_state(&sp, 1), CG_G4_LEFT);
	/* 0 loses its delay: 1, the only fresh link, comes back at once
	 * (without rejoin: with it, that is a recovery) */
	h[0].fresh_until_ms = t;
	g4(&r, &sp, h, 0x3, SPREAD, t += 100, 0, &stress, &recover);
	CHECK_EQ(sp.n_ev[1][CG_G4_REJOIN_EMPTY], 1);
	CHECK_EQ(cg_dec_get(&r, 1)->mech, CG_DEC_REJOIN_EMPTY); /* the links first, then the flags */
	CHECK_EQ(cg_dec_get(&r, 1)->from, CG_G4_LEFT);
	CHECK_EQ(cg_dec_get(&r, 1)->to, CG_G4_IN);
	CHECK_EQ(cg_dec_get(&r, 0)->mech, CG_DEC_RECOVER_ON);
}

/* G1: mute, a tried unmute, a forced one. */
static void test_decision_health(void)
{
	static struct cg_decring r;
	struct cg_hlink h[CG_MAX_LINKS];

	cg_dec_reset(&r);
	memset(h, 0, sizeof(h));
	h[1].state = CG_H_MUTED;
	h[1].have_behind = 1;
	h[1].behind_us = 45000;
	cg_decrec_health(&r, h, 0x2, 500);
	CHECK(cg_dec_get(&r, 0)->mech == CG_DEC_MUTE && cg_dec_get(&r, 0)->from == CG_H_ACTIVE &&
	      cg_dec_get(&r, 0)->to == CG_H_MUTED && cg_dec_get(&r, 0)->why == 45000);
	h[1].state = CG_H_ACTIVE;
	h[1].unmuted_ms = 900;
	h[1].behind_us = 5000;
	cg_decrec_health(&r, h, 0x2, 900);
	CHECK_EQ(cg_dec_get(&r, 0)->mech, CG_DEC_UNMUTE);
	h[2].state = CG_H_ACTIVE; /* promoted: unmuted_ms 0 */
	cg_decrec_health(&r, h, 0x4, 1000);
	CHECK_EQ(cg_dec_get(&r, 0)->mech, CG_DEC_UNMUTE_FORCED);
	CHECK_EQ(cg_dec_get(&r, 0)->why, 0); /* no delay behind */
	CHECK_EQ(cg_dec_count(&r), 3);
}

/* A link of 1000-byte packets, one report each 100 ms (capacity.h). */
struct csim {
	struct cg_cap_link l;
	struct cg_hlink h;
	uint64_t t, tx_pkts, tx_bytes;
	uint32_t rx;
	double backlog;
	int stale;
	unsigned ev[3]; /* CG_CAP_EV_* returned, counted */
	struct cg_decring r;
};

static void csim_step(struct csim *s, double send_mbit, double cap_mbit)
{
	static const struct cg_cap_cfg cfg = { .window_ms = 10000, .sat_us = 20 * MSUS };
	double sent = send_mbit * MBIT / 8 / 1000 / 10, can = cap_mbit * MBIT / 8 / 1000 / 10, out;
	uint64_t was = cg_cap_bps(&s->l);
	unsigned ev;

	s->t += 100;
	s->tx_pkts += (uint64_t)sent;
	s->tx_bytes += (uint64_t)sent * 1000;
	s->backlog += (double)(uint64_t)sent;
	out = s->backlog < can ? s->backlog : can;
	s->backlog -= (double)(uint32_t)out;
	s->rx += (uint32_t)out;
	s->h.owd = 30000u + (uint32_t)(s->backlog / can * 100 * MSUS);
	s->h.have_owd = 1;
	s->h.fresh_until_ms = s->t + 3000;
	ev = cg_cap_tick(&s->l, s->t, s->t, s->rx, s->tx_pkts, s->tx_bytes, s->stale ? NULL : &s->h, &cfg);
	for (unsigned b = 0; b < 3; b++)
		s->ev[b] += ev >> b & 1;
	cg_decrec_cap(&s->r, &s->l, 0, ev, was, s->t);
}

static void csim_run(struct csim *s, int steps, double send_mbit, double cap_mbit)
{
	for (int i = 0; i < steps; i++)
		csim_step(s, send_mbit, cap_mbit);
}

/* Capacity: a cut once per run of full samples, a cut for packets that
 * stop arriving, and fast start's restore, each counted and recorded once. */
static void test_decision_capacity(void)
{
	static struct csim s;
	const struct cg_dec *d;

	memset(&s, 0, sizeof(s));
	cg_cap_reset(&s.l);
	s.t = 1000;
	csim_run(&s, 50, 50, 100); /* 50 Mbit/s, no queue */
	CHECK_EQ(s.ev[0] + s.ev[1] + s.ev[2], 0);
	csim_run(&s, 60, 50, 10); /* the link drops to 10: full */
	CHECK_EQ(s.ev[0], 1);
	CHECK_EQ(s.l.n_cut_full, 1);
	CHECK_EQ(s.l.cut, CG_CAP_CUT_FULL);
	CHECK_EQ(s.r.total[CG_DEC_CUT_FULL], 1);
	/* the first cut came from about 50 down */
	for (unsigned k = 0; k < cg_dec_count(&s.r); k++) {
		d = cg_dec_get(&s.r, k);
		if (d->mech == CG_DEC_CUT_FULL)
			CHECK(d->from > (int64_t)(40 * MBIT) && d->to < d->from && d->why > 20000);
	}
	/* the queue is dropped, and a light load: after 10 s without being
	 * full, back to half its best, once */
	s.backlog = 0;
	csim_run(&s, 200, 2, 100);
	CHECK_EQ(s.l.cut, CG_CAP_CUT_NONE);
	CHECK_EQ(s.ev[2], 1);
	CHECK_EQ(s.l.n_restore, 1);
	CHECK_EQ(s.r.total[CG_DEC_RESTORE_REST], 1);
	CHECK(cg_cap_bps(&s.l) >= cg_cap_best_bps(&s.l) / 2);

	/* no delay reports, and a fifth of what is sent arrives: short, once
	 * (the end of the full run, its queue dropped while its packets were
	 * still short, may have counted one already) */
	{
		unsigned short0 = s.ev[1];

		s.stale = 1;
		s.backlog = 0;
		csim_run(&s, 30, 50, 10);
		CHECK_EQ(s.ev[1] - short0, 1);
		CHECK_EQ(s.l.n_cut_short, s.ev[1]);
		CHECK_EQ(s.r.total[CG_DEC_CUT_SHORT], s.ev[1]);
		CHECK_EQ(s.l.cut, CG_CAP_CUT_SHORT);
	}
	CHECK(s.l.short_n >= CG_CAP_SHORT_N);
}

/* The governor: a weight crossing half the top, down and up, once each way. */
static void test_decision_gov(void)
{
	static const struct cg_gov_cfg cfg = { .target_us = 20 * MSUS, .window_ms = 10000 };
	static struct cg_decring r;
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_gov g;
	uint16_t w0[CG_MAX_LINKS], crossed;
	uint64_t t = 1000;
	int downs = 0, ups = 0;

	CHECK_EQ(cg_gov_cross(600, 499), -1);
	CHECK_EQ(cg_gov_cross(499, 500), 1);
	CHECK_EQ(cg_gov_cross(500, 900), 0);
	CHECK_EQ(cg_gov_cross(10, 499), 0);
	cg_dec_reset(&r);
	cg_gov_reset(&g);
	CHECK_EQ(g.qmin_us, -1);
	memset(h, 0, sizeof(h));
	delay(h, 0, 50000, t);
	delay(h, 1, 50000, t);
	cg_gov_tick(&g, h, 0x3, 1, t, &cfg); /* the minima */
	delay(h, 1, 150000, t);              /* 100 ms of queue on 1 */
	for (int k = 0; k < 20; k++) {
		cg_decrec_gov_before(&g, w0);
		crossed = cg_gov_tick(&g, h, 0x3, 1, t += 100, &cfg);
		downs += crossed == 0x2;
		CHECK(!(crossed & 0x1));
		cg_decrec_gov(&r, &g, crossed, w0, t);
	}
	CHECK_EQ(downs, 1);
	CHECK_EQ(g.qmin_us, 0);
	CHECK_EQ(g.l[1].dir, -1);
	CHECK(g.l[1].ticks_down >= 20);
	CHECK(g.l[0].ticks_up + g.l[0].ticks_hold >= 20);
	CHECK_EQ(r.total[CG_DEC_GOV_DOWN], 1);
	CHECK(cg_dec_get(&r, 0)->from >= CG_GOV_HALF && cg_dec_get(&r, 0)->to < CG_GOV_HALF);
	CHECK_EQ(cg_dec_get(&r, 0)->why, 100000);
	delay(h, 1, 50000, t); /* the queue goes */
	for (int k = 0; k < 60; k++) {
		cg_decrec_gov_before(&g, w0);
		crossed = cg_gov_tick(&g, h, 0x3, 1, t += 100, &cfg);
		ups += crossed == 0x2;
		cg_decrec_gov(&r, &g, crossed, w0, t);
	}
	CHECK_EQ(ups, 1);
	CHECK_EQ(r.total[CG_DEC_GOV_UP], 1);
	CHECK_EQ(cg_dec_count(&r), 2);
}

/* The reports' freshness: their rate, and a rate that falls once they stop. */
static void test_decision_rps(void)
{
	struct cg_hlink h;
	uint64_t t = 5000;

	cg_health_reset(&h, t);
	CHECK_EQ(cg_health_rps_milli(&h, t), 0);
	for (int k = 0; k < 30; k++)
		cg_health_report(&h, 40000, t += 100, 3000);
	CHECK_EQ(h.report_ms, t);
	CHECK_EQ(cg_health_rps_milli(&h, t), 10000); /* 10 a second */
	CHECK_EQ(cg_health_rps_milli(&h, t + 500), 10000);
	/* none for 10 s: what the open window counted (9) over its length */
	CHECK_EQ(cg_health_rps_milli(&h, t + 10000), 9000000 / 10900);
}

/* The copies' budget: dry while stressed and short of it, not otherwise. */
static void test_decision_sdup_dry(void)
{
	struct cg_sdup b;

	memset(&b, 0, sizeof(b));
	CHECK(!cg_sdup(&b, 1, 1000, 4, 10)); /* 100 bytes earned, 3000 needed */
	CHECK_EQ(b.dry, 1);
	CHECK(!cg_sdup(&b, 0, 1000, 4, 10)); /* the stress ends */
	CHECK_EQ(b.dry, 0);
	b.tokens = 100000;
	CHECK(!cg_sdup(&b, 1, 1000, 4, 0)); /* off */
	CHECK_EQ(b.dry, 0);
	CHECK(cg_sdup(&b, 1, 1000, 4, 10));
	CHECK_EQ(b.dry, 0);
}

/* Which weights decide: the split by arrival only when every link in the
 * split has a rate. */
static void test_decision_weights_used(void)
{
	struct cg_eta e;

	memset(&e, 0, sizeof(e));
	e.l[0].bps = 10 * MBIT;
	e.l[1].bps = 20 * MBIT;
	CHECK(!strcmp(cg_weights_used(1, &e, 0x3, 1, 1), "eta"));
	CHECK(!strcmp(cg_weights_used(1, &e, 0x7, 1, 1), "eta")); /* 2 has no rate: it takes nothing, ETA goes on */
	CHECK(!strcmp(cg_weights_used(1, &e, 0x4, 1, 1), "capacity")); /* only 2, of no rate: the weights */
	CHECK(!strcmp(cg_weights_used(0, &e, 0x3, 0, 1), "governor"));
	CHECK(!strcmp(cg_weights_used(1, &e, 0, 0, 0), "links"));
}

void test_decision(void)
{
	test_decision_ring();
	test_decision_edge();
	test_decision_names();
	test_decision_g4();
	test_decision_g4_stall_empty();
	test_decision_health();
	test_decision_capacity();
	test_decision_gov();
	test_decision_rps();
	test_decision_sdup_dry();
	test_decision_weights_used();
}
