/* SPDX-License-Identifier: GPL-2.0-only */
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
	s->h.owd = 30000u + (uint32_t)(s->backlog / can * 100 * MS); /* queue: backlog over the rate */
	s->h.have_owd = 1;
	s->h.fresh_until_ms = s->t + 3000;
	cg_cap_tick(&s->l, s->t, s->t, s->rx, s->tx_pkts, s->tx_bytes, &s->h, &CFG);
}

static int near(uint64_t bps, double mbit, double tol)
{
	double v = (double)bps / MBIT;

	return v > mbit * (1 - tol) && v < mbit * (1 + tol);
}

/* A link sent below its capacity delivers what it is sent; above it, its
 * capacity; the capacity holds a window after the load drops, then decays. */
static void test_capacity_rates(void)
{
	static struct sim s;

	sim_start(&s);
	for (int i = 0; i < 20; i++)
		sim_step(&s, 30, 50);
	CHECK(s.l.have_rate && near(s.l.rate_bps, 30, 0.02) && near(cg_cap_bps(&s.l), 30, 0.02));
	CHECK(s.l.size == 1000);

	/* 10 for 3 s: it delivers 10, and still carries 30 */
	for (int i = 0; i < 30; i++)
		sim_step(&s, 10, 50);
	CHECK(near(s.l.rate_bps, 10, 0.02) && near(cg_cap_bps(&s.l), 30, 0.02));
	/* a window and more of 10: the old maximum is forgotten */
	for (int i = 0; i < 120; i++)
		sim_step(&s, 10, 50);
	CHECK(near(cg_cap_bps(&s.l), 10, 0.02));
}

/* Above the link's rate the queue grows: past sat_us the capacity is what
 * it delivers, at once, even below an older and larger maximum. */
static void test_capacity_saturated(void)
{
	static struct sim s;

	sim_start(&s);
	for (int i = 0; i < 20; i++)
		sim_step(&s, 40, 50); /* 40 delivered, no queue */
	CHECK(near(cg_cap_bps(&s.l), 40, 0.02));
	for (int i = 0; i < 20; i++)
		sim_step(&s, 40, 25); /* the link drops to 25: a queue builds */
	CHECK(s.l.queue_us > (int32_t)(20 * MS));
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* one bad half second while the queue lasts does not sink it */
	for (int i = 0; i < 5; i++)
		sim_step(&s, 40, 2);
	for (int i = 0; i < 5; i++)
		sim_step(&s, 40, 25);
	CHECK(near(cg_cap_bps(&s.l), 25, 0.05));
	/* the queue empties: more delivered than sent in an interval, and no
	 * new start for that; then the link back at 50: the capacity follows */
	for (int i = 0; i < 6; i++)
		sim_step(&s, 0, 50);
	CHECK(s.l.have_rate && s.l.rate_bps > 20 * MBIT);
	for (int i = 0; i < 34; i++)
		sim_step(&s, 0, 50);
	for (int i = 0; i < 20; i++)
		sim_step(&s, 45, 50);
	/* the queue emptied at the link's 50: that is what it carries */
	CHECK(near(cg_cap_bps(&s.l), 50, 0.05) && near(s.l.rate_bps, 45, 0.05));
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
	cg_cap_tick(&l, t + 100, t + 100, 200, 200, 200000, &h, &CFG); /* 100 ms: too short */
	CHECK(!l.have_rate);
	cg_cap_tick(&l, t + 250, t + 250, 350, 350, 350000, &h, &CFG); /* 250 packets of 1000 B in 250 ms */
	CHECK(l.have_rate && l.rate_bps == 8 * MBIT);
	/* the same report again: nothing */
	cg_cap_tick(&l, t + 600, t + 250, 350, 900, 900000, &h, &CFG);
	CHECK(l.rate_bps == 8 * MBIT && l.t_ms == t + 250);
	/* the other end's counters went back (a new session): start again */
	cg_cap_tick(&l, t + 700, t + 700, 5, 1000, 1000000, &h, &CFG);
	CHECK(l.have && !l.have_rate && l.rx == 5 && cg_cap_bps(&l) == 0);
	/* more delivered than sent: the same */
	cg_cap_tick(&l, t + 1000, t + 1000, 5000, 1100, 1100000, &h, &CFG);
	CHECK(!l.have_rate && l.rx == 5000);
	/* nothing sent: the size of before, nothing delivered, 0 */
	cg_cap_tick(&l, t + 1300, t + 1300, 5000, 1100, 1100000, &h, &CFG);
	CHECK(l.have_rate && l.rate_bps == 0);
}

void test_capacity(void)
{
	test_capacity_rates();
	test_capacity_saturated();
	test_capacity_edges();
}
