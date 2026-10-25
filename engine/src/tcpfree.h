/* TCP-aware release (story 014, phase C): with tunnel = native the sender
 * sees inner TCP, and marks CG_F_FREE its segments without data (pure ACKs,
 * SYN, FIN, RST), which the receiver's reorderer hands on at once, and its
 * retransmissions. A retransmission was handed on at once too, at first:
 * held behind the gap of the packet it repairs, it waited for the tunnel
 * to give that gap up (on the testnet TCP repaired in ~30 ms and its
 * retransmission waited 500 ms, reorder_max_wait_ms). But ahead of what
 * was held, sent before it, it made TCP take all of that for lost and send
 * it again (story 014, 2026-10-16): the receiver now tells it apart
 * (cg_tcp_data) and keeps it in sequence (reorder.h).
 *
 * Per flow (flow.h's hash, a direct-mapped table of CG_TCPF_N entries, no
 * allocation per packet) the highest sequence sent, its end: a segment that
 * ends at or below it is sent again. A flow sharing an entry takes it over
 * (it is new to the table, nothing is marked); SYN starts a flow again.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TCPFREE_H
#define CG_TCPFREE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "flow.h"

#define CG_TCPF_N 4096 /* flows tracked: a power of two */

_Static_assert(!(CG_TCPF_N & (CG_TCPF_N - 1)), "CG_TCPF_N is a power of two");

#define CG_TCP_FIN 0x01
#define CG_TCP_SYN 0x02
#define CG_TCP_RST 0x04

struct cg_tcpf {
	uint32_t key[CG_TCPF_N]; /* the flow's hash */
	uint32_t hi[CG_TCPF_N];  /* the end (seq + length) of its highest segment */
	uint32_t rhi[CG_TCPF_N]; /* the end of its highest retransmission (with rok) */
	uint8_t used[CG_TCPF_N];
	uint8_t rok[CG_TCPF_N];
	uint64_t ctl, retx, retx_again; /* marked: without data (ACK, SYN, FIN, RST); sent again; of them, again */
};

/* What a segment is to the sender (story 023): not marked, a segment
 * without data (CG_F_FREE), a first retransmission of its bytes, or one of
 * bytes already retransmitted (the one copy did not do). */
enum { CG_TCPF_NO = 0, CG_TCPF_CTL, CG_TCPF_RETX1, CG_TCPF_RETXN };

/* Whether the IP packet b (len bytes, flow f from cg_flow_of) is a TCP
 * segment to mark CG_F_FREE, and why (CG_TCPF_*); it updates the flow's
 * highest end and, for a retransmission, the highest end retransmitted:
 * one that ends at or below that is sent again (a timeout's go-back, or a
 * retransmission lost too). */
static inline int cg_tcpf_class(struct cg_tcpf *t, const struct cg_flow *f, const uint8_t *b, size_t len)
{
	size_t off = f->l4off, doff;
	uint32_t seq, end, i;
	uint8_t fl;

	if (!f->tcp || f->frag || off + 20 > len)
		return 0;
	doff = (size_t)(b[off + 12] >> 4) * 4;
	if (doff < 20 || off + doff > len)
		return 0;
	seq = (uint32_t)b[off + 4] << 24 | (uint32_t)b[off + 5] << 16 | (uint32_t)b[off + 6] << 8 | b[off + 7];
	fl = b[off + 13];
	end = seq + (uint32_t)(len - off - doff) + (fl & CG_TCP_SYN ? 1 : 0) + (fl & CG_TCP_FIN ? 1 : 0);
	i = f->hash & (CG_TCPF_N - 1);
	if (fl & (CG_TCP_SYN | CG_TCP_RST)) {
		t->key[i] = f->hash;
		t->hi[i] = end;
		t->used[i] = !(fl & CG_TCP_RST);
		t->rok[i] = 0;
		t->ctl++;
		return CG_TCPF_CTL;
	}
	if (len == off + doff && !(fl & CG_TCP_FIN)) { /* a pure ACK (or a window update) */
		t->ctl++;
		return CG_TCPF_CTL;
	}
	if (!t->used[i] || t->key[i] != f->hash) {
		t->key[i] = f->hash;
		t->hi[i] = end;
		t->used[i] = 1;
		t->rok[i] = 0;
		return CG_TCPF_NO;
	}
	if ((int32_t)(end - t->hi[i]) <= 0) {
		t->retx++;
		if (t->rok[i] && (int32_t)(end - t->rhi[i]) <= 0) {
			t->retx_again++;
			return CG_TCPF_RETXN;
		}
		t->rhi[i] = end;
		t->rok[i] = 1;
		return CG_TCPF_RETX1;
	}
	t->hi[i] = end;
	return CG_TCPF_NO;
}

/* Whether the IP packet b is a TCP segment to mark CG_F_FREE (any class
 * but CG_TCPF_NO). */
static inline int cg_tcpf_free(struct cg_tcpf *t, const struct cg_flow *f, const uint8_t *b, size_t len)
{
	return cg_tcpf_class(t, f, b, len) != CG_TCPF_NO;
}

/* At the receiver, whether a packet marked CG_F_FREE is one of TCP's
 * retransmissions rather than a segment without data: IP packet b (len
 * bytes) is a TCP segment, not a fragment, without SYN or RST, that
 * carries data. No state, and no change to the protocol: what was marked
 * for which reason shows in the segment itself. A fragment, a short or
 * malformed header, or anything not TCP reads as not one (as a free ACK:
 * delivered at once and nothing more). */
static inline int cg_tcp_data(const uint8_t *b, size_t len)
{
	struct cg_flow f;
	size_t off, doff;

	if (!cg_flow_of(b, len, 0, &f) || !f.tcp || f.frag)
		return 0;
	off = f.l4off;
	if (off + 20 > len)
		return 0;
	doff = (size_t)(b[off + 12] >> 4) * 4;
	if (doff < 20 || off + doff >= len)
		return 0;
	return !(b[off + 13] & (CG_TCP_SYN | CG_TCP_RST));
}


/* ---- where the tunnel delivers TCP: the flows' round trip and the losses
 * beyond it (story 023) ----
 *
 * The end that writes a flow's data out of the tunnel (the server for an
 * upload, the router for a download) also reads that flow's ACKs into it.
 * From the handshake it learns how far the far host is: a SYN written out
 * at t1 and its SYN-ACK read in at t2 put the far leg at t2 - t1; the
 * flow's round trip is that plus the tunnel's (the caller's fastest path),
 * and TCP retransmits a hole about that much (plus RACK's quarter) after
 * the packets behind it arrive (harq.h's budget).
 *
 * From the ACKs it sees the far host's holes: a SACK block above the
 * cumulative ACK says the bytes between were not received. If the tunnel
 * delivered past them, and they were not a hole of its own (a sequence it
 * gave up, written out late or never: the last CG_TCPE_HOLES holes in TCP
 * sequence where it wrote a segment past the flow's highest end), they
 * were lost beyond the tunnel: counted (beyond), never fed to a link's
 * estimators, which only ever see the tunnel's own sequence.
 *
 * Per flow a direct-mapped entry by the hash of its data direction (the
 * ACKs look it up by their reversed tuple: cg_flow_of with rev); a flow
 * sharing it takes it over. Approximate by design: one entry per bucket,
 * the first SACK block only. */

#define CG_TCPE_HOLES 4

struct cg_tcpe_flow {
	uint32_t key;
	uint32_t hi;       /* the highest end written out */
	uint32_t syn_t;    /* when its SYN went out (us, low bits | 1), 0: none pending */
	uint32_t ack;      /* the cumulative ACK of the last hole counted */
	uint32_t hole_lo[CG_TCPE_HOLES], hole_hi[CG_TCPE_HOLES];
	uint8_t used, nh, ack_ok;
};

struct cg_tcpe {
	struct cg_tcpe_flow f[CG_TCPF_N];
	uint32_t leg_us;   /* the far leg's round trip, a min that rises slowly; 0: none yet */
	uint32_t tcp_t;    /* when TCP data last went out (us, low bits | 1), 0: never */
	uint64_t holes, beyond, own, legs; /* SACK holes seen; beyond the tunnel; the tunnel's; handshakes timed */
};

/* A TCP segment's sequence, flags and data length (b, len bytes, flow f):
 * 0, or -1 when it is not one to read. */
static inline int cg_tcp_seg(const struct cg_flow *f, const uint8_t *b, size_t len, uint32_t *seq, uint32_t *ack,
			     uint8_t *fl, uint32_t *dlen, size_t *doff)
{
	size_t off = f->l4off;

	if (!f->tcp || f->frag || off + 20 > len)
		return -1;
	*doff = (size_t)(b[off + 12] >> 4) * 4;
	if (*doff < 20 || off + *doff > len)
		return -1;
	*seq = (uint32_t)b[off + 4] << 24 | (uint32_t)b[off + 5] << 16 | (uint32_t)b[off + 6] << 8 | b[off + 7];
	*ack = (uint32_t)b[off + 8] << 24 | (uint32_t)b[off + 9] << 16 | (uint32_t)b[off + 10] << 8 | b[off + 11];
	*fl = b[off + 13];
	*dlen = (uint32_t)(len - off - *doff);
	return 0;
}

/* The first SACK block above ack of a segment's options (b, len bytes, its
 * header at f->l4off, doff long): 1 and its left edge, else 0. */
static inline int cg_tcp_sack_left(const struct cg_flow *f, const uint8_t *b, size_t doff, uint32_t ack,
				   uint32_t *left)
{
	size_t o = (size_t)f->l4off + 20, end = (size_t)f->l4off + doff;
	int found = 0;

	while (o < end) {
		uint8_t kind = b[o], ol;

		if (kind == 0)
			break;
		if (kind == 1) {
			o++;
			continue;
		}
		if (o + 1 >= end)
			break;
		ol = b[o + 1];
		if (ol < 2 || o + ol > end)
			break;
		if (kind == 5)
			for (size_t q = o + 2; q + 8 <= o + ol; q += 8) {
				uint32_t l = (uint32_t)b[q] << 24 | (uint32_t)b[q + 1] << 16 | (uint32_t)b[q + 2] << 8 |
					     b[q + 3];

				if ((int32_t)(l - ack) > 0 && (!found || (int32_t)(l - *left) < 0)) {
					*left = l;
					found = 1;
				}
			}
		o += ol;
	}
	return found;
}

/* A segment the tunnel wrote out (b, len bytes, flow f), at now_us. */
static inline void cg_tcpe_out(struct cg_tcpe *e, const struct cg_flow *f, const uint8_t *b, size_t len,
			       uint32_t now_us)
{
	struct cg_tcpe_flow *x;
	uint32_t seq, ack, dl, end;
	size_t doff;
	uint8_t fl;

	if (cg_tcp_seg(f, b, len, &seq, &ack, &fl, &dl, &doff) < 0)
		return;
	x = &e->f[f->hash & (CG_TCPF_N - 1)];
	end = seq + dl + (fl & CG_TCP_SYN ? 1 : 0) + (fl & CG_TCP_FIN ? 1 : 0);
	if (fl & CG_TCP_SYN) {
		memset(x, 0, sizeof(*x));
		x->key = f->hash;
		x->used = 1;
		x->hi = end;
		if (!(fl & 0x10)) /* a SYN without ACK: the handshake starts here */
			x->syn_t = now_us | 1;
		return;
	}
	if (!dl)
		return;
	e->tcp_t = now_us | 1;
	if (!x->used || x->key != f->hash) {
		memset(x, 0, sizeof(*x));
		x->key = f->hash;
		x->used = 1;
		x->hi = end;
		return;
	}
	if ((int32_t)(seq - x->hi) > 0) { /* past the highest end: a hole of the tunnel's */
		unsigned h = x->nh++ % CG_TCPE_HOLES;

		x->hole_lo[h] = x->hi;
		x->hole_hi[h] = seq;
	}
	if ((int32_t)(end - x->hi) > 0)
		x->hi = end;
}

/* A segment read into the tunnel (b, len bytes) whose reversed flow is r
 * (cg_flow_of with rev: the data direction's hash), at now_us: a SYN-ACK
 * times the far leg; an ACK with a SACK hole is counted. */
static inline void cg_tcpe_in(struct cg_tcpe *e, const struct cg_flow *r, const uint8_t *b, size_t len,
			      uint32_t now_us)
{
	struct cg_tcpe_flow *x = &e->f[r->hash & (CG_TCPF_N - 1)];
	uint32_t seq, ack, dl, left = 0;
	size_t doff;
	uint8_t fl;

	if (!x->used || x->key != r->hash || cg_tcp_seg(r, b, len, &seq, &ack, &fl, &dl, &doff) < 0)
		return;
	if ((fl & CG_TCP_SYN) && (fl & 0x10) && x->syn_t) {
		uint32_t leg = (now_us | 1) - x->syn_t;

		x->syn_t = 0;
		if (leg < 10u * 1000 * 1000) {
			e->legs++;
			if (!e->leg_us || leg < e->leg_us)
				e->leg_us = leg ? leg : 1;
			else
				e->leg_us += (leg - e->leg_us) / 8;
		}
		return;
	}
	if (!(fl & 0x10) || !cg_tcp_sack_left(r, b, doff, ack, &left))
		return;
	if (x->ack_ok && x->ack == ack)
		return; /* the same hole, counted */
	x->ack = ack;
	x->ack_ok = 1;
	e->holes++;
	for (unsigned h = 0; h < CG_TCPE_HOLES && h < x->nh; h++)
		if ((int32_t)(ack - x->hole_lo[h]) >= 0 && (int32_t)(ack - x->hole_hi[h]) < 0) {
			e->own++;
			return;
		}
	if ((int32_t)(left - x->hi) <= 0)
		e->beyond++;
}

/* The round trip of the TCP flows through this end now (the far leg plus
 * the tunnel's, tunnel_us), 0 when no TCP data went out within idle_us. */
static inline uint32_t cg_tcpe_rtt(const struct cg_tcpe *e, uint32_t tunnel_us, uint32_t now_us, uint32_t idle_us)
{
	if (!e->tcp_t || (now_us | 1) - e->tcp_t > idle_us)
		return 0;
	return e->leg_us + tunnel_us;
}

#endif
