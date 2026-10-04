/* One-secret pairing: every key a client/server pair needs (the cengarde
 * link key and both WireGuard ends) derives from a single 32-byte secret,
 * so the router and the VPS only have to share that one value.
 *
 *   key(label) = BLAKE2s-256(key = secret, "cengarde pairing v1: " label)
 *
 * WireGuard private keys come out clamped, as "wg genkey" prints them.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PAIR_H
#define CG_PAIR_H

#include <stdint.h>

#define CG_PAIR_LEN 32

enum cg_pair_key {
	CG_PAIR_LINK,      /* cengarde "key" on both ends */
	CG_PAIR_WG_SERVER, /* WireGuard private key of the VPS */
	CG_PAIR_WG_CLIENT, /* WireGuard private key of the router */
	CG_PAIR_WG_PSK,    /* WireGuard preshared key */
	CG_PAIR_NKEYS
};

/* Name for "cengarde keys" output, e.g. "WG_SERVER_KEY". */
const char *cg_pair_name(enum cg_pair_key k);

void cg_pair_derive(uint8_t out[CG_PAIR_LEN], const uint8_t secret[CG_PAIR_LEN], enum cg_pair_key k);

#endif
