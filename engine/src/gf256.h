/* GF(2^8) for the erasure codes (story 021: rs.h, rlc.h, repair.h).
 *
 * The field of the AES/Reed-Solomon tradition, polynomial x^8 + x^4 + x^3
 * + x^2 + 1 (0x11d), generator 2. The caller owns the tables (struct
 * cg_gf, 64 KB of products and 256 inverses), built once with cg_gf_init;
 * a region operation (dst ^= c * src) reads one 256-byte row of products
 * per coefficient, so it stays in L1 on a Pi.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_GF256_H
#define CG_GF256_H

#include <stddef.h>
#include <stdint.h>

/* A coded symbol: a DATA payload (at most CG_SYM_PLEN, engine.h's
 * CG_MAX_PAYLOAD) behind its 16-bit big-endian length, so that a rebuilt
 * one carries its own length; shorter ones are padded with zeros. */
#define CG_SYM_PLEN 2024
#define CG_SYM_MAX (2 + CG_SYM_PLEN)

struct cg_gf {
	uint8_t mul[256][256]; /* mul[a][b] = a * b */
	uint8_t inv[256];      /* inv[a] = 1 / a, inv[0] = 0 */
	uint8_t ready;
};

static inline void cg_gf_init(struct cg_gf *g)
{
	uint8_t exp[512], lg[256];
	unsigned x = 1;

	if (g->ready)
		return;
	for (unsigned i = 0; i < 255; i++) {
		exp[i] = (uint8_t)x;
		lg[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11d;
	}
	for (unsigned i = 255; i < 512; i++)
		exp[i] = exp[i - 255];
	lg[0] = 0;
	for (unsigned a = 0; a < 256; a++)
		for (unsigned b = 0; b < 256; b++)
			g->mul[a][b] = a && b ? exp[lg[a] + lg[b]] : 0;
	g->inv[0] = 0;
	for (unsigned a = 1; a < 256; a++)
		g->inv[a] = exp[255 - lg[a]];
	g->ready = 1;
}

/* dst[i] ^= c * src[i], n bytes. */
static inline void cg_gf_madd(const struct cg_gf *g, uint8_t *dst, const uint8_t *src, uint8_t c, size_t n)
{
	const uint8_t *row = g->mul[c];

	if (!c)
		return;
	if (c == 1) {
		for (size_t i = 0; i < n; i++)
			dst[i] ^= src[i];
		return;
	}
	for (size_t i = 0; i < n; i++)
		dst[i] ^= row[src[i]];
}

/* dst ^= c * (the symbol of a payload of len bytes at p): its length
 * prefix, then the payload; dst holds at least 2 + len bytes. */
static inline void cg_gf_madd_sym(const struct cg_gf *g, uint8_t *dst, const uint8_t *p, uint16_t len, uint8_t c)
{
	uint8_t h[2] = { (uint8_t)(len >> 8), (uint8_t)len };

	cg_gf_madd(g, dst, h, c, 2);
	cg_gf_madd(g, dst + 2, p, c, len);
}

/* dst[i] = c * dst[i], n bytes. */
static inline void cg_gf_scale(const struct cg_gf *g, uint8_t *dst, uint8_t c, size_t n)
{
	const uint8_t *row = g->mul[c];

	if (c == 1)
		return;
	for (size_t i = 0; i < n; i++)
		dst[i] = row[dst[i]];
}

/* Inverts the n x n matrix a (row-major, stride n) into b by Gauss-Jordan;
 * a is destroyed. Returns 0, or -1 when a is singular. n at most 64. */
static inline int cg_gf_invert(const struct cg_gf *g, uint8_t *a, uint8_t *b, unsigned n)
{
	for (unsigned r = 0; r < n; r++)
		for (unsigned c = 0; c < n; c++)
			b[r * n + c] = r == c;
	for (unsigned c = 0; c < n; c++) {
		unsigned p = c;
		uint8_t iv;

		while (p < n && !a[p * n + c])
			p++;
		if (p == n)
			return -1;
		if (p != c)
			for (unsigned k = 0; k < n; k++) {
				uint8_t t = a[p * n + k];

				a[p * n + k] = a[c * n + k];
				a[c * n + k] = t;
				t = b[p * n + k];
				b[p * n + k] = b[c * n + k];
				b[c * n + k] = t;
			}
		iv = g->inv[a[c * n + c]];
		cg_gf_scale(g, a + c * n, iv, n);
		cg_gf_scale(g, b + c * n, iv, n);
		for (unsigned r = 0; r < n; r++) {
			uint8_t f = a[r * n + c];

			if (r == c || !f)
				continue;
			cg_gf_madd(g, a + r * n, a + c * n, f, n);
			cg_gf_madd(g, b + r * n, b + c * n, f, n);
		}
	}
	return 0;
}

#endif
