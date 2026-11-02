/* cengarde wire protocol, version 16 (version 1 had no delay reports,
 * version 2 no IP pass flags, version 3 no client hint, cookies or separate
 * control sequence, version 4 no previous link in DATA's flags, version 5
 * no NACK, version 6 no CG_F_FREE, version 7 no parity, version 8 no
 * repairs of an erasure code, version 9 no parity asked for again and no
 * CG_F_PREV written after the copies were decided, version 10 no
 * protocontroller's streams in DATA, story 025: protoctl.h; version 11 adds
 * FEC v3's wide windows, story 026: CG_REP_RLCW repairs, repair.h, and
 * repairs and parity requests either way, and the active mask after the
 * bonding tail, cg_probe_act: the links the sender carries DATA on, for
 * the receiver's reorderer, active.h; version 12 adds each link's own
 * sequence, sseq, and SFLOSS / SFACK, story 029; version 13 moves the
 * protocontroller's stream messages out of DATA into their own class,
 * CG_T_STREAM, with its own sequence: the reorderer, the rescue and the
 * codes see the TUN's DATA only, GRIT recovers its own from the subflows,
 * story 030; version 14 drops the stream messages' own tag, protoctl.h:
 * the MAC covers every one of them; version 15 their DATA's own clock: the
 * header's ts is it; version 16 puts the link and the sseq under the MAC,
 * last, so that each copy only finishes it: cg_hdr_mac_finish).
 *
 * Every datagram between client and server carries a 24-byte header followed
 * by the payload (an IP packet from the TUN for DATA, or a protocontroller
 * stream's message). All integers are big-endian.
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
 *                          STREAM another, the control messages, probes and
 *                          replies, a third, so that the DATA sequence has no
 *                          holes of their making; HELLO carries 0)
 *   12..15  timestamp      (sender monotonic clock, microseconds, wraps)
 *   16..23  MAC            SipHash-2-4 over bytes 0..15 (byte 3 zeroed) and
 *                          the payload, stored little-endian
 *
 * Each direction has its own 16-byte key (client->server, server->client),
 * both taken from the 32-byte shared secret.
 *
 * Invariant of DATA: the sender writes the sequence and the timestamp
 * together and in order, so a packet with a higher sequence never carries an
 * older timestamp (serial arithmetic, both wrap). The receiver's reorderer
 * (story 012) bounds how long it waits for a missing sequence with the
 * timestamp of a packet that did arrive after it, and the bound is only
 * sound if that timestamp is not older than the missing packet's.
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
 * and ignores the rest). The bonding tail of probes and replies
 * (cg_probe_bond) is the first such growth, and the reason it is still
 * version 4 (stories 010 and 012): an engine without it reads the first 40
 * bytes and ignores the 4 after them, one with it reads no tail from a
 * 40-byte peer, and the MAC covers the whole payload either way.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PROTO_H
#define CG_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "siphash.h"

#define CG_PROTO_VERSION 16
/* The version in the first byte's high nibble, 1 to 15: past 15 it goes
 * round from 1 (16 is 1, 17 is 2), over versions retired long ago (every
 * engine still running speaks 12 or more). Never 0: a WireGuard message
 * (first byte 1-4) or most junk starts with it, and goes before any MAC. */
#define CG_PROTO_WIRE ((CG_PROTO_VERSION - 1) % 15 + 1)
#define CG_HDR_LEN 28
#define CG_TS_OFF 12
#define CG_MAC_OFF 16
#define CG_SSEQ_OFF 24 /* the link's own sequence (MPGRIT's subflow, story 029): after the MAC field; the MAC covers it last with the link (16), so a copy on another link finishes the MAC with its own */
#define CG_HINT_OFF 2
#define CG_LINK_OFF 3
#define CG_KEY_LEN 32 /* shared secret: c2s key || s2c key */

enum cg_type {
	CG_T_DATA = 1,        /* payload: an IP packet (or a protocontroller stream's message) */
	CG_T_PROBE = 2,       /* client -> server, one link; payload: cg_probe_info */
	CG_T_PROBE_REPLY = 3, /* server -> client, same path; payload: cg_probe_info */
	CG_T_HELLO = 4,       /* server -> client, to a probe it cannot take yet; payload: cg_hello */
	CG_T_NACK = 5,        /* either way, one link; payload: DATA sequences to send again */
	CG_T_FEC = 6,         /* either way, one link; payload: the parity of a group of DATA (fec.h) */
	CG_T_REPAIR = 7,      /* either way (client -> server before 11), one link; payload: an erasure code's repair (repair.h) */
	CG_T_PARQ = 8,        /* either way (server -> client before 11), one link; payload: repairs asked for again (harq.h) */
	CG_T_SFLOSS = 9,      /* either way (12), one link; payload: a link id, then the sseqs that link lost as a NACK's ranges (subflow.h) */
	CG_T_SFACK = 10,      /* either way (12), one link; payload: how far each link got, and the bytes it took (subflow.h) */
	CG_T_STREAM = 11,     /* either way (13); payload: a protocontroller stream's message (protoctl.h), its own sequence */
};

/* The anti-replay class of a type: DATA, STREAM, or the control messages. */
enum { CG_CLS_DATA = 0, CG_CLS_STREAM, CG_CLS_CTL };

/* Probes, replies, NACKs and parities count in the control sequence, DATA
 * in its own. */
static inline int cg_type_ctl(uint8_t type)
{
	return type == CG_T_PROBE || type == CG_T_PROBE_REPLY || type == CG_T_NACK || type == CG_T_FEC ||
	       type == CG_T_REPAIR || type == CG_T_PARQ || type == CG_T_SFLOSS || type == CG_T_SFACK;
}

static inline int cg_type_cls(uint8_t type)
{
	return type == CG_T_DATA ? CG_CLS_DATA : type == CG_T_STREAM ? CG_CLS_STREAM : CG_CLS_CTL;
}

/* PARQ (version 9, story 023: the parity asked for again): the server,
 * whose reorderer misses DATA that the client's erasure code covered but
 * could not rebuild (or whose repairs were lost), asks for the code's
 * rank deficit instead of the packets: fresh combinations of exactly the
 * missing members (RLC), or more indices of the same Reed-Solomon block
 * (its repairs declare the block's geometry, m, larger than what the plan
 * sends). The client rebuilds them from what it keeps for the rescue
 * (rescue.h) and sends them as CG_T_REPAIR on a link with room. Payload:
 * harq.h's (a count, then requests of CG_PARQ_REQ bytes). */
#define CG_PARQ_REQ 20
#define CG_PARQ_MAX 8
#define CG_PARQ_LEN (1 + CG_PARQ_REQ * CG_PARQ_MAX)

/* REPAIR (version 8, story 021: redund_spare = rs | rlc): a repair of an
 * erasure code over DATA the client sent on its first link alone, carried
 * on another link's spare (spare.h): a Reed-Solomon block's (Leopard, rs.h)
 * or a sliding window's (rlc.h); the server rebuilds the DATA missing once
 * it has enough of them. Payload: repair.h's (base sequence, members' map,
 * kind, m, index or seed, the symbol). A server without fec drops it,
 * counted. Version 11 (story 026, FEC v3: fec_code = rlc): also a wide
 * window's repair over every DATA of a split or of a lone link (kind
 * CG_REP_RLCW: base, span, density and seed instead of a map), sent by
 * either end over the DATA it sends; the receiver without fec drops it. */

/* FEC (version 7, story 013: the spatial parity, fec.h): the sender of
 * split DATA sends, after each group of packets that went one per link, a
 * parity of them on a link outside the group; the receiver rebuilds one
 * packet of the group lost or late from it, before the rescue asks.
 * Payload: fec.h's (base sequence, count, lengths' XOR, the payloads' XOR).
 * A receiver without fec drops it, counted. */

/* NACK (version 5, story 013: the rescue): the receiver's reorderer is about
 * to give up these DATA sequences, and asks the sender, which keeps what it
 * sent for a while (CG_BOND_RESCUE in its probes or replies), to send them
 * again on its fastest path: the client for the download, the server for
 * the upload. It goes on the receiver's two fastest links (one may lose it),
 * and the sender sends a sequence once for both. Payload: a count k of
 * ranges (1 to CG_NACK_RANGES), then k ranges of 12 bytes: a big-endian
 * sequence, asked for, and a big-endian 64-bit mask whose bit i asks for
 * that sequence + 1 + i. CG_F_RETRY: these were asked for before and did not
 * come; the sender sends each on two paths. */
#define CG_NACK_RANGES 16
#define CG_NACK_LEN (1 + 12 * CG_NACK_RANGES)
#define CG_NACK_SEQ_MAX (65 * CG_NACK_RANGES) /* sequences one NACK can ask for */
#define CG_F_RETRY 0x01 /* NACK: asked for again */
/* NACK, from the one asked: these will not be sent again (not kept, lost on
 * a full path, out of budget); the asker gives them up at once instead of
 * waiting out rescue_wait_ms for what never comes. */
#define CG_F_GONE 0x02

/* Header flags on DATA, for bonding (story 012). CG_F_SPLIT is written
 * only by a sender that splits (split.h), and is under the MAC: this packet
 * skips at least one carrier, some link that carries payload does not bring
 * this sequence (a weight-0 link may bring it too, so it is not "the only
 * copy"). */
/* CG_F_TUNHOLE (story 023, 2026-10-24; reserved before, written 0 and
 * ignored, as bit 0x02 still is by a peer that predates it): with tunnel =
 * native and tcp_aware, a TCP ACK the sender read into the tunnel with a
 * SACK hole the tunnel lost on every link (tcpfree.h's cg_tcpe_in); the
 * other end, which sends that flow's data, sends the retransmissions of
 * those holes copied (tcpack.h). Without it, with the peer's
 * CG_BOND_TCPMARK, the tunnel did not lose them as far as it knows
 * (delivered and lost beyond it, or still on their way).
 * Why no new version: like CG_BOND_TYPE, a peer that predates it writes 0
 * and ignores it, and reads as "not known" whatever a peer writes that does
 * not say CG_BOND_TCPMARK. */
#define CG_F_TUNHOLE 0x02
/* Version 6 (story 014, tcpfree.h): the receiver's reorderer hands this
 * packet on at once, never holding it nor waiting for a gap before it:
 * with tunnel = native, the sender's mark on inner TCP's retransmissions
 * and segments without data. */
#define CG_F_FREE 0x01
#define CG_F_SPLIT 0x04 /* this packet skips at least one carrier */
/* The link the previous DATA sequence went on, when it went on that link
 * alone (version 5, story 013): CG_F_PREV, and its id in the high 4 bits.
 * The receiver's reorderer then waits for that gap as long as that link
 * may take, not the slowest, and gives it up as soon as that link brings a
 * later sequence (links are FIFO). Under the MAC, as every flag. */
#define CG_F_PREV 0x08
#define CG_F_PREV_SHIFT 4

/* DATA flags for a packet whose previous sequence went on link alone
 * (0-15), or on several (-1). */
static inline uint8_t cg_prev_flags(int link)
{
	return link < 0 || link > 15 ? 0 : (uint8_t)(CG_F_PREV | link << CG_F_PREV_SHIFT);
}

/* The link the previous sequence went on alone, from DATA flags; -1: not said. */
static inline int cg_prev_link(uint8_t flags)
{
	return flags & CG_F_PREV ? flags >> CG_F_PREV_SHIFT : -1;
}

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
	uint32_t sseq; /* the link's own sequence of a DATA (subflow.h, 12); 0: not numbered */
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
 * At least CG_PROBE_INFO_LEN bytes; a longer payload is valid and what comes
 * after the fields a receiver knows is ignored: cg_probe_bond at
 * CG_PROBE_INFO_LEN, then room for bonding's per-link report and for padded
 * probes. */
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
	uint32_t rx_top_stream; /* newest STREAM sequence received from it (0: none; 13) */
	uint32_t tx_next_stream; /* the sender's next STREAM sequence (13) */
	uint32_t rx_sseq;     /* newest sseq received on this path (0: none; 13): what its loss counts against (capacity.h) */
};
#define CG_PROBE_INFO_LEN 52
#define CG_SEQ_LEAP (1u << 20)

/* The bonding tail (story 012, the field mode of split.h): 4 bytes after
 * cg_probe_info, a big-endian word whose bits say
 *   0-9  weight: the sender's share of the split for this link, 0 to 1000
 *        (0: it carries a copy of every packet; config.h's [link] weight);
 *   16   CG_BOND_REORDERS: the sender's receive side puts DATA back in
 *        sequence (reorder.h);
 *   17   CG_BOND_SPLIT_ME: the sender asks to receive split DATA;
 *   18   CG_BOND_RESCUE: the sender keeps the DATA it sent and sends it
 *        again on a NACK (version 5);
 *   19   CG_BOND_FULL: but not what is lost on this path now: its queue is
 *        full (that loss is for TCP to see) or the rescue budget is spent,
 *        so the other end does not ask and wait;
 *   20-22 CG_BOND_TYPE: the client's type of this link (linkmodel.h, story
 *        015), so that the server treats its download alike; 0 is auto,
 *        today's behaviour, and a reply writes 0;
 *   23   CG_BOND_TCPMARK: the sender marks the TCP ACKs it reads into the
 *        tunnel CG_F_TUNHOLE (story 023: tunnel = native, tcp_aware), so
 *        that an ACK without the mark says its holes were delivered; a
 *        reply carries it too (the server's own marks), and without it
 *        the mark means nothing;
 * and every other bit is reserved: written 0, ignored when read, so that a
 * newer peer can use them. CG_BOND_TYPE is the first such use, and why it
 * needs no new version: a peer that predates it reads those bits as
 * reserved and ignores them (auto, as before), and one that has it reads 0
 * (auto) from a peer that wrote them 0, as reserved bits are written. The
 * MAC covers them either way. Bits 20+ are reserved alike in versions 6
 * and 7, so it is the same in both. CG_BOND_TCPMARK is the second such use,
 * in version 9, for the same reason. A client sends it in every probe; a server
 * answers with one only a probe that had one, so that a reply is never
 * larger than its probe, and a reply's weight and CG_BOND_SPLIT_ME are 0
 * (the client's upload split depends only on the server reordering). */
#define CG_PROBE_BOND_LEN (CG_PROBE_INFO_LEN + 4)
#define CG_BOND_WEIGHT 0x3ffu
#define CG_BOND_WEIGHT_MAX 1000
#define CG_BOND_REORDERS (1u << 16)
#define CG_BOND_SPLIT_ME (1u << 17)
#define CG_BOND_RESCUE (1u << 18) /* the sender sends DATA again on a NACK */
#define CG_BOND_FULL (1u << 19)   /* but not what is lost on this path now (full, or out of budget) */
#define CG_BOND_TYPE_SHIFT 20
#define CG_BOND_TYPE (7u << CG_BOND_TYPE_SHIFT) /* the client's type of this link (linkmodel.h), 0: auto */
#define CG_BOND_TCPMARK (1u << 23) /* the sender marks its TCP ACKs CG_F_TUNHOLE (tcp_aware) */

struct cg_probe_bond {
	uint16_t weight;  /* 0 to CG_BOND_WEIGHT_MAX */
	uint8_t reorders; /* CG_BOND_REORDERS */
	uint8_t split_me; /* CG_BOND_SPLIT_ME */
	uint8_t rescue;   /* CG_BOND_RESCUE */
	uint8_t full;     /* CG_BOND_FULL */
	uint8_t type;     /* CG_BOND_TYPE: enum cg_link_type, 0 to 7 as written (a reader normalizes it) */
	uint8_t tcpmark;  /* CG_BOND_TCPMARK */
};

/* The active mask (version 11, clockwork audit C2, active.h): 4 bytes after
 * the bonding tail, at CG_PROBE_BOND_LEN: a big-endian 16-bit mask whose
 * bit i says the sender carried DATA, a copy, a packet sent again, a parity
 * or a repair on link i within hold before this message's ts, and the hold
 * in ms (big-endian 16 bits). A link out of the mask carried nothing sent
 * after ts - hold; for hold after a start or a reload every link is in it.
 * The receiver's reorderer waits for a gap only on the links it does not
 * mute that are in the newest mask, or that it knows carried something
 * since, or whose packets may still be on their way (active.h). A client
 * sends it in every probe (its upload), a server in its reply to a probe
 * that had one (the download): a reply is never larger than its probe. A
 * receiver without it reads 0 (cg_probe_act_read) and waits for every link
 * it does not mute, as before. */
#define CG_PROBE_ACT_LEN (CG_PROBE_BOND_LEN + 4)

struct cg_probe_act {
	uint16_t mask;    /* links that carried something within hold_ms before ts */
	uint16_t hold_ms; /* the sender's hold */
};

/* HELLO payload: the server answers a probe it cannot take yet. echo_ts ties
 * it to that probe (the client takes each echo once, epoch.h), cookie is what
 * the probes of that link have to carry from then on. */
struct cg_hello {
	uint32_t echo_ts;
	uint32_t cookie;
};
#define CG_HELLO_LEN 8

/* Writes a complete header, MAC included, for hdr and payload. The MAC
 * (16) covers the header without its link, the payload, and then the link
 * and the sseq: a copy that goes on another link, with that link's sseq,
 * only finishes it from the base (cg_hdr_mac_finish, ~0.1 us on a
 * Cortex-A53 against ~2.1 us for 1400 bytes), and neither can be rewritten
 * on the way (a forged sseq made the receiver say losses that never were). */
void cg_hdr_write(uint8_t out[CG_HDR_LEN], const struct cg_hdr *h, const uint8_t key[CG_SIPHASH_KEY_LEN],
		  const void *payload, size_t plen);
/* The same, and *base: the MAC's state before the link and the sseq. */
void cg_hdr_write_base(uint8_t out[CG_HDR_LEN], const struct cg_hdr *h, const uint8_t key[CG_SIPHASH_KEY_LEN],
		       const void *payload, size_t plen, struct cg_siphash *base);
/* The MAC's state of hdr (its link left out) and payload under key. */
void cg_hdr_mac_base(struct cg_siphash *s, const uint8_t *hdr, const uint8_t key[CG_SIPHASH_KEY_LEN],
		     const void *payload, size_t plen);
/* The MAC of hdr from base and the link and sseq hdr holds now, into hdr:
 * after cg_hdr_set_link and cg_hdr_set_sseq. */
void cg_hdr_mac_finish(const struct cg_siphash *base, uint8_t *hdr);
/* The whole MAC again (a kept copy sent on another link, a probe given its
 * sseq after it was written). */
void cg_hdr_reseal(uint8_t *hdr, const uint8_t key[CG_SIPHASH_KEY_LEN], const void *payload, size_t plen);

/* Parses buf (header + payload) without checking the MAC. Returns 0 when the
 * header is well formed and the payload length fits the type, -1 otherwise. */
int cg_hdr_parse(struct cg_hdr *h, const uint8_t *buf, size_t len);

/* Returns 1 when the MAC of buf (header + payload) is valid under key. */
int cg_hdr_verify(const uint8_t *buf, size_t len, const uint8_t key[CG_SIPHASH_KEY_LEN]);

static inline void cg_hdr_set_link(uint8_t *hdr, uint8_t link)
{
	hdr[CG_LINK_OFF] = link;
}

/* The ts of a header the sender kept (its own copy, already verified when
 * sealed): bytes 12..15, big-endian. */
static inline uint32_t cg_hdr_ts(const uint8_t *hdr)
{
	return (uint32_t)hdr[CG_TS_OFF] << 24 | (uint32_t)hdr[CG_TS_OFF + 1] << 16 | (uint32_t)hdr[CG_TS_OFF + 2] << 8 |
	       hdr[CG_TS_OFF + 3];
}

/* The link's own sequence of a packet about to go on it (subflow.h): out
 * of the MAC, set at send time like the link. */
static inline void cg_hdr_set_sseq(uint8_t *hdr, uint32_t sseq)
{
	hdr[CG_SSEQ_OFF] = (uint8_t)(sseq >> 24);
	hdr[CG_SSEQ_OFF + 1] = (uint8_t)(sseq >> 16);
	hdr[CG_SSEQ_OFF + 2] = (uint8_t)(sseq >> 8);
	hdr[CG_SSEQ_OFF + 3] = (uint8_t)sseq;
}

void cg_probe_info_write(uint8_t out[CG_PROBE_INFO_LEN], const struct cg_probe_info *pi);
void cg_probe_info_read(struct cg_probe_info *pi, const uint8_t in[CG_PROBE_INFO_LEN]);
/* The bonding tail, written at payload + CG_PROBE_INFO_LEN (a weight above
 * CG_BOND_WEIGHT_MAX is written as the maximum). */
void cg_probe_bond_write(uint8_t *payload, const struct cg_probe_bond *b);
/* The tail of a probe or reply payload of plen bytes (plen >= CG_PROBE_INFO_LEN,
 * as cg_hdr_parse checks): 1 with *b read, the reserved bits ignored and a
 * weight above CG_BOND_WEIGHT_MAX read as the maximum; 0 with *b zeroed when
 * the payload has none (a peer without it, plen < CG_PROBE_BOND_LEN). */
int cg_probe_bond_read(struct cg_probe_bond *b, const uint8_t *payload, size_t plen);
/* The active mask, at payload + CG_PROBE_BOND_LEN (after the tail). */
void cg_probe_act_write(uint8_t *payload, const struct cg_probe_act *a);
/* The active mask of a probe or reply payload of plen bytes: 1 with *a
 * read; 0 with *a zeroed when it has none (plen < CG_PROBE_ACT_LEN). */
int cg_probe_act_read(struct cg_probe_act *a, const uint8_t *payload, size_t plen);

/* A NACK payload (at most CG_NACK_LEN bytes into out) for seq[0..n) (n >=
 * 1), as ranges in their order: a sequence within 64 after a range's base
 * goes in its mask, any other starts a range. *used: how many it took (all
 * of them, or as many as CG_NACK_RANGES ranges hold). Returns its length. */
size_t cg_nack_write(uint8_t *out, const uint32_t *seq, unsigned n, unsigned *used);
/* The sequences of a NACK payload of plen bytes (cg_hdr_parse checked it)
 * into seq, at most max; returns how many. */
unsigned cg_nack_read(uint32_t *seq, unsigned max, const uint8_t *payload, size_t plen);
void cg_hello_write(uint8_t out[CG_HELLO_LEN], const struct cg_hello *hl);
void cg_hello_read(struct cg_hello *hl, const uint8_t in[CG_HELLO_LEN]);

/* Size of the IP packet that carries a payload of len bytes (an IP packet
 * from the TUN) over family AF_INET or AF_INET6: IP header, UDP header and
 * ours. The largest tun_mtu over a path MTU is mtu - cg_outer_len(family, 0):
 * 1444 over IPv4 and 1424 over IPv6 on a 1500-byte path (1448 and 1428
 * before protocol 12's subflow sequence). */
static inline uint32_t cg_outer_len(int family, uint32_t len)
{
	return len + CG_HDR_LEN + 8 + (family == AF_INET6 ? 40 : 20);
}

#endif
