/* Client side: telling a server that started over from a link that lags.
 *
 * A restarted server opens the session again with a new random sequence.
 * About half the time it lands more than the anti-replay window behind what
 * the client marked last, and every packet down is then OLD for good. An OLD
 * packet alone does not prove a restart: probe replies share the server's
 * sequence with DATA, so a link more than CG_REPLAY_WINDOW packets behind the
 * others delivers OLD replies too, while the other links still deliver NEW.
 *
 * So the window is reset only on an OLD probe reply that (cg_restart_reply):
 * 1. verifies (MAC);
 * 2. answers a probe this link sent at most CG_ECHO_MAX_AGE_MS ago, whose
 *    entry in the link's echo ring it consumes (an exact microsecond ts is a
 *    stronger proof of freshness than a range: the 32-bit ts wraps every
 *    71.6 minutes and travels in clear);
 * 3. comes after no verified NEW packet on any link for a threshold, which
 *    the client sets at 2 x probe_idle_ms.
 * A DATA packet never resets the window.
 *
 * The ring holds the last CG_ECHO_N probes of a link: 6.4 s of probes every
 * 100 ms, so a reply still matches on a link with seconds of queue. Pure
 * functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_EPOCH_H
#define CG_EPOCH_H

#include <stdint.h>

#include "proto.h"
#include "replay.h"

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
	uint64_t used; /* bit i: e[i] holds a probe no reply has consumed */
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

/* Whether a packet the window judged v proves that the server started over,
 * so that the client resets its window and takes the packet as NEW.
 * echo_found: cg_echo_take() on the link it came from, only after the MAC
 * verified; ms_since_new_any: since the newest verified NEW packet on any
 * link. */
static inline int cg_restart_reply(enum cg_replay_verdict v, uint8_t type, int mac_ok, int echo_found,
				   uint64_t ms_since_new_any, uint32_t threshold_ms)
{
	return v == CG_RP_OLD && type == CG_T_PROBE_REPLY && mac_ok && echo_found &&
	       ms_since_new_any >= threshold_ms;
}

#endif
