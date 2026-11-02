/* The operator's word on what a flow is (flow_class, story 031, step 3a):
 * rules that put a flow in a class whatever dgclass.h would guess from its
 * bytes. A rule is an endpoint and a class:
 *
 *   ADDR[/LEN][:PORT] CLASS     192.168.50.158:9330 ordered
 *   [ADDR6][/LEN][:PORT] CLASS  [2001:db8::7]:5000 direct
 *   :PORT CLASS                 :5004 deadline (any address)
 *
 * It matches a packet whose source or destination is that endpoint (the
 * address within the prefix, LEN 32 or 128 when left out; the port of UDP or
 * TCP, any when left out), so one rule covers a flow both ways; the first
 * rule that matches wins. The classes, for the receiver's reorderer:
 *
 *   ordered   held in order and waits for its rescue (MPEG-TS's rule: a
 *             receiver with no reorder buffer of its own)
 *   deadline  held up to its deadline, asked for, never waited for longer
 *             (RTP's rule: a jitter buffer orders what comes late)
 *   direct    never held nor rescued (QUIC, SRT, WebRTC, a game: their own
 *             ARQ and order)
 *   auto      what dgclass.h guesses (a rule that keeps a flow out of a
 *             broader one after it)
 *   reliable  the reliable lane (step 3c): the router takes these datagrams
 *             off the tunnel (TPROXY) into a datagram stream of the
 *             protocontroller (B), which delivers every one, in order, late
 *             rather than never; for telemetry, not for real time. The
 *             reorderer never sees them: for it, the guess
 *
 * Pure: parsing and a few comparisons, no state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FLOWRULE_H
#define CG_FLOWRULE_H

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ippkt.h"

enum { CG_FC_NONE = 0, CG_FC_AUTO, CG_FC_ORDERED, CG_FC_DEADLINE, CG_FC_DIRECT, CG_FC_RELIABLE };

#define CG_FC_MAX 32 /* rules, at most */

struct cg_fc_rule {
	uint8_t fam; /* 4 or 6; 0: any address */
	uint8_t len; /* the prefix's bits */
	uint16_t port; /* 0: any */
	uint8_t cls;
	uint8_t a[16];
};

struct cg_fc {
	unsigned n;
	struct cg_fc_rule r[CG_FC_MAX];
};

static inline int cg_fc_class_of(const char *s)
{
	static const char *const names[] = { "auto", "ordered", "deadline", "direct", "reliable" };

	for (int i = 0; i < 5; i++)
		if (!strcmp(s, names[i]))
			return CG_FC_AUTO + i;
	return CG_FC_NONE;
}

/* A rule from its text (above) into *r: 0, or -1 when malformed. */
static inline int cg_fc_parse(struct cg_fc_rule *r, const char *text)
{
	char ep[96], cls[16], *p, *q, *port = NULL, *slash;
	long v;

	memset(r, 0, sizeof(*r));
	if (sscanf(text, " %95s %15s", ep, cls) != 2 || !(r->cls = (uint8_t)cg_fc_class_of(cls)))
		return -1;
	p = ep;
	if (*p == '[') { /* [IPv6] */
		if (!(q = strchr(p, ']')))
			return -1;
		*q++ = 0;
		p++;
		if (*q == '/') {
			slash = q;
			q = strchr(q, ':');
			if (q)
				*q++ = 0, port = q;
			v = strtol(slash + 1, &q, 10);
			if (*q || v < 0 || v > 128)
				return -1;
			r->len = (uint8_t)v;
		} else if (*q == ':') {
			port = q + 1;
			r->len = 128;
		} else if (*q) {
			return -1;
		} else {
			r->len = 128;
		}
		if (inet_pton(AF_INET6, p, r->a) != 1)
			return -1;
		r->fam = 6;
	} else if (*p == ':') { /* any address */
		port = p + 1;
	} else {
		if ((q = strchr(p, ':'))) /* IPv4 only: an IPv6 address goes in brackets */
			*q = 0, port = q + 1;
		r->len = 32;
		if ((slash = strchr(p, '/'))) {
			*slash = 0;
			v = strtol(slash + 1, &q, 10);
			if (*q || v < 0 || v > 32)
				return -1;
			r->len = (uint8_t)v;
		}
		if (inet_pton(AF_INET, p, r->a) != 1)
			return -1;
		r->fam = 4;
	}
	if (port) {
		v = strtol(port, &q, 10);
		if (*q || v < 1 || v > 65535)
			return -1;
		r->port = (uint16_t)v;
	}
	return r->fam || r->port ? 0 : -1;
}

/* Whether address x (n bytes) is within rule r's prefix. */
static inline int cg_fc_in(const struct cg_fc_rule *r, const uint8_t *x)
{
	unsigned full = r->len / 8, rest = r->len % 8;

	if (memcmp(r->a, x, full))
		return 0;
	return !rest || !((r->a[full] ^ x[full]) & (uint8_t)(0xff << (8 - rest)));
}

/* Whether rule r matches the endpoint (address x of family fam, port, 0
 * when the packet has none). */
static inline int cg_fc_ep(const struct cg_fc_rule *r, int fam, const uint8_t *x, uint16_t port)
{
	if (r->fam && (r->fam != fam || !cg_fc_in(r, x)))
		return 0;
	return !r->port || r->port == port;
}

/* The class rule table t gives IP packet b (len bytes): the first rule that
 * matches its source or its destination; CG_FC_NONE when none does. */
static inline int cg_fc_match(const struct cg_fc *t, const uint8_t *b, size_t len)
{
	int ver;
	const uint8_t *src, *dst;
	uint16_t sp = 0, dp = 0;
	size_t o;
	uint8_t proto;

	if (!t->n || !(ver = cg_ip_check(b, len)))
		return CG_FC_NONE;
	if (ver == 4) {
		src = b + 12;
		dst = b + 16;
		proto = b[9];
		o = (size_t)(b[0] & 15) * 4;
		if ((b[6] & 0x1f) || b[7]) /* a later fragment: no ports */
			proto = 0;
	} else {
		src = b + 8;
		dst = b + 24;
		proto = b[6];
		o = CG_IP6_HDR;
	}
	if ((proto == 17 || proto == 6) && o + 4 <= len) {
		sp = (uint16_t)(b[o] << 8 | b[o + 1]);
		dp = (uint16_t)(b[o + 2] << 8 | b[o + 3]);
	}
	for (unsigned i = 0; i < t->n; i++)
		if (cg_fc_ep(&t->r[i], ver, src, sp) || cg_fc_ep(&t->r[i], ver, dst, dp))
			return t->r[i].cls;
	return CG_FC_NONE;
}

/* For the reorderer: whether a datagram waits for its rescue (*waits) or is
 * never held (*nohold), from its rule's class fc (CG_FC_NONE or AUTO: the
 * guess, ts: dgclass.h took it for MPEG-TS). */
static inline void cg_fc_hold(int fc, int ts, uint8_t *waits, uint8_t *nohold)
{
	*waits = (uint8_t)(fc == CG_FC_ORDERED || ((fc == CG_FC_NONE || fc == CG_FC_AUTO || fc == CG_FC_RELIABLE) && ts));
	*nohold = (uint8_t)(fc == CG_FC_DIRECT);
}

/* Whether table t has a reliable rule: the router opens the lane. */
static inline int cg_fc_reliable(const struct cg_fc *t)
{
	for (unsigned i = 0; i < t->n; i++)
		if (t->r[i].cls == CG_FC_RELIABLE)
			return 1;
	return 0;
}

#endif
