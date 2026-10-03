/* Link health: which links carry every packet in the sender's direction.
 *
 * The receiver measures the one-way delay of each probe and probe reply
 * (its clock minus the sender's timestamp, so only differences between the
 * links of one session mean anything) and reports it on the same link. The
 * sender filters those reports and compares its links:
 *
 * - An active link that stays more than mute_behind_us behind the fastest
 *   active link for settle_ms is muted: it keeps its socket and its probes,
 *   but carries no payload (or one packet in `trickle`).
 * - A muted link comes back when it stays within unmute_behind_us of the
 *   fastest active link for 2 x settle_ms. Each unmute that does not last
 *   CG_H_FLAP_MS doubles that wait, up to 2^CG_H_MAX_BACKOFF times.
 * - At least min_active live links always carry everything: muting stops
 *   there, and when an active link stalls the fastest muted one is promoted
 *   at once. The fastest active link is never behind itself, so it is never
 *   muted for delay.
 *
 * Liveness (stall) is separate and immediate: a stalled link carries nothing
 * until it answers again, whatever its state. A stall does not erase what a
 * link was doing (a late link that falls silent while its queue fills keeps
 * counting towards its mute). Pure functions, no clock and no I/O
 * (docs/historias/006).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_HEALTH_H
#define CG_HEALTH_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */

#define CG_H_ACTIVE 0
#define CG_H_MUTED 1
#define CG_H_MAX_BACKOFF 3  /* unmute wait up to 2 x settle x 8 */
#define CG_H_FLAP_MS 30000  /* an unmute that lasts less than this did not work */
#define CG_STALL_PROBES 3   /* unanswered probes, or announced intervals, before a stall */
/* A stall is a silence longer than a live link can go through: when a
 * link's queue fills, its replies stop for as long as the delay jumps and
 * most probes are lost to the full queue. That is for muting to handle, not
 * for stalls; a dead link costs only its own copies meanwhile, since at
 * least min_active links carry everything. */
#define CG_STALL_SLACK_MS 1000

struct cg_hcfg {
	uint32_t mute_behind_us;   /* 0: never mute for delay */
	uint32_t unmute_behind_us; /* <= mute_behind_us */
	uint32_t settle_ms;
	uint32_t min_active;       /* >= 1 */
};

struct cg_hlink {
	uint8_t state;     /* CG_H_ACTIVE or CG_H_MUTED */
	uint8_t backoff;   /* unmute wait = 2 x settle << backoff */
	uint8_t have_owd;
	uint8_t holding;   /* the pending transition's condition holds since cond_ms */
	uint8_t have_behind;
	int32_t behind_us; /* last delay behind the fastest active link (negative: ahead) */
	uint32_t owd;      /* filtered one-way delay reported by the receiver (raw, wraps) */
	uint64_t fresh_until_ms;
	uint64_t cond_ms;
	uint64_t changed_ms;
	uint64_t unmuted_ms; /* last unmute that was a try (0: none) */
	uint64_t mutes;
	uint32_t trickle_cnt;
};

static inline void cg_health_reset(struct cg_hlink *h, uint64_t now_ms)
{
	memset(h, 0, sizeof(*h));
	h->changed_ms = now_ms;
}

/* a - b for two raw one-way delays of the same session, wrap-safe. */
static inline int32_t cg_owd_diff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

/* A delay report from the receiver, usable for valid_ms. The filter is an
 * EWMA with gain 1/4: about 4 reports to follow a change. */
static inline void cg_health_report(struct cg_hlink *h, uint32_t owd, uint64_t now_ms, uint32_t valid_ms)
{
	if (!h->have_owd) {
		h->owd = owd;
		h->have_owd = 1;
	} else {
		h->owd += (uint32_t)(cg_owd_diff(owd, h->owd) / 4);
	}
	h->fresh_until_ms = now_ms + valid_ms;
}

static inline int cg_health_fresh(const struct cg_hlink *h, uint64_t now_ms)
{
	return h->have_owd && now_ms <= h->fresh_until_ms;
}

static inline int cg_popcount16(uint16_t m)
{
	int n = 0;

	for (; m; m &= (uint16_t)(m - 1))
		n++;
	return n;
}

/* The fastest link in mask with a fresh report, or -1. */
static inline int cg_health_fastest(const struct cg_hlink *h, int n, uint16_t mask, uint64_t now_ms)
{
	int best = -1;

	for (int i = 0; i < n; i++)
		if ((mask >> i & 1) && cg_health_fresh(&h[i], now_ms) &&
		    (best < 0 || cg_owd_diff(h[i].owd, h[best].owd) < 0))
			best = i;
	return best;
}

/* One evaluation of links 0..n-1 (n <= CG_MAX_LINKS), every ~100 ms. live:
 * links that can carry right now. Returns the links whose state changed. */
static inline uint16_t cg_health_eval(struct cg_hlink *h, int n, uint16_t live, uint64_t now_ms,
				      const struct cg_hcfg *c)
{
	uint16_t act = 0, muted = 0, changed = 0;
	int nact, best;

	for (int i = 0; i < n; i++) {
		if (!(live >> i & 1))
			continue; /* frozen until it answers again */
		if (h[i].state == CG_H_ACTIVE)
			act |= (uint16_t)(1u << i);
		else
			muted |= (uint16_t)(1u << i);
	}
	nact = cg_popcount16(act);

	/* Safety net: promote the fastest muted links until min_active carry. */
	while (nact < (int)c->min_active && muted) {
		int p = cg_health_fastest(h, n, muted, now_ms);

		if (p < 0)
			for (p = 0; !(muted >> p & 1); p++)
				;
		h[p].state = CG_H_ACTIVE;
		h[p].changed_ms = now_ms;
		h[p].holding = 0;
		h[p].unmuted_ms = 0; /* forced, not a try: no backoff if it gets muted again */
		act |= (uint16_t)(1u << p);
		muted &= (uint16_t)~(1u << p);
		changed |= (uint16_t)(1u << p);
		nact++;
	}

	best = cg_health_fastest(h, n, act, now_ms);
	for (int i = 0; i < n; i++) {
		int cond;

		if (!((act | muted) >> i & 1))
			continue;
		if (best < 0 || !cg_health_fresh(&h[i], now_ms)) {
			h[i].holding = 0;
			h[i].have_behind = 0;
			continue;
		}
		h[i].behind_us = cg_owd_diff(h[i].owd, h[best].owd);
		h[i].have_behind = 1;
		if (h[i].state == CG_H_ACTIVE)
			cond = c->mute_behind_us && h[i].behind_us > (int32_t)c->mute_behind_us;
		else
			cond = h[i].behind_us <= (int32_t)c->unmute_behind_us;
		if (!cond) {
			h[i].holding = 0;
		} else if (!h[i].holding) {
			h[i].holding = 1;
			h[i].cond_ms = now_ms;
		}
	}

	/* Unmute every muted link whose condition held long enough. */
	for (int i = 0; i < n; i++) {
		uint64_t wait = (2 * (uint64_t)c->settle_ms) << h[i].backoff;

		if (!(muted >> i & 1) || !h[i].holding || now_ms - h[i].cond_ms < wait)
			continue;
		h[i].state = CG_H_ACTIVE;
		h[i].changed_ms = h[i].unmuted_ms = now_ms;
		h[i].holding = 0;
		act |= (uint16_t)(1u << i);
		changed |= (uint16_t)(1u << i);
		nact++;
	}

	/* Mute the furthest behind first, as long as min_active carriers remain. */
	for (;;) {
		int w = -1;

		for (int i = 0; i < n; i++)
			if ((act >> i & 1) && h[i].holding && now_ms - h[i].cond_ms >= c->settle_ms &&
			    (w < 0 || h[i].behind_us > h[w].behind_us))
				w = i;
		if (w < 0 || nact - 1 < (int)c->min_active)
			break;
		if (h[w].unmuted_ms && now_ms - h[w].unmuted_ms < CG_H_FLAP_MS) {
			if (h[w].backoff < CG_H_MAX_BACKOFF)
				h[w].backoff++;
		} else {
			h[w].backoff = 0;
		}
		h[w].state = CG_H_MUTED;
		h[w].mutes++;
		h[w].changed_ms = now_ms;
		h[w].holding = 0;
		h[w].trickle_cnt = 0;
		act &= (uint16_t)~(1u << w);
		changed |= (uint16_t)(1u << w);
		nact--;
	}
	return changed;
}

/* Links that carry every packet: the live active ones; if none, every live
 * link; if none is live, every link present (never black out). */
static inline uint16_t cg_health_carriers(const struct cg_hlink *h, int n, uint16_t present, uint16_t live)
{
	uint16_t act = 0;

	live &= present;
	for (int i = 0; i < n; i++)
		if ((live >> i & 1) && h[i].state == CG_H_ACTIVE)
			act |= (uint16_t)(1u << i);
	return act ? act : live ? live : present;
}

/* A muted link's share: 1 for one packet in every `every` (0: none). */
static inline int cg_trickle(struct cg_hlink *h, uint32_t every)
{
	if (!every || ++h->trickle_cnt < every)
		return 0;
	h->trickle_cnt = 0;
	return 1;
}

/* Client side: a link is stalled when CG_STALL_PROBES probes in a row are
 * unanswered and the oldest has been out for two RTTs plus slack. This
 * follows the probe rate (fast with traffic, slow when idle) on its own. */
static inline int cg_probes_stalled(uint32_t unanswered, uint64_t first_unanswered_ms, uint64_t now_ms,
				    uint32_t srtt_ms)
{
	return unanswered >= CG_STALL_PROBES &&
	       now_ms - first_unanswered_ms > 2 * (uint64_t)srtt_ms + CG_STALL_SLACK_MS;
}

/* Server side: a path is stalled after CG_STALL_PROBES of the probe
 * intervals the client announced, plus slack, without a verified packet. */
static inline int cg_path_stalled(uint64_t last_rx_ms, uint64_t now_ms, uint32_t interval_ms)
{
	return !last_rx_ms || now_ms - last_rx_ms > (uint64_t)CG_STALL_PROBES * interval_ms + CG_STALL_SLACK_MS;
}

#endif
