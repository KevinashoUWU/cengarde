/* A sliding-window random linear code over GF(2^8) (story 021,
 * redund_spare = rlc): each repair is a random combination of the last W
 * source packets (DATA that went on the first link alone), its
 * coefficients drawn from a 16-bit seed in its header (repair.h). The
 * window slides with every packet, so a loss is covered by the next
 * repair, not by the end of a block: for the same repairs per packet as a
 * block code of k, a loss waits about k/m packets for its repair instead of
 * up to k (RFC 8681's idea, here without its signalling).
 *
 * Sender (struct cg_rlc_tx): the last CG_RLC_WMAX packets, copied; a
 * repair over the last w of them that fit CG_REP_SPAN sequences costs w
 * multiply-adds of a symbol. Receiver (struct cg_rlc_rx): each repair with
 * members missing is kept as an equation over them (the members it has
 * subtracted at once, the rest as they arrive), and on each change the
 * equations are eliminated (coefficients only, at most CG_RLC_EQ of them
 * over CG_RLC_UNK unknowns); each unknown left alone in a row is rebuilt
 * from the payloads of the rows that made it, written into the ring
 * (repair.h) and subtracted from the rest. Random coefficients over 2^8:
 * a set of e equations over e unknowns is singular with probability about
 * 1/255 (the next repair usually settles it).
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

#define CG_RLC_WMAX 64 /* the sender's window, at most (CG_REP_SPAN) */
#define CG_RLC_EQ 32   /* equations the receiver keeps */
#define CG_RLC_UNK 96  /* unknowns in one elimination */

_Static_assert(CG_RLC_WMAX <= CG_REP_SPAN, "a window fits a repair's map");

/* The coefficient of the member at distance d from the base in a repair of
 * seed s: a hash of both, never 0. */
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
	h->base = base;
	h->map = 0;
	h->kind = CG_REP_RLC;
	h->m = 0;
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

/* One kept repair: an equation over the members still missing. */
struct cg_rlc_eq {
	uint32_t base;
	uint64_t miss;          /* members missing, bit d: base + d */
	uint8_t c[CG_REP_SPAN]; /* their coefficients */
	uint16_t slen;
	uint8_t used;
	uint32_t born;          /* the order it came in, for eviction */
	uint8_t x[CG_SYM_MAX];  /* the repair less the members it has */
};

struct cg_rlc_rx {
	struct cg_rlc_eq eq[CG_RLC_EQ];
	uint8_t want[CG_REP_RING]; /* per ring entry: equations missing a sequence there */
	uint32_t newest, born;
	uint8_t have_newest;
	uint64_t repairs, useless, recovered, dropped, bad, kept;
};

static inline void cg_rlc_eq_free(struct cg_rlc_rx *r, struct cg_rlc_eq *e)
{
	for (uint64_t m = e->miss; m; m &= m - 1)
		r->want[(e->base + (unsigned)__builtin_ctzll(m)) & (CG_REP_RING - 1)]--;
	e->miss = 0;
	e->used = 0;
}

/* DATA seq is in the ring now (arrived, or rebuilt): out of every
 * equation missing it. Returns whether one changed. */
static inline int cg_rlc_rx_sub(const struct cg_gf *g, struct cg_rlc_rx *r, const struct cg_rep_ring *ring, uint32_t seq)
{
	uint32_t ix = seq & (CG_REP_RING - 1);
	int changed = 0;

	if (!r->want[ix])
		return 0;
	for (unsigned k = 0; k < CG_RLC_EQ; k++) {
		struct cg_rlc_eq *e = &r->eq[k];
		uint32_t d = seq - e->base;
		uint16_t len;
		const uint8_t *p;

		if (!e->used || d >= CG_REP_SPAN || !(e->miss >> d & 1))
			continue;
		p = cg_rep_data(ring, seq, &len);
		if ((unsigned)len + 2 <= e->slen)
			cg_gf_madd_sym(g, e->x, p, len, e->c[d]);
		else
			r->bad++; /* longer than the repair: it cannot be a member; the
				     equation is let go below */
		e->miss &= ~(1ull << d);
		r->want[ix]--;
		changed = 1;
		if ((unsigned)len + 2 > e->slen) {
			cg_rlc_eq_free(r, e);
			continue;
		}
		if (!e->miss)
			cg_rlc_eq_free(r, e);
	}
	return changed;
}

/* Eliminates the equations and rebuilds what they settle into the ring;
 * their sequences into out (nout at most); returns how many. */
static inline unsigned cg_rlc_rx_solve(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring,
				       uint32_t *out, unsigned nout)
{
	uint8_t a[CG_RLC_EQ][CG_RLC_UNK], t[CG_RLC_EQ][CG_RLC_EQ], sym[CG_SYM_MAX];
	uint32_t col[CG_RLC_UNK];
	int row_eq[CG_RLC_EQ], piv[CG_RLC_EQ];
	unsigned n = 0, u = 0, rank = 0, got = 0;

	for (unsigned k = 0; k < CG_RLC_EQ; k++) {
		struct cg_rlc_eq *e = &r->eq[k];
		unsigned need = 0;

		if (!e->used)
			continue;
		/* its unknowns' columns; an equation that would overflow them waits */
		for (uint64_t m = e->miss; m; m &= m - 1) {
			uint32_t s = e->base + (unsigned)__builtin_ctzll(m);
			unsigned j = 0;

			while (j < u && col[j] != s)
				j++;
			need += j == u;
		}
		if (u + need > CG_RLC_UNK)
			continue;
		memset(a[n], 0, sizeof(a[n]));
		for (uint64_t m = e->miss; m; m &= m - 1) {
			unsigned d = (unsigned)__builtin_ctzll(m), j = 0;
			uint32_t s = e->base + d;

			while (j < u && col[j] != s)
				j++;
			if (j == u)
				col[u++] = s;
			a[n][j] = e->c[d];
		}
		memset(t[n], 0, sizeof(t[n]));
		row_eq[n] = (int)k;
		n++;
	}
	for (unsigned i = 0; i < n; i++)
		t[i][i] = 1;
	/* reduced row echelon form, t the rows' combinations */
	for (unsigned c = 0; c < u && rank < n; c++) {
		unsigned p = rank;
		uint8_t iv;

		while (p < n && !a[p][c])
			p++;
		if (p == n)
			continue;
		if (p != rank) {
			uint8_t tmp[CG_RLC_UNK > CG_RLC_EQ ? CG_RLC_UNK : CG_RLC_EQ];
			int te = row_eq[p];

			memcpy(tmp, a[p], u);
			memcpy(a[p], a[rank], u);
			memcpy(a[rank], tmp, u);
			memcpy(tmp, t[p], n);
			memcpy(t[p], t[rank], n);
			memcpy(t[rank], tmp, n);
			row_eq[p] = row_eq[rank];
			row_eq[rank] = te;
		}
		iv = g->inv[a[rank][c]];
		cg_gf_scale(g, a[rank], iv, u);
		cg_gf_scale(g, t[rank], iv, n);
		for (unsigned i = 0; i < n; i++)
			if (i != rank && a[i][c]) {
				uint8_t f = a[i][c];

				cg_gf_madd(g, a[i], a[rank], f, u);
				cg_gf_madd(g, t[i], t[rank], f, n);
			}
		piv[rank++] = (int)c;
	}
	/* a row whose one nonzero is its pivot settles that unknown */
	for (unsigned i = 0; i < rank && got < nout; i++) {
		unsigned nz = 0, slen = 0;

		for (unsigned c = 0; c < u; c++)
			nz += a[i][c] != 0;
		if (nz != 1)
			continue;
		for (unsigned s = 0; s < n; s++)
			if (t[i][s] && r->eq[row_eq[s]].slen > slen)
				slen = r->eq[row_eq[s]].slen;
		memset(sym, 0, slen);
		for (unsigned s = 0; s < n; s++)
			if (t[i][s])
				cg_gf_madd(g, sym, r->eq[row_eq[s]].x, t[i][s], r->eq[row_eq[s]].slen);
		if (cg_rep_put_sym(ring, col[piv[i]], sym, slen) < 0) {
			r->bad++;
			continue;
		}
		out[got++] = col[piv[i]];
		r->recovered++;
	}
	/* the rebuilt are known now: out of every equation (after the loop:
	 * the rows above read the equations as they were) */
	for (unsigned i = 0; i < got; i++)
		cg_rlc_rx_sub(g, r, ring, out[i]);
	return got;
}

/* A repair (header h, slen bytes of symbol at s): kept as an equation if
 * members are missing, and what it settles rebuilt into the ring, their
 * sequences into out; returns how many. */
static inline unsigned cg_rlc_rx_repair(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring,
					const struct cg_rep_hdr *h, const uint8_t *s, unsigned slen, uint32_t *out,
					unsigned nout)
{
	struct cg_rlc_eq *e = NULL;
	uint64_t miss;

	r->repairs++;
	if (!r->have_newest || (int32_t)(h->base - r->newest) > 0) {
		r->newest = h->base;
		r->have_newest = 1;
	}
	/* let go of what is too old to come */
	for (unsigned k = 0; k < CG_RLC_EQ; k++)
		if (r->eq[k].used && cg_rep_stale(r->eq[k].base, r->newest)) {
			cg_rlc_eq_free(r, &r->eq[k]);
			r->dropped++;
		}
	if (cg_rep_stale(h->base, r->newest)) {
		r->useless++;
		return 0;
	}
	miss = cg_rep_missing(ring, h->base, h->map);
	if (!miss) {
		r->useless++;
		return 0;
	}
	for (unsigned k = 0; k < CG_RLC_EQ; k++) {
		if (!r->eq[k].used) {
			e = &r->eq[k];
			break;
		}
		if (!e || (int32_t)(r->eq[k].born - e->born) < 0)
			e = &r->eq[k];
	}
	if (e->used) {
		cg_rlc_eq_free(r, e);
		r->dropped++;
	}
	e->base = h->base;
	e->slen = (uint16_t)slen;
	e->born = r->born++;
	memcpy(e->x, s, slen);
	memset(e->c, 0, sizeof(e->c));
	for (uint64_t m = h->map; m; m &= m - 1) {
		unsigned d = (unsigned)__builtin_ctzll(m);
		uint8_t c = cg_rlc_coef(h->idx, d);

		if (miss >> d & 1) {
			e->c[d] = c;
		} else {
			uint16_t len;
			const uint8_t *p = cg_rep_data(ring, h->base + d, &len);

			if ((unsigned)len + 2 > slen) {
				r->bad++; /* a member longer than the repair */
				return 0;
			}
			cg_gf_madd_sym(g, e->x, p, len, c);
		}
	}
	e->miss = miss;
	e->used = 1;
	r->kept++;
	for (uint64_t m = miss; m; m &= m - 1)
		r->want[(h->base + (unsigned)__builtin_ctzll(m)) & (CG_REP_RING - 1)]++;
	return cg_rlc_rx_solve(g, r, ring, out, nout);
}

/* DATA seq went into the ring: out of the equations, and what that settles
 * rebuilt (into out, as cg_rlc_rx_repair). */
static inline unsigned cg_rlc_rx_data(const struct cg_gf *g, struct cg_rlc_rx *r, struct cg_rep_ring *ring, uint32_t seq,
				      uint32_t *out, unsigned nout)
{
	if (!cg_rlc_rx_sub(g, r, ring, seq))
		return 0;
	return cg_rlc_rx_solve(g, r, ring, out, nout);
}


/* ---- the parity asked for again (harq.h, story 023) ---- */

/* A combination over members given by the caller (the sender's rescue
 * ring, not its window): seed, and for each bit d of map (from base) the
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
	h->base = base;
	h->map = map;
	h->kind = CG_REP_RLC;
	h->m = 0;
	h->idx = seed;
	return slen;
}

/* The rank deficit of the equations that miss seq, and of those that share
 * unknowns with them: their unknowns in *base and *map (within CG_REP_SPAN
 * of *base), and how many more independent equations would settle all of
 * them. An equation whose unknowns do not fit that span is left out (the
 * deficit is then larger: more is asked than needed, never less). 0 when
 * no equation misses seq. */
static inline unsigned cg_rlc_rx_need(const struct cg_gf *g, const struct cg_rlc_rx *r, uint32_t seq, uint32_t *base,
				      uint64_t *map)
{
	uint8_t a[CG_RLC_EQ][CG_REP_SPAN];
	uint8_t in[CG_RLC_EQ] = { 0 };
	uint32_t lo = seq;
	uint64_t u = 1; /* unknowns, bit i: lo + i */
	unsigned n = 0, rank = 0, grew = 1;
	int any = 0;

	if (!r->want[seq & (CG_REP_RING - 1)])
		return 0;
	while (grew) {
		grew = 0;
		for (unsigned k = 0; k < CG_RLC_EQ; k++) {
			const struct cg_rlc_eq *e = &r->eq[k];
			uint32_t elo, nlo;
			uint64_t em, nu;
			int touch = 0;

			if (in[k] || !e->used || !e->miss)
				continue;
			elo = e->base + (unsigned)__builtin_ctzll(e->miss);
			em = e->miss >> __builtin_ctzll(e->miss);
			/* shares an unknown with u? */
			for (uint64_t m = em; m && !touch; m &= m - 1) {
				uint32_t d = elo + (unsigned)__builtin_ctzll(m) - lo;

				touch = d < CG_REP_SPAN && (u >> d & 1);
			}
			if (!touch)
				continue;
			/* u and its unknowns in one span from the lower of the two */
			nlo = (int32_t)(elo - lo) < 0 ? elo : lo;
			if (lo - nlo >= CG_REP_SPAN || (lo - nlo && (u >> (CG_REP_SPAN - (lo - nlo))))) {
				in[k] = 2; /* does not fit: left out */
				continue;
			}
			nu = u << (lo - nlo);
			if (elo - nlo >= CG_REP_SPAN || (elo - nlo && (em >> (CG_REP_SPAN - (elo - nlo))))) {
				in[k] = 2;
				continue;
			}
			nu |= em << (elo - nlo);
			u = nu;
			lo = nlo;
			in[k] = 1;
			any = 1;
			grew = 1;
		}
	}
	if (!any)
		return 0;
	/* the coefficient rows over u's columns, then their rank */
	for (unsigned k = 0; k < CG_RLC_EQ; k++) {
		const struct cg_rlc_eq *e = &r->eq[k];

		if (in[k] != 1)
			continue;
		memset(a[n], 0, sizeof(a[n]));
		for (uint64_t m = e->miss; m; m &= m - 1) {
			unsigned d = (unsigned)__builtin_ctzll(m);

			a[n][e->base + d - lo] = e->c[d];
		}
		n++;
	}
	for (unsigned c = 0; c < CG_REP_SPAN && rank < n; c++) {
		unsigned p = rank;

		if (!(u >> c & 1))
			continue;
		while (p < n && !a[p][c])
			p++;
		if (p == n)
			continue;
		if (p != rank) {
			uint8_t tmp[CG_REP_SPAN];

			memcpy(tmp, a[p], CG_REP_SPAN);
			memcpy(a[p], a[rank], CG_REP_SPAN);
			memcpy(a[rank], tmp, CG_REP_SPAN);
		}
		cg_gf_scale(g, a[rank], g->inv[a[rank][c]], CG_REP_SPAN);
		for (unsigned i = rank + 1; i < n; i++)
			if (a[i][c])
				cg_gf_madd(g, a[i], a[rank], a[i][c], CG_REP_SPAN);
		rank++;
	}
	*base = lo;
	*map = u;
	return (unsigned)__builtin_popcountll(u) - rank;
}

#endif
