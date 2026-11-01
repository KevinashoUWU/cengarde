/* docs/desvan/linkcc-cola-reparto.h: the split's queue by the delay (link_cc_bits 1 QETA and 16 QMAX)
 *
 * Out of the engine since claude/lc-clean (story 029, «La limpieza del
 * ritmo por enlace»); it lived in engine/src/linkcc.h until commit 3e35f6f
 * (`git show 3e35f6f:engine/src/linkcc.h`). Not compiled: a block to pick up
 * again with a chisel, for some scenario. How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#define CG_LC_QETA 1u   /* the split's queue: its delay at the pacing rate (else in flight over bw x rtprop) */
#define CG_LC_QMAX 16u  /* the split's queue: the larger of in flight over bw x rtprop and its delay at the pacing rate */

/* The bytes queued on the link: what is in flight over bw x rtprop, or
 * (QETA) its queue's delay at its pacing rate, or (QMAX) the larger. What
 * stayed in the engine is the first. QETA lost 20-53 % in the lab
 * (2026-10-28): its delay is the newest acknowledged packet's and leaves
 * out what went since, so the split overfilled the links. */
static inline uint64_t cg_lc_queued_delay(const struct cg_lc *c, unsigned bits)
{
	uint64_t bdp = c->rtprop ? cg_lc_bw(c) * c->rtprop / 1000000 : 0;
	uint64_t fl = c->inflight > bdp ? c->inflight - bdp : 0, d;

	if (!c->have_owd || !(bits & (CG_LC_QETA | CG_LC_QMAX)))
		return fl;
	d = cg_lc_rate(c) * c->q_us / 1000000;
	if (bits & CG_LC_QETA)
		return d;
	return d > fl ? d : fl;
}

/* From its tests (engine/tests/test_linkcc.c): QETA, in test_lc_queue,
 *
 *	CHECK_EQ(cg_lc_queued(&c), cg_lc_rate(&c) * 80 * MS / 1000000);
 *
 * and QMAX, in test_lc_satq_qmax:
 *
 *	c.bits = CG_LC_QMAX;
 *	c.rtprop = 40 * MS;
 *	c.inflight = cg_lc_bw(&c) * 40 * MS / 1000000 + 100000;
 *	cg_lc_owd(&c, t + 20 * MS, 1000000 + 2 * MS);
 *	CHECK_EQ(cg_lc_queued(&c), 100000);
 *	cg_lc_owd(&c, t + 30 * MS, 1000000 + 500 * MS);
 *	CHECK_EQ(cg_lc_queued(&c), cg_lc_rate(&c) * 500 * MS / 1000000);
 */
