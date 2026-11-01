/* docs/desvan/linkcc-apagados.h: what the bits that won did while off: the capacity's seed, STARTUP at x2, no queue for the modes, bw by the largest sample
 *
 * Out of the engine since claude/lc-clean (story 029, «La limpieza del
 * ritmo por enlace»); it lived in engine/src/linkcc.h until commit 3e35f6f
 * (`git show 3e35f6f:engine/src/linkcc.h`). Not compiled: a block to pick
 * up again with a chisel, for some scenario. How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* NOSEED off (1024): the engine's capacity (capacity.h) as the start of a
 * link's bw, each tick in lc_tick, and as GRIT's hint (pctl_hint) for a
 * link without SFACKs yet:
 *
 *	if (c->cap[i].have_rate)
 *		cg_lc_seed(lc, cg_cap_bps(&c->cap[i]) / 8);
 *
 * Level in the lab (+0.5 %) and in the field (2026-11-01). */
/* The engine's capacity for the link (capacity.h), as a start: the estimate
 * begins there instead of from nothing. */
static inline void cg_lc_seed(struct cg_lc *c, uint64_t bps)
{
	if (!c->bw && bps > CG_LC_FLOOR)
		c->bw = c->bmax[0] = bps;
}

/* S32 off (8): STARTUP at x2 (512) instead of 3/2; it overshot.
 *
 *	if (c->mode == CG_LC_STARTUP_M)
 *		return c->bits & CG_LC_S32 ? CG_LC_STARTUP : 512;
 *
 * QMODE off (2): STARTUP, DRAIN and PROBE without the one-way delay, as
 * BBR v1: STARTUP ends only when bw stops growing, DRAIN while in flight
 * passes bw x rtprop (still the fallback without a delay), PROBE's cycle
 * whatever the queue (cg_lc_qstate returned 0, cg_lc_gain the cycle's
 * gain). QMODE + S32 won +8.7 % and 3-10 times fewer losses per link in
 * the lab (2026-10-28).
 *
 * MEAN off (256): past STARTUP too, bw the largest sample of 1-2 s (the
 * bmax buckets, still STARTUP's):
 *
 *	} else if (c->bmax[0] || c->bmax[1]) {
 *		c->bw = c->bmax[0] > c->bmax[1] ? c->bmax[0] : c->bmax[1];
 *		if (c->bw < CG_LC_FLOOR)
 *			c->bw = CG_LC_FLOOR;
 *	}
 *
 * It read the operators' bursts: 1.7-5.2 times a saturated modem's
 * capacity (bench/rateeval.py). Its test, test_lc_loss's tail:
 *
 *	// after 2 s without larger samples bw follows the delivery down
 *	for (int i = 0; i < 50; i++) {
 *		t += 50 * MS;
 *		cg_lc_acked(&c, t, 0, 10 * MBIT, 40 * MS);
 *	}
 *	CHECK_EQ(c.bw, 10 * MBIT);
 *
 * F3 off (4096) and DOM off (2048) are still the engine's path with
 * link_cc = no: FEC v3's guard and loss from capacity.h, the dominant link
 * by its capacity. */
