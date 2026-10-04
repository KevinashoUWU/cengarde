/* SPDX-License-Identifier: GPL-2.0-only */
#include "pair.h"

#include <string.h>

#include "blake2s.h"

static const struct {
	const char *label, *name;
	int clamp;
} keys[CG_PAIR_NKEYS] = {
	[CG_PAIR_LINK] = { "link", "LINK_KEY", 0 },
	[CG_PAIR_WG_SERVER] = { "wg-server", "WG_SERVER_KEY", 1 },
	[CG_PAIR_WG_CLIENT] = { "wg-client", "WG_CLIENT_KEY", 1 },
	[CG_PAIR_WG_PSK] = { "wg-psk", "WG_PSK", 0 },
};

static const char prefix[] = "cengarde pairing v1: ";
static const char hint_msg[] = "cengarde v4 client hint";

static void derive(uint8_t out[CG_PAIR_LEN], const uint8_t secret[CG_PAIR_LEN], const char *label)
{
	struct cg_blake2s s;

	cg_blake2s_init(&s, CG_PAIR_LEN, secret, CG_PAIR_LEN);
	cg_blake2s_update(&s, prefix, sizeof(prefix) - 1);
	cg_blake2s_update(&s, label, strlen(label));
	cg_blake2s_final(&s, out);
}

const char *cg_pair_name(enum cg_pair_key k)
{
	return keys[k].name;
}

void cg_pair_derive(uint8_t out[CG_PAIR_LEN], const uint8_t secret[CG_PAIR_LEN], enum cg_pair_key k)
{
	derive(out, secret, keys[k].label);
	if (keys[k].clamp) { /* Curve25519 scalar */
		out[0] &= 248;
		out[31] &= 127;
		out[31] |= 64;
	}
}

void cg_pair_tunnel4(uint8_t out[4], const uint8_t secret[CG_PAIR_LEN])
{
	uint8_t d[CG_PAIR_LEN];
	unsigned v;

	derive(d, secret, "tunnel");
	v = 2 + ((unsigned)d[0] << 8 | d[1]) % 65533;
	out[0] = 10;
	out[1] = 79;
	out[2] = (uint8_t)(v >> 8);
	out[3] = (uint8_t)(v & 255);
	explicit_bzero(d, sizeof(d));
}

void cg_pair_tunnel_ula(uint8_t out[CG_PAIR_ULA_LEN], const uint8_t secret[CG_PAIR_LEN])
{
	uint8_t d[CG_PAIR_LEN];

	derive(d, secret, "tunnel-ula");
	out[0] = 0xfd;
	memcpy(out + 1, d, CG_PAIR_ULA_LEN - 1);
	explicit_bzero(d, sizeof(d));
}

uint8_t cg_client_hint(const uint8_t key[32])
{
	uint8_t d[32], hint;

	cg_blake2s(d, sizeof(d), hint_msg, sizeof(hint_msg) - 1, key, 32);
	hint = d[0];
	explicit_bzero(d, sizeof(d));
	return hint;
}
