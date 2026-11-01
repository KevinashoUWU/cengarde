/* linkcc.h: each link's own rate (MPGRIT, story 029, step 2). One BBR per
 * link, as OpenMPTCProuter runs one per MPTCP subflow: in the field each of
 * its modems kept 45-60 ms of queue and gave 2-3 times the capacity the
 * engine estimated (a modem grants more as its queue grows), while one
 * controller for the mix of the links and the engine's capacity (the
 * largest rate a link delivered, cut at a queue) left them half empty.
 *
 * The receiver says, link by link, how far it got and how many bytes it
 * took (subflow.h's SFACK); the sender turns each answer into a sample:
 * the bytes delivered since the newest sseq it covers was sent, over the
 * time since (BBR's delivery rate), and that sseq's round trip.
 *
 * - The estimate bw is the largest sample over 1-2 s (two buckets) in
 *   STARTUP, past it what the link delivered over the last 0.5 s (its
 *   mean), at most 5/4 of what the link delivered while it held a queue (sat: the operator
 *   grants in bursts, and the largest sample of an uplink summed 140 Mbit/s
 *   for 86-100 delivered, field, 2026-10-28); the least round trip of 10 s
 *   is rtprop.
 * - Its queue (q_us): the one-way delay of the newest sseq an SFACK names
 *   (the receiver's clock less the send time) over its least of 15-30 s:
 *   neither bw nor the way back count in it.
 * - STARTUP sends at 3/2 bw (2 overshot) from its own first samples
 *   until bw stops growing by a quarter for three rounds (a round: a round
 *   trip, at least CG_LC_ROUND_MIN_US) or the queue passes the target, then
 *   DRAIN at half until the queue is under half the target (without one,
 *   until what is in flight fits bw x rtprop), then PROBE's cycle (5/4 for a
 *   round, 3/4, then 1 for six): over the target a link drains (3/4), and it
 *   probes only under half of it.
 * - In flight at most twice bw x (rtprop + the queue target): the target is
 *   the latency budget's share a link may hold (permissive: the user allows
 *   ~300-500 ms of round trip).
 * - A round that loses more than CG_LC_LOSS_PCT % of what it sent (with at
 *   least CG_LC_LOSS_MIN packets) is the link saying it is too much: bw
 *   falls to 7/8 of itself and STARTUP ends (BBRv2's answer to loss; the
 *   subflows' losses are each link's own, never another's lateness).
 * - The queue's uses that lost (the split's queue by the delay, the window
 *   as OMR's scheduler, MTPT's own target per link, the rate alone
 *   deciding) are in docs/desvan/, out of the engine (story 029).
 *
 * Pure: no clock, no I/O. Times in us, rates in bytes/s.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LINKCC_H
#define CG_LINKCC_H

#include <stdint.h>
#include <string.h>

#define CG_LC_ONE 256
#define CG_LC_STARTUP 384          /* 3/2 */
#define CG_LC_DRAIN 128            /* 1/2 */
#define CG_LC_CYCLE 8
#define CG_LC_BW_WIN_US 1000000u   /* a bucket of the estimate's window (two: 1-2 s) */
#define CG_LC_RTPROP_US 10000000u  /* rtprop's window */
#define CG_LC_ROUND_MIN_US 10000u  /* a round lasts a round trip, at least this */
#define CG_LC_FLOOR 125000u        /* bytes/s: 1 Mbit/s, the least rate */
#define CG_LC_MEAN_N 5u            /* CG_LC_MEAN: buckets of the delivery's mean */
#define CG_LC_MEAN_BKT_US 100000u  /* of this long each: the mean is over the last 0.5 s */
#define CG_LC_START 250000u        /* bytes/s: 2 Mbit/s before any sample */
#define CG_LC_LOSS_PCT 5u
#define CG_LC_LOSS_MIN 20u         /* packets a round needs before its loss says anything */
#define CG_LC_MIN_CWND 16000u      /* bytes in flight at least */
#define CG_LC_OWD_WIN_US 15000000u /* a bucket of the one-way delay's least (two: 15-30 s) */
#define CG_LC_SAT_US 2000000u      /* how long what a queued link delivered caps bw */
#define CG_LC_TARGET_US 60000u     /* the queue target until the caller sets one */

enum { CG_LC_STARTUP_M = 0, CG_LC_DRAIN_M, CG_LC_PROBE_M };

/* The queue's uses, each measured as a bit of link_cc_bits until it was
 * fixed (story 029, 2026-11-01; the ones that lost: docs/desvan/):
 * - The modes by the queue: STARTUP ends, DRAIN ends and PROBE drains or
 *   probes by the one-way delay; STARTUP at 3/2 (lab, 2026-10-28, n = 2, 7
 *   cases x 2 ways: +8.7 % geometric mean and 3-10 times fewer losses per
 *   link).
 * - SAT: what a queued link delivered caps bw at 5/4 (the operator grants
 *   an uplink in bursts: in the field the upload went from 78 / 59 to
 *   96 / 101 Mbit/s); with satq (the VPS's download) only while the link
 *   holds a queue, half the target or more: without it two links held
 *   513-587 ms of queue, with it always the fast link starved (60 Mbit/s
 *   where it gave 176; field-l5, 2026-10-28).
 * - The mean: past STARTUP, bw is what the link delivered over the last
 *   CG_LC_MEAN_N buckets (cg_lc_mean): while it holds a queue that is its
 *   capacity, up or down; without one, what it was sent, which may only
 *   raise bw. The largest sample of 1-2 s reads the operator's bursts:
 *   against the ceiling capture (curva1, bench/rateeval.py) the largest
 *   40 ms sample of 1.5 s read 5.2 times a saturated modem's capacity down
 *   and 3.7 up, the mean of 500 ms 0.96 and 1.00 (2026-10-31); on the
 *   testnet at night the download +25 %.
 * - The old capacity (capacity.h) no longer seeds bw: a link starts from its
 *   own STARTUP; the dominant link and FEC v3's guard and loss go by each
 *   link's own rate, queue and subflow (the callers; cg_lc_f3_queue,
 *   cg_lc_tick_loss): lab +4.1 %, FEC v3's the gain (2026-11-01). */

struct cg_lc {
	uint64_t bw;             /* the estimate, 0: none yet */
	uint64_t bmax[2], bt;    /* the window's two buckets, the current one's start */
	uint32_t rtprop, srtt;   /* us, 0: none yet */
	uint64_t rtprop_t;
	uint8_t mode, cycle, full_n;
	uint64_t full_bw;        /* STARTUP's last bw that grew by a quarter */
	uint64_t round_t;        /* the current round's start */
	uint32_t r_sent, r_lost; /* packets sent and lost in the current round */
	uint64_t inflight;       /* bytes sent, neither delivered nor lost */
	uint64_t cuts;           /* rounds that lost too much */
	/* the queue: the one-way delay's least in two buckets, the newest one */
	int32_t owd_b[2];
	uint64_t owd_t;
	uint8_t have_owd;
	uint32_t q_us;
	uint32_t target_us;      /* the queue the link may hold (set by the caller) */
	uint64_t sat, sat_t;     /* what it delivered while queued (EWMA), and when */
	uint8_t satq;            /* SAT only while queued (the VPS's download): set by the caller */
	/* the mean: bytes delivered in each bucket (mb_i the current one,
	 * started at mb_t), and how many whole buckets are behind it */
	uint64_t mb[CG_LC_MEAN_N + 1], mb_t;
	uint8_t mb_i, mb_n;
};

static inline void cg_lc_init(struct cg_lc *c)
{
	memset(c, 0, sizeof(*c));
	c->target_us = CG_LC_TARGET_US;
}

/* The caller's queue target, each tick. */
static inline void cg_lc_target(struct cg_lc *c, uint32_t target_us)
{
	c->target_us = target_us;
}

/* The estimate, bytes/s, at most 5/4 of what the link delivered lately
 * while it held a queue (with satq, only while it holds one now). */
static inline uint64_t cg_lc_bw(const struct cg_lc *c)
{
	uint64_t b = c->bw ? c->bw : CG_LC_START;

	int cap = !c->satq || (c->have_owd && c->q_us >= c->target_us / 2);

	if (cap && c->sat && c->sat * 5 / 4 < b)
		b = c->sat * 5 / 4 > CG_LC_FLOOR ? c->sat * 5 / 4 : CG_LC_FLOOR;
	return b;
}

/* Whether the link holds more than the target (1) or less than half of it
 * (-1), by its one-way delay; 0: in between, or not known. */
static inline int cg_lc_qstate(const struct cg_lc *c)
{
	if (!c->have_owd)
		return 0;
	if (c->q_us > c->target_us)
		return 1;
	return c->q_us < c->target_us / 2 ? -1 : 0;
}

static inline uint32_t cg_lc_gain(const struct cg_lc *c)
{
	static const uint16_t cyc[CG_LC_CYCLE] = { 320, 192, 256, 256, 256, 256, 256, 256 };

	uint32_t g;

	if (c->mode == CG_LC_STARTUP_M)
		return CG_LC_STARTUP;
	if (c->mode == CG_LC_DRAIN_M)
		return CG_LC_DRAIN;
	g = cyc[c->cycle % CG_LC_CYCLE];
	if (cg_lc_qstate(c) > 0) /* over the target: drain */
		return 192;
	if (g > CG_LC_ONE && cg_lc_qstate(c) == 0) /* probe only well under it */
		return CG_LC_ONE;
	return g;
}

/* The queue FEC v3's guard reads, us: the link's own (its one-way delay
 * over its least) once it has one, else the capacity's cap_q. */
static inline uint32_t cg_lc_f3_queue(const struct cg_lc *c, uint32_t cap_q)
{
	return c->have_owd ? c->q_us : cap_q;
}

/* A link's loss over the last tick, for FEC v3, per mille, from its subflow's
 * counters (sent, and lost as the receiver's SFLOSS said; *psent, *plost:
 * the last tick's, updated); -1 when nothing was sent. */
static inline int cg_lc_tick_loss(uint64_t sent, uint64_t lost, uint64_t *psent, uint64_t *plost)
{
	uint64_t ds = sent - *psent, dl = lost - *plost;

	*psent = sent;
	*plost = lost;
	if (!ds)
		return -1;
	return dl >= ds ? 1000 : (int)(dl * 1000 / ds);
}

/* The pacing rate, bytes/s. */
static inline uint64_t cg_lc_rate(const struct cg_lc *c)
{
	uint64_t r = cg_lc_bw(c) * cg_lc_gain(c) / CG_LC_ONE;

	return r > CG_LC_FLOOR ? r : CG_LC_FLOOR;
}

/* How many bytes may be in flight with a queue target of target_us. */
static inline uint64_t cg_lc_cwnd(const struct cg_lc *c, uint32_t target_us)
{
	uint64_t rt = c->rtprop ? c->rtprop : 100000, w;

	w = 2 * cg_lc_bw(c) * (rt + target_us) / 1000000;
	return w > CG_LC_MIN_CWND ? w : CG_LC_MIN_CWND;
}

/* The bytes queued on the link: what is in flight over bw x rtprop. */
static inline uint64_t cg_lc_queued(const struct cg_lc *c)
{
	uint64_t bdp = c->rtprop ? cg_lc_bw(c) * c->rtprop / 1000000 : 0;

	return c->inflight > bdp ? c->inflight - bdp : 0;
}

/* The one-way delay of a packet at now (us, the receiver's clock less the
 * sender's: a constant apart, only its changes count): the queue is how
 * far it is over its least of 15-30 s. */
static inline void cg_lc_owd(struct cg_lc *c, uint64_t now, int32_t owd)
{
	int32_t m;

	if (!c->have_owd) {
		c->owd_b[0] = c->owd_b[1] = owd;
		c->owd_t = now;
		c->have_owd = 1;
	} else if (now - c->owd_t >= CG_LC_OWD_WIN_US) {
		c->owd_b[1] = c->owd_b[0];
		c->owd_b[0] = owd;
		c->owd_t = now;
	} else if (owd < c->owd_b[0]) {
		c->owd_b[0] = owd;
	}
	m = c->owd_b[0] < c->owd_b[1] ? c->owd_b[0] : c->owd_b[1];
	c->q_us = owd > m ? (uint32_t)(owd - m) : 0;
}

/* Whether the link is short of data: less in flight than bw x rtprop, so
 * what it delivers says how much it was given, not how much it can carry
 * (BBR's app-limited; such a sample counts only if larger). */
static inline int cg_lc_idle(const struct cg_lc *c)
{
	return c->rtprop && c->bw && c->inflight < cg_lc_bw(c) * c->rtprop / 1000000;
}

/* Whether len more bytes fit in flight. */
static inline int cg_lc_room(const struct cg_lc *c, uint32_t len, uint32_t target_us)
{
	return c->inflight + len <= cg_lc_cwnd(c, target_us);
}

/* A packet of len bytes went on the link. */
static inline void cg_lc_sent(struct cg_lc *c, uint32_t len)
{
	c->inflight += len;
	c->r_sent++;
}

/* A packet of len bytes the link lost (its subflow said, subflow.h). */
static inline void cg_lc_lost(struct cg_lc *c, uint32_t len)
{
	c->inflight -= c->inflight >= len ? len : c->inflight;
	c->r_lost++;
}

/* The end of a round at now: STARTUP's exit, the loss's verdict, PROBE's
 * cycle. */
static inline void cg_lc_round(struct cg_lc *c, uint64_t now)
{
	if (c->r_sent >= CG_LC_LOSS_MIN && (uint64_t)c->r_lost * 100 > (uint64_t)CG_LC_LOSS_PCT * c->r_sent) {
		c->bw = c->bw * 7 / 8;
		if (c->bw < CG_LC_FLOOR)
			c->bw = CG_LC_FLOOR;
		c->bmax[0] = c->bmax[1] = c->bw;
		c->cuts++;
		if (c->mode == CG_LC_STARTUP_M)
			c->mode = CG_LC_DRAIN_M;
	} else if (c->mode == CG_LC_STARTUP_M) {
		if (cg_lc_qstate(c) > 0) { /* the pipe is full: its queue grows */
			c->mode = CG_LC_DRAIN_M;
		} else if (c->bw >= c->full_bw + c->full_bw / 4) {
			c->full_bw = c->bw;
			c->full_n = 0;
		} else if (++c->full_n >= 3) {
			c->mode = CG_LC_DRAIN_M;
		}
	}
	if (c->mode == CG_LC_DRAIN_M && (c->have_owd ? cg_lc_qstate(c) < 0
					   : c->inflight <= cg_lc_bw(c) * (c->rtprop ? c->rtprop : 0) / 1000000)) {
		c->mode = CG_LC_PROBE_M;
		c->cycle = 2; /* after the probe's 5/4 and its 3/4: cruise first */
	} else if (c->mode == CG_LC_PROBE_M) {
		c->cycle = (uint8_t)((c->cycle + 1) % CG_LC_CYCLE);
	}
	c->r_sent = c->r_lost = 0;
	c->round_t = now;
}

/* The mean: bytes delivered at now go into the current bucket; buckets
 * the clock passed over are empty ones. */
static inline void cg_lc_mean_add(struct cg_lc *c, uint64_t now, uint64_t bytes)
{
	if (!c->mb_t)
		c->mb_t = now;
	while (now - c->mb_t >= CG_LC_MEAN_BKT_US) {
		c->mb_i = (uint8_t)((c->mb_i + 1) % (CG_LC_MEAN_N + 1));
		c->mb[c->mb_i] = 0;
		c->mb_t += CG_LC_MEAN_BKT_US;
		if (c->mb_n < CG_LC_MEAN_N)
			c->mb_n++;
		if (now - c->mb_t > CG_LC_MEAN_N * CG_LC_MEAN_BKT_US) { /* a long silence: start over */
			memset(c->mb, 0, sizeof(c->mb));
			c->mb_t = now;
			c->mb_n = 0;
		}
	}
	c->mb[c->mb_i] += bytes;
}

/* What the link delivered over its last CG_LC_MEAN_N whole buckets,
 * bytes/s; 0 until it has them. The current one does not count: an answer
 * at its start counted over a sliver read high, and "only up" without a
 * queue kept the reading. */
static inline uint64_t cg_lc_mean(const struct cg_lc *c)
{
	uint64_t sum = 0;

	if (c->mb_n < CG_LC_MEAN_N)
		return 0;
	for (unsigned i = 0; i <= CG_LC_MEAN_N; i++)
		if (i != c->mb_i)
			sum += c->mb[i];
	return sum * 1000000 / ((uint64_t)CG_LC_MEAN_N * CG_LC_MEAN_BKT_US);
}

/* The receiver says the link delivered bytes more (since the previous
 * answer), and the newest sseq they cover gives a sample: rate (bytes/s,
 * 0: none) and its round trip rtt_us (0: none). */
static inline void cg_lc_acked(struct cg_lc *c, uint64_t now, uint64_t bytes, uint64_t rate, uint32_t rtt_us)
{
	uint32_t round_us;

	c->inflight -= c->inflight >= bytes ? bytes : c->inflight;
	if (rtt_us) {
		if (!c->rtprop || rtt_us <= c->rtprop || now - c->rtprop_t > CG_LC_RTPROP_US) {
			c->rtprop = rtt_us;
			c->rtprop_t = now;
		}
		c->srtt = c->srtt ? (uint32_t)(((uint64_t)c->srtt * 7 + rtt_us) / 8) : rtt_us;
	}
	if (now - c->bt >= CG_LC_BW_WIN_US) {
		c->bmax[1] = now - c->bt >= 2 * CG_LC_BW_WIN_US ? 0 : c->bmax[0];
		c->bmax[0] = 0;
		c->bt = now;
	}
	if (rate > c->bmax[0])
		c->bmax[0] = rate;
	/* with a queue the link delivers what it can: the cap on bw */
	if (rate && c->have_owd && c->q_us >= c->target_us / 2) {
		c->sat = c->sat && now - c->sat_t < CG_LC_SAT_US ? (c->sat * 3 + rate) / 4 : rate;
		c->sat_t = now;
	} else if (c->sat && now - c->sat_t >= CG_LC_SAT_US) {
		c->sat = 0;
	}
	cg_lc_mean_add(c, now, bytes);
	if (c->mode != CG_LC_STARTUP_M) {
		uint64_t m = cg_lc_mean(c);

		/* queued, its capacity; else what it was sent, which only raises
		 * bw; without its 0.5 s yet (after a silence), bw as it was */
		if (m && ((c->have_owd && c->q_us >= c->target_us / 2) || m > c->bw))
			c->bw = m > CG_LC_FLOOR ? m : CG_LC_FLOOR;
	} else if (c->bmax[0] || c->bmax[1]) {
		c->bw = c->bmax[0] > c->bmax[1] ? c->bmax[0] : c->bmax[1];
		if (c->bw < CG_LC_FLOOR)
			c->bw = CG_LC_FLOOR;
	}
	round_us = c->srtt > CG_LC_ROUND_MIN_US ? c->srtt : CG_LC_ROUND_MIN_US;
	if (!c->round_t)
		c->round_t = now;
	else if (now - c->round_t >= round_us)
		cg_lc_round(c, now);
}

#endif
