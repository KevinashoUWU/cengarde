/* The protocontroller's messages and tables (story 025, protocol 10): the TCP the
 * router takes from its LAN becomes a stream (stream.h) inside the native
 * tunnel, and the VPS opens a fresh TCP to its destination.
 *
 * A stream's messages are the payload of a tunnel DATA (under its MAC,
 * anti-replay, copies, FEC and rescue, all unchanged) whose first nibble
 * is 0xF: never IPv4 nor IPv6, so the receiver tells them from the TUN's
 * packets by their first byte, and hands them on at once (the reorderer
 * does not hold them: a stream keeps its own order). Big-endian:
 *
 *   0     0xF0 | type     CG_PCTL_OPEN, _DATA, _ACK, _RST
 *   1     flags           CG_PCTLF_*
 *   2..5  stream id       chosen by the router: generation << 16 | slot
 *
 *   OPEN  6: family (4 or 6), 7..8: port, 9..: address (4 or 16 bytes).
 *         Router -> VPS, again on the timer until the VPS's first ACK.
 *   DATA  6..9: the offset's low 32 bits; 10..13: the sender's clock when it
 *         sent it (us, low 32 bits); then the bytes. CG_PCTLF_FIN: the
 *         stream ends after them (the FIN takes one byte of the sequence);
 *         CG_PCTLF_EST: the sender got an ACK of this stream already (a peer
 *         that does not know it answers RST; without it, the OPEN may have
 *         been lost, and it stays quiet).
 *   ACK   6..7: its sequence; 8..11: cumulative offset; 12..15: the
 *         window's right edge (absolute); 16..19: the least one-way delay
 *         of the DATA that came since the last ACK, the receiver's clock at
 *         arrival less the sender's in the DATA (us, signed 32 bits, the two
 *         clocks' offset included: the sender compares it with its own
 *         minimum, CG_PCTL_NOOWD: none); 20: n (at most CG_ST_ACK_RANGES);
 *         then n ranges of start and end, the newest frame's first.
 *         CG_PCTLF_EST as in DATA; CG_PCTLF_DUP: a duplicate arrived since the
 *         last ACK (a segment sent again for nothing: the sender's RACK
 *         window widens, as RACK's with DSACK).
 *   RST   6: the reason (CG_PCTL_R_*).
 *
 * No tag of its own (protocol 14, story 030): a message travels only in the
 * tunnel's STREAM class, always under its MAC, which no repair code covers
 * (until 13 each ended in a SipHash tag, because a DATA the tunnel rebuilt
 * from its repairs carried no MAC; rebuilt.h now drops a rebuilt payload
 * that looks like a message).
 *
 * Also here: which destinations the VPS refuses to connect to, the
 * server's table of streams by (session, id), and the slots' generations.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PCTL_H
#define CG_PCTL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stream.h"

#define CG_PCTL_MAGIC 0xF0
enum { CG_PCTL_OPEN = 1, CG_PCTL_DATA = 2, CG_PCTL_ACK = 3, CG_PCTL_RST = 4 };
#define CG_PCTLF_FIN 0x01
#define CG_PCTLF_EST 0x02
#define CG_PCTLF_DUP 0x04 /* ACK: a duplicate came since the last ACK (the sender widens its RACK window) */
#define CG_PCTL_HDR 6
#define CG_PCTL_DATA_HDR 14
#define CG_PCTL_OPEN_MAX (CG_PCTL_HDR + 3 + 16)
#define CG_PCTL_ACK_MAX (CG_PCTL_HDR + 15 + 8 * CG_ST_ACK_RANGES)
#define CG_PCTL_NOOWD INT32_MIN /* an ACK with no one-way delay */
#define CG_PCTL_RST_LEN (CG_PCTL_HDR + 1)
#define CG_PCTL_OVERHEAD CG_PCTL_DATA_HDR /* a DATA's bytes besides the stream's */

/* Why a stream was reset. */
enum {
	CG_PCTL_R_NONE = 0,
	CG_PCTL_R_PEER,        /* the TCP peer reset its connection */
	CG_PCTL_R_REFUSED,     /* the destination refused the connection */
	CG_PCTL_R_UNREACHABLE, /* no route to it */
	CG_PCTL_R_TIMEOUT,     /* the connection or the stream timed out (protocontroller_dead_s) */
	CG_PCTL_R_DENIED,      /* a destination the VPS does not connect to */
	CG_PCTL_R_FULL,        /* no room: protocontroller_max_flows or protocontroller_mem_mb */
	CG_PCTL_R_OFF,         /* this end does not take streams (protocontroller = off) */
	CG_PCTL_R_UNKNOWN,     /* no such stream here (the other end restarted) */
	CG_PCTL_R_ERROR,       /* a socket error */
	CG_PCTL_R_N
};

static inline const char *cg_pctl_reason(unsigned r)
{
	static const char *const n[CG_PCTL_R_N] = { "none",    "peer reset", "refused", "unreachable", "timeout",
						   "denied",  "full",       "off",     "unknown",     "error" };

	return r < CG_PCTL_R_N ? n[r] : "?";
}

/* Whether a DATA payload is a stream message (else an IP packet). */
static inline int cg_pctl_is(const uint8_t *p, size_t len)
{
	return len >= 1 && (p[0] & 0xF0) == CG_PCTL_MAGIC;
}

static inline void cg_pctl_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t cg_pctl_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline size_t cg_pctl_hdr(uint8_t *b, unsigned type, uint8_t flags, uint32_t id)
{
	b[0] = (uint8_t)(CG_PCTL_MAGIC | type);
	b[1] = flags;
	cg_pctl_be32(b + 2, id);
	return CG_PCTL_HDR;
}

/* OPEN to family (4 or 6) addr (4 or 16 bytes, network order), port (host
 * order). Returns its length. */
static inline size_t cg_pctl_put_open(uint8_t *b, uint32_t id, int family, const uint8_t *addr, uint16_t port)
{
	size_t al = family == 6 ? 16 : 4;

	cg_pctl_hdr(b, CG_PCTL_OPEN, 0, id);
	b[6] = (uint8_t)family;
	b[7] = (uint8_t)(port >> 8);
	b[8] = (uint8_t)port;
	memcpy(b + 9, addr, al);
	return 9 + al;
}

/* A DATA's header, sent at ts (us), the bytes after it (CG_PCTL_DATA_HDR on). */
static inline size_t cg_pctl_put_data(uint8_t *b, uint32_t id, uint8_t flags, uint64_t off, uint32_t ts)
{
	cg_pctl_hdr(b, CG_PCTL_DATA, flags, id);
	cg_pctl_be32(b + 6, (uint32_t)off);
	cg_pctl_be32(b + 10, ts);
	return CG_PCTL_DATA_HDR;
}

static inline size_t cg_pctl_put_ack(uint8_t *b, uint32_t id, uint8_t flags, const struct cg_st_ack *a, int32_t owd)
{
	size_t n = a->n > CG_ST_ACK_RANGES ? CG_ST_ACK_RANGES : a->n;

	cg_pctl_hdr(b, CG_PCTL_ACK, flags, id);
	b[6] = (uint8_t)(a->aseq >> 8);
	b[7] = (uint8_t)a->aseq;
	cg_pctl_be32(b + 8, (uint32_t)a->cum);
	cg_pctl_be32(b + 12, (uint32_t)a->edge);
	cg_pctl_be32(b + 16, (uint32_t)owd);
	b[20] = (uint8_t)n;
	for (size_t i = 0; i < n; i++) {
		cg_pctl_be32(b + 21 + 8 * i, (uint32_t)a->s[i]);
		cg_pctl_be32(b + 25 + 8 * i, (uint32_t)a->e[i]);
	}
	return 21 + 8 * n;
}

static inline size_t cg_pctl_put_rst(uint8_t *b, uint32_t id, uint8_t reason)
{
	cg_pctl_hdr(b, CG_PCTL_RST, 0, id);
	b[6] = reason;
	return CG_PCTL_RST_LEN;
}


/* A message, parsed. Offsets are the wire's 32 bits (cg_st_unwrap). */
struct cg_pctl_msg {
	uint8_t type, flags;
	uint32_t id;
	/* OPEN */
	uint8_t family;
	uint16_t port;
	uint8_t addr[16];
	/* DATA */
	uint32_t off, ts;
	const uint8_t *data;
	uint32_t len;
	/* ACK */
	uint16_t aseq;
	uint32_t cum, edge;
	int32_t owd;
	uint8_t n;
	uint32_t s[CG_ST_ACK_RANGES], e[CG_ST_ACK_RANGES];
	/* RST */
	uint8_t reason;
};

/* Returns 0, or -1 for a malformed message. */
static inline int cg_pctl_parse(const uint8_t *p, size_t len, struct cg_pctl_msg *m)
{
	if (len < CG_PCTL_HDR || !cg_pctl_is(p, len))
		return -1;
	m->type = p[0] & 0x0F;
	m->flags = p[1];
	m->id = cg_pctl_rd32(p + 2);
	switch (m->type) {
	case CG_PCTL_OPEN:
		if (len < 9)
			return -1;
		m->family = p[6];
		m->port = (uint16_t)(p[7] << 8 | p[8]);
		if (m->family == 4 && len >= 13)
			memcpy(m->addr, p + 9, 4);
		else if (m->family == 6 && len >= 25)
			memcpy(m->addr, p + 9, 16);
		else
			return -1;
		return 0;
	case CG_PCTL_DATA:
		if (len < CG_PCTL_DATA_HDR || len - CG_PCTL_DATA_HDR > 0xffff)
			return -1;
		m->off = cg_pctl_rd32(p + 6);
		m->ts = cg_pctl_rd32(p + 10);
		m->data = p + CG_PCTL_DATA_HDR;
		m->len = (uint32_t)(len - CG_PCTL_DATA_HDR);
		return 0;
	case CG_PCTL_ACK:
		if (len < 21)
			return -1;
		m->aseq = (uint16_t)(p[6] << 8 | p[7]);
		m->cum = cg_pctl_rd32(p + 8);
		m->edge = cg_pctl_rd32(p + 12);
		m->owd = (int32_t)cg_pctl_rd32(p + 16);
		m->n = p[20];
		if (m->n > CG_ST_ACK_RANGES || len < 21 + 8 * (size_t)m->n)
			return -1;
		for (unsigned i = 0; i < m->n; i++) {
			m->s[i] = cg_pctl_rd32(p + 21 + 8 * i);
			m->e[i] = cg_pctl_rd32(p + 25 + 8 * i);
		}
		return 0;
	case CG_PCTL_RST:
		if (len < CG_PCTL_RST_LEN)
			return -1;
		m->reason = p[6];
		return 0;
	default:
		return -1;
	}
}

/* What the rescue does with a packet it kept, named by a NACK (story 025:
 * the stream retransmits what it loses, so the tunnel does not send its
 * messages again):
 *   CG_PCTL_NACK_NOT    not a stream message (an IP packet): the rescue
 *                       sends it again, as always;
 *   CG_PCTL_NACK_DATA   a DATA: its loss goes to its stream (id, off and
 *                       ts set: cg_st_tx_nack), not the packet again;
 *   CG_PCTL_NACK_ACK    an ACK: a fresh one says more than the old (every
 *                       ACK carries the whole state), not the packet again;
 *   CG_PCTL_NACK_RESEND an OPEN or a RST (rare, idempotent), or one that
 *                       does not parse: sent again, as before.
 * The packet is the sender's own copy. */
enum { CG_PCTL_NACK_NOT = 0, CG_PCTL_NACK_DATA, CG_PCTL_NACK_ACK, CG_PCTL_NACK_RESEND };

static inline int cg_pctl_nack_kind(const uint8_t *p, size_t len, uint32_t *id, uint32_t *off, uint32_t *ts)
{
	if (!cg_pctl_is(p, len))
		return CG_PCTL_NACK_NOT;
	if (len < CG_PCTL_HDR)
		return CG_PCTL_NACK_RESEND;
	*id = cg_pctl_rd32(p + 2);
	switch (p[0] & 0x0F) {
	case CG_PCTL_DATA:
		if (len < CG_PCTL_DATA_HDR)
			return CG_PCTL_NACK_RESEND;
		*off = cg_pctl_rd32(p + 6);
		*ts = cg_pctl_rd32(p + 10);
		return CG_PCTL_NACK_DATA;
	case CG_PCTL_ACK:
		return CG_PCTL_NACK_ACK;
	default:
		return CG_PCTL_NACK_RESEND;
	}
}

/* An ACK message's offsets, unwrapped against what the sender knows: una
 * (cumulative and ranges) and the edge it had. */
static inline void cg_pctl_ack_of(const struct cg_pctl_msg *m, uint64_t una, uint64_t edge, struct cg_st_ack *a)
{
	a->aseq = m->aseq;
	a->cum = cg_st_unwrap(una, m->cum);
	a->edge = cg_st_unwrap(edge, m->edge);
	a->n = m->n;
	for (unsigned i = 0; i < m->n; i++) {
		a->s[i] = cg_st_unwrap(una, m->s[i]);
		a->e[i] = cg_st_unwrap(una, m->e[i]);
	}
}

/* ---- destinations ---- */

/* Whether the VPS may connect to family (4 or 6) addr: not loopback,
 * unspecified, multicast, broadcast or link-local (a cloud's metadata
 * service lives at 169.254.169.254), nor an IPv4-mapped one of those.
 * Returns 0 or CG_PCTL_R_DENIED. */
static inline int cg_pctl_dest_ok(int family, const uint8_t *a)
{
	static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

	if (family == 6) {
		int zero = a[15] <= 1;

		for (int i = 0; i < 15; i++)
			zero &= a[i] == 0;
		if (zero) /* :: or ::1 */
			return CG_PCTL_R_DENIED;
		if (a[0] == 0xff || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80))
			return CG_PCTL_R_DENIED;
		if (!memcmp(a, mapped, 12))
			return cg_pctl_dest_ok(4, a + 12);
		return 0;
	}
	if (a[0] == 0 || a[0] == 127 || a[0] >= 224 || (a[0] == 169 && a[1] == 254))
		return CG_PCTL_R_DENIED;
	return 0;
}

/* Whether a (family 4 or 6) is in the prefix pfx/len: variant A's VPS
 * takes its middle connections only from the tunnel's addresses. */
static inline int cg_pctl_in_pfx(int family, const uint8_t *a, const uint8_t *pfx, unsigned len)
{
	unsigned max = family == 6 ? 128 : 32, i;

	if (len > max)
		len = max;
	for (i = 0; i + 8 <= len; i += 8)
		if (a[i / 8] != pfx[i / 8])
			return 0;
	if (i < len) {
		uint8_t m = (uint8_t)(0xFF00u >> (len - i));

		if ((a[i / 8] & m) != (pfx[i / 8] & m))
			return 0;
	}
	return 1;
}

/* ---- the router's slots ---- */

/* The id of slot s at generation g (both 16 bits; generation 0 is never
 * used, so an id is never 0). */
static inline uint32_t cg_pctl_id(uint16_t gen, uint16_t slot)
{
	return (uint32_t)gen << 16 | slot;
}

static inline uint16_t cg_pctl_next_gen(uint16_t gen)
{
	uint16_t n = (uint16_t)(gen + 1);

	return n ? n : 1;
}

/* ---- the server's table: (session id, stream id) -> slot ---- */

#define CG_PCTL_TAB_EMPTY UINT32_MAX
#define CG_PCTL_TAB_GONE (UINT32_MAX - 1)

struct cg_pctl_tab {
	uint64_t *key;
	uint32_t *val; /* a slot, CG_PCTL_TAB_EMPTY or CG_PCTL_TAB_GONE */
	uint32_t mask; /* size - 1, a power of two at least twice the slots */
};

static inline uint64_t cg_pctl_key(uint32_t sid, uint32_t id)
{
	return (uint64_t)sid << 32 | id;
}

static inline uint32_t cg_pctl_tab_hash(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdull;
	k ^= k >> 33;
	return (uint32_t)k;
}

static inline void cg_pctl_tab_init(struct cg_pctl_tab *t, uint64_t *key, uint32_t *val, uint32_t size)
{
	t->key = key;
	t->val = val;
	t->mask = size - 1;
	for (uint32_t i = 0; i < size; i++)
		val[i] = CG_PCTL_TAB_EMPTY;
}

/* The slot of k, or CG_PCTL_TAB_EMPTY. */
static inline uint32_t cg_pctl_tab_find(const struct cg_pctl_tab *t, uint64_t k)
{
	for (uint32_t i = cg_pctl_tab_hash(k) & t->mask, n = 0; n <= t->mask; i = (i + 1) & t->mask, n++) {
		if (t->val[i] == CG_PCTL_TAB_EMPTY)
			return CG_PCTL_TAB_EMPTY;
		if (t->val[i] != CG_PCTL_TAB_GONE && t->key[i] == k)
			return t->val[i];
	}
	return CG_PCTL_TAB_EMPTY;
}

/* Puts k -> v (k not there). Returns 0, or -1 when full (never, sized
 * twice the slots). */
static inline int cg_pctl_tab_put(struct cg_pctl_tab *t, uint64_t k, uint32_t v)
{
	for (uint32_t i = cg_pctl_tab_hash(k) & t->mask, n = 0; n <= t->mask; i = (i + 1) & t->mask, n++)
		if (t->val[i] == CG_PCTL_TAB_EMPTY || t->val[i] == CG_PCTL_TAB_GONE) {
			t->key[i] = k;
			t->val[i] = v;
			return 0;
		}
	return -1;
}

static inline void cg_pctl_tab_del(struct cg_pctl_tab *t, uint64_t k)
{
	for (uint32_t i = cg_pctl_tab_hash(k) & t->mask, n = 0; n <= t->mask; i = (i + 1) & t->mask, n++) {
		if (t->val[i] == CG_PCTL_TAB_EMPTY)
			return;
		if (t->val[i] != CG_PCTL_TAB_GONE && t->key[i] == k) {
			/* the next one empty: this one too, and the run of gone before it */
			if (t->val[(i + 1) & t->mask] == CG_PCTL_TAB_EMPTY) {
				t->val[i] = CG_PCTL_TAB_EMPTY;
				for (uint32_t j = (i - 1) & t->mask; t->val[j] == CG_PCTL_TAB_GONE; j = (j - 1) & t->mask)
					t->val[j] = CG_PCTL_TAB_EMPTY;
			} else {
				t->val[i] = CG_PCTL_TAB_GONE;
			}
			return;
		}
	}
}

/* ---- the receiver's window ---- */

/* The right edge a receiver advertises: consumed (written to its socket)
 * plus its buffer cap, or plus backlog_max alone while what it holds in
 * order and not written passes backlog_max (the destination does not take
 * it: the sender stops, the router stops reading its LAN). It may shrink:
 * the sender stops sending new bytes past it, and the receiver still takes
 * whatever comes inside consumed + cap. */
static inline uint64_t cg_pctl_edge(uint64_t consumed, uint64_t nxt, uint64_t cap, uint64_t backlog_max)
{
	uint64_t held = nxt > consumed ? nxt - consumed : 0;

	if (backlog_max < cap && held > backlog_max)
		return consumed + backlog_max;
	return consumed + cap;
}

/* The bytes a stream's sending side may hold unsent (read from its socket
 * and not sent yet): rate (its share, bytes/s) for backlog_ms, at least
 * min_bytes. The router stops reading its LAN socket beyond it. */
static inline uint64_t cg_pctl_backlog(uint64_t rate, uint32_t backlog_ms, uint64_t min_bytes)
{
	uint64_t b = rate * backlog_ms / 1000;

	return b > min_bytes ? b : min_bytes;
}

#endif
