/* BLAKE2s, written from RFC 7693.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "blake2s.h"

#include <string.h>

static const uint32_t iv[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
				0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };

static const uint8_t sigma[10][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

static inline uint32_t rotr32(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static inline uint32_t load32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define G(a, b, c, d, x, y)                                                                        \
	do {                                                                                       \
		v[a] = v[a] + v[b] + (x);                                                          \
		v[d] = rotr32(v[d] ^ v[a], 16);                                                    \
		v[c] = v[c] + v[d];                                                                \
		v[b] = rotr32(v[b] ^ v[c], 12);                                                    \
		v[a] = v[a] + v[b] + (y);                                                          \
		v[d] = rotr32(v[d] ^ v[a], 8);                                                     \
		v[c] = v[c] + v[d];                                                                \
		v[b] = rotr32(v[b] ^ v[c], 7);                                                     \
	} while (0)

static void compress(struct cg_blake2s *s, const uint8_t block[CG_BLAKE2S_BLOCK], int last)
{
	uint32_t m[16], v[16];

	for (int i = 0; i < 16; i++)
		m[i] = load32_le(block + 4 * i);
	for (int i = 0; i < 8; i++) {
		v[i] = s->h[i];
		v[i + 8] = iv[i];
	}
	v[12] ^= s->t[0];
	v[13] ^= s->t[1];
	if (last)
		v[14] = ~v[14];
	for (int r = 0; r < 10; r++) {
		const uint8_t *z = sigma[r];

		G(0, 4, 8, 12, m[z[0]], m[z[1]]);
		G(1, 5, 9, 13, m[z[2]], m[z[3]]);
		G(2, 6, 10, 14, m[z[4]], m[z[5]]);
		G(3, 7, 11, 15, m[z[6]], m[z[7]]);
		G(0, 5, 10, 15, m[z[8]], m[z[9]]);
		G(1, 6, 11, 12, m[z[10]], m[z[11]]);
		G(2, 7, 8, 13, m[z[12]], m[z[13]]);
		G(3, 4, 9, 14, m[z[14]], m[z[15]]);
	}
	for (int i = 0; i < 8; i++)
		s->h[i] ^= v[i] ^ v[i + 8];
}

static void count(struct cg_blake2s *s, uint32_t n)
{
	s->t[0] += n;
	if (s->t[0] < n)
		s->t[1]++;
}

void cg_blake2s_init(struct cg_blake2s *s, size_t outlen, const void *key, size_t keylen)
{
	memset(s, 0, sizeof(*s));
	memcpy(s->h, iv, sizeof(iv));
	s->h[0] ^= 0x01010000u ^ (uint32_t)(keylen << 8) ^ (uint32_t)outlen;
	s->outlen = outlen;
	if (keylen) {
		memcpy(s->buf, key, keylen); /* the key is a full zero-padded first block */
		s->buflen = CG_BLAKE2S_BLOCK;
	}
}

void cg_blake2s_update(struct cg_blake2s *s, const void *in, size_t len)
{
	const uint8_t *p = in;

	while (len) {
		size_t n;

		/* Compress a full buffer only when more input follows: the last
		 * block must be compressed by final(). */
		if (s->buflen == CG_BLAKE2S_BLOCK) {
			count(s, CG_BLAKE2S_BLOCK);
			compress(s, s->buf, 0);
			s->buflen = 0;
		}
		n = CG_BLAKE2S_BLOCK - s->buflen;
		if (n > len)
			n = len;
		memcpy(s->buf + s->buflen, p, n);
		s->buflen += n;
		p += n;
		len -= n;
	}
}

void cg_blake2s_final(struct cg_blake2s *s, uint8_t *out)
{
	count(s, (uint32_t)s->buflen);
	memset(s->buf + s->buflen, 0, CG_BLAKE2S_BLOCK - s->buflen);
	compress(s, s->buf, 1);
	for (size_t i = 0; i < s->outlen; i++)
		out[i] = (uint8_t)(s->h[i / 4] >> (8 * (i % 4)));
	memset(s, 0, sizeof(*s));
}

void cg_blake2s(uint8_t *out, size_t outlen, const void *in, size_t inlen, const void *key, size_t keylen)
{
	struct cg_blake2s s;

	cg_blake2s_init(&s, outlen, key, keylen);
	cg_blake2s_update(&s, in, inlen);
	cg_blake2s_final(&s, out);
}
