/* The dominant link (story 015, task #62): a Starlink far above the
 * cellular links next to it keeps the split to itself.
 *
 * Field, nostalgica 2026-10-19: Starlink gave ~450 Mbit/s down alone, each
 * LTE link 20-30, and the bond ~140 against 264-352 for redundancy. Bench
 * dominant (Starlink 400 Mbit/s down with a 15 ms buffer, cellular 25 and
 * 18 with 28-32 ms more RTT) gives the same: the bond 114-149 against
 * 265-350 for Starlink alone. What a spill to the slower links costs: the
 * receiver holds Starlink's packets for theirs (held_total ~ every packet),
 * releases them in bursts, TCP answers the burst of ACKs with a burst of
 * data, and Starlink's shallow buffer drops it (netem: 2-4 % lost at
 * 100-150 Mbit/s, against 0.3 % at 270-370 alone); its capacity, lossy and
 * not probed (linkmodel.h), stays where the split left it, ~half of what it
 * carries. The slower links add 10 % at most for that. With Starlink at
 * 100 Mbit/s (+43 %) the bond and Starlink alone came even; at 200 (+21 %)
 * the bond lost 40 %.
 *
 * So a link whose model has dominant_pct, delivering (its best rate over
 * CG_DOM_WIN_MS) at least dominant_pct % of what the links slower than it
 * (base delay CG_DOM_BASE_US or more behind) can carry together, makes the
 * split step aside: every packet goes as in redundancy, on every carrier,
 * the copies to the slower links paced (starlink's pace_peers_ms, eta.h),
 * and the receiver holds nothing. Redundancy is what measured as good as
 * Starlink alone, in the field and on the bench; keeping only the slower
 * links out of the split (as guard G4 does, split.h) left the split's
 * reorderer and rescue on one lossy link, and TCP stalled for seconds
 * (bench dominant, 2026-10-19). The split comes back when the dominant link
 * delivers under 4/5 of that: the load went, or it lost its edge (an
 * obstruction, a slot of low rate). Links as fast as it do not count: no
 * reordering, the spill costs nothing (shallowqd).
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DOMINANT_H
#define CG_DOMINANT_H

#include <stdint.h>
#include <string.h>

#include "capacity.h"  /* struct cg_cap_link, cg_cap_bps */
#include "linkmodel.h" /* struct cg_link_model */

#define CG_DOM_WIN_MS 10000u /* the dominant link's delivery: its best over this long (two halves) */
#define CG_DOM_BASE_US 2000  /* a link this much slower in base delay arrives behind it */
#define CG_DOM_OUT_NUM 4     /* it keeps them out down to 4/5 of its dominant_pct */
#define CG_DOM_OUT_DEN 5

struct cg_dom {
	uint64_t hi_cur, hi_prev; /* the dominant link's best delivery rate (bps), in two halves of CG_DOM_WIN_MS */
	uint64_t hi_ms;           /* when the current half began */
	int8_t link;              /* the candidate, -1: none */
	uint16_t out;             /* the slower links it dominates; not 0: the split steps aside */
};

static inline void cg_dom_reset(struct cg_dom *d)
{
	memset(d, 0, sizeof(*d));
	d->link = -1;
}

/* A link's base delay: the minimum of its delays (govern.h's halves). */
static inline uint32_t cg_dom_base(const struct cg_cap_link *l)
{
	return cg_owd_diff(l->q.min_prev, l->q.min_cur) < 0 ? l->q.min_prev : l->q.min_cur;
}

/* One tick (after the capacities'): mask, the links that carry; m, each
 * link's model (dominant_pct, with the global knob over the preset).
 * Returns the slower links it dominates (0: none, the split goes on), also
 * in d->out. */
static inline uint16_t cg_dom_tick(struct cg_dom *d, const struct cg_cap_link l[CG_MAX_LINKS],
				   const struct cg_link_model *m, uint16_t mask, uint64_t now_ms)
{
	uint64_t best = 0, hi, sum = 0;
	uint16_t slower = 0;
	uint32_t base, pct;
	int dl = -1;

	/* the dominant candidate: of the links with dominant_pct, the one that
	 * delivers most */
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((mask >> i & 1) && m[i].dominant_pct && l[i].have_rate && l[i].q.have_min &&
		    (dl < 0 || l[i].rate_bps > best)) {
			dl = (int)i;
			best = l[i].rate_bps;
		}
	if (dl < 0) {
		cg_dom_reset(d);
		return 0;
	}
	if (dl != d->link) {
		cg_dom_reset(d);
		d->link = (int8_t)dl;
		d->hi_ms = now_ms;
	}
	if (now_ms - d->hi_ms >= CG_DOM_WIN_MS / 2) {
		d->hi_prev = d->hi_cur;
		d->hi_cur = l[dl].rate_bps;
		d->hi_ms = now_ms;
	} else if (l[dl].rate_bps > d->hi_cur) {
		d->hi_cur = l[dl].rate_bps;
	}
	hi = d->hi_cur > d->hi_prev ? d->hi_cur : d->hi_prev;
	base = cg_dom_base(&l[dl]);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		if (!(mask >> i & 1) || (int)i == dl || !l[i].q.have_min ||
		    cg_owd_diff(cg_dom_base(&l[i]), base) < CG_DOM_BASE_US)
			continue;
		if (!l[i].have_rate)
			return d->out = 0; /* not measured yet: what it adds is not known */
		slower |= (uint16_t)(1u << i);
		sum += cg_cap_bps(&l[i]);
	}
	pct = m[dl].dominant_pct;
	if (!slower)
		d->out = 0;
	else if (!d->out)
		d->out = hi * 100 >= sum * pct ? slower : 0;
	else
		d->out = hi * 100 * CG_DOM_OUT_DEN >= sum * pct * CG_DOM_OUT_NUM ? slower : 0;
	return d->out;
}

#endif
