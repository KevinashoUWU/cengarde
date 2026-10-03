/* SipHash-2-4, written from the reference description
 * (https://131002.net/siphash/siphash.pdf).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "siphash.h"

#include <string.h>

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND                                                                                   \
	do {                                                                                       \
		v0 += v1;                                                                          \
		v1 = ROTL(v1, 13);                                                                 \
		v1 ^= v0;                                                                          \
		v0 = ROTL(v0, 32);                                                                 \
		v2 += v3;                                                                          \
		v3 = ROTL(v3, 16);                                                                 \
		v3 ^= v2;                                                                          \
		v0 += v3;                                                                          \
		v3 = ROTL(v3, 21);                                                                 \
		v3 ^= v0;                                                                          \
		v2 += v1;                                                                          \
		v1 = ROTL(v1, 17);                                                                 \
		v1 ^= v2;                                                                          \
		v2 = ROTL(v2, 32);                                                                 \
	} while (0)

static inline uint64_t load64_le(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	v = __builtin_bswap64(v);
#endif
	return v;
}

void cg_siphash_init(struct cg_siphash *s, const uint8_t key[CG_SIPHASH_KEY_LEN])
{
	uint64_t k0 = load64_le(key), k1 = load64_le(key + 8);

	s->v0 = 0x736f6d6570736575ULL ^ k0;
	s->v1 = 0x646f72616e646f6dULL ^ k1;
	s->v2 = 0x6c7967656e657261ULL ^ k0;
	s->v3 = 0x7465646279746573ULL ^ k1;
	s->tail = 0;
	s->ntail = 0;
	s->len = 0;
}

void cg_siphash_update(struct cg_siphash *s, const void *data, size_t len)
{
	const uint8_t *p = data;
	uint64_t v0 = s->v0, v1 = s->v1, v2 = s->v2, v3 = s->v3, m;

	s->len += len;
	if (s->ntail) {
		while (len && s->ntail < 8) {
			s->tail |= (uint64_t)*p++ << (8 * s->ntail++);
			len--;
		}
		if (s->ntail < 8)
			return;
		m = s->tail;
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
		s->tail = 0;
		s->ntail = 0;
	}
	while (len >= 8) {
		m = load64_le(p);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
		p += 8;
		len -= 8;
	}
	while (len) {
		s->tail |= (uint64_t)*p++ << (8 * s->ntail++);
		len--;
	}
	s->v0 = v0;
	s->v1 = v1;
	s->v2 = v2;
	s->v3 = v3;
}

uint64_t cg_siphash_final(struct cg_siphash *s)
{
	uint64_t v0 = s->v0, v1 = s->v1, v2 = s->v2, v3 = s->v3;
	uint64_t b = (s->len << 56) | s->tail;

	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t cg_siphash24(const uint8_t key[CG_SIPHASH_KEY_LEN], const void *data, size_t len)
{
	struct cg_siphash s;

	cg_siphash_init(&s, key);
	cg_siphash_update(&s, data, len);
	return cg_siphash_final(&s);
}
