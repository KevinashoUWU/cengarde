/* docs/desvan/split-stress-single.h: split_stress_single: under G4's stress the split uses only the fastest link
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/split.h, client.c and server.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* The links that may take a share of a split packet under G4's stress
 * (either kind): only the fastest link in, while the links' delays are
 * apart. Splitting between links 30-50 ms apart reordered: in the lab
 * (ltespike down) the old empty split, which by accident copied everything
 * or used one link and never split, had 44 TCP retransmissions where
 * splitting had 700-800. elig otherwise; elig too when the lead is not in
 * it. */
static inline uint16_t cg_split_stress_elig(const struct cg_spread *s, uint16_t elig, int single)
{
	if (!single || !(s->stress || s->recover) || s->lead < 0 || !(elig >> s->lead & 1))
		return elig;
	return (uint16_t)(1u << s->lead);
}

/* It was step 2 of cg_split_plan (audit C13), between G3/G4's elig and the
 * XOR's cap, with its own count (CG_PLAN_SINGLE, status split_plan.single):
 *
 *	static inline struct cg_split_plan cg_split_plan(uint16_t elig, uint16_t single, uint16_t share, int copy)
 *	{
 *		struct cg_split_plan p = { elig, 0, 0, CG_PLAN_SPLIT };
 *
 *		single &= elig;
 *		if (single != p.pick) {
 *			p.pick = single;
 *			p.by = CG_PLAN_SINGLE;
 *		}
 *		share &= p.pick; ...
 *
 * The callers: client.c's up_one,
 *	elig = cg_split_stress_elig(&c->spread, u->k.eligible, c->cfg->split_stress_single);
 * and server.c's down path,
 *	uint16_t single = cg_split_stress_elig(&S->spread, d->k.eligible, s->cfg->split_stress_single);
 *	struct cg_split_plan plan = cg_split_plan(d->k.eligible, single, single, stress);
 *
 * Its tests (engine/tests/test_split.c), in test_split_spread_stress:
 * /* under stress the split takes the lead only (0 here), or as given */
 * CHECK_EQ(sp.lead, 0);
 * CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 1), 0x1);
 * CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 0), 0x3);
 * CHECK_EQ(cg_split_stress_elig(&sp, 0x2, 1), 0x2); /* lead not eligible */
 * sp.stress = sp.recover = 0;
 * CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 1), 0x3); /* no stress */
 * and in test_split_plan:
 *	p = cg_split_plan(0x7, 0x1, 0x1, 0); // stress: the lead alone
 *	CHECK_EQ(p.pick, 0x1);
 *	CHECK_EQ(p.xor_by, 0x1); // one link: no XOR group (was 0x7)
 *	CHECK_EQ(p.by, CG_PLAN_SINGLE);
 *	p = cg_split_plan(0x3, 0x1, 0x3, 0); // the cap after single: on single's
 *	CHECK_EQ(p.pick, 0x1);
 *	CHECK_EQ(p.by, CG_PLAN_SINGLE);
 *
 * Measured (story 012/013): splitting between links 30-50 ms apart
 * reordered, the lead alone had 44 TCP retransmissions where splitting had
 * 700-800 (lab, ltespike down); the copies under stress (split_stress_dup,
 * cg_sdup) took its place, and it stayed off. */
