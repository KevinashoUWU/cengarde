/* Each link's capacity, in the direction this end sends (story 013, PR 2
 * of step 4). The other end reports in every probe (or reply) how many
 * verified packets it received on each path, rx (proto.h); this end knows
 * how many packets and bytes it sent on each. Between two reports:
 *
 * - delivered: the change in rx, times the mean size of what was sent on
 *   that link meanwhile (the peer counts packets, not bytes);
 * - the delivery rate: delivered over the time between the two reports, as
 *   this end received them, smoothed (gain 1/4). A sample spans at least
 *   CG_CAP_MIN_MS, so that a report's own delay and what is still in flight
 *   weigh little; raw samples of 250 ms swung so much that their maximum
 *   was 1.5-2 times what the link carried (story 013, the testnet);
 * - the capacity: the largest delivery rate seen: a link carries at least
 *   what it once delivered. Unless the link's queue is above sat_us for
 *   CG_CAP_SAT_N samples in a row: then it carries what it delivers while
 *   full and no more. The queue is the link's one-way delay over its own
 *   minimum of window_ms (govern.h's cg_gov_base).
 *
 * What a full link delivers is its own average, over the samples with a
 * queue only, started at the second of them (the queue formed during the
 * first, part of which the link was not full; gain 1/4 too: one bad sample
 * does not sink it). A full link is busy the whole sample, so each of those
 * samples measures it; the delivery rate above also carries the samples
 * from before the queue, when the link was sent less than it carries. Taken
 * from that, a link whose senders were still speeding up when its queue
 * formed fell to what it delivered on the way up: on the testnet, with 16
 * TCP flows down, a link of 72 Mbit/s fell to 27 as its queue passed
 * 20 ms, the split by expected arrival sent it less, and the download sank
 * to 16-70 Mbit/s against 160 with 4 flows (story 013). Two samples in a row,
 * so that one swing of a cellular link's delay over sat_us is not a full
 * link.
 *
 * A link sent less than it could carry delivers what it was sent, and that
 * says nothing about what it carries: its capacity holds. Only a queue
 * brings it down. (It used to decay to what the link delivered after a
 * window, like BBR's bottleneck bandwidth; with capacity_weights that fed
 * on itself: a link given a small share delivered little, its capacity
 * became that and its share smaller, and on the testnet two or three of
 * the four links ended at the floor, the tunnel at 40-50 % of what the
 * links gave outside it; story 013.) A
 * report whose rx went back, or that says more arrived than was sent (the
 * other end restarted its counters: a new session, a new path), starts the
 * link again; a link that delivers more than was sent in one interval is
 * emptying a queue, so the check is on the totals since that start.
 *
 * A link that delivers less than half of what it is sent for
 * CG_CAP_SHORT_N samples in a row (at least CG_CAP_SHORT_MIN packets each)
 * counts as full even when its delay says nothing: a link whose queue has
 * grown to seconds stops answering in time, its last measured queue is the
 * one from before, and its packets arrive too late for the anti-replay
 * window (which the other end does not count in rx). On the testnet Claro
 * kept its share that way while nearly nothing of it arrived in time
 * (story 013).
 *
 * With capacity_weights (config.h), cg_cap_weights turns the capacities
 * into the split's weights (story 013, PR 3): each link's share as its
 * capacity, so that a link of 20 Mbit/s next to one of 200 is not given a
 * quarter of the flow. A link whose queue is below half sat_us takes 5/4
 * of its capacity and 1/16 of the largest one's: offered more than it has
 * shown, it shows more if it can (BBR's probing), and if it cannot its
 * queue grows and its capacity is what it delivers. The added 1/16 is for
 * a link whose capacity was measured under a queue that has gone: from its
 * own 5/4 alone, a link at 0.1 Mbit/s took the floor and never delivered
 * more than that (Entel on the testnet, 11 Mbit/s outside the tunnel). A link not measured yet takes the largest share, so
 * that it gets measured; none goes below CG_GOV_MIN, so that a link keeps
 * a trickle and with it a measure. With governor too, its weight from the
 * queue (govern.h) scales each share.
 *
 * cg_cap_total_tick keeps the rate a shaper in front of the tunnel should
 * hold (story 013, PR 4), as cake-autorate does but from the engine's own
 * measures: nothing until the links with a queue above sat_us carry most
 * of what is delivered, and that is at least CG_CAP_SHAPE_MIN (no queue, no
 * need to shape; an idle link's delay swings above its minimum, and one
 * slow link's queue is for the weights: on the testnet either alone set the
 * rate to a few Mbit/s); then 9/10 of what the links deliver, so that the
 * queue forms in the shaper and not in the modems; up by 1/32 a tick
 * (about a third a second) while they deliver at least 4/5 of the rate and
 * no queue appears (the shaper may be what holds them: a rate set from what
 * they deliver behind it would only ever go down); held otherwise (an idle
 * tunnel says nothing). The engine
 * only says the rate; a service outside it applies cake.
 *
 * Three lessons of a controlled interferer (story 013, 2026-10-16: an RTMP
 * stream of 8 Mbit/s up on Entel alone while a phone loaded the same cell).
 * Only a load in the same direction hurt; the cell queued instead of
 * dropping (an upload of 20 Mbit/s grew Entel's queue 35-40 ms each second,
 * to 354-502 ms in 10 s; one of 40, to 2 s), shared the uplink about evenly,
 * and drained in under 0.2 s when the load stopped. The queue passed 50 ms
 * 0.8-2.2 s after the load started, while its slope said so at once; the
 * capacity lagged the whole phase, came back in ~20 s, and after the
 * 40 Mbit/s phase overshot to 24 Mbit/s (against ~9) for ~10 s.
 *
 * - Leaving earlier (split_predict, config.h, off by default): the queue
 *   the split sees for each link, in cg_cap_eta and in the probe gate of
 *   both splits, is where it will be in a second (cg_cap_pred_us: the
 *   queue plus its slope plus half the slope's slope, between the queue
 *   and CG_CAP_PRED_MAX_US over it), and a link whose queue will be over
 *   probe_us is not probed. Only for a slope over CG_CAP_PRED_S1 for
 *   CG_CAP_PRED_N trend samples in a row, and then while it stays above 0:
 *   a cellular delay swings by tens of ms, and one swing makes a slope of
 *   hundreds of ms a second for one sample; a rising queue keeps it. In
 *   the unit tests, a delay swinging at random over 30 ms every 100 ms
 *   turns it on under 1 % of the time, and a queue growing 40 ms a second
 *   (the field's) under 20 ms of swing within 0.6-1 s.
 * - Coming back after a short episode (cg_cap_back): the capacity from
 *   before the link was found full is kept, and once the queue has stayed
 *   under probe_us (sat_us / 2: Entel's idle queue alone was 5-6 ms, over
 *   the 5 that a stricter sat_us / 4 would ask) with a slope under
 *   CG_CAP_CALM_S1 for CG_CAP_CALM_MS, an episode (from the first sample
 *   found full until the queue went) shorter than CG_CAP_EP_MS (a
 *   contention, not a slower cell) gives it back at once,
 *   not half the best after CG_CAP_REST_MS and the probing's ~20 s. A link
 *   that really carries less fills again at once and the full logic takes
 *   it down; a restore followed by another episode within CG_CAP_RETRY_MS
 *   failed, and the next one waits twice as long each time (up to 16 times
 *   CG_CAP_RETRY_MS), so that a slower link does not bounce. The queue
 *   here is over the minimum of CG_CAP_LONG_MS (ql_us), as the trend's: a
 *   queue that stands longer than window_ms is in queue_us's minimum, and
 *   looks gone.
 * - No overshoot from a drain: a sample in which the link delivered more
 *   than it was sent by over 1/16 is emptying a queue, at whatever rate the
 *   cell gives it then; it counts for the delivery rate, but it does not
 *   raise the capacity, nor the average while full (it is held), and the
 *   capacity rises by no more than the sample itself delivered (the
 *   smoothed rate carries a drain into the samples after it). The check on
 *   the totals since the counters' start (CG_CAP_SLACK) still holds: a
 *   drain delivers what was sent before, never more than the total.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_CAPACITY_H
#define CG_CAPACITY_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "eta.h"     /* struct cg_eta */
#include "govern.h"  /* cg_gov_base, struct cg_gov_link */
#include "health.h"  /* struct cg_hlink, cg_health_fresh, cg_owd_diff */

#define CG_CAP_MIN_MS 500 /* a sample spans at least this */
#define CG_CAP_GAIN 4     /* the delivery rate's smoothing: 1/4 of each sample */
#define CG_CAP_SHORT_N 2  /* samples in a row delivering under half of what was sent: full */
#define CG_CAP_SHORT_MIN 64 /* packets sent in a sample for it to count */
#define CG_CAP_SAT_N 2    /* samples in a row with a queue above sat_us, loaded: full */
#define CG_CAP_SAT_LONG 4 /* samples in a row with a queue above sat_us, loaded or not: full */
#define CG_CAP_W_ONE 1024u /* cg_cap_rescue_w's full weight */
#define CG_CAP_BEST_MS 300000u /* fast start: a link's best capacity, over this long */
#define CG_CAP_REST_MS 10000u  /* fast start: this long without being full, it comes back to half its best */
#define CG_CAP_TREND_MS 100 /* the queue's trend takes samples at least this far apart */
#define CG_CAP_LONG_MS 60000 /* the rescue's queue: over the delay's minimum of this long */
/* More delivered than sent by this much (packets), and the report is not
 * of this run of the counters: probes and what was in flight at the start
 * make a few packets of difference, never this many. */
#define CG_CAP_SLACK 64
/* split_predict: a slope over this (us of queue per second) for this many
 * trend samples in a row, and the prediction adds up to this much */
#define CG_CAP_PRED_S1 10000
#define CG_CAP_PRED_N 4
#define CG_CAP_PRED_MAX_US 500000
/* fast return: an episode shorter than this, calm (a slope under this, us
 * per second) for this long; a restore followed by an episode within this
 * failed, and the next waits twice as long, up to 1 << CG_CAP_FAIL_MAX */
#define CG_CAP_EP_MS 30000u
#define CG_CAP_CALM_S1 5000
#define CG_CAP_CALM_MS 1000u
#define CG_CAP_RETRY_MS 5000u
#define CG_CAP_FAIL_MAX 4

struct cg_cap_cfg {
	uint32_t window_ms; /* of the capacity's maximum and of the queue's minimum */
	uint32_t sat_us;    /* a queue above this: the link gives no more */
};

struct cg_cap_link {
	uint64_t t_ms;              /* the last report sampled */
	uint64_t tx_pkts, tx_bytes; /* what this end had sent by then */
	uint32_t rx;                /* what the other end had received by then */
	uint64_t tx0;               /* tx_pkts at the start of this run of the counters */
	uint64_t rx_total;          /* received since then */
	uint32_t size;              /* mean size of what was sent, bytes */
	uint64_t rate_bps;          /* the delivery rate, smoothed */
	uint64_t sat_bps;           /* the delivery rate over the samples with a queue, smoothed */
	uint64_t max_cur, max_prev; /* the capacity: both the same (the largest rate, or the rate under a queue) */
	struct cg_gov_link q;       /* the delay's minimum, for the queue */
	int32_t queue_us;           /* the last queue seen, -1: none */
	uint8_t have;               /* a report taken as the start */
	uint8_t have_rate;          /* a rate measured */
	uint8_t short_n;            /* samples in a row that delivered under half of what was sent */
	uint8_t sat_n;              /* samples in a row with a queue above sat_us */
	uint8_t sat_load_n;         /* of them, in a row, sent at least 3/4 of its capacity */
	/* fast start: the best capacity over CG_CAP_BEST_MS (in two halves),
	 * and the last sample that found the link full */
	uint64_t best_cur, best_prev, best_ms, full_ms;
	/* The queue's trend, for the rescue's weight (cg_cap_rescue_w): its
	 * slope (us of queue per second) and the slope's slope (per second
	 * squared), smoothed (1/4), from samples at least CG_CAP_TREND_MS apart. */
	struct cg_gov_link ql;      /* the delay's minimum over CG_CAP_LONG_MS */
	int32_t ql_us;              /* the queue over it, -1: none */
	int32_t q_last;
	uint64_t q_t_ms;
	int64_t s1, s2;
	uint8_t have_trend, have_s1;
	/* For the status: whether the last sample cut the capacity (enum
	 * cg_cap_cut), and how often it began to (each run of full or short
	 * samples once) or fast start restored it. Kept across a restart of
	 * the counters. */
	uint8_t cut;
	uint64_t n_cut_full, n_cut_short, n_restore;
	uint8_t rise_n;             /* trend samples in a row with s1 over CG_CAP_PRED_S1 */
	/* fast return: when the episode started (0: none), the capacity before
	 * it, when the link turned calm (0: not), the last restore, and how
	 * many restores in a row failed */
	uint64_t ep_ms, pre_bps, calm_ms, back_ms;
	uint8_t fail_n;
};

/* What cg_cap_tick did, as bits of its return value. */
#define CG_CAP_EV_CUT_FULL 1u  /* a run of full samples began: the capacity is what it delivers full */
#define CG_CAP_EV_CUT_SHORT 2u /* a run of samples whose packets stop arriving began */
#define CG_CAP_EV_RESTORE 4u   /* fast start: back to half its best */

enum cg_cap_cut {
	CG_CAP_CUT_NONE = 0,
	CG_CAP_CUT_FULL,
	CG_CAP_CUT_SHORT,
};

static inline void cg_cap_reset(struct cg_cap_link *l)
{
	memset(l, 0, sizeof(*l));
	l->queue_us = l->ql_us = -1;
}

static inline uint64_t cg_cap_bps(const struct cg_cap_link *l)
{
	return l->max_cur > l->max_prev ? l->max_cur : l->max_prev;
}

/* The best capacity of the last CG_CAP_BEST_MS. */
static inline uint64_t cg_cap_best_bps(const struct cg_cap_link *l)
{
	return l->best_cur > l->best_prev ? l->best_cur : l->best_prev;
}

/* What the split offers a link whose queue is short, to see if it carries
 * more (BBR's probing): 5/4 of its capacity and 1/16 of the largest link's;
 * twice its capacity while it is under half its best (fast start, as BBR's
 * startup). */
static inline uint64_t cg_cap_probe(const struct cg_cap_link *l, uint64_t c, uint64_t big)
{
	return (2 * c < cg_cap_best_bps(l) ? 2 * c : c + c / 4) + big / 16;
}

/* The queue's slope and the slope's slope, from a new queue_us at now_ms. */
static inline void cg_cap_trend(struct cg_cap_link *l, uint64_t now_ms)
{
	int64_t d1, s1;
	uint64_t dt;

	if (!l->have_trend) {
		l->q_last = l->ql_us;
		l->q_t_ms = now_ms;
		l->have_trend = 1;
		return;
	}
	dt = now_ms - l->q_t_ms;
	if (dt < CG_CAP_TREND_MS)
		return;
	d1 = ((int64_t)l->ql_us - l->q_last) * 1000 / (int64_t)dt;
	s1 = l->have_s1 ? l->s1 + (d1 - l->s1) / 4 : d1;
	if (l->have_s1)
		l->s2 += ((s1 - l->s1) * 1000 / (int64_t)dt - l->s2) / 4;
	l->s1 = s1;
	l->have_s1 = 1;
	/* a rise once sustained lasts while the slope stays up at all */
	if (s1 > CG_CAP_PRED_S1)
		l->rise_n = l->rise_n < 255 ? (uint8_t)(l->rise_n + 1) : 255;
	else if (s1 <= 0 || l->rise_n < CG_CAP_PRED_N)
		l->rise_n = 0;
	l->q_last = l->ql_us;
	l->q_t_ms = now_ms;
}

/* How much of the rescue a link may have (rescue.h), 0 to CG_CAP_W_ONE,
 * from where its queue will be in a second: the queue now, plus its slope,
 * plus half its slope's slope (the second derivative says it fills before
 * the queue says so). Up to target_us, all of it; from 4 x target_us
 * (full: what it loses is congestion, for TCP to see), none; a straight
 * line between. Without a queue measured, all of it. */
static inline uint32_t cg_cap_rescue_w(const struct cg_cap_link *l, uint32_t target_us)
{
	int64_t pred, lo = target_us, hi = 4 * (int64_t)target_us;

	if (l->ql_us < 0)
		return CG_CAP_W_ONE;
	pred = (int64_t)l->ql_us + (l->have_s1 ? l->s1 + l->s2 / 2 : 0);
	if (pred <= lo)
		return CG_CAP_W_ONE;
	if (pred >= hi)
		return 0;
	return (uint32_t)((hi - pred) * CG_CAP_W_ONE / (hi - lo));
}

/* split_predict: the queue in a second (us), for a queue that has been
 * rising for CG_CAP_PRED_N trend samples; the queue now otherwise. -1: none
 * measured. */
static inline int32_t cg_cap_pred_us(const struct cg_cap_link *l)
{
	int64_t add;

	if (l->queue_us < 0 || !l->have_s1 || l->rise_n < CG_CAP_PRED_N)
		return l->queue_us;
	add = l->s1 + l->s2 / 2;
	if (add <= 0)
		return l->queue_us;
	if (add > CG_CAP_PRED_MAX_US)
		add = CG_CAP_PRED_MAX_US;
	return (int32_t)((int64_t)l->queue_us + add > INT32_MAX ? INT32_MAX : (int64_t)l->queue_us + add);
}

/* The queue the split goes by: the predicted one with predict. */
static inline int32_t cg_cap_split_q(const struct cg_cap_link *l, int predict)
{
	return predict ? cg_cap_pred_us(l) : l->queue_us;
}

/* The link found full at t_ms: its capacity is bps. The first of an
 * episode keeps the capacity from before it, for cg_cap_back; one within
 * CG_CAP_RETRY_MS of a restore says that restore failed. */
static inline void cg_cap_full(struct cg_cap_link *l, uint64_t bps, uint64_t t_ms)
{
	if (!l->ep_ms) {
		l->ep_ms = t_ms ? t_ms : 1;
		l->pre_bps = cg_cap_bps(l);
		if (l->back_ms && t_ms - l->back_ms < CG_CAP_RETRY_MS)
			l->fail_n = l->fail_n < CG_CAP_FAIL_MAX ? (uint8_t)(l->fail_n + 1) : CG_CAP_FAIL_MAX;
		else
			l->fail_n = 0;
	}
	l->max_cur = l->max_prev = bps;
	l->full_ms = t_ms;
}

/* Fast return, at each fresh delay: an episode over (the queue over the
 * long minimum under calm_us and its slope under CG_CAP_CALM_S1, since
 * after the last full sample, for CG_CAP_CALM_MS) that lasted under
 * CG_CAP_EP_MS until then gives the capacity from before it back, unless a
 * failed restore says to wait. */
static inline int cg_cap_back(struct cg_cap_link *l, uint64_t now_ms, uint32_t calm_us)
{
	int back = 0;

	if (!l->ep_ms)
		return 0;
	if (l->ql_us < 0 || (uint32_t)l->ql_us >= calm_us || (l->have_s1 && l->s1 > CG_CAP_CALM_S1)) {
		l->calm_ms = 0;
		return 0;
	}
	if (!l->calm_ms || l->calm_ms <= l->full_ms)
		l->calm_ms = now_ms;
	if (now_ms - l->calm_ms < CG_CAP_CALM_MS)
		return 0;
	if (l->calm_ms - l->ep_ms < CG_CAP_EP_MS && l->pre_bps > cg_cap_bps(l) &&
	    (!l->back_ms || now_ms - l->back_ms >= (uint64_t)CG_CAP_RETRY_MS << l->fail_n)) {
		l->max_cur = l->max_prev = l->pre_bps;
		l->back_ms = now_ms;
		back = 1;
	}
	l->ep_ms = l->calm_ms = 0;
	return back;
}

/* Fast start: the best capacity of the last CG_CAP_BEST_MS, and a link that
 * has not been full for CG_CAP_REST_MS back to half of it at least. A
 * capacity is lowered only while the link is full, and an idle link is not
 * measured again: on the testnet three of four links waited at 0.3-8 Mbit/s,
 * measured full minutes before, and a download took 3-4 s to give them their
 * 20 (story 013). If it really carries less now, its queue says so at once. */
static inline int cg_cap_best(struct cg_cap_link *l, uint64_t now_ms)
{
	uint64_t cap = cg_cap_bps(l), best;

	if (now_ms - l->best_ms >= CG_CAP_BEST_MS / 2) {
		l->best_prev = l->best_cur;
		l->best_cur = cap;
		l->best_ms = now_ms;
	} else if (cap > l->best_cur) {
		l->best_cur = cap;
	}
	best = cg_cap_best_bps(l);
	if (cap < best / 2 && (!l->full_ms || now_ms - l->full_ms >= CG_CAP_REST_MS)) {
		l->max_cur = l->max_prev = best / 2;
		l->n_restore++;
		return 1;
	}
	return 0;
}

/* One tick for one link. report_ms: when this end received the other's
 * newest report on it (0: none yet), rx: what that report says; tx_pkts,
 * tx_bytes: what this end has sent on it so far; h: its health in this
 * direction (the delay, when fresh). Returns CG_CAP_EV_* bits: what the
 * caller may record (decision.h). */
static inline unsigned cg_cap_tick(struct cg_cap_link *l, uint64_t now_ms, uint64_t report_ms, uint32_t rx, uint64_t tx_pkts,
			       uint64_t tx_bytes, const struct cg_hlink *h, const struct cg_cap_cfg *cfg)
{
	uint64_t dt, dpk, dby, bytes, rate, raw, ref;
	uint32_t drx;
	int loaded, drain;
	uint8_t cut;
	unsigned ev = 0;

	if (h && cg_health_fresh(h, now_ms)) {
		int32_t q = cg_owd_diff(h->owd, cg_gov_base(&l->q, h->owd, now_ms, cfg->window_ms));

		l->queue_us = q > 0 ? q : 0;
		/* over a minute's minimum: a link full for longer than window_ms
		 * measures queue_us against a minimum that has the queue in it */
		q = cg_owd_diff(h->owd, cg_gov_base(&l->ql, h->owd, now_ms, CG_CAP_LONG_MS));
		l->ql_us = q > 0 ? q : 0;
		cg_cap_trend(l, now_ms);
		if (cg_cap_back(l, now_ms, cfg->sat_us / 2)) {
			l->n_restore++; /* fast return: counted with fast start's */
			ev |= CG_CAP_EV_RESTORE;
		}
	}
	if (!report_ms)
		return ev;
	if (!l->have) {
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return ev;
	}
	if (report_ms <= l->t_ms || report_ms - l->t_ms < CG_CAP_MIN_MS)
		return ev;
	dt = report_ms - l->t_ms;
	drx = rx - l->rx;
	dpk = tx_pkts - l->tx_pkts;
	dby = tx_bytes - l->tx_bytes;
	if (tx_pkts < l->tx_pkts || (int32_t)drx < 0 || l->rx_total + drx > tx_pkts - l->tx0 + CG_CAP_SLACK) {
		/* Counters from another run: start again from this report; the
		 * delay's measures (queue, its trend) are the link's, and stay. */
		struct cg_cap_link keep = *l;

		cg_cap_reset(l);
		l->q = keep.q;
		l->queue_us = keep.queue_us;
		l->ql = keep.ql;
		l->ql_us = keep.ql_us;
		l->q_last = keep.q_last;
		l->q_t_ms = keep.q_t_ms;
		l->s1 = keep.s1;
		l->s2 = keep.s2;
		l->have_trend = keep.have_trend;
		l->have_s1 = keep.have_s1;
		l->n_cut_full = keep.n_cut_full;
		l->n_cut_short = keep.n_cut_short;
		l->n_restore = keep.n_restore;
		l->rise_n = keep.rise_n;
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return ev;
	}
	if (dpk)
		l->size = (uint32_t)(dby / dpk);
	bytes = (uint64_t)drx * l->size;
	rate = raw = bytes * 8 * 1000 / dt;
	l->t_ms = report_ms;
	l->rx = rx;
	l->tx_pkts = tx_pkts;
	l->tx_bytes = tx_bytes;
	l->rx_total += drx;
	/* emptying a queue: more delivered than sent, by over 1/16 */
	drain = drx > dpk + dpk / 16;
	if (l->have_rate) {
		if (rate >= l->rate_bps)
			l->rate_bps += (rate - l->rate_bps) / CG_CAP_GAIN;
		else
			l->rate_bps -= (l->rate_bps - rate) / CG_CAP_GAIN;
		rate = l->rate_bps;
	} else {
		l->rate_bps = rate;
	}
	l->have_rate = 1;
	if (dpk >= CG_CAP_SHORT_MIN && (uint64_t)drx * 2 < dpk)
		l->short_n = l->short_n < 255 ? (uint8_t)(l->short_n + 1) : 255;
	else
		l->short_n = 0;
	/* Loaded: sent at least 3/4 of its capacity (or of half its best, for a
	 * link brought down before). A queue then is the link's limit; at a
	 * light load it is a hiccup of the radio's scheduling (story 013: every
	 * cellular link stalls 100-600 ms now and then, at any load and for any
	 * protocol), and it counts only once it lasts CG_CAP_SAT_LONG samples:
	 * counted at once, Movistar's capacity fell to the 1.6 Mbit/s of an
	 * 8 Mbit/s stream's share, and its queue's blips kept it there. */
	ref = cg_cap_bps(l) > cg_cap_best_bps(l) / 2 ? cg_cap_bps(l) : cg_cap_best_bps(l) / 2;
	loaded = dby * 8 * 1000 / dt * 4 >= ref * 3;
	if (l->queue_us >= 0 && (uint32_t)l->queue_us > cfg->sat_us) {
		/* Until the first sample full from start to end (the queue formed
		 * during the one before), each sample starts it again. */
		uint64_t sent = dby * 8 * 1000 / dt;

		if (l->sat_n + 1 <= CG_CAP_SAT_N)
			l->sat_bps = drain && raw > sent ? sent : raw;
		else if (drain)
			; /* held: a drain is not what the link gives this end */
		else if (raw >= l->sat_bps)
			l->sat_bps += (raw - l->sat_bps) / CG_CAP_GAIN;
		else
			l->sat_bps -= (l->sat_bps - raw) / CG_CAP_GAIN;
		l->sat_n = l->sat_n < 255 ? (uint8_t)(l->sat_n + 1) : 255;
		l->sat_load_n = loaded ? (uint8_t)(l->sat_load_n < 255 ? l->sat_load_n + 1 : 255) : 0;
	} else {
		l->sat_n = l->sat_load_n = 0;
	}
	if (l->sat_load_n >= CG_CAP_SAT_N || l->sat_n >= CG_CAP_SAT_LONG) {
		/* Full: the link gives no more than it delivers while full. */
		cg_cap_full(l, l->sat_bps, report_ms);
		cut = CG_CAP_CUT_FULL;
	} else if (l->short_n >= CG_CAP_SHORT_N) {
		/* Its packets stop arriving: no more than it delivers now. */
		cg_cap_full(l, rate, report_ms);
		cut = CG_CAP_CUT_SHORT;
	} else {
		/* not from a drain, nor from its echo in the smoothed rate */
		if (!drain && (rate < raw ? rate : raw) > l->max_cur)
			l->max_cur = l->max_prev = rate < raw ? rate : raw;
		cut = CG_CAP_CUT_NONE;
	}
	if (cut != l->cut && cut == CG_CAP_CUT_FULL) {
		l->n_cut_full++;
		ev |= CG_CAP_EV_CUT_FULL;
	} else if (cut != l->cut && cut == CG_CAP_CUT_SHORT) {
		l->n_cut_short++;
		ev |= CG_CAP_EV_CUT_SHORT;
	}
	l->cut = cut;
	if (cg_cap_best(l, report_ms))
		ev |= CG_CAP_EV_RESTORE;
	return ev;
}

/* The split's weights (cg_split_pick: up to CG_GOV_MAX) from l's
 * capacities: dup0's links 0 (a copy of every packet, split.h); gov, when
 * not NULL, the governor's weights, each scaling its link's. probe_us:
 * below this queue a link takes 5/4; predict (split_predict): the queue in
 * a second (cg_cap_pred_us) instead of the queue now. */
static inline void cg_cap_weights(const struct cg_cap_link l[CG_MAX_LINKS], uint16_t dup0, const uint16_t *gov,
				  uint32_t probe_us, int predict, uint16_t w[CG_MAX_LINKS])
{
	uint64_t eff[CG_MAX_LINKS], top = 0, big = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if (!(dup0 >> i & 1) && l[i].have_rate && cg_cap_bps(&l[i]) > big)
			big = cg_cap_bps(&l[i]);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t c = l[i].have_rate ? cg_cap_bps(&l[i]) : 0;
		int32_t q = cg_cap_split_q(&l[i], predict);

		if (l[i].have_rate && q >= 0 && (uint32_t)q < probe_us)
			c = cg_cap_probe(&l[i], c, big);
		eff[i] = c;
		if (!(dup0 >> i & 1) && c > top)
			top = c;
	}
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t v;

		if (dup0 >> i & 1) {
			w[i] = 0;
			continue;
		}
		/* Not measured, or nothing delivered anywhere: the largest share; a
		 * link that delivered nothing while others did: the floor. */
		v = !l[i].have_rate || !top ? CG_GOV_MAX : eff[i] * CG_GOV_MAX / top;
		if (gov)
			v = v * gov[i] / CG_GOV_MAX;
		w[i] = (uint16_t)(v < CG_GOV_MIN ? CG_GOV_MIN : v > CG_GOV_MAX ? CG_GOV_MAX : v);
	}
}

#define CG_CAP_SHAPE_MIN 2000000ull /* bps: below this a tunnel is idle */

/* A tick's measures into the split by expected arrival (eta.h): each link of
 * mask's capacity, its delay without a queue (the minimum of its delays,
 * over the smallest of the links') and its queue; a link without a
 * capacity or a delay yet gets rate 0, and the split goes by weights.
 *
 * A link whose queue is under probe_us is taken at 5/4 of its capacity and
 * 1/16 of the largest one's, as cg_cap_weights does: the capacity only
 * grows when the link delivers more than it, and a split that gives it no
 * more than its capacity never lets it show more. On the testnet, after
 * MPTCP traffic had filled the same modems, the four capacities stayed at
 * 5-7 Mbit/s against 15-30 and the upload under split_eta fell to 3 Mbit/s
 * (story 013).
 *
 * With predict (split_predict), the queue, for the probe and for the
 * backlog, is where it will be in a second (cg_cap_pred_us): a link whose
 * queue is rising takes less before it is full (story 013, 2026-10-16). */
static inline void cg_cap_eta(struct cg_eta *e, const struct cg_cap_link l[CG_MAX_LINKS], uint16_t mask,
			      uint32_t probe_us, int predict, uint64_t now_us)
{
	uint32_t ref = 0, mins[CG_MAX_LINKS];
	uint64_t big = 0;
	int any = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((mask >> i & 1) && l[i].have_rate && cg_cap_bps(&l[i]) > big)
			big = cg_cap_bps(&l[i]);

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_gov_link *q = &l[i].q;

		mins[i] = cg_owd_diff(q->min_prev, q->min_cur) < 0 ? q->min_prev : q->min_cur;
		if ((mask >> i & 1) && l[i].have_rate && q->have_min && (!any || cg_owd_diff(mins[i], ref) < 0)) {
			ref = mins[i];
			any = 1;
		}
	}
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t bps = cg_cap_bps(&l[i]);
		int32_t qu = cg_cap_split_q(&l[i], predict);

		if (!(mask >> i & 1) || !l[i].have_rate || !l[i].q.have_min || !bps || !any) {
			cg_eta_set(e, i, 0, 0, 0, now_us);
			continue;
		}
		if (qu >= 0 && (uint32_t)qu < probe_us)
			bps = cg_cap_probe(&l[i], bps, big);
		cg_eta_set(e, i, bps, (uint32_t)cg_owd_diff(mins[i], ref), qu > 0 ? (uint32_t)qu : 0, now_us);
	}
}

struct cg_cap_total {
	uint64_t bps; /* the shaper's rate, 0: none yet */
};

/* One tick: mask, the links that carry (every live one); sat_us, the queue
 * that says a link is full. */
static inline void cg_cap_total_tick(struct cg_cap_total *t, const struct cg_cap_link l[CG_MAX_LINKS], uint16_t mask,
				     uint32_t sat_us)
{
	uint64_t del = 0, sat_del = 0;
	int any = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((mask >> i & 1) && l[i].have_rate) {
			del += l[i].rate_bps;
			any = 1;
			if (l[i].queue_us >= 0 && (uint32_t)l[i].queue_us > sat_us)
				sat_del += l[i].rate_bps;
		}
	if (!any)
		return;
	if (del >= CG_CAP_SHAPE_MIN && sat_del * 2 >= del) {
		uint64_t to = del / 10 * 9;

		if (to && (!t->bps || to < t->bps))
			t->bps = to;
	} else if (t->bps && del * 5 >= t->bps * 4 && sat_del * 2 < del) {
		t->bps += t->bps / 32 ? t->bps / 32 : 1;
	}
}

/* The shaper's rate, bps; 0: no shaping yet. */
static inline uint64_t cg_cap_shape_bps(const struct cg_cap_total *t)
{
	return t->bps;
}

#endif
