/* BLAKE2s (RFC 7693), keyed and unkeyed, incremental API. Used to derive
 * every key of a client/server pair from one secret (pair.h); not on the
 * data path.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_BLAKE2S_H
#define CG_BLAKE2S_H

#include <stddef.h>
#include <stdint.h>

#define CG_BLAKE2S_OUT_MAX 32
#define CG_BLAKE2S_KEY_MAX 32
#define CG_BLAKE2S_BLOCK 64

struct cg_blake2s {
	uint32_t h[8];
	uint32_t t[2]; /* bytes compressed so far */
	uint8_t buf[CG_BLAKE2S_BLOCK];
	size_t buflen;
	size_t outlen;
};

/* outlen 1..32, keylen 0..32 (key may be NULL when keylen is 0). */
void cg_blake2s_init(struct cg_blake2s *s, size_t outlen, const void *key, size_t keylen);
void cg_blake2s_update(struct cg_blake2s *s, const void *in, size_t len);
void cg_blake2s_final(struct cg_blake2s *s, uint8_t *out);

void cg_blake2s(uint8_t *out, size_t outlen, const void *in, size_t inlen, const void *key, size_t keylen);

#endif
