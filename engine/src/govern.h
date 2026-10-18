/* The split's governor: each link's weight from its own queue (story 012,
 * step 2 of the bonding plan). With fixed weights a split gives every link
 * the same share whatever it can carry, and the weaker links queue: on the
 * bench (story 012, 2026-10-14, 4 links of 5G) two of them held 120-150 ms
 * of upload queue while the others held 55. This sets the weights instead,
 * from the one-way delay of the direction this end sends (health.h), so
 * that each link takes what it carries without a queue.
 *
 * - The queue: a link's one-way delay minus its own minimum over the last
 *   window_ms (a windowed minimum: two half windows, the current one and
 *   the one before). The delays are raw (the two clocks are not in step)
 *   but the offset is constant for a session, so the difference is the
 *   queue. A queue that stood for a whole window would become the base:
 *   the weight falls as soon as the queue grows, so it does not stand.
 * - The weight, per tick, while the end splits and the link is a candidate
 *   with a fresh delay, from its queue over the shortest queue of the
 *   candidates (its excess): down by 1/8 while the excess is above
 *   target_us (in one second at ten ticks, to a quarter), up by CG_GOV_UP
 *   while it is below half the target (from the floor to the top in five
 *   seconds), and held in between. The weights only count as a ratio: when
 *   the flow fills every link, every queue grows, and weights that all
 *   went down together would split as before (in the lab, 2026-10-14, a
 *   rule on each link's own queue sent them all to the floor at once). So
 *   the links share out the queue instead: the one with the shortest queue
 *   takes more, the ones that hold more than target_us beyond it less, and
 *   the delays even out, which is also what the reorderer needs. Saturated,
 *   the shares end up as the links' rates.
 * - Never below CG_GOV_MIN (1 % of the top): a link keeps a trickle, and
 *   with it the delay that says when its queue has gone.
 * - Out of the split (not a candidate, or no fresh delay) a weight is left
 *   as it was; a link that (re)starts begins at the top, as with fixed
 *   weights.
 * - Weight 0 keeps its meaning (a copy of every packet, split.h): the
 *   caller passes those links in dup0 and they stay 0.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_GOVERN_H
#define CG_GOVERN_H

#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "health.h"  /* struct cg_hlink, cg_owd_diff, cg_health_fresh */

#define CG_GOV_MAX 1000   /* the top weight (CG_BOND_WEIGHT_MAX) */
#define CG_GOV_MIN 10     /* the floor */
#define CG_GOV_UP 20      /* added per tick below half the target: 2 % of the top */
#define CG_GOV_SCALE 64   /* the weight is kept times this, for the 1/8 steps */
#define CG_GOV_HALF (CG_GOV_MAX / 2) /* a weight crossing this is a decision (decision.h) */

struct cg_gov_cfg {
	uint32_t target_us; /* the queue a link may hold beyond the shortest one */
	uint32_t window_ms; /* of the minimum delay */
};

struct cg_gov_link {
	uint32_t min_cur, min_prev; /* raw delay minima of the current and the previous half window */
	uint64_t half_ms;           /* when the current half window started */
	uint32_t x;                 /* the weight times CG_GOV_SCALE */
	int32_t queue_us;           /* the last queue measured (status) */
	uint8_t have_min;
	uint8_t have_queue;
	int8_t dir;                 /* the last active tick: -1 down, 0 held, 1 up (status) */
	uint64_t ticks_down, ticks_up, ticks_hold; /* active ticks of each kind (status) */
};

struct cg_gov {
	struct cg_gov_link l[CG_MAX_LINKS];
	int32_t qmin_us; /* the shortest queue of the candidates at the last tick, -1: none */
};

static inline void cg_gov_link_reset(struct cg_gov *g, unsigned i)
{
	memset(&g->l[i], 0, sizeof(g->l[i]));
	g->l[i].x = CG_GOV_MAX * CG_GOV_SCALE;
}

static inline void cg_gov_reset(struct cg_gov *g)
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_gov_link_reset(g, i);
	g->qmin_us = -1;
}

/* A weight's crossing of CG_GOV_HALF from prev to now: -1 down, 1 up, 0 none. */
static inline int cg_gov_cross(uint32_t prev, uint32_t now)
{
	if (prev >= CG_GOV_HALF && now < CG_GOV_HALF)
		return -1;
	if (prev < CG_GOV_HALF && now >= CG_GOV_HALF)
		return 1;
	return 0;
}

/* The minimum delay of link l, given a new one: the windowed minimum. */
static inline uint32_t cg_gov_base(struct cg_gov_link *l, uint32_t owd, uint64_t now_ms, uint32_t window_ms)
{
	uint32_t half = window_ms / 2 ? window_ms / 2 : 1;

	if (!l->have_min) {
		l->min_cur = l->min_prev = owd;
		l->half_ms = now_ms;
		l->have_min = 1;
	} else if (now_ms - l->half_ms >= half) {
		l->min_prev = l->min_cur;
		l->min_cur = owd;
		l->half_ms = now_ms;
	} else if (cg_owd_diff(owd, l->min_cur) < 0) {
		l->min_cur = owd;
	}
	return cg_owd_diff(l->min_prev, l->min_cur) < 0 ? l->min_prev : l->min_cur;
}

/* One tick. h: each link's send-direction health (its filtered one-way
 * delay); cand: the links in the split (eligible, weight above 0); active:
 * the end splits now (the weights only move then). Returns the links whose
 * weight crossed CG_GOV_HALF (cg_gov_cross), for the decisions. */
static inline uint16_t cg_gov_tick(struct cg_gov *g, const struct cg_hlink *h, uint16_t cand, int active, uint64_t now_ms,
			       const struct cg_gov_cfg *cfg)
{
	uint16_t fresh = 0, crossed = 0;
	int32_t qmin = INT32_MAX;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct cg_gov_link *l = &g->l[i];
		int32_t q;

		if (!(cand >> i & 1) || !cg_health_fresh(&h[i], now_ms))
			continue;
		q = cg_owd_diff(h[i].owd, cg_gov_base(l, h[i].owd, now_ms, cfg->window_ms));
		l->queue_us = q > 0 ? q : 0;
		l->have_queue = 1;
		fresh |= (uint16_t)(1u << i);
		if (l->queue_us < qmin)
			qmin = l->queue_us;
	}
	g->qmin_us = fresh ? qmin : -1;
	if (!active)
		return 0;
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct cg_gov_link *l = &g->l[i];
		uint32_t excess, was = l->x / CG_GOV_SCALE;

		if (!(fresh >> i & 1))
			continue;
		excess = (uint32_t)(l->queue_us - qmin);
		if (excess > cfg->target_us) {
			l->x -= l->x / 8;
			if (l->x < CG_GOV_MIN * CG_GOV_SCALE)
				l->x = CG_GOV_MIN * CG_GOV_SCALE;
			l->dir = -1;
			l->ticks_down++;
		} else if (excess < cfg->target_us / 2) {
			l->x += CG_GOV_UP * CG_GOV_SCALE;
			if (l->x > CG_GOV_MAX * CG_GOV_SCALE)
				l->x = CG_GOV_MAX * CG_GOV_SCALE;
			l->dir = 1;
			l->ticks_up++;
		} else {
			l->dir = 0;
			l->ticks_hold++;
		}
		if (cg_gov_cross(was, l->x / CG_GOV_SCALE))
			crossed |= (uint16_t)(1u << i);
	}
	return crossed;
}

/* The weights for cg_split_pick: each link's, 0 for the links of dup0. */
static inline void cg_gov_weights(const struct cg_gov *g, uint16_t dup0, uint16_t w[CG_MAX_LINKS])
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint32_t v = g->l[i].x / CG_GOV_SCALE;

		w[i] = (dup0 >> i & 1) ? 0 : (uint16_t)(v < CG_GOV_MIN ? CG_GOV_MIN : v);
	}
}

#endif
