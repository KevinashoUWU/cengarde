/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "predict.h"
#include "test.h"

#define MS 1000 /* microseconds per millisecond */

static uint32_t rnd(uint32_t *seed, uint32_t n)
{
	*seed = *seed * 1103515245u + 12345u;
	return (*seed >> 8) % (n + 1);
}

/* PR #67's first split_predict, for comparison: an exponential average
 * (1/4) of the slope and of its slope, on after 4 samples over 10 ms/s,
 * kept while the slope is over 0; what it added. */
struct old_pred {
	int have, have_s1, rise_n;
	int32_t last;
	int64_t s1, s2;
};

static int32_t old_update(struct old_pred *o, int32_t q)
{
	int64_t d1, s1, add;

	if (!o->have) {
		o->have = 1;
		o->last = q;
		return 0;
	}
	d1 = ((int64_t)q - o->last) * 10; /* 100 ms apart */
	s1 = o->have_s1 ? o->s1 + (d1 - o->s1) / 4 : d1;
	if (o->have_s1)
		o->s2 += ((s1 - o->s1) * 10 - o->s2) / 4;
	o->s1 = s1;
	o->have_s1 = 1;
	o->last = q;
	if (s1 > 10000)
		o->rise_n++;
	else if (s1 <= 0 || o->rise_n < 4)
		o->rise_n = 0;
	if (o->rise_n < 4)
		return 0;
	add = s1 + o->s2 / 2;
	return add <= 0 ? 0 : add > 500000 ? 500000 : (int32_t)add;
}

/* Feed queue samples 100 ms apart from t; how many changes of what is
 * added to the queue, and when (samples from the start) it first engaged
 * (-1: never). */
struct feed {
	struct cg_pred p;
	uint64_t t;
	uint32_t seed;
};

static void feed_start(struct feed *f, uint32_t seed)
{
	memset(f, 0, sizeof(*f));
	f->t = 1000;
	f->seed = seed;
}

static void feed(struct feed *f, int32_t q)
{
	f->t += 100;
	cg_pred_update(&f->p, q < 0 ? 0 : q, f->t);
}

/* Random swings over 30 ms every 100 ms: 10 minutes, never engaged. */
static void test_predict_swings(void)
{
	for (uint32_t s = 1; s <= 5; s++) {
		struct feed f;

		feed_start(&f, s * 7919);
		for (int k = 0; k < 6000; k++)
			feed(&f, (int32_t)rnd(&f.seed, 30 * MS));
		CHECK_EQ(f.p.engaged, 0);
		CHECK(f.p.add_us == 0 && !f.p.on);
	}
}

/* The interferer: a queue growing 35 or 40 ms a second for 10 s under 20 ms
 * of swing. Engaged within ~1 s, stays so for the whole ramp, adds about
 * the slope, in at most 2 steps a second; released once the queue drains,
 * and not engaged again for CG_PRED_HOLD_MS. */
static void test_predict_ramp(void)
{
	for (int slope = 35; slope <= 40; slope += 5) {
		int at[40], worst = 0, sum = 0;

		for (uint32_t s = 0; s < 40; s++) {
			struct feed f;
			uint32_t steps;
			uint64_t rel;

			feed_start(&f, 1000 + s);
			for (int k = 0; k < 50; k++)
				feed(&f, (int32_t)rnd(&f.seed, 20 * MS));
			at[s] = -1;
			for (int k = 1; k <= 100; k++) {
				feed(&f, (int32_t)(rnd(&f.seed, 20 * MS) + (uint32_t)(slope * 100 * k)));
				if (at[s] < 0 && f.p.on)
					at[s] = k;
				if (at[s] > 0)
					CHECK(f.p.on); /* no release during the ramp */
			}
			CHECK(at[s] > 0);
			CHECK_EQ(f.p.engaged, 1);
			CHECK(f.p.add_us >= 20 * MS && f.p.add_us <= 60 * MS);
			/* 1 to engage, then at most one each CG_PRED_STEP_MS */
			CHECK(f.p.steps <= 1 + (uint32_t)(100 - at[s]) / 5);
			if (at[s] > worst)
				worst = at[s];
			sum += at[s];
			/* the load stops: the queue drains in 0.2 s */
			feed(&f, 200 * MS);
			for (int k = 0; k < 20; k++)
				feed(&f, (int32_t)rnd(&f.seed, 20 * MS));
			CHECK(!f.p.on && f.p.add_us == 0);
			rel = f.p.rel_ms;
			CHECK(rel > 0 && f.t - rel <= 1500);
			/* another ramp at once: held off until CG_PRED_HOLD_MS */
			steps = f.p.steps;
			for (int k = 1; k <= 100; k++) {
				feed(&f, (int32_t)(rnd(&f.seed, 20 * MS) + (uint32_t)(slope * 100 * k)));
				if (f.t - rel < CG_PRED_HOLD_MS)
					CHECK(!f.p.on && f.p.steps == steps);
			}
			CHECK(f.p.on && f.p.engaged == 2);
		}
		/* 40 ms/s: all within 1.5 s, 1.1 on average; 35: 2 s, 1.3 */
		if (slope == 40)
			CHECK(worst <= 15 && sum <= 40 * 11);
		else
			CHECK(worst <= 20 && sum <= 40 * 13);
	}
}

/* The neighbourhood's ramps: from under 20 ms to ~100 in 1-4 s, then down
 * in 0.5 s. A ramp of 4 s (21 ms/s) seldom engages it; one of 1-2 s (as
 * steep as the interferer's start, or more) may, once, for one step up and
 * one down, and it is released within ~1 s of the queue going down. */
static void test_predict_short(void)
{
	for (int dur = 1; dur <= 4; dur++) {
		int n = 0;

		for (uint32_t s = 0; s < 100; s++) {
			struct feed f;
			int down = -1, k;

			feed_start(&f, 5000 + s);
			for (k = 0; k < 30; k++)
				feed(&f, (int32_t)rnd(&f.seed, 20 * MS));
			for (k = 0; k < dur * 10; k++)
				feed(&f, (int32_t)(rnd(&f.seed, 20 * MS) + (uint32_t)(85 * MS * k / (dur * 10))));
			for (k = 0; k < 5; k++)
				feed(&f, (int32_t)(rnd(&f.seed, 20 * MS) + (uint32_t)(85 * MS - 17 * MS * k)));
			for (k = 0; k < 30; k++) {
				feed(&f, (int32_t)rnd(&f.seed, 20 * MS));
				if (down < 0 && !f.p.on)
					down = k;
			}
			CHECK(f.p.engaged <= 1);
			CHECK(f.p.steps <= 2 * f.p.engaged + (uint32_t)dur * 2);
			CHECK(!f.p.on && down >= 0 && down <= 12);
			n += f.p.engaged;
		}
		if (dur == 4)
			CHECK(n <= 10);
	}
}

/* A noisy oscillating queue (±25 ms over 2-6 s, plus 20 ms of swing) for 10
 * minutes: what is added changes far less often than the old one's. */
static void test_predict_flaps(void)
{
	static const int periods[] = { 20, 40, 60 };

	for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
		struct feed f;
		struct old_pred o;
		int32_t prev = 0;
		uint32_t old_changes = 0;

		feed_start(&f, 77 + i);
		memset(&o, 0, sizeof(o));
		for (int k = 0; k < 6000; k++) {
			int ph = k % periods[i], half = periods[i] / 2;
			int32_t tri = ph < half ? 50 * MS * ph / half : 50 * MS * (periods[i] - ph) / half;
			int32_t q = tri + (int32_t)rnd(&f.seed, 20 * MS), add;

			feed(&f, q);
			add = old_update(&o, q);
			/* a change of over one step, as the new one's */
			if (abs(add - prev) >= CG_PRED_Q_US) {
				old_changes++;
				prev = add;
			}
		}
		/* each engagement is held, and the short ones back off: at most
		 * one per 4 CG_PRED_HOLD_MS, and few steps each (in a run, over
		 * the 3 periods: 28, 17 and 0 engagements, 67, 34 and 0 steps,
		 * against 1236, 843 and 321 changes of the old one) */
		CHECK(f.p.engaged <= 600000 / (CG_PRED_HOLD_MS << CG_PRED_FAIL_MAX) + 2);
		CHECK(f.p.steps * 10 <= old_changes);
	}
}

static void test_predict_basics(void)
{
	struct cg_pred p;
	int64_t s = 0;

	memset(&p, 0, sizeof(p));
	CHECK(!cg_pred_slope(&p, 1000, &s));
	/* a straight line: its slope exactly */
	for (int k = 0; k < 14; k++)
		cg_pred_update(&p, 10 * MS + 50 * MS * k / 10, (uint64_t)(1000 + 100 * k));
	CHECK(cg_pred_slope(&p, 2300, &s) && s == 50 * MS);
	CHECK(p.on && p.add_us == 40 * MS); /* 50 rounded down to 20s */
	/* samples too few or too old: none */
	CHECK(!cg_pred_slope(&p, 10000, &s));
	CHECK_EQ(cg_pred_target(0), CG_PRED_Q_US);
	CHECK_EQ(cg_pred_target(59999), 40 * MS);
	CHECK_EQ(cg_pred_target(10 * 1000 * 1000), CG_PRED_MAX_US);
	/* steps at most every CG_PRED_STEP_MS, whole ones: the slope growing
	 * to 200 ms/s (the field's 40 Mbit/s load), add_us follows it */
	{
		uint32_t st = p.steps;
		uint64_t t = 2300, last = p.step_ms;
		int32_t q = 75 * MS;

		for (int k = 0; k < 30; k++) {
			t += 100;
			q += 200 * MS / 10;
			cg_pred_update(&p, q, t);
			if (p.steps != st) {
				CHECK(p.steps == st + 1 && t - last >= CG_PRED_STEP_MS);
				CHECK(p.add_us % CG_PRED_Q_US == 0);
				st = p.steps;
				last = t;
			}
		}
		CHECK(p.on && p.add_us == 200 * MS);
	}
}

/* Short engagements back off: each one shorter than CG_PRED_SHORT_MS
 * doubles the next hold, up to CG_PRED_FAIL_MAX times; a long one, or
 * CG_PRED_FORGET_MS without any, brings it back. */
static void test_predict_backoff(void)
{
	struct feed f;
	uint64_t gap[4];

	feed_start(&f, 4242);
	for (int e = 0; e < 4; e++) {
		uint64_t rel;
		int k;

		/* a 1.5 s ramp at 60 ms/s, then flat: engaged, released */
		for (k = 0; k < 15 || f.p.on; k++)
			feed(&f, (int32_t)(5 * MS + (uint32_t)(60 * 100 * (k < 15 ? k : 15))));
		rel = f.p.rel_ms;
		CHECK_EQ(f.p.engaged, (uint32_t)e + 1);
		/* ramps again and again until it engages */
		for (k = 0; !f.p.on && k < 1000; k++)
			feed(&f, (int32_t)(5 * MS + (uint32_t)(60 * 100 * (k % 20 < 15 ? k % 20 : 0))));
		gap[e] = f.t - rel;
		/* back to the start of a ramp */
		for (k = 0; f.p.on && k < 100; k++)
			feed(&f, 5 * MS);
	}
	CHECK(gap[0] >= CG_PRED_HOLD_MS * 2 && gap[0] < CG_PRED_HOLD_MS * 2 + 2000);
	CHECK(gap[1] >= CG_PRED_HOLD_MS * 4 && gap[1] < CG_PRED_HOLD_MS * 4 + 2000);
	CHECK(gap[2] >= CG_PRED_HOLD_MS * 4 && gap[2] < CG_PRED_HOLD_MS * 4 + 2000); /* the cap */
	CHECK(f.p.fail_n == CG_PRED_FAIL_MAX || f.p.on);
	/* a long quiet: the plain hold again */
	for (int k = 0; k < (int)(CG_PRED_FORGET_MS / 100) + 10; k++)
		feed(&f, 5 * MS);
	{
		uint32_t e0 = f.p.engaged;
		int k;

		for (k = 0; !f.p.on && k < 100; k++)
			feed(&f, (int32_t)(5 * MS + (uint32_t)(60 * 100 * k)));
		CHECK(f.p.engaged == e0 + 1 && k < 20);
		/* a long engagement (a 10 s load): the next hold is the plain one */
		for (int j = 0; j < 80; j++)
			feed(&f, (int32_t)(5 * MS + (uint32_t)(60 * 100 * (k + j))));
		CHECK(f.p.on);
		for (int j = 0; f.p.on && j < 100; j++)
			feed(&f, 5 * MS);
		CHECK(!f.p.on && f.p.fail_n == 0);
	}
}

void test_predict(void)
{
	test_predict_basics();
	test_predict_swings();
	test_predict_ramp();
	test_predict_short();
	test_predict_flaps();
	test_predict_backoff();
}
