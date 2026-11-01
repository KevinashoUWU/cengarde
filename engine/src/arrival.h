/* Per-link receive statistics in a redundant stream: which link delivered
 * each packet first, how far behind the others arrived, and which links
 * never delivered a copy. They are for display only (see below): link
 * health (probe delays, health.h), scheduling and bonding never read them.
 *
 * Slots are indexed by sequence; when a slot is reused, the links that were
 * expected but never delivered the old packet are counted as missed.
 * Duplicates are recorded without MAC verification, so the numbers are
 * informational: they must never drive security decisions.
 *
 * Arrival times may come out of order: the router's hub takes each link
 * pump's batches in turn, with the time each pump read them, so it can take
 * a later copy before an earlier one waiting in another ring. A duplicate
 * stamped before the recorded first copy takes the first place (and its
 * win) from it. Since a copy whose MAC was never checked can move that
 * credit, these numbers are for display only: scheduling and bonding take
 * verified inputs (probe delays, first copies), never these.
 *
 * Joint lateness (story 012, "Instrumento casi gratis"): each slot also
 * keeps, for the deadlines D = 50, 100 and 150 ms after the first copy, the
 * links whose copy came within D (in[], the first copy's link in all
 * three). When the slot is reused, late = expect & ~in[D] (a link that never
 * delivered is late too) is counted in a histogram by link mask, one per D,
 * next to a histogram of expect. P(every link of S late or lost by D) is
 * the sum of the late buckets whose mask contains S over the sum of the
 * expect buckets whose mask contains S: what k-of-N would be decided on.
 * Measurement only, for the field recorder (bench/fieldrec.py); it is
 * never an input of any decision, for the same reason as above. Packets
 * left out of the histograms go to other instead (so that every packet
 * evaluated is in exactly one place):
 *   - an expected link with an id of 8 or more (the masks cover ids 0-7);
 *   - a slot reused before the longest deadline passed (CG_ARR_SLOTS
 *     packets in 150 ms or less, about 27 kpps and up): a copy still on its
 *     way would read as late. Judging by the time alone, never by what
 *     arrived, leaves no bias towards the packets that came in time.
 * Memory: in[] takes a slot from 16 to 20 bytes, and the histograms add
 * 8 KiB: 24 KiB more for each cg_arrivals (the router has one, the server
 * one per session).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_ARRIVAL_H
#define CG_ARRIVAL_H

#include <stdint.h>

#define CG_MAX_LINKS 16
#define CG_ARR_SLOTS 4096

#define CG_JOINT_D 3            /* deadlines */
#define CG_JOINT_STEP_US 50000  /* deadline d is (d + 1) x 50 ms: 50, 100 and 150 */
#define CG_JOINT_MASKS 256      /* link masks of ids 0 to 7 */

struct cg_arr_slot {
	uint32_t seq;
	uint32_t t_us;   /* arrival time of the first copy */
	uint16_t mask;   /* links that delivered a copy */
	uint16_t expect; /* links expected to deliver one */
	uint8_t used;
	uint8_t first;   /* link of the first copy */
	uint16_t in[CG_JOINT_D]; /* links whose copy came within each deadline of the first */
};

/* Display only: duplicates are counted before any MAC (see above). */
struct cg_link_rx {
	uint64_t wins;   /* first copies */
	uint64_t dups;   /* later copies inside the stats window */
	uint64_t late;   /* copies later than the stats window */
	uint64_t missed; /* packets this link never delivered */
	uint64_t lag8;   /* 8 x smoothed delay behind the first copy, microseconds */
	uint32_t last_us; /* when it last brought a copy (any), 0: never */
	uint32_t top;     /* the newest sequence it brought, any copy (with last_us; story 023: tcpfree.h's view) */
};

/* A copy of seq from link l: its newest sequence (serial max). */
static inline void cg_arr_top(struct cg_link_rx *l, uint32_t seq)
{
	if (!l->last_us || (int32_t)(seq - l->top) > 0)
		l->top = seq;
}

struct cg_arrivals {
	struct cg_arr_slot slot[CG_ARR_SLOTS];
	uint64_t evaluated; /* packets whose slot was reused (denominator for missed) */
	/* Joint lateness (above), over the evaluated packets: */
	uint64_t expect[CG_JOINT_MASKS];            /* by expect */
	uint64_t late[CG_JOINT_D][CG_JOINT_MASKS]; /* by the links late at each deadline */
	uint64_t other;                             /* in neither */
};

static inline uint32_t cg_joint_deadline_us(unsigned d)
{
	return (d + 1) * CG_JOINT_STEP_US;
}

static inline void cg_lag_sample(struct cg_link_rx *l, uint32_t sample_us)
{
	l->lag8 = l->lag8 - l->lag8 / 8 + sample_us;
}

static inline uint32_t cg_lag_us(const struct cg_link_rx *l)
{
	return (uint32_t)(l->lag8 / 8);
}

/* The joint lateness of slot s, reused by a first copy at now_us. */
static inline void cg_joint_retire(struct cg_arrivals *a, const struct cg_arr_slot *s, uint32_t now_us)
{
	uint16_t e = s->expect;

	if ((e >> 8) || now_us - s->t_us <= cg_joint_deadline_us(CG_JOINT_D - 1)) {
		a->other++;
		return;
	}
	a->expect[e]++;
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		a->late[d][e & (uint16_t)~s->in[d]]++;
}

/* First (verified) copy of seq, from link at now_us. expect: links that should
 * deliver a copy of it. */
static inline void cg_arr_first(struct cg_arrivals *a, struct cg_link_rx rx[CG_MAX_LINKS], uint32_t seq,
				uint32_t now_us, unsigned link, uint16_t expect)
{
	struct cg_arr_slot *s = &a->slot[seq % CG_ARR_SLOTS];
	uint16_t bit = (uint16_t)(1u << link);

	if (s->used) {
		uint16_t missing = s->expect & (uint16_t)~s->mask;

		a->evaluated++;
		for (unsigned i = 0; missing; i++, missing >>= 1)
			if (missing & 1)
				rx[i].missed++;
		cg_joint_retire(a, s, now_us);
	}
	s->seq = seq;
	s->t_us = now_us;
	s->mask = bit;
	s->expect = expect;
	s->used = 1;
	s->first = (uint8_t)link;
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		s->in[d] = bit;
	rx[link].wins++;
	cg_arr_top(&rx[link], seq);
	rx[link].last_us = now_us ? now_us : 1;
	cg_lag_sample(&rx[link], 0);
}

/* Later copy of seq from link. */
static inline void cg_arr_dup(struct cg_arrivals *a, struct cg_link_rx rx[CG_MAX_LINKS], uint32_t seq,
			      uint32_t now_us, unsigned link)
{
	struct cg_arr_slot *s = &a->slot[seq % CG_ARR_SLOTS];
	uint16_t bit = (uint16_t)(1u << link);
	uint32_t lag;

	cg_arr_top(&rx[link], seq);
	rx[link].last_us = now_us ? now_us : 1;
	if (!s->used || s->seq != seq) {
		rx[link].late++;
		return;
	}
	if (s->mask & bit)
		return; /* same link twice: a replay, not a copy */
	s->mask |= bit;
	if ((int32_t)(now_us - s->t_us) < 0) {
		/* Received before the copy taken first: it was the first. */
		struct cg_link_rx *was = &rx[s->first];

		was->wins--;
		was->dups++;
		cg_lag_sample(was, s->t_us - now_us);
		rx[link].wins++;
		cg_lag_sample(&rx[link], 0);
		s->first = (uint8_t)link;
		s->t_us = now_us;
		/* It joins every deadline. The copies already in keep their
		 * deadlines, measured from the old first: off by the skew between
		 * the pumps' rings, milliseconds, not worth a slot of their times. */
		for (unsigned d = 0; d < CG_JOINT_D; d++)
			s->in[d] |= bit;
		return;
	}
	lag = now_us - s->t_us;
	rx[link].dups++;
	cg_lag_sample(&rx[link], lag);
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		if (lag <= cg_joint_deadline_us(d))
			s->in[d] |= bit;
}

#endif
