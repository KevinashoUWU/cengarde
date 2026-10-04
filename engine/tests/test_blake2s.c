/* BLAKE2s vectors: RFC 7693 appendix B ("abc"), the reference keyed KAT
 * (key 00..1f, message 00..n-1) and Python's hashlib.blake2s for the rest.
 * Pairing vectors (keys, tunnel addresses and the client hint) from
 * hashlib.blake2s with the derivations in pair.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "blake2s.h"
#include "pair.h"
#include "test.h"
#include "util.h"

static int unhex(uint8_t *out, const char *hex)
{
	int n = 0;

	for (; hex[0] && hex[1]; hex += 2) {
		unsigned v = 0;

		for (int i = 0; i < 2; i++) {
			char c = hex[i];

			v = v << 4 | (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
		}
		out[n++] = (uint8_t)v;
	}
	return n;
}

static int hash_is(const void *in, size_t inlen, const void *key, size_t keylen, const char *hex)
{
	uint8_t want[32], got[32];
	int n = unhex(want, hex);

	cg_blake2s(got, (size_t)n, in, inlen, key, keylen);
	return !memcmp(got, want, (size_t)n);
}

void test_blake2s(void)
{
	static const struct {
		size_t len;
		const char *hex;
	} keyed[] = {
		{ 0, "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49" },
		{ 1, "40d15fee7c328830166ac3f918650f807e7e01e177258cdc0a39b11f598066f1" },
		{ 63, "c65382513f07460da39833cb666c5ed82e61b9e998f4b0c4287cee56c3cc9bcd" },
		{ 64, "8975b0577fd35566d750b362b0897a26c399136df07bababbde6203ff2954ed4" },
		{ 65, "21fe0ceb0052be7fb0f004187cacd7de67fa6eb0938d927677f2398c132317a8" },
		{ 127, "ddbfea75cc467882eb3483ce5e2e756a4f4701b76b445519e89f22d60fa86e06" },
		{ 128, "0c311f38c35a4fb90d651c289d486856cd1413df9b0677f53ece2cd9e477c60a" },
		{ 129, "46a73a8dd3e70f59d3942c01df599def783c9da82fd83222cd662b53dce7dbdf" },
		{ 255, "3fb735061abc519dfe979e54c1ee5bfad0a9d858b3315bad34bde999efd724dd" },
	}, plain[] = {
		{ 0, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9" },
		{ 64, "56f34e8b96557e90c1f24b52d0c89d51086acf1b00f634cf1dde9233b8eaaa3e" },
		{ 65, "1b53ee94aaf34e4b159d48de352c7f0661d0a40edff95a0b1639b4090e974472" },
		{ 200, "6d244e1a06ce4ef578dd0f63aff0936706735119ca9c8d22d86c801414ab9741" },
	};
	uint8_t key[32], msg[256], a[32], b[32];
	struct cg_blake2s s;

	for (int i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	for (int i = 0; i < 256; i++)
		msg[i] = (uint8_t)i;

	CHECK(hash_is("abc", 3, NULL, 0, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"));
	CHECK(hash_is("abc", 3, NULL, 0, "aa4938119b1dc7b87cbad0ffd200d0ae")); /* 16-byte output */
	CHECK(hash_is("abc", 3, NULL, 0, "0d"));
	for (size_t i = 0; i < sizeof(keyed) / sizeof(keyed[0]); i++)
		CHECK(hash_is(msg, keyed[i].len, key, 32, keyed[i].hex));
	for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
		CHECK(hash_is(msg, plain[i].len, NULL, 0, plain[i].hex));
	CHECK(hash_is(msg, 100, key, 16, "23f6c104967143ff2fe862cdb21c24d3aab0eec7f420ab12915f3eb1c8ff3f8f"));

	/* Incremental updates in irregular chunks match one-shot, keyed and not. */
	for (size_t keylen = 0; keylen <= 32; keylen += 32) {
		for (size_t len = 0; len <= 200; len++) {
			size_t off = 0, step = 1;

			cg_blake2s_init(&s, 32, key, keylen);
			while (off < len) {
				size_t c = step < len - off ? step : len - off;

				cg_blake2s_update(&s, msg + off, c);
				off += c;
				step = step % 67 + 1;
			}
			cg_blake2s_final(&s, a);
			cg_blake2s(b, 32, msg, len, key, keylen);
			CHECK(!memcmp(a, b, 32));
		}
	}
}

/* Secret of the tunnel edge vectors: 28 zero bytes, then n big-endian. */
static void counter_secret(uint8_t s[CG_PAIR_LEN], uint32_t n)
{
	memset(s, 0, CG_PAIR_LEN);
	s[28] = (uint8_t)(n >> 24);
	s[29] = (uint8_t)(n >> 16);
	s[30] = (uint8_t)(n >> 8);
	s[31] = (uint8_t)n;
}

void test_pair(void)
{
	static const char *want[2][CG_PAIR_NKEYS] = {
		{ "de0c339245be68b01200b54d79b2252ce68c9f003b0c295da272d4a5ef86b2c2",
		  "68674045be71fadf3aa5767aa515cb7ab793dda99808773b2b31d03c79749448",
		  "3027876fd47dd41e566098ca264292b0b94db9526d7f723c983e73e531c90c63",
		  "3c77c5b078ecf32e983c6a1d399d1952b3d623f28b4ce683ba1526364dede60f" },
		{ "35db58444c9a9404df51720b33ae8053e975cf9c08b14f18b03747480989ed0f",
		  "2810950d72e786a47b97c26d4571e268cd1af008e2f66c48be612d443303ba4d",
		  "40be0b23243987470f807af1d9c2fa175b19f2606adfa582af97789c56c7bf72",
		  "b7d45d88e88a061573748c17db6020646cfb9c1d4704ae139a4c2646e8b13329" },
	};
	/* Tunnel addresses of the same secrets, and the hint of their link key. */
	static const struct {
		const char *addr, *ula;
		int hint;
	} tun[2] = {
		{ "0a4fede4", "fd082f26d3b6", 183 }, /* u = 60898: 10.79.237.228, fd08:2f26:d3b6 */
		{ "0a4f2a78", "fdefba8736b1", 120 }, /* u = 10870: 10.79.42.120, fdef:ba87:36b1 */
	};
	/* Counter secrets whose u lands on the edges of v = 2 + u % 65533. */
	static const struct {
		uint32_t n;
		const char *addr;
	} edge[] = {
		{ 44348, "0a4f0002" }, /* u = 0 */
		{ 34118, "0a4ffffe" }, /* u = 65532, the highest: 10.79.255.254 */
		{ 58116, "0a4f0002" }, /* u = 65533 wraps to the lowest */
		{ 15557, "0a4f0003" }, /* u = 65534 */
		{ 9455, "0a4f0004" },  /* u = 65535 */
	};
	uint8_t secret[2][CG_PAIR_LEN], out[CG_PAIR_LEN], exp[CG_PAIR_LEN], a4[4], ula[CG_PAIR_ULA_LEN];
	int bad = 0;

	for (int i = 0; i < CG_PAIR_LEN; i++)
		secret[0][i] = (uint8_t)i;
	CHECK_EQ(cg_base64_decode(secret[1], CG_PAIR_LEN, "q8X0bqk6m3W1yJw3Q0bq5gRZzH1y8dQe9n8b4m2v0Ks="),
		 CG_PAIR_LEN);

	for (int s = 0; s < 2; s++) {
		for (int k = 0; k < CG_PAIR_NKEYS; k++) {
			cg_pair_derive(out, secret[s], (enum cg_pair_key)k);
			unhex(exp, want[s][k]);
			CHECK(!memcmp(out, exp, CG_PAIR_LEN));
		}
		cg_pair_tunnel4(a4, secret[s]);
		CHECK_EQ(unhex(exp, tun[s].addr), 4);
		CHECK(!memcmp(a4, exp, 4));
		cg_pair_tunnel_ula(ula, secret[s]);
		CHECK_EQ(unhex(exp, tun[s].ula), CG_PAIR_ULA_LEN);
		CHECK(!memcmp(ula, exp, CG_PAIR_ULA_LEN));
		cg_pair_derive(out, secret[s], CG_PAIR_LINK);
		CHECK_EQ(cg_client_hint(out), tun[s].hint);
	}

	/* WireGuard private keys are clamped Curve25519 scalars. */
	cg_pair_derive(out, secret[1], CG_PAIR_WG_CLIENT);
	CHECK((out[0] & 7) == 0 && (out[31] & 0xc0) == 0x40);
	CHECK(!strcmp(cg_pair_name(CG_PAIR_WG_PSK), "WG_PSK"));

	/* The hint keyed directly with 00..1f. */
	CHECK_EQ(cg_client_hint(secret[0]), 198);

	for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
		counter_secret(out, edge[i].n);
		cg_pair_tunnel4(a4, out);
		unhex(exp, edge[i].addr);
		CHECK(!memcmp(a4, exp, 4));
	}
	/* Never 10.79.0.0, the VPS's 10.79.0.1 or 10.79.255.255; always a ULA. */
	for (uint32_t n = 0; n < 4096; n++) {
		unsigned v;

		counter_secret(out, n);
		cg_pair_tunnel4(a4, out);
		cg_pair_tunnel_ula(ula, out);
		v = (unsigned)a4[2] << 8 | a4[3];
		bad += a4[0] != 10 || a4[1] != 79 || v < 2 || v > 65534 || ula[0] != 0xfd;
	}
	CHECK_EQ(bad, 0);
}
