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
 *   4. the anti-replay window, before any MAC: OLD is dropped unless it is a
 *      probe reply that proves the server started over (epoch.h), which
 *      verifies its MAC and resets the window; DUP is dropped once its
 *      arrival is noted (cg_arr_dup, display only: no MAC was checked);
 *   5. the MAC, paid only by NEW packets;
 *   6. the window is marked, only now;
 *   7. a probe reply goes back to the caller (it updates the link's health);
 *      DATA is the first copy, noted in the arrival ring, for WireGuard.
 * cg_up_header() writes the header of one packet that goes up: the next
 * sequence of the session and the MAC, computed once (the link byte is
 * patched per link, outside the MAC).
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
	uint64_t last_rx_ms;   /* newest verified packet on it */
	struct cg_echo probes; /* its probes, for the echoes in replies (epoch.h) */
};

/* The download state of the session (one owner: the loop, or the hub). */
struct cg_rxs {
	uint32_t session;
	const uint8_t *k_rx;
	uint32_t restart_ms; /* a reply resets the window only after this long without anything new */
	uint64_t newest_ms;  /* newest verified NEW packet on any link */
	struct cg_replay replay;
	struct cg_arrivals arr;
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t malformed, foreign, auth_fail, old, dups, trunc, stale_gen, window_resets;
};

enum cg_rxv {
	CG_RXV_DROP = 0, /* counted, nothing else to do */
	CG_RXV_DATA,     /* a first copy: to WireGuard */
	CG_RXV_REPLY,    /* a verified probe reply: the caller takes it */
	CG_RXV_RESET,    /* the same, and it reset the window: the server started over */
	CG_RXV_AUTH,     /* dropped: its MAC failed (the caller warns, rate-limited) */
};

/* The upload side of the session. */
struct cg_txs {
	uint32_t session;
	uint32_t seq; /* next sequence, shared by DATA and probes */
	const uint8_t *k_tx;
};

/* An OLD packet: when it is a probe reply showing that the server started
 * over (epoch.h), the window starts again and the packet goes on as NEW, its
 * MAC verified (returns 1). Only probe replies pay for a MAC here, and the
 * echo is taken only from a verified one. Arrival times may come out of
 * order between links (the hub takes each pump's batches in turn), so the
 * silence never counts as negative. */
static inline int cg_rx_started_over(struct cg_rxs *r, struct cg_rxl *l, const struct cg_hdr *h, const uint8_t *b,
				     size_t len, uint64_t now_ms)
{
	struct cg_probe_info pi;
	int mac_ok, echo = 0;

	if (h->type != CG_T_PROBE_REPLY)
		return 0;
	mac_ok = cg_hdr_verify(b, len, r->k_rx);
	if (mac_ok) {
		cg_probe_info_read(&pi, b + CG_HDR_LEN);
		echo = cg_echo_take(&l->probes, pi.echo_ts, now_ms, CG_ECHO_MAX_AGE_MS);
	}
	if (!cg_restart_reply(CG_RP_OLD, h->type, mac_ok, echo, now_ms > r->newest_ms ? now_ms - r->newest_ms : 0,
			      r->restart_ms))
		return 0;
	cg_replay_reset(&r->replay);
	r->window_resets++;
	return 1;
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
	enum cg_rxv v = CG_RXV_REPLY;

	if (!l->open || gen != l->gen) {
		r->stale_gen++;
		return CG_RXV_DROP;
	}
	if (trunc) {
		r->trunc++;
		return CG_RXV_DROP;
	}
	if (cg_hdr_parse(h, b, len) < 0 || (h->type != CG_T_DATA && h->type != CG_T_PROBE_REPLY)) {
		r->malformed++;
		return CG_RXV_DROP;
	}
	if (h->session != r->session) {
		r->foreign++;
		return CG_RXV_DROP;
	}
	switch (cg_replay_check(&r->replay, h->seq)) {
	case CG_RP_OLD:
		if (!cg_rx_started_over(r, l, h, b, len, now_ms)) {
			r->old++;
			return CG_RXV_DROP;
		}
		v = CG_RXV_RESET; /* verified */
		break;
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
	cg_replay_mark(&r->replay, h->seq);
	l->last_rx_ms = now_ms;
	if (now_ms > r->newest_ms)
		r->newest_ms = now_ms;
	if (h->type == CG_T_PROBE_REPLY)
		return v;
	cg_arr_first(&r->arr, r->rx, h->seq, now32, link, expect);
	return CG_RXV_DATA;
}

/* Writes the header of a packet going up, MAC included; returns its
 * sequence. */
static inline uint32_t cg_up_header(struct cg_txs *t, uint8_t out[CG_HDR_LEN], uint8_t type, uint8_t flags,
				    uint8_t link, uint32_t ts, const void *payload, size_t len)
{
	struct cg_hdr h = {
		.type = type, .flags = flags, .link = link, .session = t->session, .seq = t->seq++, .ts = ts
	};

	cg_hdr_write(out, &h, t->k_tx, payload, len);
	return h.seq;
}

#endif
