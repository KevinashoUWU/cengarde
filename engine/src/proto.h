/* cengarde wire protocol, version 3 (version 1 had no delay reports,
 * version 2 no IP pass flags).
 *
 * Every datagram between client and server carries a 24-byte header followed
 * by the payload (a WireGuard datagram for DATA). All integers are big-endian.
 *
 *   0       ver(4 bits) | type(4 bits)
 *   1       flags          (CG_F_*; 0 on DATA)
 *   2       reserved (0)
 *   3       link id        <- not authenticated, so the sender MACs a packet
 *                             once and patches this byte per link
 *   4..7    session id     (random, chosen by the client)
 *   8..11   sequence       (per session and direction, shared by all types)
 *   12..15  timestamp      (sender monotonic clock, microseconds, wraps)
 *   16..23  MAC            SipHash-2-4 over bytes 0..15 (byte 3 zeroed) and
 *                          the payload, stored little-endian
 *
 * Each direction has its own 16-byte key (client->server, server->client),
 * both taken from the 32-byte shared secret.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PROTO_H
#define CG_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "siphash.h"

#define CG_PROTO_VERSION 3
#define CG_HDR_LEN 24
#define CG_MAC_OFF 16
#define CG_LINK_OFF 3
#define CG_KEY_LEN 32 /* shared secret: c2s key || s2c key */

enum cg_type {
	CG_T_DATA = 1,        /* payload: WireGuard datagram */
	CG_T_PROBE = 2,       /* client -> server, one link; payload: cg_probe_info */
	CG_T_PROBE_REPLY = 3, /* server -> client, same path; payload: cg_probe_info */
};

/* Header flags on probes and probe replies. */
#define CG_F_OWD 0x01   /* cg_probe_info.owd holds a measurement */
#define CG_F_MUTED 0x02 /* the sender carries no payload on this link (link health) */
/* IP pass, the server's public ports forwarded to the client's site: a probe
 * with CG_F_PASS_SET asks for it on (CG_F_PASS) or off; a reply with it says
 * what the server has handed on to apply. The newest session decides. */
#define CG_F_PASS_SET 0x04
#define CG_F_PASS 0x08

/* IP pass as flags, and back: -1 says nothing, 0 off, 1 on. */
static inline uint8_t cg_pass_flags(int pass)
{
	return pass < 0 ? 0 : (uint8_t)(CG_F_PASS_SET | (pass ? CG_F_PASS : 0));
}

static inline int cg_pass_get(uint8_t flags)
{
	return flags & CG_F_PASS_SET ? !!(flags & CG_F_PASS) : -1;
}

struct cg_hdr {
	uint8_t type;
	uint8_t flags;
	uint8_t link;
	uint32_t session;
	uint32_t seq;
	uint32_t ts;
};

/* Probe and probe reply payload: each side reports what it measures on this
 * path in its receive direction, so both ends see both directions.
 *
 * owd is the one-way delay of the newest probe (in a reply) or probe reply
 * (in a probe) received on this path: the receiver's clock at arrival minus
 * the header ts. The two clocks are unrelated, so the value only means
 * something compared with the other paths of the same session; the sender
 * uses it for link health (health.h). */
struct cg_probe_info {
	uint32_t echo_ts;     /* reply: ts of the probe being answered; probe: 0 */
	uint32_t owd;         /* valid with CG_F_OWD */
	uint32_t interval_ms; /* probe: time until the next probe on this path; reply: 0 */
	uint32_t rx;          /* verified packets received on this path (wraps) */
	uint32_t wins;        /* of those, copies that arrived first (wraps) */
	uint32_t lag_us;      /* smoothed delay behind the first copy */
};
#define CG_PROBE_INFO_LEN 24

/* Writes a complete header, MAC included, for hdr and payload. */
void cg_hdr_write(uint8_t out[CG_HDR_LEN], const struct cg_hdr *h, const uint8_t key[CG_SIPHASH_KEY_LEN],
		  const void *payload, size_t plen);

/* Parses buf (header + payload) without checking the MAC. Returns 0 when the
 * header is well formed and the payload length fits the type, -1 otherwise. */
int cg_hdr_parse(struct cg_hdr *h, const uint8_t *buf, size_t len);

/* Returns 1 when the MAC of buf (header + payload) is valid under key. */
int cg_hdr_verify(const uint8_t *buf, size_t len, const uint8_t key[CG_SIPHASH_KEY_LEN]);

static inline void cg_hdr_set_link(uint8_t *hdr, uint8_t link)
{
	hdr[CG_LINK_OFF] = link;
}

void cg_probe_info_write(uint8_t out[CG_PROBE_INFO_LEN], const struct cg_probe_info *pi);
void cg_probe_info_read(struct cg_probe_info *pi, const uint8_t in[CG_PROBE_INFO_LEN]);

/* Does buf have the shape of a WireGuard message (type 1-4, reserved bytes
 * zero, fixed handshake sizes, data >= 32 bytes)? See docs/historias/002. */
int cg_looks_like_wg(const uint8_t *buf, size_t len);

#endif
