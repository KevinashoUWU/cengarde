/* docs/desvan/linkcc-mtpt.h: MTPT, Maximum Transfer Point Tracking (link_cc_bits 128, link_cc_budget_ms)
 *
 * Out of the engine since claude/lc-clean (story 029, «La limpieza del
 * ritmo por enlace»); it lived in engine/src/linkcc.h until commit 3e35f6f
 * (`git show 3e35f6f:engine/src/linkcc.h`). Not compiled: a block to pick up
 * again with a chisel, for some scenario. How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* The constants it used */
#define CG_LC_MTPT_EPOCH_US 1000000u /* MTPT: one step a second at most */
#define CG_LC_MTPT_STEP_US 20000u    /* by this much */
#define CG_LC_MTPT_MIN_US 20000u     /* the least target */
#define CG_LC_MTPT_CAP_US 300000u    /* the ceiling until the caller sets one */
#define CG_LC_MTPT_GAIN_PCT 3u       /* a change of what the link delivered under this is flat */

/* Its fields in struct cg_lc: the epoch's start, what the link delivered
 * in it, its largest queue, packets sent and lost; the previous epoch's
 * delivery; the way the target moves; the ceiling.
 *
 *	uint64_t mt_t, mt_bytes, mt_rate;
 *	uint32_t mt_qmax, mt_sent, mt_lost, mt_cap_us, mt_base_us;
 *	int8_t mt_dir;
 *	uint32_t mt_up, mt_down;  // steps taken each way (status)
 *
 * cg_lc_init: c->mt_cap_us = CG_LC_MTPT_CAP_US; c->mt_dir = 1;
 * cg_lc_sent: c->mt_sent++;   cg_lc_lost: c->mt_lost++;
 * cg_lc_acked, after the estimate: cg_lc_mtpt(c, now, bytes);
 */

/* The caller's queue target, each tick: base_us is where MTPT starts, then
 * only the ceiling cap_us is the caller's (without MTPT: target = base). */
static inline void cg_lc_target_mtpt(struct cg_lc *c, uint32_t base_us, uint32_t cap_us)
{
	c->mt_cap_us = cap_us > CG_LC_MTPT_MIN_US ? cap_us : CG_LC_MTPT_MIN_US;
	c->mt_base_us = base_us > CG_LC_MTPT_MIN_US ? base_us : CG_LC_MTPT_MIN_US;
	if (c->mt_base_us > c->mt_cap_us)
		c->mt_base_us = c->mt_cap_us;
	if (!c->mt_t || c->target_us < c->mt_base_us)
		c->target_us = c->mt_base_us;
	if (c->target_us > c->mt_cap_us)
		c->target_us = c->mt_cap_us;
}

/* MTPT's ceiling for a link: the budget less how much slower it is than
 * the fastest link in the split (its rtprop over ref, the least: the SFACKs
 * come back on the fastest link, so that is the one-way difference), so
 * that what it carries arrives within the group's window; at least the
 * least target. */
static inline uint32_t cg_lc_mtpt_cap(const struct cg_lc *c, uint32_t budget_ms, uint32_t ref)
{
	uint64_t b = (uint64_t)budget_ms * 1000, lag = ref && c->rtprop > ref ? c->rtprop - ref : 0;

	return b > lag + CG_LC_MTPT_MIN_US ? (uint32_t)(b - lag) : CG_LC_MTPT_MIN_US;
}

/* MTPT at now, the receiver having said the link delivered bytes more:
 * perturb and observe, at the end of an epoch the target moves a step, or
 * holds. */
static inline void cg_lc_mtpt(struct cg_lc *c, uint64_t now, uint64_t bytes)
{
	uint64_t dt, rate;
	int64_t t;
	int steps;

	if (!c->mt_t) {
		c->mt_t = now;
		return;
	}
	c->mt_bytes += bytes;
	if (c->have_owd && c->q_us > c->mt_qmax)
		c->mt_qmax = c->q_us;
	dt = now - c->mt_t;
	if (dt < CG_LC_MTPT_EPOCH_US)
		return;
	rate = c->mt_bytes * 1000000 / dt;
	if (c->mt_sent >= CG_LC_LOSS_MIN && (uint64_t)c->mt_lost * 100 > (uint64_t)CG_LC_LOSS_PCT * c->mt_sent) {
		c->mt_dir = -1; /* too much: back two steps */
		steps = -2;
	} else if (c->mt_qmax < c->target_us / 2) {
		steps = 0; /* the target did not bind: nothing to learn */
	} else if (!c->mt_rate || rate * 100 >= c->mt_rate * (100 + CG_LC_MTPT_GAIN_PCT)) {
		steps = c->mt_dir; /* it gained: on the same way */
	} else if (rate * 100 <= c->mt_rate * (100 - CG_LC_MTPT_GAIN_PCT)) {
		c->mt_dir = (int8_t)-c->mt_dir; /* it lost: the other way */
		steps = c->mt_dir;
	} else {
		steps = 0; /* flat: it stays */
	}
	t = (int64_t)c->target_us + (int64_t)steps * CG_LC_MTPT_STEP_US;
	if (t > c->mt_cap_us) {
		t = c->mt_cap_us;
		c->mt_dir = -1;
	}
	if (t < c->mt_base_us) { /* never under the caller's target: it only explores above */
		t = c->mt_base_us;
		c->mt_dir = 1;
	}
	if (t > c->target_us)
		c->mt_up++;
	else if (t < c->target_us)
		c->mt_down++;
	c->target_us = (uint32_t)t;
	c->mt_rate = rate;
	c->mt_bytes = 0;
	c->mt_qmax = c->mt_sent = c->mt_lost = 0;
	c->mt_t = now;
}

/* The callers (client.c and server.c), each tick, with ref the least
 * rtprop of the links in the split (lc_ref) and link_cc_budget_ms (20-1000,
 * 300 by default: the user's latency budget):
 *
 *	cg_lc_target_mtpt(lc, cfg->governor_target_ms * 1000,
 *			  cg_lc_mtpt_cap(lc, cfg->link_cc_budget_ms, ref));
 *
 * and the expected arrivals (lc_eta) and the status took the link's own
 * target (lc->target_us) instead of governor_target_ms; the status said
 * mtpt_cap_ms, mtpt_up, mtpt_down. */

/* Its test (engine/tests/test_linkcc.c) */
/* One MTPT epoch on a model link: its queue reaches q_ms (binding the
 * target when at least half of it), it delivers mbit Mbit/s, and loses
 * lost of sent packets. */
static void mtpt_epoch(struct cg_lc *c, uint64_t *t, uint32_t q_ms, uint64_t mbit, uint32_t sent, uint32_t lost)
{
	for (uint32_t i = 0; i < sent; i++)
		cg_lc_sent(c, 1400);
	for (uint32_t i = 0; i < lost; i++)
		cg_lc_lost(c, 1400);
	cg_lc_owd(c, *t + 500 * MS, 1000000 + (int32_t)q_ms * (int32_t)MS);
	*t += CG_LC_MTPT_EPOCH_US;
	cg_lc_acked(c, *t, mbit * MBIT, 0, 0);
}

/* MTPT: without the bit the caller's target is the target; with it the
 * target starts there and climbs while the link delivers more, turns
 * around at the knee and stays near it, falls two steps on a lossy epoch,
 * holds while it does not bind or delivers the same, and stays between the
 * caller's target and its ceiling. */
static void test_lc_mtpt(void)
{
	struct cg_lc c;
	uint64_t t = 1000;
	uint32_t lo = UINT32_MAX, hi = 0;

	cg_lc_init(&c);
	c.bits = CG_LC_QMODE;
	cg_lc_target(&c, 60 * MS, 300 * MS);
	CHECK_EQ(c.target_us, 60 * MS);
	c.bits |= CG_LC_MTPT;
	cg_lc_target(&c, 60 * MS, 300 * MS);
	CHECK_EQ(c.target_us, 60 * MS);
	CHECK_EQ(c.mt_cap_us, 300 * MS);
	cg_lc_owd(&c, t, 1000000); /* the least one-way delay */
	cg_lc_acked(&c, t, 0, 0, 0); /* the first epoch starts */
	/* a link that gives 40 Mbit/s with 100 ms of queue or more, less with
	 * less (a modem grants more as its queue grows) */
	for (int e = 0; e < 40; e++) {
		uint64_t mbit = c.target_us >= 100 * MS ? 40 : 40 * c.target_us / (100 * MS);

		mtpt_epoch(&c, &t, c.target_us / MS, mbit, 200, 0);
		cg_lc_target(&c, 60 * MS, 300 * MS); /* the caller, each tick: the base no longer resets it */
		if (e >= 10) {
			lo = c.target_us < lo ? c.target_us : lo;
			hi = c.target_us > hi ? c.target_us : hi;
		}
	}
	CHECK(lo >= 60 * MS && hi <= 160 * MS); /* around the knee, not up to the ceiling */
	CHECK(c.mt_up > 0);
	/* an epoch losing 10 %: two steps down */
	{
		uint32_t before = c.target_us;

		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 20);
		CHECK_EQ(c.target_us, before - 2 * CG_LC_MTPT_STEP_US);
		CHECK_EQ(c.mt_dir, -1);
	}
	/* a link with less to carry than its target allows: it holds */
	{
		uint32_t before = c.target_us;

		for (int e = 0; e < 5; e++)
			mtpt_epoch(&c, &t, 5, 10, 200, 0);
		CHECK_EQ(c.target_us, before);
	}
	/* the ceiling: a slower link's (the caller's budget less its lag) */
	cg_lc_target(&c, 60 * MS, 50 * MS);
	CHECK(c.target_us <= 50 * MS);
	for (int e = 0; e < 10; e++) {
		mtpt_epoch(&c, &t, c.target_us / MS, 10 + 10 * (uint64_t)e, 200, 0); /* always more */
		cg_lc_target(&c, 60 * MS, 50 * MS);
		CHECK(c.target_us <= 50 * MS);
	}
	/* never under the caller's target, however lossy */
	cg_lc_target(&c, 60 * MS, 300 * MS);
	for (int e = 0; e < 20; e++)
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 40);
	CHECK_EQ(c.target_us, 60 * MS);
	/* flat: it stays */
	{
		uint32_t before;

		for (int e = 0; e < 3; e++)
			mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		before = c.target_us;
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		mtpt_epoch(&c, &t, c.target_us / MS, 40, 200, 0);
		CHECK_EQ(c.target_us, before);
	}
}

/* Measured (story 029, «MTPT»): v1 (down to 20 ms, down on flat) neutral up
 * and -22 % down in the field; v2 (only above governor_target_ms, still on
 * flat) -12 % down and -6 % up (field, 2026-10-29). Where it might pay: a
 * link whose knee moves a lot and a group that tolerates more queue (the
 * user's 300-500 ms budget), or as a per-link ceiling for links much
 * slower than the rest. */
