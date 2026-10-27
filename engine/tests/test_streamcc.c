/* streamcc.h: the middle leg's rate (story 025).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "streamcc.h"
#include "test.h"

/* A path of rate bps (bytes/s), base round trip rtt_us and a drop-tail
 * queue of qmax_us, losing loss_pm per mille at random besides; the
 * controller sends segments of 1200 bytes as its pacer and window let it,
 * for secs. Returns what was delivered per second (bytes/s), the largest
 * queue in us and how much the queue overflowed. */
struct simres {
	uint64_t goodput, qmax, overflow;
	uint64_t rate_end;
};

#define SIM_Q 8192

static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

static struct simres sim(uint64_t bps, uint32_t rtt_us, uint32_t qmax_us, uint32_t loss_pm, uint32_t secs,
			 uint64_t hint, int hint_full)
{
	struct cg_scc p;
	struct cg_scc_cfg c = { .queue_us = 10000, .mss = 1200 };
	struct simres res = { 0, 0, 0, 0 };
	/* in flight: each segment's send time, arrival at the receiver (after
	 * the queue) and its snapshot */
	static struct {
		uint64_t sent, ack_at, dl, dla, dl_us, ss;
		int lost, q;
	} f[SIM_Q];
	uint32_t head = 0, n = 0, seed = 7;
	uint64_t now = 0, link_free = 0, delivered = 0, end = (uint64_t)secs * 1000000;

	cg_scc_init(&p, 0);
	cg_scc_hint(&p, hint, hint_full);
	while (now < end) {
		now += 100;
		/* ACKs (and losses, found a little after) */
		while (n && f[head].ack_at <= now) {
			if (f[head].lost) {
				p.inflight -= 1200;
				if (!f[head].q) /* as protoctl.c: sent with a queue, an overflow */
					cg_scc_lost(&p, 1200);
			} else {
				p.inflight -= 1200;
				cg_scc_ack(&p, &c, now, 1200, 1, 1, 0, f[head].sent, f[head].dl, f[head].dla, f[head].dl_us, f[head].ss);
				if (now > end / 2)
					delivered += 1200;
			}
			head = (head + 1) % SIM_Q;
			n--;
		}
		cg_scc_refill(&p, &c, now);
		while (cg_scc_may(&p, &c, 0) && n < SIM_Q) {
			uint32_t i = (head + n) % SIM_Q;
			uint64_t q = link_free > now ? link_free - now : 0, tx = 1200 * 1000000 / bps;

			f[i].sent = now;
			f[i].dl = p.dl;
			f[i].dla = p.dla;
			f[i].dl_us = p.dl_us;
			f[i].ss = p.sent;
			f[i].lost = 0;
			f[i].q = p.qnow || p.cong;
			if (q > res.qmax)
				res.qmax = q;
			if (rnd(&seed) % 1000 < loss_pm) {
				/* random loss before the queue, as netem's */
				f[i].lost = 1;
				f[i].ack_at = now + q + rtt_us + 2000;
			} else if (q > qmax_us) {
				f[i].lost = 1; /* overflow */
				res.overflow++;
				f[i].ack_at = now + rtt_us + qmax_us;
			} else {
				link_free = (link_free > now ? link_free : now) + tx;
				f[i].ack_at = link_free + rtt_us;
			}
			/* acks come back in order of ack_at: keep the ring sorted
			 * by pushing a later time when the queue reorders */
			if (n && f[(i + SIM_Q - 1) % SIM_Q].ack_at > f[i].ack_at)
				f[i].ack_at = f[(i + SIM_Q - 1) % SIM_Q].ack_at;
			n++;
			p.inflight += 1200;
			cg_scc_spend(&p, 1200);
		}
	}
	res.goodput = delivered * 2 / secs;
	res.rate_end = cg_scc_rate(&p);
	return res;
}


/* A path that changes under the stream at sw_us (the engine moves its data
 * to another link): before, a (bytes/s, one-way delay, queue), after, b,
 * with one segment in trickle (0: none) still by a; the ACKs come back in back_us and carry each segment's one-way delay (the
 * receiver's clock 5 s off ours), as protoctl.c's. Returns what was
 * delivered per second over the last half, the largest queue after the
 * switch, and the rate at the end. */
struct pathx {
	uint64_t bps;
	uint32_t owd_us, qmax_us;
};

static struct simres sim_switch(struct pathx a, struct pathx b, uint64_t sw_us, uint32_t trickle, uint32_t back_us,
				uint32_t secs, struct cg_scc *out)
{
	struct cg_scc p;
	struct cg_scc_cfg c = { .queue_us = 5000, .mss = 1200 };
	struct simres res = { 0, 0, 0, 0 };
	static struct {
		uint64_t sent, at, ack_at, dl, dla, dl_us, ss;
		int lost, q;
	} f[SIM_Q];
	uint32_t head = 0, n = 0, k = 0;
	uint64_t now = 0, free_a = 0, free_b = 0, delivered = 0, end = (uint64_t)secs * 1000000;

	cg_scc_init(&p, 0);
	while (now < end) {
		now += 100;
		while (n && f[head].ack_at <= now) {
			p.inflight -= 1200;
			if (f[head].lost) {
				if (!f[head].q)
					cg_scc_lost(&p, 1200);
			} else {
				cg_scc_owd(&p, &c, now, (int32_t)(f[head].at - f[head].sent) - 5000000);
				cg_scc_ack(&p, &c, now, 1200, 1, 1, 0, f[head].sent, f[head].dl, f[head].dla, f[head].dl_us,
					   f[head].ss);
				if (now > end / 2)
					delivered += 1200;
			}
			head = (head + 1) % SIM_Q;
			n--;
		}
		cg_scc_refill(&p, &c, now);
		while (cg_scc_may(&p, &c, 0) && n < SIM_Q) {
			uint32_t i = (head + n) % SIM_Q;
			int on_a = now < sw_us || (trickle && ++k % trickle == 0);
			const struct pathx *x = on_a ? &a : &b;
			uint64_t *lf = on_a ? &free_a : &free_b;
			uint64_t q = *lf > now ? *lf - now : 0, tx = 1200 * 1000000 / x->bps;

			f[i].sent = now;
			f[i].dl = p.dl;
			f[i].dla = p.dla;
			f[i].dl_us = p.dl_us;
			f[i].ss = p.sent;
			f[i].q = p.qnow || p.cong;
			f[i].lost = 0;
			if (!on_a && q > res.qmax)
				res.qmax = q;
			if (q > x->qmax_us) {
				f[i].lost = 1;
				res.overflow++;
				f[i].at = now + x->owd_us + x->qmax_us;
			} else {
				*lf = (*lf > now ? *lf : now) + tx;
				f[i].at = *lf + x->owd_us;
			}
			f[i].ack_at = f[i].at + back_us;
			if (n && f[(i + SIM_Q - 1) % SIM_Q].ack_at > f[i].ack_at)
				f[i].ack_at = f[(i + SIM_Q - 1) % SIM_Q].ack_at;
			n++;
			p.inflight += 1200;
			cg_scc_spend(&p, 1200);
		}
	}
	res.goodput = delivered * 2 / secs;
	res.rate_end = cg_scc_rate(&p);
	if (out)
		*out = p;
	return res;
}

void test_streamcc(void)
{
	struct cg_scc p;
	struct cg_scc_cfg c = { .queue_us = 10000, .mss = 1200 };
	struct simres r;

	/* before anything: the first rate, the window of a start */
	cg_scc_init(&p, 0);
	CHECK_EQ(cg_scc_bw(&p), CG_SCC_START);
	CHECK_EQ(cg_scc_cwnd(&p, &c, 0), CG_SCC_IW_SEGS * 1200);
	CHECK_EQ(cg_scc_rto(&p, 0, 0), 200000);
	CHECK_EQ(cg_scc_rto(&p, 10, 0), 2000000);
	/* a hint starts it there */
	cg_scc_hint(&p, 5000000, 0);
	CHECK_EQ(cg_scc_bw(&p), 5000000);
	CHECK_EQ(cg_scc_rate(&p), 5000000ull * CG_SCC_STARTUP / CG_SCC_ONE);
	/* the token bucket: empty at the start, a rate's worth after a while */
	CHECK(!cg_scc_may(&p, &c, 0));
	cg_scc_refill(&p, &c, 1000);
	CHECK(cg_scc_may(&p, &c, 0));
	cg_scc_spend(&p, 1000000);
	CHECK(!cg_scc_may(&p, &c, 0));
	CHECK(cg_scc_wait_us(&p) > 0);
	CHECK(cg_scc_mul(UINT64_MAX / 2, 4, 2) == UINT64_MAX / 2 / 2 * 4);

	/* how far duplicates widen RACK's window: the code's horizon (at most
	 * 100 ms), 20 ms of reordering without a code, never under the base */
	CHECK_EQ(cg_scc_reo_cap(10000, 0), CG_SCC_REO_NOCODE_US);
	CHECK_EQ(cg_scc_reo_cap(10000, 60000), 60000);
	CHECK_EQ(cg_scc_reo_cap(10000, 100000), 100000);
	CHECK_EQ(cg_scc_reo_cap(10000, 1000000), CG_SCC_REO_MAX_US);
	CHECK_EQ(cg_scc_reo_cap(50000, 0), 50000);
	CHECK_EQ(cg_scc_reo_cap(50000, 30000), 50000);

	/* the forward delay decides the queue: a round trip inflated by the
	 * ACK's way back (its copies on a slow link) is no queue */
	{
		struct cg_scc q;
		uint64_t t = 1000000, dl = 0;

		cg_scc_init(&q, 0);
		for (int i = 0; i < 400; i++, t += 1000) {
			/* the clocks' offset: -5 000 000 us; forward delay 10 ms flat */
			cg_scc_owd(&q, &c, t, (int32_t)(-5000000 + 10000));
			/* the round trip: 20 ms, then 60 (the ACKs late) */
			q.sent += 1200;
			cg_scc_ack(&q, &c, t, 1200, 1, 1, 0, t - (i < 100 ? 20000 : 60000), dl, dl, t - 20000, q.sent - 24000);
			dl += 1200;
		}
		CHECK(!q.cong);
		CHECK_EQ(q.n_cut, 0);
		/* now the forward delay grows by 20 ms: a queue */
		for (int i = 0; i < 100; i++, t += 1000) {
			cg_scc_owd(&q, &c, t, (int32_t)(-5000000 + 30000));
			q.sent += 1200;
			cg_scc_ack(&q, &c, t, 1200, 1, 1, 0, t - 40000, dl, dl, t - 20000, q.sent - 24000);
			dl += 1200;
		}
		CHECK(q.cong);
		CHECK(q.qnow);
		CHECK_EQ(cg_scc_gain(&q), 192); /* drains */
		/* the offset wraps the 32 bits: the same verdict */
		cg_scc_init(&q, 0);
		cg_scc_owd(&q, &c, 1, INT32_MAX - 5000);
		cg_scc_owd(&q, &c, 2, (int32_t)((uint32_t)INT32_MAX + 7000u)); /* 12 ms more, wrapped */
		CHECK(q.qnow);
		CHECK_EQ(q.owd_base, INT32_MAX - 5000);
	}
	/* idle for over a second: STARTUP again, the estimate kept */
	{
		struct cg_scc q;

		cg_scc_init(&q, 0);
		q.mode = CG_SCC_PROBE_M;
		q.bw = 5000000;
		cg_scc_restart(&q, 1000);
		CHECK_EQ(q.mode, CG_SCC_PROBE_M);
		cg_scc_restart(&q, 3000000);
		CHECK_EQ(q.mode, CG_SCC_STARTUP_M);
		CHECK_EQ(q.bw, 5000000);
		CHECK_EQ(q.n_restart, 1);
	}
	/* an idle link's capacity (its probes') is no ceiling, nor a start */
	{
		struct cg_scc q;

		cg_scc_init(&q, 0);
		cg_scc_hint(&q, 400, 1);
		CHECK(!q.hint_full);
		CHECK_EQ(cg_scc_bw(&q), CG_SCC_START);
		q.bw = 7000000;
		cg_scc_hint(&q, 2000000, 1); /* under half the estimate: stale */
		CHECK(!q.hint_full);
		CHECK_EQ(q.bw, 7000000);
		cg_scc_hint(&q, 5000000, 1);
		CHECK(q.hint_full);
		CHECK_EQ(q.bw, 5000000);
	}
	/* a burst of SACKs after a hole fills: the sample is over the
	 * segment's own flight, not the short ack interval (story 025: 300 to
	 * 900 Mbit/s read on a 60 Mbit/s link) */
	{
		struct cg_scc q;

		cg_scc_init(&q, 0);
		q.mode = CG_SCC_PROBE_M;
		q.sent = 200000;
		/* 120 000 bytes delivered 1 ms after the delivery before its send,
		 * the segment out for 20 ms, 120 000 sent meanwhile: 6 MB/s, not
		 * 120 */
		cg_scc_ack(&q, &c, 1000000, 120000, 1, 1, 0, 980000, 0, 0, 999000, 80000);
		CHECK(q.bw <= 6000000);
		CHECK(q.dmax[0] <= 6000000);
	}
	/* heavy random loss: more than 1 byte in 5 of a window declared lost
	 * while the queue was short */
	{
		struct cg_scc q;

		cg_scc_init(&q, 0);
		cg_scc_lost(&q, 30000);
		cg_scc_ack(&q, &c, 50000, 70000, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(!q.heavy); /* the window not over yet */
		cg_scc_ack(&q, &c, CG_SCC_LOSSW_US + 1, 1000, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(q.heavy);  /* 30 000 lost of 100 000 */
		cg_scc_lost(&q, 10000);
		cg_scc_ack(&q, &c, 2 * CG_SCC_LOSSW_US + 2, 89000, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(q.heavy); /* that window: 10 000 of 11 000, the ACK after it */
		cg_scc_ack(&q, &c, 3 * CG_SCC_LOSSW_US + 3, 1000, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(!q.heavy); /* 89 000 delivered, none lost */
		cg_scc_ack(&q, &c, 4 * CG_SCC_LOSSW_US + 4, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(!q.heavy); /* few bytes and none lost: none */
		CHECK_EQ(q.n_heavy, 2);
	}
	/* RACK's window widens the window: what is lost but not declared */
	cg_scc_init(&p, 0);
	p.min_rtt = 20000;
	p.bw = 1000000;
	CHECK_EQ(cg_scc_cwnd(&p, &c, 100000), cg_scc_cwnd(&p, &c, 0)); /* not in STARTUP */
	p.mode = CG_SCC_PROBE_M;
	CHECK_EQ(cg_scc_cwnd(&p, &c, 100000) - cg_scc_cwnd(&p, &c, 0), 100000);

	/* 60 Mbit/s, 20 ms, 15 ms of queue: fills it, the queue stays short */
	r = sim(7500000, 20000, 15000, 0, 8, 0, 0);
	CHECK(r.goodput > 7500000 * 9 / 10);
	CHECK(r.overflow < 200);
	/* a queue shorter than queue_us (Starlink's): it overflows before any
	 * sample says queue; its losses are not capacity beyond the path's
	 * random loss (before: the estimate went to 240-320 Mbit/s, 100 000
	 * overflowed in 8 s; story 025) */
	for (uint32_t qq = 2000; qq <= 9000; qq += 3500) {
		r = sim(7500000, 20000, qq, 0, 8, 0, 0);
		CHECK(r.goodput > 7500000 * 9 / 10);
		CHECK(r.rate_end < 7500000 * 3 / 2);
		CHECK(r.overflow < 5000);
		r = sim(7500000, 20000, qq, 20, 8, 0, 0); /* and 2 % at random */
		CHECK(r.goodput > 7500000 * 85 / 100);
		CHECK(r.rate_end < 7500000 * 3 / 2);
	}
	/* random loss of 0.7 % does not slow it */
	r = sim(7500000, 20000, 15000, 7, 8, 0, 0);
	CHECK(r.goodput > 7500000 * 85 / 100);
	/* 60 % lost (the microcut): it keeps sending at the link's rate (what
	 * arrives is 40 % of it; the ARQ sends the rest again) */
	r = sim(7500000, 20000, 15000, 600, 8, 0, 0);
	CHECK(r.goodput > 7500000 * 30 / 100);
	CHECK(r.rate_end > 7500000 / 2);
	/* the engine says the link is queued at 2 MB/s: that is the ceiling */
	r = sim(7500000, 20000, 15000, 0, 4, 2000000, 1);
	CHECK(r.rate_end <= 2000000ull * 320 / 256 + 1);

	/* the engine moves the stream to a link 15 ms longer (GRIT's download
	 * collapse, story 025, 2026-10-27: the first bytes by the Starlink-like
	 * link set the base, the rest went by the cellular one): a queue the
	 * cuts do not drain is not the stream's; the base moves up and the
	 * stream fills the new link. Before, it sat at the floor (0.5 Mbit/s)
	 * until the base expired, 10 s later */
	{
		struct pathx sl = { 7500000, 10000, 15000 }, cell = { 625000, 25000, 2000000 };
		struct cg_scc q;

		r = sim_switch(sl, cell, 50000, 0, 10000, 4, &q);
		CHECK(r.goodput > 625000 * 3 / 4); /* before: 60 000, the floor */
		CHECK(r.qmax < 300000); /* STARTUP's overshoot, drained: never the 2 s */
		CHECK(q.n_rebase >= 1);
		/* a packet now and then still by the shorter link (one in 40 to
		 * 160): the base falls to it only when windows in a row find it, or
		 * each one set it back and the stream's own delay read as a queue
		 * again (before: 17 moves of the base, a quarter of the link) */
		for (uint32_t t = 40; t <= 160; t *= 2) {
			r = sim_switch(sl, cell, 50000, t, 10000, 6, &q);
			CHECK(r.goodput > 625000 * 85 / 100);
			CHECK(q.n_rebase <= 2);
		}
		/* only the delay changes (another link as fast): the same */
		cell.bps = 7500000;
		cell.qmax_us = 15000;
		r = sim_switch(sl, cell, 50000, 0, 10000, 4, &q);
		CHECK(r.goodput > 7500000 * 85 / 100);
		CHECK(q.n_rebase >= 1);
		/* the stream's own queue (a deep one, STARTUP's overshoot) drains
		 * under the cuts: no move of the base */
		cell = (struct pathx){ 625000, 25000, 2000000 };
		r = sim_switch(cell, cell, 0, 0, 10000, 6, &q);
		CHECK(r.goodput > 625000 * 85 / 100);
		CHECK(r.qmax < 300000);
		CHECK_EQ(q.n_rebase, 0);
		/* the shorter link after the longer: the base falls at once (the
		 * rate grows by PROBE's cycle) */
		r = sim_switch((struct pathx){ 625000, 25000, 2000000 }, sl, 1000000, 0, 10000, 4, &q);
		CHECK(r.goodput > 625000 * 2);
		CHECK_EQ(q.n_rebase, 0);
		CHECK_EQ(q.n_cut, 0);
	}
	/* the run's verdict alone: a queue that falls is the stream's, one that
	 * stays for CG_SCC_STUCK_ROUNDS round trips and 4 times its depth is
	 * not; the base goes to the run's least and STARTUP again */
	{
		struct cg_scc q;
		struct cg_scc_cfg c5 = { .queue_us = 5000, .mss = 1200 };
		uint64_t t = 1000000, hold = CG_SCC_STUCK_ROUNDS * 20000 + 4 * 17000;

		cg_scc_init(&q, 0);
		q.mode = CG_SCC_PROBE_M;
		q.min_rtt = 20000;
		q.owd_base = 10000;
		q.bw = 62500;
		CHECK(!cg_scc_stuck(&q, &c5, t, 17000));              /* the run begins */
		CHECK(!cg_scc_stuck(&q, &c5, t + hold / 2, 17500));   /* not yet */
		CHECK(!cg_scc_stuck(&q, &c5, t + hold - 1, 16800));   /* a fall under the tolerance: not yet */
		CHECK(cg_scc_stuck(&q, &c5, t + hold, 17200));         /* stuck */
		CHECK_EQ(q.owd_base, 10000 + 16800);                   /* the run's least */
		CHECK_EQ(q.mode, CG_SCC_STARTUP_M);
		CHECK_EQ(q.bw, 62500);                                 /* the estimate kept */
		CHECK_EQ(q.n_rebase, 1);
		CHECK(!q.cong);
		/* a queue that falls restarts the clock */
		cg_scc_init(&q, 0);
		q.min_rtt = 20000;
		q.owd_base = 10000;
		CHECK(!cg_scc_stuck(&q, &c5, t, 40000));
		CHECK(!cg_scc_stuck(&q, &c5, t + hold, 38000));        /* fell 2 ms: draining */
		CHECK(!cg_scc_stuck(&q, &c5, t + hold + hold / 2, 37000));
		CHECK_EQ(q.n_rebase, 0);
		/* a shorter path: one packet by it does not lower the base, two
		 * windows in a row do (to the larger of their least) */
		cg_scc_init(&q, 0);
		cg_scc_owd(&q, &c5, t, 27000);
		cg_scc_ack(&q, &c5, t, 1200, 1, 1, 0, t - 20000, 0, 0, t - 20000, 0);
		cg_scc_owd(&q, &c5, t + 30000, 10000);
		CHECK_EQ(q.owd_base, 27000);
		cg_scc_ack(&q, &c5, t + 30000, 1200, 1, 1, 0, t + 10000, 0, 0, t + 10000, 0);
		CHECK_EQ(q.owd_base, 27000);
		cg_scc_owd(&q, &c5, t + 60000, 27000);
		cg_scc_ack(&q, &c5, t + 60000, 1200, 1, 1, 0, t + 40000, 0, 0, t + 40000, 0);
		cg_scc_owd(&q, &c5, t + 90000, 10000);
		cg_scc_ack(&q, &c5, t + 90000, 1200, 1, 1, 0, t + 70000, 0, 0, t + 70000, 0);
		CHECK_EQ(q.owd_base, 27000);
		cg_scc_owd(&q, &c5, t + 120000, 10100);
		cg_scc_ack(&q, &c5, t + 120000, 1200, 1, 1, 0, t + 100000, 0, 0, t + 100000, 0);
		CHECK_EQ(q.owd_base, 10100);
		/* a small fall (within queue_us) lowers it at once */
		cg_scc_owd(&q, &c5, t + 130000, 8000);
		CHECK_EQ(q.owd_base, 8000);
		/* the offset wraps the 32 bits */
		cg_scc_init(&q, 0);
		q.owd_base = INT32_MAX - 5000;
		CHECK(!cg_scc_stuck(&q, &c5, t, 15000));
		CHECK(cg_scc_stuck(&q, &c5, t + 10 * hold, 15000));
		CHECK_EQ(q.owd_base, (int32_t)((uint32_t)INT32_MAX + 10000u));
	}
}
