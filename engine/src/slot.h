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
 * trusted: its losses there are the switch, not a full link (capacity.h).
 * Copies to its peers only there (starlink_edge_copies) are in
 * docs/desvan/.
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

#endif
