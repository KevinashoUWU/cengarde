/* Starlink's slot clock (story 015, task #65): every 15 s, at :12, :27,
 * :42 and :57 of each minute (UTC), Starlink reconfigures its links, and a
 * link stops for ~45 ms. On nostalgica (2026-10-19, 1.7 h of probes at
 * 10 Hz, 477 slots) what was sent 45 to 20 ms before the switch was lost,
 * what came in the last 20 ms waited ~55 ms more, and 63 % of the losses
 * fell there against 0.19 % in the rest of the slot; the same seconds as
 * the measurements published of other dishes, so a property of the
 * constellation's scheduler, not of the site.
 *
 * The clock is the wall clock (CLOCK_REALTIME, kept by NTP on the router
 * and the VPS): edge_ms is the edge's offset in each period (12000), pre_ms
 * and post_ms the window around it in which a link with the clock is not
 * trusted: its losses there are the switch, not a full link (capacity.h),
 * and with starlink_edge_copies its peers carry copies only there.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SLOT_H
#define CG_SLOT_H

#include <stdint.h>

#define CG_SLOT_PERIOD_MS 15000u
#define CG_SLOT_EDGE_MS 12000u /* :12, :27, :42, :57 */
#define CG_SLOT_PRE_MS 100u    /* the window starts this long before the edge */
#define CG_SLOT_POST_MS 50u    /* and ends this long after it */

struct cg_slot {
	uint32_t edge_ms; /* the edge's offset in the period */
	uint32_t pre_ms, post_ms;
};

/* ms since the newest edge at or before real_ms (wall clock, ms). */
static inline uint32_t cg_slot_since(const struct cg_slot *s, uint64_t real_ms)
{
	return (uint32_t)((real_ms + CG_SLOT_PERIOD_MS - s->edge_ms % CG_SLOT_PERIOD_MS) % CG_SLOT_PERIOD_MS);
}

/* Whether real_ms is in an edge's window. */
static inline int cg_slot_near(const struct cg_slot *s, uint64_t real_ms)
{
	uint32_t t = cg_slot_since(s, real_ms);

	return t < s->post_ms || t + s->pre_ms >= CG_SLOT_PERIOD_MS;
}

/* Whether the span [a, b] (wall clock, ms; b >= a) touches an edge's
 * window. */
static inline int cg_slot_touches(const struct cg_slot *s, uint64_t a, uint64_t b)
{
	uint32_t t;

	if (b < a)
		return 0;
	if (b - a >= CG_SLOT_PERIOD_MS || cg_slot_near(s, a))
		return 1;
	/* the next window's start after a */
	t = cg_slot_since(s, a);
	return a + (CG_SLOT_PERIOD_MS - s->pre_ms - t) <= b;
}

/* ms until the next window starts (0: in one now). */
static inline uint32_t cg_slot_until(const struct cg_slot *s, uint64_t real_ms)
{
	if (cg_slot_near(s, real_ms))
		return 0;
	return CG_SLOT_PERIOD_MS - s->pre_ms - cg_slot_since(s, real_ms);
}

/* starlink_edge_copies (task #65, the low-spend redundancy of #39): in
 * redundancy, outside an edge's window a copied packet goes only on the
 * slot links that are fit (a report on them within CG_SLOT_FIT_MS, no loss
 * of theirs counted in CG_SLOT_LOST_MS, capacity.h's lost_ms), and on
 * every carrier in the window or when none is fit. The slot links of carry
 * among fit, or 0: every carrier (in a window, or none fit). */
#define CG_SLOT_FIT_MS 300u
#define CG_SLOT_LOST_MS 2000u

static inline uint16_t cg_slot_solo(const struct cg_slot *s, uint64_t real_ms, uint16_t slot_links, uint16_t fit,
				    uint16_t carry)
{
	uint16_t sl = (uint16_t)(slot_links & fit & carry);

	if (!sl || cg_slot_near(s, real_ms))
		return 0;
	return sl;
}

/* With a peer that reorders, a packet outside an edge's window goes as
 * split DATA on the first of solo (cg_slot_solo) instead of a copy on it
 * alone: CG_F_SPLIT, so that the receiver waits for a gap there and asks
 * for it (rescue.h), and the sender keeps it for that. The losses of a slot
 * that are not at its edges (37 % on nostalgica) come back in a round trip
 * instead of being lost. Small packets (under small) keep their copies.
 * With two fit Starlink links or more (several dishes, story 022) a copy
 * on each of them instead: split DATA went on the first dish alone and left
 * the others idle, where a copy on two dishes needs no round trip for what
 * one loses alone. The link, or -1: a copy (on the links of
 * cg_slot_copy_mask). */
static inline int cg_slot_pick(uint16_t solo, uint32_t len, uint32_t small, uint8_t *flags)
{
	if (!solo || len < small || (solo & (solo - 1)))
		return -1;
	*flags |= 0x04; /* CG_F_SPLIT (proto.h) */
	return __builtin_ctz(solo);
}

/* The links a copied packet (not split DATA, not small) may go on under
 * starlink_edge_copies: solo (cg_slot_solo) when its packets go as copies,
 * without a peer that reorders (pick 0) or with two fit Starlink links or
 * more (cg_slot_pick); else every carrier, 0xffff. */
static inline uint16_t cg_slot_copy_mask(uint16_t solo, int pick)
{
	if (!solo || (pick && !(solo & (solo - 1))))
		return 0xffff;
	return solo;
}

#endif
