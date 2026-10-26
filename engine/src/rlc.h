/* A sliding-window random linear code over GF(2^8) (story 021,
 * redund_spare = rlc; story 026, FEC v3: fec_code = rlc): each repair is a
 * random combination of recent source packets, its coefficients drawn
 * from a seed in its header (repair.h). The window slides with every
 * packet, so a loss is covered by the next repair, not by the end of a
 * block: for the same repairs per packet as a block code of k, a loss
 * waits about k/m packets for its repair instead of up to k (RFC 8681's
 * idea, here without its signalling).
 *
 * Two senders. The narrow one (struct cg_rlc_tx, redund_spare = rlc): the
 * last CG_RLC_WMAX packets that went on the first link alone, a repair over
 * the last w of them within CG_REP_SPAN sequences, its members in a 64-bit
 * map (kind CG_REP_RLC). The wide one (struct cg_rlcw_tx, FEC v3): every
 * DATA of the last fec_window_ms (up to CG_RLCW_SPAN sequences), a repair
 * over a range of them named by base and span (kind CG_REP_RLCW); a member
 * is in it when the low byte of its hash is at most the density dn, so a
 * repair combines about CG_RLCW_DMAX packets whatever the window (its cost
 * stays bounded at 100 Mbit/s), and the members a loss falls in still
 * cover it many times over the window.
 *
 * One receiver (struct cg_rlc_rx) for both kinds: an incremental
 * Gauss-Jordan elimination over the unknowns (the members missing), kept
 * in reduced row echelon form at every step. A repair arrives: the
 * members the ring has are subtracted from its symbol at once, the rest
 * become columns (at most CG_RLC_COLS), the row is reduced by the pivots,
 * gets a pivot of its own and is eliminated from the other rows; a DATA
 * arrives that a column waits for: it leaves every row (its pivot row,
 * if any, takes another pivot). A row left with its pivot alone is a
 * packet: written into the ring (repair.h) and handed back. Every row
 * operation is a symbol's multiply-add (gf256.h, 16 bytes at a time where
 * it can): a stall of u packets costs about u^2 of them to undo, once. An
 * unknown more than CG_RLC_KEEP sequences behind the newest repair is
 * given up with the rows that hold it, and with every column taken the
 * oldest goes. Random coefficients over 2^8: a set of e equations over e
 * unknowns is singular with probability about 1/255 (the next repair
 * usually settles it).
 *
 * The parity asked for again (harq.h, story 023): the rank deficit of an
 * unknown is the free columns (no pivot) of its row; fresh combinations
 * of exactly those (cg_rlc_combine, kind CG_REP_RLC) settle it.
 *
 * Pure: no clock, no I/O, no allocation (the caller owns the memory).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_RLC_H
#define CG_RLC_H

#include <stdint.h>
#include <string.h>

#include "gf256.h"
#include "repair.h"

#define CG_RLC_WMAX 64  /* the narrow sender's window, at most (CG_REP_SPAN) */
#define CG_RLC_COLS 256 /* unknowns the receiver follows at once (and rows) */
#define CG_RLCW_DMAX 96 /* members of a wide repair, about: its density's target */

_Static_assert(CG_RLC_WMAX <= CG_REP_SPAN, "a window fits a repair's map");
_Static_assert(CG_RLC_COLS <= 32767, "columns and rows index in 16 bits");

/* The coefficient of the member at distance d from the base in a repair of
 * seed s (kind CG_REP_RLC): a hash of both, never 0. */
static inline uint8_t cg_rlc_coef(uint16_t s, unsigned d)
{
	uint32_t x = (uint32_t)s * 0x9e3779b1u ^ (d + 1) * 0x85ebca77u;
	uint8_t c;

	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 12;
	x *= 0x297a2d39u;
	x ^= x >> 15;
	c = (uint8_t)(x >> 24);
	return c ? c : 1;
}

/* A wide repair (kind CG_REP_RLCW) of seed s and density dn: whether the
 * member at distance d from its base is in it, and its coefficient (never
 * 0) in *c. */
static inline int cg_rlcw_member(uint32_t s, uint8_t dn, unsigned d, uint8_t *c)
{
	uint32_t x = s * 0x9e3779b1u ^ (d + 1) * 0x85ebca77u;

	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	*c = (uint8_t)(x >> 24) ? (uint8_t)(x >> 24) : 1;
	return (uint8_t)x <= dn;
}

/* The density for a window of span sequences: every member up to
 * CG_RLCW_DMAX, else about CG_RLCW_DMAX of them. */
static inline uint8_t cg_rlcw_density(unsigned span)
{
	return span <= CG_RLCW_DMAX ? 255 : (uint8_t)(CG_RLCW_DMAX * 256u / span - 1);
}

/* ---- the narrow sender (redund_spare = rlc) ---- */

struct cg_rlc_tx {
	uint32_t seq[CG_RLC_WMAX];
	uint16_t len[CG_RLC_WMAX];
	unsigned head, n;   /* the next slot, packets kept */
	uint16_t seed;      /* the next repair's */
	uint32_t credit;    /* repairs owed, in 1/256 */
	unsigned fresh;     /* packets since the last repair */
	uint64_t fresh_us;  /* when the first of them went */
	uint8_t p[CG_RLC_WMAX][CG_SYM_PLEN];
};

/* A packet (seq, len bytes at p) went on the first link alone. */
static inline void cg_rlc_tx_add(struct cg_rlc_tx *t, uint32_t seq, const uint8_t *p, uint16_t len, uint64_t now_us)
{
	unsigned i = t->head;

	if (len > CG_SYM_PLEN)
		len = CG_SYM_PLEN;
	t->seq[i] = seq;
	t->len[i] = len;
	memcpy(t->p[i], p, len);
	t->head = (i + 1) % CG_RLC_WMAX;
	if (t->n < CG_RLC_WMAX)
		t->n++;
	if (!t->fresh++)
		t->fresh_us = now_us;
}

/* Repairs owed after a packet at rate256 (repairs per packet, in 1/256):
 * the credit grows by it, each whole one is a repair. */
static inline unsigned cg_rlc_tx_owed(struct cg_rlc_tx *t, unsigned rate256)
{
	unsigned n;

	t->credit += rate256;
	n = t->credit / 256;
	t->credit %= 256;
	return n;
}

/* A repair over the last w packets (at most CG_RLC_WMAX) within
 * CG_REP_SPAN sequences of the newest: its header in *h, its symbol into
 * out (CG_SYM_MAX bytes); returns the symbol's length, 0: nothing kept. */
static inline unsigned cg_rlc_tx_repair(const struct cg_gf *g, struct cg_rlc_tx *t, unsigned w, struct cg_rep_hdr *h,
					uint8_t *out)
{
	unsigned n = 0, slen = 0, newest;
	uint32_t base;

	if (!t->n || !w)
		return 0;
	if (w > t->n)
		w = t->n;
	newest = (t->head + CG_RLC_WMAX - 1) % CG_RLC_WMAX;
	base = t->seq[newest];
	/* back from the newest while within the span */
	for (unsigned j = 0; j < w; j++) {
		unsigned i = (newest + CG_RLC_WMAX - j) % CG_RLC_WMAX;

		if (t->seq[newest] - t->seq[i] >= CG_REP_SPAN)
			break;
		base = t->seq[i];
		n++;
	}
	memset(h, 0, sizeof(*h));
	h->base = base;
	h->kind = CG_REP_RLC;
	h->idx = t->seed++;
	for (unsigned j = 0; j < n; j++) {
		unsigned i = (newest + CG_RLC_WMAX - j) % CG_RLC_WMAX;

		if ((unsigned)t->len[i] + 2 > slen) {
			memset(out + slen, 0, t->len[i] + 2 - slen);
			slen = t->len[i] + 2u;
		}
	}
	for (unsigned j = 0; j < n; j++) {
		unsigned i = (newest + CG_RLC_WMAX - j) % CG_RLC_WMAX, d = t->seq[i] - base;

		h->map |= 1ull << d;
		cg_gf_madd_sym(g, out, t->p[i], t->len[i], cg_rlc_coef(h->idx, d));
	}
	t->fresh = 0;
	return slen;
}

/* ---- the wide sender (FEC v3, story 026) ---- */

/* Every DATA sent lately, by sequence (a run without holes up to the
 * newest), with when it went; about 2 MB, allocated once. */
struct cg_rlcw_tx {
	uint32_t newest; /* the newest sequence kept */
	unsigned n;      /* kept: newest - n + 1 to newest (at most CG_RLCW_SPAN) */
	uint32_t seed;   /* the next repair's */
	uint16_t len[CG_RLCW_SPAN];
	uint64_t t_us[CG_RLCW_SPAN];
	uint8_t p[CG_RLCW_SPAN][CG_SYM_PLEN];
};

/* DATA seq (len bytes at p) went at now_us. A sequence that does not
 * follow the newest starts the run over. */
static inline void cg_rlcw_add(struct cg_rlcw_tx *t, uint32_t seq, const uint8_t *p, uint16_t len, uint64_t now_us)
{
	unsigned i = seq & (CG_RLCW_SPAN - 1);

	if (len > CG_SYM_PLEN)
		len = CG_SYM_PLEN;
	if (t->n && seq == t->newest + 1)
		t->n += t->n < CG_RLCW_SPAN;
	else if (!t->n || seq != t->newest)
		t->n = 1;
	t->newest = seq;
	t->len[i] = len;
	t->t_us[i] = now_us;
	memcpy(t->p[i], p, len);
}

/* DATA seq if still kept (its length in *len), else NULL. */
static inline const uint8_t *cg_rlcw_get(const struct cg_rlcw_tx *t, uint32_t seq, uint16_t *len)
{
	uint32_t back = t->newest - seq;

	if (!t->n || back >= t->n)
		return NULL;
	*len = t->len[seq & (CG_RLCW_SPAN - 1)];
	return t->p[seq & (CG_RLCW_SPAN - 1)];
}

/* The window: the newest sequence sent at end_us or before (its top, in
 * *top), and how many up to it went at start_us or after (at most max);
 * 0 when none. A repair's window ends a little before now (end_us): the
 * newest DATA on links slower than the repair's would arrive after it, and
 * each would cost the receiver a column for nothing (fec3.h's lag). */
static inline unsigned cg_rlcw_window(const struct cg_rlcw_tx *t, uint64_t start_us, uint64_t end_us, unsigned max,
				      uint32_t *top)
{
	unsigned lo = 0, hi = t->n, back, w; /* back: the fewest of the newest to skip, in [lo, hi] */

	if (!t->n)
		return 0;
	/* the times grow with the sequence: skip the newest sent after end_us */
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;

		if (t->t_us[(t->newest - mid) & (CG_RLCW_SPAN - 1)] <= end_us)
			hi = mid;
		else
			lo = mid + 1;
	}
	back = lo;
	if (back == t->n)
		return 0;
	*top = t->newest - back;
	/* then the longest run back from the top sent at start_us or after */
	lo = 0;
	hi = t->n - back < max ? t->n - back : max;
	while (lo < hi) {
		unsigned mid = (lo + hi + 1) / 2;

		if (t->t_us[(*top - (mid - 1)) & (CG_RLCW_SPAN - 1)] >= start_us)
			lo = mid;
		else
			hi = mid - 1;
	}
	w = lo;
	return w;
}

/* A repair over the w sequences up to top (cg_rlcw_window; within what is
 * kept): its header in *h, its symbol into out (CG_SYM_MAX bytes); returns
 * the symbol's length, 0: nothing to combine. */
static inline unsigned cg_rlcw_repair(const struct cg_gf *g, struct cg_rlcw_tx *t, uint32_t top, unsigned w,
				      struct cg_rep_hdr *h, uint8_t *out)
{
	uint16_t d[CG_RLCW_SPAN];
	uint8_t c[CG_RLCW_SPAN], dn;
	unsigned k = 0, slen = 0;
	uint32_t base, s = 0, back = t->newest - top;

	if (!t->n || back >= t->n)
		return 0;
	if (w > t->n - back)
		w = t->n - back;
	if (!w)
		return 0;
	base = top - w + 1;
	dn = cg_rlcw_density(w);
	/* a seed that picks nobody (a small window, a low density) is skipped */
	for (int tries = 0; !k && tries < 16; tries++) {
		s = t->seed++;
		for (unsigned j = 0; j < w; j++)
			if (cg_rlcw_member(s, dn, j, &c[k]))
				d[k++] = (uint16_t)j;
	}
	if (!k)
		return 0;
	for (unsigned j = 0; j < k; j++) {
		unsigned l = t->len[(base + d[j]) & (CG_RLCW_SPAN - 1)] + 2u;

		if (l > slen)
			slen = l;
	}
	memset(out, 0, slen);
	for (unsigned j = 0; j < k; j++) {
		unsigned i = (base + d[j]) & (CG_RLCW_SPAN - 1);

		cg_gf_madd_sym(g, out, t->p[i], t->len[i], c[j]);
	}
	memset(h, 0, sizeof(*h));
	h->base = base;
	h->kind = CG_REP_RLCW;
	h->span = (uint16_t)w;
	h->dn = dn;
	h->seed = s;
	return slen;
}

/* ---- the receiver ---- */

/* Columns are the unknowns, rows the equations over them, in reduced row
 * echelon form: each row has a pivot column (coefficient 1) that every
 * other row has 0 in. Indices are kept plus one, so that a zeroed
 * structure is an empty receiver. About 600 KB. */
struct cg_rlc_rx {
	uint32_t col_seq[CG_RLC_COLS];
	uint16_t col_piv[CG_RLC_COLS];  /* 1 + its row when a pivot, 0: free */
	uint8_t col_used[CG_RLC_COLS];
	uint16_t slot_col[CG_REP_RING]; /* per ring slot: 1 + the column of a sequence there */
	uint16_t row_piv[CG_RLC_COLS];  /* 1 + its pivot column, 0: the row is unused */
	uint16_t row_nnz[CG_RLC_COLS];  /* its nonzero coefficients */
	uint16_t slen[CG_RLC_COLS];
	uint16_t ncol, nrow, hicol;     /* used columns and rows; 1 + the highest column used */
	uint32_t newest;                /* the newest member of a repair taken */
	uint8_t have_newest;
	uint8_t more;                   /* rows settled that out had no room for */
	uint8_t c[CG_RLC_COLS][CG_RLC_COLS];
	uint8_t x[CG_RLC_COLS][CG_SYM_MAX];
	uint8_t nc[CG_RLC_COLS];        /* the row being built */
	uint8_t nx[CG_SYM_MAX];
	/* repairs taken; with nothing missing (or too old); kept as a row;
	 * packets rebuilt; rows let go (too old, evicted, cleared); a member
	 * longer than its repair or a rebuilt symbol whose length did not fit;
	 * repairs that added nothing (dependent); wide ones; unknowns given up
	 * for room; the most unknowns at once */
	uint64_t repairs, useless, kept, recovered, dropped, bad, dependent, wide, evicted, peak;
};

/* The column waiting for seq, -1: none. */
static inline int cg_rlc_col_of(const struct cg_rlc_rx *r, uint32_t seq)
{
	unsigned k = r->slot_col[seq & (CG_REP_RING - 1)];

	return k && r->col_used[k - 1] && r->col_seq[k - 1] == seq ? (int)k - 1 : -1;
}

/* Whether a repair taken misses seq (it waits for it). */
static inline int cg_rlc_rx_wants(const struct cg_rlc_rx *r, uint32_t seq)
{
	return cg_rlc_col_of(r, seq) >= 0;
}

static inline void cg_rlc_nnz(struct cg_rlc_rx *r, unsigned i)
{
	unsigned n = 0;

	for (unsigned j = 0; j < r->hicol; j++)
		n += r->c[i][j] != 0;
	r->row_nnz[i] = (uint16_t)n;
}

/* Row i ^= f x row k, coefficients and symbol. */
static inline void cg_rlc_axpy(const struct cg_gf *g, struct cg_rlc_rx *r, unsigned i, unsigned k, uint8_t f)
{
	cg_gf_madd(g, r->c[i], r->c[k], f, r->hicol);
	if (r->slen[k] > r->slen[i]) {
		memset(r->x[i] + r->slen[i], 0, (size_t)(r->slen[k] - r->slen[i]));
		r->slen[i] = r->slen[k];
	}
	cg_gf_madd(g, r->x[i], r->x[k], f, r->slen[k]);
	cg_rlc_nnz(r, i);
}

static inline void cg_rlc_col_free(struct cg_rlc_rx *r, unsigned j)
{
	uint32_t ix = r->col_seq[j] & (CG_REP_RING - 1);

	if (r->slot_col[ix] == j + 1)
		r->slot_col[ix] = 0;
	r->col_used[j] = 0;
	r->col_piv[j] = 0;
	r->ncol--;
	while (r->hicol && !r->col_used[r->hicol - 1])
		r->hicol--;
}

static inline void cg_rlc_row_drop(struct cg_rlc_rx *r, unsigned i)
{
	if (!r->row_piv[i])
		return;
	r->col_piv[r->row_piv[i] - 1] = 0;
	r->row_piv[i] = 0;
	r->nrow--;
	r->dropped++;
}

/* Columns no row holds any more are let go, but those of keep[0..nkeep)
 * (a repair's, being taken). */
static inline void cg_rlc_gc(struct cg_rlc_rx *r, const uint32_t *keep, unsigned nkeep)
{
	for (unsigned j = 0; j < r->hicol; j++) {
		int held = 0;

		if (!r->col_used[j] || r->col_piv[j])
			continue;
		for (unsigned i = 0; i < CG_RLC_COLS && !held; i++)
			held = r->row_piv[i] && r->c[i][j];
		for (unsigned m = 0; m < nkeep && !held; m++)
			held = keep[m] == r->col_seq[j];
		if (!held)
			cg_rlc_col_free(r, j);
	}
}

/* Unknown j given up: the rows that hold it go with it, and the columns
 * they leave unheld (but keep's). */
static inline void cg_rlc_evict(struct cg_rlc_rx *r, unsigned j, const uint32_t *keep, unsigned nkeep)
{
	for (unsigned i = 0; i < CG_RLC_COLS; i++)
		if (r->row_piv[i] && r->c[i][j])
			cg_rlc_row_drop(r, i);
	cg_rlc_col_free(r, j);
	cg_rlc_gc(r, keep, nkeep);
}

/* Every row and column let go (counted dropped): a rebuilt packet the
 * guard refused (rebuilt.h) may have been subtracted from them already. */
static inline void cg_rlc_rx_clear(struct cg_rlc_rx *r)
{
	for (unsigned i = 0; i < CG_RLC_COLS; i++)
		cg_rlc_row_drop(r, i);
	for (unsigned j = 0; j < CG_RLC_COLS; j++)
		if (r->col_used[j])
			cg_rlc_col_free(r, j);
}

/* Row p has no pivot (its own came in): it takes its first nonzero, is
 * scaled to 1 there and eliminated from the other rows; a row without
 * any is satisfied and goes (not counted dropped). */
static inline void cg_rlc_repivot(const struct cg_gf *g, struct cg_rlc_rx *r, unsigned p)
{
	unsigned q = 0;

	while (q < r->hicol && !r->c[p][q])
		q++;
	if (q == r->hicol) {
		r->row_piv[p] = 0;
		r->nrow--;
		return;
	}
	{
		uint8_t iv = g->inv[r->c[p][q]];

		cg_gf_scale(g, r->c[p], iv, r->hicol);
		cg_gf_scale(g, r->x[p], iv, r->slen[p]);
	}
	for (unsigned i = 0; i < CG_RLC_COLS; i++)
		if (i != p && r->row_piv[i] && r->c[i][q])
			cg_rlc_axpy(g, r, i, p, r->c[i][q]);
	r->row_piv[p] = (uint16_t)(q + 1);
	r->col_piv[q] = (uint16_t)(p + 1);
	cg_rlc_nnz(r, p);
}

/* The rows left with their pivot alone are packets: into the ring, their
 * sequences into out (nout at most; the rest wait for the next call). */
static inline unsigned cg_rlc_emit(struct cg_rlc_rx *r, struct cg_rep_ring *ring, uint32_t *out, unsigned nout)
{
	unsigned got = 0;

	r->more = 0;
	for (unsigned i = 0; i < CG_RLC_COLS; i++) {
		unsigned q;
		int rc;

		if (!r->row_piv[i] || r->row_nnz[i] != 1)
			continue;
		if (got == nout) {
			r->more = 1;
			break;
		}
		q = r->row_piv[i] - 1u;
		rc = cg_rep_put_sym(ring, r->col_seq[q], r->x[i], r->slen[i]);
		r->row_piv[i] = 0;
		r->nrow--;
		if (rc < 0) {
			if (rc == -1)
				r->bad++;
			else
				r->dropped++;
		} else {
			out[got++] = r->col_seq[q];
			r->recovered++;
		}
		cg_rlc_col_free(r, q);
	}
	return got;
}

/* The row in nc/nx (slen bytes of symbol) into the system: reduced by the
 * pivots, given a pivot of its own (none: it added nothing) and eliminated
 * from the rest; what that settles into the ring and out. */
static inline unsigned cg_rlc_insert(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring,
				     unsigned slen, uint32_t *out, unsigned nout)
{
	unsigned q = CG_RLC_COLS, i = 0;
	uint8_t iv;

	for (unsigned j = 0; j < r->hicol; j++) {
		uint8_t f = r->nc[j];
		unsigned k;

		if (!f || !r->col_piv[j])
			continue;
		k = r->col_piv[j] - 1u;
		cg_gf_madd(g, r->nc, r->c[k], f, r->hicol);
		if (r->slen[k] > slen) {
			memset(r->nx + slen, 0, r->slen[k] - slen);
			slen = r->slen[k];
		}
		cg_gf_madd(g, r->nx, r->x[k], f, r->slen[k]);
	}
	for (unsigned j = 0; j < r->hicol && q == CG_RLC_COLS; j++)
		if (r->nc[j])
			q = j;
	if (q == CG_RLC_COLS) {
		r->dependent++; /* (its columns, all old ones: a new one has no pivot to cancel it) */
		return 0;
	}
	while (i < CG_RLC_COLS && r->row_piv[i])
		i++; /* a free row: rows have distinct pivots, so fewer than the columns used (q has none) */
	if (i == CG_RLC_COLS)
		return 0;
	iv = g->inv[r->nc[q]];
	memcpy(r->c[i], r->nc, r->hicol);
	cg_gf_scale(g, r->c[i], iv, r->hicol);
	memcpy(r->x[i], r->nx, slen);
	cg_gf_scale(g, r->x[i], iv, slen);
	r->slen[i] = (uint16_t)slen;
	r->row_piv[i] = (uint16_t)(q + 1);
	r->nrow++;
	r->kept++;
	for (unsigned k = 0; k < CG_RLC_COLS; k++)
		if (k != i && r->row_piv[k] && r->c[k][q])
			cg_rlc_axpy(g, r, k, i, r->c[k][q]);
	r->col_piv[q] = (uint16_t)(i + 1);
	cg_rlc_nnz(r, i);
	return cg_rlc_emit(r, ring, out, nout);
}

/* A column for seq, not one of keep[0..nkeep) evicted for room: its index,
 * or -1 when none could be made. */
static inline int cg_rlc_col_new(struct cg_rlc_rx *r, uint32_t seq, const uint32_t *keep, unsigned nkeep)
{
	unsigned j = 0;

	if (r->ncol == CG_RLC_COLS) {
		/* the oldest unknown not of this repair goes */
		int old = -1;

		for (unsigned k = 0; k < CG_RLC_COLS; k++) {
			int mine = 0;

			if (!r->col_used[k])
				continue;
			for (unsigned m = 0; m < nkeep && !mine; m++)
				mine = keep[m] == r->col_seq[k];
			if (!mine && (old < 0 || (int32_t)(r->col_seq[k] - r->col_seq[old]) < 0))
				old = (int)k;
		}
		if (old < 0)
			return -1;
		cg_rlc_evict(r, (unsigned)old, keep, nkeep);
		r->evicted++;
	}
	while (j < CG_RLC_COLS && r->col_used[j])
		j++;
	if (j == CG_RLC_COLS)
		return -1;
	r->col_used[j] = 1;
	r->col_seq[j] = seq;
	r->col_piv[j] = 0;
	r->slot_col[seq & (CG_REP_RING - 1)] = (uint16_t)(j + 1);
	r->ncol++;
	for (unsigned i = 0; i < CG_RLC_COLS; i++)
		r->c[i][j] = 0; /* every row's new coefficient is 0 */
	if (j + 1 > r->hicol)
		r->hicol = (uint16_t)(j + 1);
	if (r->ncol > r->peak)
		r->peak = r->ncol;
	return (int)j;
}

/* Whether seq is too old to wait for: more than CG_RLC_KEEP behind the
 * newest member of a repair. */
static inline int cg_rlc_stale(const struct cg_rlc_rx *r, uint32_t seq)
{
	return r->have_newest && (int32_t)(r->newest - seq) > CG_RLC_KEEP;
}

/* A repair (header h, slen bytes of symbol at s), of either kind: kept as
 * a row if members are missing, and what it settles rebuilt into the ring,
 * their sequences into out (CG_RLC_COLS is enough); returns how many. */
static inline unsigned cg_rlc_rx_repair(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring,
					const struct cg_rep_hdr *h, const uint8_t *s, unsigned slen, uint32_t *out,
					unsigned nout)
{
	uint32_t mseq[CG_RLCW_SPAN], miss[CG_RLC_COLS];
	uint8_t mc[CG_RLCW_SPAN];
	unsigned nm = 0, nmiss = 0;
	uint32_t top;

	r->repairs++;
	if (h->kind == CG_REP_RLCW) {
		unsigned d = 0;

		r->wide++;
		/* the common case first: nothing missing in its range, nothing
		 * to hash (a repair a packet in ten, on every DATA a ring look) */
		while (d < h->span && cg_rep_has(ring, h->base + d))
			d++;
		if (d == h->span) {
			r->useless++;
			return 0;
		}
		for (d = 0; d < h->span && d < CG_RLCW_SPAN; d++)
			if (cg_rlcw_member(h->seed, h->dn, d, &mc[nm]))
				mseq[nm++] = h->base + d;
	} else {
		for (uint64_t m = h->map; m; m &= m - 1) {
			unsigned d = (unsigned)__builtin_ctzll(m);

			mc[nm] = cg_rlc_coef(h->idx, d);
			mseq[nm++] = h->base + d;
		}
	}
	if (!nm) {
		r->useless++;
		return 0;
	}
	top = mseq[nm - 1];
	if (cg_rlc_stale(r, top)) {
		r->useless++;
		return 0;
	}
	if (!r->have_newest || (int32_t)(top - r->newest) > 0) {
		r->newest = top;
		r->have_newest = 1;
		/* let go of what is too old to come */
		for (unsigned j = 0; j < r->hicol; j++)
			if (r->col_used[j] && cg_rlc_stale(r, r->col_seq[j]))
				cg_rlc_evict(r, j, NULL, 0);
	}
	for (unsigned k = 0; k < nm; k++) {
		if (cg_rep_has(ring, mseq[k]))
			continue;
		if (cg_rlc_stale(r, mseq[k]) || nmiss == CG_RLC_COLS) {
			r->useless++; /* a member given up already: nothing to settle */
			return 0;
		}
		miss[nmiss++] = mseq[k];
	}
	if (!nmiss) {
		r->useless++;
		return 0;
	}
	/* the columns first (one made may evict an older one, never one of
	 * these), then the row */
	for (unsigned k = 0; k < nmiss; k++)
		if (cg_rlc_col_of(r, miss[k]) < 0 && cg_rlc_col_new(r, miss[k], miss, nmiss) < 0) {
			r->dropped++;
			return 0;
		}
	memset(r->nc, 0, sizeof(r->nc));
	memcpy(r->nx, s, slen);
	for (unsigned k = 0; k < nm; k++) {
		int j = cg_rlc_col_of(r, mseq[k]);

		if (j >= 0) {
			r->nc[j] = mc[k];
		} else {
			uint16_t len;
			const uint8_t *p = cg_rep_data(ring, mseq[k], &len);

			if ((unsigned)len + 2 > slen) {
				r->bad++; /* a member longer than the repair */
				cg_rlc_gc(r, NULL, 0);
				return 0;
			}
			cg_gf_madd_sym(g, r->nx, p, len, mc[k]);
		}
	}
	return cg_rlc_insert(g, r, ring, slen, out, nout);
}

/* DATA seq went into the ring (arrived, or rebuilt): out of every row,
 * and what that settles rebuilt (into out, as cg_rlc_rx_repair). */
static inline unsigned cg_rlc_rx_data(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring, uint32_t seq,
				      uint32_t *out, unsigned nout)
{
	int j = cg_rlc_col_of(r, seq), gone = 0;
	uint16_t len;
	const uint8_t *p;
	unsigned piv;

	if (j < 0)
		return r->more ? cg_rlc_emit(r, ring, out, nout) : 0; /* what an earlier call could not hand back */
	p = cg_rep_data(ring, seq, &len);
	piv = r->col_piv[j];
	for (unsigned i = 0; i < CG_RLC_COLS; i++) {
		uint8_t f;

		if (!r->row_piv[i] || !(f = r->c[i][j]))
			continue;
		if ((unsigned)len + 2 > r->slen[i]) {
			r->bad++; /* longer than the repair: it cannot be a member */
			cg_rlc_row_drop(r, i);
			gone = 1;
			continue;
		}
		cg_gf_madd_sym(g, r->x[i], p, len, f);
		r->c[i][j] = 0;
		r->row_nnz[i]--;
	}
	/* every row has 0 there now: the column goes; its pivot row, if
	 * still there, takes another pivot */
	cg_rlc_col_free(r, (unsigned)j);
	if (piv && r->row_piv[piv - 1]) {
		cg_rlc_nnz(r, piv - 1);
		gone |= !r->row_nnz[piv - 1];
		cg_rlc_repivot(g, r, piv - 1);
	}
	if (gone)
		cg_rlc_gc(r, NULL, 0);
	return cg_rlc_emit(r, ring, out, nout);
}

/* ---- the parity asked for again (harq.h, story 023) ---- */

/* A combination over members given by the caller (the sender's rescue
 * ring or its window): seed, and for each bit d of map (from base) the
 * payload p[i] of len[i] bytes, i counting the bits in order; its symbol
 * into out (CG_SYM_MAX bytes), its header in *h; the symbol's length. */
static inline unsigned cg_rlc_combine(const struct cg_gf *g, uint32_t base, uint64_t map, uint16_t seed,
				      const uint8_t *const *p, const uint16_t *len, struct cg_rep_hdr *h, uint8_t *out)
{
	unsigned slen = 0, i = 0;

	for (uint64_t m = map; m; m &= m - 1, i++)
		if ((unsigned)len[i] + 2 > slen)
			slen = len[i] + 2u;
	memset(out, 0, slen);
	i = 0;
	for (uint64_t m = map; m; m &= m - 1, i++)
		cg_gf_madd_sym(g, out, p[i], len[i], cg_rlc_coef(seed, (unsigned)__builtin_ctzll(m)));
	memset(h, 0, sizeof(*h));
	h->base = base;
	h->map = map;
	h->kind = CG_REP_RLC;
	h->idx = seed;
	return slen;
}

/* The rank deficit of seq: the unknowns fresh combinations of which settle
 * it, in *base and *map (within CG_REP_SPAN of *base), and how many. When
 * seq is a pivot, the free columns of its row (the oldest of them and
 * those within CG_REP_SPAN after it; any beyond, a later ask); when it is
 * free and some row holds it, seq itself. 0 when no row misses seq (the
 * packet, not the code, answers it). */
static inline unsigned cg_rlc_rx_need(const struct cg_gf *g, const struct cg_rlc_rx *r, uint32_t seq, uint32_t *base,
				      uint64_t *map)
{
	int j = cg_rlc_col_of(r, seq);
	unsigned p;
	uint32_t lo;
	int any = 0;

	(void)g;
	if (j < 0)
		return 0;
	if (!r->col_piv[j]) {
		for (unsigned i = 0; i < CG_RLC_COLS; i++)
			if (r->row_piv[i] && r->c[i][j]) {
				*base = seq;
				*map = 1;
				return 1;
			}
		return 0;
	}
	p = r->col_piv[j] - 1u;
	lo = seq;
	for (unsigned k = 0; k < r->hicol; k++)
		if ((int)k != j && r->c[p][k] && (!any || (int32_t)(r->col_seq[k] - lo) < 0)) {
			lo = r->col_seq[k];
			any = 1;
		}
	if (!any)
		return 0; /* settled: it is about to be handed back */
	*base = lo;
	*map = 0;
	for (unsigned k = 0; k < r->hicol; k++)
		if ((int)k != j && r->c[p][k] && r->col_seq[k] - lo < CG_REP_SPAN)
			*map |= 1ull << (r->col_seq[k] - lo);
	return (unsigned)__builtin_popcountll(*map);
}

#endif
