/* Vectors cross-checked against OpenSSL 3.0 (EVP_MAC SIPHASH, size 8) and
 * the example in the SipHash paper (appendix A).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "siphash.h"
#include "test.h"

void test_siphash(void)
{
	uint8_t key[16], msg[1452];
	struct cg_siphash s;

	for (int i = 0; i < 16; i++)
		key[i] = (uint8_t)i;
	for (int i = 0; i < 15; i++)
		msg[i] = (uint8_t)i;

	/* Empty message and the paper's 15-byte example (key 00..0f, msg 00..0e). */
	CHECK(cg_siphash24(key, msg, 0) == 0x726fdb47dd0e0e31ULL);
	CHECK(cg_siphash24(key, msg, 15) == 0xa129ca6149be45e5ULL);

	/* msg[i] = i*7+3, as fed to "openssl mac ... SIPHASH" (printed as LE bytes). */
	for (int i = 0; i < (int)sizeof(msg); i++)
		msg[i] = (uint8_t)(i * 7 + 3);
	CHECK(cg_siphash24(key, msg, 1) == 0x13d7290c4face4b3ULL);
	CHECK(cg_siphash24(key, msg, 8) == 0x842e146945dd0ad0ULL);
	CHECK(cg_siphash24(key, msg, 64) == 0x25a000cacd85c5abULL);
	CHECK(cg_siphash24(key, msg, 1400) == 0xe9cc2a7b30463ce3ULL);
	CHECK(cg_siphash24(key, msg, 1452) == 0xb7f9ccb922375704ULL);

	/* Incremental updates in irregular chunks match one-shot. */
	for (size_t len = 0; len <= 100; len++) {
		size_t off = 0, step = 1;

		cg_siphash_init(&s, key);
		while (off < len) {
			size_t c = step < len - off ? step : len - off;

			cg_siphash_update(&s, msg + off, c);
			off += c;
			step = step % 11 + 1;
		}
		CHECK(cg_siphash_final(&s) == cg_siphash24(key, msg, len));
	}
}
