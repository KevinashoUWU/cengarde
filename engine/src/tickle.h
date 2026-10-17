/* Tickling a link the split leaves idle (BBR's ProbeBW): the split by
 * expected arrival (eta.h) sends a link nothing while the faster ones'
 * queues are short, and a link sent nothing is never
 * measured again (capacity.h): on the testnet's download WOM, with n78
 * behind it, carried 0 % of an RTMP stream for minutes at a capacity of
 * 9.5 Mbit/s measured once (story 013). Every every_us, the next link (in
 * turn) that is due one, measured but not loaded for CG_CAP_STALE_MS
 * (cg_cap_stale_p), gets for CG_TICKLE_WIN_US the packets the split sends,
 * up to a rate: what cg_cap_probe offers a link whose queue is short (5/4 of
 * its capacity and 1/16 of the largest link's), at least CG_TICKLE_MIN_BPS.
 * The window spans a capacity sample (CG_CAP_MIN_MS), so the sample that
 * follows sees it loaded: its capacity moves to what it delivers, up if it
 * carries more, and its age (cg_cap_stale) starts again.
 *
 * Only packets the split would send one link anyway are taken, and only up
 * to the rate (a token bucket of at most CG_TICKLE_BURST_US of it), so a
 * probe costs the flow at most that share of reordering for 600 ms, one
 * link at a time.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TICKLE_H
#define CG_TICKLE_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */

#define CG_TICKLE_WIN_US 600000u  /* a probe lasts this long: over a capacity sample */
#define CG_TICKLE_MIN_BPS 1000000u /* a probe offers at least this */
#define CG_TICKLE_BURST_US 20000u  /* the bucket holds at most this long of the rate */

struct cg_tickle {
	int8_t link;      /* the link probed now, -1: none */
	uint8_t next;     /* where the turn goes on from */
	uint64_t until_us; /* the probe's end */
	uint64_t next_us;  /* the next probe, at the earliest */
	uint64_t t_us;     /* the bucket's last refill */
	uint64_t bps;      /* the probe's rate */
	uint64_t tokens;   /* bits, times 10^6 */
	uint64_t count;    /* probes started, for the status */
	uint64_t pkts;     /* packets sent by probes */
};

static inline void cg_tickle_reset(struct cg_tickle *p)
{
	memset(p, 0, sizeof(*p));
	p->link = -1;
}

/* One tick at now_us: ends a probe past its window; starts one, every_us
 * after the last (0: never), on the next link of due (in turn) at bps[link]
 * (at least CG_TICKLE_MIN_BPS). */
static inline void cg_tickle_tick(struct cg_tickle *p, uint64_t now_us, uint64_t every_us, uint16_t due,
				 const uint64_t bps[CG_MAX_LINKS])
{
	if (p->link >= 0 && now_us >= p->until_us)
		p->link = -1;
	if (p->link >= 0 || !every_us || !due || now_us < p->next_us)
		return;
	for (unsigned k = 0; k < CG_MAX_LINKS; k++) {
		unsigned i = (p->next + k) % CG_MAX_LINKS;

		if (!(due >> i & 1))
			continue;
		p->link = (int8_t)i;
		p->next = (uint8_t)((i + 1) % CG_MAX_LINKS);
		p->bps = bps[i] > CG_TICKLE_MIN_BPS ? bps[i] : CG_TICKLE_MIN_BPS;
		p->tokens = 0;
		p->t_us = now_us;
		p->until_us = now_us + CG_TICKLE_WIN_US;
		p->next_us = now_us + every_us;
		p->count++;
		return;
	}
}

/* The probed link, when it is in set and its bucket holds a packet of len
 * bytes (which it then takes); -1: the split picks as usual. */
static inline int cg_tickle_take(struct cg_tickle *p, uint16_t set, uint32_t len, uint64_t now_us)
{
	uint64_t cost = (uint64_t)len * 8 * 1000000, cap;

	if (p->link < 0 || now_us >= p->until_us || !(set >> p->link & 1))
		return -1;
	if (now_us > p->t_us) {
		cap = p->bps * CG_TICKLE_BURST_US;
		p->tokens += p->bps * (now_us - p->t_us);
		if (p->tokens > cap)
			p->tokens = cap > cost ? cap : cost;
		p->t_us = now_us;
	}
	if (p->tokens < cost)
		return -1;
	p->tokens -= cost;
	p->pkts++;
	return p->link;
}

#endif
