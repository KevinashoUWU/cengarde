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
 * it loses at random or above CG_F3_ON_PM, off on a clean link (no bytes
 * spent) and in a cut (its loss over half the repair rate: the code cannot
 * keep up and the repairs die with the link). Redundancy over several
 * links: off (every packet is on every link; redund_spare protects the
 * first link there).
 *
 * How many (cg_f3_earn, cg_f3_owed, cg_f3_spend): a credit in bytes x 100,
 * each DATA earns its length x fec_pct, each repair spends its size
 * (header and symbol); a tail repair (cg_f3_due) goes fec_wait_ms after the
 * first DATA it would cover when no repair followed it (the end of a
 * burst), on one repair of debt at most, so the share holds on average.
 *
 * Where (cg_f3_pick): by a smooth weighted round-robin over the links the
 * split sends on, each weighed by its DATA bytes lately (cg_f3_weigh), so
 * every link carries about fec_pct more than its share; not a link whose
 * queue is past the window (cg_f3_guard: its repairs would come after the
 * gaps they cover were given up, and they would fill its queue: a choked
 * link takes none), nor one the split left out (muted, G3, G4).
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
#define CG_F3_ON_PM 2       /* fec_single = auto: on from this loss (per mille, smoothed) */
#define CG_F3_OFF_PM 1      /* off under this */
#define CG_F3_BURST 32      /* repairs one batch may owe at most */
#define CG_F3_COST0 1500u   /* a repair's bytes before the first one is known */

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
	uint8_t gate;                /* the lone link's gate (fec_single = auto), with hysteresis */
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

/* Whether link i may not carry repairs: its queue over the minute's
 * minimum (ql_us, -1: none measured) is past the window, where a repair
 * comes after the gaps it covers were given up. */
static inline int cg_f3_guard(int32_t ql_us, uint32_t window_us)
{
	return ql_us >= 0 && (uint32_t)ql_us > window_us;
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
 * t_ms): into its average, once per sample. */
static inline void cg_f3_loss(struct cg_f3 *f, unsigned i, uint16_t loss_pm, uint64_t t_ms)
{
	if (!t_ms || t_ms == f->loss_ms[i])
		return;
	f->loss_ms[i] = t_ms;
	if (loss_pm > 1000)
		loss_pm = 1000;
	f->loss16[i] = (uint16_t)((f->loss16[i] * 3 + loss_pm * 16) / 4);
}

/* Whether a lone link i is protected: fec_single (enum cg_f3_single);
 * auto, while it loses at random (random: linksig.h's loss_kind says so)
 * or over CG_F3_ON_PM, until it loses under CG_F3_OFF_PM, and never while
 * it loses over half the repair rate (pct %: a cut). */
static inline int cg_f3_gate(struct cg_f3 *f, unsigned i, int knob, int random, uint32_t pct)
{
	uint32_t pm = f->loss16[i] / 16u;

	if (knob == CG_F3_SINGLE_YES)
		return 1;
	if (knob == CG_F3_SINGLE_NO)
		return 0;
	if (pm * 2 > pct * 10)
		f->gate = 0;
	else if (random || pm >= CG_F3_ON_PM)
		f->gate = 1;
	else if (pm < CG_F3_OFF_PM)
		f->gate = 0;
	return f->gate;
}

#endif
