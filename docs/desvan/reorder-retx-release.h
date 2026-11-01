/* docs/desvan/reorder-retx-release.h: reorder_retx_release_ms: a TCP retransmission gives up the gaps held that long before it (story 014)
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/reorder.h, engine.h and status.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* From reorder.h's header, in the walk's order of reasons a gap is given up:
 *
 *   3. with retx_release_us, when a TCP retransmission came at or after the
 *      packet held behind it, sent at least retx_release_us after that
 *      packet: lost.retx (no rescue asked);
 *
 * The release horizon (retx_release_us, 0: off, experimental): a
 * retransmission says TCP took something sent before it for lost, so the
 * gaps before it whose next held packet was sent at least retx_release_us
 * before it (the gap went no later than that packet) are given up, gap by
 * gap in the walk, and the retransmission goes in order right after what
 * they held. With nothing held there is nothing to give up; the horizon
 * stays for what is held next before it, and goes when the head reaches
 * it. In the lab it costs where gaps are packets late on a slow link, not
 * lost (a link whose delay jumps): their retransmissions give them up and
 * the originals come late, as duplicates (story 014).
 *
 * Its state: enum cg_ro_state had CG_RO_LOST_RETX (after CG_RO_LOST_OVF);
 * struct cg_ro_cfg had
 *	uint32_t retx_release_us; // a TCP retransmission gives up the gaps held this long before it; 0: never
 * struct cg_reorder had
 *	uint32_t retx_to;      // with retx_on: gaps up to it, of packets held sent by retx_ts - release, go
 *	uint32_t retx_ts;      // the sender's ts of the newest TCP retransmission
 *	uint8_t retx_on;
 * and the stats lost_retx, late_retx (status: reorder.lost.retx,
 * reorder.late.after_retx). engine.h: .retx_release_us = cfg->reorder_retx_release_ms * 1000
 * config: reorder_retx_release_ms, 0-2000.
 *
 * Hooks in reorder.h:
 *  - cg_ro_moved: if (r->retx_on && (int32_t)(r->retx_to - r->head) <= 0) r->retx_on = 0;
 *  - cg_ro_in, after cg_ro_answered: if (k->retx && cf->retx_release_us) cg_ro_retx_mark(r, k);
 *  - the resync and cg_ro_flush: r->retx_on = 0;
 *  - cg_ro_late: late_retx for a slot CG_RO_LOST_RETX, and the adaptive
 *    margin learns nothing from it (as from CG_RO_LOST_OVF);
 *  - cg_ro_lose: else if (why == CG_RO_LOST_RETX) r->st.lost_retx++;
 *  - cg_ro_next's walk, after "forced" and "nothing held":
 *	} else if (r->retx_on && cg_ro_retx_gone(r, cf, cg_ro_lowest(r), cg_ro_lowest_slot(r))) {
 *		why = CG_RO_LOST_RETX;
 *		if (s->seq == g && s->state == CG_RO_ASKED)
 *			r->st.ask_lost++;
 *	} else if ...
 *    and no rescue asked for it (why != CG_RO_LOST_RETX, as CG_RO_LOST_OVF);
 *  - cg_ro_due: if (r->retx_on && cg_ro_retx_gone(...)) return 0;
 */

/* A TCP retransmission k ahead of the head, with retx_release_us: the
 * horizon moves up to it (never back), its ts the newest. */
static inline void cg_ro_retx_mark(struct cg_reorder *r, const struct cg_ro_pkt *k)
{
	if (!r->retx_on || (int32_t)(k->seq - r->retx_to) > 0) {
		r->retx_to = k->seq;
		r->retx_ts = k->ts;
		r->retx_on = 1;
	}
}

/* Whether the gap at the head goes for the release horizon: n (at n_seq),
 * the lowest held packet above it, is at or before the horizon and was
 * sent at least retx_release_us before the retransmission that set it. */
static inline int cg_ro_retx_gone(const struct cg_reorder *r, const struct cg_ro_cfg *cf, uint32_t n_seq,
				  const struct cg_ro_slot *n)
{
	return r->retx_on && (int32_t)(r->retx_to - n_seq) >= 0 &&
	       (int32_t)(r->retx_ts - n->ts) >= (int32_t)(cf->retx_release_us > INT32_MAX ? INT32_MAX : cf->retx_release_us);
}

/* Its tests (engine/tests/test_reorder.c, the end of sc_retx) */
static void sc_retx_release(const struct base *b)
{
	/* Release 10 ms: a retransmission sent 5 ms after the held 2 leaves
	 * the gap waiting, one sent 10 ms after it gives the gap up; both go
	 * in order behind 2. 1 then comes late, after a retransmission, and
	 * teaches the adaptive margin nothing. */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	R.cf.adaptive = 1;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 1 * MS, 11 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in_free(3, 6 * MS, 16 * MS, 1), CG_RO_HOLD);
	CHECK(OUT_IS(0));
	CHECK(R.r.retx_on);
	CHECK(due(16 * MS) > 0);
	CHECK_EQ(in_free(4, 11 * MS, 21 * MS, 1), CG_RO_HOLD);
	CHECK(OUT_IS(0, 2, 3, 4));
	CHECK_EQ(R.r.st.lost_retx, 1);
	CHECK_EQ(R.r.st.retx, 2);
	CHECK_EQ(R.r.held, 0);
	CHECK(!R.r.retx_on); /* the head passed it */
	CHECK_EQ(in(1, 0, 30 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_retx, 1);
	CHECK_EQ(R.r.link[0].adapt_us, 0);
	CHECK_EQ(hsum(R.r.st.late_by), 1);
	CHECK_EQ(taken(&R.r.st), 5);

	/* Only up to it: gap 1 goes (2 held behind it, sent before the
	 * retransmission 3); gap 4, whose held 5 was sent after 3, waits. */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 1 * MS, 11 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(5, 21 * MS, 31 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in_free(3, 20 * MS, 32 * MS, 1), CG_RO_HOLD);
	CHECK(OUT_IS(0, 2, 3));
	CHECK_EQ(R.r.st.lost_retx, 1);
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(R.r.head, b->sb + 4);
	CHECK(!R.r.retx_on);
	CHECK(marks_ok(&R.r));

	/* Several gaps before it, each with held packets: all go, gap by gap;
	 * the one just before it (7, nothing held between) says nothing of its
	 * age and waits, with the retransmission and what follows. */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 1 * MS, 11 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(5, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(6, 3 * MS, 13 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(9, 31 * MS, 41 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in_free(8, 30 * MS, 42 * MS, 1), CG_RO_HOLD);
	CHECK(OUT_IS(0, 2, 5, 6));
	CHECK_EQ(R.r.st.lost_retx, 3); /* 1, 3 and 4 */
	CHECK_EQ(R.r.held, 2);
	CHECK_EQ(R.r.head, b->sb + 7);
	CHECK_EQ(in(7, 29 * MS, 43 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_TAIL(4, 7, 8, 9));
	CHECK(!R.r.retx_on);
	CHECK(marks_ok(&R.r));

	/* Nothing held but itself: nothing to give up; the horizon stays for a
	 * packet held next before it (a split packet late on its link). */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in_free(4, 30 * MS, 40 * MS, 1), CG_RO_HOLD);
	CHECK_EQ(R.r.st.lost_retx, 0);
	CHECK(R.r.retx_on);
	CHECK_EQ(in(2, 1 * MS, 45 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* and out at once */
	CHECK(OUT_IS(0, 2));
	CHECK_EQ(R.r.st.lost_retx, 1);
	CHECK_EQ(R.r.head, b->sb + 3);
	CHECK(R.r.retx_on);
	CHECK_EQ(in(3, 2 * MS, 46 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_TAIL(2, 3, 4));
	CHECK(!R.r.retx_on);
	CHECK_EQ(R.r.head, b->sb + 5);
	CHECK(marks_ok(&R.r));

	/* Due at once once the horizon covers the head gap (a caller that has
	 * not drained yet). */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 1 * MS, 11 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK(due(20 * MS) > 0);
	{
		struct cg_ro_pkt k = pkt(3, 30 * MS, 40 * MS, 0, CG_F_FREE, DATA, BIG);
		uint16_t buf;

		k.retx = 1;
		CHECK_EQ(cg_ro_in(&R.r, &R.p, &R.cf, &k, R.rset, &buf), CG_RO_HOLD);
		fill(cg_ro_pool_buf(&R.p, buf), k.seq, BIG);
	}
	CHECK_EQ(due(40 * MS), 0);
	CHECK_EQ(drain(40 * MS), 2);
	CHECK_EQ(R.r.head, b->sb + 4);
	CHECK_EQ(R.bad, 0);

	/* Rescue: a gap asked for goes for the retransmission (asked, not
	 * rescued: ask_lost), and none is asked for in its place. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	R.cf.retx_release_us = 10 * MS;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(43 * MS), 0);
	CHECK_EQ(R.r.nask, 1);
	R.r.nask = 0;
	CHECK_EQ(in_free(3, 40 * MS, 50 * MS, 1), CG_RO_HOLD);
	CHECK(OUT_IS(0, 2, 3));
	CHECK_EQ(R.r.nask, 0);
	CHECK_EQ(R.r.st.ask_lost, 1);
	CHECK_EQ(R.r.st.lost_retx, 1);
	CHECK_EQ(R.r.st.lost_cap, 0);
	CHECK_EQ(R.r.held, 0);

	/* A flush forgets the horizon. */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	R.cf.retx_release_us = 10 * MS;
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	in_free(4, 30 * MS, 40 * MS, 1);
	CHECK(R.r.retx_on);
	flush_all();
	CHECK(!R.r.retx_on);
}

/* Measured (story 014): it stayed an experiment, off; tcp_aware's hold in
 * place (the retransmission waits in sequence) is what took the fast
 * retransmissions and DSACKs back to tcp_aware off. */
