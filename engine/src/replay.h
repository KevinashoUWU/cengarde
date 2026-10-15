/* Duplicate suppression and anti-replay window over authenticated sequence
 * numbers (same layout as WireGuard's filter: a ring of 64-bit blocks).
 *
 * The receiver calls cg_replay_check() before verifying the MAC and
 * cg_replay_mark() only after the MAC verified, so a forged packet can never
 * mark a sequence number and shadow the genuine copy. A sequence already
 * marked is a duplicate and can be dropped without paying for the MAC.
 *
 * lost counts the sequences that left the window without ever being marked:
 * for the DATA window, packets the tunnel lost on every link (story 012,
 * the field recorder). It is a measurement that no verdict reads. It is
 * counted when the top moves into a new block and clears the block the
 * ring held there, so it is exact for every sequence the window held and
 * lags the top by the ring (8129 to 8255 sequences). It counts:
 *   - from the first sequence the window saw: what came before the start or
 *     a reset was never ours to lose;
 *   - nothing cg_replay_init_marked() marked (all of it is);
 *   - nothing a jump that clears the whole ring skips: it starts again at
 *     the new top, as the first sequence does. A server that took the
 *     session up again goes on 2^20 ahead (proto.h) and never sent those;
 *     an outage on every link longer than the window is counted short by
 *     what the window could not hold.
 * The control window counts too, but nobody reads it: a probe goes on one
 * link only, so its loss is no loss on every link. reset() zeroes it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_REPLAY_H
#define CG_REPLAY_H

#include <stdint.h>
#include <string.h>

/* 32768 sequences: a packet 300 ms late at 27 000 a second is still in it
 * (it was not at 8192), and the reorderer's window fits under it. */
#define CG_REPLAY_BITS 32768
#define CG_REPLAY_BLOCKS (CG_REPLAY_BITS / 64)
#define CG_REPLAY_WINDOW (CG_REPLAY_BITS - 64) /* sequences accepted behind the top */

struct cg_replay {
	uint32_t top; /* highest sequence marked */
	uint8_t init;
	uint8_t floor_on; /* lost skips what is before floor: its block is not cleared yet */
	uint32_t floor;   /* where lost starts: the first sequence, or the top after a jump */
	uint64_t lost;    /* sequences that left the window unmarked (above) */
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

/* A window that starts at top with every sequence up to it seen: what a
 * server that lost its session takes from the client's probe (proto.h), so
 * that nothing sent before can be replayed into the new session. */
static inline void cg_replay_init_marked(struct cg_replay *r, uint32_t top)
{
	unsigned bit = top & 63;

	r->init = 1;
	r->top = top;
	r->floor_on = 0; /* everything behind top is marked: nothing to skip */
	memset(r->ring, 0xff, sizeof(r->ring));
	/* Not what comes after top in its block: mark() clears only the blocks
	 * it moves into, so these would read as DUP out of order. */
	r->ring[(top >> 6) % CG_REPLAY_BLOCKS] = bit == 63 ? ~0ULL : (1ULL << (bit + 1)) - 1;
}

/* Ring block k, about to be cleared, holds block number blk: adds to lost
 * its sequences never marked, up to the top (not moved yet) and, while the
 * floor is on, from the floor. Blocks are cleared in order, the floor's
 * among them, so once it is cleared every later one lies after the floor.
 * It reads the ring and writes only lost and floor_on. */
static inline void cg_replay_count_lost(struct cg_replay *r, uint32_t k, uint32_t blk)
{
	uint32_t base = blk << 6;
	int32_t hi = (int32_t)(r->top - base), lo;
	uint64_t valid = ~0ULL;

	if (hi < 0)
		return; /* not reached by the top: cannot happen, the ring holds blocks behind it */
	if (hi < 63)
		valid = (2ULL << hi) - 1; /* the top's own block: only a jump clears it */
	if (r->floor_on) {
		lo = (int32_t)(r->floor - base);
		if (lo > 63)
			return; /* before the floor */
		if (lo > 0)
			valid &= ~0ULL << lo;
		r->floor_on = 0;
	}
	r->lost += (uint64_t)__builtin_popcountll(~r->ring[k] & valid);
}

/* Marks seq as seen. Call only for a sequence check() reported as NEW. */
static inline void cg_replay_mark(struct cg_replay *r, uint32_t seq)
{
	int32_t d;

	if (!r->init) {
		cg_replay_reset(r);
		r->init = 1;
		r->top = seq;
		r->floor = seq;
		r->floor_on = 1;
	} else {
		d = (int32_t)(seq - r->top);
		if (d > 0) {
			/* Block distance in the 2^26-block sequence space. */
			uint32_t cur = r->top >> 6, blocks = ((seq >> 6) - cur) & ((1u << 26) - 1);

			if (blocks > CG_REPLAY_BLOCKS)
				blocks = CG_REPLAY_BLOCKS;
			for (uint32_t i = 1; i <= blocks; i++) {
				/* Ring block cur + i held block number cur + i - BLOCKS. */
				cg_replay_count_lost(r, (cur + i) % CG_REPLAY_BLOCKS, cur + i - CG_REPLAY_BLOCKS);
				r->ring[(cur + i) % CG_REPLAY_BLOCKS] = 0;
			}
			r->top = seq;
			if (blocks == CG_REPLAY_BLOCKS) {
				/* The whole ring cleared: lost starts again here. */
				r->floor = seq;
				r->floor_on = 1;
			}
		} else if ((uint32_t)-(int64_t)d >= CG_REPLAY_WINDOW) {
			return;
		}
	}
	r->ring[(seq >> 6) % CG_REPLAY_BLOCKS] |= 1ULL << (seq & 63);
}

#endif
