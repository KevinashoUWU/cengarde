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
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_CAPACITY_H
#define CG_CAPACITY_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "eta.h"     /* struct cg_eta */
#include "govern.h"  /* cg_gov_base, struct cg_gov_link */
#include "health.h"  /* struct cg_hlink, cg_health_fresh, cg_owd_diff */
#include "linkmodel.h" /* struct cg_link_model */
#include "slot.h"      /* struct cg_slot, cg_slot_touches */

#define CG_CAP_MIN_MS 500 /* a sample spans at least this */
#define CG_CAP_GAIN 4     /* the delivery rate's smoothing: 1/4 of each sample */
#define CG_CAP_SHORT_N 2  /* samples in a row delivering under half of what was sent: full */
#define CG_CAP_LOSS_N 3   /* samples in a row, loaded, losing over loss_pct (cfg): full */
#define CG_CAP_QHI_MS 10000   /* the window of a link's highest queue (eta_buffer) */
#define CG_CAP_LOSSY_MS 5000  /* a link that lost this recently is held to its highest queue (eta_buffer) */
#define CG_CAP_QHI_MIN_US 5000 /* the least queue it is held to */
#define CG_CAP_PEAK_DECAY 512 /* peak_bps loses 1/this a sample: half in ~3 min at a sample each 0.5 s */
#define CG_CAP_TXH 16       /* ticks of sent counts a link keeps (cg_cap_tx_at) */
#define CG_CAP_LAG_MS 250   /* a sample's loss counts what was sent this long before its report */
#define CG_CAP_LOSSY_PERMIL 3  /* a sample losing more than this per mille of what was sent: lossy */
#define CG_CAP_SHORT_MIN 64 /* packets sent in a sample for it to count */
#define CG_CAP_SAT_N 2    /* samples in a row with a queue above sat_us, loaded: full */
#define CG_CAP_SAT_LONG 4 /* samples in a row with a queue above sat_us, loaded or not: full */
#define CG_CAP_W_ONE 1024u /* cg_cap_rescue_w's full weight */
#define CG_CAP_BEST_MS 300000u /* fast start: a link's best capacity, over this long */
#define CG_CAP_REST_MS 10000u  /* fast start: this long without being full, it comes back to half its best */
#define CG_CAP_TREND_MS 100 /* the queue's trend takes samples at least this far apart */
#define CG_CAP_LONG_MS 60000 /* the rescue's queue: over the delay's minimum of this long */
#define CG_CAP_STALE_MS 3000 /* a link not loaded for this long: its capacity grows for the ETA (cg_cap_stale) */
#define CG_CAP_EP_REST_MS 3000u /* cap_g4_episode 3: a G4 episode holds fast start back this long */
/* More delivered than sent by this much (packets), and the report is not
 * of this run of the counters: probes and what was in flight at the start
 * make a few packets of difference, never this many. */
#define CG_CAP_SLACK 64

struct cg_cap_cfg {
	uint32_t window_ms; /* of the capacity's maximum and of the queue's minimum */
	uint32_t sat_us;    /* a queue above this: the link gives no more */
	uint32_t loss_pct;  /* loaded and losing more than this, CG_CAP_LOSS_N samples: full; 0: off */
	int lossy;          /* track l->lossy (eta_buffer, or this link's model): a lossy link is not probed */
	int lossy_probe;    /* but with this, a lossy link is probed still (its model holds it only) */
	uint8_t rfull_mode; /* rescue_full_hyst (config.h): the rescue's full bit, cg_cap_rfull */
	/* The loss by sequence (capacity_seq_lag): a sample's loss counts what
	 * was sent before the newest DATA the report says arrived (rx_top), and
	 * on a link behind_us slower than the fastest, that much before it,
	 * instead of CG_CAP_LAG_MS before the report. */
	uint8_t seq_lag;
	uint32_t tx_seq;   /* this end's next DATA sequence now */
	uint32_t rx_top;   /* the report's newest DATA sequence received, 0: none */
	int32_t behind_us; /* the link's delay behind the fastest (health.h) */
	/* Not NULL: the link keeps Starlink's slot clock (slot.h), and a
	 * sample whose packets were sent in an edge's window counts no loss;
	 * real_off_ms, the wall clock minus now_ms. */
	const struct cg_slot *slot;
	int64_t real_off_ms;
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
	uint8_t loss_n;             /* samples in a row, loaded, that lost over cfg->loss_pct */
	/* eta_buffer: the highest queue over CG_CAP_QHI_MS (two halves), and
	 * the last sample that lost over 1 % of what was sent */
	uint32_t qhi_cur, qhi_prev;
	uint64_t qhi_t_ms, lossy_ms;
	uint64_t peak_bps;          /* the best it delivered, forgotten slowly (CG_CAP_PEAK_DECAY): the frontier's reference */
	/* what it had sent at each tick, for the loss of a sample: the peer's
	 * count against what was sent CG_CAP_LAG_MS before, not what is still
	 * in flight (cg_cap_tx_at) */
	uint64_t txh_t[CG_CAP_TXH], txh_n[CG_CAP_TXH];
	uint32_t txh_s[CG_CAP_TXH]; /* and the next DATA sequence then (capacity_seq_lag) */
	uint64_t txl_prev;
	uint64_t txl_t_prev;        /* when what txl_prev counts was sent (slot) */
	uint64_t lost_ms;           /* the last sample that lost over CG_CAP_LOSSY_PERMIL, at any rate (dominant.h) */
	uint64_t n_edge;            /* samples whose loss an edge's window took out (slot) */
	uint8_t txh_at, have_txl;
	uint8_t lossy;              /* lost over CG_CAP_LOSSY_PERMIL in the last CG_CAP_LOSSY_MS */
	uint8_t noprobe;            /* lossy, and not probed for it (!cfg->lossy_probe) */
	uint8_t sat_n;              /* samples in a row with a queue above sat_us */
	uint8_t sat_load_n;         /* of them, in a row, sent at least 3/4 of its capacity */
	/* fast start: the best capacity over CG_CAP_BEST_MS (in two halves),
	 * and the last sample that found the link full */
	uint64_t best_cur, best_prev, best_ms, full_ms;
	uint64_t loaded_ms;         /* the last sample sent at least 3/4 of its capacity, or its first */
	/* The queue's trend, for the rescue's weight (cg_cap_rescue_w): its
	 * slope (us of queue per second) and the slope's slope (per second
	 * squared), smoothed (1/4), from samples at least CG_CAP_TREND_MS apart. */
	struct cg_gov_link ql;      /* the delay's minimum over CG_CAP_LONG_MS */
	int32_t ql_us;              /* the queue over it, -1: none */
	int32_t q_last;
	uint64_t q_t_ms;
	int64_t s1, s2;
	uint8_t have_trend, have_s1;
	uint8_t rfull;              /* the rescue's full bit with hysteresis (cg_cap_rfull) */
	/* the last sample: what this end sent and what arrived, bps (cg_cap_episode) */
	uint64_t last_tx_bps, last_rx_bps;
	uint64_t ep_ms;             /* the last G4 episode counted (cap_g4_episode 3), 0: none */
	uint64_t n_g4_ep;           /* G4 leaves counted as an episode, for the status */
	/* For the status: whether the last sample cut the capacity (enum
	 * cg_cap_cut), and how often it began to (each run of full or short
	 * samples once) or fast start restored it. Kept across a restart of
	 * the counters. */
	uint8_t cut;
	uint64_t n_cut_full, n_cut_short, n_cut_loss, n_restore;
};

/* What cg_cap_tick did, as bits of its return value. */
#define CG_CAP_EV_CUT_FULL 1u  /* a run of full samples began: the capacity is what it delivers full */
#define CG_CAP_EV_CUT_SHORT 2u /* a run of samples whose packets stop arriving began */
#define CG_CAP_EV_RESTORE 4u   /* fast start: back to half its best */

enum cg_cap_cut {
	CG_CAP_CUT_NONE = 0,
	CG_CAP_CUT_FULL,
	CG_CAP_CUT_SHORT,
	CG_CAP_CUT_LOSS,
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

/* Whether a link whose queue is short may be probed (cg_cap_probe): not
 * while it is losing what it is sent (CG_CAP_CUT_LOSS). Its queue is short
 * because it drops instead of queueing: probing it raised the capacity the
 * split used to 5/4 of what it carried, and the link dropped the rest
 * (shallowq: Starlink at 75 Mbit/s for the split against 60). */
static inline int cg_cap_may_probe(const struct cg_cap_link *l, uint32_t probe_us)
{
	return l->queue_us >= 0 && (uint32_t)l->queue_us < probe_us && l->cut != CG_CAP_CUT_LOSS && !l->noprobe;
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

/* The rescue's full bit with hysteresis (rescue_full_hyst, an experiment
 * of the clockwork audit's second pass, C11): on once the queue a second
 * ahead (as cg_cap_rescue_w) reaches 4 x target_us; mode 1 keeps it on
 * until under 3 x target_us; mode 2 only while the queue is not rising
 * (its slope at most 0: a spike or a loss passing, not a queue filling),
 * and off at once under 4 x while it rises. Mode 1 helped spikeloss and
 * hurt bloat: a full link kept refusing the rescue while it filled. */
static inline void cg_cap_rfull(struct cg_cap_link *l, uint32_t target_us, unsigned mode)
{
	int64_t pred;

	if (l->ql_us < 0) {
		l->rfull = 0;
		return;
	}
	pred = (int64_t)l->ql_us + (l->have_s1 ? l->s1 + l->s2 / 2 : 0);
	if (pred >= 4 * (int64_t)target_us)
		l->rfull = 1;
	else if (pred <= 3 * (int64_t)target_us || (mode == 2 && l->have_s1 && l->s1 > 0))
		l->rfull = 0;
}

/* Whether the rescue takes link l as full (what it loses is congestion,
 * for TCP to see): its rescue weight 0, or with mode (rescue_full_hyst)
 * the bit with hysteresis. */
static inline int cg_cap_full(const struct cg_cap_link *l, uint32_t target_us, unsigned mode)
{
	return mode ? l->rfull : cg_cap_rescue_w(l, target_us) == 0;
}

/* Guard G4 took the link out because its queue grew (cap_g4_episode, an
 * experiment of the clockwork audit, C5): mode 1, as a full sample for fast
 * start; mode 2, only when the capacity overshot what the link carries: it
 * was sent at least 3/4 of its capacity in the last sample and delivered
 * under 7/8 of it (an overestimate that G4 cut short before the two full
 * samples that lower it); mode 3, as 2 but holding fast start back for
 * CG_CAP_EP_REST_MS only. Mode 1 cut het's download retransmissions by 4/5
 * and raised deepq's upload ones: it also held back links whose capacity
 * was right. Returns whether it counted. */
static inline int cg_cap_episode(struct cg_cap_link *l, uint64_t now_ms, unsigned mode)
{
	uint64_t cap = cg_cap_bps(l);

	if (!mode)
		return 0;
	if (mode >= 2 && !(l->have_rate && cap && l->last_tx_bps * 4 >= cap * 3 && l->last_rx_bps * 8 < cap * 7))
		return 0;
	if (mode == 3)
		l->ep_ms = now_ms ? now_ms : 1;
	else
		l->full_ms = now_ms;
	l->n_g4_ep++;
	return 1;
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
	if (cap < best / 2 && (!l->full_ms || now_ms - l->full_ms >= CG_CAP_REST_MS) &&
	    (!l->ep_ms || now_ms - l->ep_ms >= CG_CAP_EP_REST_MS)) {
		l->max_cur = l->max_prev = best / 2;
		l->n_restore++;
		return 1;
	}
	return 0;
}

/* What link l had sent by t_ms (the newest tick at or before it), 0 with
 * *ok 0 when its ticks do not go back that far. */
static inline uint64_t cg_cap_tx_at(const struct cg_cap_link *l, uint64_t t_ms, int *ok)
{
	uint64_t best_t = 0, n = 0;

	*ok = 0;
	for (unsigned k = 0; k < CG_CAP_TXH; k++)
		if (l->txh_t[k] && l->txh_t[k] <= t_ms && l->txh_t[k] >= best_t) {
			best_t = l->txh_t[k];
			n = l->txh_n[k];
			*ok = 1;
		}
	return n;
}

/* What link l had sent by t_ms, between the ticks around it in proportion
 * to the time (cg_cap_tx_at the newest at or before it otherwise); *ok 0
 * when its ticks do not go back that far. */
static inline uint64_t cg_cap_tx_ati(const struct cg_cap_link *l, uint64_t t_ms, int *ok)
{
	uint64_t n = cg_cap_tx_at(l, t_ms, ok), bt = 0, at = 0, an = 0;

	if (!*ok)
		return 0;
	for (unsigned k = 0; k < CG_CAP_TXH; k++)
		if (l->txh_t[k] && l->txh_t[k] <= t_ms && l->txh_t[k] >= bt)
			bt = l->txh_t[k];
	for (unsigned k = 0; k < CG_CAP_TXH; k++)
		if (l->txh_t[k] > t_ms && (!at || l->txh_t[k] < at)) {
			at = l->txh_t[k];
			an = l->txh_n[k];
		}
	if (!at || an < n)
		return n;
	return n + (an - n) * (t_ms - bt) / (at - bt);
}

/* When (now_ms's clock) this end sent DATA sequence seq: between the ticks
 * around it in proportion to the sequence; *ok 0 when its ticks do not go
 * back that far. Sent after the newest tick: that tick. */
static inline uint64_t cg_cap_seq_t(const struct cg_cap_link *l, uint32_t seq, int *ok)
{
	uint64_t bt = 0, at = 0;
	uint32_t bs = 0, as = 0;

	*ok = 0;
	/* the newest tick that had not sent it yet (its next sequence <= seq) */
	for (unsigned k = 0; k < CG_CAP_TXH; k++)
		if (l->txh_t[k] && (int32_t)(seq - l->txh_s[k]) >= 0 && l->txh_t[k] >= bt) {
			bt = l->txh_t[k];
			bs = l->txh_s[k];
		}
	if (!bt)
		return 0;
	*ok = 1;
	/* the oldest that had */
	for (unsigned k = 0; k < CG_CAP_TXH; k++)
		if (l->txh_t[k] > bt && (int32_t)(seq - l->txh_s[k]) < 0 && (!at || l->txh_t[k] < at)) {
			at = l->txh_t[k];
			as = l->txh_s[k];
		}
	if (!at)
		return bt;
	return bt + (at - bt) * (uint64_t)(seq - bs + 1) / (uint32_t)(as - bs);
}

/* One tick for one link. report_ms: when this end received the other's
 * newest report on it (0: none yet), rx: what that report says; tx_pkts,
 * tx_bytes: what this end has sent on it so far; h: its health in this
 * direction (the delay, when fresh). Returns CG_CAP_EV_* bits: what the
 * caller may record (decision.h). */
static inline unsigned cg_cap_tick(struct cg_cap_link *l, uint64_t now_ms, uint64_t report_ms, uint32_t rx, uint64_t tx_pkts,
			       uint64_t tx_bytes, const struct cg_hlink *h, const struct cg_cap_cfg *cfg)
{
	uint64_t dt, dpk, dby, bytes, rate, raw, ref, dlost;
	uint32_t drx;
	int loaded;
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
		if (now_ms - l->qhi_t_ms >= CG_CAP_QHI_MS / 2) {
			l->qhi_prev = l->qhi_cur;
			l->qhi_cur = (uint32_t)l->queue_us;
			l->qhi_t_ms = now_ms;
		} else if ((uint32_t)l->queue_us > l->qhi_cur) {
			l->qhi_cur = (uint32_t)l->queue_us;
		}
		cg_cap_rfull(l, cfg->sat_us, cfg->rfull_mode);
	}
	{
		unsigned last = (l->txh_at + CG_CAP_TXH - 1) % CG_CAP_TXH;

		if (!l->txh_t[last] || now_ms >= l->txh_t[last] + 40) {
			l->txh_t[l->txh_at] = now_ms;
			l->txh_n[l->txh_at] = tx_pkts;
			l->txh_s[l->txh_at] = cfg->tx_seq;
			l->txh_at = (uint8_t)((l->txh_at + 1) % CG_CAP_TXH);
		}
	}
	if (!report_ms)
		return 0;
	if (!l->have) {
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return 0;
	}
	if (report_ms <= l->t_ms || report_ms - l->t_ms < CG_CAP_MIN_MS)
		return 0;
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
		l->peak_bps = keep.peak_bps;
		l->lost_ms = keep.lost_ms;
		l->n_edge = keep.n_edge;
		l->n_cut_loss = keep.n_cut_loss;
		l->n_restore = keep.n_restore;
		l->n_g4_ep = keep.n_g4_ep;
		l->rfull = keep.rfull;
		l->t_ms = report_ms;
		l->rx = rx;
		l->tx_pkts = l->tx0 = tx_pkts;
		l->tx_bytes = tx_bytes;
		l->have = 1;
		return 0;
	}
	if (dpk)
		l->size = (uint32_t)(dby / dpk);
	bytes = (uint64_t)drx * l->size;
	rate = raw = bytes * 8 * 1000 / dt;
	l->last_rx_bps = raw;
	l->last_tx_bps = dby * 8 * 1000 / dt;
	l->t_ms = report_ms;
	l->rx = rx;
	l->tx_pkts = tx_pkts;
	l->tx_bytes = tx_bytes;
	l->rx_total += drx;
	/* Loaded: sent at least 3/4 of its capacity (or of half its best, for a
	 * link brought down before); see below. */
	ref = cg_cap_bps(l) > cg_cap_best_bps(l) / 2 ? cg_cap_bps(l) : cg_cap_best_bps(l) / 2;
	loaded = dby * 8 * 1000 / dt * 4 >= ref * 3;
	/* The first sample loaded after CG_CAP_STALE_MS without one (a tickle,
	 * tickle.h) starts the smoothing again from itself: the smoothed rate
	 * is then what the link was sent while idle, and a 600 ms tickle would
	 * move it by a quarter, never over its capacity. */
	if (loaded && l->have_rate && report_ms - l->loaded_ms >= CG_CAP_STALE_MS)
		l->have_rate = 0;
	if (loaded)
		l->loaded_ms = report_ms;
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
	/* the frontier's reference: a capacity that fell while the link was
	 * held unprobed must not become its frontier (nostalgica, 2026-10-19:
	 * Starlink's upload kept at 8 Mbit/s against 34-40, its 30 s best
	 * falling with it) */
	l->peak_bps -= l->peak_bps / CG_CAP_PEAK_DECAY;
	if (rate > l->peak_bps)
		l->peak_bps = rate;
	if (!l->loaded_ms)
		l->loaded_ms = report_ms; /* its age counts from its first sample */
	if (dpk >= CG_CAP_SHORT_MIN && (uint64_t)drx * 2 < dpk)
		l->short_n = l->short_n < 255 ? (uint8_t)(l->short_n + 1) : 255;
	else
		l->short_n = 0;
	/* Loaded (above). A queue then is the link's limit; at a
	 * light load it is a hiccup of the radio's scheduling (story 013: every
	 * cellular link stalls 100-600 ms now and then, at any load and for any
	 * protocol), and it counts only once it lasts CG_CAP_SAT_LONG samples:
	 * counted at once, Movistar's capacity fell to the 1.6 Mbit/s of an
	 * 8 Mbit/s stream's share, and its queue's blips kept it there. */
	/* lost over loss_pct of what was sent in this sample (delivered drx
	 * of dpk), while loaded: at a light load a loss is the radio's, not a
	 * full link (lossy, spikeloss) */
	/* lossy: a loss at its frontier, near the best it carried; a link that
	 * loses at any rate (a misaligned dish, nostalgica 2026-10-19: Starlink's
	 * upload marked lossy for good, its probing stopped and its capacity down
	 * to 3 Mbit/s against 30) is not held for it */
	/* what was sent and could have arrived: CG_CAP_LAG_MS before each
	 * report; a sample's own sent count has what is still in flight, 1-5 %
	 * at 30 Mbit/s and 25 ms (nostalgica 2026-10-19: Starlink's clean upload,
	 * 0 % lost by UDP up to 38 Mbit/s, marked lossy all the time) */
	{
		int ok = 0, edge = 0;
		uint64_t txl = 0, xt = 0;

		/* by sequence: what was sent before the newest packet that arrived
		 * (by the time the report left), on this link that much earlier as
		 * it is slower; a sample's own sent count has what is still in
		 * flight, and CG_CAP_LAG_MS (250 ms, 350 with the ticks) against a
		 * 25 ms Starlink counted any fall of the sending rate as a loss */
		if (cfg->seq_lag && cfg->rx_top) {
			xt = cg_cap_seq_t(l, cfg->rx_top, &ok);
			if (ok && cfg->behind_us > 0)
				xt = xt > (uint64_t)cfg->behind_us / 1000 ? xt - (uint64_t)cfg->behind_us / 1000 : 0;
			if (ok)
				txl = cg_cap_tx_ati(l, xt, &ok);
		}
		if (!ok && report_ms > CG_CAP_LAG_MS) {
			xt = report_ms - CG_CAP_LAG_MS;
			txl = cg_cap_tx_at(l, xt, &ok);
		}
		if (ok && l->have_txl && txl >= l->txl_prev)
			dlost = txl - l->txl_prev;
		else
			dlost = UINT64_MAX; /* not known: no loss counted */
		/* sent in an edge's window: the switch's loss, not the link's */
		if (ok && l->have_txl && cfg->slot && dlost != UINT64_MAX &&
		    cg_slot_touches(cfg->slot, (uint64_t)((int64_t)l->txl_t_prev + cfg->real_off_ms),
				    (uint64_t)((int64_t)xt + cfg->real_off_ms)))
			edge = 1;
		if (ok) {
			l->txl_prev = txl;
			l->txl_t_prev = xt;
			l->have_txl = 1;
		}
		if (edge) {
			l->n_edge++;
			dlost = UINT64_MAX;
		}
	}
	if (dlost != UINT64_MAX && dlost >= CG_CAP_SHORT_MIN &&
	    (uint64_t)drx * 1000 < dlost * (1000 - CG_CAP_LOSSY_PERMIL))
		l->lost_ms = report_ms;
	if (dlost != UINT64_MAX && dlost >= CG_CAP_SHORT_MIN &&
	    (uint64_t)drx * 1000 < dlost * (1000 - CG_CAP_LOSSY_PERMIL) &&
	    raw * 2 >= (l->peak_bps > cg_cap_best_bps(l) ? l->peak_bps : cg_cap_best_bps(l)))
		l->lossy_ms = report_ms;
	l->lossy = cfg->lossy && l->lossy_ms && report_ms - l->lossy_ms < CG_CAP_LOSSY_MS;
	l->noprobe = l->lossy && !cfg->lossy_probe;
	if (cfg->loss_pct && loaded && dlost != UINT64_MAX && dlost >= CG_CAP_SHORT_MIN &&
	    (uint64_t)drx * 100 < dlost * (100 - cfg->loss_pct))
		l->loss_n = l->loss_n < 255 ? (uint8_t)(l->loss_n + 1) : 255;
	else
		l->loss_n = 0;
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
		l->sat_load_n = loaded ? (uint8_t)(l->sat_load_n < 255 ? l->sat_load_n + 1 : 255) : 0;
	} else {
		l->sat_n = l->sat_load_n = 0;
	}
	if (l->sat_load_n >= CG_CAP_SAT_N || l->sat_n >= CG_CAP_SAT_LONG) {
		/* Full: the link gives no more than it delivers while full. */
		l->max_cur = l->max_prev = l->sat_bps;
		l->full_ms = report_ms;
		cut = CG_CAP_CUT_FULL;
	} else if (l->short_n >= CG_CAP_SHORT_N) {
		/* Its packets stop arriving: no more than it delivers now. */
		l->max_cur = l->max_prev = rate;
		l->full_ms = report_ms;
		cut = CG_CAP_CUT_SHORT;
	} else if (cfg->loss_pct && l->loss_n >= CG_CAP_LOSS_N) {
		/* Loaded and losing: full without a queue. Starlink drops what it
		 * cannot carry instead of queueing it (field, 2026-10-18: 15 % lost
		 * while its queue stayed under 40 ms, its capacity kept at 86 Mbit/s
		 * against ~60, the bond under Starlink alone): no more than it
		 * delivered in this sample. */
		l->max_cur = l->max_prev = raw < rate ? raw : rate;
		l->full_ms = report_ms;
		cut = CG_CAP_CUT_LOSS;
	} else {
		if (rate > l->max_cur)
			l->max_cur = l->max_prev = rate;
		cut = CG_CAP_CUT_NONE;
	}
	if (cut != l->cut && cut == CG_CAP_CUT_LOSS) {
		l->n_cut_loss++;
		ev |= CG_CAP_EV_CUT_SHORT; /* a run of samples that lost what was sent, for the decisions */
	} else if (cut != l->cut && cut == CG_CAP_CUT_FULL) {
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

		if (l[i].have_rate && cg_cap_may_probe(&l[i], probe_us))
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

/* Where link l's queue q_us will be in h_ms (split_qpred_ms, task #33, the
 * rescue's prediction of cg_cap_rescue_w brought to the split): q_us plus
 * the slope times h plus half the slope's slope times h squared, at least
 * 0 and at most q_us + 1 s. mode 1: never under q_us (only a rising queue
 * is moved: a draining one predicted empty took the split's whole flow and
 * overshot, bloat's upload); mode 2: as 1 and at most 2 q_us. h_ms 0, or no
 * trend yet: q_us. */
static inline uint32_t cg_cap_qpred(const struct cg_cap_link *l, uint32_t q_us, uint32_t h_ms, unsigned mode)
{
	int64_t p, h = h_ms;

	if (!h_ms || !l->have_s1)
		return q_us;
	p = (int64_t)q_us + l->s1 * h / 1000 + l->s2 / 2 * h * h / 1000000;
	if (mode && p < (int64_t)q_us)
		return q_us;
	if (mode == 2 && p > 2 * (int64_t)q_us)
		return 2 * q_us;
	if (p < 0)
		return 0;
	return p > (int64_t)q_us + 1000000 ? q_us + 1000000 : (uint32_t)p;
}

/* A link's capacity as the split by expected arrival takes it when the link
 * has not been loaded (sent at least 3/4 of it) for CG_CAP_STALE_MS: twice
 * it, and twice again every CG_CAP_STALE_MS more, up to ceil (the largest
 * link's capacity, or its own best) and to max_mul times its own (1:
 * never). The capacity only moves when the link is sent enough to show it,
 * and the ETA sends a link nothing while the
 * others' queues are short: a link measured low once was never given the
 * traffic that would show more. On the testnet's download WOM sat at
 * 9.5 Mbit/s and 0 % of an RTMP stream that Claro carried alone, with n78
 * behind it (story 013). Taken as larger, it gets packets as soon as the
 * others queue a little, and the samples that follow say what it carries;
 * if it carries less, its queue says so at once (cg_cap_tick). */
static inline uint64_t cg_cap_stale(const struct cg_cap_link *l, uint64_t bps, uint64_t ceil, uint64_t now_ms,
				    uint32_t max_mul)
{
	uint64_t age, lim = cg_cap_best_bps(l) > ceil ? cg_cap_best_bps(l) : ceil;

	if (max_mul > 1 && bps * max_mul < lim)
		lim = bps * max_mul;
	if (max_mul <= 1 || !l->loaded_ms || now_ms <= l->loaded_ms || now_ms - l->loaded_ms < CG_CAP_STALE_MS || bps >= lim)
		return bps;
	age = (now_ms - l->loaded_ms) / CG_CAP_STALE_MS; /* 1, 2, ... */
	if (age > 16)
		age = 16;
	return bps << age < lim ? bps << age : lim;
}

/* Whether link l is due a probe (tickle.h): measured, and not loaded for
 * CG_CAP_STALE_MS. */
static inline int cg_cap_stale_p(const struct cg_cap_link *l, uint64_t now_ms)
{
	return l->have_rate && now_ms > l->loaded_ms && now_ms - l->loaded_ms >= CG_CAP_STALE_MS;
}

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
 * m (NULL: every link auto), each link's model (linkmodel.h):
 * - lossy_hold: such a link that lost over CG_CAP_LOSSY_PERMIL of what it
 *   was sent in the last CG_CAP_LOSSY_MS (l->lossy) is held to the highest
 *   queue it showed over CG_CAP_QHI_MS (at least CG_CAP_QHI_MIN_US): past
 *   it a packet would be dropped, and it goes on another link unless every
 *   one is past its own (hold_over_ms: or while it arrives there within
 *   that, eta.h's over_us). Starlink drops at 15-40 ms of queue; the split
 *   sent it everything until its queue passed the cellular links' 25-30 ms
 *   more of base delay, so the cellular links stayed idle and Starlink
 *   dropped the bursts (shallowq, starsat; field, 2026-10-18).
 * - qpred_ms, qpred_mode: its queue taken where it will be that far ahead
 *   (cg_cap_qpred), 0: the queue now.
 * - eta_keep, eta_keep_ms: what the backlog keeps over a stale queue
 *   (eta.h's cg_eta_set).
 *
 * stale_max (split_stale_max): an idle link's capacity grows up to this
 * many times (cg_cap_stale, 1: never), except for the links of no_stale
 * (a tickle that did not show more, tickle.h's cg_tickle_backoff). */
static inline void cg_cap_eta(struct cg_eta *e, const struct cg_cap_link l[CG_MAX_LINKS], uint16_t mask,
			      uint32_t probe_us, const struct cg_link_model *m, uint32_t stale_max, uint16_t no_stale,
			      uint64_t now_us)
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
		if (cg_cap_may_probe(&l[i], probe_us))
			bps = cg_cap_probe(&l[i], bps, big);
		bps = cg_cap_stale(&l[i], bps, big, now_us / 1000, no_stale >> i & 1 ? 1 : stale_max);
		if (m) {
			e->l[i].keep = m[i].eta_keep;
			e->l[i].keep_ms = m[i].eta_keep_ms;
		}
		cg_eta_set(e, i, bps, (uint32_t)cg_owd_diff(mins[i], ref),
			   cg_cap_qpred(&l[i], l[i].queue_us > 0 ? (uint32_t)l[i].queue_us : 0, m ? m[i].qpred_ms : 0,
					m ? m[i].qpred_mode : 0),
			   now_us);
		if (m && m[i].lossy_hold && l[i].lossy) {
			uint32_t hi = l[i].qhi_cur > l[i].qhi_prev ? l[i].qhi_cur : l[i].qhi_prev;

			e->l[i].limit_us = hi > CG_CAP_QHI_MIN_US ? hi : CG_CAP_QHI_MIN_US;
			e->l[i].over_us = (uint32_t)m[i].hold_over_ms * 1000;
		}
	}
}

/* What a tickle (tickle.h) needs from a tick: *due, the links of mask that
 * are due one (cg_cap_stale_p), each one's rate, what cg_cap_probe offers
 * it, and every link's capacity (to judge a tickle by). */
static inline void cg_cap_tickle(const struct cg_cap_link l[CG_MAX_LINKS], uint16_t mask, uint64_t now_ms, uint16_t *due,
				 uint64_t bps[CG_MAX_LINKS], uint64_t cap[CG_MAX_LINKS])
{
	uint64_t big = 0;

	*due = 0;
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((mask >> i & 1) && l[i].have_rate && cg_cap_bps(&l[i]) > big)
			big = cg_cap_bps(&l[i]);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		bps[i] = 0;
		cap[i] = l[i].have_rate ? cg_cap_bps(&l[i]) : 0;
		if (!(mask >> i & 1) || !cg_cap_stale_p(&l[i], now_ms))
			continue;
		bps[i] = cg_cap_probe(&l[i], cg_cap_bps(&l[i]), big);
		*due |= (uint16_t)(1u << i);
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
