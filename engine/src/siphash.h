/* SipHash-2-4 (Aumasson & Bernstein), incremental API.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_SIPHASH_H
#define CG_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

#define CG_SIPHASH_KEY_LEN 16

struct cg_siphash {
	uint64_t v0, v1, v2, v3;
	uint64_t tail; /* pending input bytes, packed little-endian */
	unsigned ntail; /* number of pending bytes, 0..7 */
	uint64_t len;  /* total bytes absorbed */
};

void cg_siphash_init(struct cg_siphash *s, const uint8_t key[CG_SIPHASH_KEY_LEN]);
void cg_siphash_update(struct cg_siphash *s, const void *data, size_t len);
uint64_t cg_siphash_final(struct cg_siphash *s);

/* One-shot convenience wrapper. */
uint64_t cg_siphash24(const uint8_t key[CG_SIPHASH_KEY_LEN], const void *data, size_t len);

#endif
