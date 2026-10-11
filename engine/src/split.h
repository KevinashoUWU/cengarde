/* Splitting the send direction: sharing the packets out among the links
 * instead of copying each one to all of them (story 012, step 1 of the
 * bonding plan). This is the decision only; the send paths of the client and
 * the server (client.c, server.c) call it once per packet.
 *
 * - Eligible links: the live, active (not muted) links with a weight above
 *   zero whose peer still answers on them (guard G3 below). The caller builds
 *   the mask from its link masks and health (health.h), not from
 *   cg_health_carriers: its fallbacks (a muted link when none is active, every
 *   link when none is live) must end in duplication, which is what a packet
 *   gets here when fewer than 2 links are eligible.
 * - Smooth weighted round-robin (nginx's): each pick adds every eligible
 *   link's weight to its credit, takes the link with the most credit and
 *   charges it the sum of the weights. Weights 5:1:1 give a a b a c a a, not
 *   a a a a a b c, so a link's share is spread out instead of sent in bursts
 *   that fill its queue. The credits of the links that left the set are
 *   zeroed, so a link that comes back starts even.
 * - Duplicated packets (pick -1): WireGuard's handshake messages (types 1 to
 *   3: a lost one costs a retry of seconds), small datagrams (keepalives, TCP
 *   ACKs: their loss costs more than the bytes saved) and everything while
 *   fewer than 2 links are eligible. They go to every carrier, as without a
 *   split.
 * - Weight 0 (libRIST's meaning): the link carries a copy of every packet,
 *   split or not. These links are dup0 and are never picked.
 * - CG_F_SPLIT (proto.h) marks a packet that skips at least one carrier. It
 *   is under the MAC, so it is decided before the header is written.
 * - Whether an end splits at all (cg_split_active) depends on its split
 *   setting (config.h) and on what the peer's newest probe tail says
 *   (cg_probe_bond, proto.h; cg_split_probe_tail and cg_split_reply_tail are
 *   what each end writes in its own):
 *   force always (the lab); a client's on once the server says it reorders,
 *   with its [link] weights; a server's client when the client asks for it
 *   and says it reorders, with the weights the client sends per link; off
 *   never. A peer without the tail says neither, so nothing is split towards
 *   an engine that cannot put it back in order.
 *
 * Without the reorderer at the receiving end, and the steps that follow (loss
 * detection per link, a queue per link), a split stream is slower and less
 * reliable than redundancy: that is why it is experimental. Pure functions,
 * no clock and no I/O (docs/historias/012).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_SPLIT_H
#define CG_SPLIT_H

#include <stdint.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "config.h"  /* enum cg_split_mode */
#include "proto.h"   /* CG_F_SPLIT, struct cg_probe_bond */
#include "wgsize.h"  /* CG_WGS_DATA */

/* Guard G3: a link with no verified packet from the peer for more than this
 * many probe intervals is not eligible, though it still carries the
 * duplicated packets. The stall detection (health.h) takes the same three
 * intervals plus a second of slack; this shortens the time a dead link keeps
 * its share by that second. */
#define CG_SPLIT_STALE_PROBES 3

struct cg_split {
	int32_t cur[CG_MAX_LINKS]; /* the credits of the smooth round-robin */
};

/* Back to an even start, e.g. when the weights change. */
static inline void cg_split_reset(struct cg_split *s)
{
	memset(s, 0, sizeof(*s));
}

/* Where one packet goes. eligible: the links that may take a share (above);
 * dup0: the weight-0 links, which carry every packet (any of them in
 * eligible is ignored); w: the weights (0: none); wgtype and len: the
 * WireGuard message type (its first byte, 0 for an empty datagram) and the
 * datagram's length, 32 bytes of overhead included as in wgsize.h; small_len:
 * datagrams of up to this many bytes are duplicated.
 *
 * Returns the link that carries the packet, to which cg_split_dst adds the
 * weight-0 links, and ORs CG_F_SPLIT into *flags (NULL: not asked), the
 * other bits left as they are. Returns -1, with *flags untouched, when the
 * packet goes to every carrier. */
static inline int cg_split_pick(struct cg_split *s, uint16_t eligible, uint16_t dup0,
				const uint16_t w[CG_MAX_LINKS], uint8_t wgtype, uint32_t len,
				uint32_t small_len, uint8_t *flags)
{
	uint16_t set = 0, keep;
	int32_t total = 0;
	int best = -1;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((eligible >> i & 1) && !(dup0 >> i & 1) && w[i])
			set |= (uint16_t)(1u << i);

	/* Nothing to share out among fewer than 2 links: start even when a
	 * second one comes. */
	keep = set & (set - 1) ? set : 0;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (!(keep >> i & 1))
			s->cur[i] = 0;

	if (wgtype != CG_WGS_DATA || len <= small_len || !keep)
		return -1;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!(keep >> i & 1))
			continue;
		s->cur[i] += w[i];
		total += w[i];
		if (best < 0 || s->cur[i] > s->cur[best])
			best = i;
	}
	if (best < 0)
		return -1;
	s->cur[best] -= total;

	/* At least 2 links share the set, so the other one is skipped. */
	if (flags)
		*flags |= CG_F_SPLIT;
	return best;
}

/* The links a packet is sent on, given what cg_split_pick returned: every
 * carrier for -1, else the picked link and the weight-0 carriers. carriers:
 * the links that carry payload at all (dup0 may name links that do not). */
static inline uint16_t cg_split_dst(int pick, uint16_t carriers, uint16_t dup0)
{
	if (pick < 0)
		return carriers;
	return (uint16_t)(((1u << pick) | dup0) & carriers);
}

/* Whether an end with split setting mode (enum cg_split_mode) splits what it
 * sends, given the peer's newest probe tail: peer_reorders and peer_asks are
 * its CG_BOND_REORDERS and CG_BOND_SPLIT_ME (0 without a tail). */
static inline int cg_split_active(int mode, int peer_reorders, int peer_asks)
{
	switch (mode) {
	case CG_SPLIT_FORCE:
		return 1;
	case CG_SPLIT_ON: /* a client: once the server reorders */
		return !!peer_reorders;
	case CG_SPLIT_CLIENT: /* a server: when the client asks and reorders */
		return peer_reorders && peer_asks;
	default:
		return 0;
	}
}

/* The tail a client writes in its probes on a link (proto.h): weight, the
 * link's share of a split ([link] weight), its own reorder setting, and the
 * request for a split download when its split is on. */
static inline struct cg_probe_bond cg_split_probe_tail(int mode, int reorders, uint32_t weight)
{
	struct cg_probe_bond b = { 0 };

	b.weight = (uint16_t)(weight > CG_BOND_WEIGHT_MAX ? CG_BOND_WEIGHT_MAX : weight);
	b.reorders = !!reorders;
	b.split_me = mode == CG_SPLIT_ON;
	return b;
}

/* The tail a server writes in a reply: its reorder setting only. It never
 * asks for anything: the client's upload split follows the server's
 * reordering alone. */
static inline struct cg_probe_bond cg_split_reply_tail(int reorders)
{
	struct cg_probe_bond b = { 0 };

	b.reorders = !!reorders;
	return b;
}

/* Guard G3: whether a link whose peer was last heard from at last_rx_ms
 * (0: never) is too silent to take a share, now_ms later, with the peer
 * probing every probe_interval_ms. */
static inline int cg_split_stale(uint64_t last_rx_ms, uint64_t now_ms, uint32_t probe_interval_ms)
{
	return !last_rx_ms ||
	       (now_ms > last_rx_ms &&
		now_ms - last_rx_ms > (uint64_t)CG_SPLIT_STALE_PROBES * probe_interval_ms);
}

#endif
