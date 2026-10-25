/* The rate of PEP mode B's middle leg (story 025): one controller per end
 * and peer (the router has one, the VPS one per session) for all its
 * streams (stream.h), which share it in turns.
 *
 * Not from loss. Story 024 measured what a sender blind to the links does
 * in the middle: cubic takes the random loss of a Starlink-like link for
 * congestion (15 Mbit/s where the link carries 60), BBR and KCP fill the
 * cellular queue to 0.7-1.9 s. Here the loss is the ARQ's business, and
 * the rate follows what the path consumes while its queue stays short:
 *
 * - Samples as BBR's delivery rate (bytes delivered between a segment's
 *   send and its ACK, over the time since the delivery that came before
 *   its send), but counting what was declared lost too while the queue
 *   stays under queue_us (cg_pcc_lost). The queue is the forward one-way
 *   delay the receiver reports (cg_pcc_owd) over its minimum, the least
 *   sample of each window of a round trip (a queue moves every sample up,
 *   a lost packet's copy that came late by a slower link only some); with
 *   no report, the round trip over min_rtt. Random loss is
 *   bytes the path took (up to 3 lost for 1 delivered). A sample whose
 *   round trip is over that counts the
 *   acknowledged bytes only, and a whole round of them sets the estimate
 *   to what was delivered (loss by overflow is not capacity).
 * - The estimate bw is the largest sample over a window of a few round
 *   trips (two buckets); app-limited samples under it are skipped.
 * - The pacing rate is bw times a gain: STARTUP 3/2 (BBR's 2.89, and 2,
 *   overflowed a short queue at once) until bw stops growing by 1/4 for three rounds or
 *   a sample shows the queue, DRAIN until what is in flight fits, then
 *   PROBE's cycle (5/4 for a round trip, 3/4, then 1 for six; the 5/4 ends
 *   early at a queue), and 3/4 while a window shows a standing queue.
 * - In flight at most twice bw x (min_rtt + queue_us) and a few segments:
 *   the pacing holds the rate, the window the late ACKs; a queue over
 *   queue_us for a round trip cuts the estimate to what is delivered.
 * - The engine's capacity (capacity.h, cg_pcc_hint): the estimate starts
 *   from it (at least CG_PCC_START) instead of from nothing, and while the
 *   engine finds the link that carries the data full (a capacity measured
 *   under a standing queue), it is the ceiling, if it is not under half the
 *   estimate (an idle link's capacity is its probes').
 * - A token bucket of CG_PCC_BURST_US paces the sends (the event loop wakes
 *   by the millisecond).
 *
 * Times in us (64-bit, monotonic), rates in bytes/s.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PEPCC_H
#define CG_PEPCC_H

#include <stdint.h>
#include <string.h>

#define CG_PCC_ONE 256           /* gains in 1/256 */
#define CG_PCC_STARTUP 384       /* 3/2: BBR's 2.89 (and 2) overflowed a short queue at once (test_pepcc) */
#define CG_PCC_DRAIN 171         /* 2/3 */
#define CG_PCC_CYCLE 8
#define CG_PCC_FLOOR 62500u      /* bytes/s: 0.5 Mbit/s, the least rate */
#define CG_PCC_START 2000000u    /* bytes/s: 16 Mbit/s, the first rate without a hint (2 left a live stream's start
				  * waiting at the router for 5 rounds) */
#define CG_PCC_BURST_US 2000u    /* the token bucket's depth, in time at the rate */
#define CG_PCC_MINRTT_US 10000000ull /* min_rtt's window */
#define CG_PCC_WIN_MIN_US 1000000u   /* bw's bucket: at least this, else 8 round trips (shorter, the
					 * estimate fell with the noise of a microcut's losses) */
#define CG_PCC_SEGS 4            /* in flight beyond the bound, segments */
#define CG_PCC_IW_SEGS 32        /* in flight before any round trip is known */
#define CG_PCC_WMIN_US 10000u    /* the window of the queue's verdict: a round trip, at least this */
#define CG_PCC_IDLE_US 1000000u  /* idle this long (nothing in flight), STARTUP again */

enum { CG_PCC_STARTUP_M = 0, CG_PCC_DRAIN_M, CG_PCC_PROBE_M };

struct cg_pcc_cfg {
	uint32_t queue_us; /* pep_queue_ms: the queue the middle leg may build */
	uint32_t mss;      /* the largest segment's bytes */
};

struct cg_pcc {
	uint64_t dl;     /* bytes delivered, and lost while the queue was short */
	uint64_t dla;    /* bytes delivered (acknowledged or SACKed) */
	uint64_t dl_us;  /* the last delivery */
	uint64_t sent;   /* bytes sent, again too */
	uint64_t bw;     /* the estimate, bytes/s; 0: none */
	uint64_t bmax[2], bt; /* the window's two buckets, and the current one's start */
	uint32_t min_rtt, srtt, rttvar; /* us; 0: none yet */
	uint64_t min_t;  /* when min_rtt was taken */
	uint8_t mode, cycle, full_n, cong;
	uint8_t qnow;       /* the last sample was over min_rtt + queue_us: what is sent now may overflow */
	uint64_t cycle_t;
	uint64_t round_dl;  /* a round ends with the ACK of a segment sent once dl had passed this */
	uint64_t full_bw;
	uint64_t cong_t;    /* the first congested window of the current run, 0: none */
	uint32_t wmin;      /* the least round trip in the current window, 0: none yet */
	uint64_t wt;        /* the window's start */
	/* the forward one-way delay the receiver reports in its ACKs (pep.h:
	 * its clock less ours, the offset included): the least of the last
	 * CG_PCC_MINRTT_US (base), and of the current window (ow_min), us */
	int32_t owd_base, ow_min, owd_last;
	uint64_t owd_t;
	uint8_t have_owd, have_ow;
	uint64_t inflight;  /* bytes in flight over all streams (the caller's) */
	uint64_t hint;      /* the engine's capacity for the data, bytes/s; 0: none */
	uint8_t hint_full;  /* the engine sees those links queued */
	uint8_t app;        /* the last send found nothing more to send */
	int64_t tokens;
	uint64_t tok_t;
	uint64_t n_cong, n_cut, n_rounds; /* congested samples; rounds that cut bw; rounds */
	uint64_t last_send, n_restart;    /* the last send (cg_pcc_restart); restarts from idle */
};

static inline void cg_pcc_init(struct cg_pcc *p, uint64_t now)
{
	memset(p, 0, sizeof(*p));
	p->dl_us = p->tok_t = p->bt = now;
	p->mode = CG_PCC_STARTUP_M;
}

static inline uint64_t cg_pcc_mul(uint64_t a, uint64_t b, uint64_t d)
{
	/* a * b / d: exact while a * b fits (rates up to ~10 Gbit/s times
	 * seconds in us), else in the other order (no __int128 on 32 bits) */
	if (!d)
		return 0;
	if (b && a > UINT64_MAX / b)
		return a / d * b;
	return a * b / d;
}

/* The pacing gain now, in 1/256. */
static inline uint32_t cg_pcc_gain(const struct cg_pcc *p)
{
	static const uint16_t cyc[CG_PCC_CYCLE] = { 320, 192, 256, 256, 256, 256, 256, 256 };

	if (p->mode == CG_PCC_STARTUP_M)
		return CG_PCC_STARTUP;
	if (p->mode == CG_PCC_DRAIN_M)
		return CG_PCC_DRAIN;
	/* a standing queue over queue_us: 3/4 until it drains (the estimate
	 * at what is delivered does not drain it) */
	if (p->cong)
		return 192;
	return cyc[p->cycle % CG_PCC_CYCLE];
}

/* The estimate, or the hint or the first rate before one. */
static inline uint64_t cg_pcc_bw(const struct cg_pcc *p)
{
	if (p->bw)
		return p->bw;
	/* the engine's capacity is what a link once delivered: at least that */
	return p->hint > CG_PCC_START ? p->hint : CG_PCC_START;
}

/* The pacing rate, bytes/s. */
static inline uint64_t cg_pcc_rate(const struct cg_pcc *p)
{
	uint64_t r = cg_pcc_mul(cg_pcc_bw(p), cg_pcc_gain(p), CG_PCC_ONE);

	return r > CG_PCC_FLOOR ? r : CG_PCC_FLOOR;
}

/* How many bytes may be in flight; reo_us, RACK's window (what was lost
 * stays in flight that long before it is declared). */
static inline uint64_t cg_pcc_cwnd(const struct cg_pcc *p, const struct cg_pcc_cfg *c, uint64_t reo_us)
{
	uint64_t w;

	if (!p->min_rtt)
		return (uint64_t)CG_PCC_IW_SEGS * c->mss;
	/* twice the path's: the pacing holds the rate, the window only the
	 * ACKs that come late (a lost packet's copy by a slower link: in a
	 * microcut most of them; with min_rtt + queue_us alone the window
	 * stopped the sends and the estimate followed it down); and what is
	 * lost and not declared yet (60 % lost and a window of 100 ms filled
	 * it with the lost, the sends stopped and the estimate sank: the
	 * microcut) */
	w = 2 * cg_pcc_mul(cg_pcc_bw(p), (uint64_t)p->min_rtt + c->queue_us, 1000000);
	if (p->mode != CG_PCC_STARTUP_M) /* STARTUP's overshoot is bounded by the path's alone */
		w += cg_pcc_mul(cg_pcc_bw(p), reo_us, 1000000);
	return w + (uint64_t)CG_PCC_SEGS * c->mss;
}

/* The engine's capacity for the links that carry the data, bytes/s (0:
 * none), and whether it sees them queued. */
static inline void cg_pcc_hint(struct cg_pcc *p, uint64_t hint, int full)
{
	p->hint = hint;
	/* a ceiling only near what the streams take: an idle link's capacity
	 * is what its probes deliver, "full" of them, and as a ceiling it held
	 * a new stream at a few kbit/s (live, protect) */
	p->hint_full = (uint8_t)(full && hint && hint >= CG_PCC_START / 2 && 2 * hint >= p->bw);
	if (p->hint_full && p->bw > hint) {
		p->bw = hint;
		p->bmax[0] = p->bmax[1] = hint;
	}
}

static inline void cg_pcc_filter(struct cg_pcc *p, uint64_t now, uint64_t rate, int app)
{
	uint64_t win = (uint64_t)p->srtt * 8;

	if (win < CG_PCC_WIN_MIN_US)
		win = CG_PCC_WIN_MIN_US;
	if (now - p->bt >= win) {
		p->bmax[1] = now - p->bt >= 2 * win ? 0 : p->bmax[0];
		p->bmax[0] = 0;
		p->bt = now;
	}
	/* an app-limited sample under what is known says nothing: the senders
	 * had no more */
	if (app && rate < cg_pcc_bw(p))
		rate = 0;
	if (rate > p->bmax[0])
		p->bmax[0] = rate;
	if (p->bmax[0] || p->bmax[1]) {
		p->bw = p->bmax[0] > p->bmax[1] ? p->bmax[0] : p->bmax[1];
		if (p->hint_full && p->bw > p->hint)
			p->bw = p->hint;

		if (p->bw < CG_PCC_FLOOR)
			p->bw = CG_PCC_FLOOR;
	}
}

/* An ACK delivered acked bytes at now; rs, the sample of its newest-sent
 * segment (stream.h's cg_st_rs: have, tx, app, sent_us, dl, dla, dl_us). */
static inline void cg_pcc_ack(struct cg_pcc *p, const struct cg_pcc_cfg *c, uint64_t now, uint64_t acked, int have,
			      int tx, int app, uint64_t sent_us, uint64_t s_dl, uint64_t s_dla, uint64_t s_dl_us,
			      uint64_t s_sent)
{
	uint32_t rtt = 0;
	uint64_t rate, iv, bytes;

	p->dl += acked;
	p->dla += acked;
	p->dl_us = now;
	if (!have)
		return;
	if (now > sent_us && now - sent_us < 10000000)
		rtt = (uint32_t)(now - sent_us);
	/* the round trip of a first transmission (Karn's rule) for min_rtt
	 * and srtt */
	if (rtt && tx == 1) {
		if (!p->min_rtt || rtt <= p->min_rtt || now - p->min_t > CG_PCC_MINRTT_US) {
			p->min_rtt = rtt;
			p->min_t = now;
		}
		if (!p->srtt) {
			p->srtt = rtt;
			p->rttvar = rtt / 2;
		} else {
			uint32_t d = rtt > p->srtt ? rtt - p->srtt : p->srtt - rtt;

			p->rttvar = (3 * p->rttvar + d) / 4;
			p->srtt = (7 * p->srtt + rtt) / 8;
		}
		if (!p->have_owd)
			p->qnow = rtt > p->min_rtt + c->queue_us;
		/* STARTUP ends at the first sample over: it grows by half a
		 * round, so a window's lag is a round of overflow */
		if (p->mode == CG_PCC_STARTUP_M && p->n_rounds >= 2 && p->qnow)
			p->mode = CG_PCC_DRAIN_M;
	}
	/* STARTUP's exit by the forward delay, when there is one */
	if (p->have_owd && p->mode == CG_PCC_STARTUP_M && p->n_rounds >= 2 && p->qnow)
		p->mode = CG_PCC_DRAIN_M;
	/* the queue's verdict: a queue moves every sample up, a late copy (a
	 * lost packet's copy on a slower link) only some, so the least of a
	 * window of a round trip says which. A segment sent again counts if it
	 * took at least min_rtt (it may be the first one's ACK, earlier than
	 * the path: never later); a window without samples says no queue
	 * (with only retransmissions in flight, the last verdict stuck and the
	 * rate with it) */
	if (rtt && p->min_rtt && (tx == 1 || rtt >= p->min_rtt) && (!p->wmin || rtt < p->wmin))
		p->wmin = rtt;
	if (now - p->wt >= (p->min_rtt > CG_PCC_WMIN_US ? p->min_rtt : CG_PCC_WMIN_US)) {
		/* the forward delay when the receiver reports it: a round trip
		 * also carries the ACK's way back, whose losses and slower
		 * copies (a microcut) read as a queue and cut the rate */
		if (p->have_owd)
			p->cong = p->have_ow && (int32_t)((uint32_t)p->ow_min - (uint32_t)p->owd_base) > (int32_t)c->queue_us;
		else
			p->cong = p->wmin && p->wmin > p->min_rtt + c->queue_us;
		p->wmin = 0;
		p->have_ow = 0;
		p->wt = now;
	}
	p->n_cong += p->cong;
	iv = now > s_dl_us ? now - s_dl_us : 0;
	/* what the path took: delivered, and lost while the queue was short
	 * (no more than 3 lost for 1 delivered: past 75 % lost, sending more
	 * is only waste) */
	bytes = p->dla - s_dla;
	/* in PROBE only: in STARTUP the queue's first overflow came before any
	 * sample said queue, its losses read as capacity, the estimate went to
	 * twice the link and stayed for a second (beyond: 2 s with nothing
	 * delivered behind the holes) */
	if (!p->cong && p->mode == CG_PCC_PROBE_M)
		bytes = p->dl - s_dl < 4 * bytes ? p->dl - s_dl : 4 * bytes;
	/* never more than went out meanwhile: losses are declared in bursts
	 * (RACK's window), and a burst over a short interval read as five
	 * times the link (a microcut) */
	if (bytes > p->sent - s_sent)
		bytes = p->sent - s_sent;
	if (iv >= 100) {
		rate = cg_pcc_mul(bytes, 1000000, iv);
		cg_pcc_filter(p, now, rate, app);
		/* a whole round trip with a queue: what is delivered, no more
		 * (and STARTUP is over) */
		if (p->cong) {
			if (!p->cong_t) {
				p->cong_t = now;
			} else if (now - p->cong_t >= p->min_rtt) {
				uint64_t d = cg_pcc_mul(p->dla - s_dla, 1000000, iv);

				if (d < cg_pcc_bw(p)) {
					p->bw = d > CG_PCC_FLOOR ? d : CG_PCC_FLOOR;
					p->bmax[0] = p->bmax[1] = p->bw;
					p->n_cut++;
				}
				if (p->mode == CG_PCC_STARTUP_M)
					p->mode = CG_PCC_DRAIN_M;
				p->cong_t = now;
			}
		} else {
			p->cong_t = 0;
		}
	}
	/* rounds, for STARTUP's end */
	if (s_dl >= p->round_dl) {
		p->round_dl = p->dl;
		p->n_rounds++;
		if (p->mode == CG_PCC_STARTUP_M && !app) {
			if (p->bw >= p->full_bw + p->full_bw / 4) {
				p->full_bw = p->bw;
				p->full_n = 0;
			} else if (++p->full_n >= 3) {
				p->mode = CG_PCC_DRAIN_M;
			}
		}
	}
	/* STARTUP's overshoot ends at the first queue (after two rounds: the
	 * first round trips set min_rtt) */
	if (p->mode == CG_PCC_STARTUP_M && p->cong && p->n_rounds >= 2)
		p->mode = CG_PCC_DRAIN_M;
	if (p->mode == CG_PCC_DRAIN_M &&
	    p->inflight <= cg_pcc_mul(cg_pcc_bw(p), (uint64_t)p->min_rtt + c->queue_us, 1000000)) {
		p->mode = CG_PCC_PROBE_M;
		p->cycle = 2;
		p->cycle_t = now;
	}
	/* far under what the engine saw the links deliver, or under
	 * CG_PCC_START / 4 (a link back from a stall, a cut that went), no
	 * queue and the senders not short of bytes: STARTUP again, not 5/4 a
	 * cycle (from the floor to 60 Mbit/s, 3.4 s) */
	if (p->mode == CG_PCC_PROBE_M && !p->cong && !p->qnow && !app &&
	    p->bw < (p->hint > CG_PCC_START ? p->hint : CG_PCC_START) / 4) {
		p->mode = CG_PCC_STARTUP_M;
		p->full_bw = 0;
		p->full_n = 0;
		p->n_restart++;
	}
	if (p->mode == CG_PCC_PROBE_M) {
		uint32_t g = cg_pcc_gain(p);

		if (now - p->cycle_t >= (p->min_rtt ? p->min_rtt : 20000) || (g > CG_PCC_ONE && p->cong)) {
			p->cycle = (uint8_t)((p->cycle + 1) % CG_PCC_CYCLE);
			p->cycle_t = now;
		}
	}
}

/* A send at now: after a second with nothing in flight the path may have
 * changed, and STARTUP goes again from the estimate it has (BBR's restart
 * from idle keeps the estimate too). */
static inline void cg_pcc_restart(struct cg_pcc *p, uint64_t now)
{
	if (!p->inflight && p->last_send && now - p->last_send > CG_PCC_IDLE_US && p->mode != CG_PCC_STARTUP_M) {
		p->mode = CG_PCC_STARTUP_M;
		p->full_bw = 0;
		p->full_n = 0;
		p->n_restart++;
	}
	p->last_send = now;
}

/* An ACK's one-way delay (pep.h: the least of the DATA it answers; the two
 * clocks' offset included, so only its changes mean anything). Call it
 * before cg_pcc_ack with the same ACK. */
static inline void cg_pcc_owd(struct cg_pcc *p, const struct cg_pcc_cfg *c, uint64_t now, int32_t owd)
{
	p->owd_last = owd;
	if (!p->have_owd || (int32_t)((uint32_t)owd - (uint32_t)p->owd_base) <= 0 || now - p->owd_t > CG_PCC_MINRTT_US) {
		p->owd_base = owd;
		p->owd_t = now;
		p->have_owd = 1;
	}
	if (!p->have_ow || (int32_t)((uint32_t)owd - (uint32_t)p->ow_min) < 0) {
		p->ow_min = owd;
		p->have_ow = 1;
	}
	p->qnow = (int32_t)((uint32_t)owd - (uint32_t)p->owd_base) > (int32_t)c->queue_us;
}

/* Bytes declared lost, of segments sent while the path's queue was short
 * (the caller leaves out those sent with a queue: overflow): what the path
 * took. */
static inline void cg_pcc_lost(struct cg_pcc *p, uint64_t bytes)
{
	p->dl += bytes;
}

/* The token bucket at now. */
static inline void cg_pcc_refill(struct cg_pcc *p, const struct cg_pcc_cfg *c, uint64_t now)
{
	uint64_t r = cg_pcc_rate(p), dt = now > p->tok_t ? now - p->tok_t : 0;
	int64_t cap = (int64_t)cg_pcc_mul(r, CG_PCC_BURST_US, 1000000) + 2 * (int64_t)c->mss;

	if (dt > 1000000)
		dt = 1000000;
	p->tokens += (int64_t)cg_pcc_mul(r, dt, 1000000);
	if (p->tokens > cap)
		p->tokens = cap;
	p->tok_t = now;
}

/* Whether a segment may go now: tokens left and room in flight. */
static inline int cg_pcc_may(const struct cg_pcc *p, const struct cg_pcc_cfg *c, uint64_t reo_us)
{
	return p->tokens > 0 && p->inflight < cg_pcc_cwnd(p, c, reo_us);
}

/* Whether a segment declared lost may go again now: tokens left (the
 * window does not hold a repair back: full of what was lost and not
 * declared yet, it held every repair until the timer declared it, a
 * second at a time after STARTUP's overflow) */
static inline int cg_pcc_may_repair(const struct cg_pcc *p)
{
	return p->tokens > 0;
}

/* A segment of bytes went (the caller adds it to inflight). */
static inline void cg_pcc_spend(struct cg_pcc *p, uint32_t bytes)
{
	p->tokens -= bytes;
	p->sent += bytes;
}

/* The us until the bucket has tokens again, 0: it has. */
static inline uint64_t cg_pcc_wait_us(const struct cg_pcc *p)
{
	if (p->tokens > 0)
		return 0;
	return cg_pcc_mul((uint64_t)(-p->tokens) + 1, 1000000, cg_pcc_rate(p)) + 1;
}

/* The retransmission timer: srtt + 4 rttvar, at least twice min_rtt and
 * 20 ms, plus extra_us (RACK's window beyond min_rtt / 4: the tunnel's own
 * repairs come first), doubled backoff times, 2 s at most; 200 ms before
 * any sample. */
static inline uint64_t cg_pcc_rto(const struct cg_pcc *p, unsigned backoff, uint32_t extra_us)
{
	uint64_t r = p->srtt ? (uint64_t)p->srtt + 4 * (uint64_t)p->rttvar : 200000;

	if (r < 2 * (uint64_t)p->min_rtt)
		r = 2 * (uint64_t)p->min_rtt;
	if (r < 20000)
		r = 20000;
	r += extra_us;
	r <<= backoff > 6 ? 6 : backoff;
	return r > 2000000 ? 2000000 : r;
}

/* RACK's reordering window: a quarter of min_rtt and extra_us (the
 * tunnel's own repair: its codes and rescue come first). */
static inline uint64_t cg_pcc_reo(const struct cg_pcc *p, uint32_t extra_us)
{
	return (uint64_t)p->min_rtt / 4 + extra_us;
}

#endif
