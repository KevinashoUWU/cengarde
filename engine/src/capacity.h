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
 * - the capacity: the largest delivery rate of the last window_ms (two
 *   half windows, like BBR's bottleneck bandwidth): a link carries at least
 *   what it once delivered. Unless the link's queue is above sat_us: then
 *   it carries what it delivers now and no more, and the window starts
 *   again from there (the smoothing keeps one bad sample from sinking it). The queue is the link's one-way delay over its own minimum of
 *   the window (govern.h's cg_gov_base).
 *
 * A link sent less than it could carry delivers what it was sent: its
 * capacity then holds for a window and decays to what it delivers. A
 * report whose rx went back, or that says more arrived than was sent (the
 * other end restarted its counters: a new session, a new path), starts the
 * link again; a link that delivers more than was sent in one interval is
 * emptying a queue, so the check is on the totals since that start.
 *
 * With capacity_weights (config.h), cg_cap_weights turns the capacities
 * into the split's weights (story 013, PR 3): each link's share as its
 * capacity, so that a link of 20 Mbit/s next to one of 200 is not given a
 * quarter of the flow. A link whose queue is below half sat_us takes 5/4
 * of its capacity: offered more than it has shown, it shows more if it can
 * (BBR's probing), and if it cannot its queue grows and its capacity is
 * what it delivers. A link not measured yet takes the largest share, so
 * that it gets measured; none goes below CG_GOV_MIN, so that a link keeps
 * a trickle and with it a measure. With governor too, its weight from the
 * queue (govern.h) scales each share.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CAPACITY_H
#define CG_CAPACITY_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "govern.h"  /* cg_gov_base, struct cg_gov_link */
#include "health.h"  /* struct cg_hlink, cg_health_fresh, cg_owd_diff */

#define CG_CAP_MIN_MS 500 /* a sample spans at least this */
#define CG_CAP_GAIN 4     /* the delivery rate's smoothing: 1/4 of each sample */
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
	uint64_t max_cur, max_prev; /* the largest rate of the current and the previous half window */
	uint64_t half_ms;           /* when the current half window started */
	struct cg_gov_link q;       /* the delay's minimum, for the queue */
	int32_t queue_us;           /* the last queue seen, -1: none */
	uint8_t have;               /* a report taken as the start */
	uint8_t have_rate;          /* a rate measured */
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

/* One tick for one link. report_ms: when this end received the other's
 * newest report on it (0: none yet), rx: what that report says; tx_pkts,
 * tx_bytes: what this end has sent on it so far; h: its health in this
 * direction (the delay, when fresh). */
static inline void cg_cap_tick(struct cg_cap_link *l, uint64_t now_ms, uint64_t report_ms, uint32_t rx, uint64_t tx_pkts,
			       uint64_t tx_bytes, const struct cg_hlink *h, const struct cg_cap_cfg *cfg)
{
	uint64_t dt, dpk, dby, bytes, rate;
	uint32_t drx, half = cfg->window_ms / 2 ? cfg->window_ms / 2 : 1;
	int sat;

	if (h && cg_health_fresh(h, now_ms)) {
		int32_t q = cg_owd_diff(h->owd, cg_gov_base(&l->q, h->owd, now_ms, cfg->window_ms));

		l->queue_us = q > 0 ? q : 0;
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
		struct cg_gov_link q = l->q;

		int32_t queue = l->queue_us;

		cg_cap_reset(l);
		l->q = q;
		l->queue_us = queue;
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
	rate = bytes * 8 * 1000 / dt;
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
	sat = l->queue_us >= 0 && (uint32_t)l->queue_us > cfg->sat_us;
	if (sat) {
		/* The link gives no more than it delivers now (smoothed), and the
		 * window starts again from there. */
		l->max_cur = l->max_prev = rate;
		l->half_ms = report_ms;
	} else if (report_ms - l->half_ms >= half) {
		l->max_prev = l->max_cur;
		l->max_cur = rate;
		l->half_ms = report_ms;
	} else if (rate > l->max_cur) {
		l->max_cur = rate;
	}
}

/* The split's weights (cg_split_pick: up to CG_GOV_MAX) from l's
 * capacities: dup0's links 0 (a copy of every packet, split.h); gov, when
 * not NULL, the governor's weights, each scaling its link's. probe_us:
 * below this queue a link takes 5/4. */
static inline void cg_cap_weights(const struct cg_cap_link l[CG_MAX_LINKS], uint16_t dup0, const uint16_t *gov,
				  uint32_t probe_us, uint16_t w[CG_MAX_LINKS])
{
	uint64_t eff[CG_MAX_LINKS], top = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t c = l[i].have_rate ? cg_cap_bps(&l[i]) : 0;

		if (c && l[i].queue_us >= 0 && (uint32_t)l[i].queue_us < probe_us)
			c += c / 4;
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

#endif
