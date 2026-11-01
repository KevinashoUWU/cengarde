/* docs/desvan/capacity-experimentos.h: three experiments of the old capacity: capacity_expire_ms, capacity_loss_queue_ms, capacity_stand
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/capacity.h, client.c, server.c, decision.h, decrec.h and status.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* From capacity.h's header: */
/*
 * With expire_ms (capacity_expire_ms, story 016), a link out of the send
 * set (muted, stalled or out of the split: cfg->out) that has not been
 * sent a loaded sample since for expire_ms has its capacity halved, and
 * again every expire_ms while it stays out, down to 1/16 of its best (at
 * least CG_CAP_EXP_FLOOR); each halving holds fast start back for
 * CG_CAP_REST_MS as a full sample does (cg_cap_expire). Its capacity is
 * otherwise frozen at what it was before it left: on the virtual cell an
 * LTE link that fell off the SNR cliff and was out kept 8-16 Mbit/s against
 * 0.2-1 it carried. On its return it starts from the halved value and the
 * split's probing (twice its capacity under half its best) brings it up
 * as fast as it shows it carries more.
 *
 * With lossq_us (capacity_loss_queue_ms, story 019), a link that loses at
 * least CG_CAP_LQ_PCT % of what it was sent in CG_CAP_LQ_N samples in a
 * row while its queue over the minute's minimum (ql_us) reached lossq_us
 * in each is full at any load: its capacity is what it delivers then
 * (smoothed 1/4 from the second such sample, as sat_bps), a CG_CAP_CUT_LOSS.
 * An operator's short buffer in front of a sender that adapts (TCP) never
 * shows a queue over sat_us nor a load of 3/4 of an overestimated
 * capacity: in ns-3 (LTE, 10 KB of RLC, AIMD) the capacity stayed at 3-6
 * times what the link carried for 40 s, and the loss with a queue cut it
 * to within 2 % in 1.0-1.4 s; random loss without a queue (1-3 %, iid and
 * Gilbert-Elliott) never cut (cg_cap_lossq). A link that tracks its loss
 * (linkmodel.h: Starlink's preset, which drops instead of queueing and is
 * held to its highest queue for it) is cut so only while loaded: its
 * delay shifts by slots, and a light load's loss is the dish's.
 *
 * With stand (capacity_stand, story 019), a standing queue holds fast
 * start back: ql_us above sat_us for CG_CAP_SAT_LONG samples in a row is
 * a full sample for it. A queue that lasts longer than window_ms becomes
 * queue_us's base, the cut lets go and fast start brought the capacity
 * back to half its best: in ns-3, 9.96 Mbit/s against 0.73 carried (the
 * clockwork audit's C5). Mode 2 also lets fast start go before
 * CG_CAP_REST_MS once the link shows room: CG_CAP_ROOM_N samples in a row
 * delivering at least 9/8 of its capacity with no loss and a queue under
 * sat_us (cg_cap_room).
 *
 */

#define CG_CAP_EXP_FLOOR 64000u /* bps: capacity_expire_ms halves a capacity down to no less than this */
#define CG_CAP_LQ_PCT 1  /* capacity_loss_queue_ms: a sample losing at least this % of what was sent */
#define CG_CAP_LQ_N 2    /* in a row, each with ql_us reaching lossq_us: full at any load */
#define CG_CAP_ROOM_N 2  /* capacity_stand 2: samples in a row showing room before fast start may go */
#define CG_CAP_EV_EXPIRE 8u /* cg_cap_tick: out of the send set too long, halved (capacity_expire_ms) */

/* struct cg_cap_cfg had
 *	// capacity_expire_ms (0: off), and whether the link is out of the send
 *	// set this tick (cg_cap_expire)
 *	uint32_t expire_ms;
 *	uint8_t out;
 *	// capacity_loss_queue_ms in us (0: off), and capacity_stand (0 off, 1
 *	// a standing queue holds fast start back, 2 and fast start goes once
 *	// the link shows room)
 *	uint32_t lossq_us;
 *	uint8_t lossq_loaded; // the link tracks its loss (linkmodel.h): lossq only while loaded
 *	uint8_t stand;
 * struct cg_cap_link had
 *	// capacity_expire_ms: since when it is out of the send set (0: in),
 *	// its last halving, and how many
 *	uint64_t out_ms, exp_ms, n_expire;
 *	// capacity_loss_queue_ms: the highest ql_us since the last sample, the
 *	// samples in a row that lost with a queue, what they delivered
 *	// (smoothed), and how often a run of them cut the capacity
 *	int32_t ql_max;
 *	uint8_t lq_n;
 *	uint64_t lq_bps, n_cut_lossq;
 *	// capacity_stand: samples in a row with ql_us above sat_us, and (mode
 *	// 2) samples in a row showing room; how often fast start went early
 *	uint8_t stand_n, room_n;
 *	uint64_t room_bps; // the capacity when the run showing room began
 *	uint64_t n_room;
 * (cg_cap_reset: ql_max = -1; the counters and out_ms/exp_ms kept across a
 * restart of the counters, as the others.)
 *
 * In cg_cap_best (fast start), room let it go before CG_CAP_REST_MS:
 *	int room = l->room_n >= CG_CAP_ROOM_N;
 *	if (cap < best / 2 && (!l->full_ms || now_ms - l->full_ms >= CG_CAP_REST_MS || room) && ...) {
 *		// early: it showed room (capacity_stand 2)
 *		if (room && l->full_ms && now_ms - l->full_ms < CG_CAP_REST_MS)
 *			l->n_room++;
 *		...
 *		l->room_n = 0;
 *
 * In cg_cap_tick: first
 *	if (cg_cap_expire(l, now_ms, cfg->out, cfg->expire_ms))
 *		ev |= CG_CAP_EV_EXPIRE;
 * with each fresh queue sample
 *	if (l->ql_us > l->ql_max)
 *		l->ql_max = l->ql_us;
 * after the loss count
 *	lossq = cg_cap_lossq(l, dpk, dlost, drx, raw, loaded, cfg);
 *	cg_cap_stand(l, report_ms, cfg);
 *	cg_cap_room(l, dlost, drx, raw, cap0, cfg);
 * and after the cuts
 *	// Losing with a queue (capacity_loss_queue_ms): full at any load, and
 *	// no more than it delivers so, below a full sample's smoothed rate too
 *	// (that one fell from 0.87 to 0.25 Mbit/s over 4 s on a link that
 *	// delivered 0.02-0.17, ns-3 with fading); while the run lasts, what it
 *	// delivers so, up or down.
 *	if (lossq && (l->lq_bps < cg_cap_bps(l) || (cut == CG_CAP_CUT_NONE && l->cut == CG_CAP_CUT_LOSS))) {
 *		if (cut == CG_CAP_CUT_NONE) {
 *			if (l->cut != CG_CAP_CUT_LOSS)
 *				l->n_cut_lossq++;
 *			cut = CG_CAP_CUT_LOSS;
 *		}
 *		l->max_cur = l->max_prev = l->lq_bps;
 *		l->full_ms = report_ms;
 *	}
 *
 * The callers (client.c's and server.c's cap_tick) set
 *	cc.expire_ms = cfg->capacity_expire_ms;
 *	cc.lossq_us = cfg->capacity_loss_queue_ms * 1000;
 *	cc.stand = (uint8_t)cfg->capacity_stand;
 *	cc.lossq_loaded = (uint8_t)cc.lossy; // Starlink's preset: its loss without a queue is held, not cut
 *	cc.out = !(in >> i & 1);
 * with in, the links that carry now: the client's send_set (below), the
 * server's from down_paths (live carriers, in the split or copied).
 * decision.h had CG_DEC_CAP_EXPIRE ("cap_expire", why: queue_us), recorded
 * by decrec.h's cg_decrec_cap; the status said capacity.expired,
 * cut_lossq, restore_room. */

/* capacity_expire_ms: link l is out of the send set (out) or not at now_ms.
 * Out for expire_ms with no loaded sample since it left (a tickle that
 * loaded it measured it), its capacity and smoothed rate are halved, and
 * again every expire_ms while it stays out, to no less than 1/16 of its
 * best and CG_CAP_EXP_FLOOR (nor than they are); full_ms is set, so that
 * fast start does not bring it back to half its best before it has
 * carried CG_CAP_REST_MS without being full, and loaded_ms, so that the
 * split by expected arrival does not grow it back as an idle link's
 * (cg_cap_stale) for CG_CAP_STALE_MS. Returns whether it halved. */
static inline int cg_cap_expire(struct cg_cap_link *l, uint64_t now_ms, int out, uint32_t expire_ms)
{
	uint64_t since, cap, floor;

	if (!out || !expire_ms) {
		l->out_ms = 0;
		return 0;
	}
	if (!l->out_ms) {
		l->out_ms = now_ms ? now_ms : 1;
		l->exp_ms = 0;
	}
	since = l->loaded_ms > l->out_ms ? l->loaded_ms : l->out_ms;
	if (!l->have_rate || now_ms < since || now_ms - since < expire_ms ||
	    (l->exp_ms && now_ms - l->exp_ms < expire_ms))
		return 0;
	cap = cg_cap_bps(l);
	floor = cg_cap_best_bps(l) / 16;
	if (floor < CG_CAP_EXP_FLOOR)
		floor = CG_CAP_EXP_FLOOR;
	l->exp_ms = now_ms;
	l->full_ms = now_ms;
	/* as measured now: the ETA's growth of an idle link's capacity
	 * (cg_cap_stale) counts from here, not from before it left */
	l->loaded_ms = now_ms;
	if (cap <= floor)
		return 0;
	cap /= 2;
	if (cap < floor)
		cap = floor;
	l->max_cur = l->max_prev = cap;
	if (l->rate_bps > cap)
		l->rate_bps = cap;
	if (l->sat_bps > cap)
		l->sat_bps = cap;
	l->n_expire++;
	return 1;
}

/* capacity_loss_queue_ms: one sample of link l that was sent dpk packets
 * and delivered drx of dlost that could have arrived (UINT64_MAX: not
 * known), at raw bps, loaded or not. A loss of at least CG_CAP_LQ_PCT % (of
 * at least CG_CAP_SHORT_MIN packets) while ql_us reached lossq_us since the
 * last sample counts, unless it delivered more than it was sent (a queue
 * emptying: its loss is from before, and part of the sample it had nothing
 * to carry; in ns-3 a CBR of 18 then 4 Mbit/s took a link of 12.6 to 6.9
 * so); what such samples deliver is smoothed as sat_bps (each of the first
 * CG_CAP_LQ_N starts it again). Returns whether CG_CAP_LQ_N in a row have:
 * the link is full, whatever its load (with lossq_loaded, only loaded
 * samples count). */
static inline int cg_cap_lossq(struct cg_cap_link *l, uint64_t dpk, uint64_t dlost, uint32_t drx, uint64_t raw,
			       int loaded, const struct cg_cap_cfg *cfg)
{
	int32_t qmax = l->ql_max;

	l->ql_max = -1; /* the next sample's, from its ticks */
	if (!cfg->lossq_us || dlost == UINT64_MAX || dlost < CG_CAP_SHORT_MIN ||
	    (uint64_t)drx * 100 > dlost * (100 - CG_CAP_LQ_PCT) || qmax < 0 || (uint32_t)qmax < cfg->lossq_us ||
	    dpk * 100 < (uint64_t)drx * (100 + CG_CAP_LQ_PCT) || (cfg->lossq_loaded && !loaded)) {
		l->lq_n = 0;
		return 0;
	}
	if (l->lq_n + 1 <= CG_CAP_LQ_N)
		l->lq_bps = raw;
	else if (raw >= l->lq_bps)
		l->lq_bps += (raw - l->lq_bps) / CG_CAP_GAIN;
	else
		l->lq_bps -= (l->lq_bps - raw) / CG_CAP_GAIN;
	l->lq_n = l->lq_n < 255 ? (uint8_t)(l->lq_n + 1) : 255;
	return l->lq_n >= CG_CAP_LQ_N;
}

/* capacity_stand: one sample of link l at report_ms. Its queue over the
 * minute's minimum above sat_us for CG_CAP_SAT_LONG samples in a row is a
 * standing queue, and a full sample for fast start (full_ms), though
 * queue_us has taken it into its base. */
static inline void cg_cap_stand(struct cg_cap_link *l, uint64_t report_ms, const struct cg_cap_cfg *cfg)
{
	if (cfg->stand && l->ql_us >= 0 && (uint32_t)l->ql_us > cfg->sat_us)
		l->stand_n = l->stand_n < 255 ? (uint8_t)(l->stand_n + 1) : 255;
	else
		l->stand_n = 0;
	if (l->stand_n >= CG_CAP_SAT_LONG)
		l->full_ms = report_ms;
}

/* capacity_stand 2: one sample of link l showing room or not: it delivered
 * raw >= 9/8 of its capacity cap as it was before the run's first sample
 * (the capacity follows what it delivers up, a quarter at a time), lost under
 * CG_CAP_LOSSY_PERMIL of dlost (known, at least CG_CAP_SHORT_MIN), and its
 * queue over the minute's minimum is at most sat_us. CG_CAP_ROOM_N in a row
 * let fast start go without waiting CG_CAP_REST_MS (cg_cap_best): a link
 * cut while it was full or losing that now carries more than its capacity
 * says has room again (in ns-3 an AIMD sender's link waited 10 s at the
 * capacity of its bad time after its signal came back). */
static inline void cg_cap_room(struct cg_cap_link *l, uint64_t dlost, uint32_t drx, uint64_t raw, uint64_t cap,
			       const struct cg_cap_cfg *cfg)
{
	uint64_t ref = l->room_n ? l->room_bps : cap;

	if (cfg->stand >= 2 && dlost != UINT64_MAX && dlost >= CG_CAP_SHORT_MIN &&
	    (uint64_t)drx * 1000 >= dlost * (1000 - CG_CAP_LOSSY_PERMIL) && l->ql_us >= 0 &&
	    (uint32_t)l->ql_us <= cfg->sat_us && ref && raw * 8 >= ref * 9) {
		l->room_bps = ref;
		l->room_n = l->room_n < 255 ? (uint8_t)(l->room_n + 1) : 255;
	} else {
		l->room_n = 0;
	}
}

/* The client's links that carry the upload now (client.c): */
/* The links that carry the upload now, for the capacity's expiry
 * (capacity_expire_ms): live and carrying (not muted), and when the upload
 * is split, in the split or taking a copy of every packet. */
static uint16_t send_set(const struct client *c, uint16_t present, uint16_t live, uint64_t now_ms)
{
	uint16_t carry = (uint16_t)(cg_health_carriers(c->uh, CG_MAX_LINKS, present, live) & live), dup0;
	struct cg_split_masks k;

	if (!split_active(c))
		return carry;
	split_weights(c, &dup0);
	k = split_masks(c, carry, split_eligible(c, live, carry, now_ms), dup0);
	return (uint16_t)((k.eligible | k.dup) & carry);
}

/* Its tests (engine/tests/test_capacity.c) */
/* A link out of the send set for expire_ms with nothing measured has its
 * capacity halved, and again every expire_ms, down to 1/16 of its best; on
 * its return fast start holds back for CG_CAP_REST_MS and the probe offers
 * twice its capacity (story 016: an LTE link off the SNR cliff kept the
 * 16 Mbit/s of before while out). Off (0) or in the send set: frozen. */
static void test_capacity_expire(void)
{
	struct sim s;
	struct cg_cap_cfg out = CFG, in = CFG;

	out.expire_ms = in.expire_ms = 2000;
	out.out = 1;

	/* off: 30 s out changes nothing */
	sim_start(&s);
	run(&s, 30, 16, 20);
	CHECK(near(cg_cap_bps(&s.l), 16, 0.05));
	{
		struct cg_cap_cfg off = out;

		off.expire_ms = 0;
		CHECK_EQ(out_ticks(&s, 300, &off), 0);
		CHECK(near(cg_cap_bps(&s.l), 16, 0.05));
		CHECK_EQ(s.l.out_ms, 0);
	}

	/* in the send set (no traffic): nothing either */
	sim_start(&s);
	run(&s, 30, 16, 20);
	CHECK_EQ(out_ticks(&s, 300, &in), 0);
	CHECK(near(cg_cap_bps(&s.l), 16, 0.05));

	/* out: halved after 2 s, not before (its first tick out counts from
	 * itself); every 2 s; down to best / 16 */
	sim_start(&s);
	run(&s, 30, 16, 20);
	CHECK_EQ(out_ticks(&s, 20, &out), 0);
	CHECK(near(cg_cap_bps(&s.l), 16, 0.05));
	CHECK_EQ(out_ticks(&s, 1, &out), 1);
	CHECK(near(cg_cap_bps(&s.l), 8, 0.05));
	CHECK(s.l.rate_bps <= cg_cap_bps(&s.l)); /* the next sample does not bring the old rate back */
	CHECK_EQ(out_ticks(&s, 19, &out), 0);
	CHECK_EQ(out_ticks(&s, 1, &out), 1);
	CHECK(near(cg_cap_bps(&s.l), 4, 0.05));
	CHECK_EQ(out_ticks(&s, 100, &out), 2); /* 2, then 1 = 16 / 16 */
	CHECK(near(cg_cap_bps(&s.l), 1, 0.05));
	CHECK_EQ(out_ticks(&s, 100, &out), 0);
	CHECK_EQ(s.l.n_expire, 4);
	CHECK(near(cg_cap_best_bps(&s.l), 16, 0.05)); /* its best is kept */

	/* back in: fast start waits CG_CAP_REST_MS (the last halving counts as
	 * a full sample), the probe offers twice it, and it measures again */
	CHECK_EQ(out_ticks(&s, 1, &in), 0);
	CHECK_EQ(s.l.out_ms, 0);
	CHECK(cg_cap_probe(&s.l, cg_cap_bps(&s.l), 0) == 2 * cg_cap_bps(&s.l));
	{
		uint64_t c0 = cg_cap_bps(&s.l);

		for (int i = 0; i < 20; i++) /* 2 s at 2 x the capacity, a link of 3 */
			sim_step_cfg(&s, 2 * (double)c0 / MBIT, 3, &in);
		CHECK(cg_cap_bps(&s.l) < 4 * MBIT); /* no jump back to half its best (8) */
		CHECK(cg_cap_bps(&s.l) > c0);       /* it measured more */
	}

	/* a loaded sample while out (a tickle) counts as measured: no halving
	 * for expire_ms after it */
	sim_start(&s);
	run(&s, 30, 16, 20);
	CHECK_EQ(out_ticks(&s, 5, &out), 0);
	for (int i = 0; i < 10; i++)
		sim_step_cfg(&s, 16, 20, &out);
	CHECK_EQ(out_ticks(&s, 10, &out), 0); /* 2.5 s out, 1.4 s after the last loaded sample */
	CHECK(near(cg_cap_bps(&s.l), 16, 0.05));
	CHECK_EQ(out_ticks(&s, 10, &out), 1);
}

/* capacity_loss_queue_ms (story 019, the ns-3 traces of an LTE uplink):
 * an operator's short buffer (6 packets: at most 16 ms at 3 Mbit/s, under
 * sat_us) in front of a sender a little over the link (TCP's sawtooth
 * near the top): the queue never says full and the load is never 3/4 of
 * the old capacity, so off, the capacity stays at the 20 of before. On,
 * losing >= 1 % with a queue >= 10 ms two samples in a row, it is what the
 * link delivers, in about a second. */
static void test_capacity_lossq(void)
{
	struct sim s;
	struct cg_cap_cfg on = CFG;
	int t_cut = -1;

	on.lossq_us = 10 * MS;
	/* off */
	sim_start(&s);
	run(&s, 60, 20, 30); /* 20 delivered, no queue */
	s.buf_pkts = 6;
	for (int i = 0; i < 400; i++)
		sim_step(&s, i % 8 < 4 ? 3.6 : 2.8, 3);
	CHECK(s.l.cut == CG_CAP_CUT_NONE);
	CHECK(near(cg_cap_bps(&s.l), 20, 0.05));
	/* on: cut within 1.5 s, to what it delivers */
	sim_start(&s);
	for (int i = 0; i < 60; i++)
		sim_step_cfg(&s, 20, 30, &on);
	s.buf_pkts = 6;
	for (int i = 0; i < 400; i++) {
		sim_step_cfg(&s, i % 8 < 4 ? 3.6 : 2.8, 3, &on);
		if (t_cut < 0 && s.l.cut == CG_CAP_CUT_LOSS)
			t_cut = i;
	}
	CHECK(t_cut >= 0 && t_cut <= 15);
	CHECK(s.l.n_cut_lossq >= 1);
	CHECK(cg_cap_bps(&s.l) <= 3300000ull && cg_cap_bps(&s.l) >= 2500000ull);
	CHECK(s.l.full_ms > 0);

	/* random loss without a queue (2 %, Starlink's or a radio's): never */
	sim_start(&s);
	for (int i = 0; i < 60; i++)
		sim_step_cfg(&s, 20, 30, &on);
	s.drop_every = 50;
	for (int i = 0; i < 400; i++)
		sim_step_cfg(&s, i % 10 < 5 ? 25 : 15, 30, &on);
	CHECK(s.l.cut == CG_CAP_CUT_NONE && s.l.n_cut_lossq == 0);
	CHECK(cg_cap_bps(&s.l) >= 19 * MBIT);

	/* lossq_loaded (a link that tracks its loss: Starlink's preset): the
	 * short buffer at a light load does not cut; loaded, it does */
	{
		struct cg_cap_cfg ld = on;

		ld.lossq_loaded = 1;
		sim_start(&s);
		for (int i = 0; i < 60; i++)
			sim_step_cfg(&s, 40, 50, &ld);
		s.buf_pkts = 6;
		for (int i = 0; i < 200; i++)
			sim_step_cfg(&s, i % 8 < 4 ? 3.6 : 2.8, 3, &ld);
		CHECK(s.l.cut == CG_CAP_CUT_NONE && near(cg_cap_bps(&s.l), 40, 0.05));
		s.buf_pkts = 60; /* 16 ms at 30 Mbit/s: loaded (33 of 40) and losing */
		for (int i = 0; i < 40; i++)
			sim_step_cfg(&s, 33, 30, &ld);
		CHECK(s.l.cut == CG_CAP_CUT_LOSS);
		CHECK(cg_cap_bps(&s.l) <= 31 * MBIT);
	}
}

/* capacity_stand: a link whose queue stands longer than window_ms (100 ms
 * on a link of 1, from 20) loses it from queue_us, the full cut lets go,
 * and off, fast start brings it back to half its best, 10 times what it
 * carries (the audit's C5, measured in ns-3). On, the standing queue over
 * the minute's minimum is a full sample for it. */
static void test_capacity_stand(void)
{
	struct sim s;
	struct cg_cap_cfg st = CFG;
	uint64_t hi;

	st.stand = 1;
	for (int on = 0; on < 2; on++) {
		const struct cg_cap_cfg *cf = on ? &st : &CFG;

		sim_start(&s);
		for (int i = 0; i < 300; i++) /* 30 s at 20: its best, and the minute's minimum */
			sim_step_cfg(&s, 20, 30, cf);
		for (int i = 0; i < 10; i++) /* 1 s over a link of 1: 100 ms of queue */
			sim_step_cfg(&s, 2, 1, cf);
		hi = 0;
		for (int i = 0; i < 300; i++) { /* 30 s at exactly 1: the queue stands */
			sim_step_cfg(&s, 1, 1, cf);
			if (i > 50 && cg_cap_bps(&s.l) > hi)
				hi = cg_cap_bps(&s.l);
		}
		CHECK(s.l.ql_us > 50000);
		CHECK(s.l.queue_us < 20000); /* the 10 s minimum has the queue in it */
		if (on) {
			CHECK(hi < 2 * MBIT);
			CHECK_EQ(s.l.n_restore, 0);
		} else {
			CHECK(hi >= 9 * MBIT);
			CHECK(s.l.n_restore >= 1);
		}
	}
}

/* capacity_stand 2: a link cut to 10 (best 80) whose queue drains (its
 * capacity follows the smoothed rate to 24) and that then delivers 30, more
 * than 9/8 of it, with no loss and no queue, twice, has room: fast start
 * brings it to half its best (40) in about a second, not after
 * CG_CAP_REST_MS. A link fed just under the 10 it carries does not. */
static void test_capacity_room(void)
{
	struct sim s;
	struct cg_cap_cfg r2 = CFG, r1 = CFG;
	int t_rest;

	r2.stand = 2;
	r1.stand = 1;
	for (int mode = 1; mode <= 2; mode++) {
		const struct cg_cap_cfg *cf = mode == 2 ? &r2 : &r1;

		sim_start(&s);
		for (int i = 0; i < 20; i++)
			sim_step_cfg(&s, 80, 100, cf);
		for (int i = 0; i < 30; i++) /* full at 10 */
			sim_step_cfg(&s, 12, 10, cf);
		CHECK(near(cg_cap_bps(&s.l), 10, 0.1));
		for (int i = 0; i < 10; i++) /* the queue drains: back at 100 */
			sim_step_cfg(&s, 0, 100, cf);
		CHECK(cg_cap_bps(&s.l) < 26 * MBIT);
		t_rest = -1;
		for (int i = 0; i < 120; i++) { /* sent 30: more than its capacity */
			sim_step_cfg(&s, 30, 100, cf);
			if (t_rest < 0 && cg_cap_bps(&s.l) >= 39 * MBIT)
				t_rest = i;
		}
		if (mode == 2) {
			CHECK(t_rest >= 0 && t_rest <= 15);
			CHECK_EQ(s.l.n_room, 1);
		} else {
			CHECK(t_rest > 50); /* the rest, 10 s after the last full sample */
			CHECK_EQ(s.l.n_room, 0);
		}
	}
	/* fed under what it carries: no room, no early fast start */
	sim_start(&s);
	for (int i = 0; i < 20; i++)
		sim_step_cfg(&s, 50, 60, &r2);
	for (int i = 0; i < 30; i++)
		sim_step_cfg(&s, 12, 10, &r2);
	for (int i = 0; i < 10; i++)
		sim_step_cfg(&s, 0, 10, &r2);
	for (int i = 0; i < 60; i++)
		sim_step_cfg(&s, 9.5, 10, &r2);
	CHECK(cg_cap_bps(&s.l) < 20 * MBIT); /* the drain's smoothed rate, not half its best */
	CHECK_EQ(s.l.n_room, 0);
	CHECK_EQ(s.l.n_restore, 0);
}

/* Measured: capacity_expire_ms (story 016): on the virtual cell the LTE came
 * back at 0.3-1.3 Mbit/s instead of 3.6-21, but the old capacity did no
 * harm (back whole, it was right; back weak, the usual cuts took it down
 * in 1.5-2.5 s); lab unchanged. capacity_loss_queue_ms and capacity_stand
 * (story 019): in ns-3 with known truth they fixed the estimate (+510 % ->
 * -2 %, the standing queue +523 % -> +3 %), but neither the calibrated cell
 * nor the lab showed a TCP gain. Their next step was a field arm with
 * capacity_loss_queue_ms = 10 and capacity_stand = 1; with link_cc each
 * link's own rate took most of what the capacity decided (story 029). */
