/* Copies and parity in a choked link's spare (story 020, redund_spare).
 *
 * In redundancy every packet goes on every link, and a link slower than
 * the traffic queues the copies it cannot carry: on the field (story 017)
 * WOM, its knee near 5 Mbit/s under a 6-9 Mbit/s upload, built 3-8 s of
 * queue, and its copies came too late or were dropped at its tail; cutting
 * the copies by their expected arrival (eta.h, 60 ms) left 40-64 % of the
 * upload on Starlink alone, which loses 0.3-1.3 % without a queue. Here a
 * link other than the first (the one where a packet arrives first, eta.h)
 * gets an allowance, a rate for what it carries besides the first link's
 * traffic, that keeps its queue at about target_us:
 *
 *   - while its queue (over the minute's minimum, capacity.h's ql_us) is
 *     over target, the allowance falls by a quarter (at most every
 *     CG_SPARE_HOLD_MS: the queue is seen a round trip late);
 *   - under half the target, with the allowance in use, it grows by a
 *     sixteenth (and CG_SPARE_STEP) a tick;
 *   - it never goes over 3/2 of the upload's data rate (a full copy of
 *     everything needs no more) nor under CG_SPARE_MIN_BPS.
 *
 * A token bucket at that rate decides each copy: a priority packet (small,
 * or TCP's ACKs and retransmissions with tcp_aware) may take it below zero
 * by up to a burst, the rest only what is there. What goes in it, by mode:
 *
 *   copies   copies of the packets that fit (partial copies);
 *   parity   no copies but the priority ones: an XOR parity (fec.h, the
 *            same CG_T_FEC of the protocol) over k packets of the first
 *            link, k from the allowance (cg_spare_k), on the link with the
 *            most room; copies while the allowance holds everything;
 *   mix      everything while the allowance holds the whole upload (full
 *            copies); copies while the first link loses over
 *            CG_SPARE_MIX_PM (a microcut: a parity over k loses two), and
 *            parity the rest of the time.
 *   rs, rlc  as mix, with an erasure code instead of the XOR (story 021,
 *            repair.h): Reed-Solomon blocks (rs.h, Leopard) or a sliding
 *            window (rlc.h), k and m from the first link's loss and the
 *            allowance (cg_spare_plan); copies while the code would not fit
 *            the loss (a microcut), or the allowance holds everything.
 *
 * The deadline is the target: what the bucket lets through waits at most
 * about target_us in the link's queue; the eta.h window (redund_pace_ms)
 * still applies on top when set.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SPARE_H
#define CG_SPARE_H

#include <stdint.h>

enum { CG_SPARE_OFF = 0, CG_SPARE_COPIES = 1, CG_SPARE_PARITY = 2, CG_SPARE_MIX = 3, CG_SPARE_RS = 4, CG_SPARE_RLC = 5 };

#define CG_SPARE_MIN_BPS 128000u /* the allowance's floor */
#define CG_SPARE_STEP 64000u     /* its additive growth a tick, with a sixteenth */
#define CG_SPARE_HOLD_MS 500     /* between two decreases */
#define CG_SPARE_BURST_US 20000  /* the bucket's depth, in time at its rate */
#define CG_SPARE_BURST_MIN 36000 /* ... and at least three 1500-byte packets, in bits */
#define CG_SPARE_MIX_PM 50       /* mix: the first link losing over this (permil) takes copies */
#define CG_SPARE_KMIN 2
#define CG_SPARE_HOLD_CUT_MS 3000 /* the first link stays this long after a sample losing over CG_SPARE_MIX_PM */

/* One link's allowance. Zero is a link never measured. */
struct cg_spare_link {
	uint64_t rate_bps;  /* the allowance, 0: not set yet */
	int64_t tok;        /* bits in the bucket (negative: priority took ahead) */
	uint64_t t_us;      /* its last refill */
	uint64_t cut_ms;    /* the last decrease */
	uint64_t used;      /* bits taken since the last tick */
	uint64_t n_cut, n_up, n_take, n_skip;
};

/* A link measured afresh (a new socket): its allowance and bucket start
 * again, its counters stay. */
static inline void cg_spare_restart(struct cg_spare_link *l)
{
	l->rate_bps = l->t_us = l->cut_ms = l->used = 0;
	l->tok = 0;
}

/* The bucket's depth at rate_bps, in bits. */
static inline int64_t cg_spare_burst(uint64_t rate_bps)
{
	uint64_t b = rate_bps * CG_SPARE_BURST_US / 1000000;

	return (int64_t)(b < CG_SPARE_BURST_MIN ? CG_SPARE_BURST_MIN : b);
}

/* One tick (dt_ms since the last) of a link that is not the first: q_us
 * its queue over the minute's minimum (-1: not known, no change),
 * target_us the queue to hold, data_bps the upload's data rate, cap_bps
 * its capacity (0: not measured; the first allowance is half of it). */
static inline void cg_spare_tick(struct cg_spare_link *l, uint64_t now_ms, uint32_t dt_ms, int32_t q_us,
				 uint32_t target_us, uint64_t data_bps, uint64_t cap_bps)
{
	uint64_t used_bps = dt_ms ? l->used * 1000 / dt_ms : 0, top;

	l->used = 0;
	if (!l->rate_bps)
		l->rate_bps = cap_bps / 2 > CG_SPARE_MIN_BPS ? cap_bps / 2 : CG_SPARE_MIN_BPS;
	if (q_us >= 0 && (uint32_t)q_us > target_us) {
		if (!l->cut_ms || now_ms - l->cut_ms >= CG_SPARE_HOLD_MS) {
			l->rate_bps -= l->rate_bps / 4;
			l->cut_ms = now_ms;
			l->n_cut++;
		}
	} else if (q_us >= 0 && (uint32_t)q_us * 2 <= target_us && used_bps * 4 >= l->rate_bps * 3) {
		l->rate_bps += l->rate_bps / 16 + CG_SPARE_STEP;
		l->n_up++;
	}
	top = data_bps * 3 / 2;
	if (top < 4 * CG_SPARE_MIN_BPS)
		top = 4 * CG_SPARE_MIN_BPS;
	if (l->rate_bps > top)
		l->rate_bps = top;
	if (l->rate_bps < CG_SPARE_MIN_BPS)
		l->rate_bps = CG_SPARE_MIN_BPS;
}

/* Whether a packet of bits may go on the link now (and if so, taken from
 * its bucket). prio: a priority packet, which may take the bucket down to
 * minus a burst; must: one that goes anyway (a small packet), only
 * counted, the bucket held at minus a burst. */
static inline int cg_spare_take(struct cg_spare_link *l, uint64_t now_us, uint32_t bits, int prio, int must)
{
	int64_t burst;
	uint64_t dt;

	if (!l->rate_bps)
		l->rate_bps = CG_SPARE_MIN_BPS;
	burst = cg_spare_burst(l->rate_bps);
	dt = now_us > l->t_us ? now_us - l->t_us : 0;
	if (!l->t_us || dt > 1000000)
		dt = 1000000;
	l->t_us = now_us;
	l->tok += (int64_t)(l->rate_bps * dt / 1000000);
	if (l->tok > burst)
		l->tok = burst;
	if (must || l->tok >= (int64_t)bits || (prio && l->tok > -burst)) {
		l->tok -= bits;
		if (l->tok < -burst)
			l->tok = -burst;
		l->used += bits;
		l->n_take++;
		return 1;
	}
	l->n_skip++;
	return 0;
}

/* The parity's group size: the smallest k whose parity (a packet per k)
 * fits 7/8 of the allowance spare_bps against the data rate data_bps, in
 * [CG_SPARE_KMIN, kmax]; kmax when it does not fit at all (the parities
 * then go only as the bucket lets them). */
static inline unsigned cg_spare_k(uint64_t data_bps, uint64_t spare_bps, unsigned kmax)
{
	uint64_t a = spare_bps * 7 / 8, k;

	if (!a)
		return kmax;
	k = (data_bps + a - 1) / a;
	if (k < CG_SPARE_KMIN)
		k = CG_SPARE_KMIN;
	return k > kmax ? kmax : (unsigned)k;
}

/* A link is choked: its queue over the minute's minimum (q_us, -1 not
 * known) or its delay without a queue over the fastest link's (base_us)
 * past choke_us, or its lag (lag_us, cg_spare_lag) past choke_us and
 * CG_SPARE_LAG_US. The base is what shows a load that lasts: the minute's
 * minimum climbs with the queue and q reads near 0 (the NUC, 2026-10-23:
 * Claro on B5, ~5 Mbit/s carrying all the upload as the first link, RTT
 * 900 ms with q ~0). The lag is what shows a link seconds behind whose
 * one-way measures say nothing: they come with its probe replies, and
 * stop or arrive late with them (the NUC, 2026-10-23: Claro taken first
 * with an RTT of 8.9 s, its queue ~3 ms). It is a round trip, the
 * download's queue in it: only seconds count (a download fills a
 * cellular link's queue to hundreds of ms, its upload clear). */
#define CG_SPARE_LAG_US 1000000

static inline int cg_spare_choked(int32_t q_us, uint32_t base_us, uint32_t lag_us, uint32_t choke_us)
{
	return (q_us >= 0 && (uint32_t)q_us > choke_us) || base_us > choke_us ||
	       (lag_us > choke_us && lag_us > CG_SPARE_LAG_US);
}

/* Whether a link's delay is measured now, for the first link's choice: a
 * report (report_ms, 0: none) in the last CG_SPARE_FRESH_MS. A link whose
 * replies stopped keeps the queue and minimum of its last report and reads
 * clear (the NUC, 2026-10-23: Claro first with an RTT of 8.9 s), and its
 * minimum, the smallest, made every other link's base look large. Not
 * health.h's freshness (5 probe intervals, 500 ms under traffic): a
 * microcut's first second loses most replies, and the first link went
 * stale and out before its loss could hold it (the lab, protectcut). */
#define CG_SPARE_FRESH_MS 3000

static inline int cg_spare_fresh(uint64_t report_ms, uint64_t now_ms)
{
	return report_ms && now_ms >= report_ms && now_ms - report_ms <= CG_SPARE_FRESH_MS;
}

/* A link's lag behind the fastest from its probes: its round trip (rtt_us,
 * the caller's: the smoothed one, or the last when longer) over the least
 * smoothed one of the links measured (ref_us). Not its unanswered probes:
 * in a microcut (60 % lost) a second without a reply is loss, not delay,
 * and counting it banned the link the hold is for (the lab, protectcut:
 * the 1200-byte ping's p99 at 1.8 s, against 0.37); a link with no reply
 * at all goes stale and out anyway (spare_tick). */
static inline uint32_t cg_spare_lag(uint32_t rtt_us, uint32_t ref_us)
{
	return rtt_us > ref_us ? rtt_us - ref_us : 0;
}

/* The first link, the one the others protect: of the links of set (n at
 * most 16) whose delay without a queue is known (base_us[i], over the
 * smallest; known bit i: measured now, cg_spare_fresh) and that are not
 * banned, those not choked (cg_spare_choked: q_us[i], over the minute's
 * minimum, -1 not known counts as none; base_us[i]; lag_us[i]), and of
 * those the one of least delay. cur (-1: none) stays while it is one of
 * them and no other is more than CG_SPARE_FIRST_HYST_US below it; with
 * none of them, cur if in set, else the least late of set (cg_spare_late:
 * not the least queue alone, which took a link seconds behind with a
 * queue read ~0). hold: cur stays whatever the rest says, even out
 * of set (a microcut on it: health may take it out for a moment). The
 * caller bans for CG_SPARE_BAN_MS a first link choked for CG_SPARE_CHOKE_MS
 * (cg_spare_keep): the first link carries everything, and a choked one
 * taken for it (the least delay, or the only one measured at the start)
 * queues at once.
 *
 * Not the capacity: a link that carries only the others' spare measures
 * only that, and with parity a first link taken as the choked one at the
 * start (both measured at a few kbit/s) stayed there for good (the lab,
 * protect: 0 Mbit/s of TCP). Not each packet's expected arrival (eta.h):
 * that flipped to the choked link for a few packets a tick. */
#define CG_SPARE_FIRST_HYST_US 5000
#define CG_SPARE_BAN_MS 60000

/* How late a link reads, for a choice among choked links: the largest of
 * its queue (q_us, -1 not known: 0), base and lag. */
static inline uint32_t cg_spare_late(int32_t q_us, uint32_t base_us, uint32_t lag_us)
{
	uint32_t m = q_us > 0 ? (uint32_t)q_us : 0;

	if (base_us > m)
		m = base_us;
	return lag_us > m ? lag_us : m;
}

static inline int cg_spare_first(const uint32_t *base_us, uint16_t known, const int32_t *q_us, const uint32_t *lag_us,
				 unsigned n, uint16_t set, uint16_t banned, uint32_t choke_us, int cur, int hold)
{
	uint16_t cand = 0;
	int best = -1;

	n = n > 16 ? 16 : n;
	if (hold && cur >= 0 && cur < (int)n)
		return cur;
	for (unsigned i = 0; i < n; i++)
		if ((set & known & ~banned) >> i & 1 && !cg_spare_choked(q_us[i], base_us[i], lag_us[i], choke_us))
			cand |= (uint16_t)(1u << i);
	for (unsigned i = 0; i < n; i++)
		if ((cand >> i & 1) && (best < 0 || base_us[i] < base_us[best]))
			best = (int)i;
	if (best < 0) {
		if (cur >= 0 && cur < (int)n && (set >> cur & 1))
			return cur;
		for (unsigned i = 0; i < n; i++)
			if ((set >> i & 1) && (best < 0 || cg_spare_late(q_us[i], base_us[i], lag_us[i]) <
								  cg_spare_late(q_us[best], base_us[best], lag_us[best])))
				best = (int)i;
		return best;
	}
	if (cur >= 0 && cur < (int)n && cur != best && (cand >> cur & 1) &&
	    base_us[cur] <= base_us[best] + CG_SPARE_FIRST_HYST_US)
		return cur;
	return best;
}

/* The first link with one pinned (redund_spare_first, pin; -1: none):
 * why the pinned link is or is not the first. CG_SPARE_PIN_OK while it is
 * in set (live and carrying), measured (known: a fresh measure), not
 * banned and not choked; else the reason, and the automatic choice
 * (cg_spare_first) decides. A pinned link that chokes as the first for
 * CG_SPARE_CHOKE_MS is banned (cg_spare_keep), CG_SPARE_PIN_BAN_MS
 * (cg_spare_ban_ms): it comes back once that is over and it is clear. */
enum {
	CG_SPARE_PIN_NONE = 0, /* no pin */
	CG_SPARE_PIN_OK,       /* the first link */
	CG_SPARE_PIN_ABSENT,   /* no such link up (no socket) */
	CG_SPARE_PIN_DOWN,     /* not live, or not carrying (muted) */
	CG_SPARE_PIN_UNKNOWN,  /* no fresh measure of its delay */
	CG_SPARE_PIN_BANNED,
	CG_SPARE_PIN_CHOKED,
};

static inline int cg_spare_pin_why(int pin, uint16_t set, uint16_t known, uint16_t banned, int choked)
{
	if (pin < 0)
		return CG_SPARE_PIN_NONE;
	if (pin >= 16)
		return CG_SPARE_PIN_ABSENT;
	if (!(set >> pin & 1))
		return CG_SPARE_PIN_DOWN;
	if (!(known >> pin & 1))
		return CG_SPARE_PIN_UNKNOWN;
	if (banned >> pin & 1)
		return CG_SPARE_PIN_BANNED;
	return choked ? CG_SPARE_PIN_CHOKED : CG_SPARE_PIN_OK;
}

static inline int cg_spare_pinned(int pin, int auto_f, uint16_t set, uint16_t known, uint16_t banned, int choked)
{
	return cg_spare_pin_why(pin, set, known, banned, choked) == CG_SPARE_PIN_OK ? pin : auto_f;
}

/* How long a link stays banned from first: CG_SPARE_BAN_MS, the pinned
 * one CG_SPARE_PIN_BAN_MS (it was chosen by hand: back sooner, and banned
 * again if it chokes again). */
#define CG_SPARE_PIN_BAN_MS 15000

static inline uint32_t cg_spare_ban_ms(int pinned)
{
	return pinned ? CG_SPARE_PIN_BAN_MS : CG_SPARE_BAN_MS;
}

/* What becomes of the current first link this tick: CG_SPARE_KEEP_HOLD
 * while it is in a microcut (until CG_SPARE_HOLD_CUT_MS after a sample
 * losing over CG_SPARE_MIX_PM, lost; *cut_ms the last such tick, 0: none),
 * CG_SPARE_KEEP_WAIT while it is choked (cg_spare_choked) for less than
 * CG_SPARE_CHOKE_MS in a row and wait says it may (*choke_ms: since when,
 * 0: not choked; the caller clears it when the first link changes),
 * CG_SPARE_KEEP_BAN once it has been that long or at once without wait,
 * else CG_SPARE_KEEP_FREE. Both HOLD and WAIT keep it the first. wait: the
 * other links' allowance holds the whole upload (every link carries every
 * packet, the first no more than the rest: its choke is not from being
 * the first, and a ban would not help it).
 *
 * A microcut loses without a queue and without moving its base (Starlink
 * past the dish, story 017); a choked link loses to its own full queue,
 * so its loss is no microcut: the hold is cleared, or that loss renewed
 * it every tick and kept the choked link first for good. The wait is for
 * a choke that does not last: a cellular link's delay jumps for a moment
 * (a radio's promotion from idle, a handover) with or without load, and a
 * ban at the first tick of it put a pinned link out for a minute at each
 * jump (the NUC, 2026-10-23: Entel pinned, never the first on a quiet
 * tunnel; the lab, 300 ms for 1 s on it every 20 s: out 66 % of the time).
 * Under a load the others cannot copy whole the first carries more than
 * they do, and its choke is its own: banned at once, as before (waiting
 * 2 s there cost protectslow 0.6 % of its stream, against 0.2 %). */
enum { CG_SPARE_KEEP_FREE = 0, CG_SPARE_KEEP_HOLD = 1, CG_SPARE_KEEP_BAN = 2, CG_SPARE_KEEP_WAIT = 3 };

#define CG_SPARE_CHOKE_MS 2000

static inline int cg_spare_keep(int lost, int choked, int wait, uint64_t now_ms, uint64_t *cut_ms, uint64_t *choke_ms)
{
	if (choked) {
		*cut_ms = 0;
		if (!*choke_ms)
			*choke_ms = now_ms;
		if (wait && now_ms - *choke_ms < CG_SPARE_CHOKE_MS)
			return CG_SPARE_KEEP_WAIT;
		*choke_ms = 0;
		return CG_SPARE_KEEP_BAN;
	}
	*choke_ms = 0;
	if (lost)
		*cut_ms = now_ms;
	return *cut_ms && now_ms - *cut_ms < CG_SPARE_HOLD_CUT_MS ? CG_SPARE_KEEP_HOLD : CG_SPARE_KEEP_FREE;
}

/* What a packet is protected with on the other links, by mode: 1 copies,
 * 2 parity. Copies, in every mode, while the allowance holds the whole
 * upload (spare_bps at least data_bps): they cost what a parity of k = 1
 * would, rebuild without waiting, and keep the link carrying what it
 * measures (a first link taken as the choked one, its peer carrying only
 * parity, could not measure its way back: the cell, TCP, 1 run in 3);
 * mix also while the first link loses over CG_SPARE_MIX_PM (loss_pm, its
 * last sample's loss in permil; a microcut), and parity otherwise. */
static inline int cg_spare_how(int mode, uint64_t data_bps, uint64_t spare_bps, unsigned loss_pm)
{
	if (mode == CG_SPARE_COPIES || spare_bps >= data_bps)
		return CG_SPARE_COPIES;
	if (mode == CG_SPARE_PARITY)
		return mode;
	return loss_pm > CG_SPARE_MIX_PM ? CG_SPARE_COPIES : CG_SPARE_PARITY;
}

/* The erasure codes' plan (redund_spare = rs | rlc, story 021): how the
 * next packets are protected, and with what k (source packets per block,
 * or the window) and m (repairs per k). Inputs: data_bps the upload, spare_bps
 * the other links' allowance (all of it: the repairs are spread over them,
 * by their buckets), loss_pm the first link's last sample's loss and
 * avg_pm its average (permil), pps the packets that go on the first link
 * alone each second, horizon_ms how long a block may take to fill
 * (redund_spare_code_ms).
 *
 *   - copies while the allowance holds the whole upload (as every mode);
 *   - k: the packets of the horizon, in [CG_SPARE_CODE_KMIN, kmax];
 *   - the loss planned for, p: the larger of twice the average and the last
 *     sample, and at least CG_SPARE_CODE_FLOOR_PM (losses come in bunches,
 *     and a sample of a second holds a few);
 *   - m: the fewest repairs for which a block of k loses more than m with a
 *     probability under CG_SPARE_CODE_TAIL (binomial), at most mmax and k;
 *   - when that m does not fit 7/8 of the allowance (a repair costs about a
 *     packet), copies if the first link loses over CG_SPARE_MIX_PM (a
 *     microcut: the copies that fit protect what they carry, a short code
 *     nothing), or under it the whole allowance's m, rounded up (at least
 *     1, at most the need): the bucket trims the last one, and the
 *     allowance, which grows only while 3/4 of it is in use (cg_spare_tick),
 *     can grow. Rounded down, WOM's 0.63 Mbit/s held one repair per 21 of a
 *     7 Mbit/s upload (0.33 Mbit/s, half of it) and never grew: the cell,
 *     m 1 against a need of 4, 0.07 % lost outside the microcut.
 *
 * Doubles: once a tick, not per packet. */
#define CG_SPARE_CODE_TAIL 1e-4
#define CG_SPARE_CODE_FLOOR_PM 5
#define CG_SPARE_CODE_KMIN 4

struct cg_spare_plan {
	int how;        /* CG_SPARE_COPIES, or the code (CG_SPARE_RS or CG_SPARE_RLC) */
	unsigned k, m;
	unsigned need;  /* the m the loss asks for (for the status) */
};

/* P(X > m) for X ~ Bin(k, p). */
static inline double cg_spare_tail(unsigned k, unsigned m, double p)
{
	double q = 1 - p, f, cdf = 0;

	if (m >= k)
		return 0;
	f = 1;
	for (unsigned i = 0; i < k; i++)
		f *= q; /* P(X = 0) */
	for (unsigned i = 0; i <= m; i++) {
		cdf += f;
		f = f * (double)(k - i) / (double)(i + 1) * p / q;
	}
	return cdf >= 1 ? 0 : 1 - cdf;
}

static inline struct cg_spare_plan cg_spare_plan(int mode, uint64_t data_bps, uint64_t spare_bps, unsigned loss_pm,
						 unsigned avg_pm, uint64_t pps, uint32_t horizon_ms, unsigned kmax,
						 unsigned mmax)
{
	struct cg_spare_plan pl = { CG_SPARE_COPIES, 0, 0, 0 };
	uint64_t k = pps * horizon_ms / 1000, mb;
	unsigned p_pm = 2 * avg_pm > loss_pm ? 2 * avg_pm : loss_pm, top;

	if (!data_bps || spare_bps >= data_bps)
		return pl;
	if (k < CG_SPARE_CODE_KMIN)
		k = CG_SPARE_CODE_KMIN;
	if (k > kmax)
		k = kmax;
	if (p_pm < CG_SPARE_CODE_FLOOR_PM)
		p_pm = CG_SPARE_CODE_FLOOR_PM;
	if (p_pm > 900)
		p_pm = 900;
	top = (unsigned)k < mmax ? (unsigned)k : mmax;
	pl.need = top + 1;
	for (unsigned m = 1; m <= top; m++)
		if (cg_spare_tail((unsigned)k, m, p_pm / 1000.0) <= CG_SPARE_CODE_TAIL) {
			pl.need = m;
			break;
		}
	mb = k * (spare_bps * 7 / 8) / data_bps;
	pl.k = (unsigned)k;
	if (pl.need <= mb) {
		pl.m = pl.need;
	} else if (loss_pm > CG_SPARE_MIX_PM) {
		pl.k = 0;
		return pl;
	} else {
		mb = (k * spare_bps + data_bps - 1) / data_bps;
		if (mb > pl.need)
			mb = pl.need;
		pl.m = mb < 1 ? 1 : mb > top ? top : (unsigned)mb;
	}
	pl.how = mode;
	return pl;
}

#endif

