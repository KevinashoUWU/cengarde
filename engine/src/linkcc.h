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
 * - The estimate bw is the largest sample over 1-2 s (two buckets); the
 *   least round trip of 10 s is rtprop.
 * - STARTUP sends at twice bw until bw stops growing by a quarter for three
 *   rounds (a round: a round trip, at least CG_LC_ROUND_MIN_US), then DRAIN
 *   at half until what is in flight fits bw x rtprop, then PROBE's cycle
 *   (5/4 for a round, 3/4, then 1 for six).
 * - In flight at most twice bw x (rtprop + the queue target): the target is
 *   the latency budget's share a link may hold (permissive: the user allows
 *   ~300-500 ms of round trip).
 * - A round that loses more than CG_LC_LOSS_PCT % of what it sent (with at
 *   least CG_LC_LOSS_MIN packets) is the link saying it is too much: bw
 *   falls to 7/8 of itself and STARTUP ends (BBRv2's answer to loss; the
 *   subflows' losses are each link's own, never another's lateness).
 *
 * Pure: no clock, no I/O. Times in us, rates in bytes/s.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LINKCC_H
#define CG_LINKCC_H

#include <stdint.h>
#include <string.h>

#define CG_LC_ONE 256
#define CG_LC_STARTUP 512          /* 2 */
#define CG_LC_DRAIN 128            /* 1/2 */
#define CG_LC_CYCLE 8
#define CG_LC_BW_WIN_US 1000000u   /* a bucket of the estimate's window (two: 1-2 s) */
#define CG_LC_RTPROP_US 10000000u  /* rtprop's window */
#define CG_LC_ROUND_MIN_US 10000u  /* a round lasts a round trip, at least this */
#define CG_LC_FLOOR 125000u        /* bytes/s: 1 Mbit/s, the least rate */
#define CG_LC_START 250000u        /* bytes/s: 2 Mbit/s before any sample, unless seeded */
#define CG_LC_LOSS_PCT 5u
#define CG_LC_LOSS_MIN 20u         /* packets a round needs before its loss says anything */
#define CG_LC_MIN_CWND 16000u      /* bytes in flight at least */

enum { CG_LC_STARTUP_M = 0, CG_LC_DRAIN_M, CG_LC_PROBE_M };

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
};

static inline void cg_lc_init(struct cg_lc *c)
{
	memset(c, 0, sizeof(*c));
}

static inline uint64_t cg_lc_bw(const struct cg_lc *c)
{
	return c->bw ? c->bw : CG_LC_START;
}

/* The engine's capacity for the link (capacity.h), as a start: the estimate
 * begins there instead of from nothing. */
static inline void cg_lc_seed(struct cg_lc *c, uint64_t bps)
{
	if (!c->bw && bps > CG_LC_FLOOR)
		c->bw = c->bmax[0] = bps;
}

static inline uint32_t cg_lc_gain(const struct cg_lc *c)
{
	static const uint16_t cyc[CG_LC_CYCLE] = { 320, 192, 256, 256, 256, 256, 256, 256 };

	if (c->mode == CG_LC_STARTUP_M)
		return CG_LC_STARTUP;
	if (c->mode == CG_LC_DRAIN_M)
		return CG_LC_DRAIN;
	return cyc[c->cycle % CG_LC_CYCLE];
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
		if (c->bw >= c->full_bw + c->full_bw / 4) {
			c->full_bw = c->bw;
			c->full_n = 0;
		} else if (++c->full_n >= 3) {
			c->mode = CG_LC_DRAIN_M;
		}
	}
	if (c->mode == CG_LC_DRAIN_M && c->inflight <= cg_lc_bw(c) * (c->rtprop ? c->rtprop : 0) / 1000000) {
		c->mode = CG_LC_PROBE_M;
		c->cycle = 2; /* after the probe's 5/4 and its 3/4: cruise first */
	} else if (c->mode == CG_LC_PROBE_M) {
		c->cycle = (uint8_t)((c->cycle + 1) % CG_LC_CYCLE);
	}
	c->r_sent = c->r_lost = 0;
	c->round_t = now;
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
	if (c->bmax[0] || c->bmax[1]) {
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
