/* The links the sender carries DATA on, told to the receiver's reorderer
 * (clockwork audit C2, story 013, protocol 11).
 *
 * The problem: the reorderer (reorder.h) waits for a gap on every link of
 * rset, and rset was every link the peer does not mute. RACK gives a gap up
 * only when every link of rset brought a later sequence, and the deadline
 * takes the slowest link of rset's delay plus four deviations: a live link
 * the sender leaves idle (a backup, a link the ETA or G4 sends nothing,
 * split.h, eta.h) brings nothing, so every gap waited for the hold cap
 * (reorder_max_wait_ms). Neither G4 nor the ETA reach the receiver, and
 * leaving out of rset a link without a delay breaks the reorderer's own
 * guarantee (a gap may ride a link not yet measured).
 *
 * Sender (cg_act_tx): each link's last time it carried something that can
 * fill a gap, on the sender's clock, marked when it is sent, after its ts
 * was taken: DATA that went on it alone, a packet sent again on a NACK, a
 * parity, a repair; and of DATA sent on several links (the small packets
 * on every carrier, TCP's retransmissions, copies), only the link expected
 * to bring it first, by the sender's one-way delays, and any of them whose
 * delay it does not know (cg_act_tx_counts). The other copies fill a gap
 * only when the first one is lost, and then come late, as a split
 * packet lost on its link: a backup carrying only copies of small packets
 * (ACKs, pings) no longer holds every gap. Every probe (client) and probe reply (server) carries the mask of
 * the links that carried one within the hold before its own ts, and the
 * hold (proto.h's cg_probe_act): a link out of the mask carried nothing
 * sent after ts - hold. For hold after a start or a reload every link is
 * in it (what the links carried just before is not known).
 *
 * Receiver (cg_act_rx): the mask of the newest probe or reply (by control
 * sequence, as the bonding tail: split.h's cg_split_tail_newer). A link is
 *   - open, and counts in rset as before, when no mask came (or the newest
 *     is older than CG_ACT_STALE_US here: the peer says nothing), when the
 *     mask has it, or when it is known to have carried DATA at or after the
 *     mask's ts (evidence, below): the sender may go on using it;
 *   - closed otherwise, and counts in rset until everything it carried may
 *     have arrived: quiet + its delay + four deviations + its adaptive
 *     margin + reorder_margin_ms (reorder.h's cg_ro_link_until, the
 *     deadline's own terms), quiet being the sender's time after which it
 *     carried nothing (ts - hold of the mask that closed it, kept while the
 *     masks that follow overlap: their quiet spans join; or the newest
 *     evidence before the mask, if later). A link without a delay yet
 *     stays (the reorderer's guarantee: only the cap then).
 * Then rset = the links the peer does not mute AND active (open, or closed
 * and not past that horizon).
 *
 * Evidence (cg_act_rx_data), on every verified first copy of DATA, a
 * parity or a repair: the link it came on carried something at its ts, and
 * the link DATA's CG_F_PREV names carried seq - 1 at or before it. A link
 * the sender starts using after the newest mask is open from the first
 * such evidence, before any mask says so: its own packet, or (sooner, on a
 * faster link) the packet after the first one it carried, which names it.
 * The links it opens are also gathered in fresh, for a caller holding an
 * rset worked out before (a batch).
 *
 * Invariant: a gap is never given up while its packet may still arrive, by
 * the reorderer's own delays, on a link that carried data around when it
 * was sent. Every packet a closed link carried went at or before quiet and
 * arrives within the horizon; a packet sent on it after the newest mask
 * makes it open as soon as that packet, or the one after it that names it,
 * arrives. Gaps cannot go before then: the lowest held packet's link is
 * open (it arrived), so the deadline counts the link of the packet before
 * it, which arrives within that deadline and opens the link of the packet
 * before it, and so on back to the gap; RACK waits for every link so
 * opened to bring something later. What it needs: DATA that goes on one
 * link (split) or with copies on links already counted; loss of the very
 * packet that would name a link that just started may give its gap up
 * early (it comes late, as with any delay beyond the deviations). Probes
 * lost or reordered only leave an older mask in place, which keeps links
 * open longer. A control sequence 2^20 ahead (a server back from a
 * restart) is newer, and evidence too far from the newest mask's ts
 * (CG_ACT_FORGET_US: another clock, or 35 minutes of wrap) is forgotten
 * when a mask comes. The receiver starts over with the session.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_ACTIVE_H
#define CG_ACTIVE_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "reorder.h" /* cg_ro_link_until */

#define CG_ACT_HOLD_MS 200          /* the sender's hold: a link stays in its mask this long after it carried something */
#define CG_ACT_STALE_US 2000000u    /* receiver: a mask taken this long ago says nothing (every link open) */
#define CG_ACT_FORGET_US 10000000   /* receiver: evidence this far from a new mask's ts is forgotten */

/* ---- sender ---- */

struct cg_act_tx {
	uint64_t last_us[CG_MAX_LINKS]; /* when each link last carried something (with sent) */
	uint16_t sent;
	uint8_t started;   /* zeroed memory: every link in the mask */
	uint64_t start_us; /* the start or reload: every link in the mask for the hold after it */
};

/* A start or a reload at now_us: for the hold, every link is in the mask. */
static inline void cg_act_tx_start(struct cg_act_tx *a, uint64_t now_us)
{
	memset(a, 0, sizeof(*a));
	a->start_us = now_us;
	a->started = 1;
}

/* Link carried DATA, a copy, a packet sent again, a parity or a repair at
 * now_us (after the ts it carries was taken). */
static inline void cg_act_tx_sent(struct cg_act_tx *a, unsigned link, uint64_t now_us)
{
	if (link < CG_MAX_LINKS) {
		a->last_us[link] = now_us;
		a->sent |= (uint16_t)(1u << link);
	}
}

/* Of a packet sent on links d, the links it counts on: its one link; with
 * copies, the one expected first by the one-way delays owd (raw, compared
 * wrap-safe; a tie to the lower id) of the links in known, and every link
 * of d not in known. */
static inline uint16_t cg_act_tx_counts(uint16_t d, uint16_t known, const uint32_t owd[CG_MAX_LINKS])
{
	uint16_t out, k = d & known;
	int best = -1;

	if (!(d & (d - 1)))
		return d;
	out = d & (uint16_t)~known;
	for (unsigned i = 0; k; i++, k >>= 1)
		if ((k & 1) && (best < 0 || (int32_t)(owd[i] - owd[best]) < 0))
			best = (int)i;
	if (best >= 0)
		out |= (uint16_t)(1u << best);
	return out;
}

/* The mask for a probe or reply whose ts is now_us: the links that carried
 * something within hold_us before it (or after: a later mark is newer). */
static inline uint16_t cg_act_tx_mask(const struct cg_act_tx *a, uint64_t now_us, uint32_t hold_us)
{
	uint16_t m = 0;

	if (!a->started || (int64_t)(now_us - a->start_us) < (int64_t)hold_us)
		return 0xffff;
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((a->sent >> i & 1) && (int64_t)(now_us - a->last_us[i]) < (int64_t)hold_us)
			m |= (uint16_t)(1u << i);
	return m;
}

/* ---- receiver ---- */

struct cg_act_rx {
	uint8_t have;     /* a mask came */
	uint16_t mask;    /* the newest mask */
	uint32_t ts;      /* its sender ts */
	uint32_t hold_us; /* its hold */
	uint64_t at_us;   /* when it was taken, here */
	uint32_t quiet[CG_MAX_LINKS]; /* closed: the sender's time after which it carried nothing */
	uint32_t ev_ts[CG_MAX_LINKS]; /* the newest sender ts it is known to have carried something at */
	uint16_t ev_ok;
	uint16_t fresh;   /* links evidence opened, for a caller's rset worked out before; it empties it */
	uint64_t masks;   /* masks taken */
	uint64_t opened;  /* links evidence opened before a mask said so */
};

static inline void cg_act_rx_init(struct cg_act_rx *a)
{
	memset(a, 0, sizeof(*a));
}

/* A reload: what was known of the links goes (their ids may have moved),
 * the counters stay; every link is open until the next mask. */
static inline void cg_act_rx_forget(struct cg_act_rx *a)
{
	a->have = 0;
	a->ev_ok = 0;
	a->fresh = 0;
}

/* Whether a mask is in force at now_us (taken, and not stale). A caller's
 * clock a little behind the one the mask was taken by (a batch's read
 * time) still finds it in force. */
static inline int cg_act_rx_on(const struct cg_act_rx *a, uint64_t now_us)
{
	return a->have && (int64_t)(now_us - a->at_us) < (int64_t)CG_ACT_STALE_US;
}

/* Whether link l is open at now_us: the peer may be sending on it now. */
static inline int cg_act_rx_open(const struct cg_act_rx *a, unsigned l, uint64_t now_us)
{
	return !cg_act_rx_on(a, now_us) || (a->mask >> l & 1) ||
	       ((a->ev_ok >> l & 1) && (int32_t)(a->ev_ts[l] - a->ts) >= 0);
}

/* The mask of the newest probe or reply (the caller decides newest, by
 * control sequence), sent at ts with hold_ms, taken at now_us. */
static inline void cg_act_rx_mask(struct cg_act_rx *a, uint32_t ts, uint16_t mask, uint32_t hold_ms, uint64_t now_us)
{
	uint32_t hold_us = hold_ms * 1000u;
	int on = cg_act_rx_on(a, now_us);

	for (unsigned l = 0; l < CG_MAX_LINKS; l++) {
		uint16_t bit = (uint16_t)(1u << l);
		int32_t ev;

		/* evidence of another clock, or of long ago, says nothing now */
		if (a->ev_ok & bit) {
			ev = (int32_t)(ts - a->ev_ts[l]);
			if (ev > CG_ACT_FORGET_US || ev < -CG_ACT_FORGET_US)
				a->ev_ok &= (uint16_t)~bit;
		}
		if (mask & bit)
			continue;
		/* Closed: quiet since ts - hold, or since the quiet of the mask
		 * before when that one closed it too and the two spans touch
		 * (the next mask comes before the hold of this one ends). */
		if (!(on && !(a->mask & bit) && !((a->ev_ok & bit) && (int32_t)(a->ev_ts[l] - a->ts) >= 0) &&
		      (int32_t)(ts - hold_us - a->ts) <= 0))
			a->quiet[l] = ts - hold_us;
	}
	a->have = 1;
	a->mask = mask;
	a->ts = ts;
	a->hold_us = hold_us;
	a->at_us = now_us;
	a->masks++;
}

/* One piece of evidence: link l carried something sent at ts. Returns its
 * bit when that opened it. */
static inline uint16_t cg_act_rx_one(struct cg_act_rx *a, unsigned l, uint32_t ts, uint64_t now_us)
{
	uint16_t bit;
	int was;

	if (l >= CG_MAX_LINKS)
		return 0;
	bit = (uint16_t)(1u << l);
	if ((a->ev_ok & bit) && (int32_t)(ts - a->ev_ts[l]) <= 0)
		return 0;
	was = cg_act_rx_open(a, l, now_us);
	a->ev_ts[l] = ts;
	a->ev_ok |= bit;
	if (was || !cg_act_rx_open(a, l, now_us))
		return 0;
	a->fresh |= bit;
	a->opened++;
	return bit;
}

/* A verified first copy of DATA (or a parity, a repair: prev -1) on link,
 * sent at ts, whose flags name prev (cg_prev_link) as the link of the
 * sequence before it. Returns the links it opened (also added to fresh). */
static inline uint16_t cg_act_rx_data(struct cg_act_rx *a, unsigned link, uint32_t ts, int prev, uint64_t now_us)
{
	uint16_t o = cg_act_rx_one(a, link, ts, now_us);

	if (prev >= 0)
		o |= cg_act_rx_one(a, (unsigned)prev, ts, now_us);
	return o;
}

/* Closed link l: when everything it carried may have arrived, on the
 * reorderer's clock (with margin_us, reorder_margin_ms). 0 when that is not
 * known yet (no delay): it stays. */
static inline int cg_act_rx_until(const struct cg_act_rx *a, const struct cg_reorder *r, uint32_t margin_us,
				  unsigned l, uint32_t *until)
{
	uint32_t q = a->quiet[l];

	if ((a->ev_ok >> l & 1) && (int32_t)(a->ev_ts[l] - q) > 0)
		q = a->ev_ts[l];
	return cg_ro_link_until(r, l, margin_us, q, until);
}

/* rset: the links of base (those the peer does not mute) that are active
 * at now_us (now32: the same time on the reorderer's clock). */
static inline uint16_t cg_act_rx_rset(const struct cg_act_rx *a, const struct cg_reorder *r, uint32_t margin_us,
				      uint16_t base, uint64_t now_us, uint32_t now32)
{
	uint16_t m = base;

	if (!cg_act_rx_on(a, now_us))
		return base;
	for (unsigned l = 0; l < CG_MAX_LINKS; l++) {
		uint32_t until;

		if (!(base >> l & 1) || cg_act_rx_open(a, l, now_us))
			continue;
		/* out once past it: a packet may arrive at the horizon itself */
		if (cg_act_rx_until(a, r, margin_us, l, &until) && (int32_t)(now32 - until) > 0)
			m &= (uint16_t)~(1u << l);
	}
	return m;
}

/* Microseconds from now until a closed link of base leaves rset (its
 * horizon), INT32_MAX when none will: a deadline may come sooner then. */
static inline int32_t cg_act_rx_next_us(const struct cg_act_rx *a, const struct cg_reorder *r, uint32_t margin_us,
					uint16_t base, uint64_t now_us, uint32_t now32)
{
	int32_t next = INT32_MAX;

	if (!cg_act_rx_on(a, now_us))
		return INT32_MAX;
	for (unsigned l = 0; l < CG_MAX_LINKS; l++) {
		uint32_t until;
		int32_t left;

		if (!(base >> l & 1) || cg_act_rx_open(a, l, now_us) || !cg_act_rx_until(a, r, margin_us, l, &until))
			continue;
		left = (int32_t)(until - now32) + 1;
		if (left > 0 && left < next)
			next = left;
	}
	return next;
}

#endif
