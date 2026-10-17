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

struct cg_eta_link {
	uint64_t backlog; /* bits put on it and not carried yet, times 10^6: over bps, the us they take */
	uint64_t t_us;    /* when backlog was last drained */
	uint64_t bps;     /* its rate, 0: not known */
	uint32_t base_us; /* its delay without a queue, over the smallest */
};

struct cg_eta {
	struct cg_eta_link l[CG_MAX_LINKS];
};

static inline void cg_eta_reset(struct cg_eta *e)
{
	memset(e, 0, sizeof(*e));
}

/* A tick's measures of link i: its rate (bps, 0: unknown), its delay without
 * a queue over the smallest of the links (us), and its queue now (us, the
 * time what is in it takes to leave). */
static inline void cg_eta_set(struct cg_eta *e, unsigned i, uint64_t bps, uint32_t base_us, uint32_t queue_us,
			      uint64_t now_us)
{
	struct cg_eta_link *l = &e->l[i];

	l->bps = bps;
	l->base_us = base_us;
	l->backlog = bps * queue_us; /* the bits in the queue, times 10^6 */
	l->t_us = now_us;
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

/* A packet of len bytes put on link i at now_us by someone else (a probe,
 * probe.h): counted in its backlog. */
static inline void cg_eta_add(struct cg_eta *e, unsigned i, uint32_t len, uint64_t now_us)
{
	cg_eta_drain(&e->l[i], now_us);
	e->l[i].backlog += (uint64_t)len * 8 * 1000000;
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

		if (!(set >> i & 1))
			continue;
		if (!l->bps)
			return -1;
		cg_eta_drain(l, now_us);
		/* us: base + (backlog + 8 len 10^6) / bps */
		eta = l->base_us + (l->backlog + (uint64_t)len * 8 * 1000000) / l->bps;
		if (eta < best_eta) {
			best_eta = eta;
			best = (int)i;
		}
	}
	if (best >= 0)
		e->l[best].backlog += (uint64_t)len * 8 * 1000000;
	return best;
}

#endif
