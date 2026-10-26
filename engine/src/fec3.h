/* FEC v3 (story 026, fec = yes with fec_code = rlc): the sending end's
 * decisions around the wide sliding window of rlc.h (struct cg_rlcw_tx).
 * Every DATA goes into the window; repairs over it go at a fixed share of
 * the bytes (fec_pct), whatever the loss, so that a link that stalls is
 * rebuilt from the repairs the others carry: a link with share s stalling
 * T is rebuilt about s T / (r (1 - s)) after it began at repair rate r
 * (story 026's estimate: four equal links, 50 ms, ~67 ms at 25 %), within
 * the window (fec_window_ms) and the receiver's wait for the code
 * (rescue_code_ms, harq.h).
 *
 * When it protects (cg_f3_mode): a split over two links or more (bonding:
 * a stall is one link's packets, the others' repairs rebuild them), or a
 * lone link (single: no other link to carry the repairs, so a cut takes
 * them too; they only answer scattered loss, Starlink's 0.3-1.3 %, a
 * radio's), gated by its loss (cg_f3_gate, fec_single = auto): on while
 * it loses at random (linksig.h), off on a clean link and on congestive
 * loss (no bytes spent) and in a cut (its loss over half the repair rate:
 * the code cannot keep up and the repairs die with the link). Redundancy over several
 * links: off (every packet is on every link; redund_spare protects the
 * first link there).
 *
 * How much (cg_f3_pct, with fec_pct_min under fec_pct): the whole share
 * while some link it protects lost in a burst lately (a stall, an outage,
 * a satellite switch: a sample losing CG_F3_BURST_PM or more without a
 * queue), else the floor plus CG_F3_PER_LOSS times the scattered loss
 * (links losing at random, weighed by their bytes), so a clean or only
 * congested bond pays the floor (story 026: 20 % of the bytes for nothing,
 * and BBR's capacity with them).
 *
 * How many (cg_f3_earn, cg_f3_owed, cg_f3_spend): a credit in bytes x 100,
 * each DATA earns its length x the share, each repair spends its size
 * (header and symbol); a tail repair (cg_f3_due) goes fec_wait_ms after the
 * first DATA it would cover when no repair followed it (the end of a
 * burst), on one repair of debt at most, so the share holds on average.
 *
 * Where (cg_f3_pick): by a smooth weighted round-robin over the links the
 * split sends on, each weighed by its DATA bytes lately (cg_f3_weigh), so
 * every link carries about fec_pct more than its share; not a link without
 * room (cg_f3_guard: its queue a second ahead past 2.5 x rescue_target_ms,
 * or past the window) nor one the split left out (muted, G3, G4). With no
 * link left, the repair is not sent (its bytes are spent all the same): the
 * share is a ceiling, filled where the links have room.
 *
 * Repairs are counted apart from DATA (tx_fec_*: the server counts a
 * path's DATA, and capacity.h would read a repair counted as DATA as
 * lost).
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FEC3_H
#define CG_FEC3_H

#include <stdint.h>
#include <string.h>

#include "proto.h"  /* CG_HDR_LEN */
#include "repair.h" /* CG_REP_HDR */

#define CG_F3_LINKS 16      /* CG_MAX_LINKS */
#define CG_F3_OFF_PM 1      /* fec_single = auto: off under this loss (per mille, smoothed) */
#define CG_F3_RAND_HOLD_MS 30000u /* and this long after the link last lost at random (linksig.h) */
#define CG_F3_RAND_PM 2     /* a sample losing this (per mille) or more ... */
#define CG_F3_RAND_Q_US 10000 /* ... with its queue within this: random (linksig.h's CG_SIG_RAND_Q_US) */
#define CG_F3_BURST 32      /* repairs one batch may owe at most */
#define CG_F3_COST0 1500u   /* a repair's bytes before the first one is known */
#define CG_F3_SHORT_MIN_US 20000u /* a short repair's window, at least */
#define CG_F3_BURST_PM 50   /* a sample losing this (per mille) or more without a queue: a burst (a 50 ms stall: ~100 in its 0.5 s sample) */
#define CG_F3_BURST_Q_US 10000 /* without a queue: its top over the minute's minimum within this (BBR draining 26 ms read as a burst at 30) */
#define CG_F3_HOLD_MS 30000u /* the whole share this long after a burst */
#define CG_F3_SRAND_HOLD_MS 120000u /* the loss's share this long after it was random off the link's capacity */
#define CG_F3_PER_LOSS 25   /* scattered loss alone: repairs this many times the loss, over the floor (story 026: 1 % wanted 20 % or more) */
#define CG_F3_WARM_MS 10000u /* the whole share this long from the first batch: nothing known yet, and a cubic that collapses at its start is slow to come back */

enum cg_f3_mode { CG_F3_OFF = 0, CG_F3_BOND, CG_F3_SINGLE };
enum cg_f3_single { CG_F3_SINGLE_AUTO = 0, CG_F3_SINGLE_YES, CG_F3_SINGLE_NO };

struct cg_f3 {
	int64_t credit;              /* repair bytes earned x 100, less those spent */
	uint32_t cost;               /* the last repair's bytes */
	uint64_t fresh_us;           /* the first DATA since the last repair, 0: none */
	int32_t cur[CG_F3_LINKS];    /* the round-robin's current weights */
	uint64_t w[CG_F3_LINKS];     /* each link's DATA bytes per tick, smoothed (1/4) */
	uint64_t txb[CG_F3_LINKS];   /* its DATA bytes sent at the last tick */
	uint16_t loss16[CG_F3_LINKS]; /* its loss, per mille x 16, smoothed (1/4) over its samples */
	uint64_t loss_ms[CG_F3_LINKS]; /* the sample last taken */
	uint64_t rand_ms[CG_F3_LINKS]; /* when its loss last read random (linksig.h's loss_kind), 0: never */
	uint64_t burst_ms[CG_F3_LINKS]; /* its last burst (CG_F3_BURST_PM), 0: never */
	uint64_t srand_ms[CG_F3_LINKS]; /* its last random loss while not loaded (the share's), 0: never */
	int32_t qprev[CG_F3_LINKS];  /* the previous sample's top queue (us, -1: none) */
	uint8_t lprev[CG_F3_LINKS];  /* and whether it was loaded */
	uint64_t bursts;             /* bursts seen, every link */
	uint32_t pct;                /* the share now, % (cg_f3_pct) */
	uint64_t warm_ms;            /* the whole share until then (CG_F3_WARM_MS from the first batch), 0: none yet */
	uint8_t gate;                /* the lone link's gate (fec_single = auto) */
	uint8_t alt;                 /* the next repair's span: the window or its quarter (cg_f3_span_us) */
	uint8_t mode;                /* enum cg_f3_mode, at the last batch */
};

/* How it protects now: n links carry the DATA (the split's, or every
 * carrier in redundancy). */
static inline enum cg_f3_mode cg_f3_mode_of(int split, unsigned n)
{
	if (n == 1)
		return CG_F3_SINGLE;
	return split && n >= 2 ? CG_F3_BOND : CG_F3_OFF;
}

/* The bytes a repair of slen bytes of symbol puts on a link. */
static inline uint32_t cg_f3_cost(unsigned slen)
{
	return CG_HDR_LEN + CG_REP_HDR + slen;
}

/* A DATA of len bytes went into the window at now_us: its share of
 * repairs earned (pct %), and it waits for a repair. */
static inline void cg_f3_earn(struct cg_f3 *f, uint32_t len, uint32_t pct, uint64_t now_us)
{
	f->credit += (int64_t)len * pct;
	if (!f->fresh_us)
		f->fresh_us = now_us ? now_us : 1;
}

/* Whether a repair is owed by the credit (the last repair's size). */
static inline int cg_f3_owed(const struct cg_f3 *f)
{
	return f->credit >= (int64_t)(f->cost ? f->cost : CG_F3_COST0) * 100;
}

/* Repair bytes not over the window (the parity asked for again): paid
 * from the credit. */
static inline void cg_f3_pay(struct cg_f3 *f, uint32_t bytes)
{
	f->credit -= (int64_t)bytes * 100;
}

/* A repair of bytes went (or was given up: it is spent all the same, the
 * share holds): the credit pays it, nothing waits for one. */
static inline void cg_f3_spend(struct cg_f3 *f, uint32_t bytes)
{
	cg_f3_pay(f, bytes);
	f->cost = bytes;
	f->fresh_us = 0;
}

/* Whether a tail repair is due: DATA waited wait_us for one, and the
 * credit is not in debt (it may go one repair into debt for it). */
static inline int cg_f3_due(const struct cg_f3 *f, uint64_t now_us, uint64_t wait_us)
{
	return f->fresh_us && now_us - f->fresh_us >= wait_us && f->credit >= 0;
}

/* When the next tail repair is due (us, the same clock), 0: none waits. */
static inline uint64_t cg_f3_due_at(const struct cg_f3 *f, uint64_t wait_us)
{
	return f->fresh_us ? f->fresh_us + wait_us : 0;
}

/* Nothing is protected now: no credit carried into the next time. */
static inline void cg_f3_idle(struct cg_f3 *f)
{
	f->credit = 0;
	f->fresh_us = 0;
}

/* Each tick: link i had sent txb DATA bytes in all (tx_bytes); its weight
 * follows what it sent since the last tick. */
static inline void cg_f3_weigh(struct cg_f3 *f, unsigned i, uint64_t txb)
{
	uint64_t d = txb >= f->txb[i] ? txb - f->txb[i] : 0;

	f->txb[i] = txb;
	f->w[i] = (f->w[i] * 3 + d) / 4;
}

/* The span of the next repair's window, us: every other one covers only
 * the last quarter of the window (CG_F3_SHORT_MIN_US at least). The long
 * ones reach back over a stall (a link's lost run needs the repairs of the
 * whole window); the short ones combine fewer packets each, so at the same
 * members per repair (rlc.h's CG_RLCW_DMAX) a scattered loss is in more of
 * them and comes back sooner (a lone link losing 1 %: TCP 26 Mbit/s with a
 * window of 100 ms against 15 with 300, its losses waiting for repairs past
 * TCP's own retransmission). */
static inline uint64_t cg_f3_span_us(struct cg_f3 *f, uint64_t window_us)
{
	uint64_t s = window_us / 4;

	f->alt ^= 1;
	if (!f->alt || window_us <= CG_F3_SHORT_MIN_US)
		return window_us;
	return s > CG_F3_SHORT_MIN_US ? s : CG_F3_SHORT_MIN_US;
}

/* Whether link i may not carry repairs: it is full (full: capacity.h's
 * rescue weight, from where its queue will be in a second, is 0: past 4 x
 * rescue_target_ms, where what it loses is congestion and a choked link's
 * queue would only grow with repairs), or its queue over the minute's
 * minimum (ql_us, -1: none measured) is past the window, where a repair
 * comes after the gaps it covers were given up. (Half the weight, 2.5 x
 * the target, also cut the repairs a loss-bound cubic needs while it
 * fills a lone link, and did not catch BBR, whose queue stays small while
 * the repairs take its capacity: story 026.) */
static inline int cg_f3_guard(int32_t ql_us, uint32_t window_us, int full)
{
	return full || (ql_us >= 0 && (uint32_t)ql_us > window_us);
}

/* How much before now a repair on link `link` ends its window: the DATA
 * sent since then on a slower link of set (behind[i]: its delay up over the
 * fastest's, when have says it is known) would arrive after the repair, a
 * column at the receiver for nothing until it does (under TCP the links'
 * queues tell up to 100 ms apart in the lab, and every repair carried a
 * hundred such members); at most cap_us. */
static inline uint32_t cg_f3_lag_us(const int32_t *behind, uint16_t have, uint16_t set, unsigned link, uint32_t cap_us)
{
	int32_t mine = (have >> link & 1) ? behind[link] : 0, most = mine;

	for (unsigned i = 0; i < CG_F3_LINKS; i++)
		if ((set & have) >> i & 1 && behind[i] > most)
			most = behind[i];
	if (most - mine <= 0)
		return 0;
	return (uint32_t)(most - mine) < cap_us ? (uint32_t)(most - mine) : cap_us;
}

/* The link for the next repair among cand (a bit per link), by a smooth
 * weighted round-robin on f->w (every candidate weighs 1 when none has a
 * weight yet); -1: no candidate. */
static inline int cg_f3_pick(struct cg_f3 *f, uint16_t cand)
{
	int64_t total = 0;
	int best = -1, any = 0;

	for (unsigned i = 0; i < CG_F3_LINKS; i++)
		any |= (cand >> i & 1) && f->w[i];
	for (unsigned i = 0; i < CG_F3_LINKS; i++) {
		int64_t w;

		if (!(cand >> i & 1))
			continue;
		/* in units of 64 bytes a tick, at least 1: the round-robin's
		 * 32-bit weights hold any rate */
		w = any ? (int64_t)(f->w[i] / 64) + 1 : 1;
		if (w > 1 << 20)
			w = 1 << 20;
		f->cur[i] += (int32_t)w;
		total += w;
		if (best < 0 || f->cur[i] > f->cur[best])
			best = (int)i;
	}
	if (best >= 0)
		f->cur[best] -= (int32_t)total;
	return best;
}

/* Link i's loss sample (its capacity's loss_pm, per mille, reported at
 * t_ms, with its queue over the minute's minimum ql_us, -1: none, and the
 * highest during the sample qmax_us, linksig.h's last_qmax; loaded: the
 * sample sent at least 3/4 of the link's capacity, capacity.h): into
 * its average, once per sample; a loss of CG_F3_RAND_PM or more without a
 * queue (CG_F3_RAND_Q_US) is random (Starlink's 0.3 % is under linksig.h's
 * lossy sample, 5 per mille, and its loss_kind never reads random). */
static inline void cg_f3_loss(struct cg_f3 *f, unsigned i, uint16_t loss_pm, uint64_t t_ms, int32_t ql_us,
			      int32_t qmax_us, int loaded)
{
	int32_t q2;
	int l2;

	if (!t_ms || t_ms == f->loss_ms[i])
		return;
	/* the share's evidence over this sample and the one before: a TCP
	 * that just backed off reports its overflow's loss in a sample already
	 * quiet and under its capacity (clean4) */
	q2 = f->loss_ms[i] && f->qprev[i] > qmax_us ? f->qprev[i] : qmax_us;
	l2 = loaded || (f->loss_ms[i] && f->lprev[i]);
	f->qprev[i] = qmax_us;
	f->lprev[i] = (uint8_t)!!loaded;
	f->loss_ms[i] = t_ms;
	if (loss_pm > 1000)
		loss_pm = 1000;
	f->loss16[i] = (uint16_t)((f->loss16[i] * 3 + loss_pm * 16) / 4);
	if (loss_pm >= CG_F3_RAND_PM && ql_us >= 0 && ql_us <= CG_F3_RAND_Q_US && loss_pm * 2 < 1000) {
		f->rand_ms[i] = t_ms;
		/* for the share, only off its capacity: a link at it loses what
		 * overflows (BBR keeps no queue, and the repairs on top of it
		 * overflowed clean4's links: loss that asked for more repairs) */
		if (!l2 && q2 >= 0 && q2 <= CG_F3_RAND_Q_US)
			f->srand_ms[i] = t_ms;
	}
	/* a burst: by the highest queue of the sample, not the last one (a TCP
	 * filling the link drops at the top of its sawtooth, and its standing
	 * queue lifts the minute's minimum: clean4 read 5-13 % "bursts" by
	 * ql_us), over this sample and the one before (BBR overflowing
	 * clean4's links held 15-70 ms; a stall on a full link is still one:
	 * judged by the load too, cubic's stalls went unseen, story 026) */
	if (loss_pm >= CG_F3_BURST_PM && ql_us >= 0 && q2 >= 0 && q2 <= CG_F3_BURST_Q_US) {
		f->burst_ms[i] = t_ms;
		f->bursts++;
	}
}

/* The share of repairs now (%, into f->pct), between lo and hi (fec_pct_min
 * and fec_pct; lo at hi or over: hi, the fixed share) over the links of set
 * (rnd: those whose loss_kind reads random now, linksig.h): hi while one of
 * them had a burst within CG_F3_HOLD_MS; else lo plus CG_F3_PER_LOSS times
 * the loss of the links that lost at random off their capacity within
 * CG_F3_SRAND_HOLD_MS (long: once repaired, a TCP fills the link and the
 * evidence stops; the share must outlive that, not drop under it), weighed by
 * their bytes (a link losing only with its queue counts as clean: its drops
 * are its TCP's signal); hi for CG_F3_WARM_MS from the first call. 0: no
 * repairs at all. now_ms: the samples' clock. */
static inline uint32_t cg_f3_pct(struct cg_f3 *f, uint16_t set, uint16_t rnd, uint32_t hi, uint32_t lo, uint64_t now_ms)
{
	uint64_t sw = 0, sl = 0;
	uint32_t mx = 0, pm, need;

	for (unsigned i = 0; i < CG_F3_LINKS; i++)
		if ((set & rnd) >> i & 1)
			f->srand_ms[i] = now_ms ? now_ms : 1;
	if (lo >= hi)
		return f->pct = hi;
	if (!f->warm_ms)
		f->warm_ms = now_ms + CG_F3_WARM_MS;
	if (now_ms < f->warm_ms)
		return f->pct = hi;
	for (unsigned i = 0; i < CG_F3_LINKS; i++) {
		if (!(set >> i & 1))
			continue;
		if (f->burst_ms[i] && now_ms - f->burst_ms[i] <= CG_F3_HOLD_MS)
			return f->pct = hi;
		sw += f->w[i];
		if (!f->srand_ms[i] || now_ms - f->srand_ms[i] > CG_F3_SRAND_HOLD_MS)
			continue;
		pm = f->loss16[i] / 16u;
		sl += f->w[i] * pm;
		if (pm > mx)
			mx = pm;
	}
	pm = sw ? (uint32_t)(sl / sw) : mx;
	need = lo + (pm * CG_F3_PER_LOSS + 9) / 10;
	return f->pct = need < hi ? need : hi;
}

/* Whether a lone link i is protected: fec_single (enum cg_f3_single);
 * auto, while linksig.h has seen it lose at random (random: its loss_kind
 * says so now) within CG_F3_RAND_HOLD_MS and it still loses CG_F3_OFF_PM or
 * more, and never while it loses over half the repair rate (pct %: a cut,
 * the repairs die with it). Not on congestive loss: a full link's drops are
 * TCP's signal to slow down, and repairs would take its capacity and hide
 * that signal (in the lab, a clean link saturated by TCP lost 0.2-1 % at
 * its queue: protecting it cost 6-12 % of its throughput). The hold keeps
 * it on once TCP, with the loss repaired, fills the link and its queue
 * makes the kind read congestive. now_ms: the tick's clock. */
static inline int cg_f3_gate(struct cg_f3 *f, unsigned i, int knob, int random, uint32_t pct, uint64_t now_ms)
{
	uint32_t pm = f->loss16[i] / 16u;

	if (random)
		f->rand_ms[i] = now_ms ? now_ms : 1;
	if (knob == CG_F3_SINGLE_YES)
		return 1;
	if (knob == CG_F3_SINGLE_NO)
		return 0;
	f->gate = pm * 2 <= pct * 10 && f->rand_ms[i] && now_ms - f->rand_ms[i] <= CG_F3_RAND_HOLD_MS &&
		  (random || pm >= CG_F3_OFF_PM);
	return f->gate;
}

#endif
