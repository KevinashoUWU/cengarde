/* cengarde wire protocol, version 4 (version 1 had no delay reports,
 * version 2 no IP pass flags, version 3 no client hint, cookies or separate
 * control sequence).
 *
 * Every datagram between client and server carries a 24-byte header followed
 * by the payload (a WireGuard datagram for DATA). All integers are big-endian.
 *
 *   0       ver(4 bits) | type(4 bits)
 *   1       flags          (CG_F_*, by type)
 *   2       client hint    (cg_client_hint() of the client's key, pair.h):
 *                          lets a server with several clients pick the key
 *                          before it computes a MAC
 *   3       link id        <- not authenticated, so the sender MACs a packet
 *                             once and patches this byte per link
 *   4..7    session id     (random, chosen by the client)
 *   8..11   sequence       (per session, direction and class: DATA has one,
 *                          the control messages, probes and replies, another,
 *                          so that the DATA sequence has no holes of their
 *                          making; HELLO carries 0)
 *   12..15  timestamp      (sender monotonic clock, microseconds, wraps)
 *   16..23  MAC            SipHash-2-4 over bytes 0..15 (byte 3 zeroed) and
 *                          the payload, stored little-endian
 *
 * Each direction has its own 16-byte key (client->server, server->client),
 * both taken from the 32-byte shared secret.
 *
 * A server takes a new session, a new path (link id) or a path's new address
 * only from a probe that echoes a cookie it handed out in a HELLO to that
 * address (cookie.h): a probe captured earlier and sent again from elsewhere,
 * or after the server restarted, cannot create a session, steer a path or
 * set IP pass. The probe also says where the client's windows stand, so a
 * restarted server goes on ahead of them (cg_probe_info).
 *
 * Fields for what comes after redundancy (bonding, story 012) are reserved
 * now so that it needs no new version: the ECN and split bits of DATA, and
 * the probe payload, which may grow (a receiver reads the fields it knows
 * and ignores the rest).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PROTO_H
#define CG_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "siphash.h"

#define CG_PROTO_VERSION 4
#define CG_HDR_LEN 24
#define CG_MAC_OFF 16
#define CG_HINT_OFF 2
#define CG_LINK_OFF 3
#define CG_KEY_LEN 32 /* shared secret: c2s key || s2c key */

enum cg_type {
	CG_T_DATA = 1,        /* payload: WireGuard datagram */
	CG_T_PROBE = 2,       /* client -> server, one link; payload: cg_probe_info */
	CG_T_PROBE_REPLY = 3, /* server -> client, same path; payload: cg_probe_info */
	CG_T_HELLO = 4,       /* server -> client, to a probe it cannot take yet; payload: cg_hello */
};

/* Probes and replies count in the control sequence, DATA in its own. */
static inline int cg_type_ctl(uint8_t type)
{
	return type == CG_T_PROBE || type == CG_T_PROBE_REPLY;
}

/* Header flags on DATA, reserved for bonding: senders write 0 for now and
 * receivers ignore them. */
#define CG_F_ECN 0x03   /* the ECN field of the WireGuard datagram (RFC 3168 codepoint) */
#define CG_F_SPLIT 0x04 /* the only copy, sent on one link: no other link brings this sequence */

/* Header flags on HELLO. */
#define CG_F_REFUSED 0x01 /* the server will not take the session (limit, client off) */

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
	uint8_t hint;
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
 * uses it for link health (health.h).
 *
 * The last four fields let a server that has no session for a client's probe
 * (it restarted, or the session timed out) take it up where the client is:
 * its own sequences go on 2^20 past the newest the client received
 * (CG_SEQ_LEAP), so that the client's windows take them as new without a
 * reset, and its upload windows start with everything up to tx_data marked
 * as seen, so that nothing the client sent before can be replayed to it.
 *
 * At least CG_PROBE_INFO_LEN bytes; a longer payload is valid and its tail is
 * ignored (room for bonding's per-link report and for padded probes). */
struct cg_probe_info {
	uint32_t echo_ts;     /* reply: ts of the probe being answered; probe: 0 */
	uint32_t owd;         /* valid with CG_F_OWD */
	uint32_t interval_ms; /* probe: time until the next probe on this path; reply: 0 */
	uint32_t rx;          /* verified packets received on this path (wraps) */
	uint32_t wins;        /* of those, copies that arrived first (wraps) */
	uint32_t lag_us;      /* smoothed delay behind the first copy */
	uint32_t cookie;      /* probe: the cookie of the newest HELLO on this link, 0: none; reply: 0 */
	uint32_t rx_top;      /* newest DATA sequence received from the other end (0: none) */
	uint32_t rx_top_ctl;  /* newest control sequence received from it (0: none) */
	uint32_t tx_next;     /* the sender's next DATA sequence */
};
#define CG_PROBE_INFO_LEN 40
#define CG_SEQ_LEAP (1u << 20)

/* HELLO payload: the server answers a probe it cannot take yet. echo_ts ties
 * it to that probe (the client takes each echo once, epoch.h), cookie is what
 * the probes of that link have to carry from then on. */
struct cg_hello {
	uint32_t echo_ts;
	uint32_t cookie;
};
#define CG_HELLO_LEN 8

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
void cg_hello_write(uint8_t out[CG_HELLO_LEN], const struct cg_hello *hl);
void cg_hello_read(struct cg_hello *hl, const uint8_t in[CG_HELLO_LEN]);

/* Size of the IP packet that carries a datagram of len bytes (WireGuard's)
 * over family AF_INET or AF_INET6: IP header, UDP header and ours. */
static inline uint32_t cg_outer_len(int family, uint32_t len)
{
	return len + CG_HDR_LEN + 8 + (family == AF_INET6 ? 40 : 20);
}

/* What WireGuard adds to each packet it carries: 16 bytes of header and 16
 * of tag. The largest WireGuard MTU over a path MTU is therefore
 * mtu - cg_outer_len(family, CG_WG_OVERHEAD): 1416 over IPv4 and 1396 over
 * IPv6 on a 1500-byte path. */
#define CG_WG_OVERHEAD 32

/* Does buf have the shape of a WireGuard message (type 1-4, reserved bytes
 * zero, fixed handshake sizes, data >= 32 bytes)? See docs/historias/002. */
int cg_looks_like_wg(const uint8_t *buf, size_t len);

#endif
