/* Duplicate suppression and anti-replay window over authenticated sequence
 * numbers (same layout as WireGuard's filter: a ring of 64-bit blocks).
 *
 * The receiver calls cg_replay_check() before verifying the MAC and
 * cg_replay_mark() only after the MAC verified, so a forged packet can never
 * mark a sequence number and shadow the genuine copy. A sequence already
 * marked is a duplicate and can be dropped without paying for the MAC.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_REPLAY_H
#define CG_REPLAY_H

#include <stdint.h>
#include <string.h>

#define CG_REPLAY_BITS 8192
#define CG_REPLAY_BLOCKS (CG_REPLAY_BITS / 64)
#define CG_REPLAY_WINDOW (CG_REPLAY_BITS - 64) /* sequences accepted behind the top */

struct cg_replay {
	uint32_t top; /* highest sequence marked */
	uint8_t init;
	uint64_t ring[CG_REPLAY_BLOCKS];
};

enum cg_replay_verdict {
	CG_RP_NEW = 0, /* not seen and inside the window */
	CG_RP_DUP = 1, /* already marked */
	CG_RP_OLD = 2, /* too far behind the window to tell */
};

static inline void cg_replay_reset(struct cg_replay *r)
{
	memset(r, 0, sizeof(*r));
}

static inline enum cg_replay_verdict cg_replay_check(const struct cg_replay *r, uint32_t seq)
{
	int32_t d;

	if (!r->init)
		return CG_RP_NEW;
	d = (int32_t)(seq - r->top);
	if (d > 0)
		return CG_RP_NEW;
	if ((uint32_t)-(int64_t)d >= CG_REPLAY_WINDOW)
		return CG_RP_OLD;
	return (r->ring[(seq >> 6) % CG_REPLAY_BLOCKS] >> (seq & 63)) & 1 ? CG_RP_DUP : CG_RP_NEW;
}

/* Marks seq as seen. Call only for a sequence check() reported as NEW. */
static inline void cg_replay_mark(struct cg_replay *r, uint32_t seq)
{
	int32_t d;

	if (!r->init) {
		cg_replay_reset(r);
		r->init = 1;
		r->top = seq;
	} else {
		d = (int32_t)(seq - r->top);
		if (d > 0) {
			/* Block distance in the 2^26-block sequence space. */
			uint32_t cur = r->top >> 6, blocks = ((seq >> 6) - cur) & ((1u << 26) - 1);

			if (blocks > CG_REPLAY_BLOCKS)
				blocks = CG_REPLAY_BLOCKS;
			for (uint32_t i = 1; i <= blocks; i++)
				r->ring[(cur + i) % CG_REPLAY_BLOCKS] = 0;
			r->top = seq;
		} else if ((uint32_t)-(int64_t)d >= CG_REPLAY_WINDOW) {
			return;
		}
	}
	r->ring[(seq >> 6) % CG_REPLAY_BLOCKS] |= 1ULL << (seq & 63);
}

#endif
