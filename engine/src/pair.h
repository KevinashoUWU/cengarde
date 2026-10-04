/* One-secret pairing: every key a client/server pair needs (the cengarde
 * link key and both WireGuard ends) derives from a single 32-byte secret,
 * so the router and the VPS only have to share that one value.
 *
 *   key(label) = BLAKE2s-256(key = secret, "cengarde pairing v1: " label)
 *
 * WireGuard private keys come out clamped, as "wg genkey" prints them.
 * The router's tunnel addresses come from the same formula, so nobody has
 * to pick them (cg_pair_tunnel4, cg_pair_tunnel_ula).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PAIR_H
#define CG_PAIR_H

#include <stdint.h>

#define CG_PAIR_LEN 32
#define CG_PAIR_ULA_LEN 6 /* a /48 */

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

/* The router's IPv4 address inside the tunnel, 10.79.x.y in network order.
 * With d = key("tunnel") and u = d[0] << 8 | d[1] (big-endian, read byte by
 * byte, so every host gets the same address): v = 2 + u % 65533, and the
 * address is 10.79.(v >> 8).(v & 255). v stays in 2..65534, never 10.79.0.0,
 * the VPS's 10.79.0.1 or 10.79.255.255. */
void cg_pair_tunnel4(uint8_t out[4], const uint8_t secret[CG_PAIR_LEN]);

/* The router's IPv6 ULA prefix, a /48 in network order: fd followed by
 * key("tunnel-ula")[0..4]. The tunnel uses its ::/64. */
void cg_pair_tunnel_ula(uint8_t out[CG_PAIR_ULA_LEN], const uint8_t secret[CG_PAIR_LEN]);

/* Client hint, the 8 bits that byte 2 of the protocol v4 header will carry
 * so that the server can pick a client's key before computing a MAC (no
 * packet carries it yet):
 *
 *   BLAKE2s-256(key = link key, "cengarde v4 client hint")[0]
 *
 * key is the 32-byte cengarde "key" (CG_KEY_LEN). Meant to be computed
 * when the configuration loads, never per packet. */
uint8_t cg_client_hint(const uint8_t key[32]);

#endif
