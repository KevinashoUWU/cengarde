/* Two observations of each link, for the status only (story 018, task #59):
 * whose its queue is, and what kind of loss it has. No decision of the
 * engine reads them yet; they are the ground for a link model that detects
 * itself (linkmodel.h's `auto`).
 *
 * Whose the queue is (queue_owner). When a link's queue rises, its delivery
 * around the onset against its delivery 2-4 s before says it: down by more
 * than CG_SIG_DROP_PCT %, other users of the cell took its share (the link
 * carries less than it did); otherwise it is our own load (a full link
 * keeps delivering its capacity, or more). In ns-3 (LTE, 320 episodes, the
 * other users' arrival as ground truth) the rule was right 88 % of the
 * time; our own send rate, its rise before the queue's or its correlation
 * with the queue, 48-65 %. An episode starts when the queue over its
 * window's minimum (capacity.h's queue_us) passes CG_SIG_Q_US after
 * CG_SIG_QUIET_MS without. The queue is seen late (its probe's reply comes
 * behind it, and the delay is smoothed): ~0.7 s in ns-3, so the delivery
 * "around the onset" is that of the samples from CG_SIG_BACK_MS before it
 * to CG_SIG_NEW_MS after (86 % right in ns-3 through capacity.h, against
 * 82 % with the second after it). The verdict holds while the queue lasts
 * (over the window's minimum or the minute's: a queue standing longer than
 * the window becomes its minimum, capacity.h's C5) and CG_SIG_HOLD_MS
 * after, then it is unknown again; a new episode replaces it. No reference delivery (a link younger than 4 s, or idle
 * then), or no sample within CG_SIG_GIVEUP_MS: unknown.
 *
 * What kind of loss (loss_kind). A capacity sample that lost over
 * CG_SIG_LOSS_PM per mille of what was sent is random if the queue over
 * the minute's minimum (capacity.h's ql_us) stayed within CG_SIG_RAND_Q_US
 * during it and it lost under CG_SIG_RAND_LOSS_PM (Starlink: it loses
 * without a queue); otherwise congestive (a cellular link drops when its
 * deep queue overflows, or a short buffer drops much at a high rate). In
 * ns-3 the rule was right 89 % of the time over 2967 windows with loss
 * (95.5 % of the random ones); through capacity.h, 94.7 % of the windows
 * with a lossy sample. The kind changes after CG_SIG_KIND_N lossy samples
 * in a row agree on another (a sample without loss breaks the run: on a
 * clean link the loss's estimate gives a lossy sample now and then), and
 * goes back to none CG_SIG_HOLD_MS after the last lossy sample.
 *
 * Fed each tick after cg_cap_tick (cg_sig_cap), from what it already
 * measures; nothing per packet. Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LINKSIG_H
#define CG_LINKSIG_H

#include <stdint.h>
#include <string.h>

#include "capacity.h" /* struct cg_cap_link */

#define CG_SIG_RING 16           /* delivery samples kept: 8 s or more at one each 0.5 s */
#define CG_SIG_Q_US 20000        /* a queue over this starts an episode */
#define CG_SIG_QUIET_MS 1500u    /* after this long without one */
#define CG_SIG_REF_FROM_MS 4000u /* the reference delivery: samples from 4 s before the onset */
#define CG_SIG_REF_TO_MS 2000u   /* to 2 s before */
#define CG_SIG_BACK_MS 800u      /* against the samples around it: from 0.8 s before */
#define CG_SIG_NEW_MS 500u       /* to 0.5 s after */
#define CG_SIG_GIVEUP_MS 3000u   /* no sample that late: unknown */
#define CG_SIG_DROP_PCT 15       /* delivery down by more than this: others */
#define CG_SIG_HOLD_MS 10000u    /* a verdict outlives its queue or its last loss by this */
#define CG_SIG_LOSS_PM 5         /* a sample losing over this per mille is lossy */
#define CG_SIG_RAND_Q_US 10000   /* random: the queue over the minute's minimum within this */
#define CG_SIG_RAND_LOSS_PM 100  /* and it lost under this per mille (delivered over 90 %) */
#define CG_SIG_KIND_N 2          /* lossy samples in a row agreeing change the kind */

enum cg_sig_owner {
	CG_SIG_UNKNOWN = 0,
	CG_SIG_MINE,
	CG_SIG_OTHERS,
};

enum cg_sig_kind {
	CG_SIG_NONE = 0,
	CG_SIG_RANDOM,
	CG_SIG_CONGESTIVE,
};

struct cg_sig_link {
	/* the delivery of the last capacity samples: report time, bps */
	uint64_t d_t[CG_SIG_RING], d_bps[CG_SIG_RING];
	uint8_t d_at;
	uint64_t t_last;  /* the last capacity sample taken (its report time) */
	int32_t qmax;     /* the highest ql_us over the ticks since then, -1: none */
	/* queue_owner */
	uint64_t sig_ms;  /* the last tick with a queue over CG_SIG_Q_US, 0: none */
	uint64_t hold_ms; /* the last with one over the window's or the minute's minimum */
	uint64_t ep_t0;   /* an episode waiting for its verdict: its onset, 0: none */
	uint64_t ep_ref;  /* its reference delivery, 0: none */
	uint64_t ep_sum;  /* the delivery of its samples, from CG_SIG_BACK_MS before the onset */
	uint32_t ep_n, ep_post; /* how many; of them, taken after the onset */
	uint8_t owner;    /* enum cg_sig_owner */
	uint8_t have_drop;
	int16_t drop_pct; /* the last verdict's delivery drop, % (negative: it rose) */
	uint64_t owner_ms; /* when owner last changed, 0: never */
	/* loss_kind */
	uint8_t kind, cand, cand_n; /* enum cg_sig_kind; the run of lossy samples agreeing on cand */
	uint64_t lossy_ms; /* the last lossy sample */
	uint64_t kind_ms;  /* when kind last changed, 0: never */
	/* the last sample whose loss was known, as it was judged (status) */
	int16_t last_pm;   /* its loss, per mille, -1: none yet */
	int32_t last_qmax; /* the highest ql_us over it, -1: none */
	/* for the status: verdicts and samples, by kind */
	uint64_t n_mine, n_others, n_unknown, n_random, n_congestive;
};

static inline void cg_sig_reset(struct cg_sig_link *s)
{
	memset(s, 0, sizeof(*s));
	s->qmax = s->last_qmax = -1;
	s->last_pm = -1;
}

/* The mean delivery of the samples reported in [from, to], 0: none. */
static inline uint64_t cg_sig_mean(const struct cg_sig_link *s, uint64_t from, uint64_t to)
{
	uint64_t sum = 0, n = 0;

	for (unsigned i = 0; i < CG_SIG_RING; i++)
		if (s->d_t[i] && s->d_t[i] >= from && s->d_t[i] <= to) {
			sum += s->d_bps[i];
			n++;
		}
	return n ? sum / n : 0;
}

/* Whose a queue is, from the delivery before it (ref_bps, 0: unknown) and
 * around its onset (new_bps); *drop_pct, how far the delivery fell (%). */
static inline enum cg_sig_owner cg_sig_owner_of(uint64_t ref_bps, uint64_t new_bps, int *drop_pct)
{
	int64_t d;

	if (!ref_bps)
		return CG_SIG_UNKNOWN;
	d = 100 - (int64_t)(new_bps * 100 / ref_bps);
	*drop_pct = d < -999 ? -999 : (int)d;
	return d > CG_SIG_DROP_PCT ? CG_SIG_OTHERS : CG_SIG_MINE;
}

/* The kind of one sample's loss: loss_pm, its loss per mille (-1: not
 * known); qmax_us, the highest queue over the minute's minimum during it
 * (-1: none measured). */
static inline enum cg_sig_kind cg_sig_loss_of(int loss_pm, int32_t qmax_us)
{
	if (loss_pm <= CG_SIG_LOSS_PM || qmax_us < 0)
		return CG_SIG_NONE;
	if (qmax_us <= CG_SIG_RAND_Q_US && loss_pm < CG_SIG_RAND_LOSS_PM)
		return CG_SIG_RANDOM;
	return CG_SIG_CONGESTIVE;
}

static inline void cg_sig_set_owner(struct cg_sig_link *s, enum cg_sig_owner o, uint64_t now_ms)
{
	if (s->owner != o) {
		s->owner = (uint8_t)o;
		s->owner_ms = now_ms;
	}
}

/* An episode's verdict at now_ms: from its samples, if one came after
 * its onset. */
static inline void cg_sig_decide(struct cg_sig_link *s, uint64_t now_ms)
{
	int drop = 0;
	enum cg_sig_owner o = s->ep_post ? cg_sig_owner_of(s->ep_ref, s->ep_sum / s->ep_n, &drop) : CG_SIG_UNKNOWN;

	if (o != CG_SIG_UNKNOWN) {
		s->drop_pct = (int16_t)drop;
		s->have_drop = 1;
	}
	if (o == CG_SIG_MINE)
		s->n_mine++;
	else if (o == CG_SIG_OTHERS)
		s->n_others++;
	else
		s->n_unknown++;
	cg_sig_set_owner(s, o, now_ms);
	s->ep_t0 = 0;
}

/* A queue over CG_SIG_Q_US at now_ms. After CG_SIG_QUIET_MS without one,
 * an episode starts, with the samples of the last CG_SIG_BACK_MS. */
static inline void cg_sig_signal(struct cg_sig_link *s, uint64_t now_ms)
{
	if (!s->sig_ms || now_ms - s->sig_ms >= CG_SIG_QUIET_MS) {
		s->ep_t0 = now_ms ? now_ms : 1;
		s->ep_ref = now_ms >= CG_SIG_REF_FROM_MS
				    ? cg_sig_mean(s, now_ms - CG_SIG_REF_FROM_MS, now_ms - CG_SIG_REF_TO_MS)
				    : 0;
		s->ep_sum = 0;
		s->ep_n = s->ep_post = 0;
		for (unsigned i = 0; i < CG_SIG_RING; i++)
			if (s->d_t[i] && s->d_t[i] + CG_SIG_BACK_MS > now_ms && s->d_t[i] <= now_ms) {
				s->ep_sum += s->d_bps[i];
				s->ep_n++;
			}
	}
	s->sig_ms = now_ms;
}

/* One tick at now_ms: q_us, the queue over the window's minimum (the
 * episodes'); ql_us, over the minute's (the loss's kind); -1: none. */
static inline void cg_sig_tick(struct cg_sig_link *s, uint64_t now_ms, int32_t q_us, int32_t ql_us)
{
	if (ql_us > s->qmax)
		s->qmax = ql_us;
	if (q_us > CG_SIG_Q_US)
		cg_sig_signal(s, now_ms);
	if (q_us > CG_SIG_Q_US || ql_us > CG_SIG_Q_US)
		s->hold_ms = now_ms;
	if (s->ep_t0 && now_ms - s->ep_t0 >= CG_SIG_GIVEUP_MS)
		cg_sig_decide(s, now_ms);
	if (!s->ep_t0 && s->owner != CG_SIG_UNKNOWN && now_ms - s->hold_ms >= CG_SIG_HOLD_MS)
		cg_sig_set_owner(s, CG_SIG_UNKNOWN, now_ms);
	if (s->kind != CG_SIG_NONE && now_ms - s->lossy_ms >= CG_SIG_HOLD_MS) {
		s->kind = CG_SIG_NONE;
		s->kind_ms = now_ms;
		s->cand_n = 0;
	}
}

/* One capacity sample reported at t_ms: what it delivered (bps) and its
 * loss per mille (-1: not known). After the tick. */
static inline void cg_sig_sample(struct cg_sig_link *s, uint64_t t_ms, uint64_t rx_bps, int loss_pm)
{
	enum cg_sig_kind k = cg_sig_loss_of(loss_pm, s->qmax);

	if (loss_pm >= 0) {
		s->last_pm = (int16_t)(loss_pm < 1000 ? loss_pm : 1000);
		s->last_qmax = s->qmax;
	}
	s->qmax = -1;
	s->t_last = t_ms;
	s->d_t[s->d_at] = t_ms;
	s->d_bps[s->d_at] = rx_bps;
	s->d_at = (uint8_t)((s->d_at + 1) % CG_SIG_RING);
	if (s->ep_t0) {
		/* every sample taken after the onset is new to it (a report
		 * slightly older than the tick that saw the queue too) */
		s->ep_sum += rx_bps;
		s->ep_n++;
		s->ep_post++;
		if (t_ms >= s->ep_t0 + CG_SIG_NEW_MS)
			cg_sig_decide(s, t_ms);
	}
	if (loss_pm < 0)
		return; /* not known: the run toward another kind stands */
	if (k == CG_SIG_NONE || k == s->kind) {
		s->cand_n = 0;
		if (k != CG_SIG_NONE)
			s->lossy_ms = t_ms;
		if (k == CG_SIG_RANDOM)
			s->n_random++;
		else if (k == CG_SIG_CONGESTIVE)
			s->n_congestive++;
		return;
	}
	s->lossy_ms = t_ms;
	if (k == CG_SIG_RANDOM)
		s->n_random++;
	else
		s->n_congestive++;
	s->cand_n = k == s->cand && s->cand_n ? (uint8_t)(s->cand_n < 255 ? s->cand_n + 1 : 255) : 1;
	s->cand = (uint8_t)k;
	if (s->cand_n >= CG_SIG_KIND_N) {
		s->kind = (uint8_t)k;
		s->kind_ms = t_ms;
		s->cand_n = 0;
	}
}

/* One tick of link l's capacity (after cg_cap_tick), at now_ms. */
static inline void cg_sig_cap(struct cg_sig_link *s, const struct cg_cap_link *l, uint64_t now_ms)
{
	cg_sig_tick(s, now_ms, l->queue_us, l->ql_us);
	if (l->have_rate && l->t_ms != s->t_last)
		cg_sig_sample(s, l->t_ms, l->last_rx_bps, l->loss_pm_ms == l->t_ms ? (int)l->loss_pm : -1);
}

static inline const char *cg_sig_owner_str(unsigned o)
{
	return o == CG_SIG_MINE ? "mine" : o == CG_SIG_OTHERS ? "others" : "unknown";
}

static inline const char *cg_sig_kind_str(unsigned k)
{
	return k == CG_SIG_RANDOM ? "random" : k == CG_SIG_CONGESTIVE ? "congestive" : "none";
}

#endif
