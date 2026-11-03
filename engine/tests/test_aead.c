/* ChaCha20-Poly1305 (aead.c, story 032): RFC 8439's vectors and an
 * independent reference's (aead_vectors.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "aead.h"
#include "test.h"

#include "aead_vectors.h"

void test_aead(void)
{
	/* RFC 8439 2.3.2: a ChaCha20 block */
	{
		uint8_t key[32], out[64];
		static const uint8_t nonce[12] = { 0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
		static const uint8_t want[16] = { 0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
						  0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4 };

		for (int i = 0; i < 32; i++)
			key[i] = (uint8_t)i;
		cg_chacha20_block(out, key, nonce, 1);
		CHECK(!memcmp(out, want, 16));
	}
	/* RFC 8439 2.5.2: Poly1305 */
	{
		static const uint8_t key[32] = { 0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
						 0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
						 0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b };
		static const uint8_t want[16] = { 0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
						  0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9 };
		const char *m = "Cryptographic Forum Research Group";
		uint8_t tag[16];

		cg_poly1305(tag, (const uint8_t *)m, strlen(m), key);
		CHECK(!memcmp(tag, want, 16));
	}
	/* RFC 8439 2.8.2: the AEAD */
	{
		uint8_t key[32], ct[114], tag[16], back[114];
		static const uint8_t nonce[12] = { 7, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
		static const uint8_t aad[12] = { 0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
		static const uint8_t want_tag[16] = { 0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
						      0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91 };
		static const uint8_t want_ct[16] = { 0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
						     0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2 };
		const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
				 "sunscreen would be it.";

		for (int i = 0; i < 32; i++)
			key[i] = (uint8_t)(0x80 + i);
		CHECK_EQ(strlen(pt), 114u);
		cg_aead_seal(ct, tag, (const uint8_t *)pt, 114, aad, sizeof(aad), key, nonce);
		CHECK(!memcmp(tag, want_tag, 16));
		CHECK(!memcmp(ct, want_ct, 16));
		CHECK_EQ(cg_aead_open(back, ct, 114, tag, aad, sizeof(aad), key, nonce), 0);
		CHECK(!memcmp(back, pt, 114));
	}
	/* the reference's vectors: seal, open, in place, and any change refused */
	for (size_t v = 0; v < sizeof(aead_vecs) / sizeof(aead_vecs[0]); v++) {
		const struct aead_vec *a = &aead_vecs[v];
		static uint8_t buf[1424], tag[16];

		cg_aead_seal(buf, tag, a->pt, a->len, a->aad, a->aadlen, a->key, a->nonce);
		CHECK(!memcmp(buf, a->ct, a->len));
		CHECK(!memcmp(tag, a->tag, 16));
		memcpy(buf, a->ct, a->len);
		CHECK_EQ(cg_aead_open(buf, buf, a->len, a->tag, a->aad, a->aadlen, a->key, a->nonce), 0);
		CHECK(!memcmp(buf, a->pt, a->len));
		memcpy(buf, a->ct, a->len);
		if (a->len) {
			buf[a->len / 2] ^= 0x10; /* a bit of the body */
			CHECK_EQ(cg_aead_open(buf, buf, a->len, a->tag, a->aad, a->aadlen, a->key, a->nonce), -1);
			CHECK_EQ(buf[a->len / 2], a->ct[a->len / 2] ^ 0x10); /* refused: left as it came */
			buf[a->len / 2] ^= 0x10;
		}
		memcpy(tag, a->tag, 16);
		tag[15] ^= 1; /* the tag */
		CHECK_EQ(cg_aead_open(buf, buf, a->len, tag, a->aad, a->aadlen, a->key, a->nonce), -1);
		if (a->aadlen) {
			uint8_t aad[32];

			memcpy(aad, a->aad, a->aadlen);
			aad[0] ^= 0x80; /* the header */
			CHECK_EQ(cg_aead_open(buf, buf, a->len, a->tag, aad, a->aadlen, a->key, a->nonce), -1);
		}
	}
	/* Poly1305 fed in pieces gives the one-shot tag */
	for (size_t v = 0; v < sizeof(poly_vecs) / sizeof(poly_vecs[0]); v++) {
		const struct poly_vec *p = &poly_vecs[v];
		struct cg_poly1305 st;
		uint8_t tag[16];
		size_t at = 0, step = 1;

		cg_poly1305(tag, p->msg, p->len, p->key);
		CHECK(!memcmp(tag, p->tag, 16));
		cg_poly1305_init(&st, p->key);
		while (at < p->len) {
			size_t n = p->len - at < step ? p->len - at : step;

			cg_poly1305_update(&st, p->msg + at, n);
			at += n;
			step = step * 3 % 37 + 1;
		}
		cg_poly1305_final(&st, tag);
		CHECK(!memcmp(tag, p->tag, 16));
	}
}
