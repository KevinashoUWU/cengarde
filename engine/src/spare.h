/* Copies and parity in a choked link's spare (story 020, redund_spare).
 *
 * In redundancy every packet goes on every link, and a link slower than
 * the traffic queues the copies it cannot carry: on the field (story 017)
 * WOM, its knee near 5 Mbit/s under a 6-9 Mbit/s upload, built 3-8 s of
 * queue, and its copies came too late or were dropped at its tail; cutting
 * the copies by their expected arrival (eta.h, 60 ms) left 40-64 % of the
 * upload on Starlink alone, which loses 0.3-1.3 % without a queue. Here a
 * link other than the first (the one where a packet arrives first, eta.h)
 * gets an allowance, a rate for what it carries besides the first link's
 * traffic, that keeps its queue at about target_us:
 *
 *   - while its queue (over the minute's minimum, capacity.h's ql_us) is
 *     over target, the allowance falls by a quarter (at most every
 *     CG_SPARE_HOLD_MS: the queue is seen a round trip late);
 *   - under half the target, with the allowance in use, it grows by a
 *     sixteenth (and CG_SPARE_STEP) a tick;
 *   - it never goes over 3/2 of the upload's data rate (a full copy of
 *     everything needs no more) nor under CG_SPARE_MIN_BPS.
 *
 * A token bucket at that rate decides each copy: a priority packet (small,
 * or TCP's ACKs and retransmissions with tcp_aware) may take it below zero
 * by up to a burst, the rest only what is there. What goes in it, by mode:
 *
 *   copies   copies of the packets that fit (partial copies);
 *   parity   no copies but the priority ones: an XOR parity (fec.h, the
 *            same CG_T_FEC of the protocol) over k packets of the first
 *            link, k from the allowance (cg_spare_k), on the link with the
 *            most room; copies while the allowance holds everything;
 *   mix      everything while the allowance holds the whole upload (full
 *            copies); copies while the first link loses over
 *            CG_SPARE_MIX_PM (a microcut: a parity over k loses two), and
 *            parity the rest of the time.
 *
 * The deadline is the target: what the bucket lets through waits at most
 * about target_us in the link's queue; the eta.h window (redund_pace_ms)
 * still applies on top when set.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SPARE_H
#define CG_SPARE_H

#include <stdint.h>

enum { CG_SPARE_OFF = 0, CG_SPARE_COPIES = 1, CG_SPARE_PARITY = 2, CG_SPARE_MIX = 3 };

#define CG_SPARE_MIN_BPS 128000u /* the allowance's floor */
#define CG_SPARE_STEP 64000u     /* its additive growth a tick, with a sixteenth */
#define CG_SPARE_HOLD_MS 500     /* between two decreases */
#define CG_SPARE_BURST_US 20000  /* the bucket's depth, in time at its rate */
#define CG_SPARE_BURST_MIN 36000 /* ... and at least three 1500-byte packets, in bits */
#define CG_SPARE_MIX_PM 50       /* mix: the first link losing over this (permil) takes copies */
#define CG_SPARE_KMIN 2
#define CG_SPARE_HOLD_CUT_MS 3000 /* the first link stays this long after a sample losing over CG_SPARE_MIX_PM */

/* One link's allowance. Zero is a link never measured. */
struct cg_spare_link {
	uint64_t rate_bps;  /* the allowance, 0: not set yet */
	int64_t tok;        /* bits in the bucket (negative: priority took ahead) */
	uint64_t t_us;      /* its last refill */
	uint64_t cut_ms;    /* the last decrease */
	uint64_t used;      /* bits taken since the last tick */
	uint64_t n_cut, n_up, n_take, n_skip;
};

/* The bucket's depth at rate_bps, in bits. */
static inline int64_t cg_spare_burst(uint64_t rate_bps)
{
	uint64_t b = rate_bps * CG_SPARE_BURST_US / 1000000;

	return (int64_t)(b < CG_SPARE_BURST_MIN ? CG_SPARE_BURST_MIN : b);
}

/* One tick (dt_ms since the last) of a link that is not the first: q_us
 * its queue over the minute's minimum (-1: not known, no change),
 * target_us the queue to hold, data_bps the upload's data rate, cap_bps
 * its capacity (0: not measured; the first allowance is half of it). */
static inline void cg_spare_tick(struct cg_spare_link *l, uint64_t now_ms, uint32_t dt_ms, int32_t q_us,
				 uint32_t target_us, uint64_t data_bps, uint64_t cap_bps)
{
	uint64_t used_bps = dt_ms ? l->used * 1000 / dt_ms : 0, top;

	l->used = 0;
	if (!l->rate_bps)
		l->rate_bps = cap_bps / 2 > CG_SPARE_MIN_BPS ? cap_bps / 2 : CG_SPARE_MIN_BPS;
	if (q_us >= 0 && (uint32_t)q_us > target_us) {
		if (!l->cut_ms || now_ms - l->cut_ms >= CG_SPARE_HOLD_MS) {
			l->rate_bps -= l->rate_bps / 4;
			l->cut_ms = now_ms;
			l->n_cut++;
		}
	} else if (q_us >= 0 && (uint32_t)q_us * 2 <= target_us && used_bps * 4 >= l->rate_bps * 3) {
		l->rate_bps += l->rate_bps / 16 + CG_SPARE_STEP;
		l->n_up++;
	}
	top = data_bps * 3 / 2;
	if (top < 4 * CG_SPARE_MIN_BPS)
		top = 4 * CG_SPARE_MIN_BPS;
	if (l->rate_bps > top)
		l->rate_bps = top;
	if (l->rate_bps < CG_SPARE_MIN_BPS)
		l->rate_bps = CG_SPARE_MIN_BPS;
}

/* Whether a packet of bits may go on the link now (and if so, taken from
 * its bucket). prio: a priority packet, which may take the bucket down to
 * minus a burst; must: one that goes anyway (a small packet), only
 * counted, the bucket held at minus a burst. */
static inline int cg_spare_take(struct cg_spare_link *l, uint64_t now_us, uint32_t bits, int prio, int must)
{
	int64_t burst;
	uint64_t dt;

	if (!l->rate_bps)
		l->rate_bps = CG_SPARE_MIN_BPS;
	burst = cg_spare_burst(l->rate_bps);
	dt = now_us > l->t_us ? now_us - l->t_us : 0;
	if (!l->t_us || dt > 1000000)
		dt = 1000000;
	l->t_us = now_us;
	l->tok += (int64_t)(l->rate_bps * dt / 1000000);
	if (l->tok > burst)
		l->tok = burst;
	if (must || l->tok >= (int64_t)bits || (prio && l->tok > -burst)) {
		l->tok -= bits;
		if (l->tok < -burst)
			l->tok = -burst;
		l->used += bits;
		l->n_take++;
		return 1;
	}
	l->n_skip++;
	return 0;
}

/* The parity's group size: the smallest k whose parity (a packet per k)
 * fits 7/8 of the allowance spare_bps against the data rate data_bps, in
 * [CG_SPARE_KMIN, kmax]; kmax when it does not fit at all (the parities
 * then go only as the bucket lets them). */
static inline unsigned cg_spare_k(uint64_t data_bps, uint64_t spare_bps, unsigned kmax)
{
	uint64_t a = spare_bps * 7 / 8, k;

	if (!a)
		return kmax;
	k = (data_bps + a - 1) / a;
	if (k < CG_SPARE_KMIN)
		k = CG_SPARE_KMIN;
	return k > kmax ? kmax : (unsigned)k;
}

/* The first link, the one the others protect: of the links of set (n at
 * most 16) whose delay without a queue is known (base_us[i], over the
 * smallest; known bit i) and that are not banned, those without a standing
 * queue (q_us[i], over the minute's minimum, at most choke_us; -1 not
 * known counts as none), and of those the one of least delay. cur (-1:
 * none) stays while it is one of them and no other is more than
 * CG_SPARE_FIRST_HYST_US below it; with none of them, cur if in set, else
 * the one of least queue. hold: cur stays whatever the rest says, even out
 * of set (a microcut on it: health may take it out for a moment). The
 * caller bans for CG_SPARE_BAN_MS a first link whose queue passes choke_us:
 * the first link carries everything, and a choked one taken for it (the
 * least delay, or the only one measured at the start) queues at once.
 *
 * Not the capacity: a link that carries only the others' spare measures
 * only that, and with parity a first link taken as the choked one at the
 * start (both measured at a few kbit/s) stayed there for good (the lab,
 * protect: 0 Mbit/s of TCP). Not each packet's expected arrival (eta.h):
 * that flipped to the choked link for a few packets a tick. */
#define CG_SPARE_FIRST_HYST_US 5000
#define CG_SPARE_BAN_MS 60000

static inline int cg_spare_first(const uint32_t *base_us, uint16_t known, const int32_t *q_us, unsigned n,
				 uint16_t set, uint16_t banned, uint32_t choke_us, int cur, int hold)
{
	uint16_t cand = 0;
	int best = -1;

	n = n > 16 ? 16 : n;
	if (hold && cur >= 0 && cur < (int)n)
		return cur;
	for (unsigned i = 0; i < n; i++)
		if ((set & known & ~banned) >> i & 1 && (q_us[i] < 0 || (uint32_t)q_us[i] <= choke_us))
			cand |= (uint16_t)(1u << i);
	for (unsigned i = 0; i < n; i++)
		if ((cand >> i & 1) && (best < 0 || base_us[i] < base_us[best]))
			best = (int)i;
	if (best < 0) {
		if (cur >= 0 && cur < (int)n && (set >> cur & 1))
			return cur;
		for (unsigned i = 0; i < n; i++)
			if ((set >> i & 1) && (best < 0 || (q_us[i] >= 0 && (q_us[best] < 0 || q_us[i] < q_us[best]))))
				best = (int)i;
		return best;
	}
	if (cur >= 0 && cur < (int)n && cur != best && (cand >> cur & 1) &&
	    base_us[cur] <= base_us[best] + CG_SPARE_FIRST_HYST_US)
		return cur;
	return best;
}

/* What a packet is protected with on the other links, by mode: 1 copies,
 * 2 parity. Copies, in every mode, while the allowance holds the whole
 * upload (spare_bps at least data_bps): they cost what a parity of k = 1
 * would, rebuild without waiting, and keep the link carrying what it
 * measures (a first link taken as the choked one, its peer carrying only
 * parity, could not measure its way back: the cell, TCP, 1 run in 3);
 * mix also while the first link loses over CG_SPARE_MIX_PM (loss_pm, its
 * last sample's loss in permil; a microcut), and parity otherwise. */
static inline int cg_spare_how(int mode, uint64_t data_bps, uint64_t spare_bps, unsigned loss_pm)
{
	if (mode == CG_SPARE_COPIES || spare_bps >= data_bps)
		return CG_SPARE_COPIES;
	if (mode == CG_SPARE_PARITY)
		return mode;
	return loss_pm > CG_SPARE_MIX_PM ? CG_SPARE_COPIES : CG_SPARE_PARITY;
}

#endif
