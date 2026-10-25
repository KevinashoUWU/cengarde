/* A reliable byte stream inside the tunnel, for PEP mode B (story 025, the
 * "protocontrolador"): each TCP connection the router takes from its LAN is
 * one, to the VPS, which opens a fresh TCP to the destination. Each end of
 * a stream sends bytes (socket to tunnel: struct cg_st_tx) and receives
 * them (tunnel to socket: struct cg_st_rx), by 64-bit offsets.
 *
 * - The bytes live in a ring of 16 KB chunks (struct cg_st_buf) taken from
 *   one pool per process (struct cg_st_pool, pep_mem_mb): what was read
 *   from the socket and is not acknowledged yet on the sending side, what
 *   arrived and was not written to the socket yet on the receiving side
 *   (in order or not).
 * - The sender keeps a segment per DATA it sent (struct cg_st_seg, in
 *   offset order; a retransmission reuses its segment, so the receiver
 *   never sees two framings of the same bytes). A segment is SENT, SACKED
 *   or LOST; LOST ones go again first. Loss is RACK's: a segment still
 *   SENT, sent more than a reordering window before one already delivered,
 *   is lost (cg_st_tx_rack); and a timer for the tail (cg_st_tx_rto: due
 *   rto after the oldest SENT segment's last transmission). It
 *   never gives up: a segment goes again until it arrives (the caller
 *   closes a stream that made no progress for pep_dead_s).
 * - The receiver keeps the next byte in order (nxt) and up to
 *   CG_ST_RANGES ranges above it; more holes than that, and a frame that
 *   would need a new range is dropped (the sender sends it again). The FIN
 *   takes one byte of the sequence, as in TCP: the stream's length is the
 *   FIN's offset, and an ACK past it acknowledges the FIN.
 * - Every ACK carries the whole state: the cumulative offset, the window's
 *   right edge (absolute: how far the receiver takes bytes) and up to
 *   CG_ST_ACK_RANGES SACK ranges, with a sequence number of its own (an ACK
 *   overtaken by a later one does not move the edge back).
 * - Offsets go on the wire as their low 32 bits (cg_st_unwrap): a window
 *   is at most CG_ST_SLOTS chunks, far under 2^31.
 *
 * The rate (pepcc.h) and the sockets (pep.c) are the caller's: this file
 * only keeps the stream's state, and each call returns what changed for
 * the rate controller (bytes delivered, lost, and the rate sample).
 *
 * Pure: no clock, no I/O, no allocation (the caller gives the pool's
 * memory and the segment ring).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_STREAM_H
#define CG_STREAM_H

#include <stdint.h>
#include <string.h>

#define CG_ST_CHUNK 16384u /* bytes per chunk of a stream's buffer */
#define CG_ST_SLOTS 256u   /* chunks a buffer spans: its window is under (CG_ST_SLOTS - 1) chunks */
#define CG_ST_NOCH UINT32_MAX
#define CG_ST_MAXWIN ((uint64_t)(CG_ST_SLOTS - 1) * CG_ST_CHUNK)
#define CG_ST_RANGES 256   /* out-of-order ranges a receiver keeps */
#define CG_ST_ACK_RANGES 8 /* SACK ranges an ACK carries */
#define CG_ST_NOFIN UINT64_MAX

/* ---- the pool of chunks ---- */

struct cg_st_pool {
	uint8_t *mem;    /* n chunks of CG_ST_CHUNK */
	uint32_t *stack; /* the free ones: the first nfree */
	uint32_t n, nfree, peak;
};

static inline void cg_st_pool_init(struct cg_st_pool *p, uint8_t *mem, uint32_t *stack, uint32_t n)
{
	p->mem = mem;
	p->stack = stack;
	p->n = p->nfree = n;
	p->peak = 0;
	for (uint32_t i = 0; i < n; i++)
		stack[i] = n - 1 - i;
}

static inline uint32_t cg_st_pool_used(const struct cg_st_pool *p)
{
	return p->n - p->nfree;
}

static inline uint32_t cg_st_get(struct cg_st_pool *p)
{
	uint32_t c;

	if (!p->nfree)
		return CG_ST_NOCH;
	c = p->stack[--p->nfree];
	if (p->n - p->nfree > p->peak)
		p->peak = p->n - p->nfree;
	return c;
}

static inline void cg_st_put(struct cg_st_pool *p, uint32_t c)
{
	if (c < p->n && p->nfree < p->n)
		p->stack[p->nfree++] = c;
}

static inline uint8_t *cg_st_chunk(const struct cg_st_pool *p, uint32_t c)
{
	return p->mem + (size_t)c * CG_ST_CHUNK;
}

/* ---- a stream's buffer: bytes by absolute offset, from lo up ---- */

struct cg_st_buf {
	uint64_t lo; /* nothing below is kept */
	uint32_t ch[CG_ST_SLOTS];
	uint32_t nch; /* chunks held */
};

static inline void cg_st_buf_init(struct cg_st_buf *b)
{
	b->lo = 0;
	b->nch = 0;
	for (unsigned i = 0; i < CG_ST_SLOTS; i++)
		b->ch[i] = CG_ST_NOCH;
}

/* Whether off falls in what the buffer can hold now: from lo's chunk on,
 * CG_ST_SLOTS chunks. */
static inline int cg_st_buf_in(const struct cg_st_buf *b, uint64_t off)
{
	uint64_t base = b->lo / CG_ST_CHUNK;

	return off >= b->lo && off / CG_ST_CHUNK < base + CG_ST_SLOTS;
}

/* Where byte off is, and in *room how many bytes follow it in its chunk;
 * with alloc, its chunk is taken from the pool if missing. NULL: out of
 * range, or no chunk (none in the pool, or !alloc). */
static inline uint8_t *cg_st_buf_at(struct cg_st_buf *b, struct cg_st_pool *p, uint64_t off, int alloc, uint32_t *room)
{
	uint32_t *c;

	if (!cg_st_buf_in(b, off))
		return NULL;
	c = &b->ch[(off / CG_ST_CHUNK) % CG_ST_SLOTS];
	if (*c == CG_ST_NOCH) {
		if (!alloc || (*c = cg_st_get(p)) == CG_ST_NOCH)
			return NULL;
		b->nch++;
	}
	if (room)
		*room = CG_ST_CHUNK - (uint32_t)(off % CG_ST_CHUNK);
	return cg_st_chunk(p, *c) + off % CG_ST_CHUNK;
}

/* Copies len bytes from src to offset off, taking chunks as needed.
 * Returns 0, or -1 (out of range or out of chunks: what was written stays,
 * harmless, the caller drops the frame). */
static inline int cg_st_buf_write(struct cg_st_buf *b, struct cg_st_pool *p, uint64_t off, const uint8_t *src,
				  uint32_t len)
{
	while (len) {
		uint32_t room, k;
		uint8_t *d = cg_st_buf_at(b, p, off, 1, &room);

		if (!d)
			return -1;
		k = len < room ? len : room;
		memcpy(d, src, k);
		src += k;
		off += k;
		len -= k;
	}
	return 0;
}

/* Copies len bytes at offset off to dst. Returns 0, or -1 if a chunk is
 * missing (never for bytes written). */
static inline int cg_st_buf_read(struct cg_st_buf *b, struct cg_st_pool *p, uint64_t off, uint8_t *dst, uint32_t len)
{
	while (len) {
		uint32_t room, k;
		const uint8_t *s = cg_st_buf_at(b, p, off, 0, &room);

		if (!s)
			return -1;
		k = len < room ? len : room;
		memcpy(dst, s, k);
		dst += k;
		off += k;
		len -= k;
	}
	return 0;
}

/* Nothing below lo is needed any more: the chunks wholly under it go back. */
static inline void cg_st_buf_release(struct cg_st_buf *b, struct cg_st_pool *p, uint64_t lo)
{
	uint64_t c0, c1;

	if (lo <= b->lo)
		return;
	c0 = b->lo / CG_ST_CHUNK;
	c1 = lo / CG_ST_CHUNK; /* chunks c0 .. c1 - 1 are done */
	if (c1 - c0 > CG_ST_SLOTS)
		c0 = c1 - CG_ST_SLOTS;
	for (uint64_t c = c0; c < c1; c++) {
		uint32_t *s = &b->ch[c % CG_ST_SLOTS];

		if (*s != CG_ST_NOCH) {
			cg_st_put(p, *s);
			*s = CG_ST_NOCH;
			b->nch--;
		}
	}
	b->lo = lo;
}

static inline void cg_st_buf_free(struct cg_st_buf *b, struct cg_st_pool *p)
{
	for (unsigned i = 0; i < CG_ST_SLOTS; i++)
		if (b->ch[i] != CG_ST_NOCH) {
			cg_st_put(p, b->ch[i]);
			b->ch[i] = CG_ST_NOCH;
		}
	b->nch = 0;
}

/* ---- offsets on the wire ---- */

/* The 64-bit offset whose low 32 bits are v, nearest to ref. */
static inline uint64_t cg_st_unwrap(uint64_t ref, uint32_t v)
{
	int32_t d = (int32_t)(v - (uint32_t)ref);

	if (d < 0 && (uint64_t)(-(int64_t)d) > ref)
		return 0;
	return (uint64_t)((int64_t)ref + d);
}

/* ---- the receiving side ---- */

struct cg_st_rx {
	uint64_t nxt; /* the next byte in order (past the FIN's once it came) */
	uint64_t fin; /* the FIN's offset: the stream's length; CG_ST_NOFIN: not known */
	uint16_t nr;  /* ranges above nxt, sorted, disjoint and not touching */
	uint64_t rs[CG_ST_RANGES], re[CG_ST_RANGES];
	uint64_t last;   /* the offset of the newest frame taken (its range goes first in an ACK) */
	uint64_t frames, dup_frames, ooo_frames, drop_frames; /* data frames taken, all duplicate, out of order, dropped */
};

static inline void cg_st_rx_init(struct cg_st_rx *r)
{
	memset(r, 0, sizeof(*r));
	r->fin = CG_ST_NOFIN;
}

enum { CG_ST_RX_NEW = 0, CG_ST_RX_DUP, CG_ST_RX_DROP };

/* What to do with a data frame of len bytes at off (fin: the stream ends
 * after them; its byte counts in span), with the receiver taking bytes up to
 * lim (the window's right edge as it will hold them): NEW, store the
 * bytes and call cg_st_rx_add; DUP, all of it came already; DROP, beyond
 * the window, past a known FIN, or a new range with none left. */
/* The first range whose end is at least off (binary search). */
static inline unsigned cg_st_rx_find(const struct cg_st_rx *r, uint64_t off)
{
	unsigned lo = 0, hi = r->nr;

	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;

		if (r->re[mid] < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static inline int cg_st_rx_judge(const struct cg_st_rx *r, uint64_t off, uint32_t len, int fin, uint64_t lim)
{
	uint64_t end = off + len + (fin ? 1 : 0);
	unsigned i;

	if (end <= r->nxt)
		return CG_ST_RX_DUP;
	if (r->fin != CG_ST_NOFIN && (end > r->fin + 1 || (fin && off + len != r->fin)))
		return CG_ST_RX_DROP;
	if (off + len > lim)
		return CG_ST_RX_DROP;
	i = cg_st_rx_find(r, off);
	if (i < r->nr && off >= r->rs[i] && end <= r->re[i])
		return CG_ST_RX_DUP;
	/* touching nxt or a range: it merges; else it needs a range of its own */
	if (off > r->nxt && !(i < r->nr && r->rs[i] <= end) && r->nr == CG_ST_RANGES)
		return CG_ST_RX_DROP;
	return CG_ST_RX_NEW;
}

/* The frame judged NEW is stored: its span goes into the ranges and nxt
 * moves past what is now in order. Returns 1 if nxt moved. */
static inline int cg_st_rx_add(struct cg_st_rx *r, uint64_t off, uint32_t len, int fin)
{
	uint64_t s = off < r->nxt ? r->nxt : off, e = off + len + (fin ? 1 : 0);
	unsigned i, j, k;
	uint64_t was = r->nxt;

	r->frames++;
	r->last = off;
	if (fin && r->fin == CG_ST_NOFIN)
		r->fin = off + len;
	if (s > r->nxt)
		r->ooo_frames++;
	/* merge [s, e) with every range it overlaps or touches */
	i = cg_st_rx_find(r, s);
	for (j = i; j < r->nr && r->rs[j] <= e; j++) {
		if (r->rs[j] < s)
			s = r->rs[j];
		if (r->re[j] > e)
			e = r->re[j];
	}
	/* ranges i .. j-1 become one: [s, e) at i */
	if (j == i) {
		if (r->nr == CG_ST_RANGES)
			return 0; /* judged before: not reached */
		for (k = r->nr; k > i; k--) {
			r->rs[k] = r->rs[k - 1];
			r->re[k] = r->re[k - 1];
		}
		r->nr++;
	} else {
		for (k = 0; j + k < r->nr; k++) {
			r->rs[i + 1 + k] = r->rs[j + k];
			r->re[i + 1 + k] = r->re[j + k];
		}
		r->nr = (uint8_t)(r->nr - (j - i - 1));
	}
	r->rs[i] = s;
	r->re[i] = e;
	if (r->nr && r->rs[0] <= r->nxt) {
		if (r->re[0] > r->nxt)
			r->nxt = r->re[0];
		for (k = 1; k < r->nr; k++) {
			r->rs[k - 1] = r->rs[k];
			r->re[k - 1] = r->re[k];
		}
		r->nr--;
	}
	return r->nxt != was;
}

/* Bytes in order not written yet, from done on: up to nxt, the FIN's byte
 * not counted. */
static inline uint64_t cg_st_rx_ready(const struct cg_st_rx *r, uint64_t done)
{
	uint64_t top = r->fin != CG_ST_NOFIN && r->nxt > r->fin ? r->fin : r->nxt;

	return top > done ? top - done : 0;
}

/* Whether the FIN came and everything before it too. */
static inline int cg_st_rx_ended(const struct cg_st_rx *r)
{
	return r->fin != CG_ST_NOFIN && r->nxt > r->fin;
}

/* ---- ACK ---- */

struct cg_st_ack {
	uint16_t aseq; /* the ACK's own sequence, per stream and direction */
	uint64_t cum, edge;
	uint8_t n;
	uint64_t s[CG_ST_ACK_RANGES], e[CG_ST_ACK_RANGES];
};

/* The ACK of what r holds, the edge given (the caller's window): the
 * range of the newest frame first (the sender's RACK learns the newest
 * delivery even with more holes than an ACK says), then the lowest ones
 * (the holes nearest to the cumulative offset). */
static inline void cg_st_rx_ack(const struct cg_st_rx *r, uint64_t edge, uint16_t aseq, struct cg_st_ack *a)
{
	unsigned newest = CG_ST_RANGES;

	a->aseq = aseq;
	a->cum = r->nxt;
	a->edge = edge;
	a->n = 0;
	for (unsigned i = 0; i < r->nr; i++)
		if (r->last >= r->rs[i] && r->last < r->re[i]) {
			newest = i;
			a->s[0] = r->rs[i];
			a->e[0] = r->re[i];
			a->n = 1;
			break;
		}
	for (unsigned i = 0; i < r->nr && a->n < CG_ST_ACK_RANGES; i++) {
		if (i == newest)
			continue;
		a->s[a->n] = r->rs[i];
		a->e[a->n] = r->re[i];
		a->n++;
	}
}

/* ---- the sending side ---- */

enum { CG_ST_SENT = 0, CG_ST_SACKED, CG_ST_LOST };

struct cg_st_seg {
	uint64_t off;
	uint64_t sent_us;       /* its last transmission */
	uint64_t dl, dla, dl_us, ss; /* the rate controller's counters when it was sent (pepcc.h) */
	uint16_t len;           /* bytes; the FIN, if any, after them */
	uint8_t fin, st, tx, app;
	uint8_t q;              /* sent while the path showed a queue (pepcc.h's cong) */
};

/* The rate sample an ACK gives: from the newest-sent segment it delivered. */
struct cg_st_rs {
	uint8_t have, tx, app;
	uint64_t sent_us, dl, dla, dl_us, ss;
};

struct cg_st_tx {
	uint64_t una;  /* everything below is acknowledged */
	uint64_t nxt;  /* the next byte never sent */
	uint64_t tail; /* buffered up to here: [una, tail) */
	uint8_t eof;   /* the socket ended: the FIN goes at tail */
	uint8_t fin_sent;
	uint8_t have_aseq;
	uint16_t aseq;  /* the newest ACK's sequence */
	uint64_t edge;  /* the peer's window right edge */
	struct cg_st_seg *seg;
	uint32_t cap, head, n; /* the ring of segments, from una up */
	uint32_t nlost, lost_from; /* LOST segments; none before index lost_from */
	uint64_t inflight;     /* bytes of SENT segments */
	uint64_t rack_us;      /* the newest send time of a delivered segment */
	uint64_t retx_segs, retx_bytes, rack_lost, rto_lost, copies;
};

static inline void cg_st_tx_init(struct cg_st_tx *t, struct cg_st_seg *seg, uint32_t cap, uint64_t edge)
{
	memset(t, 0, sizeof(*t));
	t->seg = seg;
	t->cap = cap;
	t->edge = edge;
}

static inline struct cg_st_seg *cg_st_tx_seg(const struct cg_st_tx *t, uint32_t i)
{
	return &t->seg[(t->head + i) % t->cap];
}

static inline uint32_t cg_st_span(const struct cg_st_seg *s)
{
	return (uint32_t)s->len + s->fin;
}

/* Bytes read from the socket and not sent yet. */
static inline uint64_t cg_st_tx_unsent(const struct cg_st_tx *t)
{
	return t->tail - t->nxt;
}

/* Whether everything was sent and acknowledged, the FIN too. */
static inline int cg_st_tx_done(const struct cg_st_tx *t)
{
	return t->eof && t->fin_sent && t->una > t->tail;
}

/* What goes next: a LOST segment (*idx its index), else new bytes (*idx =
 * t->n: a segment to append; *len bytes at *off, *fin with them), mss bytes
 * at most and inside the peer's window (the FIN alone may pass it).
 * Returns 0 when nothing can go. */
static inline int cg_st_tx_next(struct cg_st_tx *t, uint32_t mss, uint32_t *idx, uint64_t *off, uint32_t *len,
				int *fin)
{
	if (t->nlost) {
		for (uint32_t i = t->lost_from; i < t->n; i++) {
			const struct cg_st_seg *s = cg_st_tx_seg(t, i);

			if (s->st == CG_ST_LOST) {
				t->lost_from = i;
				*idx = i;
				*off = s->off;
				*len = s->len;
				*fin = s->fin;
				return 1;
			}
		}
		t->nlost = 0; /* not reached */
	}
	if (t->n >= t->cap || t->fin_sent)
		return 0;
	{
		uint64_t k = t->tail - t->nxt;

		if (t->nxt + k > t->edge)
			k = t->edge > t->nxt ? t->edge - t->nxt : 0;
		if (k > mss)
			k = mss;
		if (!k && !(t->eof && t->nxt == t->tail))
			return 0;
		*idx = t->n;
		*off = t->nxt;
		*len = (uint32_t)k;
		*fin = t->eof && t->nxt + k == t->tail;
	}
	return 1;
}

/* What cg_st_tx_next gave went at now, with the rate controller's counters
 * then (dl, dla, dl_us, ss) and whether it was app-limited. Returns the bytes
 * that went into flight. */
static inline uint32_t cg_st_tx_sent(struct cg_st_tx *t, uint32_t idx, uint64_t off, uint32_t len, int fin,
				     uint64_t now, uint64_t dl, uint64_t dla, uint64_t dl_us, uint64_t ss, int app, int q)
{
	struct cg_st_seg *s;

	if (idx == t->n) {
		s = cg_st_tx_seg(t, t->n++);
		s->off = off;
		s->len = (uint16_t)len;
		s->fin = (uint8_t)!!fin;
		s->tx = 0;
		t->nxt = off + len;
		if (fin)
			t->fin_sent = 1;
	} else {
		s = cg_st_tx_seg(t, idx);
		if (s->st != CG_ST_LOST)
			return 0;
		t->nlost--;
		t->retx_segs++;
		t->retx_bytes += s->len;
	}
	s->st = CG_ST_SENT;
	if (s->tx < 255)
		s->tx++;
	s->sent_us = now;
	s->dl = dl;
	s->dla = dla;
	s->dl_us = dl_us;
	s->ss = ss;
	s->app = (uint8_t)!!app;
	s->q = (uint8_t)!!q;
	t->inflight += cg_st_span(s);
	return cg_st_span(s);
}

/* What an ACK changed. */
struct cg_st_ackres {
	uint64_t delivered; /* bytes newly acknowledged or SACKed (FIN's included) */
	uint64_t freed;     /* bytes below the new una, to release from the buffer */
	uint64_t was_inflight, was_lost; /* of delivered, bytes that were SENT, and LOST */
	struct cg_st_rs rs;
	uint8_t progress;   /* una moved */
	uint8_t edge_moved;
};

static inline void cg_st_rs_take(struct cg_st_rs *rs, const struct cg_st_seg *s)
{
	if (!rs->have || s->sent_us > rs->sent_us) {
		rs->have = 1;
		rs->tx = s->tx;
		rs->app = s->app;
		rs->sent_us = s->sent_us;
		rs->dl = s->dl;
		rs->dla = s->dla;
		rs->dl_us = s->dl_us;
		rs->ss = s->ss;
	}
}

/* A segment delivered (by the cumulative offset or a SACK): out of flight
 * or out of the lost, into the sample. */
static inline void cg_st_tx_deliver(struct cg_st_tx *t, struct cg_st_seg *s, struct cg_st_ackres *r)
{
	uint32_t sp = cg_st_span(s);

	if (s->st == CG_ST_SACKED)
		return;
	if (s->st == CG_ST_SENT) {
		t->inflight -= sp;
		r->was_inflight += sp;
	} else {
		t->nlost--;
		r->was_lost += sp;
	}
	r->delivered += sp;
	cg_st_rs_take(&r->rs, s);
	if (s->sent_us > t->rack_us)
		t->rack_us = s->sent_us;
	s->st = CG_ST_SACKED;
}

/* The first segment index whose offset is at least off (binary search: the
 * ring is in offset order). */
static inline uint32_t cg_st_tx_find(const struct cg_st_tx *t, uint64_t off)
{
	uint32_t lo = 0, hi = t->n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (cg_st_tx_seg(t, mid)->off < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* An ACK came (offsets already unwrapped). Returns what changed in *r. */
static inline void cg_st_tx_ack(struct cg_st_tx *t, const struct cg_st_ack *a, struct cg_st_ackres *r)
{
	memset(r, 0, sizeof(*r));
	if (!t->have_aseq || (int16_t)(a->aseq - t->aseq) > 0) {
		t->have_aseq = 1;
		t->aseq = a->aseq;
		r->edge_moved = a->edge != t->edge;
		t->edge = a->edge;
	}
	/* the cumulative offset: segments wholly under it go */
	if (a->cum > t->una && a->cum <= t->nxt + (t->fin_sent ? 1 : 0)) {
		while (t->n) {
			struct cg_st_seg *s = cg_st_tx_seg(t, 0);

			if (s->off + cg_st_span(s) > a->cum)
				break;
			cg_st_tx_deliver(t, s, r);
			t->head = (t->head + 1) % t->cap;
			t->n--;
			if (t->lost_from)
				t->lost_from--;
		}
		r->freed = a->cum - t->una;
		t->una = a->cum;
		r->progress = 1;
	}
	for (unsigned k = 0; k < a->n; k++) {
		uint32_t i;

		if (a->e[k] <= t->una || a->s[k] >= a->e[k])
			continue;
		for (i = cg_st_tx_find(t, a->s[k]); i < t->n; i++) {
			struct cg_st_seg *s = cg_st_tx_seg(t, i);

			if (s->off + cg_st_span(s) > a->e[k])
				break;
			cg_st_tx_deliver(t, s, r);
		}
	}
}

/* A segment declared lost: *lq gets its bytes too if it was sent while
 * the path showed a queue (a loss by overflow, not the path's own). */
static inline uint32_t cg_st_tx_lose(struct cg_st_tx *t, struct cg_st_seg *s, uint32_t i, uint64_t *lq)
{
	uint32_t sp = cg_st_span(s);

	s->st = CG_ST_LOST;
	t->inflight -= sp;
	if (s->q && lq)
		*lq += sp;
	if (!t->nlost++ || i < t->lost_from)
		t->lost_from = i;
	return sp;
}

/* RACK: SENT segments sent more than reo_us before the newest one
 * delivered are lost. Returns the bytes marked (*lq: of them, sent with a
 * queue; may be NULL). */
static inline uint64_t cg_st_tx_rack(struct cg_st_tx *t, uint64_t reo_us, uint64_t *lq)
{
	uint64_t lost = 0;

	if (!t->rack_us || t->rack_us < reo_us)
		return 0;
	for (uint32_t i = 0; i < t->n; i++) {
		struct cg_st_seg *s = cg_st_tx_seg(t, i);

		if (s->st == CG_ST_SENT && s->sent_us + reo_us < t->rack_us)
			lost += cg_st_tx_lose(t, s, i, lq);
	}
	t->rack_lost += lost;
	return lost;
}

/* The timer fired at now: every SENT segment sent at least rto_us ago is
 * lost. Returns the bytes marked (*lq as cg_st_tx_rack's). */
static inline uint64_t cg_st_tx_rto(struct cg_st_tx *t, uint64_t now, uint64_t rto_us, uint64_t *lq)
{
	uint64_t lost = 0;

	for (uint32_t i = 0; i < t->n; i++) {
		struct cg_st_seg *s = cg_st_tx_seg(t, i);

		if (s->st == CG_ST_SENT && s->sent_us + rto_us <= now)
			lost += cg_st_tx_lose(t, s, i, lq);
	}
	t->rto_lost += lost;
	return lost;
}

/* Bytes the stream holds in flight or lost (what the timer guards). */
static inline int cg_st_tx_busy(const struct cg_st_tx *t)
{
	return t->inflight || t->nlost;
}

/* The oldest send time among SENT segments, 0: none. */
static inline uint64_t cg_st_tx_oldest(const struct cg_st_tx *t)
{
	uint64_t o = 0;

	for (uint32_t i = 0; i < t->n; i++) {
		const struct cg_st_seg *s = cg_st_tx_seg(t, i);

		if (s->st == CG_ST_SENT && (!o || s->sent_us < o))
			o = s->sent_us;
	}
	return o;
}

#endif
