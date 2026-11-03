/* ChaCha20-Poly1305 (RFC 8439): see aead.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "aead.h"

#include <string.h>

static inline uint32_t ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void st32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

#define ROTL(v, n) ((v) << (n) | (v) >> (32 - (n)))
#define QR(a, b, c, d)                     \
	do {                               \
		a += b, d ^= a, d = ROTL(d, 16); \
		c += d, b ^= c, b = ROTL(b, 12); \
		a += b, d ^= a, d = ROTL(d, 8);  \
		c += d, b ^= c, b = ROTL(b, 7);  \
	} while (0)

static void chacha_core(uint32_t out[16], const uint32_t in[16])
{
	uint32_t x[16];

	memcpy(x, in, sizeof(x));
	for (int i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]);
		QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]);
		QR(x[3], x[4], x[9], x[14]);
	}
	for (int i = 0; i < 16; i++)
		out[i] = x[i] + in[i];
}

static void chacha_setup(uint32_t s[16], const uint8_t key[32], const uint8_t nonce[12], uint32_t counter)
{
	s[0] = 0x61707865;
	s[1] = 0x3320646e;
	s[2] = 0x79622d32;
	s[3] = 0x6b206574;
	for (int i = 0; i < 8; i++)
		s[4 + i] = ld32(key + 4 * i);
	s[12] = counter;
	for (int i = 0; i < 3; i++)
		s[13 + i] = ld32(nonce + 4 * i);
}

void cg_chacha20_block(uint8_t out[64], const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN],
		       uint32_t counter)
{
	uint32_t s[16], o[16];

	chacha_setup(s, key, nonce, counter);
	chacha_core(o, s);
	for (int i = 0; i < 16; i++)
		st32(out + 4 * i, o[i]);
}

#if defined(__ARM_NEON) || defined(__SSE2__)
/* Four blocks at once, word i of each in one vector (GCC's vector types:
 * NEON on ARM, SSE2 on x86): 3-4 times the scalar core on the A53 and the
 * PC (story 032); MIPS and the rest keep the scalar one. */
typedef uint32_t cg_v4 __attribute__((vector_size(16)));
#ifdef __clang__
#define CG_SHUF(a, b, i, j, k, l) __builtin_shufflevector(a, b, i, j, k, l)
#else
#define CG_SHUF(a, b, i, j, k, l) __builtin_shuffle(a, b, (cg_v4){ i, j, k, l })
#endif
#ifdef __ARM_NEON
#include <arm_neon.h>
/* NEON's own rotations: a halfword swap for 16, shift-and-insert for the
 * rest (two instructions where the generic form takes three) */
#define VROT16(v) ((cg_v4)vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32((uint32x4_t)(v)))))
#define VROTN(v, n) ((cg_v4)vsriq_n_u32(vshlq_n_u32((uint32x4_t)(v), n), (uint32x4_t)(v), 32 - (n)))
#else
#define VROT16(v) ((v) << 16 | (v) >> 16)
#define VROTN(v, n) ((v) << (n) | (v) >> (32 - (n)))
#endif
#define VQR(a, b, c, d)                     \
	do {                                \
		a += b, d ^= a, d = VROT16(d);    \
		c += d, b ^= c, b = VROTN(b, 12); \
		a += b, d ^= a, d = VROTN(d, 8);  \
		c += d, b ^= c, b = VROTN(b, 7);  \
	} while (0)

static void chacha_core4(uint8_t *out, const uint8_t *in, const uint32_t s[16])
{
	cg_v4 x[16], o[16];

	for (int i = 0; i < 16; i++)
		o[i] = (cg_v4){ s[i], s[i], s[i], s[i] };
	o[12] += (cg_v4){ 0, 1, 2, 3 };
	memcpy(x, o, sizeof(x));
	for (int i = 0; i < 10; i++) {
		VQR(x[0], x[4], x[8], x[12]);
		VQR(x[1], x[5], x[9], x[13]);
		VQR(x[2], x[6], x[10], x[14]);
		VQR(x[3], x[7], x[11], x[15]);
		VQR(x[0], x[5], x[10], x[15]);
		VQR(x[1], x[6], x[11], x[12]);
		VQR(x[2], x[7], x[8], x[13]);
		VQR(x[3], x[4], x[9], x[14]);
	}
	for (int i = 0; i < 16; i++)
		x[i] += o[i];
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__))
	/* words 4k..4k+3 of the four blocks, transposed in registers into each
	 * block's 16 bytes, and XORed 16 bytes at a time (extracting word by
	 * word cost the A53 most of the gain) */
	for (int k = 0; k < 4; k++) {
		cg_v4 t0 = CG_SHUF(x[4 * k], x[4 * k + 1], 0, 4, 1, 5), t1 = CG_SHUF(x[4 * k], x[4 * k + 1], 2, 6, 3, 7);
		cg_v4 t2 = CG_SHUF(x[4 * k + 2], x[4 * k + 3], 0, 4, 1, 5), t3 = CG_SHUF(x[4 * k + 2], x[4 * k + 3], 2, 6, 3, 7);
		cg_v4 r[4] = { CG_SHUF(t0, t2, 0, 1, 4, 5), CG_SHUF(t0, t2, 2, 3, 6, 7), CG_SHUF(t1, t3, 0, 1, 4, 5),
			       CG_SHUF(t1, t3, 2, 3, 6, 7) };

		for (int b = 0; b < 4; b++) {
			cg_v4 v;

			memcpy(&v, in + 64 * b + 16 * k, 16);
			v ^= r[b];
			memcpy(out + 64 * b + 16 * k, &v, 16);
		}
	}
#else
	for (int b = 0; b < 4; b++)
		for (int i = 0; i < 16; i++)
			st32(out + 64 * b + 4 * i, ld32(in + 64 * b + 4 * i) ^ x[i][b]);
#endif
}
#endif

void cg_chacha20(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[CG_AEAD_KEY_LEN],
		 const uint8_t nonce[CG_AEAD_NONCE_LEN], uint32_t counter)
{
	uint32_t s[16], o[16];
	uint8_t ks[64];

	chacha_setup(s, key, nonce, counter);
#if defined(__ARM_NEON) || defined(__SSE2__)
	while (len >= 256) {
		chacha_core4(out, in, s);
		s[12] += 4;
		in += 256;
		out += 256;
		len -= 256;
	}
#endif
	while (len >= 64) {
		chacha_core(o, s);
		for (int i = 0; i < 16; i++)
			st32(out + 4 * i, ld32(in + 4 * i) ^ o[i]);
		s[12]++;
		in += 64;
		out += 64;
		len -= 64;
	}
	if (len) {
		chacha_core(o, s);
		for (int i = 0; i < 16; i++)
			st32(ks + 4 * i, o[i]);
		for (size_t i = 0; i < len; i++)
			out[i] = in[i] ^ ks[i];
	}
}

#if defined(__SIZEOF_INT128__) && defined(__x86_64__)
/* Poly1305 with three 44-bit limbs (poly1305-donna's 64-bit form): half the
 * multiplications of the 26-bit one where 64x64 -> 128 is fast (x86-64: 806
 * -> 623 ns for 1424 bytes on the PC). Not on aarch64: on the A53 (CPE1) the
 * high half of the product is slow, and it took 3.5 -> 6.0 us. */
typedef unsigned __int128 cg_u128;

static inline uint64_t ld64(const uint8_t *p)
{
	return (uint64_t)ld32(p) | (uint64_t)ld32(p + 4) << 32;
}

static inline void st64(uint8_t *p, uint64_t v)
{
	st32(p, (uint32_t)v);
	st32(p + 4, (uint32_t)(v >> 32));
}

void cg_poly1305_init(struct cg_poly1305 *st, const uint8_t key[32])
{
	uint64_t t0 = ld64(key), t1 = ld64(key + 8);

	/* r clamped (RFC 8439 2.5.1) */
	st->r[0] = t0 & 0xffc0fffffff;
	st->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffff;
	st->r[2] = (t1 >> 24) & 0x00ffffffc0f;
	st->h[0] = st->h[1] = st->h[2] = 0;
	st->pad[0] = ld64(key + 16);
	st->pad[1] = ld64(key + 24);
	st->left = 0;
}

static void poly_blocks(struct cg_poly1305 *st, const uint8_t *m, size_t len, int last)
{
	const uint64_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
	const uint64_t hibit = last ? 0 : (uint64_t)1 << 40;
	uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];

	while (len >= 16) {
		uint64_t t0 = ld64(m), t1 = ld64(m + 8), c;
		cg_u128 d0, d1, d2;

		h0 += t0 & 0xfffffffffff;
		h1 += ((t0 >> 44) | (t1 << 20)) & 0xfffffffffff;
		h2 += ((t1 >> 24) & 0x3ffffffffff) | hibit;
		d0 = (cg_u128)h0 * r0 + (cg_u128)h1 * s2 + (cg_u128)h2 * s1;
		d1 = (cg_u128)h0 * r1 + (cg_u128)h1 * r0 + (cg_u128)h2 * s2;
		d2 = (cg_u128)h0 * r2 + (cg_u128)h1 * r1 + (cg_u128)h2 * r0;
		c = (uint64_t)(d0 >> 44);
		h0 = (uint64_t)d0 & 0xfffffffffff;
		d1 += c;
		c = (uint64_t)(d1 >> 44);
		h1 = (uint64_t)d1 & 0xfffffffffff;
		d2 += c;
		c = (uint64_t)(d2 >> 42);
		h2 = (uint64_t)d2 & 0x3ffffffffff;
		h0 += c * 5;
		c = h0 >> 44;
		h0 &= 0xfffffffffff;
		h1 += c;
		m += 16;
		len -= 16;
	}
	st->h[0] = h0, st->h[1] = h1, st->h[2] = h2;
}

static void poly_finish(struct cg_poly1305 *st, uint8_t tag[CG_AEAD_TAG_LEN])
{
	const uint64_t m44 = 0xfffffffffff, m42 = 0x3ffffffffff;
	uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], c, g0, g1, g2, mask, t0, t1;

	/* carry fully */
	c = h1 >> 44, h1 &= m44, h2 += c;
	c = h2 >> 42, h2 &= m42, h0 += c * 5;
	c = h0 >> 44, h0 &= m44, h1 += c;
	c = h1 >> 44, h1 &= m44, h2 += c;
	c = h2 >> 42, h2 &= m42, h0 += c * 5;
	c = h0 >> 44, h0 &= m44, h1 += c;
	/* h + -p, kept if h >= p (constant time) */
	g0 = h0 + 5, c = g0 >> 44, g0 &= m44;
	g1 = h1 + c, c = g1 >> 44, g1 &= m44;
	g2 = h2 + c - ((uint64_t)1 << 42);
	mask = (g2 >> 63) - 1;
	g0 &= mask, g1 &= mask, g2 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0, h1 = (h1 & mask) | g1, h2 = (h2 & mask) | g2;
	/* + s */
	t0 = st->pad[0], t1 = st->pad[1];
	h0 += t0 & m44, c = h0 >> 44, h0 &= m44;
	h1 += (((t0 >> 44) | (t1 << 20)) & m44) + c, c = h1 >> 44, h1 &= m44;
	h2 += ((t1 >> 24) & m42) + c, h2 &= m42;
	st64(tag, h0 | h1 << 44);
	st64(tag + 8, h1 >> 20 | h2 << 24);
	memset(st, 0, sizeof(*st));
}
#else
/* Poly1305 with five 26-bit limbs (poly1305-donna's 32-bit form): products
 * fit in 64 bits, so 32-bit CPUs need no 128-bit arithmetic. */
void cg_poly1305_init(struct cg_poly1305 *st, const uint8_t key[32])
{
	/* r clamped (RFC 8439 2.5.1) */
	st->r[0] = ld32(key + 0) & 0x3ffffff;
	st->r[1] = (ld32(key + 3) >> 2) & 0x3ffff03;
	st->r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ff;
	st->r[3] = (ld32(key + 9) >> 6) & 0x3f03fff;
	st->r[4] = (ld32(key + 12) >> 8) & 0x00fffff;
	memset(st->h, 0, sizeof(st->h));
	for (int i = 0; i < 4; i++)
		st->pad[i] = ld32(key + 16 + 4 * i);
	st->left = 0;
}

static void poly_blocks(struct cg_poly1305 *st, const uint8_t *m, size_t len, int last)
{
	const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
	const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5, hibit = last ? 0 : 1u << 24;
	uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];

	while (len >= 16) {
		uint64_t d0, d1, d2, d3, d4;
		uint32_t c;

		h0 += ld32(m + 0) & 0x3ffffff;
		h1 += (ld32(m + 3) >> 2) & 0x3ffffff;
		h2 += (ld32(m + 6) >> 4) & 0x3ffffff;
		h3 += (ld32(m + 9) >> 6) & 0x3ffffff;
		h4 += (ld32(m + 12) >> 8) | hibit;
		d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
		d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
		d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
		d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
		d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
		c = (uint32_t)(d0 >> 26);
		h0 = (uint32_t)d0 & 0x3ffffff;
		d1 += c;
		c = (uint32_t)(d1 >> 26);
		h1 = (uint32_t)d1 & 0x3ffffff;
		d2 += c;
		c = (uint32_t)(d2 >> 26);
		h2 = (uint32_t)d2 & 0x3ffffff;
		d3 += c;
		c = (uint32_t)(d3 >> 26);
		h3 = (uint32_t)d3 & 0x3ffffff;
		d4 += c;
		c = (uint32_t)(d4 >> 26);
		h4 = (uint32_t)d4 & 0x3ffffff;
		h0 += c * 5;
		c = h0 >> 26;
		h0 &= 0x3ffffff;
		h1 += c;
		m += 16;
		len -= 16;
	}
	st->h[0] = h0, st->h[1] = h1, st->h[2] = h2, st->h[3] = h3, st->h[4] = h4;
}

static void poly_finish(struct cg_poly1305 *st, uint8_t tag[CG_AEAD_TAG_LEN])
{
	uint32_t h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4, mask;
	uint64_t f;

	h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
	/* carry fully */
	c = h1 >> 26, h1 &= 0x3ffffff, h2 += c;
	c = h2 >> 26, h2 &= 0x3ffffff, h3 += c;
	c = h3 >> 26, h3 &= 0x3ffffff, h4 += c;
	c = h4 >> 26, h4 &= 0x3ffffff, h0 += c * 5;
	c = h0 >> 26, h0 &= 0x3ffffff, h1 += c;
	/* h + -p, kept if h >= p (constant time) */
	g0 = h0 + 5, c = g0 >> 26, g0 &= 0x3ffffff;
	g1 = h1 + c, c = g1 >> 26, g1 &= 0x3ffffff;
	g2 = h2 + c, c = g2 >> 26, g2 &= 0x3ffffff;
	g3 = h3 + c, c = g3 >> 26, g3 &= 0x3ffffff;
	g4 = h4 + c - (1u << 26);
	mask = (g4 >> 31) - 1; /* all ones when g4 did not go negative: h >= p */
	g0 &= mask, g1 &= mask, g2 &= mask, g3 &= mask, g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0, h1 = (h1 & mask) | g1, h2 = (h2 & mask) | g2, h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;
	/* h mod 2^128, as four 32-bit words */
	h0 = (h0 | h1 << 26) & 0xffffffff;
	h1 = (h1 >> 6 | h2 << 20) & 0xffffffff;
	h2 = (h2 >> 12 | h3 << 14) & 0xffffffff;
	h3 = (h3 >> 18 | h4 << 8) & 0xffffffff;
	/* + s */
	f = (uint64_t)h0 + st->pad[0], h0 = (uint32_t)f;
	f = (uint64_t)h1 + st->pad[1] + (f >> 32), h1 = (uint32_t)f;
	f = (uint64_t)h2 + st->pad[2] + (f >> 32), h2 = (uint32_t)f;
	f = (uint64_t)h3 + st->pad[3] + (f >> 32), h3 = (uint32_t)f;
	st32(tag + 0, h0);
	st32(tag + 4, h1);
	st32(tag + 8, h2);
	st32(tag + 12, h3);
	memset(st, 0, sizeof(*st));
}

#endif

void cg_poly1305_update(struct cg_poly1305 *st, const uint8_t *m, size_t len)
{
	if (st->left) {
		size_t want = 16 - st->left;

		if (want > len)
			want = len;
		memcpy(st->buf + st->left, m, want);
		st->left += want;
		m += want;
		len -= want;
		if (st->left < 16)
			return;
		poly_blocks(st, st->buf, 16, 0);
		st->left = 0;
	}
	if (len >= 16) {
		size_t full = len & ~(size_t)15;

		poly_blocks(st, m, full, 0);
		m += full;
		len -= full;
	}
	if (len) {
		memcpy(st->buf, m, len);
		st->left = len;
	}
}

void cg_poly1305_final(struct cg_poly1305 *st, uint8_t tag[CG_AEAD_TAG_LEN])
{
	if (st->left) { /* the last partial block: a 1 after it, no 2^128 bit */
		st->buf[st->left] = 1;
		memset(st->buf + st->left + 1, 0, 16 - st->left - 1);
		poly_blocks(st, st->buf, 16, 1);
	}
	poly_finish(st, tag);
}

void cg_poly1305(uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *m, size_t len, const uint8_t key[32])
{
	struct cg_poly1305 st;

	cg_poly1305_init(&st, key);
	cg_poly1305_update(&st, m, len);
	cg_poly1305_final(&st, tag);
}

/* Poly1305 over aad, ct and their lengths, as RFC 8439 2.8 pads them. */
static void aead_tag(uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *ct, size_t len, const uint8_t *aad, size_t aadlen,
		     const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN])
{
	static const uint8_t zero[16];
	uint8_t otk[64], lens[16];
	struct cg_poly1305 st;

	cg_chacha20_block(otk, key, nonce, 0);
	cg_poly1305_init(&st, otk);
	cg_poly1305_update(&st, aad, aadlen);
	cg_poly1305_update(&st, zero, (16 - aadlen % 16) % 16);
	cg_poly1305_update(&st, ct, len);
	cg_poly1305_update(&st, zero, (16 - len % 16) % 16);
	st32(lens, (uint32_t)aadlen);
	st32(lens + 4, (uint32_t)((uint64_t)aadlen >> 32));
	st32(lens + 8, (uint32_t)len);
	st32(lens + 12, (uint32_t)((uint64_t)len >> 32));
	cg_poly1305_update(&st, lens, 16);
	cg_poly1305_final(&st, tag);
	memset(otk, 0, sizeof(otk));
}

void cg_aead_seal(uint8_t *ct, uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *pt, size_t len, const uint8_t *aad,
		  size_t aadlen, const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN])
{
	cg_chacha20(ct, pt, len, key, nonce, 1);
	aead_tag(tag, ct, len, aad, aadlen, key, nonce);
}

int cg_aead_open(uint8_t *pt, const uint8_t *ct, size_t len, const uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *aad,
		 size_t aadlen, const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN])
{
	uint8_t want[CG_AEAD_TAG_LEN];
	uint32_t d = 0;

	aead_tag(want, ct, len, aad, aadlen, key, nonce);
	for (int i = 0; i < CG_AEAD_TAG_LEN; i++)
		d |= (uint32_t)(want[i] ^ tag[i]);
	if (d)
		return -1;
	cg_chacha20(pt, ct, len, key, nonce, 1);
	return 0;
}
