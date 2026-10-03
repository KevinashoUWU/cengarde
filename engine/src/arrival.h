/* Per-link receive statistics in a redundant stream: which link delivered
 * each packet first, how far behind the others arrived, and which links
 * never delivered a copy. These are the inputs for link health decisions.
 *
 * Slots are indexed by sequence; when a slot is reused, the links that were
 * expected but never delivered the old packet are counted as missed.
 * Duplicates are recorded without MAC verification, so the numbers are
 * informational: they must never drive security decisions.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_ARRIVAL_H
#define CG_ARRIVAL_H

#include <stdint.h>

#define CG_MAX_LINKS 16
#define CG_ARR_SLOTS 4096

struct cg_arr_slot {
	uint32_t seq;
	uint32_t t_us;   /* arrival time of the first copy */
	uint16_t mask;   /* links that delivered a copy */
	uint16_t expect; /* links expected to deliver one */
	uint8_t used;
};

struct cg_link_rx {
	uint64_t wins;   /* first copies */
	uint64_t dups;   /* later copies inside the stats window */
	uint64_t late;   /* copies later than the stats window */
	uint64_t missed; /* packets this link never delivered */
	uint64_t lag8;   /* 8 x smoothed delay behind the first copy, microseconds */
};

struct cg_arrivals {
	struct cg_arr_slot slot[CG_ARR_SLOTS];
	uint64_t evaluated; /* packets whose slot was reused (denominator for missed) */
};

static inline void cg_lag_sample(struct cg_link_rx *l, uint32_t sample_us)
{
	l->lag8 = l->lag8 - l->lag8 / 8 + sample_us;
}

static inline uint32_t cg_lag_us(const struct cg_link_rx *l)
{
	return (uint32_t)(l->lag8 / 8);
}

/* First (verified) copy of seq, from link at now_us. expect: links that should
 * deliver a copy of it. */
static inline void cg_arr_first(struct cg_arrivals *a, struct cg_link_rx rx[CG_MAX_LINKS], uint32_t seq,
				uint32_t now_us, unsigned link, uint16_t expect)
{
	struct cg_arr_slot *s = &a->slot[seq % CG_ARR_SLOTS];

	if (s->used) {
		uint16_t missing = s->expect & (uint16_t)~s->mask;

		a->evaluated++;
		for (unsigned i = 0; missing; i++, missing >>= 1)
			if (missing & 1)
				rx[i].missed++;
	}
	s->seq = seq;
	s->t_us = now_us;
	s->mask = (uint16_t)(1u << link);
	s->expect = expect;
	s->used = 1;
	rx[link].wins++;
	cg_lag_sample(&rx[link], 0);
}

/* Later copy of seq from link. */
static inline void cg_arr_dup(struct cg_arrivals *a, struct cg_link_rx rx[CG_MAX_LINKS], uint32_t seq,
			      uint32_t now_us, unsigned link)
{
	struct cg_arr_slot *s = &a->slot[seq % CG_ARR_SLOTS];
	uint16_t bit = (uint16_t)(1u << link);

	if (!s->used || s->seq != seq) {
		rx[link].late++;
		return;
	}
	if (s->mask & bit)
		return; /* same link twice: a replay, not a copy */
	s->mask |= bit;
	rx[link].dups++;
	cg_lag_sample(&rx[link], now_us - s->t_us);
}

#endif
