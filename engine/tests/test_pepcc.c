/* pepcc.h: the middle leg's rate (story 025).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "pepcc.h"
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
	struct cg_pcc p;
	struct cg_pcc_cfg c = { .queue_us = 10000, .mss = 1200 };
	struct simres res = { 0, 0, 0, 0 };
	/* in flight: each segment's send time, arrival at the receiver (after
	 * the queue) and its snapshot */
	static struct {
		uint64_t sent, ack_at, dl, dla, dl_us;
		int lost, q;
	} f[SIM_Q];
	uint32_t head = 0, n = 0, seed = 7;
	uint64_t now = 0, link_free = 0, delivered = 0, end = (uint64_t)secs * 1000000;

	cg_pcc_init(&p, 0);
	cg_pcc_hint(&p, hint, hint_full);
	while (now < end) {
		now += 100;
		/* ACKs (and losses, found a little after) */
		while (n && f[head].ack_at <= now) {
			if (f[head].lost) {
				p.inflight -= 1200;
				if (!f[head].q) /* as pep.c: sent with a queue, an overflow */
					cg_pcc_lost(&p, 1200);
			} else {
				p.inflight -= 1200;
				cg_pcc_ack(&p, &c, now, 1200, 1, 1, 0, f[head].sent, f[head].dl, f[head].dla, f[head].dl_us);
				if (now > end / 2)
					delivered += 1200;
			}
			head = (head + 1) % SIM_Q;
			n--;
		}
		cg_pcc_refill(&p, &c, now);
		while (cg_pcc_may(&p, &c) && n < SIM_Q) {
			uint32_t i = (head + n) % SIM_Q;
			uint64_t q = link_free > now ? link_free - now : 0, tx = 1200 * 1000000 / bps;

			f[i].sent = now;
			f[i].dl = p.dl;
			f[i].dla = p.dla;
			f[i].dl_us = p.dl_us;
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
			cg_pcc_spend(&p, 1200);
		}
	}
	res.goodput = delivered * 2 / secs;
	res.rate_end = cg_pcc_rate(&p);
	return res;
}

void test_pepcc(void)
{
	struct cg_pcc p;
	struct cg_pcc_cfg c = { .queue_us = 10000, .mss = 1200 };
	struct simres r;

	/* before anything: the first rate, the window of a start */
	cg_pcc_init(&p, 0);
	CHECK_EQ(cg_pcc_bw(&p), CG_PCC_START);
	CHECK_EQ(cg_pcc_cwnd(&p, &c), CG_PCC_IW_SEGS * 1200);
	CHECK_EQ(cg_pcc_rto(&p, 0, 0), 200000);
	CHECK_EQ(cg_pcc_rto(&p, 10, 0), 2000000);
	/* a hint starts it there */
	cg_pcc_hint(&p, 5000000, 0);
	CHECK_EQ(cg_pcc_bw(&p), 5000000);
	CHECK_EQ(cg_pcc_rate(&p), 5000000ull * CG_PCC_STARTUP / CG_PCC_ONE);
	/* the token bucket: empty at the start, a rate's worth after a while */
	CHECK(!cg_pcc_may(&p, &c));
	cg_pcc_refill(&p, &c, 1000);
	CHECK(cg_pcc_may(&p, &c));
	cg_pcc_spend(&p, 1000000);
	CHECK(!cg_pcc_may(&p, &c));
	CHECK(cg_pcc_wait_us(&p) > 0);
	CHECK(cg_pcc_mul(UINT64_MAX / 2, 4, 2) == UINT64_MAX / 2 / 2 * 4);

	/* 60 Mbit/s, 20 ms, 15 ms of queue: fills it, the queue stays short */
	r = sim(7500000, 20000, 15000, 0, 8, 0, 0);
	CHECK(r.goodput > 7500000 * 9 / 10);
	CHECK(r.overflow < 200);
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
}
