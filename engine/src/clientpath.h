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
 *   2. truncated, not of this version, or of another session: dropped;
 *   3. the tag (wire.h), under the key of the server's newest epoch or,
 *      for a kind 1 with another epoch, that epoch's; then the inner header
 *      decrypted (a type the client does not take: dropped);
 *   4. a HELLO (the server cannot take our probes yet, proto.h) has no
 *      sequence: it answers a probe of this link that no reply or HELLO
 *      answered yet (epoch.h), or it is dropped; the caller keeps its
 *      cookie for the link's next probes; its epoch is its own, never the
 *      download's key;
 *   5. the anti-replay window of the packet's class (DATA, STREAM, or the
 *      control messages): OLD is dropped; DUP is dropped once its arrival
 *      is noted (cg_arr_dup), its payload never decrypted;
 *   6. the payload decrypted (cg_wire_open_body), paid only by NEW packets,
 *      and the packet's epoch and counter taken (cg_unseal_accept);
 *   7. the window is marked, only now;
 *   8. a probe reply goes back to the caller (it updates the link's health);
 *      DATA is the first copy, noted in the arrival ring (expected on every
 *      link of expect, or on its own link only when split), for the TUN; a
 *      STREAM (13) goes to the protocontroller as it comes, past the
 *      reorderer (its stream keeps its own order, and recovers its own
 *      losses from the subflows).
 * With subflow, every DATA's and STREAM's sseq goes to its link's subflow
 * (subflow.h), a copy's too (5: a copy says its link did not lose it).
 * A server that restarted needs no window reset here: it goes on past what
 * the probes say we received (cg_probe_info), and the reorderer takes its
 * 2^20 leap as a far packet, then resyncs (reorder.h).
 * What a DATA verdict hands on goes through the reorderer (reorder.h,
 * client.c's down_deliver) before the TUN; it lives here, with the window,
 * because it has the same one owner.
 * cg_up_header() writes the in-memory header of one packet that goes up,
 * with the next sequence of its class; cg_up_wire() seals a packet for one
 * link (wire.h), each copy with its own counter.
 *
 * Pure: no clock, no I/O, no log; the caller sends and logs what the verdict
 * says. tests/test_client_rx.c checks the order with a fake link.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_CLIENTPATH_H
#define CG_CLIENTPATH_H

#include <stddef.h>
#include <stdint.h>

#include "arrival.h"
#include "epoch.h"
#include "proto.h"
#include "reorder.h"
#include "replay.h"
#include "subflow.h"
#include "wire.h"

/* A link as the download sees it. */
struct cg_rxl {
	uint8_t gen;           /* of its socket: moves on every open and close */
	uint8_t open;          /* it has a socket */
	uint8_t refused;       /* its newest HELLO said CG_F_REFUSED */
	uint32_t cookie;       /* of its newest HELLO, for its probes (0: none) */
	uint64_t last_rx_ms;   /* newest verified packet on it */
	uint64_t hello_ms;     /* its newest HELLO */
	struct cg_echo probes; /* its probes, for the echoes in replies and HELLOs (epoch.h) */
	struct cg_sf_rx sf;    /* its subflow (subflow.h): the sseq the server numbers its DATA with */
};

/* The download state of the session (one owner: the loop, or the hub). */
struct cg_rxs {
	uint32_t session;
	const uint8_t *secret; /* the shared secret (the configuration's) */
	struct cg_unseal un;   /* the download's key, of the server's newest epoch (wire.h) */
	uint64_t newest_ms;  /* newest verified NEW packet on any link */
	struct cg_replay replay; /* DATA; its lost: lost on every link (the status's lost_all) */
	struct cg_replay stream; /* STREAM (13) */
	struct cg_replay ctl;    /* probe replies */
	struct cg_arrivals arr;  /* by link, and joint lateness (the status's joint) */
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t malformed, foreign, auth_fail, old, dups, trunc, stale_gen, hellos, hellos_stale;
	int subflow;         /* follow each link's subflow (the subflow knob): a copy's sseq counts too */
	struct cg_reorder ro; /* the first copies, back in sequence (reorder.h); about 81 KiB */
};

enum cg_rxv {
	CG_RXV_DROP = 0, /* counted, nothing else to do */
	CG_RXV_DATA,     /* a first copy: to the TUN */
	CG_RXV_REPLY,    /* a verified probe reply: the caller takes it */
	CG_RXV_HELLO,    /* a verified HELLO answering a probe of the link: its cookie is taken */
	CG_RXV_AUTH,     /* dropped: its tag failed (the caller warns, rate-limited) */
	CG_RXV_NACK,     /* a verified NACK: the caller sends those sequences again (rescue.h) */
	CG_RXV_FEC,      /* a verified parity: the caller may rebuild DATA from it (fec.h) */
	CG_RXV_PARQ,     /* a verified PARQ: the caller sends the repairs it asks for (harq.h) */
	CG_RXV_REPAIR,   /* a verified repair of the download: the caller may rebuild DATA from it (rlc.h, story 026) */
	CG_RXV_SFLOSS,   /* a verified SFLOSS: what a link's subflow lost up, for the caller to send again (subflow.h) */
	CG_RXV_SFACK,    /* a verified SFACK: how far each link's subflow got up (subflow.h) */
	CG_RXV_STREAM,   /* a first copy of a STREAM: to the protocontroller, past the reorderer (13) */
};

/* The upload side of the session. */
struct cg_txs {
	uint32_t session;
	uint32_t seq;     /* next DATA sequence */
	uint32_t seq_stream; /* next STREAM sequence (13) */
	uint32_t seq_ctl; /* next probe sequence */
	uint8_t hint;     /* cg_client_hint() of the key */
	struct cg_seal seal; /* the upload's key and counter (wire.h) */
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
				      uint64_t now_ms)
{
	struct cg_hello hl;

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

/* One datagram of *len bytes in b from link (its socket of generation gen,
 * an id below CG_MAX_LINKS), received at now_us; trunc: the kernel cut it.
 * expect: the links the server should send each duplicated packet on (for
 * the arrival ring). *h gets the header; a packet handed on (any verdict
 * but CG_RXV_DROP and CG_RXV_AUTH) is opened in place: b and *len hold the
 * in-memory packet (proto.h's header, the payload at b + CG_HDR_LEN). */
static inline enum cg_rxv cg_rx_entry(struct cg_rxs *r, struct cg_rxl *l, unsigned link, uint8_t gen, uint8_t *b,
				      size_t *len, int trunc, uint64_t now_us, uint16_t expect, struct cg_hdr *h)
{
	uint64_t now_ms = now_us / 1000, ctr;
	uint32_t now32 = (uint32_t)now_us, mlen;
	uint8_t tmp[CG_AEAD_KEY_LEN];
	const uint8_t *key;
	struct cg_replay *w;
	struct cg_wire wi;
	size_t n;

	if (!l->open || gen != l->gen) {
		r->stale_gen++;
		return CG_RXV_DROP;
	}
	if (trunc) {
		r->trunc++;
		return CG_RXV_DROP;
	}
	if (cg_wire_peek(&wi, b, *len) < 0) {
		r->malformed++;
		return CG_RXV_DROP;
	}
	if (wi.session != r->session) {
		r->foreign++;
		return CG_RXV_DROP;
	}
	key = cg_unseal_key(&r->un, &wi, r->secret, r->session, tmp);
	ctr = cg_wire_ctr(&wi, r->un.top);
	if (!key || cg_wire_open_head(h, b, *len, &wi, key, CG_DIR_S2C, ctr) < 0) {
		r->auth_fail++;
		return CG_RXV_AUTH;
	}
	if (h->type != CG_T_DATA && h->type != CG_T_PROBE_REPLY && h->type != CG_T_HELLO && h->type != CG_T_NACK &&
	    h->type != CG_T_FEC && h->type != CG_T_PARQ && h->type != CG_T_REPAIR && h->type != CG_T_SFLOSS &&
	    h->type != CG_T_SFACK && h->type != CG_T_STREAM) {
		r->malformed++;
		return CG_RXV_DROP;
	}
	/* what the packet weighs in memory: the subflow's bytes, as its sender counts them */
	mlen = (uint32_t)(*len - wi.hlen - CG_WIRE_INNER - CG_WIRE_TAG + CG_HDR_LEN);
	if (h->type == CG_T_HELLO) {
		n = cg_wire_open_body(b, *len, h, &wi, key, CG_DIR_S2C, ctr);
		if (!n) {
			r->malformed++;
			return CG_RXV_DROP;
		}
		*len = n;
		return cg_rx_hello(r, l, h, b, now_ms);
	}
	w = h->type == CG_T_DATA ? &r->replay : h->type == CG_T_STREAM ? &r->stream : &r->ctl;
	switch (cg_replay_check(w, h->seq)) {
	case CG_RP_OLD:
		r->old++;
		return CG_RXV_DROP;
	case CG_RP_DUP:
		r->dups++;
		if (h->type != CG_T_DATA && h->type != CG_T_STREAM)
			return CG_RXV_DROP;
		if (h->type == CG_T_DATA)
			cg_arr_dup(&r->arr, r->rx, h->seq, now32, link);
		else
			r->rx[link].stream++;
		/* a copy is still its link's: its sseq says the link did not
		 * lose it (its tag said the server sent it) */
		if (r->subflow && h->sseq)
			cg_sf_rx_took(&l->sf, h->sseq, cg_sf_rx_in(&l->sf, h->sseq, now32), mlen, now32);
		return CG_RXV_DROP;
	default:
		break;
	}
	n = cg_wire_open_body(b, *len, h, &wi, key, CG_DIR_S2C, ctr);
	if (!n) {
		r->malformed++;
		return CG_RXV_DROP;
	}
	*len = n;
	cg_unseal_accept(&r->un, &wi, key, ctr);
	cg_replay_mark(w, h->seq);
	l->last_rx_ms = now_ms;
	if (now_ms > r->newest_ms)
		r->newest_ms = now_ms;
	if (h->type == CG_T_PROBE_REPLY) {
		if (r->subflow && h->sseq) /* how far the server sent on this link */
			cg_sf_rx_sent(&l->sf, h->sseq, now32);
		return CG_RXV_REPLY;
	}
	if (h->type == CG_T_NACK)
		return CG_RXV_NACK;
	if (h->type == CG_T_FEC)
		return CG_RXV_FEC;
	if (h->type == CG_T_PARQ)
		return CG_RXV_PARQ;
	if (h->type == CG_T_REPAIR)
		return CG_RXV_REPAIR;
	if (h->type == CG_T_SFLOSS)
		return CG_RXV_SFLOSS;
	if (h->type == CG_T_SFACK)
		return CG_RXV_SFACK;
	if (r->subflow && h->sseq)
		cg_sf_rx_took(&l->sf, h->sseq, cg_sf_rx_in(&l->sf, h->sseq, now32), mlen, now32);
	if (h->type == CG_T_STREAM) {
		r->rx[link].stream++;
		r->rx[link].last_us = now32 ? now32 : 1;
		return CG_RXV_STREAM;
	}
	/* A split packet (split.h) comes on its one link: the others do not
	 * miss it, and are not late for it. */
	cg_arr_first(&r->arr, r->rx, h->seq, now32, link, h->flags & CG_F_SPLIT ? (uint16_t)(1u << link) : expect);
	return CG_RXV_DATA;
}

/* Writes the in-memory header of a packet going up (payload and len: what
 * follows it, sealed when it goes, cg_up_wire); returns its sequence. */
static inline uint32_t cg_up_header(struct cg_txs *t, uint8_t out[CG_HDR_LEN], uint8_t type, uint8_t flags,
				    uint8_t link, uint32_t ts, const void *payload, size_t len)
{
	struct cg_hdr h = { .type = type,
			    .flags = flags,
			    .hint = t->hint,
			    .link = link,
			    .session = t->session,
			    .seq = cg_type_ctl(type) ? t->seq_ctl++ : type == CG_T_STREAM ? t->seq_stream++ : t->seq++,
			    .ts = ts };

	cg_hdr_put(out, &h);
	return h.seq;
}

/* The next DATA sequence going up, its header written later (cg_up_seal):
 * the router decides a packet's copies after its sequence, and what the
 * next packet says of them is in its header (story 023). */
static inline uint32_t cg_up_next(struct cg_txs *t)
{
	return t->seq++;
}

/* The next STREAM sequence going up (13), as cg_up_next. */
static inline uint32_t cg_up_next_stream(struct cg_txs *t)
{
	return t->seq_stream++;
}

/* Writes the in-memory header of a packet going up with sequence seq
 * (cg_up_next); its link and sseq are set per copy as it goes. */
static inline void cg_up_seal(const struct cg_txs *t, uint8_t out[CG_HDR_LEN], uint8_t type, uint8_t flags,
			      uint8_t link, uint32_t seq, uint32_t ts)
{
	struct cg_hdr h = {
		.type = type, .flags = flags, .hint = t->hint, .link = link, .session = t->session, .seq = seq, .ts = ts
	};

	cg_hdr_put(out, &h);
}

/* Seals the in-memory packet hdr and its payload (plen bytes) for the link
 * and sseq hdr holds now into out (room for CG_HDR_LEN + plen +
 * CG_WIRE_EXTRA_MAX), with the upload's next counter; epoch1: a probe's
 * kind 1 (the whole counter and our epoch, wire.h). Returns its length. */
static inline size_t cg_up_wire(struct cg_txs *t, uint8_t *out, const uint8_t *hdr, const void *payload, size_t plen,
				int epoch1)
{
	return cg_wire_seal(out, hdr, payload, plen, &t->seal, epoch1);
}

#endif
