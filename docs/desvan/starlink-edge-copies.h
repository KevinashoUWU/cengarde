/* docs/desvan/starlink-edge-copies.h: starlink_edge_copies: in redundancy, copies to a Starlink link's peers only in an edge's window (task #65)
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/slot.h, client.c and server.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#define CG_SLOT_FIT_MS 300u
#define CG_SLOT_LOST_MS 2000u

/* starlink_edge_copies (task #65, the low-spend redundancy of #39): in
 * redundancy, outside an edge's window a copied packet goes only on the
 * slot links that are fit (a report on them within CG_SLOT_FIT_MS, no loss
 * of theirs counted in CG_SLOT_LOST_MS, capacity.h's lost_ms), and on
 * every carrier in the window or when none is fit. The slot links of carry
 * among fit, or 0: every carrier (in a window, or none fit). */
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

/* The client's (server.c's the same, on S->dh and S->cap): */
/* starlink_edge_copies (slot.h): the Starlink links a copied packet goes
 * on alone now, 0: every carrier. */
static uint16_t edge_solo(const struct client *c, uint16_t carry, uint64_t now_ms)
{
	struct cg_slot s = cg_config_slot(c->cfg);
	uint64_t wall = cg_wall_ms();
	uint16_t sl = 0, fit = 0;

	if (wall < CG_WALL_SET_MS)
		return 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_hlink *h = &c->uh[i];
		const struct cg_cap_link *k = &c->cap[i];

		if (!c->lm[i].slot_clock)
			continue;
		sl |= (uint16_t)(1u << i);
		if (h->state == CG_H_ACTIVE && h->report_ms && now_ms - h->report_ms < CG_SLOT_FIT_MS &&
		    !(k->lost_ms && now_ms - k->lost_ms < CG_SLOT_LOST_MS))
			fit |= (uint16_t)(1u << i);
	}
	return cg_slot_solo(&s, wall, sl, fit, carry);
}

/* The hooks: in up_begin (server.c's down_paths, d.esolo),
 *	// starlink_edge_copies with a server that reorders: outside an
 *	// edge's window on Starlink alone, as split DATA (cg_slot_pick)
 *	u->esolo = c->cfg->starlink_edge_copies && c->peer_reorders && c->resc_idx ? edge_solo(c, u->carry, now_ms) : 0;
 * in up_one, the packet's link when the split steps aside (copy, a dominant
 * link): cg_slot_pick(u->esolo, (uint32_t)len, CG_PACE_SMALL, &flags)
 * instead of -1; and in seal_batch, on the copies' masks:
 *	if (c->cfg->starlink_edge_copies) {
 *		uint16_t cm = cg_slot_copy_mask(edge_solo(c, carry, now_ms), c->peer_reorders && c->resc_idx);
 *
 *		for (int j = 0; cm != 0xffff && j < m; j++)
 *			if (c->pick[j] < 0 && c->oiov[j][1].iov_len >= CG_PACE_SMALL)
 *				c->cmask[j] &= cm;
 *	}
 *
 * Its tests (engine/tests/test_slot.c):
 * /* the copies: outside a window, on the fit Starlink links of carry only */
 * CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x1, 0x7), 0x1);
 * CHECK_EQ(cg_slot_solo(&s, T0 + 11950, 0x1, 0x1, 0x7), 0); /* in a window: every carrier */
 * CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x0, 0x7), 0); /* not fit */
 * CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x1, 0x6), 0); /* not carrying */
 * CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x5, 0x4, 0x7), 0x4);
 * CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x0, 0x7, 0x7), 0); /* no Starlink */
 * /* as split DATA on the first of them, small packets copied */
 * {
 * 	uint8_t fl = 0;
 *
 * 	CHECK_EQ(cg_slot_pick(0x4, 1200, 300, &fl), 2);
 * 	CHECK_EQ(fl, CG_F_SPLIT);
 * 	fl = 0;
 * 	/* two dishes fit: a copy on both, not split DATA on the first */
 * 	CHECK_EQ(cg_slot_pick(0x6, 1200, 300, &fl), -1);
 * 	CHECK_EQ(fl, 0);
 * 	CHECK_EQ(cg_slot_pick(0x8, 1200, 300, &fl), 3);
 * 	fl = 0;
 * 	CHECK_EQ(cg_slot_pick(0x4, 100, 300, &fl), -1);
 * 	CHECK_EQ(cg_slot_pick(0, 1200, 300, &fl), -1);
 * 	CHECK_EQ(fl, 0);
 * }
 * /* the copies' mask: solo without a reordering peer, or with two dishes
 *  * or more; one dish with a reordering peer goes as split DATA, its
 *  * copies (small packets) on every carrier */
 * CHECK_EQ(cg_slot_copy_mask(0, 0), 0xffff);
 * CHECK_EQ(cg_slot_copy_mask(0, 1), 0xffff);
 * CHECK_EQ(cg_slot_copy_mask(0x4, 0), 0x4);
 * CHECK_EQ(cg_slot_copy_mask(0x4, 1), 0xffff);
 * CHECK_EQ(cg_slot_copy_mask(0x6, 1), 0x6);
 * CHECK_EQ(cg_slot_copy_mask(0x6, 0), 0x6);
 *
 * Measured (story 015 §4, story 022): copies 2.96 -> 1.02-1.09 a packet
 * with a 8 Mbit/s stream, and with the rescue no datagram lost (starsat,
 * dominant); but with several dishes it is worse (0.43-0.85 % lost): a
 * Starlink losing 0.6 % almost never goes 2 s without a loss, so one dish
 * or none is "fit". Made for one Starlink and expensive cellular links
 * (the low-spend redundancy of task #39). */
