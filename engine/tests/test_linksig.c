/* A link's observations (linksig.h, story 018): whose its queue is (the
 * delivery around the queue's onset against 2-4 s before) and what kind of
 * loss it has (random without a queue, congestive with one), with their
 * hysteresis, for the status only.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "linksig.h"
#include "test.h"

#define M 1000000ull /* bps per Mbit/s */

/* The link over [*t, until): a tick each 100 ms with queue q_us (over the
 * window's minimum) and ql_us (over the minute's), a capacity sample each
 * 500 ms delivering rx Mbit/s, losing loss_pm per mille. */
static void run(struct cg_sig_link *s, uint64_t *t, uint64_t until, int32_t q_us, int32_t ql_us, uint64_t rx,
		int loss_pm)
{
	for (; *t < until; *t += 100) {
		cg_sig_tick(s, *t, q_us, ql_us);
		if (*t % 500 == 0)
			cg_sig_sample(s, *t, rx * M, loss_pm);
	}
}

static void test_linksig_pure(void)
{
	int d = 0;

	/* 15 % down is still ours; over it, others' */
	CHECK_EQ(cg_sig_owner_of(10 * M, 85 * M / 10, &d), CG_SIG_MINE);
	CHECK_EQ(d, 15);
	CHECK_EQ(cg_sig_owner_of(10 * M, 84 * M / 10, &d), CG_SIG_OTHERS);
	CHECK_EQ(d, 16);
	CHECK_EQ(cg_sig_owner_of(10 * M, 15 * M, &d), CG_SIG_MINE); /* it rose */
	CHECK_EQ(d, -50);
	CHECK_EQ(cg_sig_owner_of(0, 5 * M, &d), CG_SIG_UNKNOWN);
	/* a loss of 5 per mille or less, or not known, or no queue measured: none */
	CHECK_EQ(cg_sig_loss_of(5, 0), CG_SIG_NONE);
	CHECK_EQ(cg_sig_loss_of(-1, 0), CG_SIG_NONE);
	CHECK_EQ(cg_sig_loss_of(20, -1), CG_SIG_NONE);
	/* within 10 ms of queue and under 10 % lost: random */
	CHECK_EQ(cg_sig_loss_of(6, 10000), CG_SIG_RANDOM);
	CHECK_EQ(cg_sig_loss_of(99, 0), CG_SIG_RANDOM);
	CHECK_EQ(cg_sig_loss_of(20, 10001), CG_SIG_CONGESTIVE);
	CHECK_EQ(cg_sig_loss_of(100, 0), CG_SIG_CONGESTIVE); /* a short buffer at a high rate */
	CHECK(!strcmp(cg_sig_owner_str(CG_SIG_OTHERS), "others"));
	CHECK(!strcmp(cg_sig_kind_str(CG_SIG_NONE), "none"));
}

/* Nothing yet: unknown and none; a queue before the link has 2-4 s of
 * delivery behind it is unknown. */
static void test_linksig_cold(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
	CHECK_EQ(s.kind, CG_SIG_NONE);
	run(&s, &t, 2000, 0, 0, 5, 0);
	run(&s, &t, 5000, 80000, 80000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
	CHECK_EQ(s.n_unknown, 1);
	CHECK_EQ(s.n_mine + s.n_others, 0);
	CHECK_EQ(s.have_drop, 0);
	CHECK_EQ(s.owner_ms, 0); /* never changed */
}

/* Our own load: the queue rises and the link keeps delivering what it
 * did (or more): mine, held while the queue lasts and 10 s after. */
static void test_linksig_mine(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000, on;

	cg_sig_reset(&s);
	run(&s, &t, 10000, 2000, 2000, 5, 0);
	on = t;
	run(&s, &t, 20000, 60000, 60000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	CHECK_EQ(s.drop_pct, 0);
	CHECK_EQ(s.n_mine, 1);
	CHECK(s.owner_ms >= on && s.owner_ms <= on + CG_SIG_NEW_MS);
	/* the queue went: held 10 s, then unknown */
	run(&s, &t, 29000, 1000, 1000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	run(&s, &t, 31000, 1000, 1000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
	CHECK_EQ(s.n_mine, 1);
	CHECK_EQ(s.drop_pct, 0); /* the last verdict's, kept */
	/* a queue that stands longer than the window: its minimum takes it in
	 * (queue_us back to 0), the minute's does not, and the verdict stays */
	run(&s, &t, 40000, 60000, 60000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	CHECK_EQ(s.n_mine, 2);
	run(&s, &t, 80000, 0, 60000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	run(&s, &t, 90100, 0, 0, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
}

/* Other users of the cell: the delivery falls (5 to 3 Mbit/s while we
 * send as before) and the queue is seen half a second later: others, 40 %
 * down. */
static void test_linksig_others(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 9500, 2000, 2000, 5, 0);
	run(&s, &t, 10000, 2000, 2000, 3, 0);
	run(&s, &t, 12000, 60000, 60000, 3, 0);
	CHECK_EQ(s.owner, CG_SIG_OTHERS);
	CHECK_EQ(s.drop_pct, 40);
	CHECK_EQ(s.n_others, 1);
	/* the others leave, the queue drains, and later our own excess
	 * makes one: a new episode, mine */
	run(&s, &t, 30000, 1000, 1000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
	run(&s, &t, 35000, 60000, 60000, 5, 0);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	CHECK_EQ(s.n_others, 1);
	CHECK_EQ(s.n_mine, 1);
}

/* A queue that wavers around the threshold is one episode: gaps under
 * 1.5 s start no new one, and the verdict stays. */
static void test_linksig_no_flap(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 10000, 2000, 2000, 5, 0);
	for (int i = 0; i < 20; i++) {
		/* 1 s over 20 ms, 1 s under, the delivery swinging */
		run(&s, &t, t + 1000, 25000, 25000, i % 2 ? 3 : 6, 0);
		run(&s, &t, t + 1000, 15000, 15000, 5, 0);
	}
	CHECK_EQ(s.n_mine + s.n_others + s.n_unknown, 1);
	CHECK_EQ(s.owner, CG_SIG_MINE);
}

/* Starlink: it loses 2 % without a queue and delivers 98 %: random, after
 * two lossy samples; back to none 10 s after the last loss. */
static void test_linksig_random(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 1100, 3000, 3000, 49, 20);
	CHECK_EQ(s.kind, CG_SIG_NONE); /* one sample is not enough */
	run(&s, &t, 1600, 3000, 3000, 49, 20);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
	CHECK_EQ(s.kind_ms, 1500);
	run(&s, &t, 10000, 3000, 3000, 49, 20);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN); /* no queue, no episode */
	CHECK_EQ(s.n_mine + s.n_others + s.n_unknown, 0);
	run(&s, &t, 19500, 3000, 3000, 50, 0);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
	run(&s, &t, 20000, 3000, 3000, 50, 0);
	CHECK_EQ(s.kind, CG_SIG_NONE);
	CHECK_EQ(s.kind_ms, 19500);
}

/* A cellular link: its deep queue overflows and drops 5 % with 400 ms of
 * queue: congestive. A short buffer that drops at a high rate without a
 * queue but delivers under 90 % is congestive too. */
static void test_linksig_congestive(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 10000, 2000, 2000, 5, 0);
	run(&s, &t, 15000, 400000, 400000, 5, 50);
	CHECK_EQ(s.kind, CG_SIG_CONGESTIVE);
	CHECK_EQ(s.owner, CG_SIG_MINE);
	cg_sig_reset(&s);
	t = 1000;
	run(&s, &t, 5000, 5000, 5000, 9, 300);
	CHECK_EQ(s.kind, CG_SIG_CONGESTIVE);
}

/* Hysteresis of the kind: single odd samples do not flip it, two in a
 * row do; a sample agreeing with the kind, or without loss, breaks a run
 * toward another, and one whose loss is not known does not. */
static void test_linksig_kind_hyst(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 3000, 3000, 3000, 49, 20);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
	for (int i = 0; i < 11; i++) {
		/* random, congestive (a burst of 15 %), random, ... */
		run(&s, &t, t + 500, 3000, 3000, i % 2 ? 42 : 49, i % 2 ? 150 : 20);
		CHECK_EQ(s.kind, CG_SIG_RANDOM);
	}
	/* a sample without loss between two congestive ones breaks the run
	 * (a clean link's odd lossy samples never add up) */
	run(&s, &t, t + 500, 3000, 3000, 42, 150);
	run(&s, &t, t + 500, 3000, 3000, 50, 0);
	run(&s, &t, t + 500, 3000, 3000, 42, 150);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
	/* one whose loss is not known does not */
	run(&s, &t, t + 500, 3000, 3000, 50, -1);
	run(&s, &t, t + 500, 3000, 3000, 42, 150);
	CHECK_EQ(s.kind, CG_SIG_CONGESTIVE);
	CHECK(s.n_random > 0 && s.n_congestive > 0);
	/* a clean link: lossy samples one apart never make it lossy */
	cg_sig_reset(&s);
	t = 1000;
	for (int i = 0; i < 20; i++)
		run(&s, &t, t + 500, 3000, 3000, 50, i % 2 ? 0 : 8);
	CHECK_EQ(s.kind, CG_SIG_NONE);
	CHECK_EQ(s.n_random, 10);
}

/* An episode whose samples stop (the link went quiet): unknown after
 * CG_SIG_GIVEUP_MS. */
static void test_linksig_giveup(void)
{
	struct cg_sig_link s;
	uint64_t t = 1000;

	cg_sig_reset(&s);
	run(&s, &t, 10000, 2000, 2000, 5, 0);
	for (; t < 10000 + CG_SIG_GIVEUP_MS + 100; t += 100)
		cg_sig_tick(&s, t, 60000, 60000);
	CHECK_EQ(s.n_unknown, 1);
	CHECK_EQ(s.owner, CG_SIG_UNKNOWN);
	CHECK_EQ(s.ep_t0, 0);
}

/* From the capacity's link (cg_sig_cap): a new sample when its report
 * time moves, its loss only when known for that sample. */
static void test_linksig_cap(void)
{
	struct cg_sig_link s;
	struct cg_cap_link l;

	cg_sig_reset(&s);
	cg_cap_reset(&l);
	cg_sig_cap(&s, &l, 1000);
	CHECK_EQ(s.t_last, 0); /* no rate: no sample */
	CHECK_EQ(s.qmax, -1);
	l.have_rate = 1;
	l.t_ms = 1000;
	l.queue_us = l.ql_us = 2000;
	l.last_rx_bps = 49 * M;
	l.loss_pm = 20;
	l.loss_pm_ms = 1000;
	cg_sig_cap(&s, &l, 1000);
	CHECK_EQ(s.t_last, 1000);
	CHECK_EQ(s.n_random, 1);
	cg_sig_cap(&s, &l, 1100); /* the same sample: nothing new */
	CHECK_EQ(s.n_random, 1);
	l.t_ms = 1600; /* a sample whose loss is not known */
	cg_sig_cap(&s, &l, 1600);
	CHECK_EQ(s.t_last, 1600);
	CHECK_EQ(s.n_random, 1);
	l.t_ms = l.loss_pm_ms = 2100;
	cg_sig_cap(&s, &l, 2100);
	CHECK_EQ(s.n_random, 2);
	CHECK_EQ(s.kind, CG_SIG_RANDOM);
}

void test_linksig(void)
{
	test_linksig_pure();
	test_linksig_cold();
	test_linksig_mine();
	test_linksig_others();
	test_linksig_no_flap();
	test_linksig_random();
	test_linksig_congestive();
	test_linksig_kind_hyst();
	test_linksig_giveup();
	test_linksig_cap();
}
