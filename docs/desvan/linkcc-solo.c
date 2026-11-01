/* docs/desvan/linkcc-solo.c: each link's own rate alone decides (link_cc_bits 512, ONLY)
 *
 * Out of the engine since claude/lc-clean (story 029, «La limpieza del
 * ritmo por enlace»); it lived in engine/src/linkcc.h, client.c and server.c until commit 3e35f6f
 * (`git show 3e35f6f:engine/src/linkcc.h`). Not compiled: a block to pick
 * up again with a chisel, for some scenario. How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#define CG_LC_ONLY 512u
/* ONLY: each link's own rate alone decides, the engine's capacity
 * (capacity.h) left to measure (story 029, "one rate per link"): a link
 * with its own samples enters the split before the capacity has a rate for
 * it, the capacity's split weights never leave it out (weight 0: copies
 * only), and it is full for the rescue (congestion, not a loss to send
 * again) when its own queue passes its target. Lab +3.4 % (onlylab, n = 3);
 * in the field, 7 rounds: download level, upload -8 %, +-30 % between
 * rounds: it stayed off. */

/* CG_LC_ONLY: whether the link holds more than its queue target now, by its
 * own one-way delay; 0 while not known. */
static inline int cg_lc_full(const struct cg_lc *c)
{
	return c->have_owd && c->q_us > c->target_us;
}

/* client.c's up_full (server.c's path_full, the same with S->sf[p]->tx):
 *
 *	if (c->cfg->link_cc && (lc->bits & CG_LC_ONLY) && c->link[i].sft.acks)
 *		return cg_lc_full(lc); // its own queue (CG_LC_ONLY)
 *	return cg_cap_full(&c->cap[i], c->cfg->rescue_target_ms * 1000, c->lm[i].rfull_hyst);
 *
 * lc_eta: a live link with its own samples entered the expected arrivals
 * though the capacity had no rate for it:
 *
 *	if (!c->link[i].sft.acks || !lc->rtprop ||
 *	    (!e->l[i].bps && !((lc->bits & CG_LC_ONLY) && (live >> i & 1))))
 *		return;
 *
 * cap_tick, after cg_cap_weights: never weight 0 for a link with its own rate
 *
 *	// CG_LC_ONLY: a link with its own rate is never left to copies by
 *	// the capacity's weight 0; the expected arrivals share it out
 *	for (int i = 0; c->cfg->link_cc && (c->cfg->link_cc_bits & CG_LC_ONLY) && i < CG_MAX_LINKS; i++)
 *		if (c->link[i].sft.acks && c->link[i].sft.lc.rtprop && !(c->dup0 >> i & 1))
 *			c->cw[i] = CG_GOV_MAX;
 *
 * and its test, at the end of test_lc_mean:
 *
 *	cg_lc_owd(&c, t, 1000000 + 61 * MS);
 *	CHECK(cg_lc_full(&c));
 *	cg_lc_owd(&c, t, 1000000 + 59 * MS);
 *	CHECK(!cg_lc_full(&c));
 */
