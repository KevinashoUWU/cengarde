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
 * delivered them they were lost beyond the tunnel: counted (beyond), never
 * fed to a link's estimators, which only ever see the tunnel's own
 * sequence. A hole of the tunnel's own is a TCP range it skipped (it wrote
 * a segment past the flow's highest end) and has not written since: a late
 * copy or the retransmission that fills it trims it or drops it. It keeps
 * CG_TCPE_HOLES of them; past that it only knows the range they lie in,
 * and below the flow's first segment seen (its base) nothing.
 *
 * And it says so to the end that sends the data (tcpack.h, story 023's
 * rule from evidence): cg_tcpe_in's verdict on each ACK with SACK blocks
 * goes into the tunnel with that ACK, CG_F_TUNHOLE on its DATA (proto.h),
 * when some hole between its cumulative ACK and its highest SACK edge is
 * the tunnel's own and lost on every link; never when it does not know
 * (below the base, SACKed above what it wrote, a flow it does not track:
 * the lab's spurious case, story 023, where marking what it could not
 * vouch for copied retransmissions of what a stall only delayed, and TCP
 * retransmitted twice as much). A hole of its own is lost
 * when every link that may still bring it brought a later tunnel sequence
 * (links are FIFO; any copy counts, not only first ones): the one link the
 * packet after it says it went on alone (CG_F_PREV), when exactly one
 * tunnel sequence is missing before that packet, else every link that
 * brought anything lately (the caller's view, struct cg_tcpe_view: not the
 * links the reorderer waits for, which leave a stalled or muted link out).
 * Until then it is on its way, maybe on a
 * slow or stalled link (CG_TCPE_INFLIGHT): not marked, so the
 * retransmission is not copied (the lab's spurious case, story 023: a
 * retransmission copied past a stalled link reached the far host first
 * and RACK took everything still stalled for lost).
 *
 * Per flow a direct-mapped entry by the hash of its data direction (the
 * ACKs look it up by their reversed tuple: cg_flow_of with rev); a flow
 * sharing it takes it over. */

#define CG_TCPE_HOLES 8
#define CG_TCP_SACK_MAX 4 /* blocks read from one segment: 4 fit in 40 bytes of options */

/* cg_tcpe_in's verdict on an ACK's holes: none (no SACK block above its
 * cumulative ACK, or not one to read), all delivered by the tunnel (lost
 * beyond it), the tunnel's own but maybe still on their way, not known,
 * or the tunnel's own and lost on every link: only OWN is marked. */
enum { CG_TCPE_NONE = 0, CG_TCPE_THROUGH, CG_TCPE_INFLIGHT, CG_TCPE_UNVOUCHED, CG_TCPE_OWN };

/* What the receiving end knows of its links when an ACK is judged: those
 * that brought a copy of anything lately (rset: one of them may still
 * bring a missing sequence), and per link the newest sequence it brought,
 * any copy (hw, valid with hw_ok). */
#define CG_TCPE_RECENT_US 2000000u /* "lately": a link that brought nothing for this long brings nothing old */
struct cg_tcpe_view {
	uint16_t rset, hw_ok;
	uint32_t hw[16];
};

struct cg_tcpe_flow {
	uint32_t key;
	uint32_t hi;       /* the highest end written out */
	uint32_t base;     /* below it the tunnel cannot vouch for what it wrote */
	uint32_t syn_t;    /* when its SYN went out (us, low bits | 1), 0: none pending */
	uint32_t ack;      /* the cumulative ACK of the last hole counted */
	uint32_t cum;      /* the highest cumulative ACK read (with cum_ok) */
	uint32_t tseq;     /* the newest tunnel sequence of its segments written */
	uint32_t hole_lo[CG_TCPE_HOLES], hole_hi[CG_TCPE_HOLES]; /* not written yet; lo == hi: a free slot */
	uint32_t hole_ts[CG_TCPE_HOLES]; /* the tunnel sequence of the segment written past it */
	uint8_t hole_ln[CG_TCPE_HOLES];  /* 1 + the link the one tunnel sequence before that went on alone, 0: not known */
	uint32_t unk_lo, unk_hi;          /* with unk_ok: holes it had no slot for lie in [unk_lo, unk_hi) */
	uint8_t used, ack_ok, cum_ok, unk_ok;
};

struct cg_tcpe {
	struct cg_tcpe_flow f[CG_TCPF_N];
	uint32_t leg_us;   /* the far leg's round trip, a min that rises slowly; 0: none yet */
	uint32_t tcp_t;    /* when TCP data last went out (us, low bits | 1), 0: never */
	/* SACK holes seen; beyond the tunnel; the tunnel's on their way; the
	 * tunnel's lost; not vouched for; handshakes timed; ACKs that went into
	 * the tunnel marked */
	uint64_t holes, beyond, inflight, own, unvouched, legs, marked;
};

/* Serial order of TCP sequences: a before b. */
static inline int cg_tseq_lt(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

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

/* The SACK blocks above ack of a segment's options (b, its header at
 * f->l4off, doff long) into l[] and r[], in their left edges' order, at
 * most CG_TCP_SACK_MAX: how many. A block that is empty, or not above ack
 * (a D-SACK), is left out. */
static inline int cg_tcp_sacks(const struct cg_flow *f, const uint8_t *b, size_t doff, uint32_t ack, uint32_t *l,
			       uint32_t *r)
{
	size_t o = (size_t)f->l4off + 20, end = (size_t)f->l4off + doff;
	int n = 0;

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
			for (size_t q = o + 2; q + 8 <= o + ol && n < CG_TCP_SACK_MAX; q += 8) {
				uint32_t bl = (uint32_t)b[q] << 24 | (uint32_t)b[q + 1] << 16 | (uint32_t)b[q + 2] << 8 |
					      b[q + 3];
				uint32_t br = (uint32_t)b[q + 4] << 24 | (uint32_t)b[q + 5] << 16 |
					      (uint32_t)b[q + 6] << 8 | b[q + 7];
				int k = n;

				if (!cg_tseq_lt(ack, bl) || !cg_tseq_lt(bl, br))
					continue;
				while (k > 0 && cg_tseq_lt(bl, l[k - 1])) {
					l[k] = l[k - 1];
					r[k] = r[k - 1];
					k--;
				}
				l[k] = bl;
				r[k] = br;
				n++;
			}
		o += ol;
	}
	return n;
}

/* Keeps [lo, hi) as one of x's holes: in a free slot or one below the
 * cumulative ACK; with none, the range it cannot keep track of grows by it
 * (a hole there is not known: never judged lost, nor delivered). Merging
 * two holes instead took the bytes written between them for missing, and
 * a stall that opened dozens of holes read as lost on every link (the
 * lab's spurious case, story 023). */
static inline void cg_tcpe_hole_add(struct cg_tcpe_flow *x, uint32_t lo, uint32_t hi, uint32_t ts, uint8_t ln)
{
	for (unsigned h = 0; h < CG_TCPE_HOLES; h++)
		if (x->hole_lo[h] == x->hole_hi[h] || (x->cum_ok && !cg_tseq_lt(x->cum, x->hole_hi[h]))) {
			x->hole_lo[h] = lo;
			x->hole_hi[h] = hi;
			x->hole_ts[h] = ts;
			x->hole_ln[h] = ln;
			return;
		}
	if (!x->unk_ok) {
		x->unk_lo = lo;
		x->unk_hi = hi;
		x->unk_ok = 1;
		return;
	}
	if (cg_tseq_lt(lo, x->unk_lo))
		x->unk_lo = lo;
	if (cg_tseq_lt(x->unk_hi, hi))
		x->unk_hi = hi;
}

/* [s, e) written out (below the highest end): the holes it covers are
 * trimmed, dropped or split. */
static inline void cg_tcpe_fill(struct cg_tcpe_flow *x, uint32_t s, uint32_t e)
{
	uint32_t slo = 0, shi = 0, sts = 0;
	uint8_t sln = 0;

	for (unsigned h = 0; h < CG_TCPE_HOLES; h++) {
		uint32_t lo = x->hole_lo[h], hi = x->hole_hi[h];

		if (lo == hi || !cg_tseq_lt(s, hi) || !cg_tseq_lt(lo, e))
			continue;
		if (!cg_tseq_lt(lo, s) && !cg_tseq_lt(e, hi)) {
			x->hole_lo[h] = x->hole_hi[h] = 0;
		} else if (!cg_tseq_lt(lo, s)) {
			x->hole_lo[h] = e;
		} else if (!cg_tseq_lt(e, hi)) {
			x->hole_hi[h] = s;
		} else { /* in the middle: two holes */
			x->hole_hi[h] = s;
			slo = e;
			shi = hi;
			sts = x->hole_ts[h];
			sln = x->hole_ln[h];
		}
	}
	if (slo != shi)
		cg_tcpe_hole_add(x, slo, shi, sts, sln);
}

/* A segment the tunnel wrote out (b, len bytes, flow f), at now_us, from
 * DATA of tunnel sequence tseq whose header said the sequence before it
 * went on link prev alone (CG_F_PREV; -1: not said). */
static inline void cg_tcpe_out(struct cg_tcpe *e, const struct cg_flow *f, const uint8_t *b, size_t len,
			       uint32_t now_us, uint32_t tseq, int prev)
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
		x->hi = x->base = end;
		x->tseq = tseq;
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
		x->base = seq;
		x->tseq = tseq;
		return;
	}
	if (cg_tseq_lt(x->hi, seq)) /* past the highest end: a hole of the tunnel's */
		cg_tcpe_hole_add(x, x->hi, seq, tseq,
				 tseq - x->tseq == 2 && prev >= 0 && prev < 16 ? (uint8_t)(prev + 1) : 0);
	else /* at or below it: a late copy, or a retransmission */
		cg_tcpe_fill(x, seq, cg_tseq_lt(end, x->hi) ? end : x->hi);
	if (cg_tseq_lt(x->hi, end))
		x->hi = end;
	if ((int32_t)(tseq - x->tseq) > 0)
		x->tseq = tseq;
}

/* Whether hole h of x is lost on every link that may still bring it (v:
 * the receiver's links; NULL, as if they all had): each brought a tunnel
 * sequence past the newest one that may hold its bytes, the one before
 * the segment written past it. */
static inline int cg_tcpe_hole_lost(const struct cg_tcpe_flow *x, unsigned h, const struct cg_tcpe_view *v)
{
	uint32_t g = x->hole_ts[h] - 1;
	uint16_t m;

	if (!v)
		return 1;
	m = x->hole_ln[h] ? (uint16_t)(1u << (x->hole_ln[h] - 1)) : v->rset;
	for (unsigned l = 0; l < 16; l++)
		if ((m >> l & 1) && (!(v->hw_ok >> l & 1) || (int32_t)(v->hw[l] - g) <= 0))
			return 0;
	return 1;
}

/* What the tunnel knows of the holes of an ACK of flow x: cumulative ACK
 * ack, SACK blocks l/r (n >= 1, in order), the receiver's links v. */
static inline int cg_tcpe_verdict(const struct cg_tcpe_flow *x, uint32_t ack, const uint32_t *l, const uint32_t *r,
				  int n, const struct cg_tcpe_view *v)
{
	uint32_t from = ack, top = r[0];
	int way = 0, unk = 0;

	for (int i = 1; i < n; i++)
		if (cg_tseq_lt(top, r[i]))
			top = r[i];
	if (cg_tseq_lt(ack, x->base) || cg_tseq_lt(x->hi, top))
		return CG_TCPE_UNVOUCHED;
	for (int i = 0; i < n; i++) {
		if (cg_tseq_lt(from, l[i]) && x->unk_ok && cg_tseq_lt(x->unk_lo, l[i]) && cg_tseq_lt(from, x->unk_hi))
			unk = 1;
		if (cg_tseq_lt(from, l[i])) /* the far host's hole [from, l[i]) */
			for (unsigned h = 0; h < CG_TCPE_HOLES; h++)
				if (x->hole_lo[h] != x->hole_hi[h] && cg_tseq_lt(x->hole_lo[h], l[i]) &&
				    cg_tseq_lt(from, x->hole_hi[h])) {
					if (cg_tcpe_hole_lost(x, h, v))
						return CG_TCPE_OWN;
					way = 1;
				}
		if (cg_tseq_lt(from, r[i]))
			from = r[i];
	}
	return unk ? CG_TCPE_UNVOUCHED : way ? CG_TCPE_INFLIGHT : CG_TCPE_THROUGH;
}

/* A segment read into the tunnel (b, len bytes) whose reversed flow is r
 * (cg_flow_of with rev: the data direction's hash), at now_us: a SYN-ACK
 * times the far leg; an ACK with SACK blocks is judged against the
 * receiver's links v (CG_TCPE_*: OWN and UNVOUCHED go into the tunnel
 * marked CG_F_TUNHOLE), and its holes counted once per cumulative ACK. */
static inline int cg_tcpe_in(struct cg_tcpe *e, const struct cg_flow *r, const uint8_t *b, size_t len,
			     uint32_t now_us, const struct cg_tcpe_view *v)
{
	struct cg_tcpe_flow *x = &e->f[r->hash & (CG_TCPF_N - 1)];
	uint32_t seq, ack, dl, sl[CG_TCP_SACK_MAX], sr[CG_TCP_SACK_MAX];
	size_t doff;
	uint8_t fl;
	int n, k, known = x->used && x->key == r->hash;

	if (cg_tcp_seg(r, b, len, &seq, &ack, &fl, &dl, &doff) < 0)
		return CG_TCPE_NONE;
	if ((fl & CG_TCP_SYN) && (fl & 0x10)) {
		if (known && x->syn_t) {
			uint32_t leg = (now_us | 1) - x->syn_t;

			x->syn_t = 0;
			if (leg < 10u * 1000 * 1000) {
				e->legs++;
				if (!e->leg_us || leg < e->leg_us)
					e->leg_us = leg ? leg : 1;
				else
					e->leg_us += (leg - e->leg_us) / 8;
			}
		}
		return CG_TCPE_NONE;
	}
	if (!(fl & 0x10) || (fl & (CG_TCP_SYN | CG_TCP_RST)))
		return CG_TCPE_NONE;
	if (known && (!x->cum_ok || cg_tseq_lt(x->cum, ack))) {
		x->cum = ack;
		x->cum_ok = 1;
		if (x->unk_ok && !cg_tseq_lt(ack, x->unk_hi)) /* past what it lost track of */
			x->unk_ok = 0;
	}
	n = cg_tcp_sacks(r, b, doff, ack, sl, sr);
	if (!n)
		return CG_TCPE_NONE;
	if (!known)
		return CG_TCPE_UNVOUCHED;
	k = cg_tcpe_verdict(x, ack, sl, sr, n, v);
	if (!x->ack_ok || x->ack != ack) { /* a hole not counted yet */
		x->ack = ack;
		x->ack_ok = 1;
		e->holes++;
		if (k == CG_TCPE_THROUGH)
			e->beyond++;
		else if (k == CG_TCPE_INFLIGHT)
			e->inflight++;
		else if (k == CG_TCPE_OWN)
			e->own++;
		else
			e->unvouched++;
	}
	if (k == CG_TCPE_OWN)
		e->marked++;
	return k;
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
