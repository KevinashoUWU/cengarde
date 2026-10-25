/* GF(2^8) for the erasure codes (story 021: rs.h, rlc.h, repair.h).
 *
 * The field of the AES/Reed-Solomon tradition, polynomial x^8 + x^4 + x^3
 * + x^2 + 1 (0x11d), generator 2. The caller owns the tables (struct
 * cg_gf, 64 KB of products, 256 inverses and 8 KB of half-byte products),
 * built once with cg_gf_init.
 *
 * A region operation (dst ^= c * src, story 026) goes 16 bytes at a time
 * where the CPU can look up a table of 16 bytes in one instruction: SSSE3's
 * pshufb on x86 (chosen at run time, cg_gf_init asks cpuid: the engine is
 * built for any x86-64), NEON's tbl on aarch64 (always there). c * x is
 * c * (x & 15) xor c * (x >> 4 << 4), two such tables per coefficient
 * (the half-byte products). Elsewhere (armhf, MIPS) a byte at a time from
 * the 256-byte row of products, which stays in L1 on a Pi. The wide
 * windows of FEC v3 (rlc.h) combine tens of packets per repair; the
 * table-per-byte path cost ~0.5 us per 1200-byte packet on this PC
 * (story 021), the 16-byte one several times less (bench/fecbench.c).
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_GF256_H
#define CG_GF256_H

#include <stddef.h>
#include <stdint.h>

#if (defined(__x86_64__) || defined(__i386__)) && !defined(CG_GF_SCALAR)
#define CG_GF_SSSE3 1
#include <cpuid.h>
#include <immintrin.h>
#elif defined(__aarch64__) && !defined(CG_GF_SCALAR)
#define CG_GF_NEON 1
#include <arm_neon.h>
#endif

/* A coded symbol: a DATA payload (at most CG_SYM_PLEN, engine.h's
 * CG_MAX_PAYLOAD) behind its 16-bit big-endian length, so that a rebuilt
 * one carries its own length; shorter ones are padded with zeros. */
#define CG_SYM_PLEN 2024
#define CG_SYM_MAX (2 + CG_SYM_PLEN)

struct cg_gf {
	uint8_t mul[256][256]; /* mul[a][b] = a * b */
	uint8_t inv[256];      /* inv[a] = 1 / a, inv[0] = 0 */
	uint8_t nib[256][32];  /* nib[c]: c * x for x 0..15, then c * (x << 4) */
	uint8_t ready;
	uint8_t simd;          /* the 16-byte path (SSSE3 found, or NEON) */
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
	for (unsigned c = 0; c < 256; c++)
		for (unsigned h = 0; h < 16; h++) {
			g->nib[c][h] = g->mul[c][h];
			g->nib[c][16 + h] = g->mul[c][h << 4];
		}
#if defined(CG_GF_SSSE3)
	{
		unsigned a, b, cx, d;

		g->simd = __get_cpuid(1, &a, &b, &cx, &d) && (cx & bit_SSSE3);
	}
#elif defined(CG_GF_NEON)
	g->simd = 1;
#endif
	g->ready = 1;
}

#if defined(CG_GF_SSSE3)
/* dst[i] ^= c * src[i] (mul: 0) or dst[i] = c * dst[i] (mul: 1, src
 * unused), the whole 16-byte blocks of n; returns how many bytes it did. */
__attribute__((target("ssse3"))) static inline size_t cg_gf_simd(const uint8_t *nib, uint8_t *dst, const uint8_t *src,
								 size_t n, int scale)
{
	const __m128i lo = _mm_loadu_si128((const __m128i *)nib), hi = _mm_loadu_si128((const __m128i *)(nib + 16)),
		      m = _mm_set1_epi8(0x0f);
	size_t i = 0;

	for (; i + 16 <= n; i += 16) {
		__m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
		__m128i x = scale ? d : _mm_loadu_si128((const __m128i *)(src + i));
		__m128i p = _mm_xor_si128(_mm_shuffle_epi8(lo, _mm_and_si128(x, m)),
					  _mm_shuffle_epi8(hi, _mm_and_si128(_mm_srli_epi64(x, 4), m)));

		_mm_storeu_si128((__m128i *)(dst + i), scale ? p : _mm_xor_si128(d, p));
	}
	return i;
}
#elif defined(CG_GF_NEON)
static inline size_t cg_gf_simd(const uint8_t *nib, uint8_t *dst, const uint8_t *src, size_t n, int scale)
{
	const uint8x16_t lo = vld1q_u8(nib), hi = vld1q_u8(nib + 16), m = vdupq_n_u8(0x0f);
	size_t i = 0;

	for (; i + 16 <= n; i += 16) {
		uint8x16_t d = vld1q_u8(dst + i);
		uint8x16_t x = scale ? d : vld1q_u8(src + i);
		uint8x16_t p = veorq_u8(vqtbl1q_u8(lo, vandq_u8(x, m)), vqtbl1q_u8(hi, vshrq_n_u8(x, 4)));

		vst1q_u8(dst + i, scale ? p : veorq_u8(d, p));
	}
	return i;
}
#endif

/* dst[i] ^= c * src[i], n bytes. */
static inline void cg_gf_madd(const struct cg_gf *g, uint8_t *dst, const uint8_t *src, uint8_t c, size_t n)
{
	const uint8_t *row = g->mul[c];
	size_t i = 0;

	if (!c)
		return;
	if (c == 1) {
		for (; i < n; i++)
			dst[i] ^= src[i];
		return;
	}
#if defined(CG_GF_SSSE3) || defined(CG_GF_NEON)
	if (g->simd && n >= 16)
		i = cg_gf_simd(g->nib[c], dst, src, n, 0);
#endif
	for (; i < n; i++)
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
	size_t i = 0;

	if (c == 1)
		return;
#if defined(CG_GF_SSSE3) || defined(CG_GF_NEON)
	if (g->simd && n >= 16)
		i = cg_gf_simd(g->nib[c], dst, dst, n, 1);
#endif
	for (; i < n; i++)
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
