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
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
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
#define CG_CAP_SAT_N 2    /* samples in a row with a queue above sat_us: full */
#define CG_CAP_LONG_MS 60000 /* cg_cap_full's queue: over the delay's minimum of this long */
/* More delivered than sent by this much (packets), and the report is not
 * of this run of the counters: probes and what was in flight at the start
 * make a few packets of difference, never this many. */
#define CG_CAP_SLACK 64

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
	struct cg_gov_link ql;      /* the minimum over CG_CAP_LONG_MS, for cg_cap_full */
	int32_t qm_us;              /* that queue, smoothed (gain 1/8) */
	uint32_t qd_us;             /* its mean deviation (gain 1/4) */
	int32_t qx_cur, qx_prev;    /* the smoothed queue's maximum over CG_CAP_LONG_MS, in two halves */
	uint64_t qx_ms;             /* when the current half started */
	uint8_t have_q;
	int32_t queue_us;           /* the last queue seen, -1: none */
	uint8_t have;               /* a report taken as the start */
	uint8_t have_rate;          /* a rate measured */
	uint8_t short_n;            /* samples in a row that delivered under half of what was sent */
	uint8_t sat_n;              /* samples in a row with a queue above sat_us */
};

static inline void cg_cap_reset(struct cg_cap_link *l)
{
	memset(l, 0, sizeof(*l));
	l->queue_us = -1;
}

static inline uint64_t cg_cap_bps(const struct cg_cap_link *l)
{
	return l->max_cur > l->max_prev ? l->max_cur : l->max_prev;
}

/* The queue cg_cap_full reads, from a delay sample: over the minimum of
 * CG_CAP_LONG_MS (a link full for longer than window_ms measures its queue
 * against a minimum that already has the queue in it), smoothed, with its
 * mean deviation. */
static inline void cg_cap_queue_long(struct cg_cap_link *l, uint32_t owd, uint64_t now_ms)
{
	int32_t q = cg_owd_diff(owd, cg_gov_base(&l->ql, owd, now_ms, CG_CAP_LONG_MS)), e;

	if (q < 0)
		q = 0;
	if (!l->have_q) {
		l->qm_us = l->qx_cur = l->qx_prev = q;
		l->qd_us = 0;
		l->qx_ms = now_ms;
		l->have_q = 1;
		return;
	}
	e = q - l->qm_us;
	l->qm_us += e / 8;
	l->qd_us = (uint32_t)((int32_t)l->qd_us + ((e < 0 ? -e : e) - (int32_t)l->qd_us) / 4);
	if (now_ms - l->qx_ms >= CG_CAP_LONG_MS / 2) {
		l->qx_prev = l->qx_cur;
		l->qx_cur = l->qm_us;
		l->qx_ms = now_ms;
	} else if (l->qm_us > l->qx_cur) {
		l->qx_cur = l->qm_us;
	}
}

/* Whether the link is full, so that what it loses is congestion, for TCP to
 * see, and not a loss to send again (rescue.h): its queue is over target_us,
 * stays there (more than twice its swing), and is near the most it held
 * lately (3/4 of its maximum over CG_CAP_LONG_MS: the buffer is at its top,
 * where a modem drops for want of room; RED's and ECN's reading). A modem
 * whose radio loses and retransmits (HARQ) has a queue that jumps and
 * drains: on the testnet Entel on a lossy B2 was taken for full by a queue
 * over 20 ms alone. A link losing at random keeps losing at any height of
 * its queue: in the lab, with the rescue hiding that loss TCP filled the
 * queue, a rule without the maximum took it for full, and the rescue gave
 * nothing (11 Mbit/s against 50). */
static inline int cg_cap_full(const struct cg_cap_link *l, uint32_t target_us)
{
	int32_t qx = l->qx_cur > l->qx_prev ? l->qx_cur : l->qx_prev;

	return l->have_q && l->qm_us > (int32_t)target_us && (uint32_t)l->qm_us > 2 * l->qd_us &&
	       (int64_t)l->qm_us * 4 >= (int64_t)qx * 3;
}

/* One tick for one link. report_ms: when this end received the other's
 * newest report on it (0: none yet), rx: what that report says; tx_pkts,
 * tx_bytes: what this end has sent on it so far; h: its health in this
 * direction (the delay, when fresh). */
static inline void cg_cap_tick(struct cg_cap_link *l, uint64_t now_ms, uint64_t report_ms, uint32_t rx, uint64_t tx_pkts,
			       uint64_t tx_bytes, const struct cg_hlink *h, const struct cg_cap_cfg *cfg)
{
	uint64_t dt, dpk, dby, bytes, rate, raw;
	uint32_t drx;

	if (h && cg_health_fresh(h, now_ms)) {
		int32_t q = cg_owd_diff(h->owd, cg_gov_base(&l->q, h->owd, now_ms, cfg->window_ms));

		l->queue_us = q > 0 ? q : 0;
		cg_cap_queue_long(l, h->owd, now_ms);
	}
	if (!report_ms)
		return;
	if (!l->have) {
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return;
	}
	if (report_ms <= l->t_ms || report_ms - l->t_ms < CG_CAP_MIN_MS)
		return;
	dt = report_ms - l->t_ms;
	drx = rx - l->rx;
	dpk = tx_pkts - l->tx_pkts;
	dby = tx_bytes - l->tx_bytes;
	if (tx_pkts < l->tx_pkts || (int32_t)drx < 0 || l->rx_total + drx > tx_pkts - l->tx0 + CG_CAP_SLACK) {
		/* Counters from another run: start again from this report. */
		struct cg_cap_link keep = *l;

		cg_cap_reset(l);
		l->q = keep.q;
		l->queue_us = keep.queue_us;
		l->ql = keep.ql;
		l->qm_us = keep.qm_us;
		l->qd_us = keep.qd_us;
		l->qx_cur = keep.qx_cur;
		l->qx_prev = keep.qx_prev;
		l->qx_ms = keep.qx_ms;
		l->have_q = keep.have_q;
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return;
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
	if (l->queue_us >= 0 && (uint32_t)l->queue_us > cfg->sat_us) {
		/* Until the first sample full from start to end (the queue formed
		 * during the one before), each sample starts it again. */
		if (l->sat_n + 1 <= CG_CAP_SAT_N)
			l->sat_bps = raw;
		else if (raw >= l->sat_bps)
			l->sat_bps += (raw - l->sat_bps) / CG_CAP_GAIN;
		else
			l->sat_bps -= (l->sat_bps - raw) / CG_CAP_GAIN;
		l->sat_n = l->sat_n < 255 ? (uint8_t)(l->sat_n + 1) : 255;
	} else {
		l->sat_n = 0;
	}
	if (l->sat_n >= CG_CAP_SAT_N) {
		/* Full: the link gives no more than it delivers while full. */
		l->max_cur = l->max_prev = l->sat_bps;
	} else if (l->short_n >= CG_CAP_SHORT_N) {
		/* Its packets stop arriving: no more than it delivers now. */
		l->max_cur = l->max_prev = rate;
	} else if (rate > l->max_cur) {
		l->max_cur = l->max_prev = rate;
	}
}

/* The split's weights (cg_split_pick: up to CG_GOV_MAX) from l's
 * capacities: dup0's links 0 (a copy of every packet, split.h); gov, when
 * not NULL, the governor's weights, each scaling its link's. probe_us:
 * below this queue a link takes 5/4. */
static inline void cg_cap_weights(const struct cg_cap_link l[CG_MAX_LINKS], uint16_t dup0, const uint16_t *gov,
				  uint32_t probe_us, uint16_t w[CG_MAX_LINKS])
{
	uint64_t eff[CG_MAX_LINKS], top = 0, big = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if (!(dup0 >> i & 1) && l[i].have_rate && cg_cap_bps(&l[i]) > big)
			big = cg_cap_bps(&l[i]);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t c = l[i].have_rate ? cg_cap_bps(&l[i]) : 0;

		if (l[i].have_rate && l[i].queue_us >= 0 && (uint32_t)l[i].queue_us < probe_us)
			c += c / 4 + big / 16;
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
 * (story 013). */
static inline void cg_cap_eta(struct cg_eta *e, const struct cg_cap_link l[CG_MAX_LINKS], uint16_t mask,
			      uint32_t probe_us, uint64_t now_us)
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

		if (!(mask >> i & 1) || !l[i].have_rate || !l[i].q.have_min || !bps || !any) {
			cg_eta_set(e, i, 0, 0, 0, now_us);
			continue;
		}
		if (l[i].queue_us >= 0 && (uint32_t)l[i].queue_us < probe_us)
			bps += bps / 4 + big / 16;
		cg_eta_set(e, i, bps, (uint32_t)cg_owd_diff(mins[i], ref), l[i].queue_us > 0 ? (uint32_t)l[i].queue_us : 0,
			   now_us);
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
