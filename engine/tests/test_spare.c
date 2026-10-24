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
	uint32_t base[3] = { 35000, 0, 30000 }, lag[3] = { 0, 0, 0 };
	int32_t q[3] = { 2000, 1000, 5000 };

	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, -1, 0), 1);
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 5, 0, 200000, -1, 0), 2);  /* link 1 not in the set */
	CHECK_EQ(cg_spare_first(base, 5, q, lag, 3, 7, 0, 200000, -1, 0), 2);  /* link 1 not measured */
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 5, 0, 200000, 0, 0), 0);   /* 5 ms worse: stays */
	base[0] = 36000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 5, 0, 200000, 0, 0), 2);   /* over the hysteresis */
	/* the least delay but a standing queue (it carries everything and
	 * cannot): out, and the caller bans it */
	q[1] = 900000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, 1, 0), 2);
	q[1] = 1000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 2, 200000, 2, 0), 2);   /* banned: still out */
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, 2, 0), 1);   /* the ban over: back */
	q[2] = -1;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 5, 0, 200000, -1, 0), 2);  /* no queue known: counts as none */
	/* none without a queue: as it was, or the least queue */
	q[0] = 300000;
	q[1] = 900000;
	q[2] = 250000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, 1, 0), 1);
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, -1, 0), 2);
	/* held (a microcut on it): stays, even out of the set */
	q[1] = 1000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 5, 0, 200000, 0, 1), 0);
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 0, 0, 200000, -1, 0), -1); /* none */
	/* choked by its base with no queue over its (climbed) minimum: out */
	base[0] = 0;
	base[1] = 600000;
	base[2] = 30000;
	q[0] = q[1] = q[2] = 0;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 6, 0, 200000, -1, 0), 2);
}

/* The current first link: held in a microcut, kept while choked for less
 * than CG_SPARE_CHOKE_MS, banned once choked that long (its queue, its
 * base or its lag), even while it loses. */
static void test_spare_keep(void)
{
	uint64_t cut = 0, ch = 0;

	CHECK(!cg_spare_choked(-1, 0, 0, 200000));
	CHECK(!cg_spare_choked(150000, 150000, 150000, 200000));
	CHECK(cg_spare_choked(250000, 0, 0, 200000));
	CHECK(cg_spare_choked(0, 670000, 0, 200000)); /* the NUC's Claro: q ~0, 670 ms behind */
	CHECK(cg_spare_choked(3000, 0, 8900000, 200000)); /* the NUC's Claro: q ~3 ms, an RTT of 8.9 s */
	CHECK(!cg_spare_choked(3000, 0, 900000, 200000)); /* 0.9 s: a download's queue in the round trip */
	CHECK(cg_spare_choked(3000, 0, CG_SPARE_LAG_US + 1, 200000));
	CHECK(cg_spare_choked(3000, 0, 2100000, 2000000) == 1); /* past a larger choke too */
	CHECK(!cg_spare_choked(3000, 0, 1900000, 2000000));
	/* a microcut (loss, no queue, its base where it was): held, and for
	 * CG_SPARE_HOLD_CUT_MS after its last losing sample */
	CHECK_EQ(cg_spare_keep(1, 0, 1, 1000, &cut, &ch), CG_SPARE_KEEP_HOLD);
	CHECK_EQ(cut, 1000);
	CHECK_EQ(cg_spare_keep(0, 0, 1, 1000 + CG_SPARE_HOLD_CUT_MS - 1, &cut, &ch), CG_SPARE_KEEP_HOLD);
	CHECK_EQ(cg_spare_keep(0, 0, 1, 1000 + CG_SPARE_HOLD_CUT_MS, &cut, &ch), CG_SPARE_KEEP_FREE);
	CHECK_EQ(cg_spare_keep(0, 0, 1, 9000, &cut, &ch), CG_SPARE_KEEP_FREE);
	/* choked and losing to its own queue: the hold cleared at once, kept
	 * (waiting) and banned once choked for CG_SPARE_CHOKE_MS */
	CHECK_EQ(cg_spare_keep(1, 0, 1, 10000, &cut, &ch), CG_SPARE_KEEP_HOLD);
	CHECK_EQ(cg_spare_keep(1, cg_spare_choked(900000, 0, 0, 200000), 1, 10100, &cut, &ch), CG_SPARE_KEEP_WAIT);
	CHECK_EQ(cut, 0);
	CHECK_EQ(ch, 10100);
	CHECK_EQ(cg_spare_keep(1, 1, 1, 10100 + CG_SPARE_CHOKE_MS - 1, &cut, &ch), CG_SPARE_KEEP_WAIT);
	CHECK_EQ(cg_spare_keep(1, 1, 1, 10100 + CG_SPARE_CHOKE_MS, &cut, &ch), CG_SPARE_KEEP_BAN);
	CHECK_EQ(ch, 0);
	CHECK_EQ(cg_spare_keep(0, 0, 1, 13000, &cut, &ch), CG_SPARE_KEEP_FREE);
	/* choked by its base with q ~0, losing: banned once it lasts */
	CHECK_EQ(cg_spare_keep(1, cg_spare_choked(0, 670000, 0, 200000), 1, 20000, &cut, &ch), CG_SPARE_KEEP_WAIT);
	CHECK_EQ(cg_spare_keep(1, cg_spare_choked(0, 670000, 0, 200000), 1, 20000 + CG_SPARE_CHOKE_MS, &cut, &ch),
		 CG_SPARE_KEEP_BAN);
	/* a jump that does not last (a radio's promotion from idle, the lab's
	 * 300 ms for 1 s): kept, never banned, and the count starts again */
	for (uint64_t t = 30000; t < 30000 + CG_SPARE_CHOKE_MS - 100; t += 100)
		CHECK_EQ(cg_spare_keep(0, cg_spare_choked(300000, 0, 0, 200000), 1, t, &cut, &ch), CG_SPARE_KEEP_WAIT);
	CHECK_EQ(cg_spare_keep(0, 0, 1, 32000, &cut, &ch), CG_SPARE_KEEP_FREE);
	CHECK_EQ(ch, 0);
	CHECK_EQ(cg_spare_keep(0, 1, 1, 32100, &cut, &ch), CG_SPARE_KEEP_WAIT);
	CHECK_EQ(cg_spare_keep(0, 1, 1, 32100 + CG_SPARE_CHOKE_MS - 1, &cut, &ch), CG_SPARE_KEEP_WAIT);
	/* under a load the others do not copy whole: banned at once, as before */
	ch = 0;
	CHECK_EQ(cg_spare_keep(0, 1, 0, 40000, &cut, &ch), CG_SPARE_KEEP_BAN);
	CHECK_EQ(ch, 0);
	CHECK_EQ(cg_spare_keep(1, 1, 0, 40100, &cut, &ch), CG_SPARE_KEEP_BAN);
	CHECK_EQ(cut, 0);
	CHECK_EQ(cg_spare_keep(0, 1, 1, 41000, &cut, &ch), CG_SPARE_KEEP_WAIT); /* quiet again: waits */
	CHECK_EQ(cg_spare_keep(0, 1, 0, 41100, &cut, &ch), CG_SPARE_KEEP_BAN);  /* loaded meanwhile */
	/* and replaced: the ban takes it out of the first link's choice */
	{
		uint32_t base[3] = { 670000, 20000, 30000 }, lag[3] = { 0, 0, 0 };
		int32_t q[3] = { 0, 1000, 5000 };

		CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 1, 200000, 0, 0), 1);
	}
}

/* A link seconds behind by its probes, its one-way measures stale and
 * clear: not the first, and among choked links the least late. */
static void test_spare_lag(void)
{
	uint32_t base[3] = { 0, 2000, 6000 }, lag[3];
	int32_t q[3] = { 3000, 1000, 5000 };

	CHECK(!cg_spare_fresh(0, 5000)); /* no report yet */
	CHECK(cg_spare_fresh(5000, 5000 + CG_SPARE_FRESH_MS));
	CHECK(!cg_spare_fresh(5000, 5001 + CG_SPARE_FRESH_MS));
	CHECK(cg_spare_fresh(5000, 5600)); /* a microcut's first second: still measured */
	CHECK_EQ(cg_spare_lag(43000, 30000), 13000);
	CHECK_EQ(cg_spare_lag(20000, 30000), 0);
	lag[0] = cg_spare_lag(8900000, 30000); /* Claro, RTT 8.9 s */
	lag[1] = cg_spare_lag(30000, 30000);
	lag[2] = cg_spare_lag(43000, 30000);
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, -1, 0), 1);
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, 0, 0), 1); /* not kept either */
	/* every one choked: cur if in the set, else the least late, not the
	 * least queue (Claro's 3 ms) */
	base[1] = base[2] = 300000;
	CHECK_EQ(cg_spare_first(base, 7, q, lag, 3, 7, 0, 200000, -1, 0), 1);
	CHECK_EQ(cg_spare_late(-1, 0, 0), 0);
	CHECK_EQ(cg_spare_late(3000, 0, 8870000), 8870000);
	CHECK_EQ(cg_spare_late(250000, 300000, 13000), 300000);
}

/* A pinned first link: it while clear, the automatic choice while it is
 * choked, banned, unmeasured or down, and back once clear; why, for the
 * status. */
static void test_spare_pinned(void)
{
	CHECK_EQ(cg_spare_pinned(-1, 2, 7, 7, 0, 0), 2);  /* none pinned */
	CHECK_EQ(cg_spare_pinned(0, 2, 7, 7, 0, 0), 0);   /* pinned, clear: stays */
	CHECK_EQ(cg_spare_pinned(0, 2, 7, 7, 0, 1), 2);   /* choked: the automatic one */
	CHECK_EQ(cg_spare_pinned(0, 2, 7, 7, 1, 0), 2);   /* banned (choked as first) */
	CHECK_EQ(cg_spare_pinned(0, 2, 6, 7, 0, 0), 2);   /* down */
	CHECK_EQ(cg_spare_pinned(0, 2, 7, 6, 0, 0), 2);   /* not measured yet */
	CHECK_EQ(cg_spare_pinned(0, 2, 7, 7, 0, 0), 0);   /* clear again: back */
	CHECK_EQ(cg_spare_pinned(20, 2, 7, 7, 0, 0), 2);  /* out of range */
	CHECK_EQ(cg_spare_pin_why(-1, 7, 7, 0, 0), CG_SPARE_PIN_NONE);
	CHECK_EQ(cg_spare_pin_why(16, 7, 7, 0, 0), CG_SPARE_PIN_ABSENT); /* named, no socket */
	CHECK_EQ(cg_spare_pin_why(3, 7, 15, 0, 0), CG_SPARE_PIN_DOWN);
	CHECK_EQ(cg_spare_pin_why(3, 15, 7, 0, 0), CG_SPARE_PIN_UNKNOWN);
	CHECK_EQ(cg_spare_pin_why(3, 15, 15, 8, 0), CG_SPARE_PIN_BANNED);
	CHECK_EQ(cg_spare_pin_why(3, 15, 15, 0, 1), CG_SPARE_PIN_CHOKED);
	CHECK_EQ(cg_spare_pin_why(3, 15, 15, 0, 0), CG_SPARE_PIN_OK);
	CHECK_EQ(cg_spare_ban_ms(1), CG_SPARE_PIN_BAN_MS);
	CHECK_EQ(cg_spare_ban_ms(0), CG_SPARE_BAN_MS);
	CHECK(CG_SPARE_PIN_BAN_MS < CG_SPARE_BAN_MS);
	/* the round trip: pinned first chokes for CG_SPARE_CHOKE_MS, is
	 * banned, the ban holds it out while clear, then it returns */
	{
		uint64_t cut = 0, ch = 0;
		uint16_t banned = 0;
		int f = 0, k;

		for (uint64_t t = 1000; !(banned & 1) && t < 1000 + 2 * CG_SPARE_CHOKE_MS; t += 100) {
			k = cg_spare_keep(1, cg_spare_choked(0, 600000, 0, 200000), 1, t, &cut, &ch);
			if (k == CG_SPARE_KEEP_BAN)
				banned |= 1;
			else
				CHECK_EQ(k, CG_SPARE_KEEP_WAIT); /* kept the first meanwhile */
		}
		CHECK(banned & 1);
		f = cg_spare_pinned(0, 1, 7, 7, banned, 1);
		CHECK_EQ(f, 1);
		CHECK_EQ(cg_spare_pinned(0, 1, 7, 7, banned, 0), 1); /* clear but banned: no flap */
		banned = 0;                                           /* CG_SPARE_PIN_BAN_MS later */
		CHECK_EQ(cg_spare_pinned(0, 1, 7, 7, banned, 0), 0);
	}
}

/* The NUC's quiet tunnel, tick by tick as client.c's spare_tick: Entel
 * (link 3) pinned, its delay jumping by 300 ms for a second every 20 s.
 * Before, the first tick of each jump banned it for a minute (the lab,
 * the same jumps: Entel the first 31 s of 90). Now it stays the first. */
static void test_spare_pin_jumps(void)
{
	uint32_t base[4] = { 0, 3000, 5000, 6000 }, lag[4] = { 0, 0, 0, 0 };
	int32_t q[4] = { 2000, 2000, 2000, 2000 };
	uint64_t cut = 0, ch = 0, ban[4] = { 0, 0, 0, 0 };
	unsigned on = 0, n = 0;
	int f = -1;

	for (uint64_t t = 100; t <= 180000; t += 100, n++) {
		uint16_t banned = 0;
		int hold = 0, k, choked;

		/* the jump: 300 ms read for ~1.3 s (the filter follows and falls back) */
		q[3] = t % 20000 >= 15000 && t % 20000 < 16300 ? 300000 : 2000;
		for (int i = 0; i < 4; i++)
			if (ban[i] && t - ban[i] < cg_spare_ban_ms(i == 3))
				banned |= (uint16_t)(1u << i);
		if (f >= 0) {
			k = cg_spare_keep(0, cg_spare_choked(q[f], base[f], lag[f], 200000), 1, t, &cut, &ch);
			if (k == CG_SPARE_KEEP_BAN) {
				ban[f] = t;
				banned |= (uint16_t)(1u << f);
			}
			hold = k == CG_SPARE_KEEP_HOLD || k == CG_SPARE_KEEP_WAIT;
		}
		k = cg_spare_first(base, 15, q, lag, 4, 15, banned, 200000, f, hold);
		choked = cg_spare_choked(q[3], base[3], lag[3], 200000);
		if (cg_spare_pin_why(3, 15, 15, banned, choked) == CG_SPARE_PIN_OK)
			k = 3;
		if (k != f)
			ch = 0;
		f = k;
		on += f == 3;
	}
	CHECK_EQ(on, n);
	/* a choke that lasts is still banned (CG_SPARE_PIN_BAN_MS), then back */
	q[3] = 300000;
	for (uint64_t t = 200000; t < 200000 + CG_SPARE_CHOKE_MS + 100; t += 100)
		if (cg_spare_keep(0, cg_spare_choked(q[3], base[3], lag[3], 200000), 1, t, &cut, &ch) == CG_SPARE_KEEP_BAN)
			ban[3] = t;
	CHECK_EQ(ban[3], 200000 + CG_SPARE_CHOKE_MS);
}

/* A link measured afresh (its socket reopened after a pause or an
 * ifdown): its allowance starts again, its counters stay. */
static void test_spare_restart(void)
{
	struct cg_spare_link l = { 0 };

	cg_spare_tick(&l, 1000, 100, 1000, 50000, 8000000, 4000000);
	cg_spare_take(&l, 1000000, 12000, 0, 0);
	CHECK(l.rate_bps);
	CHECK_EQ(l.n_take, 1);
	cg_spare_restart(&l);
	CHECK_EQ(l.rate_bps, 0);
	CHECK_EQ(l.tok, 0);
	CHECK_EQ(l.t_us, 0);
	CHECK_EQ(l.n_take, 1);
}

/* The erasure codes' plan (story 021): k from the horizon, m the fewest
 * repairs whose block fails under CG_SPARE_CODE_TAIL, copies with the whole
 * upload in the allowance or a microcut that the code would not fit. */
static void test_spare_plan(void)
{
	struct cg_spare_plan pl;
	double t = cg_spare_tail(10, 0, 0.1);

	CHECK(t > 0.6512 && t < 0.6514); /* 1 - 0.9^10 */
	CHECK(cg_spare_tail(10, 10, 0.5) == 0);
	CHECK(cg_spare_tail(20, 3, 0.01) < cg_spare_tail(20, 2, 0.01));
	/* WOM's 4.4 of a 7 Mbit/s upload, the fixed link losing 0.7 %: 729
	 * packets a second, 30 ms: k 21, p 1.4 % (twice the average) */
	pl = cg_spare_plan(CG_SPARE_RS, 7000000, 4400000, 7, 7, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_RS);
	CHECK_EQ(pl.k, 21);
	CHECK(cg_spare_tail(21, pl.m, 0.014) <= CG_SPARE_CODE_TAIL);
	CHECK(cg_spare_tail(21, pl.m - 1, 0.014) > CG_SPARE_CODE_TAIL);
	CHECK_EQ(pl.m, pl.need);
	CHECK(pl.m >= 2 && pl.m <= 5);
	/* the loss floor: nothing lost lately still plans for 0.5 % */
	pl = cg_spare_plan(CG_SPARE_RLC, 7000000, 4400000, 0, 0, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_RLC);
	CHECK(cg_spare_tail(21, pl.m, 0.005) <= CG_SPARE_CODE_TAIL);
	CHECK(pl.m >= 1);
	/* Entel's 1.3: 7/8 of it is 3 repairs per 21; the loss of 3 % asks
	 * more, so the whole allowance's, rounded up: 21 x 1.3 / 7 = 3.9, 4
	 * (the bucket trims, the allowance may grow) */
	pl = cg_spare_plan(CG_SPARE_RS, 7000000, 1300000, 30, 30, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_RS);
	CHECK(pl.need > 4);
	CHECK_EQ(pl.m, 4);
	/* WOM's allowance stuck low (the cell): 21 x 0.633 / 7 = 1.9, 2, not 1 */
	pl = cg_spare_plan(CG_SPARE_RLC, 7000000, 633000, 7, 7, 729, 30, 64, 32);
	CHECK_EQ(pl.m, 2);
	/* a microcut that does not fit: copies */
	pl = cg_spare_plan(CG_SPARE_RS, 7000000, 4400000, 600, 50, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_COPIES);
	/* 20 % that fits (k 21, 60 % of it in repairs): the code */
	pl = cg_spare_plan(CG_SPARE_RS, 7000000, 6900000, 200, 20, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_RS);
	CHECK(pl.m > 8 && pl.m <= 17);
	/* the whole upload fits: copies; a slow stream: k at least 4, m at most k */
	pl = cg_spare_plan(CG_SPARE_RS, 7000000, 7000000, 7, 7, 729, 30, 64, 32);
	CHECK_EQ(pl.how, CG_SPARE_COPIES);
	pl = cg_spare_plan(CG_SPARE_RS, 400000, 200000, 7, 7, 40, 30, 64, 32);
	CHECK_EQ(pl.k, CG_SPARE_CODE_KMIN);
	CHECK(pl.m <= pl.k);
	/* k at most kmax, m at most mmax */
	pl = cg_spare_plan(CG_SPARE_RS, 100000000, 90000000, 100, 100, 10000, 30, 64, 32);
	CHECK_EQ(pl.k, 64);
	CHECK(pl.m <= 32);
}

void test_spare(void)
{
	test_spare_plan();
	test_spare_pinned();
	test_spare_pin_jumps();
	test_spare_lag();
	test_spare_restart();
	test_spare_first();
	test_spare_keep();
	test_spare_tick();
	test_spare_take();
	test_spare_k_how();
	test_spare_loop();
	test_spare_parity();
}
