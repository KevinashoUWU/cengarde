/* What the ticks record in the decision ring (decision.h), from what the
 * pure decision functions say changed: health.h's changed links, G4's
 * (split.h) changed links and reasons, capacity.h's events and govern.h's
 * crossings. The client (upload) and each server session (download) call
 * the same functions. Pure: no clock, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DECREC_H
#define CG_DECREC_H

#include <stdint.h>

#include "capacity.h"
#include "decision.h"
#include "govern.h"
#include "health.h"
#include "split.h"

_Static_assert(CG_DEC_LEAVE_QUEUE + CG_G4_LEAVE_STALL == CG_DEC_LEAVE_STALL, "G4 events in decision.h's order");
_Static_assert(CG_DEC_LEAVE_QUEUE + CG_G4_REJOIN_OFF == CG_DEC_REJOIN_OFF, "G4 events in decision.h's order");

/* G1: the links cg_health_eval changed. */
static inline void cg_decrec_health(struct cg_decring *r, const struct cg_hlink *h, uint16_t changed, uint64_t now_ms)
{
	for (int i = 0; changed; i++, changed >>= 1) {
		unsigned m;

		if (!(changed & 1))
			continue;
		if (h[i].state == CG_H_MUTED)
			m = CG_DEC_MUTE;
		else if (h[i].unmuted_ms == now_ms)
			m = CG_DEC_UNMUTE;
		else
			m = CG_DEC_UNMUTE_FORCED;
		cg_dec_add(r, now_ms, (unsigned)i, m, h[i].state == CG_H_MUTED ? CG_H_ACTIVE : CG_H_MUTED, h[i].state,
			   h[i].have_behind ? h[i].behind_us : 0);
	}
}

/* The G4 states of every link, before an evaluation, for cg_decrec_g4. */
static inline void cg_decrec_g4_before(const struct cg_spread *s, uint8_t st[CG_MAX_LINKS])
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		st[i] = (uint8_t)cg_g4_state(s, i);
}

/* G4: the links cg_split_spread_eval_r changed (each with its reason in
 * s->ev), and the stress and recovery flags against what was recorded
 * last (*stress, *recover). */
static inline void cg_decrec_g4(struct cg_decring *r, const struct cg_spread *s, uint16_t changed,
				const uint8_t before[CG_MAX_LINKS], uint8_t *stress, uint8_t *recover, uint64_t now_ms)
{
	for (int i = 0; changed; i++, changed >>= 1)
		if (changed & 1)
			cg_dec_add(r, now_ms, (unsigned)i, CG_DEC_LEAVE_QUEUE + s->ev[i], before[i],
				   cg_g4_state(s, (unsigned)i), s->behind >> i & 1 ? s->behind_us[i] : 0);
	cg_dec_edge(r, stress, s->stress, now_ms, CG_DEC_NOLINK, CG_DEC_STRESS_ON, CG_DEC_STRESS_OFF,
		    (int64_t)s->n_qleave);
	cg_dec_edge(r, recover, s->recover, now_ms, CG_DEC_NOLINK, CG_DEC_RECOVER_ON, CG_DEC_RECOVER_OFF,
		    (int64_t)s->n_recover);
}

/* The capacity of one link: what cg_cap_tick returned (ev), and its
 * capacity before the tick. */
static inline void cg_decrec_cap(struct cg_decring *r, const struct cg_cap_link *l, unsigned link, unsigned ev,
				 uint64_t before_bps, uint64_t now_ms)
{
	int64_t q = l->queue_us;

	if (ev & CG_CAP_EV_CUT_FULL)
		cg_dec_add(r, now_ms, link, CG_DEC_CUT_FULL, (int64_t)before_bps, (int64_t)cg_cap_bps(l), q);
	if (ev & CG_CAP_EV_CUT_SHORT)
		cg_dec_add(r, now_ms, link, CG_DEC_CUT_SHORT, (int64_t)before_bps, (int64_t)cg_cap_bps(l), q);
	if (ev & CG_CAP_EV_RESTORE)
		cg_dec_add(r, now_ms, link, CG_DEC_RESTORE_REST, (int64_t)before_bps, (int64_t)cg_cap_bps(l), q);
}

/* The governor: the links whose weight crossed CG_GOV_HALF (cg_gov_tick),
 * with the weights before the tick (w0: each link's x / CG_GOV_SCALE). */
static inline void cg_decrec_gov(struct cg_decring *r, const struct cg_gov *g, uint16_t crossed,
				 const uint16_t w0[CG_MAX_LINKS], uint64_t now_ms)
{
	for (int i = 0; crossed; i++, crossed >>= 1) {
		uint32_t w = g->l[i].x / CG_GOV_SCALE;

		if (crossed & 1)
			cg_dec_add(r, now_ms, (unsigned)i, cg_gov_cross(w0[i], w) < 0 ? CG_DEC_GOV_DOWN : CG_DEC_GOV_UP,
				   w0[i], w, g->l[i].queue_us);
	}
}

/* The governor's weights before a tick, for cg_decrec_gov. */
static inline void cg_decrec_gov_before(const struct cg_gov *g, uint16_t w0[CG_MAX_LINKS])
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		w0[i] = (uint16_t)(g->l[i].x / CG_GOV_SCALE);
}

/* What decides a split now, for the status ("weights_used"): "eta" when
 * the split by expected arrival has a rate for a link of mask (the links
 * that share the split; cg_eta_pick skips a link of none, and falls back to
 * the weights only when no link has one), and the weights (governor's,
 * capacity's) are unused; else whose weights it takes. */
static inline const char *cg_weights_used(int split_eta, const struct cg_eta *e, uint16_t mask, int capacity,
					  int governor)
{
	int any = 0;

	for (unsigned i = 0; split_eta && i < CG_MAX_LINKS; i++)
		if ((mask >> i & 1) && e->l[i].bps)
			any = 1;
	if (any)
		return "eta";
	return capacity ? "capacity" : governor ? "governor" : "links";
}

#endif
