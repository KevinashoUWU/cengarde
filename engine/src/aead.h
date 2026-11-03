/* ChaCha20-Poly1305 (RFC 8439), the tunnel's AEAD from the 1.0 format
 * (story 032): ChaCha20 for the body, Poly1305 for the tag over the
 * header (AAD) and the ciphertext. Portable C (32-bit limbs for Poly1305,
 * no 128-bit integers: the routers' MIPS and 32-bit ARM run it as is), no
 * table lookups and no branches on secret data. Pure: no clock, no I/O.
 *
 * Why not AES: the Pi 4 (BCM2711's Cortex-A72) has no ARMv8 crypto
 * extensions and the MIPS routers no AES at all; ChaCha20 in software is
 * faster than AES in software there and does not leak through timing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_AEAD_H
#define CG_AEAD_H

#include <stddef.h>
#include <stdint.h>

#define CG_AEAD_KEY_LEN 32
#define CG_AEAD_NONCE_LEN 12
#define CG_AEAD_TAG_LEN 16

/* ChaCha20 (RFC 8439 2.3, 2.4): out = in XOR the keystream of key, nonce
 * from block counter; in and out may be the same buffer. */
void cg_chacha20(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[CG_AEAD_KEY_LEN],
		 const uint8_t nonce[CG_AEAD_NONCE_LEN], uint32_t counter);

/* One ChaCha20 block (64 bytes) of key, nonce and counter. */
void cg_chacha20_block(uint8_t out[64], const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN],
		       uint32_t counter);

/* Poly1305 (RFC 8439 2.5): the tag of msg under the one-time key. */
struct cg_poly1305 {
#if defined(__SIZEOF_INT128__) && defined(__x86_64__)
	uint64_t r[3], h[3], pad[2]; /* 44-bit limbs (x86-64) */
#else
	uint32_t r[5], h[5], pad[4]; /* 26-bit limbs */
#endif
	uint8_t buf[16];
	size_t left;
};
void cg_poly1305_init(struct cg_poly1305 *st, const uint8_t key[32]);
void cg_poly1305_update(struct cg_poly1305 *st, const uint8_t *m, size_t len);
void cg_poly1305_final(struct cg_poly1305 *st, uint8_t tag[CG_AEAD_TAG_LEN]);
void cg_poly1305(uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *m, size_t len, const uint8_t key[32]);

/* ChaCha20 from byte off of the keystream that starts at block 1 (the
 * AEAD's: off 0 is block 1's first byte): a body sealed or opened in
 * pieces (proto.c's wire). in and out may be the same buffer. */
void cg_chacha20_at(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[CG_AEAD_KEY_LEN],
		    const uint8_t nonce[CG_AEAD_NONCE_LEN], uint64_t off);

/* The AEAD's tag over aad and the ciphertext ct (RFC 8439 2.8's Poly1305
 * input, with the one-time key of block 0). */
void cg_aead_tag(uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *ct, size_t len, const uint8_t *aad, size_t aadlen,
		 const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN]);

/* Whether two tags are the same, in constant time. */
int cg_aead_tag_eq(const uint8_t a[CG_AEAD_TAG_LEN], const uint8_t b[CG_AEAD_TAG_LEN]);

/* The AEAD (RFC 8439 2.8): ct = ChaCha20(key, nonce, from block 1) of pt
 * (len bytes; ct may be pt), and tag = Poly1305 over aad and ct with the
 * one-time key of block 0. */
void cg_aead_seal(uint8_t *ct, uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *pt, size_t len, const uint8_t *aad,
		  size_t aadlen, const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN]);

/* The tag checked first (in constant time), then pt from ct (pt may be
 * ct). 0, or -1 for a forged or damaged packet (pt untouched). */
int cg_aead_open(uint8_t *pt, const uint8_t *ct, size_t len, const uint8_t tag[CG_AEAD_TAG_LEN], const uint8_t *aad,
		 size_t aadlen, const uint8_t key[CG_AEAD_KEY_LEN], const uint8_t nonce[CG_AEAD_NONCE_LEN]);

#endif
