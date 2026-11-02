/* FEC v3 over the STREAM class (story 031, protocol 17): the protocontroller's
 * messages, which go as STREAM past the reorderer and the rescue (story 030),
 * get their own sliding window of RLC (rlc.h's struct cg_rlcw_tx), in the
 * STREAM sequence, and their repairs say so (repair.h's cls, CG_REP_STREAM).
 *
 * A rebuilt DATA is checked by rebuilt.h (it is an IP packet); a STREAM is a
 * GRIT message, which carries no checksum of its own. So each member's
 * symbol is the message and, behind it, its CRC32C (4 bytes, big-endian),
 * which both ends compute (the sender as it feeds the window, the receiver
 * as it keeps what came) and nothing carries: a rebuilt message passes only
 * when its CRC32C matches. A message too long to take the 4 bytes within a
 * symbol (CG_SYM_PLEN) is kept as an empty symbol at both ends, so the
 * window's run of sequences stays whole and it is never rebuilt.
 *
 * The receiver keeps the STREAMs it got lately in a ring of its own (the
 * same layout as the DATA's, repair.h's struct cg_rep_ring) with its own
 * RLC receiver; about 4.7 MB, allocated when the first STREAM repair comes.
 *
 * CRC32C: the SSE4.2 or ARMv8 CRC instructions when the build has them,
 * else slicing by 8 over tables the caller owns (struct cg_crc32c, 8 KB).
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_STREAMFEC_H
#define CG_STREAMFEC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "repair.h"
#include "rlc.h"

#if defined(__SSE4_2__) && defined(__x86_64__)
#include <nmmintrin.h>
#define CG_CRC_HW 1
#elif defined(__ARM_FEATURE_CRC32) && defined(__aarch64__)
#include <arm_acle.h>
#define CG_CRC_HW 1
#endif

#define CG_SFEC_TAIL 4 /* the CRC32C behind each message */

struct cg_crc32c {
	uint32_t t[8][256];
	uint8_t ready;
};

static inline void cg_crc32c_init(struct cg_crc32c *c)
{
	if (c->ready)
		return;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t x = i;

		for (int k = 0; k < 8; k++)
			x = x & 1 ? x >> 1 ^ 0x82f63b78u : x >> 1;
		c->t[0][i] = x;
	}
	for (unsigned i = 0; i < 256; i++)
		for (int k = 1; k < 8; k++)
			c->t[k][i] = c->t[k - 1][i] >> 8 ^ c->t[0][c->t[k - 1][i] & 0xff];
	c->ready = 1;
}

/* The CRC32C of n bytes at p (c initialised: the tables, when no
 * instruction does it). */
static inline uint32_t cg_crc32c(const struct cg_crc32c *c, const uint8_t *p, size_t n)
{
	uint32_t x = 0xffffffffu;

#ifdef CG_CRC_HW
	(void)c;
	for (; n >= 8; n -= 8, p += 8) {
		uint64_t v;

		memcpy(&v, p, 8);
#ifdef __x86_64__
		x = (uint32_t)_mm_crc32_u64(x, v);
#else
		x = __crc32cd(x, v);
#endif
	}
	for (; n; n--, p++)
#ifdef __x86_64__
		x = _mm_crc32_u8(x, *p);
#else
		x = __crc32cb(x, *p);
#endif
#else
	for (; n >= 8; n -= 8, p += 8) {
		uint32_t lo = x ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);

		x = c->t[7][lo & 0xff] ^ c->t[6][lo >> 8 & 0xff] ^ c->t[5][lo >> 16 & 0xff] ^ c->t[4][lo >> 24] ^
		    c->t[3][p[4]] ^ c->t[2][p[5]] ^ c->t[1][p[6]] ^ c->t[0][p[7]];
	}
	for (; n; n--, p++)
		x = x >> 8 ^ c->t[0][(x ^ *p) & 0xff];
#endif
	return ~x;
}

static inline void cg_sfec_put32(uint8_t *o, uint32_t v)
{
	o[0] = (uint8_t)(v >> 24);
	o[1] = (uint8_t)(v >> 16);
	o[2] = (uint8_t)(v >> 8);
	o[3] = (uint8_t)v;
}

/* The symbol's payload length a message of len bytes takes: with its
 * CRC32C, or 0 (an empty symbol) when that does not fit. */
static inline uint16_t cg_sfec_slen(size_t len)
{
	return len && len + CG_SFEC_TAIL <= CG_SYM_PLEN ? (uint16_t)(len + CG_SFEC_TAIL) : 0;
}

/* The sender: STREAM seq (len bytes at p) into its window at now_us, with
 * its CRC32C behind it. */
static inline void cg_sfec_add(struct cg_rlcw_tx *t, const struct cg_crc32c *c, uint32_t seq, const uint8_t *p,
			       size_t len, uint64_t now_us)
{
	uint16_t sl = cg_sfec_slen(len);
	unsigned i = seq & (CG_RLCW_SPAN - 1);

	cg_rlcw_add(t, seq, p, sl ? (uint16_t)len : 0, now_us);
	if (!sl)
		return;
	cg_sfec_put32(t->p[i] + len, cg_crc32c(c, p, len));
	t->len[i] = sl;
}

/* The receiver: its ring of STREAMs and their RLC receiver. */
struct cg_sfec_rx {
	struct cg_rlc_rx rlc;
	uint64_t last_ms;   /* the last STREAM repair taken */
	uint64_t rebuilt;   /* messages rebuilt that passed their CRC32C */
	uint64_t bad;       /* rebuilt that did not */
	uint64_t dups;      /* rebuilt that had come already */
	uint64_t repairs;   /* STREAM repairs taken */
	uint32_t seq[CG_REP_RING];
	uint16_t len[CG_REP_RING];
	uint8_t have[CG_REP_RING];
	uint8_t mem[(size_t)CG_REP_RING * CG_SYM_PLEN];
};

static inline struct cg_rep_ring cg_sfec_ring(struct cg_sfec_rx *r)
{
	struct cg_rep_ring g = { r->seq, r->len, r->have, r->mem };

	return g;
}

/* A STREAM seq that came (len bytes at p) into the ring, with its CRC32C
 * (an empty symbol when it does not fit, as the sender's). */
static inline void cg_sfec_keep(struct cg_sfec_rx *r, const struct cg_crc32c *c, uint32_t seq, const uint8_t *p,
				size_t len)
{
	uint32_t i = seq & (CG_REP_RING - 1);
	uint16_t sl = cg_sfec_slen(len);
	uint8_t *m = r->mem + (size_t)i * CG_SYM_PLEN;

	r->seq[i] = seq;
	r->len[i] = sl;
	r->have[i] = 1;
	if (!sl)
		return;
	memcpy(m, p, len);
	cg_sfec_put32(m + len, cg_crc32c(c, p, len));
}

/* A rebuilt STREAM seq in the ring: its message's length when its CRC32C
 * matches, else -1, and the ring forgets it (the message itself may still
 * come). The message is at cg_rep_data's pointer. */
static inline int cg_sfec_check(struct cg_sfec_rx *r, const struct cg_crc32c *c, uint32_t seq)
{
	struct cg_rep_ring g = cg_sfec_ring(r);
	uint16_t sl;
	const uint8_t *m = cg_rep_data(&g, seq, &sl);
	uint32_t want;

	if (cg_rep_has(&g, seq) && sl > CG_SFEC_TAIL) {
		want = (uint32_t)m[sl - 4] << 24 | (uint32_t)m[sl - 3] << 16 | (uint32_t)m[sl - 2] << 8 | m[sl - 1];
		if (cg_crc32c(c, m, sl - CG_SFEC_TAIL) == want)
			return sl - CG_SFEC_TAIL;
	}
	r->have[seq & (CG_REP_RING - 1)] = 0;
	return -1;
}

#endif
