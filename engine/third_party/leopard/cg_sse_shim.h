/* cengarde: the seven SSE2/SSSE3 intrinsics Leopard's 8-bit field uses,
 * for targets that are not x86, instead of sse2neon (story 021):
 *
 *   aarch64 (LEO_USE_SSE2NEON)  NEON, the same work as SSSE3 (16 bytes a
 *                               step, pshufb as tbl); CpuHasSSSE3 is true
 *   others  (LEO_USE_SSE_SCALAR) portable C on 16 bytes; Leopard never takes
 *                               these paths (CpuHasSSSE3 is false: its
 *                               byte-wise reference paths run instead), they
 *                               only have to compile, and are exact anyway
 *
 * Leopard indexes its tables with nibbles only (0..15), so tbl's zero for
 * an index of 16 or more and pshufb's use of the low 4 bits never differ
 * there; the mask below keeps pshufb's meaning for any index all the same.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SSE_SHIM_H
#define CG_SSE_SHIM_H

#include <stdint.h>

#if defined(LEO_USE_SSE2NEON)
#include <arm_neon.h>

typedef uint8x16_t __m128i;

static inline __m128i _mm_loadu_si128(const __m128i *p)
{
	return vld1q_u8((const uint8_t *)p);
}

static inline void _mm_storeu_si128(__m128i *p, __m128i a)
{
	vst1q_u8((uint8_t *)p, a);
}

static inline __m128i _mm_xor_si128(__m128i a, __m128i b)
{
	return veorq_u8(a, b);
}

static inline __m128i _mm_and_si128(__m128i a, __m128i b)
{
	return vandq_u8(a, b);
}

static inline __m128i _mm_set1_epi8(char c)
{
	return vdupq_n_u8((uint8_t)c);
}

/* Leopard shifts by 4 only (the high nibbles) */
#define _mm_srli_epi64(a, n) vreinterpretq_u8_u64(vshrq_n_u64(vreinterpretq_u64_u8(a), (n)))

static inline __m128i _mm_shuffle_epi8(__m128i t, __m128i i)
{
	return vqtbl1q_u8(t, vandq_u8(i, vdupq_n_u8(0x8f)));
}

#else /* LEO_USE_SSE_SCALAR */

typedef struct {
	uint8_t b[16];
} __m128i;

static inline __m128i _mm_loadu_si128(const __m128i *p)
{
	__m128i r;
	const uint8_t *s = (const uint8_t *)p;

	for (int i = 0; i < 16; i++)
		r.b[i] = s[i];
	return r;
}

static inline void _mm_storeu_si128(__m128i *p, __m128i a)
{
	uint8_t *d = (uint8_t *)p;

	for (int i = 0; i < 16; i++)
		d[i] = a.b[i];
}

static inline __m128i _mm_xor_si128(__m128i a, __m128i b)
{
	for (int i = 0; i < 16; i++)
		a.b[i] ^= b.b[i];
	return a;
}

static inline __m128i _mm_and_si128(__m128i a, __m128i b)
{
	for (int i = 0; i < 16; i++)
		a.b[i] &= b.b[i];
	return a;
}

static inline __m128i _mm_set1_epi8(char c)
{
	__m128i r;

	for (int i = 0; i < 16; i++)
		r.b[i] = (uint8_t)c;
	return r;
}

/* Two little-endian 64-bit lanes, as on x86, whatever this target's order. */
static inline __m128i _mm_srli_epi64(__m128i a, int n)
{
	__m128i r;

	for (int h = 0; h < 2; h++) {
		uint64_t v = 0;

		for (int i = 7; i >= 0; i--)
			v = v << 8 | a.b[8 * h + i];
		v = n > 63 ? 0 : v >> n;
		for (int i = 0; i < 8; i++)
			r.b[8 * h + i] = (uint8_t)(v >> (8 * i));
	}
	return r;
}

static inline __m128i _mm_shuffle_epi8(__m128i t, __m128i i)
{
	__m128i r;

	for (int k = 0; k < 16; k++)
		r.b[k] = i.b[k] & 0x80 ? 0 : t.b[i.b[k] & 15];
	return r;
}

#endif

#endif
