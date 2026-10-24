/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "fec.h"
#include "spare.h"
#include "test.h"

/* The allowance against the link's queue: down a quarter over the target,
 * at most every CG_SPARE_HOLD_MS; up a sixteenth and a step under half of
 * it while in use; within [CG_SPARE_MIN_BPS, 3/2 of the data]. */
static void test_spare_tick(void)
{
	struct cg_spare_link l;

	memset(&l, 0, sizeof(l));
	cg_spare_tick(&l, 1000, 100, -1, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 2000000); /* half the capacity to start */
	cg_spare_tick(&l, 1100, 100, 80000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1500000);
	cg_spare_tick(&l, 1200, 100, 80000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1500000); /* held: the queue is seen late */
	cg_spare_tick(&l, 1600, 100, 80000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1125000);
	CHECK_EQ(l.n_cut, 2);
	/* room, but the allowance not in use: no growth */
	cg_spare_tick(&l, 1700, 100, 10000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1125000);
	/* in use (3/4 or more of it over the tick): growth */
	l.used = 1125000 / 10;
	cg_spare_tick(&l, 1800, 100, 10000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1125000 + 1125000 / 16 + CG_SPARE_STEP);
	CHECK_EQ(l.used, 0);
	/* between half the target and the target: no change */
	l.used = l.rate_bps;
	cg_spare_tick(&l, 1900, 100, 40000, 50000, 8000000, 4000000);
	CHECK_EQ(l.rate_bps, 1125000 + 1125000 / 16 + CG_SPARE_STEP);
	/* never over 3/2 of the data rate, never under the floor */
	l.rate_bps = 20000000;
	cg_spare_tick(&l, 2000, 100, -1, 50000, 2000000, 4000000);
	CHECK_EQ(l.rate_bps, 3000000);
	l.rate_bps = CG_SPARE_MIN_BPS;
	cg_spare_tick(&l, 5000, 100, 900000, 50000, 2000000, 4000000);
	CHECK_EQ(l.rate_bps, CG_SPARE_MIN_BPS);
	/* with little data the ceiling is 4 floors */
	l.rate_bps = 2000000;
	cg_spare_tick(&l, 6000, 100, -1, 50000, 0, 0);
	CHECK_EQ(l.rate_bps, 4 * CG_SPARE_MIN_BPS);
}

/* The bucket: what it holds, a priority packet below zero by up to a
 * burst, a must packet always (held at minus a burst). */
static void test_spare_take(void)
{
	struct cg_spare_link l;
	uint64_t t = 1000000;
	int n = 0;

	memset(&l, 0, sizeof(l));
	l.rate_bps = 1200000; /* 100 packets of 1500 B a second */
	l.t_us = t;
	CHECK_EQ(cg_spare_burst(1200000), 36000);
	CHECK_EQ(cg_spare_burst(100000000), 2000000);
	CHECK(!cg_spare_take(&l, t, 12000, 0, 0)); /* empty */
	t += 10000;                                /* 12000 bits */
	CHECK(cg_spare_take(&l, t, 12000, 0, 0));
	CHECK(!cg_spare_take(&l, t, 12000, 0, 0));
	CHECK(cg_spare_take(&l, t, 12000, 1, 0)); /* priority: below zero */
	CHECK(cg_spare_take(&l, t, 12000, 1, 0));
	CHECK(cg_spare_take(&l, t, 12000, 1, 0));
	CHECK_EQ(l.tok, -36000);
	CHECK(!cg_spare_take(&l, t, 12000, 1, 0)); /* a burst down: no more */
	CHECK(cg_spare_take(&l, t, 2000, 0, 1));    /* must: always, held at minus a burst */
	CHECK_EQ(l.tok, -36000);
	/* over a second at 1.2 Mbit/s, 1500-byte packets: about 100 go */
	t += 1000000;
	l.tok = 0;
	for (int i = 0; i < 1000; i++, t += 1000)
		n += cg_spare_take(&l, t, 12000, 0, 0);
	CHECK(n >= 98 && n <= 104);
	/* idle for an hour: a burst, not an hour of tokens */
	t += 3600ull * 1000000;
	cg_spare_take(&l, t, 0, 0, 0);
	CHECK_EQ(l.tok, cg_spare_burst(l.rate_bps));
}

static void test_spare_k_how(void)
{
	CHECK_EQ(cg_spare_k(7000000, 1000000, CG_FEC_KMAX), 8);  /* 7 / (7/8) */
	CHECK_EQ(cg_spare_k(7000000, 7000000, CG_FEC_KMAX), 2);  /* room for all: still a group */
	CHECK_EQ(cg_spare_k(7000000, 200000, CG_FEC_KMAX), 15);  /* does not fit: the most */
	CHECK_EQ(cg_spare_k(7000000, 0, CG_FEC_KMAX), CG_FEC_KMAX);
	CHECK_EQ(cg_spare_k(0, 1000000, CG_FEC_KMAX), 2);
	CHECK_EQ(cg_spare_how(CG_SPARE_COPIES, 7000000, 1000000, 600), CG_SPARE_COPIES);
	CHECK_EQ(cg_spare_how(CG_SPARE_PARITY, 7000000, 4000000, 600), CG_SPARE_PARITY);
	CHECK_EQ(cg_spare_how(CG_SPARE_PARITY, 7000000, 9000000, 0), CG_SPARE_COPIES); /* room for all: copies */
	/* mix: room for all, copies; a microcut, copies; else parity */
	CHECK_EQ(cg_spare_how(CG_SPARE_MIX, 7000000, 7000000, 0), CG_SPARE_COPIES);
	CHECK_EQ(cg_spare_how(CG_SPARE_MIX, 7000000, 4000000, 7), CG_SPARE_PARITY);
	CHECK_EQ(cg_spare_how(CG_SPARE_MIX, 7000000, 4000000, CG_SPARE_MIX_PM), CG_SPARE_PARITY);
	CHECK_EQ(cg_spare_how(CG_SPARE_MIX, 7000000, 4000000, CG_SPARE_MIX_PM + 1), CG_SPARE_COPIES);
}

/* Closed loop: a link of 5 Mbit/s behind a deep queue, seen 300 ms late,
 * under 7 Mbit/s of copies offered. The allowance settles near the
 * capacity and the queue near the target, where full copies would queue
 * without end. */
static void test_spare_loop(void)
{
	struct cg_spare_link l;
	double q_bits = 0, hist[64] = { 0 }, qmax = 0, qsum = 0, rsum = 0;
	uint64_t t = 1000000;
	int n = 0;

	memset(&l, 0, sizeof(l));
	for (int k = 0; k < 600; k++) { /* 60 s of 100 ms ticks */
		int32_t seen = (int32_t)(hist[(k + 61) % 64] / 5e6 * 1e6); /* 3 ticks ago, us */

		cg_spare_tick(&l, t / 1000, 100, k < 3 ? -1 : seen, 50000, 7000000, 5000000);
		/* 100 ms of 1200-byte packets offered at 7 Mbit/s */
		for (int p = 0; p < 73; p++) {
			t += 1370;
			if (cg_spare_take(&l, t, 9600, 0, 0))
				q_bits += 9600;
		}
		q_bits -= 500000; /* drained at 5 Mbit/s */
		if (q_bits < 0)
			q_bits = 0;
		hist[k % 64] = q_bits;
		if (k >= 300) { /* the last 30 s */
			double q_ms = q_bits / 5e6 * 1000;

			qsum += q_ms;
			qmax = q_ms > qmax ? q_ms : qmax;
			rsum += (double)l.rate_bps;
			n++;
		}
	}
	CHECK(qsum / n < 100);                            /* the queue near the target, not seconds */
	CHECK(qmax < 400);
	CHECK(rsum / n > 3.0e6 && rsum / n < 6.5e6); /* the allowance near the capacity */
}

/* A parity over k packets of one link (what redund_spare sends): the
 * receiver's (fec.h) rebuilds any one of them. */
static void test_spare_parity(void)
{
	static uint32_t rseq[CG_FEC_RING];
	static uint16_t rlen[CG_FEC_RING];
	static uint8_t rhave[CG_FEC_RING], rpidx[CG_FEC_RING];
	static uint8_t rmem[CG_FEC_RING * CG_FEC_PLEN_MAX];
	static struct cg_fec_rx rx;
	static struct cg_fec_grp g;
	static uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX], p[6][CG_FEC_PLEN_MAX], out[CG_FEC_PLEN_MAX];
	uint16_t len[6];
	uint32_t seq = 0;
	uint16_t l = 0;
	unsigned plen;
	int k;

	memset(&rx, 0, sizeof(rx));
	rx.seq = rseq;
	rx.len = rlen;
	rx.have = rhave;
	rx.mem = rmem;
	rx.pidx = rpidx;
	memset(&g, 0, sizeof(g));
	for (int i = 0; i < 6; i++) {
		len[i] = (uint16_t)(100 + i * 211);
		for (int b = 0; b < len[i]; b++)
			p[i][b] = (uint8_t)(i * 31 + b);
		cg_fec_grp_add(&g, 500 + (uint32_t)i * 2, 0, p[i], len[i], 1000); /* every other: a parity's seq between */
	}
	CHECK_EQ(g.mask, 1);
	plen = cg_fec_grp_close(&g, par);
	CHECK(plen > CG_FEC_HDR);
	for (int i = 0; i < 6; i++)
		if (i != 3)
			cg_fec_rx_put(&rx, 500 + (uint32_t)i * 2, p[i], len[i]);
	k = cg_fec_rx_parity(&rx, par, plen);
	CHECK(k >= 0);
	CHECK_EQ(cg_fec_rx_try(&rx, k, out, &seq, &l), 1);
	CHECK_EQ(seq, 506);
	CHECK_EQ(l, len[3]);
	CHECK(!memcmp(out, p[3], len[3]));
}

/* The first link: least base delay among those with a rate, sticky
 * within the hysteresis. */
static void test_spare_first(void)
{
	uint32_t base[3] = { 35000, 0, 30000 };
	int32_t q[3] = { 2000, 1000, 5000 };

	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 0, 200000, -1, 0), 1);
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 5, 0, 200000, -1, 0), 2);  /* link 1 not in the set */
	CHECK_EQ(cg_spare_first(base, 5, q, 3, 7, 0, 200000, -1, 0), 2);  /* link 1 not measured */
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 5, 0, 200000, 0, 0), 0);   /* 5 ms worse: stays */
	base[0] = 36000;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 5, 0, 200000, 0, 0), 2);   /* over the hysteresis */
	/* the least delay but a standing queue (it carries everything and
	 * cannot): out, and the caller bans it */
	q[1] = 900000;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 0, 200000, 1, 0), 2);
	q[1] = 1000;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 2, 200000, 2, 0), 2);   /* banned: still out */
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 0, 200000, 2, 0), 1);   /* the ban over: back */
	q[2] = -1;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 5, 0, 200000, -1, 0), 2);  /* no queue known: counts as none */
	/* none without a queue: as it was, or the least queue */
	q[0] = 300000;
	q[1] = 900000;
	q[2] = 250000;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 0, 200000, 1, 0), 1);
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 7, 0, 200000, -1, 0), 2);
	/* held (a microcut on it): stays, even out of the set */
	q[1] = 1000;
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 5, 0, 200000, 0, 1), 0);
	CHECK_EQ(cg_spare_first(base, 7, q, 3, 0, 0, 200000, -1, 0), -1); /* none */
}

void test_spare(void)
{
	test_spare_first();
	test_spare_tick();
	test_spare_take();
	test_spare_k_how();
	test_spare_loop();
	test_spare_parity();
}
