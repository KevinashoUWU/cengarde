/* Client side: tying what the server sends back to the probes it answers.
 *
 * Each link keeps its last CG_ECHO_N probes (their header ts, an exact
 * microsecond value) and every probe reply or HELLO (proto.h) names the
 * probe it answers. A HELLO carries no sequence, so this is what keeps a
 * captured one from being sent again: it counts only for a probe this link
 * sent at most CG_ECHO_MAX_AGE_MS ago, and only once (a stronger proof of
 * freshness than a range: the 32-bit ts wraps every 71.6 minutes and
 * travels in clear). Replies take their entry too, so a probe is answered
 * once, by a reply or by a HELLO.
 *
 * Up to protocol 3 the ring also told a server that started over from a
 * link that lags, and the client reset its window; from protocol 4 the
 * server goes on past what the probes say the client received, so there is
 * nothing to reset.
 *
 * 6.4 s of probes every 100 ms, so an answer still matches on a link with
 * seconds of queue. Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_EPOCH_H
#define CG_EPOCH_H

#include <stdint.h>

#define CG_ECHO_N 64
#define CG_ECHO_MAX_AGE_MS 10000

struct cg_echo_ent {
	uint32_t ts;      /* header ts of the probe */
	uint32_t sent_ms; /* local clock when it went out (wraps) */
};

/* All zero is an empty ring. */
struct cg_echo {
	struct cg_echo_ent e[CG_ECHO_N];
	uint8_t pos;   /* next entry to write */
	uint64_t used; /* bit i: e[i] holds a probe no reply or HELLO has consumed */
};

/* A probe with header ts goes out at now_ms; it replaces the oldest entry. */
static inline void cg_echo_push(struct cg_echo *r, uint32_t ts, uint64_t now_ms)
{
	r->e[r->pos].ts = ts;
	r->e[r->pos].sent_ms = (uint32_t)now_ms;
	r->used |= 1ULL << r->pos;
	r->pos = (uint8_t)((r->pos + 1) % CG_ECHO_N);
}

/* Does echo ts answer a probe of this ring sent at most max_age_ms ago and
 * not answered yet? A match consumes the entry: each probe counts once. */
static inline int cg_echo_take(struct cg_echo *r, uint32_t ts, uint64_t now_ms, uint32_t max_age_ms)
{
	for (unsigned i = 0; i < CG_ECHO_N; i++)
		if ((r->used >> i & 1) && r->e[i].ts == ts && (uint32_t)now_ms - r->e[i].sent_ms <= max_age_ms) {
			r->used &= ~(1ULL << i);
			return 1;
		}
	return 0;
}

#endif
