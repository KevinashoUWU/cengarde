/* The split by expected arrival (story 013, after PR 3; the bonding study,
 * story 012, axis 5: QAware's (n_k + 1) S_k, and srtla's EDPF). The split
 * by weights gives each link its share in turn, so packets sent one after
 * the other leave on links whose delays differ by tens of ms and arrive out
 * of order: the receiver's reorderer holds them, gives some gaps up, and on
 * the testnet 72 % of the download's TCP retransmissions were spurious
 * (DSACK). Here each packet goes where it would arrive first:
 *
 *   eta_i = base_i + (backlog_i + len) / rate_i
 *
 * base_i: link i's one-way delay without a queue (its windowed minimum,
 * capacity.h), relative to the smallest of them; rate_i: its capacity;
 * backlog_i: what this end has put on it that it has not carried yet, a
 * virtual queue that drains at rate_i between packets and is set again at
 * every tick from the queue the link reports (its delay over its minimum,
 * times its rate). Packets sent in order then arrive nearly in order, and a
 * link takes traffic only once the faster ones' queues would make a packet
 * arrive later than it would there: the slow links fill only when the fast
 * ones are full.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_ETA_H
#define CG_ETA_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */

#define CG_ETA_PUT_N 16 /* ticks of what was put on each link, for eta_keep 2 */

struct cg_eta_link {
	uint64_t backlog; /* bits put on it and not carried yet, times 10^6: over bps, the us they take */
	uint64_t t_us;    /* when backlog was last drained */
	uint64_t bps;     /* its rate, 0: not known */
	uint32_t base_us; /* its delay without a queue, over the smallest */
	uint32_t limit_us; /* the queue it holds before it drops (eta_buffer), 0: no limit */
	uint32_t over_us;  /* added to the arrival of a packet past limit_us, 0: CG_ETA_OVER_US */
	/* set by the caller (eta_keep, eta_keep_ms, an experiment of the
	 * clockwork audit, C9, per link): what cg_eta_set keeps of the backlog
	 * counted over a queue measured hundreds of ms ago, 0: nothing */
	uint8_t keep;
	uint16_t keep_ms;
	/* eta_keep 2: the bits put on it in each of the last CG_ETA_PUT_N
	 * ticks (times 10^6, as backlog), the newest at put_i, and in this one */
	uint8_t put_i;
	uint64_t put[CG_ETA_PUT_N], put_now;
};

/* What a packet past a link's limit_us costs by default: last resort, it
 * goes there only when every link is past its own. A link's model may set
 * less (over_us, linkmodel.h): then it goes elsewhere only while it would
 * arrive there within over_us of what it would here, and the links it
 * spills to do not fill their deep queues with it (story 015). */
#define CG_ETA_OVER_US 10000000ull

struct cg_eta {
	struct cg_eta_link l[CG_MAX_LINKS];
	uint32_t tick_ms; /* how often cg_eta_set comes, for eta_keep 2 (set by the caller) */
};

static inline void cg_eta_reset(struct cg_eta *e)
{
	memset(e, 0, sizeof(*e));
}

static inline void cg_eta_drain(struct cg_eta_link *l, uint64_t now_us)
{
	uint64_t d;

	if (now_us <= l->t_us)
		return;
	d = l->bps * (now_us - l->t_us);
	l->backlog = l->backlog > d ? l->backlog - d : 0;
	l->t_us = now_us;
}

/* When what link i holds now arrives, us behind the fastest link (its
 * delay without a queue over the smallest, and its backlog over its rate):
 * right after a pick, the packet just put there (MPGRIT step U2a, story
 * 029: GRIT's timer per segment). UINT32_MAX: no rate known. */
static inline uint32_t cg_eta_behind(const struct cg_eta *e, unsigned i)
{
	const struct cg_eta_link *l = &e->l[i];
	uint64_t us;

	if (i >= CG_MAX_LINKS || !l->bps)
		return UINT32_MAX;
	us = l->base_us + l->backlog / l->bps;
	return us < UINT32_MAX ? (uint32_t)us : UINT32_MAX - 1;
}

/* A packet of len bytes put on link i at now_us by someone else (a probe,
 * probe.h): counted in its backlog. */
static inline void cg_eta_add(struct cg_eta *e, unsigned i, uint32_t len, uint64_t now_us)
{
	cg_eta_drain(&e->l[i], now_us);
	e->l[i].backlog += (uint64_t)len * 8 * 1000000;
	e->l[i].put_now += (uint64_t)len * 8 * 1000000;
}

/* A tick's measures of link i: its rate (bps, 0: unknown), its delay without
 * a queue over the smallest of the links (us), and its queue now (us, the
 * time what is in it takes to leave).
 *
 * The queue measured is a delay report 300-500 ms old: it has not got what
 * this end sent since, and setting the backlog to it drops those bits every
 * tick (clockwork audit C9). l->keep (an experiment) keeps some of what is
 * still counted, v (the old backlog drained at the old rate up to now), over
 * the measured q: 1, the larger (the split under-used the links, -12..18 %);
 * 2, at most q plus the net growth of the last keep_ms (what was put on the
 * link then less what its rate drains in that time), the bits the report
 * cannot have seen; 3, q plus half of what v has over it, so a kept part
 * halves every tick. */
static inline void cg_eta_set(struct cg_eta *e, unsigned i, uint64_t bps, uint32_t base_us, uint32_t queue_us,
			      uint64_t now_us)
{
	struct cg_eta_link *l = &e->l[i];
	uint64_t q = bps * queue_us; /* the bits in the queue, times 10^6 */

	if (l->keep && l->bps && bps) {
		cg_eta_drain(l, now_us); /* at the old rate, up to now */
		if (l->backlog > q) {
			if (l->keep == 1) {
				q = l->backlog;
			} else if (l->keep == 2) {
				unsigned n = e->tick_ms ? (l->keep_ms + e->tick_ms - 1) / e->tick_ms : 0;
				uint64_t put = l->put_now, gone;

				n = n > CG_ETA_PUT_N ? CG_ETA_PUT_N : n;
				for (unsigned k = 1; k < n; k++)
					put += l->put[(l->put_i + CG_ETA_PUT_N - k + 1) % CG_ETA_PUT_N];
				gone = bps * ((uint64_t)l->keep_ms * 1000);
				if (put > gone)
					q = q + (put - gone) < l->backlog ? q + (put - gone) : l->backlog;
			} else {
				q += (l->backlog - q) / 2;
			}
		}
	}
	l->put_i = (uint8_t)((l->put_i + 1) % CG_ETA_PUT_N);
	l->put[l->put_i] = l->put_now;
	l->put_now = 0;
	l->bps = bps;
	l->base_us = base_us;
	l->backlog = q;
	l->limit_us = 0;
	l->over_us = 0;
	l->t_us = now_us;
}

/* Link i from its own controller (link_cc, linkcc.h), fed by its SFACKs
 * every ~10 ms: its pacing rate (bps), its delay without a queue over the
 * least (us), the bytes queued on it now (what is in flight over its rate
 * times its round trip: fresher than a tick's queue from delay reports
 * 300-500 ms old) and the queue past which it is a last resort (limit_us,
 * 0: none). */
static inline void cg_eta_own(struct cg_eta *e, unsigned i, uint64_t bps, uint32_t base_us, uint64_t queued,
			      uint32_t limit_us, uint64_t now_us)
{
	struct cg_eta_link *l = &e->l[i];

	l->bps = bps;
	l->base_us = base_us;
	l->backlog = queued * 8 * 1000000;
	l->t_us = now_us;
	l->limit_us = limit_us;
	l->over_us = 0;
}

/* The link of set where a packet of len bytes sent at now_us arrives first,
 * which then counts it in its backlog; -1 when a link of set has no rate
 * (the caller splits by weights then). */
static inline int cg_eta_pick(struct cg_eta *e, uint16_t set, uint32_t len, uint64_t now_us)
{
	uint64_t best_eta = UINT64_MAX;
	int best = -1;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct cg_eta_link *l = &e->l[i];
		uint64_t eta;

		/* a link of no rate (none yet, or its capacity sank to nothing)
		 * takes no packet while another has one, and only when none has
		 * does the caller go to the weights: a link that lost half of what
		 * it took had no rate, so every packet went by the weights, and its
		 * weight kept a quarter of the traffic on it (story 029, Claro on
		 * the testnet: 28-78 % lost, the holes held the streams) */
		if (!(set >> i & 1) || !l->bps)
			continue;
		cg_eta_drain(l, now_us);
		/* us: base + (backlog + 8 len 10^6) / bps */
		eta = l->base_us + (l->backlog + (uint64_t)len * 8 * 1000000) / l->bps;
		/* past the queue it holds it would be dropped: over_us later, last
		 * resort by default */
		if (l->limit_us && eta - l->base_us > l->limit_us)
			eta += l->over_us ? l->over_us : CG_ETA_OVER_US;
		if (eta < best_eta) {
			best_eta = eta;
			best = (int)i;
		}
	}
	if (best >= 0) {
		e->l[best].backlog += (uint64_t)len * 8 * 1000000;
		e->l[best].put_now += (uint64_t)len * 8 * 1000000;
	}
	return best;
}

#define CG_PACE_SMALL 300 /* bytes: a packet this small (an ACK, a handshake) goes on every carrier */

/* Copies (redund_pace_ms, an experiment; per link, linkmodel.h's pace_ms):
 * which of the links of set get a copy of a packet of len bytes sent at
 * now_us that goes to every carrier. The link where it arrives first always
 * does; another with a window (window_us[i], not 0) only if its copy would
 * arrive within that of the first one: a copy behind a queue that grows
 * without end (80 Mbit/s of copies into a 25 Mbit/s cellular link) arrives
 * after the receiver gave its gap up and patches nothing, and it fills the
 * queue the next copies wait behind (field, nostalgica, 2026-10-19: Entel's
 * copies 72 % late, WOM's all). A link without a window, or without a rate,
 * gets every copy. At a low load every queue is short and every link gets
 * every copy, as without it. */
/* *first (NULL: not wanted): the link where it arrives first, -1: none with a rate. */
static inline uint16_t cg_eta_copies_first(struct cg_eta *e, uint16_t set, uint32_t len, uint64_t now_us,
					   const uint32_t window_us[CG_MAX_LINKS], int *first)
{
	uint64_t at[CG_MAX_LINKS], best = UINT64_MAX, bits = (uint64_t)len * 8 * 1000000;
	uint16_t out = 0;
	int b = -1;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct cg_eta_link *l = &e->l[i];

		if (!(set >> i & 1))
			continue;
		if (!l->bps) {
			out |= (uint16_t)(1u << i);
			continue;
		}
		cg_eta_drain(l, now_us);
		at[i] = l->base_us + (l->backlog + bits) / l->bps;
		if (at[i] < best) {
			best = at[i];
			b = (int)i;
		}
	}
	for (unsigned i = 0; b >= 0 && i < CG_MAX_LINKS; i++) {
		struct cg_eta_link *l = &e->l[i];

		if (!(set >> i & 1) || !l->bps)
			continue;
		if ((int)i == b || !window_us[i] || at[i] - best <= window_us[i]) {
			l->backlog += bits;
			out |= (uint16_t)(1u << i);
		}
	}
	if (first)
		*first = b;
	return out;
}

static inline uint16_t cg_eta_copies(struct cg_eta *e, uint16_t set, uint32_t len, uint64_t now_us,
				     const uint32_t window_us[CG_MAX_LINKS])
{
	return cg_eta_copies_first(e, set, len, now_us, window_us, NULL);
}

/* A copy cg_eta_copies counted on link i that did not go after all
 * (spare.h): its bits out of the link's backlog. */
static inline void cg_eta_unadd(struct cg_eta *e, unsigned i, uint32_t len)
{
	uint64_t bits = (uint64_t)len * 8 * 1000000;

	if (i < CG_MAX_LINKS)
		e->l[i].backlog = e->l[i].backlog > bits ? e->l[i].backlog - bits : 0;
}

#endif
