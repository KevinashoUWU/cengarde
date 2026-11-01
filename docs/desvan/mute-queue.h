/* docs/desvan/mute-queue.h: mute_queue: the mute guard compares a link's lateness (its own queue over the fastest's, its unanswered probes), not only its delay behind the fastest
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/health.h, client.c, server.c, engine.h until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* From health.h's header:
 *
 * - With queue_mode (mute_queue, story 016), what a link is compared with
 *   is its lateness (cg_health_late), not only how far behind the fastest
 *   active link it is: also its own queue over the fastest one's (the
 *   shared queue sensor, one-way delay over its own minimum: a link with a
 *   short base delay and a long queue), and how long its oldest unanswered
 *   probe has been out over the fastest round trip (a lower bound on how
 *   late it is while its replies do not come). A link whose report goes
 *   stale while that bound says it is late keeps holding its mute instead
 *   of starting it over: on the virtual cell an LTE link with 1-4 s of
 *   queue answered in bursts, and every gap between them reset its 2 s.
 *   The queue and the silence are inputs set by the caller each tick
 *   (queue_us, silent_us); unmuting asks the same lateness to be back under
 *   unmute_behind_us.
 *
 * Its fields: struct cg_hcfg had
 *	uint8_t queue_mode;        // mute_queue: compare cg_health_late, not behind_us alone
 * struct cg_hlink had
 *	// queue_mode: inputs, set by the caller before each evaluation, and
 *	// the lateness the guard last compared (have_late)
 *	int32_t queue_us;  // its own queue (one-way delay over its own minimum), -1: none
 *	uint32_t silent_us; // its oldest unanswered probe's age over the fastest round trip, 0: none
 *	int32_t late_us;
 *	uint8_t have_late;
 * and cg_health_reset set h->queue_us = -1.
 */

/* How late a link is for the mute guard with queue_mode: the largest of
 * what is known of it. have_behind, behind_us: its delay behind the
 * fastest active link (a fresh report); q_us, q_best_us: its queue and the
 * fastest one's (-1: none), the difference counting only when both are
 * known (a queue the fastest has too makes no copy later than the
 * fastest's); silent_us: the bound from its unanswered probes. *have 0
 * when none of them is known (then 0). */
static inline int32_t cg_health_late(int have_behind, int32_t behind_us, int32_t q_us, int32_t q_best_us,
				     uint32_t silent_us, int *have)
{
	int64_t late = 0;

	*have = 0;
	if (have_behind) {
		late = behind_us;
		*have = 1;
	}
	if (q_us >= 0 && q_best_us >= 0 && (!*have || (int64_t)q_us - q_best_us > late)) {
		late = (int64_t)q_us - q_best_us;
		*have = 1;
	}
	if (silent_us && (!*have || (int64_t)silent_us > late)) {
		late = silent_us;
		*have = 1;
	}
	return late > INT32_MAX ? INT32_MAX : (int32_t)late;
}

/* The bound from a link's unanswered probes (client): its oldest
 * unanswered probe has been out for now_ms - first_ms, so its round trip
 * is at least that, and it is at least that minus ref_rtt_ms (the fastest
 * link's round trip) later than the fastest. 0 with no probe unanswered or
 * a shorter wait. */
static inline uint32_t cg_health_silent_us(uint32_t unanswered, uint64_t first_ms, uint64_t now_ms,
					   uint32_t ref_rtt_ms)
{
	uint64_t age;

	if (!unanswered || now_ms <= first_ms)
		return 0;
	age = now_ms - first_ms;
	if (age <= ref_rtt_ms)
		return 0;
	age = (age - ref_rtt_ms) * 1000;
	return age > INT32_MAX ? INT32_MAX : (uint32_t)age;
}

/* What the guard compares a link by: its lateness with queue_mode (when
 * known), else its delay behind the fastest. */
static inline int32_t cg_health_cmp_late(const struct cg_hlink *h, const struct cg_hcfg *c)
{
	return c->queue_mode && h->have_late ? h->late_us : h->behind_us;
}

/* In cg_health_eval, per link (fresh: a fresh report and a fastest link):
 *
 *	h[i].have_late = 0;
 *	if (!fresh) {
 *		h[i].have_behind = 0;
 *		// queue_mode: a stale report with its probes unanswered for
 *		// longer than the threshold is a late link, not an unknown one
 *		if (!c->queue_mode || best < 0 || h[i].state != CG_H_ACTIVE || !c->mute_behind_us ||
 *		    h[i].silent_us <= c->mute_behind_us) {
 *			h[i].holding = 0;
 *			continue;
 *		}
 *	} else { ...behind_us... }
 *	late = h[i].behind_us;
 *	if (c->queue_mode) {
 *		late = cg_health_late(h[i].have_behind, h[i].behind_us, h[i].queue_us, h[best].queue_us,
 *				      h[i].silent_us, &have);
 *		h[i].late_us = late;
 *		h[i].have_late = (uint8_t)have;
 *	}
 *	// cond compares late (mute: > mute_behind_us; unmute: <= unmute_behind_us)
 *
 * and the mute order took the furthest by cg_health_cmp_late.
 *
 * engine.h's cg_hcfg_from: .queue_mode = (uint8_t)!!cfg->mute_queue
 * The server, before cg_health_eval, its input (no probes of its own):
 *	for (int p = 0; p < CG_MAX_LINKS; p++)
 *		S->dh[p].queue_us = S->path[p].used ? S->cap[p].ql_us : -1;
 * The status said upload_late_ms / download_late_ms (have_late), the log and
 * decrec.h's mute record the lateness when known. The client's inputs: */

/* mute_queue's inputs (health.h): each link's queue over its minute's
 * minimum (capacity.h, the last tick's) and the bound from its unanswered
 * probes over the fastest live link's round trip. */
static void health_inputs(struct client *c, uint16_t live, uint64_t now_ms)
{
	uint32_t ref = 0;
	int any = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((live >> i & 1) && c->link[i].srtt8_us && (!any || srtt_ms(&c->link[i]) < ref)) {
			ref = srtt_ms(&c->link[i]);
			any = 1;
		}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];

		c->uh[i].queue_us = l->fd >= 0 ? c->cap[i].ql_us : -1;
		c->uh[i].silent_us = l->fd >= 0 && any ? cg_health_silent_us(l->unanswered, l->first_unanswered_ms, now_ms, ref) : 0;
	}
}

/* Its tests (engine/tests/test_health.c) */
/* mute_queue (story 016): the lateness the guard compares. */
static void test_late(void)
{
	int have;

	/* nothing known */
	CHECK_EQ(cg_health_late(0, 0, -1, -1, 0, &have), 0);
	CHECK(!have);
	/* behind alone, or the largest of what is known */
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, -1, 0, &have), 40 * MS);
	CHECK(have);
	CHECK_EQ(cg_health_late(1, -5 * MS, -1, -1, 0, &have), -5 * (int32_t)MS); /* ahead */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, 20 * MS, 0, &have), 280 * MS);
	CHECK_EQ(cg_health_late(1, 400 * MS, 300 * MS, 20 * MS, 0, &have), 400 * MS);
	/* a queue the fastest has as well makes no copy later */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, 300 * MS, 0, &have), 40 * MS);
	/* one queue unknown: not counted */
	CHECK_EQ(cg_health_late(1, 40 * MS, 300 * MS, -1, 0, &have), 40 * MS);
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, 0, 0, &have), 40 * MS);
	/* the unanswered probes' bound, also without a fresh report */
	CHECK_EQ(cg_health_late(1, 40 * MS, -1, -1, 700 * MS, &have), 700 * MS);
	CHECK_EQ(cg_health_late(0, 0, -1, -1, 700 * MS, &have), 700 * MS);
	CHECK(have);

	/* the bound: the oldest unanswered probe's age over the fastest RTT */
	CHECK_EQ(cg_health_silent_us(0, 1000, 5000, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1020, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1030, 30), 0);
	CHECK_EQ(cg_health_silent_us(3, 1000, 1530, 30), 500 * MS);
	CHECK_EQ(cg_health_silent_us(3, 2000, 1000, 30), 0); /* not before it was sent */
}

/* mute_queue in an evaluation: a link 100 ms behind the fastest (under
 * mute_behind_us) whose own queue is 300 ms over the fastest's is muted;
 * without the mode it is not. Back under unmute_behind_us of lateness (the
 * queue gone), it comes back after 2 x settle. */
static void test_queue_mode(void)
{
	struct cg_hlink h[3];
	struct cg_hcfg q = cfg;
	uint32_t owd[3] = { 50 * MS, 60 * MS, 150 * MS };

	q.min_active = 1;
	q.queue_mode = 1;
	init(h, 3);
	h[0].queue_us = 0;
	h[1].queue_us = 5 * MS;
	h[2].queue_us = 300 * MS;
	run(h, 3, owd, 7, 0, 3000, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	CHECK(h[2].behind_us == 100 * MS);
	CHECK(h[2].have_late && h[2].late_us == 300 * MS);
	CHECK_EQ(h[1].state, CG_H_ACTIVE);

	/* the same without the mode: 100 ms behind is not enough */
	{
		struct cg_hcfg o = q;

		o.queue_mode = 0;
		init(h, 3);
		h[0].queue_us = 0;
		h[2].queue_us = 300 * MS;
		run(h, 3, owd, 7, 0, 10000, &o);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
		CHECK(!h[2].have_late);
	}

	/* muted, its queue drains: lateness 100 ms behind, under unmute 120 */
	init(h, 3);
	h[0].queue_us = 0;
	h[2].queue_us = 300 * MS;
	run(h, 3, owd, 7, 0, 3000, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED);
	h[2].queue_us = 0;
	run(h, 3, owd, 7, 3100, 6900, &q);
	CHECK_EQ(h[2].state, CG_H_MUTED); /* 2 x settle not yet */
	run(h, 3, owd, 7, 7000, 7200, &q);
	CHECK_EQ(h[2].state, CG_H_ACTIVE);

	/* muting off: the mode mutes nothing */
	{
		struct cg_hcfg o = q;

		o.mute_behind_us = o.unmute_behind_us = 0;
		init(h, 3);
		h[0].queue_us = 0;
		h[2].queue_us = 3000 * MS;
		h[2].silent_us = 3000 * MS;
		run(h, 3, owd, 7, 0, 10000, &o);
		CHECK_EQ(h[2].state, CG_H_ACTIVE);
	}
}

/* mute_queue: a late link whose reports come in bursts (its replies sit in
 * a queue of seconds) keeps holding its mute through the gaps, as long as
 * its unanswered probes say it is late; without the mode each gap starts it
 * over and it is never muted (story 016, the virtual cell). */
static void test_queue_mode_gaps(void)
{
	struct cg_hlink h[2];
	struct cg_hcfg q = cfg;
	uint32_t owd[2] = { 30 * MS, 1030 * MS }, gap[2] = { 30 * MS, UINT32_MAX };

	q.min_active = 1;
	for (int mode = 0; mode < 2; mode++) {
		q.queue_mode = (uint8_t)mode;
		init(h, 2);
		/* a report from link 1 valid 500 ms every 1.5 s, its oldest
		 * probe unanswered for up to a second meanwhile */
		for (uint64_t t = 0; t <= 6000; t += 100) {
			int rep = t % 1500 == 0;

			h[1].silent_us = rep ? 0 : (uint32_t)(t % 1500) * MS + 200 * MS;
			for (int i = 0; i < 2; i++) {
				uint32_t v = rep ? owd[i] : gap[i];

				if (v != UINT32_MAX) {
					h[i].have_owd = 0;
					cg_health_report(&h[i], v, t, 500);
				}
			}
			cg_health_eval(h, 2, 3, t, &q);
		}
		if (mode)
			CHECK_EQ(h[1].state, CG_H_MUTED);
		else
			CHECK_EQ(h[1].state, CG_H_ACTIVE);
	}

	/* a short silence (a radio hiccup) is under the threshold: no hold */
	q.queue_mode = 1;
	init(h, 2);
	{
		uint32_t ok[2] = { 30 * MS, 40 * MS };

		run(h, 2, ok, 3, 0, 1000, &q);
		h[1].silent_us = 100 * MS;
		for (uint64_t t = 1100; t <= 4000; t += 100)
			step(h, 2, gap, 3, t, &q);
		CHECK_EQ(h[1].state, CG_H_ACTIVE);
		CHECK(!h[1].holding);
	}
}

/* Measured (story 016, 2026-10-22): on the virtual LTE cell it muted the
 * LTE at 3.3 s always (without it 3.8-7.9 s or never), but the
 * redundancy's TCP did not gain (the fixed link's 1 % loss limits it; the
 * rescue repairs that, not muting), and muting more costs more on the way
 * back (2 x mute_settle_ms with the backoff: 6-9 s without the LTE once
 * good). Lab: no case changed (0.96-1.06x, p >= 0.2). Where it fits: a
 * redundancy that relies on mute_behind_ms, where it is the right fix for
 * the stale-report gap. */
