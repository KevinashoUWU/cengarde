/* The dominant link (dominant.h, story 015, task #62): a Starlink far above
 * the slower links next to it keeps them out of the split, with hysteresis,
 * over a window of its delivery; links as fast as it stay.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "dominant.h"
#include "test.h"

#define M 1000000ull /* bps per Mbit/s */

/* Link i measured: delivering rate Mbit/s, capacity cap Mbit/s, base delay
 * base_ms (the minimum of its delays, on a shared clock). */
static void link_set(struct cg_cap_link *l, uint64_t rate, uint64_t cap, uint32_t base_ms)
{
	cg_cap_reset(l);
	l->have_rate = 1;
	l->rate_bps = rate * M;
	l->max_cur = l->max_prev = cap * M;
	l->q.have_min = 1;
	l->q.min_cur = l->q.min_prev = 1000000u + base_ms * 1000;
}

/* nostalgica: Starlink (link 0, 11 ms) at 400 against 25 + 18 (25, 27 ms) */
static void setup(struct cg_cap_link l[CG_MAX_LINKS], struct cg_link_model m[CG_MAX_LINKS])
{
	static const uint8_t type[CG_MAX_LINKS] = { CG_LTYPE_STARLINK, CG_LTYPE_CELLULAR, CG_LTYPE_CELLULAR };

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	link_set(&l[0], 400, 420, 11);
	link_set(&l[1], 20, 25, 25);
	link_set(&l[2], 15, 18, 27);
	cg_link_models(type, CG_MAX_LINKS, NULL, m);
}

static void test_dominant_basic(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	struct cg_dom d;
	uint64_t now = 100000;

	setup(l, m);
	cg_dom_reset(&d);
	CHECK_EQ(d.link, -1);
	CHECK_EQ(m[0].dominant_pct, CG_STARLINK_DOMINANT_PCT);
	CHECK_EQ(m[1].dominant_pct, 0);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	CHECK_EQ(d.link, 0);
	CHECK_EQ(d.out, 0x6);
	/* not in the mask (dead, muted): no dominant link, nothing out */
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x6, now + 100), 0);
	CHECK_EQ(d.link, -1);
	/* back: out again at once (its delivery is what it is now) */
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 200), 0x6);
	/* a slower link out of the mask is not named */
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x3, now + 300), 0x2);
	/* every link auto (no type): never */
	{
		uint8_t type[CG_MAX_LINKS] = { 0 };
		struct cg_dom d2;

		cg_link_models(type, CG_MAX_LINKS, NULL, m);
		cg_dom_reset(&d2);
		CHECK_EQ(cg_dom_tick(&d2, l, m, 0x7, now), 0);
		CHECK_EQ(d2.link, -1);
	}
}

/* The threshold: in at dominant_pct of what the slower links carry, out
 * under 4/5 of it; the delivery is its best over the window. */
static void test_dominant_hysteresis(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	struct cg_dom d;
	uint64_t now = 100000, sum = 25 + 18;

	setup(l, m);
	cg_dom_reset(&d);
	/* 2.4 times: not in */
	l[0].rate_bps = sum * M * 24 / 10;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	/* 2.5 times: in */
	l[0].rate_bps = sum * M * 25 / 10;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0x6);
	/* down to 2.1 times: its best of the window holds it, and 2.1 >= 2 anyway */
	l[0].rate_bps = sum * M * 21 / 10;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 200), 0x6);
	/* under 2 times for a whole window: out of it */
	l[0].rate_bps = sum * M * 19 / 10;
	for (uint64_t t = now + 300; t < now + CG_DOM_WIN_MS + 400; t += 100)
		cg_dom_tick(&d, l, m, 0x7, t);
	CHECK_EQ(d.out, 0);
	/* 2.2 times: not enough to come back */
	now += 2 * CG_DOM_WIN_MS;
	l[0].rate_bps = sum * M * 22 / 10;
	for (uint64_t t = now; t < now + CG_DOM_WIN_MS; t += 100)
		cg_dom_tick(&d, l, m, 0x7, t);
	CHECK_EQ(d.out, 0);
	/* a short peak of 3 times: in, and held a window after it */
	l[0].rate_bps = sum * 3 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + CG_DOM_WIN_MS), 0x6);
	l[0].rate_bps = sum * M * 15 / 10;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + CG_DOM_WIN_MS + 2000), 0x6);
	for (uint64_t t = now + CG_DOM_WIN_MS + 2100; t < now + 3 * CG_DOM_WIN_MS; t += 100)
		cg_dom_tick(&d, l, m, 0x7, t);
	CHECK_EQ(d.out, 0);
	/* the global knob (split_dominant_pct) moves the threshold */
	{
		static const uint8_t type[CG_MAX_LINKS] = { CG_LTYPE_STARLINK, CG_LTYPE_CELLULAR, CG_LTYPE_CELLULAR };
		struct cg_link_knobs k = { .dominant_pct = 400 };
		struct cg_dom d2;

		cg_link_models(type, CG_MAX_LINKS, &k, m);
		CHECK_EQ(m[0].dominant_pct, 400);
		CHECK_EQ(m[1].dominant_pct, 0); /* not on a type whose preset has none */
		cg_dom_reset(&d2);
		l[0].rate_bps = sum * 3 * M;
		CHECK_EQ(cg_dom_tick(&d2, l, m, 0x7, now), 0);
		l[0].rate_bps = sum * 4 * M;
		CHECK_EQ(cg_dom_tick(&d2, l, m, 0x7, now + 100), 0x6);
	}
}

/* Only the links slower than it by CG_DOM_BASE_US or more: as fast as it,
 * a link reorders nothing (shallowqd). One not measured yet: nothing out. */
static void test_dominant_slower(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	struct cg_dom d;
	uint64_t now = 100000;

	setup(l, m);
	cg_dom_reset(&d);
	l[1].q.min_cur = l[1].q.min_prev = l[0].q.min_cur + 1000; /* 1 ms behind */
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x4);
	/* and the threshold counts only the slower ones: 2.5 x 18 */
	l[0].rate_bps = 46 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0x4);
	cg_dom_reset(&d);
	l[0].rate_bps = 44 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 200), 0);
	/* every link as fast: nothing */
	setup(l, m);
	cg_dom_reset(&d);
	for (unsigned i = 1; i < 3; i++)
		l[i].q.min_cur = l[i].q.min_prev = l[0].q.min_cur;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	/* a link faster than the dominant one stays too */
	l[1].q.min_cur = l[1].q.min_prev = l[0].q.min_cur - 5000;
	l[2].q.min_cur = l[2].q.min_prev = l[0].q.min_cur + 14000;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0x4);
	/* a slower link not measured yet: nothing out */
	setup(l, m);
	cg_dom_reset(&d);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	l[2].have_rate = 0;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0);
	/* a slower link with no capacity at all (it delivered nothing): out */
	setup(l, m);
	cg_dom_reset(&d);
	l[1].max_cur = l[1].max_prev = 0;
	l[2].max_cur = l[2].max_prev = 0;
	l[0].rate_bps = 1 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	/* the dominant link itself not measured: none */
	setup(l, m);
	cg_dom_reset(&d);
	l[0].have_rate = 0;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	CHECK_EQ(d.link, -1);
}

/* Two links with dominant_pct: the one delivering most is the dominant one;
 * when it changes, the window starts again. */
static void test_dominant_two(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	static const uint8_t type[CG_MAX_LINKS] = { CG_LTYPE_STARLINK, CG_LTYPE_CELLULAR, CG_LTYPE_STARLINK };
	struct cg_dom d;
	uint64_t now = 100000;

	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, NULL, m);
	link_set(&l[2], 50, 60, 30);
	cg_dom_reset(&d);
	/* link 0 dominant: links 1 and 2 (slower) out, 400 >= 2.5 x 85 */
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	CHECK_EQ(d.link, 0);
	/* link 2 now delivers most (link 0 obstructed): its window, link 1 slower */
	l[0].rate_bps = 10 * M;
	l[2].rate_bps = 200 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0);
	CHECK_EQ(d.link, 2);
	CHECK_EQ(d.hi_cur, 200 * M);
}

/* hold (task #64): presumed on idle only with the evidence of its history,
 * held while it does not lose, out only when it loses under 4/5 of the
 * threshold; cold, as without hold. */
static void test_dominant_hold(void)
{
	static const uint8_t type[CG_MAX_LINKS] = { CG_LTYPE_STARLINK, CG_LTYPE_CELLULAR, CG_LTYPE_CELLULAR };
	struct cg_link_knobs k = { .dominant_hold = 1 };
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	struct cg_dom d;
	uint64_t now = 100000;

	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	CHECK(m[0].dominant_hold && !m[1].dominant_hold);
	/* cold: nothing measured, nothing out (no evidence) */
	cg_dom_reset(&d);
	for (unsigned i = 0; i < 3; i++)
		l[i].have_rate = l[i].q.have_min = 0;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	/* idle, measured at idle rates, no history: nothing out */
	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	for (unsigned i = 0; i < 3; i++) {
		l[i].rate_bps = l[i].peak_bps = l[i].last_tx_bps = 1 * M;
		l[i].max_cur = l[i].max_prev = 1 * M;
	}
	cg_dom_reset(&d);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	/* idle with a history: Starlink peaked at 400, the others' bests 25 and
	 * 18: presumed aside */
	l[0].peak_bps = 400 * M;
	l[1].best_cur = 25 * M;
	l[2].best_cur = 18 * M;
	l[1].best_ms = l[2].best_ms = now;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0x6);
	CHECK(d.presumed && d.why == CG_DOM_WHY_IDLE);
	/* a transfer starts (50 in its slow start, the others measured at 25
	 * and 18), not losing: held */
	l[0].rate_bps = l[0].last_tx_bps = 50 * M;
	l[1].max_cur = l[1].max_prev = 25 * M;
	l[2].max_cur = l[2].max_prev = 18 * M;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 600), 0x6);
	CHECK(!d.presumed && d.why == CG_DOM_WHY_HOLD);
	/* without hold the same would split */
	{
		struct cg_link_model m0[CG_MAX_LINKS];
		struct cg_dom d0;

		cg_link_models(type, CG_MAX_LINKS, NULL, m0);
		cg_dom_reset(&d0);
		CHECK_EQ(cg_dom_tick(&d0, l, m0, 0x7, now + 600), 0);
	}
	/* losing at 50 for a window */
	for (uint64_t t = now + 800; t < now + CG_DOM_WIN_MS + 900; t += 100) {
		l[0].lost_ms = t;
		cg_dom_tick(&d, l, m, 0x7, t);
	}
	/* losing at 50 against 43: out */
	CHECK_EQ(d.out, 0);
	CHECK_EQ(d.why, CG_DOM_WHY_RATE);
	/* history that does not dominate (peak 60 against 43): idle, not presumed */
	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	l[0].rate_bps = l[0].last_tx_bps = 1 * M;
	l[0].peak_bps = 60 * M;
	cg_dom_reset(&d);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0);
	/* held while not losing even when no link looks slower (the base
	 * delays not learned yet), out as soon as it loses there */
	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	l[0].last_tx_bps = 400 * M;
	cg_dom_reset(&d);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	for (unsigned i = 1; i < 3; i++)
		l[i].q.min_cur = l[i].q.min_prev = l[0].q.min_cur;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 200), 0x6);
	CHECK_EQ(d.why, CG_DOM_WHY_HOLD);
	l[0].lost_ms = now + 250;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 300), 0);
	CHECK_EQ(d.why, CG_DOM_WHY_NOSLOWER);
	/* a slower link not measured yet while out: it stays out */
	setup(l, m);
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	l[0].last_tx_bps = 400 * M;
	cg_dom_reset(&d);
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now), 0x6);
	l[2].have_rate = 0;
	l[0].lost_ms = now + 50;
	CHECK_EQ(cg_dom_tick(&d, l, m, 0x7, now + 100), 0x6);
	CHECK_EQ(d.why, CG_DOM_WHY_WAIT);
	/* the knob at 2: off over a preset that would have it */
	k.dominant_hold = 2;
	cg_link_models(type, CG_MAX_LINKS, &k, m);
	CHECK(!m[0].dominant_hold);
}

void test_dominant(void)
{
	test_dominant_hold();
	test_dominant_basic();
	test_dominant_hysteresis();
	test_dominant_slower();
	test_dominant_two();
}
