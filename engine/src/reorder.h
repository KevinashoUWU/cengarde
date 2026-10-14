/* Receive-side reordering on the authenticated DATA sequence (story 012,
 * Fase 5 step 1). When the sender splits packets across links of different
 * delay, or the first copy of a packet comes in before that of an earlier
 * one, the receiver holds what came early, for a bounded time, and hands
 * everything on to WireGuard in sequence order. Nothing is ever dropped:
 * every packet given to cg_ro_in() comes out exactly once, through its
 * verdict (NOW), cg_ro_next() or cg_ro_flush().
 *
 * Inputs are verified first copies only (arrival.h's rule): a DATA packet
 * whose MAC verified and whose sequence the anti-replay window took as new.
 * The reorderer reads its sequence, the sender's ts and flags (all under the
 * MAC), the link it came on, its length and WireGuard type, and its arrival
 * time on the local microsecond clock (low 32 bits, as every time here).
 * Verified probes (server) and probe replies (client) give each link's
 * one-way delay through cg_ro_owd(). Duplicates never reach it.
 *
 * Per packet, d = seq - head in serial arithmetic (head: the next sequence
 * to hand on). AGAIN aside, the packet's link first takes seq as its
 * newest (hw, serial max) and its delay (t_arr - ts) into an EWMA of gain
 * 1/8; a split packet notes its arrival. Then:
 *   - the first packet starts the window after itself: NOW;
 *   - d == 0, in order: NOW (the common case, a few comparisons);
 *   - d < 0, late: NOW. If its slot still says why its gap was given up,
 *     late.after_<why> and how late in late_by (and the slot is emptied);
 *     else late.old;
 *   - d >= N (CG_RO_SLOTS) with nothing held: resync, the window starts
 *     after it (a server back 2^20 ahead, an outage longer than the window)
 *     and the marks of the old one are emptied;
 *   - N <= d < FAR while holding: AGAIN, nothing taken: the gaps before
 *     seq - N + 1 are forced out; the caller drains (cg_ro_next until 0)
 *     and calls again with the same packet, which is then taken (never a
 *     second AGAIN). The forced gaps go even once nothing is held, so an
 *     overflow moves the window just enough and never turns into a resync;
 *   - d >= FAR while holding: NOW (far) and the window stays: what is held
 *     goes at its deadlines, and the first one after that resyncs;
 *   - 0 < d < N and small (cg_ro_bypass: not WireGuard data, or data of at
 *     most bypass_len bytes): NOW, its slot PASSED so that the head steps
 *     over it later;
 *   - 0 < d < N with nothing held, when a drain at its arrival would give up
 *     the whole gap before it (the hold cap is 0, RACK, or forced): the gap
 *     is given up now and the packet goes NOW without a copy, so redundancy
 *     with dup_wait 0 holds nothing;
 *   - else HOLD in a pool buffer. With none free: NOW (pool_full), its slot
 *     PASSED, and the gap before the lowest held packet is forced out, so
 *     that buffers come back.
 *
 * Draining (cg_ro_next) walks from the head: a held packet goes on, a PASSED
 * slot is stepped over (and emptied), and a gap (a sequence not here) is
 * given up
 *   1. when forced (overflow, pool full): lost.overflow, held or not;
 *   2. never while nothing is held: a gap with nothing behind it costs
 *      nothing;
 *   3. when every link of rset brought a DATA packet after it (RACK; links
 *      are FIFO, so none of them will bring it): lost.rack;
 *   4. at the deadline: lost.cap;
 * else the walk stops. A gap given up keeps {seq, why, when} in its slot for
 * late attribution until the slot is reused. An overflow forces the gap N
 * behind the packet that forced it, whose slot that packet then takes: in a
 * steady overflow (spread x rate above N) the late copies of forced gaps
 * mostly count as late.old, so read late.old next to lost.overflow.
 *
 * No slot behind the head says PASSED, and passed_ahead counts exactly the
 * PASSED slots: every way the head leaves a mark behind (the walk, a gap
 * given up at a packet's arrival, a late packet, a resync) empties it. The
 * in-order path never writes a slot, so a slot keeps its sequence until the
 * head comes round to it 2^32 sequences later (a day or two of one session
 * at 30-50 kpps), and a stale mark would then step the head over a missing
 * packet without a wait or a count.
 *
 * Deadline of the head gap, from s, the lowest held packet above it:
 *   C  = max_wait if s is split (CG_F_SPLIT) or a split packet arrived
 *        within max_wait of s, else dup_wait;
 *   dl = min(s.ts + M + margin, s.t_arr + C), M the largest of the links
 *        of rset of their delay plus CG_RO_DEV_K times its deviation, when
 *        all of them have one; else s.t_arr + C.
 * The sender writes sequence and ts together, in order, so the gap was sent
 * no later than s.ts and the slowest link brings it by s.ts + M; the delays
 * carry the clock offset between the ends, which s.ts + M cancels. The
 * delay is a mean, and under load a cellular link's swings tens of ms above
 * it: with M the mean alone and a 10 ms margin, 4-10 % of the split packets
 * had their gap given up and then came, most within 50 ms (story 013, the
 * bench, 2026-10-14). The deviation (the mean of |sample - delay|, gain
 * 1/4, TCP's RTTVAR) widens the deadline as much as the link swings. With the
 * delays as measured, a held packet waits at most C plus how far its link
 * is ahead of the slowest (M - its own delay): a packet before it may
 * arrive that much after it and set the deadline from its own arrival. A
 * gap is due when (int32_t)(now - dl) >= 0.
 *
 * rset: the links the peer sends DATA on. One left out may give up a gap
 * early (its packets then come late); one too many only waits for the
 * deadline. The window, N = 4096, stays under the anti-replay window (8128):
 * a packet the head waits for still passes cg_replay_check, and a packet
 * whose gap was given up has 8128 - 4096 sequences to come late.
 *
 * Buffers: the caller owns the pool's memory (struct cg_ro_pool, n buffers
 * of CG_RO_BUF bytes and a stack of free indexes) and the copying.
 *   - HOLD: *buf is the packet's buffer; the caller copies the WireGuard
 *     datagram (len bytes) into cg_ro_pool_buf(p, *buf) before its next
 *     cg_ro_next() or cg_ro_flush().
 *   - cg_ro_next() or cg_ro_flush() returning 1: o->len bytes in o->buf go
 *     on, after anything handed on before; the caller gives the buffer back
 *     (cg_ro_pool_put) once the send that carried them is done, not before:
 *     a later HOLD would take it.
 * After cg_ro_in() (NOW or HOLD) the caller drains: cg_ro_next() until it
 * returns 0 (immediately when nothing is held or marked), or until it stops
 * and calls cg_ro_burst_end(). cg_ro_due_us() says when the next gap may go:
 * an epoll timeout.
 *
 * Statistics (st): in_order, held_total, bypassed, late_*, far, resyncs and
 * pool_full partition the packets taken (each counts in exactly one);
 * split_rx counts the split ones among them; lost_* count gaps; wait (how
 * long each held packet was held) and late_by (arrival minus when its gap
 * was given up) are histograms with inclusive upper edges of 1, 2, 5, 10,
 * 20, 50, 100, 200 and 500 ms and a last bucket above; max_burst is the
 * most held packets one drain handed on. A drain ends when cg_ro_next()
 * returns 0, or when the caller stops it early (a budget per pass) and says
 * so with cg_ro_burst_end(); the rest then goes in a later drain of its own.
 *
 * Pure: no clock, no I/O, no log, no allocation (docs/historias/012). The
 * instance is zeroed memory (cg_ro_init or memset); about 81 KiB.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_REORDER_H
#define CG_REORDER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "proto.h"   /* CG_F_SPLIT */
#include "replay.h"  /* CG_REPLAY_WINDOW */

#define CG_RO_SLOTS 4096u               /* the window: a power of two, under CG_REPLAY_WINDOW */
#define CG_RO_FAR (2u * CG_RO_SLOTS)    /* a packet this far ahead, while holding, passes through */
#define CG_RO_WORDS (CG_RO_SLOTS / 64)  /* of the held bitmap */
#define CG_RO_BUF 2048u                 /* bytes per pool buffer: CG_BUF (engine.h) */
#define CG_RO_POOL_MAX 65535u           /* buffers per pool: indexes are 16-bit */
#define CG_RO_NOBUF 0xffffu
#define CG_RO_HIST 10                   /* buckets of wait and late_by */
#define CG_RO_WG_DATA 4                 /* WireGuard's data message type */
#define CG_RO_DEV_K 4                   /* deviations of a link's delay a gap waits beyond it */
/* A link's newest sequence this far behind the head, or a split packet this
 * long ago (us), is forgotten before serial arithmetic would read it as
 * ahead again. */
#define CG_RO_STALE (1u << 30)

_Static_assert(!(CG_RO_SLOTS & (CG_RO_SLOTS - 1)), "CG_RO_SLOTS is a power of two");
_Static_assert(CG_RO_SLOTS < CG_REPLAY_WINDOW, "the head's packet must pass the anti-replay window");

enum cg_ro_state {
	CG_RO_EMPTY = 0,
	CG_RO_HELD,
	CG_RO_PASSED,    /* handed on ahead (bypass, pool full) or late */
	CG_RO_LOST_RACK, /* a gap given up, and why */
	CG_RO_LOST_CAP,
	CG_RO_LOST_OVF,
};

enum cg_ro_v {
	CG_RO_NOW = 0, /* hand the packet on now, from where it is */
	CG_RO_HOLD,    /* copy it into *buf: it goes later, through cg_ro_next */
	CG_RO_AGAIN,   /* not taken: drain, then call again with the same packet */
};

struct cg_ro_cfg {
	uint32_t max_wait_us; /* hold cap for split traffic */
	uint32_t dup_wait_us; /* hold cap otherwise; at most max_wait_us */
	uint32_t margin_us;   /* added to the slowest link's delay */
	uint32_t bypass_len;  /* WireGuard data up to this length is never held */
};

struct cg_ro_pkt {
	uint32_t seq, ts; /* verified header */
	uint32_t t_arr;   /* local arrival time */
	uint16_t len;     /* of the WireGuard datagram */
	uint8_t link;     /* ids from CG_MAX_LINKS up take no per-link state */
	uint8_t flags;    /* verified header flags */
	uint8_t wgtype;   /* the datagram's first byte, 0 when empty */
};

struct cg_ro_out {
	uint32_t seq;
	uint16_t buf, len;
};

struct cg_ro_slot {
	uint32_t seq;
	uint32_t ts;    /* HELD: the sender's */
	uint32_t t_arr; /* HELD: arrival; LOST_*: when the gap was given up */
	uint16_t len;
	uint16_t buf;
	uint8_t state;
	uint8_t link;
	uint8_t flags;
	uint8_t pad;
};
_Static_assert(sizeof(struct cg_ro_slot) == 20, "cg_ro_slot is 20 bytes");

struct cg_ro_link {
	uint32_t hw;  /* newest DATA sequence it brought (serial max) */
	uint32_t owd; /* its delay, raw (local clock minus the sender's ts), EWMA 1/8 */
	uint32_t dev; /* mean of |sample - owd|, gain 1/4 (us) */
	uint8_t hw_ok, owd_ok;
};

struct cg_ro_stats {
	uint64_t in_order, held_total, bypassed, split_rx;
	uint64_t lost_rack, lost_cap, lost_overflow, lost_flush;
	uint64_t late_rack, late_cap, late_overflow, late_old;
	uint64_t far, resyncs, pool_full;
	uint32_t held_max, max_burst;
	uint64_t wait[CG_RO_HIST], late_by[CG_RO_HIST];
};

struct cg_reorder {
	uint32_t head;         /* next sequence to hand on (with init) */
	uint32_t held;         /* HELD slots */
	uint32_t passed_ahead; /* PASSED slots from head on */
	uint32_t force_to;     /* with force: gaps before it go at once */
	uint32_t last_split_t; /* with split_seen: arrival of the newest split packet */
	uint32_t burst;        /* held packets handed on by this drain */
	uint16_t tick;         /* packets taken, for the stale sweep */
	uint8_t init, force, split_seen;
	struct cg_ro_link link[CG_MAX_LINKS];
	struct cg_ro_stats st;
	uint64_t held_bits[CG_RO_WORDS]; /* HELD slots, by slot index */
	struct cg_ro_slot slot[CG_RO_SLOTS];
};

struct cg_ro_pool {
	uint8_t *mem;    /* n x CG_RO_BUF bytes */
	uint16_t *stack; /* n entries; the free buffers are the first nfree, newest last */
	uint32_t n, nfree;
	uint64_t full;   /* gets that found no buffer */
};

/* ---- pool ---- */

/* n <= CG_RO_POOL_MAX buffers in mem (n x CG_RO_BUF bytes), with stack (n
 * entries). LIFO: the buffer given back last is taken first, so a pool that
 * is seldom used touches few pages. Returns 0, or -1 when n is too large. */
static inline int cg_ro_pool_init(struct cg_ro_pool *p, uint8_t *mem, uint16_t *stack, uint32_t n)
{
	memset(p, 0, sizeof(*p));
	if (n > CG_RO_POOL_MAX)
		return -1;
	p->mem = mem;
	p->stack = stack;
	p->n = p->nfree = n;
	for (uint32_t i = 0; i < n; i++)
		stack[i] = (uint16_t)(n - 1 - i);
	return 0;
}

static inline uint8_t *cg_ro_pool_buf(const struct cg_ro_pool *p, uint16_t b)
{
	return p->mem + (size_t)b * CG_RO_BUF;
}

/* A free buffer, or CG_RO_NOBUF. */
static inline uint16_t cg_ro_pool_get(struct cg_ro_pool *p)
{
	if (!p->nfree) {
		p->full++;
		return CG_RO_NOBUF;
	}
	return p->stack[--p->nfree];
}

/* Gives b back. Returns -1, changing nothing, for an index that cannot be
 * outstanding (a caller's bug). */
static inline int cg_ro_pool_put(struct cg_ro_pool *p, uint16_t b)
{
	if (b >= p->n || p->nfree >= p->n)
		return -1;
	p->stack[p->nfree++] = b;
	return 0;
}

/* ---- pieces ---- */

static inline void cg_ro_init(struct cg_reorder *r)
{
	memset(r, 0, sizeof(*r));
}

/* Upper edge of histogram bucket i (i < CG_RO_HIST - 1), ms. */
static inline uint32_t cg_ro_edge_ms(unsigned i)
{
	static const uint16_t edge[CG_RO_HIST - 1] = { 1, 2, 5, 10, 20, 50, 100, 200, 500 };

	return edge[i];
}

/* The bucket of us microseconds: the first edge it does not pass. */
static inline unsigned cg_ro_bucket(uint32_t us)
{
	unsigned i = 0;

	while (i < CG_RO_HIST - 1 && us > cg_ro_edge_ms(i) * 1000u)
		i++;
	return i;
}

/* later - earlier, 0 when negative (the hub stamps batches out of order). */
static inline void cg_ro_hist_add(uint64_t h[CG_RO_HIST], uint32_t later, uint32_t earlier)
{
	int32_t d = (int32_t)(later - earlier);

	h[cg_ro_bucket(d > 0 ? (uint32_t)d : 0)]++;
}

/* Never held, whatever the window: what is not WireGuard data (handshakes
 * above all), and data of at most bypass_len bytes (keepalives, pure ACKs).
 * len is the WireGuard datagram's, its 32 bytes of overhead included. */
static inline int cg_ro_bypass(const struct cg_ro_cfg *cf, uint8_t wgtype, uint32_t len)
{
	return wgtype != CG_RO_WG_DATA || len <= cf->bypass_len;
}

static inline void cg_ro_owd_sample(struct cg_ro_link *l, uint32_t raw)
{
	if (!l->owd_ok) {
		l->owd = raw;
		l->dev = 0;
		l->owd_ok = 1;
	} else {
		int32_t d = (int32_t)(raw - l->owd);
		uint32_t a = d < 0 ? (uint32_t)-(int64_t)d : (uint32_t)d;

		/* The deviation first, against the delay before this sample. */
		l->dev = l->dev - l->dev / 4 + a / 4;
		l->owd += (uint32_t)(d / 8); /* divided, never shifted: it may be negative */
	}
}

/* A verified control message's delay on link: arrival minus its ts. */
static inline void cg_ro_owd(struct cg_reorder *r, unsigned link, uint32_t raw_owd)
{
	if (link < CG_MAX_LINKS)
		cg_ro_owd_sample(&r->link[link], raw_owd);
}

/* Every 65536 packets: links whose newest sequence fell CG_RO_STALE behind
 * the head (a link in redundancy that is never first) lose it before serial
 * arithmetic would put it ahead. */
static inline void cg_ro_sweep(struct cg_reorder *r)
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if (r->link[i].hw_ok && (int32_t)(r->head - r->link[i].hw) > (int32_t)CG_RO_STALE)
			r->link[i].hw_ok = 0;
}

/* What every packet taken teaches. */
static inline void cg_ro_learn(struct cg_reorder *r, const struct cg_ro_pkt *k)
{
	if (k->link < CG_MAX_LINKS) {
		struct cg_ro_link *l = &r->link[k->link];

		if (!l->hw_ok || (int32_t)(k->seq - l->hw) > 0) {
			l->hw = k->seq;
			l->hw_ok = 1;
		}
		cg_ro_owd_sample(l, k->t_arr - k->ts);
	}
	if (k->flags & CG_F_SPLIT) {
		r->last_split_t = k->t_arr;
		r->split_seen = 1;
		r->st.split_rx++;
	} else if (r->split_seen && (int32_t)(k->t_arr - r->last_split_t) > (int32_t)CG_RO_STALE) {
		r->split_seen = 0;
	}
	if (!++r->tick && r->init)
		cg_ro_sweep(r);
}

static inline int cg_ro_forced(const struct cg_reorder *r, uint32_t g)
{
	return r->force && (int32_t)(r->force_to - g) > 0;
}

/* Gaps before to go at once (keeps a further force_to). */
static inline void cg_ro_force_upto(struct cg_reorder *r, uint32_t to)
{
	if (!r->force || (int32_t)(to - r->force_to) > 0)
		r->force_to = to;
	r->force = 1;
}

/* After the head moved. */
static inline void cg_ro_moved(struct cg_reorder *r)
{
	if (r->force && (int32_t)(r->force_to - r->head) <= 0)
		r->force = 0;
}

/* RACK: 1 and *lim when every link of rset brought a sequence after head,
 * so that every gap before *lim is lost (links are FIFO); 0 when rset is
 * empty or a link of it brought nothing after head. */
static inline int cg_ro_rack(const struct cg_reorder *r, uint16_t rset, uint32_t *lim)
{
	int32_t min = INT32_MAX;

	if (!rset)
		return 0;
	for (unsigned i = 0; rset; i++, rset >>= 1) {
		int32_t a;

		if (!(rset & 1))
			continue;
		if (!r->link[i].hw_ok)
			return 0;
		a = (int32_t)(r->link[i].hw - r->head);
		if (a < min)
			min = a;
	}
	if (min <= 0)
		return 0;
	*lim = r->head + (uint32_t)min;
	return 1;
}

/* How late a link may bring a packet beyond its mean delay. */
static inline uint32_t cg_ro_swing(const struct cg_ro_link *l)
{
	return l->dev > UINT32_MAX / CG_RO_DEV_K / 2 ? UINT32_MAX / 2 : CG_RO_DEV_K * l->dev;
}

/* M: the largest delay plus swing of the links of rset, compared wrap-safe
 * against the first's delay. 0 when rset is empty or a link of it has none. */
static inline int cg_ro_owd_max(const struct cg_reorder *r, uint16_t rset, uint32_t *m)
{
	uint32_t ref = 0;
	int64_t top = 0;
	int any = 0;

	for (unsigned i = 0; rset; i++, rset >>= 1) {
		int64_t v;

		if (!(rset & 1))
			continue;
		if (!r->link[i].owd_ok)
			return 0;
		if (!any) {
			ref = r->link[i].owd;
			any = 1;
		}
		v = (int64_t)(int32_t)(r->link[i].owd - ref) + cg_ro_swing(&r->link[i]);
		if (v > top)
			top = v;
	}
	if (top > INT32_MAX)
		top = INT32_MAX;
	if (!any)
		return 0;
	*m = ref + (uint32_t)top;
	return 1;
}

/* C for a held packet with these flags, arrived at t_arr. */
static inline uint32_t cg_ro_cap(const struct cg_reorder *r, const struct cg_ro_cfg *cf, uint8_t flags,
				 uint32_t t_arr)
{
	if (flags & CG_F_SPLIT)
		return cf->max_wait_us;
	if (r->split_seen &&
	    (t_arr - r->last_split_t < cf->max_wait_us || r->last_split_t - t_arr < cf->max_wait_us))
		return cf->max_wait_us;
	return cf->dup_wait_us;
}

/* dl of a gap whose lowest held packet above is (ts, t_arr), with cap c. */
static inline uint32_t cg_ro_deadline(const struct cg_reorder *r, const struct cg_ro_cfg *cf, uint16_t rset,
				      uint32_t ts, uint32_t t_arr, uint32_t c)
{
	uint32_t dl = t_arr + c, m;

	if (cg_ro_owd_max(r, rset, &m)) {
		uint32_t e = ts + m + cf->margin_us;

		if ((int32_t)(e - dl) < 0)
			dl = e;
	}
	return dl;
}

/* The lowest held sequence (held > 0): the first HELD bit from the head's
 * slot on, around once. */
static inline uint32_t cg_ro_lowest(const struct cg_reorder *r)
{
	uint32_t from = r->head % CG_RO_SLOTS, w = from / 64;
	uint64_t m = r->held_bits[w] & (~0ULL << (from % 64));

	for (unsigned i = 0; i <= CG_RO_WORDS && !m; i++) {
		w = (w + 1) % CG_RO_WORDS;
		m = r->held_bits[w];
	}
	return r->head + ((w * 64 + (uint32_t)__builtin_ctzll(m) - from) % CG_RO_SLOTS);
}

static inline const struct cg_ro_slot *cg_ro_lowest_slot(const struct cg_reorder *r)
{
	return &r->slot[cg_ro_lowest(r) % CG_RO_SLOTS];
}

/* Gives up the gap g, for why, at when. */
static inline void cg_ro_lose(struct cg_reorder *r, uint32_t g, uint8_t why, uint32_t when)
{
	struct cg_ro_slot *s = &r->slot[g % CG_RO_SLOTS];

	s->seq = g;
	s->state = why;
	s->t_arr = when;
	if (why == CG_RO_LOST_RACK)
		r->st.lost_rack++;
	else if (why == CG_RO_LOST_CAP)
		r->st.lost_cap++;
	else
		r->st.lost_overflow++;
}

/* Marks slot s (k's) as handed on ahead of the head. */
static inline void cg_ro_pass(struct cg_reorder *r, struct cg_ro_slot *s, uint32_t seq)
{
	s->seq = seq;
	s->state = CG_RO_PASSED;
	r->passed_ahead++;
}

/* Empties the marks ahead before the window leaves them without stepping
 * over them (a resync): left there, each would say "handed on" when the head
 * comes round to its sequence 2^32 later. At most N slots, on a rare event,
 * and none to look at when there are no marks. */
static inline void cg_ro_unmark(struct cg_reorder *r)
{
	for (uint32_t g = r->head; r->passed_ahead && g != r->head + CG_RO_SLOTS; g++) {
		struct cg_ro_slot *s = &r->slot[g % CG_RO_SLOTS];

		if (s->seq == g && s->state == CG_RO_PASSED) {
			s->state = CG_RO_EMPTY;
			r->passed_ahead--;
		}
	}
	r->passed_ahead = 0;
}

/* Ends a drain: the held packets it handed on count for max_burst, and the
 * next drain counts from 0. cg_ro_next() returning 0 ends one; a caller that
 * stops before that (a budget per pass) calls this, or the rest, handed on
 * in later passes, would count as the same drain. */
static inline void cg_ro_burst_end(struct cg_reorder *r)
{
	if (r->burst > r->st.max_burst)
		r->st.max_burst = r->burst;
	r->burst = 0;
}

static inline enum cg_ro_v cg_ro_late(struct cg_reorder *r, const struct cg_ro_pkt *k)
{
	struct cg_ro_slot *s = &r->slot[k->seq % CG_RO_SLOTS];

	if (s->seq != k->seq || s->state < CG_RO_LOST_RACK) {
		r->st.late_old++;
		return CG_RO_NOW;
	}
	if (s->state == CG_RO_LOST_RACK)
		r->st.late_rack++;
	else if (s->state == CG_RO_LOST_CAP)
		r->st.late_cap++;
	else
		r->st.late_overflow++;
	cg_ro_hist_add(r->st.late_by, k->t_arr, s->t_arr);
	/* Not PASSED: behind the head a mark would outlive its sequence (see
	 * the top). EMPTY says as much to a second copy: old. */
	s->state = CG_RO_EMPTY;
	return CG_RO_NOW;
}

/* Nothing held, 0 < d < N: when a drain at k's arrival would give up the
 * whole gap [head, k->seq), it does so now and returns 1 (k goes NOW). */
static inline int cg_ro_skip(struct cg_reorder *r, const struct cg_ro_cfg *cf, const struct cg_ro_pkt *k,
			     uint16_t rset)
{
	uint32_t lim = 0, last = k->seq - 1, c;
	int rack = cg_ro_rack(r, rset, &lim);

	if (!cg_ro_forced(r, last) && !(rack && (int32_t)(lim - last) > 0)) {
		c = cg_ro_cap(r, cf, k->flags, k->t_arr);
		if (c && (int32_t)(k->t_arr - cg_ro_deadline(r, cf, rset, k->ts, k->t_arr, c)) < 0)
			return 0;
	}
	for (uint32_t g = r->head; g != k->seq; g++) {
		struct cg_ro_slot *s = &r->slot[g % CG_RO_SLOTS];

		if (s->seq == g && s->state == CG_RO_PASSED) {
			s->state = CG_RO_EMPTY;
			r->passed_ahead--;
		} else {
			cg_ro_lose(r, g,
				   cg_ro_forced(r, g)                   ? CG_RO_LOST_OVF
				   : rack && (int32_t)(lim - g) > 0 ? CG_RO_LOST_RACK
									: CG_RO_LOST_CAP,
				   k->t_arr);
		}
	}
	r->head = k->seq + 1;
	r->st.in_order++;
	cg_ro_moved(r);
	return 1;
}

/* ---- the reorderer ---- */

/* One verified first copy. rset: the links the peer sends DATA on. On HOLD
 * *buf is its buffer (copy it in); otherwise *buf is CG_RO_NOBUF. */
static inline enum cg_ro_v cg_ro_in(struct cg_reorder *r, struct cg_ro_pool *p, const struct cg_ro_cfg *cf,
				    const struct cg_ro_pkt *k, uint16_t rset, uint16_t *buf)
{
	int32_t d = (int32_t)(k->seq - r->head);
	struct cg_ro_slot *s;

	*buf = CG_RO_NOBUF;
	if (d == 0 && r->init) {
		cg_ro_learn(r, k);
		r->head++;
		r->st.in_order++;
		cg_ro_moved(r);
		return CG_RO_NOW;
	}
	if (r->init && r->held && d >= (int32_t)CG_RO_SLOTS && d < (int32_t)CG_RO_FAR) {
		cg_ro_force_upto(r, k->seq - CG_RO_SLOTS + 1);
		return CG_RO_AGAIN;
	}
	cg_ro_learn(r, k);
	if (!r->init) {
		r->init = 1;
		r->head = k->seq + 1;
		r->st.in_order++;
		return CG_RO_NOW;
	}
	if (d < 0)
		return cg_ro_late(r, k);
	if (d >= (int32_t)CG_RO_SLOTS) {
		if (r->held) {
			r->st.far++;
			return CG_RO_NOW;
		}
		r->st.resyncs++;
		cg_ro_unmark(r);
		r->head = k->seq + 1;
		r->force = 0;
		return CG_RO_NOW;
	}
	s = &r->slot[k->seq % CG_RO_SLOTS];
	if (cg_ro_bypass(cf, k->wgtype, k->len)) {
		cg_ro_pass(r, s, k->seq);
		r->st.bypassed++;
		return CG_RO_NOW;
	}
	if (!r->held && cg_ro_skip(r, cf, k, rset))
		return CG_RO_NOW;
	*buf = cg_ro_pool_get(p);
	if (*buf == CG_RO_NOBUF) {
		cg_ro_pass(r, s, k->seq);
		r->st.pool_full++;
		if (r->held)
			cg_ro_force_upto(r, cg_ro_lowest(r));
		return CG_RO_NOW;
	}
	s->seq = k->seq;
	s->ts = k->ts;
	s->t_arr = k->t_arr;
	s->len = k->len;
	s->buf = *buf;
	s->state = CG_RO_HELD;
	s->link = k->link;
	s->flags = k->flags;
	r->held_bits[(k->seq % CG_RO_SLOTS) / 64] |= 1ULL << (k->seq % 64);
	r->held++;
	r->st.held_total++;
	if (r->held > r->st.held_max)
		r->st.held_max = r->held;
	return CG_RO_HOLD;
}

/* The next held packet that may go on at now, in *o: 1, or 0 when the walk
 * stops (call it until then). */
static inline int cg_ro_next(struct cg_reorder *r, const struct cg_ro_cfg *cf, uint16_t rset, uint32_t now,
			     struct cg_ro_out *o)
{
	uint32_t lim = 0;
	int rack = -1, due = -1; /* worked out once per call, at the first gap that needs them */

	while (r->held || r->passed_ahead || r->force) {
		uint32_t g = r->head;
		struct cg_ro_slot *s = &r->slot[g % CG_RO_SLOTS];
		uint8_t why;

		if (s->seq == g && s->state == CG_RO_HELD) {
			o->seq = g;
			o->buf = s->buf;
			o->len = s->len;
			r->held_bits[(g % CG_RO_SLOTS) / 64] &= ~(1ULL << (g % 64));
			r->held--;
			s->state = CG_RO_EMPTY;
			r->head++;
			cg_ro_moved(r);
			cg_ro_hist_add(r->st.wait, now, s->t_arr);
			r->burst++;
			return 1;
		}
		if (s->seq == g && s->state == CG_RO_PASSED) {
			s->state = CG_RO_EMPTY;
			r->passed_ahead--;
			r->head++;
			cg_ro_moved(r);
			continue;
		}
		if (cg_ro_forced(r, g)) {
			why = CG_RO_LOST_OVF;
		} else if (!r->held) {
			break;
		} else {
			if (rack < 0)
				rack = cg_ro_rack(r, rset, &lim);
			if (rack && (int32_t)(lim - g) > 0) {
				why = CG_RO_LOST_RACK;
			} else {
				if (due < 0) {
					const struct cg_ro_slot *n = cg_ro_lowest_slot(r);
					uint32_t dl = cg_ro_deadline(r, cf, rset, n->ts, n->t_arr,
								     cg_ro_cap(r, cf, n->flags, n->t_arr));

					due = (int32_t)(now - dl) >= 0;
				}
				if (!due)
					break;
				why = CG_RO_LOST_CAP;
			}
		}
		cg_ro_lose(r, g, why, now);
		r->head++;
		cg_ro_moved(r);
	}
	cg_ro_burst_end(r);
	return 0;
}

/* Microseconds from now until cg_ro_next may hand something on: INT32_MAX
 * when nothing is held, <= 0 when it may now. */
static inline int32_t cg_ro_due_us(const struct cg_reorder *r, const struct cg_ro_cfg *cf, uint16_t rset,
				   uint32_t now)
{
	const struct cg_ro_slot *s;
	uint32_t lim;
	int32_t left;

	if (!r->held)
		return INT32_MAX;
	s = &r->slot[r->head % CG_RO_SLOTS];
	if (s->seq == r->head && (s->state == CG_RO_HELD || s->state == CG_RO_PASSED))
		return 0;
	if (cg_ro_forced(r, r->head) || cg_ro_rack(r, rset, &lim))
		return 0;
	s = cg_ro_lowest_slot(r);
	left = (int32_t)(cg_ro_deadline(r, cf, rset, s->ts, s->t_arr, cg_ro_cap(r, cf, s->flags, s->t_arr)) - now);
	return left == INT32_MAX ? INT32_MAX - 1 : left;
}

/* Everything held, in order (session gone, reorder turned off): 1 with the
 * next in *o, until 0. The gaps before held packets go as lost_flush. Then
 * the window starts again with the next packet (marks ahead and slots are
 * cleared). The links' delays and the statistics stay. Their newest
 * sequences (RACK) and the newest split packet are forgotten: nothing sweeps
 * them while the window is off, and 2^31 sequences later a link's newest
 * reads as ahead of any head (RACK would give up every gap at once until the
 * sequence came round), as the 32-bit clock brings an old split packet back
 * within max_wait every 72 minutes. */
static inline int cg_ro_flush(struct cg_reorder *r, struct cg_ro_out *o)
{
	if (!r->init)
		return 0;
	while (r->held) {
		uint32_t g = r->head++;
		struct cg_ro_slot *s = &r->slot[g % CG_RO_SLOTS];

		if (s->seq == g && s->state == CG_RO_HELD) {
			o->seq = g;
			o->buf = s->buf;
			o->len = s->len;
			r->held_bits[(g % CG_RO_SLOTS) / 64] &= ~(1ULL << (g % 64));
			r->held--;
			s->state = CG_RO_EMPTY;
			return 1;
		}
		if (s->seq == g && s->state == CG_RO_PASSED)
			r->passed_ahead--;
		else
			r->st.lost_flush++;
	}
	r->init = 0;
	r->head = 0;
	r->passed_ahead = 0;
	r->force = 0;
	r->force_to = 0;
	r->split_seen = 0;
	cg_ro_burst_end(r);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		r->link[i].hw_ok = 0;
	memset(r->held_bits, 0, sizeof(r->held_bits));
	memset(r->slot, 0, sizeof(r->slot));
	return 0;
}

#endif
