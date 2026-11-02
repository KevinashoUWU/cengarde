/* What a flow is, learned from the packets that say it (story 031, step
 * 3e): one packet alone cannot tell QUIC's short headers, SRT's data or an
 * RTP stream whose receiver asks for what it lost from anything else, but a
 * flow says it once in a while, and a small table keeps it.
 *
 *   QUIC   a long header (the first two bits set, then a version: 1, 2, a
 *          draft or 0 for a version negotiation): its connection follows
 *   SRT    a handshake (a control packet of type 0, UDT version 4 or 5)
 *   RIST, WebRTC   an RTCP transport feedback NACK (PT 205, FMT 1), which
 *          travels the other way from the media: learned from whichever way
 *          it is seen (each end feeds both what it receives and what it
 *          sends); with RTCP on the next port (RIST's), its RTP flow (both
 *          ports one less) too
 *
 * Each of them recovers what it loses and orders what it gets, its own way:
 * the reorderer should neither hold them nor rescue them (flowrule.h's
 * direct), when it is not split: the links' reordering is the tunnel's
 * own, and unheld QUIC lost half to 85 % of its rate to it in the lab's
 * bonding; in redundancy it trades the tunnel's rescue (rate on lossy
 * links, -30 %) for latency (p99 228 -> 68 ms on cell4), so flow_learn is
 * the operator's (off by default). A flow is both ways of a pair of endpoints (the key is the same
 * either way); it is forgotten CG_FL_TTL_MS after its last packet (a QUIC
 * connection says it only in its handshake: every packet of it after keeps
 * it remembered). A rule (flow_class) still has the last word.
 *
 * Pure: a table the caller owns, no clock of its own, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FLOWLEARN_H
#define CG_FLOWLEARN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ippkt.h"

#define CG_FL_SLOTS 512 /* flows remembered at once (a power of two) */
#define CG_FL_PROBE 8   /* slots looked at for a key */
#define CG_FL_TTL_MS 120000

enum { CG_FL_NONE = 0, CG_FL_QUIC, CG_FL_SRT, CG_FL_NACK, CG_FL_KINDS };

struct cg_fl {
	uint64_t key[CG_FL_SLOTS];
	uint64_t seen_ms[CG_FL_SLOTS];
	uint8_t kind[CG_FL_SLOTS];
	uint64_t learned[CG_FL_KINDS]; /* flows learned, by kind (the status) */
};

_Static_assert(!(CG_FL_SLOTS & (CG_FL_SLOTS - 1)), "a power of two");

/* The UDP endpoints of IP packet b (len bytes): addresses (16 bytes each,
 * IPv4 in the first 4), ports and the payload's offset; 0 when b is not an
 * unfragmented UDP datagram (IPv6 with no extension header). */
static inline int cg_fl_udp(const uint8_t *b, size_t len, uint8_t sa[16], uint8_t da[16], uint16_t *sp, uint16_t *dp,
			    size_t *off)
{
	int ver = cg_ip_check(b, len);
	size_t o;

	memset(sa, 0, 16);
	memset(da, 0, 16);
	if (ver == 4) {
		if (b[9] != 17 || (b[6] & 0x3f) || b[7])
			return 0;
		o = (size_t)(b[0] & 15) * 4;
		memcpy(sa, b + 12, 4);
		memcpy(da, b + 16, 4);
	} else if (ver == 6) {
		if (b[6] != 17)
			return 0;
		o = CG_IP6_HDR;
		memcpy(sa, b + 8, 16);
		memcpy(da, b + 24, 16);
	} else {
		return 0;
	}
	if (o + 8 > len)
		return 0;
	*sp = (uint16_t)(b[o] << 8 | b[o + 1]);
	*dp = (uint16_t)(b[o + 2] << 8 | b[o + 3]);
	*off = o + 8;
	return 1;
}

/* A flow's key, the same both ways: FNV-1a over the lesser endpoint, then
 * the greater (never 0, the free slot's). */
static inline uint64_t cg_fl_key(const uint8_t sa[16], uint16_t sp, const uint8_t da[16], uint16_t dp)
{
	const uint8_t *lo = sa, *hi = da;
	uint16_t lp = sp, hp = dp;
	uint64_t h = 1469598103934665603ull;
	int c = memcmp(sa, da, 16);

	if (c > 0 || (!c && sp > dp)) {
		lo = da, hi = sa;
		lp = dp, hp = sp;
	}
	for (int i = 0; i < 16; i++)
		h = (h ^ lo[i]) * 1099511628211ull;
	h = (h ^ (lp >> 8)) * 1099511628211ull;
	h = (h ^ (lp & 0xff)) * 1099511628211ull;
	for (int i = 0; i < 16; i++)
		h = (h ^ hi[i]) * 1099511628211ull;
	h = (h ^ (hp >> 8)) * 1099511628211ull;
	h = (h ^ (hp & 0xff)) * 1099511628211ull;
	return h ? h : 1;
}

/* What the payload (n bytes at p) says of its flow: CG_FL_QUIC, _SRT or
 * _NACK, else CG_FL_NONE. */
static inline int cg_fl_says(const uint8_t *p, size_t n)
{
	if (n >= 5 && (p[0] & 0xc0) == 0xc0) { /* QUIC's long header */
		uint32_t v = (uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4];

		if (v == 1 || v == 0x6b3343cfu || v == 0 || (v & 0xffffff00u) == 0xff000000u)
			return CG_FL_QUIC;
	}
	/* SRT's handshake: control (the top bit), type 0, its CIF's UDT version 4 or 5 */
	if (n >= 64 && p[0] == 0x80 && p[1] == 0x00 && p[16] == 0 && p[17] == 0 && p[18] == 0 && (p[19] == 4 || p[19] == 5))
		return CG_FL_SRT;
	/* RTCP transport feedback, a generic NACK (RFC 4585): V=2, FMT 1, PT 205 */
	if (n >= 16 && (p[0] & 0xc0) == 0x80 && (p[0] & 0x1f) == 1 && p[1] == 205)
		return CG_FL_NACK;
	return CG_FL_NONE;
}

/* The slot of key k (or, for a new one, where it may go: a free one, else
 * the oldest of the probe); -1 never. */
static inline unsigned cg_fl_slot(const struct cg_fl *t, uint64_t k, int *found)
{
	unsigned i0 = (unsigned)(k ^ (k >> 32)) & (CG_FL_SLOTS - 1), best = i0;

	*found = 0;
	for (unsigned j = 0; j < CG_FL_PROBE; j++) {
		unsigned i = (i0 + j) & (CG_FL_SLOTS - 1);

		if (t->key[i] == k) {
			*found = 1;
			return i;
		}
		if (!t->key[i] || t->seen_ms[i] < t->seen_ms[best])
			best = i;
		if (!t->key[i])
			break;
	}
	return best;
}

static inline void cg_fl_put(struct cg_fl *t, uint64_t k, int kind, uint64_t now_ms)
{
	int found;
	unsigned i = cg_fl_slot(t, k, &found);

	if (!found || t->kind[i] != kind)
		t->learned[kind]++;
	t->key[i] = k;
	t->kind[i] = (uint8_t)kind;
	t->seen_ms[i] = now_ms ? now_ms : 1;
}

/* IP packet b (len bytes), seen either way at now_ms: what it says of its
 * flow goes into t. Returns the kind it said, CG_FL_NONE for most. */
static inline int cg_fl_observe(struct cg_fl *t, const uint8_t *b, size_t len, uint64_t now_ms)
{
	uint8_t sa[16], da[16];
	uint16_t sp, dp;
	size_t off;
	int kind;

	if (!cg_fl_udp(b, len, sa, da, &sp, &dp, &off) || !(kind = cg_fl_says(b + off, len - off)))
		return CG_FL_NONE;
	cg_fl_put(t, cg_fl_key(sa, sp, da, dp), kind, now_ms);
	/* RTCP on the next port (RIST's): its RTP flow, both ports one less */
	if (kind == CG_FL_NACK && (sp & 1) && (dp & 1))
		cg_fl_put(t, cg_fl_key(sa, (uint16_t)(sp - 1), da, (uint16_t)(dp - 1)), kind, now_ms);
	return kind;
}

/* What t knows of the flow of IP packet b (len bytes) at now_ms: its kind
 * (and the flow is remembered from now), or CG_FL_NONE (none learned, or
 * forgotten: nothing of it for CG_FL_TTL_MS). */
static inline int cg_fl_kind(struct cg_fl *t, const uint8_t *b, size_t len, uint64_t now_ms)
{
	uint8_t sa[16], da[16];
	uint16_t sp, dp;
	size_t off;
	int found;
	unsigned i;

	if (!cg_fl_udp(b, len, sa, da, &sp, &dp, &off))
		return CG_FL_NONE;
	i = cg_fl_slot(t, cg_fl_key(sa, sp, da, dp), &found);
	if (!found || now_ms - t->seen_ms[i] > CG_FL_TTL_MS)
		return CG_FL_NONE;
	t->seen_ms[i] = now_ms ? now_ms : 1;
	return t->kind[i];
}

#endif
