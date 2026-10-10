/* The router's per-packet decisions, shared by every link_threads mode:
 * today's loop ("legacy"), and the hub that takes what the per-link pumps
 * read ("off" runs the pumps' code inline, "on" in a thread per link).
 * Only the reads, the sends and the handling of sockets differ between them.
 *
 * cg_rx_entry() judges one datagram a link received, from the generation of
 * its socket to the arrival ring, in this order:
 *   1. an entry of a closed link, or of an older socket of it (gen), is
 *      dropped before anything else, so that a reply from a server address
 *      a link left never counts for the one it moved to;
 *   2. truncated, malformed, or of a type the client does not take: dropped;
 *   3. another session: dropped;
 *   4. a HELLO (the server cannot take our probes yet, proto.h) has no
 *      sequence: its MAC verifies and it answers a probe of this link that
 *      no reply or HELLO answered yet (epoch.h), or it is dropped; the
 *      caller keeps its cookie for the link's next probes;
 *   5. the anti-replay window of the packet's class (DATA, or the control
 *      messages), before any MAC: OLD is dropped; DUP is dropped once its
 *      arrival is noted (cg_arr_dup, display only: no MAC was checked);
 *   6. the MAC, paid only by NEW packets;
 *   7. the window is marked, only now;
 *   8. a probe reply goes back to the caller (it updates the link's health);
 *      DATA is the first copy, noted in the arrival ring, for WireGuard.
 * A server that restarted needs no window reset here: it goes on past what
 * the probes say we received (cg_probe_info).
 * cg_up_header() writes the header of one packet that goes up: the next
 * sequence of its class and the MAC, computed once (the link byte is patched
 * per link, outside the MAC).
 *
 * Pure: no clock, no I/O, no log; the caller sends and logs what the verdict
 * says. tests/test_client_rx.c checks the order with a fake link.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CLIENTPATH_H
#define CG_CLIENTPATH_H

#include <stddef.h>
#include <stdint.h>

#include "arrival.h"
#include "epoch.h"
#include "proto.h"
#include "replay.h"

/* A link as the download sees it. */
struct cg_rxl {
	uint8_t gen;           /* of its socket: moves on every open and close */
	uint8_t open;          /* it has a socket */
	uint8_t refused;       /* its newest HELLO said CG_F_REFUSED */
	uint32_t cookie;       /* of its newest HELLO, for its probes (0: none) */
	uint64_t last_rx_ms;   /* newest verified packet on it */
	uint64_t hello_ms;     /* its newest HELLO */
	struct cg_echo probes; /* its probes, for the echoes in replies and HELLOs (epoch.h) */
};

/* The download state of the session (one owner: the loop, or the hub). */
struct cg_rxs {
	uint32_t session;
	const uint8_t *k_rx;
	uint64_t newest_ms;  /* newest verified NEW packet on any link */
	struct cg_replay replay; /* DATA */
	struct cg_replay ctl;    /* probe replies */
	struct cg_arrivals arr;
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t malformed, foreign, auth_fail, old, dups, trunc, stale_gen, hellos, hellos_stale;
};

enum cg_rxv {
	CG_RXV_DROP = 0, /* counted, nothing else to do */
	CG_RXV_DATA,     /* a first copy: to WireGuard */
	CG_RXV_REPLY,    /* a verified probe reply: the caller takes it */
	CG_RXV_HELLO,    /* a verified HELLO answering a probe of the link: its cookie is taken */
	CG_RXV_AUTH,     /* dropped: its MAC failed (the caller warns, rate-limited) */
};

/* The upload side of the session. */
struct cg_txs {
	uint32_t session;
	uint32_t seq;     /* next DATA sequence */
	uint32_t seq_ctl; /* next probe sequence */
	uint8_t hint;     /* cg_client_hint() of the key */
	const uint8_t *k_tx;
};

/* What a probe says about our windows, for a server that lost the session
 * (cg_probe_info): the newest sequences received, 0 when none. */
static inline uint32_t cg_rx_top(const struct cg_replay *w)
{
	return w->init ? w->top : 0;
}

/* A HELLO: verified, answering a probe of this link not answered yet; the
 * link keeps its cookie. */
static inline enum cg_rxv cg_rx_hello(struct cg_rxs *r, struct cg_rxl *l, const struct cg_hdr *h, const uint8_t *b,
				      size_t len, uint64_t now_ms)
{
	struct cg_hello hl;

	if (!cg_hdr_verify(b, len, r->k_rx)) {
		r->auth_fail++;
		return CG_RXV_AUTH;
	}
	cg_hello_read(&hl, b + CG_HDR_LEN);
	if (!cg_echo_take(&l->probes, hl.echo_ts, now_ms, CG_ECHO_MAX_AGE_MS)) {
		r->hellos_stale++;
		return CG_RXV_DROP;
	}
	r->hellos++;
	l->cookie = hl.cookie;
	l->refused = !!(h->flags & CG_F_REFUSED);
	l->hello_ms = now_ms;
	return CG_RXV_HELLO;
}

/* One datagram of len bytes in b from link (its socket of generation gen),
 * received at now_us; trunc: the kernel cut it. expect: the links the server
 * should send each packet on (for the arrival ring). *h gets the header. */
static inline enum cg_rxv cg_rx_entry(struct cg_rxs *r, struct cg_rxl *l, unsigned link, uint8_t gen,
				      const uint8_t *b, size_t len, int trunc, uint64_t now_us, uint16_t expect,
				      struct cg_hdr *h)
{
	uint64_t now_ms = now_us / 1000;
	uint32_t now32 = (uint32_t)now_us;
	struct cg_replay *w;

	if (!l->open || gen != l->gen) {
		r->stale_gen++;
		return CG_RXV_DROP;
	}
	if (trunc) {
		r->trunc++;
		return CG_RXV_DROP;
	}
	if (cg_hdr_parse(h, b, len) < 0 ||
	    (h->type != CG_T_DATA && h->type != CG_T_PROBE_REPLY && h->type != CG_T_HELLO)) {
		r->malformed++;
		return CG_RXV_DROP;
	}
	if (h->session != r->session) {
		r->foreign++;
		return CG_RXV_DROP;
	}
	if (h->type == CG_T_HELLO)
		return cg_rx_hello(r, l, h, b, len, now_ms);
	w = h->type == CG_T_DATA ? &r->replay : &r->ctl;
	switch (cg_replay_check(w, h->seq)) {
	case CG_RP_OLD:
		r->old++;
		return CG_RXV_DROP;
	case CG_RP_DUP:
		r->dups++;
		if (h->type == CG_T_DATA)
			cg_arr_dup(&r->arr, r->rx, h->seq, now32, link);
		return CG_RXV_DROP;
	default:
		if (!cg_hdr_verify(b, len, r->k_rx)) {
			r->auth_fail++;
			return CG_RXV_AUTH;
		}
	}
	cg_replay_mark(w, h->seq);
	l->last_rx_ms = now_ms;
	if (now_ms > r->newest_ms)
		r->newest_ms = now_ms;
	if (h->type == CG_T_PROBE_REPLY)
		return CG_RXV_REPLY;
	cg_arr_first(&r->arr, r->rx, h->seq, now32, link, expect);
	return CG_RXV_DATA;
}

/* Writes the header of a packet going up, MAC included; returns its
 * sequence. */
static inline uint32_t cg_up_header(struct cg_txs *t, uint8_t out[CG_HDR_LEN], uint8_t type, uint8_t flags,
				    uint8_t link, uint32_t ts, const void *payload, size_t len)
{
	struct cg_hdr h = { .type = type,
			    .flags = flags,
			    .hint = t->hint,
			    .link = link,
			    .session = t->session,
			    .seq = cg_type_ctl(type) ? t->seq_ctl++ : t->seq++,
			    .ts = ts };

	cg_hdr_write(out, &h, t->k_tx, payload, len);
	return h.seq;
}

#endif
