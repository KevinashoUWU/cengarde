/* Flows of the native tunnel: a hook for per-flow reordering, not used yet.
 *
 * With tunnel = native cengarde sees the IP packets it carries, so the
 * reorderer could hold back only the packets of a flow that has a gap
 * instead of everything behind it, and release a TCP flow's packets
 * knowing that TCP copes with a little reordering by itself. That is a
 * later phase; this header gives it the flow of a packet: a 32-bit hash of
 * its 5-tuple (addresses, protocol, ports) and whether it is TCP.
 *
 * IPv6 extension headers (hop-by-hop, routing, destination options,
 * fragment) are walked, at most CG_FLOW_EXT_MAX of them. Every fragment of
 * a fragmented packet hashes without ports, so the fragments of one packet
 * share a flow (only the first carries the ports); ICMP and anything else
 * without ports hashes on addresses and protocol.
 *
 * Pure functions, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FLOW_H
#define CG_FLOW_H

#include <stddef.h>
#include <stdint.h>

#include "ippkt.h"

#define CG_FLOW_EXT_MAX 4
#define CG_IPPROTO_TCP 6
#define CG_IPPROTO_UDP 17

struct cg_flow {
	uint32_t hash;  /* of the 5-tuple, with seed */
	uint8_t proto;  /* the transport protocol (the last next header for IPv6) */
	uint8_t tcp;    /* proto is TCP and its ports were read */
	uint8_t frag;   /* a fragment: hashed without ports */
	uint16_t sport, dport; /* host order; 0 when not read */
};

/* A 32-bit mix of one more word into h (murmur3's). */
static inline uint32_t cg_flow_mix(uint32_t h, uint32_t k)
{
	k *= 0xcc9e2d51u;
	k = k << 15 | k >> 17;
	k *= 0x1b873593u;
	h ^= k;
	h = h << 13 | h >> 19;
	return h * 5 + 0xe6546b64u;
}

static inline uint32_t cg_flow_fin(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	return h ^ (h >> 16);
}

/* The flow of IP packet b (len bytes) into f. Returns 4 or 6, or 0 when b
 * is not a packet cg_ip_check takes (f untouched). seed varies the hash
 * (a per-process random value, so nobody can aim at one bucket). */
static inline int cg_flow_of(const uint8_t *b, size_t len, uint32_t seed, struct cg_flow *f)
{
	int ver = cg_ip_check(b, len);
	size_t off, alen;
	uint32_t h = seed;
	const uint8_t *src, *dst;

	if (!ver)
		return 0;
	f->tcp = f->frag = 0;
	f->sport = f->dport = 0;
	src = cg_ip_src(b, ver);
	dst = cg_ip_dst(b, ver);
	alen = ver == 4 ? 4 : 16;
	if (ver == 4) {
		f->proto = b[9];
		off = (size_t)(b[0] & 15) * 4;
		/* MF set or an offset: a fragment */
		f->frag = (b[6] & 0x3f) || b[7];
	} else {
		uint8_t nh = b[6];

		off = CG_IP6_HDR;
		for (int i = 0; i < CG_FLOW_EXT_MAX; i++) {
			if (nh == 44) { /* fragment: 8 bytes */
				if (off + 8 > len)
					break;
				f->frag = 1;
				nh = b[off];
				off += 8;
			} else if (nh == 0 || nh == 43 || nh == 60) {
				size_t el;

				if (off + 8 > len)
					break;
				el = ((size_t)b[off + 1] + 1) * 8;
				nh = b[off];
				off += el;
			} else {
				break;
			}
		}
		f->proto = nh;
	}
	for (size_t i = 0; i < alen; i += 4) {
		h = cg_flow_mix(h, (uint32_t)src[i] << 24 | (uint32_t)src[i + 1] << 16 | (uint32_t)src[i + 2] << 8 | src[i + 3]);
		h = cg_flow_mix(h, (uint32_t)dst[i] << 24 | (uint32_t)dst[i + 1] << 16 | (uint32_t)dst[i + 2] << 8 | dst[i + 3]);
	}
	if (!f->frag && (f->proto == CG_IPPROTO_TCP || f->proto == CG_IPPROTO_UDP) && off + 4 <= len) {
		f->sport = (uint16_t)(b[off] << 8 | b[off + 1]);
		f->dport = (uint16_t)(b[off + 2] << 8 | b[off + 3]);
		f->tcp = f->proto == CG_IPPROTO_TCP;
	}
	h = cg_flow_mix(h, (uint32_t)f->proto << 16 | (uint32_t)ver);
	h = cg_flow_mix(h, (uint32_t)f->sport << 16 | f->dport);
	f->hash = cg_flow_fin(h);
	return ver;
}

#endif
