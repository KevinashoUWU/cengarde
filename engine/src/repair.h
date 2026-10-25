/* Repair packets (CG_T_REPAIR, protocol version 8, story 021): the
 * erasure codes of redund_spare = rs | rlc, over DATA that went on the
 * first link alone, carried in the other links' spare (spare.h); since
 * protocol 11 (story 026, FEC v3: fec_code = rlc) also the wide windows
 * over every DATA of a split or of a lone link, either way.
 *
 * Payload: a 16-byte header, then the coded symbol.
 *
 *   0..3    base: the first member's DATA sequence (big-endian)
 *   4..11   members: a 64-bit big-endian map, bit i for base + i (bit 0
 *           set): at most 64 sequences apart, in order
 *   12      kind: CG_REP_RS (Leopard's Reed-Solomon, rs.h) or CG_REP_RLC
 *           (the random linear code over a sliding window, rlc.h)
 *   13      RS: m, the block's repairs (1 to CG_RS_MMAX, at most its k
 *           members); RLC: 0
 *   14..15  RS: this repair's index (0 to m - 1); RLC: the coefficients'
 *           seed (big-endian)
 *   16..    RS: Leopard's recovery symbol, a multiple of 64 bytes (each
 *           member's symbol padded to the block's longest); RLC: the
 *           combination, as long as its longest member's symbol
 *
 * A wide window's repair (kind CG_REP_RLCW, protocol 11) names its members
 * by a range and a seed instead of a map, since its window spans up to
 * CG_RLCW_SPAN sequences, every DATA in it a candidate:
 *
 *   0..3    base: the window's first DATA sequence (big-endian)
 *   4..5    span: the window's sequences, base to base + span - 1 (1 to
 *           CG_RLCW_SPAN, big-endian)
 *   6       density dn: the member at distance d is in the repair when
 *           the low byte of its hash (rlc.h's cg_rlcw_member) is at most
 *           dn (255: every one)
 *   7       0
 *   8..11   seed: the hash's, which also gives each member's coefficient
 *           (big-endian)
 *   12      kind: CG_REP_RLCW
 *   13..15  0
 *   16..    the combination, as long as its longest member's symbol
 *
 * A member's symbol is its DATA payload behind its 16-bit big-endian length
 * (gf256.h's CG_SYM_*), zero-padded: a rebuilt one carries its length.
 *
 * Both receivers keep the DATA they got lately in the same ring as the XOR
 * parity's (fec.h's, CG_FEC_RING entries of CG_FEC_PLEN_MAX bytes, the
 * server's), seen here as struct cg_rep_ring, and write what they rebuild
 * into it, so that a packet rebuilt counts as received for the rest.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_REPAIR_H
#define CG_REPAIR_H

#include <stdint.h>
#include <string.h>

#include "gf256.h"

#define CG_REP_HDR 16
#define CG_REP_SPAN 64   /* a repair's members: within this many sequences */
#define CG_REP_RING 2048 /* the ring of DATA (fec.h's CG_FEC_RING) */
#define CG_REP_AGE 384   /* a pending RS block older than this (sequences behind the newest) is let go */
#define CG_RS_MMAX 32    /* an RS block's repairs, at most */
#define CG_RS_BYTES_MAX 2048 /* an RS symbol: CG_SYM_MAX rounded up to 64 */
#define CG_RLCW_SPAN 1024 /* a wide window's sequences, at most (story 026) */
#define CG_RLC_KEEP 1024  /* an unknown this far behind the newest repair is given up (rlc.h) */

_Static_assert(CG_RS_BYTES_MAX >= CG_SYM_MAX && CG_RS_BYTES_MAX % 64 == 0, "an RS symbol holds any DATA");
_Static_assert(!(CG_REP_RING & (CG_REP_RING - 1)) && CG_REP_AGE + CG_REP_SPAN < CG_REP_RING / 2,
	       "a pending repair's members stay in the ring");
_Static_assert(!(CG_RLCW_SPAN & (CG_RLCW_SPAN - 1)) && CG_RLCW_SPAN <= 65535 && CG_RLC_KEEP < CG_REP_RING,
	       "a wide window fits its 16-bit span, and an unknown its ring slot");

enum { CG_REP_RS = 1, CG_REP_RLC = 2, CG_REP_RLCW = 3 };

struct cg_rep_hdr {
	uint32_t base;
	uint64_t map;  /* RS and RLC: the members */
	uint8_t kind;
	uint8_t m;
	uint16_t idx;  /* RS: the repair's index; RLC: the seed */
	uint16_t span; /* RLCW: the window's sequences */
	uint8_t dn;    /* RLCW: its density */
	uint32_t seed; /* RLCW: the members' and coefficients' seed */
};

static inline void cg_rep_put(uint8_t *o, const struct cg_rep_hdr *h)
{
	for (int i = 0; i < 4; i++)
		o[i] = (uint8_t)(h->base >> (24 - 8 * i));
	if (h->kind == CG_REP_RLCW) {
		o[4] = (uint8_t)(h->span >> 8);
		o[5] = (uint8_t)h->span;
		o[6] = h->dn;
		o[7] = 0;
		for (int i = 0; i < 4; i++)
			o[8 + i] = (uint8_t)(h->seed >> (24 - 8 * i));
		o[12] = h->kind;
		o[13] = o[14] = o[15] = 0;
		return;
	}
	for (int i = 0; i < 8; i++)
		o[4 + i] = (uint8_t)(h->map >> (56 - 8 * i));
	o[12] = h->kind;
	o[13] = h->m;
	o[14] = (uint8_t)(h->idx >> 8);
	o[15] = (uint8_t)h->idx;
}

/* Parses a repair payload (plen bytes at p): its header in *h and its
 * symbol's length in *slen; 0, or -1 when malformed. */
static inline int cg_rep_get(const uint8_t *p, unsigned plen, struct cg_rep_hdr *h, unsigned *slen)
{
	unsigned k;

	if (plen <= CG_REP_HDR)
		return -1;
	h->base = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	h->map = 0;
	h->kind = p[12];
	h->span = 0;
	h->dn = 0;
	h->seed = 0;
	*slen = plen - CG_REP_HDR;
	if (h->kind == CG_REP_RLCW) {
		h->span = (uint16_t)(p[4] << 8 | p[5]);
		h->dn = p[6];
		h->seed = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
		h->m = 0;
		h->idx = 0;
		return h->span && h->span <= CG_RLCW_SPAN && !p[7] && !p[13] && !p[14] && !p[15] && *slen > 2 &&
				       *slen <= CG_SYM_MAX
			       ? 0
			       : -1;
	}
	for (int i = 0; i < 8; i++)
		h->map = h->map << 8 | p[4 + i];
	h->m = p[13];
	h->idx = (uint16_t)(p[14] << 8 | p[15]);
	if (!(h->map & 1))
		return -1;
	k = (unsigned)__builtin_popcountll(h->map);
	if (h->kind == CG_REP_RS)
		return h->m && h->m <= CG_RS_MMAX && h->m <= k && h->idx < h->m && *slen % 64 == 0 &&
				       *slen <= CG_RS_BYTES_MAX
			       ? 0
			       : -1;
	if (h->kind == CG_REP_RLC)
		return !h->m && *slen > 2 && *slen <= CG_SYM_MAX ? 0 : -1;
	return -1;
}

/* The ring of DATA a receiver keeps (fec.h's arrays). */
struct cg_rep_ring {
	uint32_t *seq;
	uint16_t *len;
	uint8_t *have;
	uint8_t *mem; /* CG_REP_RING x CG_SYM_PLEN */
};

static inline int cg_rep_has(const struct cg_rep_ring *r, uint32_t seq)
{
	uint32_t i = seq & (CG_REP_RING - 1);

	return r->have[i] && r->seq[i] == seq;
}

static inline const uint8_t *cg_rep_data(const struct cg_rep_ring *r, uint32_t seq, uint16_t *len)
{
	uint32_t i = seq & (CG_REP_RING - 1);

	*len = r->len[i];
	return r->mem + (size_t)i * CG_SYM_PLEN;
}

/* A rebuilt symbol (slen bytes at s) into the ring as DATA seq: 0; -1
 * when its length prefix does not fit it (counted bad by the caller); -2
 * when the slot holds a newer DATA (an old equation settled late: the
 * ring's newer entry stays, the rebuilt one is let go). */
static inline int cg_rep_put_sym(struct cg_rep_ring *r, uint32_t seq, const uint8_t *s, unsigned slen)
{
	uint32_t i = seq & (CG_REP_RING - 1);
	unsigned len = (unsigned)s[0] << 8 | s[1];

	if (!len || len + 2 > slen || len > CG_SYM_PLEN)
		return -1;
	if (r->have[i] && (int32_t)(r->seq[i] - seq) > 0)
		return -2;
	r->seq[i] = seq;
	r->len[i] = (uint16_t)len;
	r->have[i] = 1;
	memcpy(r->mem + (size_t)i * CG_SYM_PLEN, s + 2, len);
	return 0;
}

/* The members of map from base still missing from the ring (bit i: base + i). */
static inline uint64_t cg_rep_missing(const struct cg_rep_ring *r, uint32_t base, uint64_t map)
{
	uint64_t miss = 0;

	for (uint64_t m = map; m; m &= m - 1) {
		unsigned d = (unsigned)__builtin_ctzll(m);

		if (!cg_rep_has(r, base + d))
			miss |= 1ull << d;
	}
	return miss;
}

/* Whether a sequence is older than another by more than CG_REP_AGE. */
static inline int cg_rep_stale(uint32_t base, uint32_t newest)
{
	return (int32_t)(newest - base) > CG_REP_AGE;
}

#endif
