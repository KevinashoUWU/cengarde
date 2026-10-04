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

const char *cg_pair_name(enum cg_pair_key k)
{
	return keys[k].name;
}

void cg_pair_derive(uint8_t out[CG_PAIR_LEN], const uint8_t secret[CG_PAIR_LEN], enum cg_pair_key k)
{
	struct cg_blake2s s;

	cg_blake2s_init(&s, CG_PAIR_LEN, secret, CG_PAIR_LEN);
	cg_blake2s_update(&s, prefix, sizeof(prefix) - 1);
	cg_blake2s_update(&s, keys[k].label, strlen(keys[k].label));
	cg_blake2s_final(&s, out);
	if (keys[k].clamp) { /* Curve25519 scalar */
		out[0] &= 248;
		out[31] &= 127;
		out[31] |= 64;
	}
}
