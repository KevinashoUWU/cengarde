/* The rate of the protocontroller's middle leg (story 025): one controller per end
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
 *   stays under queue_us (cg_scc_lost). The queue is the forward one-way
 *   delay the receiver reports (cg_scc_owd) over its minimum, the least
 *   sample of each window of a round trip (a queue moves every sample up,
 *   a lost packet's copy that came late by a slower link only some; the
 *   minimum falls by more than queue_us only when windows in a row find
 *   the shorter path, not for a packet now and then by it); with
 *   no report, the round trip over min_rtt. Random loss is
 *   bytes the path took (up to 3 lost for 1 delivered). A sample whose
 *   round trip is over that counts the
 *   acknowledged bytes only, and a whole round of them sets the estimate
 *   to what was delivered (loss by overflow is not capacity).
 * - Each sample is over the longer of the time since the delivery before
 *   the segment's send and the segment's own flight (BBR's ack and send
 *   intervals: a burst of SACKs after a hole read as many times the link).
 * - The estimate bw is the largest sample over a window of a few round
 *   trips (two buckets); app-limited samples under it are skipped.
 * - The pacing rate is bw times a gain: STARTUP 3/2 (BBR's 2.89, and 2,
 *   overflowed a short queue at once) until bw stops growing by 1/4 for three rounds or
 *   a sample shows the queue, DRAIN until what is in flight fits, then
 *   PROBE's cycle (5/4 for a round trip, 3/4, then 1 for six; the 5/4 ends
 *   early at a queue), and 3/4 while a window shows a standing queue.
 * - In flight at most twice bw x (min_rtt + queue_us) and a few segments:
 *   the pacing holds the rate, the window the late ACKs; a queue over
 *   queue_us for a round trip cuts the estimate: the first cut of a run to
 *   the most delivered lately (behind a hole an instant delivers next to
 *   nothing), the next ones to what is delivered.
 * - A queue the cuts do not drain is not the stream's (cg_scc_stuck): the
 *   engine moved the data to a link of a longer delay (or others hold a
 *   queue the stream cannot empty). After a run of congested windows whose
 *   least delay never fell, the base moves up to it and STARTUP goes again
 *   from the estimate it has; before, the rate sat at the floor until the
 *   base expired (10 s: GRIT's download collapse, story 025, 2026-10-27).
 * - Heavy random loss (cg_scc_lossw: of a window, more than 1 byte in 5
 *   lost with the queue short) is the caller's sign that the tunnel's own
 *   repairs do not keep up: protoctl.c stops waiting for them.
 * - The engine's capacity (capacity.h, cg_scc_hint): the estimate starts
 *   from it (at least CG_SCC_START) instead of from nothing, and while the
 *   engine finds the link that carries the data full (a capacity measured
 *   under a standing queue), it is the ceiling, if it is not under half the
 *   estimate (an idle link's capacity is its probes').
 * - A token bucket of CG_SCC_BURST_US paces the sends (the event loop wakes
 *   by the millisecond).
 *
 * Times in us (64-bit, monotonic), rates in bytes/s.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_STREAMCC_H
#define CG_STREAMCC_H

#include <stdint.h>
#include <string.h>

#define CG_SCC_ONE 256           /* gains in 1/256 */
#define CG_SCC_STARTUP 384       /* 3/2: BBR's 2.89 (and 2) overflowed a short queue at once (test_streamcc) */
#define CG_SCC_DRAIN 171         /* 2/3 */
#define CG_SCC_CYCLE 8
#define CG_SCC_FLOOR 62500u      /* bytes/s: 0.5 Mbit/s, the least rate */
#define CG_SCC_START 2000000u    /* bytes/s: 16 Mbit/s, the first rate without a hint (2 left a live stream's start
				  * waiting at the router for 5 rounds) */
#define CG_SCC_HINT_MAX 3u        /* a rate sample past STARTUP is at most this many times the engine's hint */
#define CG_SCC_BURST_US 2000u    /* the token bucket's depth, in time at the rate */
#define CG_SCC_MINRTT_US 10000000ull /* min_rtt's window */
#define CG_SCC_WIN_MIN_US 1000000u   /* bw's bucket: at least this, else 8 round trips (shorter, the
					 * estimate fell with the noise of a microcut's losses) */
#define CG_SCC_SEGS 4            /* in flight beyond the bound, segments */
#define CG_SCC_IW_SEGS 32        /* in flight before any round trip is known */
#define CG_SCC_WMIN_US 10000u    /* the window of the queue's verdict: a round trip, at least this */
#define CG_SCC_IDLE_US 1000000u  /* idle this long (nothing in flight), STARTUP again */
#define CG_SCC_LOSSW_US 100000u  /* the window of the heavy-loss verdict */
#define CG_SCC_LOSSW_MIN 16384u  /* bytes a window needs for a verdict */
#define CG_SCC_STUCK_ROUNDS 4u   /* a queue not drained over this many round trips (and 4 times its depth) is not
				  * the stream's (cg_scc_stuck) */
#define CG_SCC_STUCK_TOL_US 500u /* the least fall of the queue that says it drains */
#define CG_SCC_LOW_WINS 2u       /* windows in a row that find a shorter path before the base falls to it */
#define CG_SCC_LF_US 5000000u    /* the random-loss floor's bucket (two of them: 5 to 10 s, cg_scc_lfloor) */
#define CG_SCC_LF_NONE 0xffffu   /* a bucket without a window yet */
#define CG_SCC_LF_START 30u      /* per mille: the floor before any window says (a stream's first seconds) */

enum { CG_SCC_STARTUP_M = 0, CG_SCC_DRAIN_M, CG_SCC_PROBE_M };

struct cg_scc_cfg {
	uint32_t queue_us; /* protocontroller_queue_ms: the queue the middle leg may build */
	uint32_t mss;      /* the largest segment's bytes */
};

struct cg_scc {
	uint64_t dl;     /* bytes delivered, and lost while the queue was short */
	uint64_t dla;    /* bytes delivered (acknowledged or SACKed) */
	uint64_t dl_us;  /* the last delivery */
	uint64_t sent;   /* bytes sent, again too */
	uint64_t bw;     /* the estimate, bytes/s; 0: none */
	uint64_t bmax[2], bt; /* the window's two buckets, and the current one's start */
	uint64_t dmax[2];     /* the same window's largest delivered-only samples (acknowledged bytes) */
	uint8_t cut_run;      /* a cut since the queue appeared: the next ones follow what is delivered */
	uint32_t min_rtt, srtt, rttvar; /* us; 0: none yet */
	uint64_t min_t;  /* when min_rtt was taken */
	uint8_t mode, cycle, full_n, cong;
	uint8_t offer;      /* take the engine's offer (cg_scc_offer): the subflows find each link's losses; CG_SCC_OFFER_LINKS: each link's own rate */
	uint8_t qnow;       /* the last sample was over min_rtt + queue_us: what is sent now may overflow */
	uint64_t cycle_t;
	uint64_t round_dl;  /* a round ends with the ACK of a segment sent once dl had passed this */
	uint8_t r_q;        /* this round, a sample over a quarter of queue_us: the pipe may be full */
	uint64_t r_lost, r_dla; /* this round's lost (cg_scc_lost) and delivered bytes */
	uint64_t full_bw;
	uint64_t cong_t;    /* the first congested window of the current run, 0: none */
	uint32_t wmin;      /* the least round trip in the current window, 0: none yet */
	uint64_t wt;        /* the window's start */
	/* the forward one-way delay the receiver reports in its ACKs (protoctl.h:
	 * its clock less ours, the offset included): the least of the last
	 * CG_SCC_MINRTT_US (base), and of the current window (ow_min), us */
	int32_t owd_base, ow_min, owd_last;
	uint64_t owd_t;
	uint8_t have_owd, have_ow;
	/* a run of congested windows (cg_scc_stuck): when its queue last fell
	 * (0: no run), the queue then and the least of the run, over owd_base, us */
	uint64_t qr_t;
	int32_t qr_ref, qr_min;
	uint64_t n_rebase; /* runs whose queue did not drain: the base moved up */
	/* windows in a row whose least delay was under the base by more than
	 * queue_us (another, shorter path: cg_scc_owd), and the larger of their
	 * least */
	uint8_t low_n;
	int32_t low_ow;
	uint64_t inflight;  /* bytes in flight over all streams (the caller's) */
	uint64_t hint;      /* the engine's capacity for the data, bytes/s; 0: none */
	uint8_t hint_full;  /* the engine sees those links queued */
	uint8_t app;        /* the senders ran out of bytes with tokens and room left (protoctl.c sets it) */
	int64_t tokens;
	uint64_t tok_t;
	/* heavy random loss (cg_scc_heavy): of the last window, more than 1
	 * byte in 5 declared lost while the queue was short */
	uint64_t lw_t, lw_lost, lw_dl;
	uint16_t lf[2];      /* the loss of the windows with a short queue, per mille: each bucket's least */
	uint64_t lf_t;       /* the current bucket's start */
	uint8_t heavy;
	uint64_t n_heavy; /* windows with heavy loss */
	uint64_t n_cong, n_cut, n_rounds; /* congested samples; rounds that cut bw; rounds */
	uint64_t last_send, n_restart;    /* the last send (cg_scc_restart); restarts from idle */
};

static inline void cg_scc_init(struct cg_scc *p, uint64_t now)
{
	memset(p, 0, sizeof(*p));
	p->dl_us = p->tok_t = p->bt = p->lf_t = now;
	p->lf[0] = p->lf[1] = CG_SCC_LF_NONE;
	p->mode = CG_SCC_STARTUP_M;
}

/* The path's random loss, per mille: the least of the loss windows
 * (cg_scc_lossw) over the last 5 to 10 s. Random loss is in every window;
 * a queue's overflow only while more goes out than the path takes, so the
 * least is the random part. CG_SCC_LF_START before any window. */
static inline uint32_t cg_scc_lfloor(const struct cg_scc *p)
{
	uint32_t f = p->lf[0] < p->lf[1] ? p->lf[0] : p->lf[1];

	return f == CG_SCC_LF_NONE ? CG_SCC_LF_START : f;
}

static inline uint64_t cg_scc_mul(uint64_t a, uint64_t b, uint64_t d)
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
static inline uint32_t cg_scc_gain(const struct cg_scc *p)
{
	static const uint16_t cyc[CG_SCC_CYCLE] = { 320, 192, 256, 256, 256, 256, 256, 256 };

	if (p->mode == CG_SCC_STARTUP_M)
		return CG_SCC_STARTUP;
	if (p->mode == CG_SCC_DRAIN_M)
		return CG_SCC_DRAIN;
	/* a standing queue over queue_us: 3/4 until it drains (the estimate
	 * at what is delivered does not drain it) */
	if (p->cong)
		return 192;
	return cyc[p->cycle % CG_SCC_CYCLE];
}

/* The estimate, or the hint or the first rate before one. */
static inline uint64_t cg_scc_bw(const struct cg_scc *p)
{
	if (p->bw)
		return p->bw;
	/* the engine's capacity is what a link once delivered: at least that */
	return p->hint > CG_SCC_START ? p->hint : CG_SCC_START;
}

/* The engine's offer (story 029, step 2), bytes/s, 0: none. With each
 * link's own losses (offer: subflow), while not every link that carries
 * the data is full, the engine's capacity for them and a quarter: each
 * link gets more than its capacity, so what it delivers raises it where
 * there is room (a modem grants more as its queue grows: OMR found 2-3
 * times the engine's capacities), and where there is not its queue grows
 * and the split sends the rest to the others. Without it only the
 * stream's probe offered more, a round in eight, and the links'
 * capacities and the rate with them grew slowly (in the field the pacer
 * held the sends back far more than anything else). Not while the stream
 * sees a queue over its target (cong). */
#define CG_SCC_OFFER 320 /* 5/4 of CG_SCC_ONE */
/* offer = CG_SCC_OFFER_LINKS (link_cc): the hint is the sum of each link's
 * own pacing rate (linkcc.h), each probing for more on its own, and each
 * holding its own queue: offered as it is, whatever the stream's queue. */
#define CG_SCC_OFFER_LINKS 2

static inline uint64_t cg_scc_offer(const struct cg_scc *p)
{
	if (!p->offer || !p->hint)
		return 0;
	if (p->offer == CG_SCC_OFFER_LINKS)
		return p->hint;
	if (p->hint_full || p->cong)
		return 0;
	return cg_scc_mul(p->hint, CG_SCC_OFFER, CG_SCC_ONE);
}

/* The pacing rate, bytes/s: the estimate's, or the engine's offer. */
static inline uint64_t cg_scc_rate(const struct cg_scc *p)
{
	uint64_t r = cg_scc_mul(cg_scc_bw(p), cg_scc_gain(p), CG_SCC_ONE), o = cg_scc_offer(p);

	if (o > r)
		r = o;
	return r > CG_SCC_FLOOR ? r : CG_SCC_FLOOR;
}

/* How many bytes may be in flight; reo_us, RACK's window (what was lost
 * stays in flight that long before it is declared). */
static inline uint64_t cg_scc_cwnd(const struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t reo_us)
{
	uint64_t w, b;

	if (!p->min_rtt)
		return (uint64_t)CG_SCC_IW_SEGS * c->mss;
	/* twice the path's: the pacing holds the rate, the window only the
	 * ACKs that come late (a lost packet's copy by a slower link: in a
	 * microcut most of them; with min_rtt + queue_us alone the window
	 * stopped the sends and the estimate followed it down); and what is
	 * lost and not declared yet (60 % lost and a window of 100 ms filled
	 * it with the lost, the sends stopped and the estimate sank: the
	 * microcut) */
	b = cg_scc_bw(p) > cg_scc_offer(p) ? cg_scc_bw(p) : cg_scc_offer(p); /* room for the offer too */
	w = 2 * cg_scc_mul(b, (uint64_t)p->min_rtt + c->queue_us, 1000000);
	if (p->mode != CG_SCC_STARTUP_M) /* STARTUP's overshoot is bounded by the path's alone */
		w += cg_scc_mul(b, reo_us, 1000000);
	return w + (uint64_t)CG_SCC_SEGS * c->mss;
}

/* The engine's capacity for the links that carry the data, bytes/s (0:
 * none), and whether it sees them queued. */
static inline void cg_scc_hint(struct cg_scc *p, uint64_t hint, int full)
{
	p->hint = hint;
	/* a ceiling only near what the streams take: an idle link's capacity
	 * is what its probes deliver, "full" of them, and as a ceiling it held
	 * a new stream at a few kbit/s (live, protect) */
	p->hint_full = (uint8_t)(full && hint && hint >= CG_SCC_START / 2 && 2 * hint >= p->bw);
	if (p->hint_full && p->bw > hint) {
		p->bw = hint;
		p->bmax[0] = p->bmax[1] = hint;
	}
}

static inline void cg_scc_filter(struct cg_scc *p, uint64_t now, uint64_t rate, uint64_t drate, int app)
{
	uint64_t win = (uint64_t)p->srtt * 8;

	if (win < CG_SCC_WIN_MIN_US)
		win = CG_SCC_WIN_MIN_US;
	if (now - p->bt >= win) {
		p->bmax[1] = now - p->bt >= 2 * win ? 0 : p->bmax[0];
		p->dmax[1] = now - p->bt >= 2 * win ? 0 : p->dmax[0];
		p->bmax[0] = p->dmax[0] = 0;
		p->bt = now;
	}
	if (drate > p->dmax[0])
		p->dmax[0] = drate;
	/* an app-limited sample under what is known says nothing: the senders
	 * had no more */
	if (app && rate < cg_scc_bw(p))
		rate = 0;
	if (rate > p->bmax[0])
		p->bmax[0] = rate;
	if (p->bmax[0] || p->bmax[1]) {
		p->bw = p->bmax[0] > p->bmax[1] ? p->bmax[0] : p->bmax[1];
		if (p->hint_full && p->bw > p->hint)
			p->bw = p->hint;

		if (p->bw < CG_SCC_FLOOR)
			p->bw = CG_SCC_FLOOR;
	}
}

/* A window of the run found a queue of ex us over the base (more than
 * queue_us). The stream's own queue drains under the cuts (3/4 of what is
 * delivered: a quarter of the link's rate), and its least delay falls
 * window after window; one that does not fall over CG_SCC_STUCK_ROUNDS
 * round trips and four times its depth is the path's: the engine moved the
 * data to a link of a longer delay (download, protect: the first bytes by
 * the Starlink-like link set the base, the rest went by the cellular one,
 * 15 ms longer; the rate sat at the floor, which kept that link idle, so
 * the engine never moved it back, until the base expired 10 s later), or
 * others hold a queue the stream cannot empty (cutting more only starves
 * it). The base moves up to the run's least, and STARTUP goes again from
 * the estimate it has (the path changed, as after idle): a queue of its
 * own on the new path shows over the new base. A shorter path lowers it
 * again once CG_SCC_LOW_WINS windows in a row find it (cg_scc_owd).
 * Returns 1 when it moved. */
static inline int cg_scc_stuck(struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t now, int32_t ex)
{
	uint32_t tol = c->queue_us / 4 > CG_SCC_STUCK_TOL_US ? c->queue_us / 4 : CG_SCC_STUCK_TOL_US;
	uint64_t rt = p->min_rtt > CG_SCC_WMIN_US ? p->min_rtt : CG_SCC_WMIN_US;

	if (!p->qr_t || ex + (int64_t)tol < p->qr_ref) {
		/* a run begins, or its queue fell: the stream's, it drains */
		if (!p->qr_t || ex < p->qr_min)
			p->qr_min = ex;
		p->qr_t = now;
		p->qr_ref = ex;
		return 0;
	}
	if (ex < p->qr_min)
		p->qr_min = ex;
	if (now - p->qr_t < CG_SCC_STUCK_ROUNDS * rt + 4 * (uint64_t)(uint32_t)p->qr_ref)
		return 0;
	p->owd_base = (int32_t)((uint32_t)p->owd_base + (uint32_t)p->qr_min);
	p->owd_t = now;
	p->qr_t = 0;
	p->cong = 0;
	p->cong_t = 0;
	p->cut_run = 0;
	p->qnow = 0;
	if (p->mode != CG_SCC_STARTUP_M) {
		p->mode = CG_SCC_STARTUP_M;
		p->full_bw = 0;
		p->full_n = 0;
	}
	p->n_rebase++;
	return 1;
}

/* The heavy-loss verdict at now: each CG_SCC_LOSSW_US, whether more than
 * 1 byte in 5 of what the window settled was declared lost while the
 * queue was short (cg_scc_lost); a window with too few bytes keeps the
 * last verdict. The tunnel's own repairs do not keep up with that (a
 * microcut: 60 % lost), so the caller stops waiting for them (protoctl.c:
 * RACK's window and the timer back to protocontroller_reo_ms; widened by
 * duplicates, they had reached 100 ms, the oldest hole took several of
 * them and held the stream's buffer full, story 025). */
static inline void cg_scc_lossw(struct cg_scc *p, uint64_t now)
{
	if (now - p->lw_t < CG_SCC_LOSSW_US)
		return;
	if (p->lw_lost + p->lw_dl >= CG_SCC_LOSSW_MIN || !p->lw_lost)
		p->heavy = p->lw_lost * 4 > p->lw_dl;
	/* the random-loss floor: each window's loss into its bucket's least */
	if (now - p->lf_t >= CG_SCC_LF_US) {
		p->lf[1] = p->lf[0];
		p->lf[0] = CG_SCC_LF_NONE;
		p->lf_t = now;
	}
	if (p->lw_lost + p->lw_dl >= CG_SCC_LOSSW_MIN) {
		uint64_t f = p->lw_lost * 1000 / (p->lw_lost + p->lw_dl);

		if (f < p->lf[0])
			p->lf[0] = (uint16_t)f;
	}
	p->n_heavy += p->heavy;
	p->lw_t = now;
	p->lw_lost = p->lw_dl = 0;
}

/* An ACK delivered acked bytes at now; rs, the sample of its newest-sent
 * segment (stream.h's cg_st_rs: have, tx, app, sent_us, dl, dla, dl_us). */
static inline void cg_scc_ack(struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t now, uint64_t acked, int have,
			      int tx, int app, uint64_t sent_us, uint64_t s_dl, uint64_t s_dla, uint64_t s_dl_us,
			      uint64_t s_sent)
{
	uint32_t rtt = 0;
	uint64_t rate, iv, bytes;

	p->dl += acked;
	p->dla += acked;
	p->r_dla += acked;
	p->dl_us = now;
	cg_scc_lossw(p, now);
	p->lw_dl += acked;
	if (!have)
		return;
	if (now > sent_us && now - sent_us < 10000000)
		rtt = (uint32_t)(now - sent_us);
	/* the round trip of a first transmission (Karn's rule) for min_rtt
	 * and srtt */
	if (rtt && tx == 1) {
		if (!p->min_rtt || rtt <= p->min_rtt || now - p->min_t > CG_SCC_MINRTT_US) {
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
		if (!p->have_owd) {
			p->qnow = rtt > p->min_rtt + c->queue_us;
			if (rtt > p->min_rtt + c->queue_us / 4)
				p->r_q = 1;
		}
		/* STARTUP ends at the first sample over, without the forward
		 * delay (one path: a queue moves every sample up) */
		if (!p->have_owd && p->mode == CG_SCC_STARTUP_M && p->n_rounds >= 2 && p->qnow)
			p->mode = CG_SCC_DRAIN_M;
	}
	/* With the forward delay, STARTUP ends at the window's verdict (cong,
	 * below), not at one sample over: over several links one packet by a
	 * longer one read as a queue, STARTUP ended as a download began and
	 * PROBE's 5/4 a cycle took 3-4 s to the links (story 025); the least of
	 * a window says a queue only when every path has one */
	/* the queue's verdict: a queue moves every sample up, a late copy (a
	 * lost packet's copy on a slower link) only some, so the least of a
	 * window of a round trip says which. A segment sent again counts if it
	 * took at least min_rtt (it may be the first one's ACK, earlier than
	 * the path: never later); a window without samples says no queue
	 * (with only retransmissions in flight, the last verdict stuck and the
	 * rate with it) */
	if (rtt && p->min_rtt && (tx == 1 || rtt >= p->min_rtt) && (!p->wmin || rtt < p->wmin))
		p->wmin = rtt;
	if (now - p->wt >= (p->min_rtt > CG_SCC_WMIN_US ? p->min_rtt : CG_SCC_WMIN_US)) {
		/* the forward delay when the receiver reports it: a round trip
		 * also carries the ACK's way back, whose losses and slower
		 * copies (a microcut) read as a queue and cut the rate */
		if (p->have_owd) {
			int32_t ex = (int32_t)((uint32_t)p->ow_min - (uint32_t)p->owd_base);

			/* a shorter path, window after window: the base falls to it */
			if (p->have_ow && ex < -(int32_t)c->queue_us) {
				if (!p->low_n || (int32_t)((uint32_t)p->ow_min - (uint32_t)p->low_ow) > 0)
					p->low_ow = p->ow_min;
				if (++p->low_n >= CG_SCC_LOW_WINS) {
					p->owd_base = p->low_ow;
					p->owd_t = now;
					p->low_n = 0;
					ex = (int32_t)((uint32_t)p->ow_min - (uint32_t)p->owd_base);
				}
			} else {
				p->low_n = 0;
			}
			p->cong = p->have_ow && ex > (int32_t)c->queue_us;
			if (!p->cong)
				p->qr_t = 0;
			else
				cg_scc_stuck(p, c, now, ex);
		} else {
			p->cong = p->wmin && p->wmin > p->min_rtt + c->queue_us;
		}
		p->wmin = 0;
		p->have_ow = 0;
		p->wt = now;
	}
	p->n_cong += p->cong;
	/* over the longer of the time since the delivery before its send
	 * and its own flight (BBR's ack and send intervals): after a hole
	 * filled, a burst of SACKs over a short ack interval read as 300 to
	 * 900 Mbit/s on a 60 Mbit/s link, the estimate followed and the
	 * sends overflowed it (a microcut, story 025) */
	iv = now > s_dl_us ? now - s_dl_us : 0;
	if (now > sent_us && now - sent_us > iv)
		iv = now - sent_us;
	/* what the path took: delivered, and lost while the queue was short,
	 * as much as the path's random loss explains (cg_scc_lfloor: twice its
	 * share over what was delivered, no more than 3 lost for 1). More is a
	 * queue's overflow: a link whose queue is shorter than queue_us
	 * overflows before any sample says queue (Starlink's), and counted as
	 * capacity its losses fed the estimate, the sends and the losses (on a
	 * link of 60 Mbit/s with 5 ms of queue the estimate went to 240-320,
	 * 100 000 overflowed in 8 s; in the lab 744 Mbit/s, then 2 s of queue
	 * on the cellular link and the floor, story 025) */
	bytes = p->dla - s_dla;
	/* in PROBE only: in STARTUP the queue's first overflow came before any
	 * sample said queue, its losses read as capacity, the estimate went to
	 * twice the link and stayed for a second (beyond: 2 s with nothing
	 * delivered behind the holes) */
	if (!p->cong && p->mode == CG_SCC_PROBE_M) {
		uint32_t f = cg_scc_lfloor(p);
		uint64_t lost = p->dl - s_dl > bytes ? p->dl - s_dl - bytes : 0;
		uint64_t cap = f >= 750 ? 3 * bytes : bytes * 2 * f / (1000 - f);

		if (cap > 3 * bytes)
			cap = 3 * bytes;
		bytes += lost < cap ? lost : cap;
	}
	/* never more than went out meanwhile: losses are declared in bursts
	 * (RACK's window), and a burst over a short interval read as five
	 * times the link (a microcut) */
	if (bytes > p->sent - s_sent)
		bytes = p->sent - s_sent;
	if (iv >= 100) {
		uint64_t drate = cg_scc_mul(p->dla - s_dla, 1000000, iv);

		rate = cg_scc_mul(bytes, 1000000, iv);
		/* past STARTUP, never over CG_SCC_HINT_MAX times what the engine
		 * sees its links take (protect: with the slow link's queue at
		 * 2 s, deliveries learnt in bursts read 389 and 606 Mbit/s on
		 * links of 60 and 5; a small hint is an idle link's probes and
		 * caps nothing) */
		if (p->mode != CG_SCC_STARTUP_M && p->hint >= CG_SCC_START / 2) {
			uint64_t m = p->hint * CG_SCC_HINT_MAX;

			if (rate > m)
				rate = m;
			if (drate > m)
				drate = m;
		}
		cg_scc_filter(p, now, rate, drate, app);
		/* a whole round trip with a queue: what is delivered, no more
		 * (and STARTUP is over). The first cut of a run goes to the
		 * most delivered lately, not to this instant's: behind a hole
		 * (a microcut, the data moved to a slower link) an instant
		 * delivers next to nothing, and the rate sank to the floor
		 * for the rest of the cut (story 025); the next cuts of the
		 * run follow what is delivered */
		if (p->cong) {
			if (!p->cong_t) {
				p->cong_t = now;
			} else if (now - p->cong_t >= p->min_rtt) {
				uint64_t d = drate, dm = p->dmax[0] > p->dmax[1] ? p->dmax[0] : p->dmax[1];

				if (!p->cut_run && dm > d)
					d = dm;
				if (d < cg_scc_bw(p)) {
					p->bw = d > CG_SCC_FLOOR ? d : CG_SCC_FLOOR;
					p->bmax[0] = p->bmax[1] = p->bw;
					p->n_cut++;
				}
				p->dmax[0] = p->dmax[1] = drate;
				p->cut_run = 1;
				if (p->mode == CG_SCC_STARTUP_M)
					p->mode = CG_SCC_DRAIN_M;
				p->cong_t = now;
			}
		} else {
			p->cong_t = 0;
			p->cut_run = 0;
		}
	}
	/* rounds, for STARTUP's end */
	if (s_dl >= p->round_dl) {
		p->round_dl = p->dl;
		p->n_rounds++;
		/* a round that did not grow says the pipe is full only with a
		 * sign of it: some queue, or losses past the path's random ones.
		 * Without: what fed the stream grew slower than its rounds (a
		 * download, fed by the far TCP in its slow start at 100 ms while
		 * the middle's round trip is 20: its bursts drain the tokens, so
		 * the rounds are not app-limited), STARTUP ended within a second,
		 * the estimate fell to what had come (0.9 Mbit/s) and PROBE's 5/4
		 * a cycle took 4-5 s to the link (story 025) */
		if (p->mode == CG_SCC_STARTUP_M && !app) {
			uint32_t f = cg_scc_lfloor(p) * 2;
			/* or the heavy random loss of a microcut (its capacity reads
			 * in PROBE, losses and all), or the engine's ceiling */
			int sig = p->r_q || p->r_lost * 1000 > (p->r_lost + p->r_dla) * (f > 5 ? f : 5) || p->heavy ||
				  (p->hint_full && p->bw >= p->hint);

			if (p->bw >= p->full_bw + p->full_bw / 4) {
				p->full_bw = p->bw;
				p->full_n = 0;
			} else if (sig && ++p->full_n >= 3) {
				p->mode = CG_SCC_DRAIN_M;
			}
		}
		p->r_q = 0;
		p->r_lost = p->r_dla = 0;
	}
	/* STARTUP's overshoot ends at the first queue (after two rounds: the
	 * first round trips set min_rtt) */
	if (p->mode == CG_SCC_STARTUP_M && p->cong && p->n_rounds >= 2)
		p->mode = CG_SCC_DRAIN_M;
	if (p->mode == CG_SCC_DRAIN_M &&
	    p->inflight <= cg_scc_mul(cg_scc_bw(p), (uint64_t)p->min_rtt + c->queue_us, 1000000)) {
		p->mode = CG_SCC_PROBE_M;
		p->cycle = 2;
		p->cycle_t = now;
	}
	if (p->mode == CG_SCC_PROBE_M) {
		uint32_t g = cg_scc_gain(p);

		if (now - p->cycle_t >= (p->min_rtt ? p->min_rtt : 20000) || (g > CG_SCC_ONE && p->cong)) {
			p->cycle = (uint8_t)((p->cycle + 1) % CG_SCC_CYCLE);
			p->cycle_t = now;
		}
	}
}

/* A send at now: after a second with nothing in flight the path may have
 * changed, and STARTUP goes again from the estimate it has (BBR's restart
 * from idle keeps the estimate too). */
static inline void cg_scc_restart(struct cg_scc *p, uint64_t now)
{
	if (!p->inflight && p->last_send && now - p->last_send > CG_SCC_IDLE_US && p->mode != CG_SCC_STARTUP_M) {
		p->mode = CG_SCC_STARTUP_M;
		p->full_bw = 0;
		p->full_n = 0;
		p->n_restart++;
	}
	p->last_send = now;
}

/* An ACK's one-way delay (protoctl.h: the least of the DATA it answers; the two
 * clocks' offset included, so only its changes mean anything). Call it
 * before cg_scc_ack with the same ACK. */
static inline void cg_scc_owd(struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t now, int32_t owd)
{
	int32_t d = (int32_t)((uint32_t)owd - (uint32_t)p->owd_base);

	p->owd_last = owd;
	/* the base, the least of CG_SCC_MINRTT_US; a sample under it by more
	 * than queue_us came by another, shorter path, and the base falls to
	 * it only once windows in a row find it (cg_scc_ack): a packet now
	 * and then by the Starlink-like link, among a stream the engine sends
	 * by the cellular one, set it back each time, and the stream's own
	 * delay read as a queue again (story 025, 2026-10-27) */
	if (!p->have_owd || (d <= 0 && d >= -(int32_t)c->queue_us) || now - p->owd_t > CG_SCC_MINRTT_US) {
		p->owd_base = owd;
		p->owd_t = now;
		p->have_owd = 1;
	}
	if (!p->have_ow || (int32_t)((uint32_t)owd - (uint32_t)p->ow_min) < 0) {
		p->ow_min = owd;
		p->have_ow = 1;
	}
	p->qnow = (int32_t)((uint32_t)owd - (uint32_t)p->owd_base) > (int32_t)c->queue_us;
	if ((int32_t)((uint32_t)owd - (uint32_t)p->owd_base) > (int32_t)(c->queue_us / 4))
		p->r_q = 1;
}

/* Bytes declared lost, of segments sent while the path's queue was short
 * (the caller leaves out those sent with a queue: overflow): what the path
 * took. */
static inline void cg_scc_lost(struct cg_scc *p, uint64_t bytes)
{
	p->dl += bytes;
	p->lw_lost += bytes;
	p->r_lost += bytes;
}

/* The token bucket at now. */
static inline void cg_scc_refill(struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t now)
{
	uint64_t r = cg_scc_rate(p), dt = now > p->tok_t ? now - p->tok_t : 0;
	int64_t cap = (int64_t)cg_scc_mul(r, CG_SCC_BURST_US, 1000000) + 2 * (int64_t)c->mss;

	if (dt > 1000000)
		dt = 1000000;
	p->tokens += (int64_t)cg_scc_mul(r, dt, 1000000);
	if (p->tokens > cap)
		p->tokens = cap;
	p->tok_t = now;
}

/* Whether a segment may go now: tokens left and room in flight. */
static inline int cg_scc_may(const struct cg_scc *p, const struct cg_scc_cfg *c, uint64_t reo_us)
{
	return p->tokens > 0 && p->inflight < cg_scc_cwnd(p, c, reo_us);
}

/* Whether a segment declared lost may go again now: tokens left (the
 * window does not hold a repair back: full of what was lost and not
 * declared yet, it held every repair until the timer declared it, a
 * second at a time after STARTUP's overflow) */
static inline int cg_scc_may_repair(const struct cg_scc *p)
{
	return p->tokens > 0;
}

/* A segment of bytes went (the caller adds it to inflight). */
static inline void cg_scc_spend(struct cg_scc *p, uint32_t bytes)
{
	p->tokens -= bytes;
	p->sent += bytes;
}

/* The us until the bucket has tokens again, 0: it has. */
static inline uint64_t cg_scc_wait_us(const struct cg_scc *p)
{
	if (p->tokens > 0)
		return 0;
	return cg_scc_mul((uint64_t)(-p->tokens) + 1, 1000000, cg_scc_rate(p)) + 1;
}

/* The retransmission timer: srtt + 4 rttvar, at least twice min_rtt and
 * 20 ms, plus extra_us (RACK's window beyond min_rtt / 4: the tunnel's
 * codes come first), doubled backoff times, 2 s at most; 200 ms before
 * any sample. */
static inline uint64_t cg_scc_rto(const struct cg_scc *p, unsigned backoff, uint32_t extra_us)
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
 * tunnel's codes come first: cg_scc_reo_cap). */
static inline uint64_t cg_scc_reo(const struct cg_scc *p, uint32_t extra_us)
{
	return (uint64_t)p->min_rtt / 4 + extra_us;
}

#define CG_SCC_REO_MAX_US 300000u /* duplicates widen RACK's window up to this, at most: the latency budget */

/* The timer's extra wait beyond the round trip, from RACK's window reo_us:
 * with RACK over the mix off (the engine's subflows find each link's
 * losses, story 029), the timer is the only backstop, and the round trip
 * of the mix is shorter than the slowest link's: at least the latency
 * budget, or what was sent on the slowest link times out spuriously (in the
 * lab, nearly every one of its losses came as a duplicate). */
static inline uint32_t cg_scc_rto_extra(uint32_t reo_us, int rack_off)
{
	return rack_off && reo_us < CG_SCC_REO_MAX_US ? CG_SCC_REO_MAX_US : reo_us;
}

/* How far duplicates may widen RACK's window beyond min_rtt / 4 (extra_us
 * starts at base_us, protocontroller_reo_ms), with code_us the tunnel's
 * code horizon (rescue_code_ms while the sender sends repairs; 0: none).
 * The tunnel's rescue does not count (story 025, 2026-10-27): it no longer
 * sends a stream's DATA again, its NACK tells the stream of the loss
 * instead (cg_st_tx_nack), so the only repair that may still land after
 * the stream's own retransmission is the code's, and the receiver waits
 * for it no longer than its horizon before it asks (harq.h): beyond that
 * RACK waiting longer only delays. Before, a rescued copy behind every
 * retransmission said "duplicate" and the window sat at 100 ms. Without
 * a code a duplicate means only reordering (a stream goes on one link, a
 * switch reorders by the links' delay difference). In the field (story
 * 028, «Terreno», 2026-10-27) that reordering between modems went past
 * both old caps (20 ms without a code, 100 with one): 8-28 thousand of
 * RACK's retransmissions per 100 MB came back as duplicates. The window
 * only grows on that evidence (a duplicate the receiver reports) and
 * narrows again by itself (PCTL_REO_DECAY_US), so its cap is the latency
 * budget, code or not (story 029). Never under base_us. */
static inline uint32_t cg_scc_reo_cap(uint32_t base_us, uint32_t code_us)
{
	uint32_t cap = CG_SCC_REO_MAX_US;

	(void)code_us;
	return cap > base_us ? cap : base_us;
}

#define CG_SCC_HORIZON_MAX_US 1000000u /* the engine's repair horizon, at most */

/* RACK's window beyond min_rtt / 4 for a peer (task #77): reo_us, what
 * duplicates widened (cg_scc_reo_cap); with the engine's repair horizon
 * (horizon_us: the code's repairs and the rescue's NACK; 0: neither), at
 * least that from the start. A hole a code covers is rebuilt, or the
 * receiver asks for it and its NACK tells the stream (cg_st_tx_nack); the
 * NACK is exact (asked once the hole's own link brings something later,
 * whatever the other links' delays), so RACK only stands in for a NACK
 * lost. Before, RACK waited at most the code's 100 ms and learnt it from
 * duplicates; a stall's repairs take longer (s T / (r (1 - s)), story 026,
 * ~170 ms), and nearly every RACK retransmission came back a duplicate
 * (lossy 940 of 940, stall50 3918 of 4576, the code rebuilding thousands);
 * without a code, the links' delays reordered past 20 ms and the clean
 * cases paid 1-3 % of their DATA twice (equal, het). Heavy loss (the code
 * does not keep up): base_us, as before. */
static inline uint32_t cg_scc_reo_win(uint32_t base_us, uint32_t reo_us, uint32_t horizon_us, int heavy)
{
	if (heavy)
		return base_us;
	if (horizon_us > CG_SCC_HORIZON_MAX_US)
		horizon_us = CG_SCC_HORIZON_MAX_US;
	return reo_us > horizon_us ? reo_us : horizon_us;
}

#endif
