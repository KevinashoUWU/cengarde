/* Reed-Solomon on Leopard-RS for redund_spare = rs (rs.h, story 021).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "rs.h"

#include <pthread.h>
#include <string.h>

#ifndef CG_NO_LEOPARD
#include "leopard.h"
#endif

static pthread_once_t rs_once = PTHREAD_ONCE_INIT;
static int rs_ok;

static void rs_init_once(void)
{
#ifndef CG_NO_LEOPARD
	rs_ok = leo_init() == 0;
#endif
}

int cg_rs_init(void)
{
	pthread_once(&rs_once, rs_init_once);
	return rs_ok ? 0 : -1;
}

int cg_rs_ready(void)
{
	return rs_ok;
}

int cg_rs_encode(unsigned k, unsigned m, unsigned bytes, const uint8_t *const *orig, uint8_t **work)
{
#ifndef CG_NO_LEOPARD
	unsigned wc;

	if (!rs_ok || !k || !m || m > k || k > CG_RS_KMAX || m > CG_RS_MMAX || !bytes || bytes % 64 ||
	    bytes > CG_RS_BYTES_MAX)
		return -1;
	wc = leo_encode_work_count(k, m);
	if (!wc || wc > CG_RS_WORK_ENC)
		return -1;
	return leo_encode(bytes, k, m, wc, (const void *const *)orig, (void **)work) == Leopard_Success ? 0 : -1;
#else
	(void)k, (void)m, (void)bytes, (void)orig, (void)work;
	return -1;
#endif
}

int cg_rs_decode(unsigned k, unsigned m, unsigned bytes, const uint8_t *const *orig, const uint8_t *const *rec,
		 uint8_t **work)
{
#ifndef CG_NO_LEOPARD
	unsigned wc;

	if (!rs_ok || !k || !m || m > k || k > CG_RS_KMAX || m > CG_RS_MMAX || !bytes || bytes % 64 ||
	    bytes > CG_RS_BYTES_MAX)
		return -1;
	wc = leo_decode_work_count(k, m);
	if (!wc || wc > CG_RS_WORK_DEC)
		return -1;
	return leo_decode(bytes, k, m, wc, (const void *const *)orig, (const void *const *)rec, (void **)work) ==
			       Leopard_Success
		       ? 0
		       : -1;
#else
	(void)k, (void)m, (void)bytes, (void)orig, (void)rec, (void)work;
	return -1;
#endif
}

void cg_rs_tx_add(struct cg_rs_tx *t, uint32_t seq, const uint8_t *p, uint16_t len, unsigned m, uint64_t now_us)
{
	uint8_t *o;

	if (len > CG_SYM_PLEN)
		len = CG_SYM_PLEN;
	if (!t->k) {
		t->base = seq;
		t->map = 0;
		t->slen = 0;
		t->t0_us = now_us;
		t->m = (uint8_t)(m < 1 ? 1 : m > CG_RS_MMAX ? CG_RS_MMAX : m);
	}
	o = t->orig[t->k];
	o[0] = (uint8_t)(len >> 8);
	o[1] = (uint8_t)len;
	memcpy(o + 2, p, len);
	if (len + 2u > t->slen)
		t->slen = (uint16_t)(len + 2u);
	t->map |= 1ull << (seq - t->base);
	t->k++;
}

unsigned cg_rs_tx_close(struct cg_rs_tx *t, struct cg_rep_hdr *h, uint8_t **sym, unsigned *bytes)
{
	const uint8_t *orig[CG_RS_KMAX];
	uint8_t *work[CG_RS_WORK_ENC];
	unsigned k = t->k, m = t->m < k ? t->m : k, b = cg_rs_bytes(t->slen), mh = cg_rs_mhdr(k, m);

	t->k = 0;
	if (!k)
		return 0;
	/* each member padded to the block's size (what it did not write) */
	for (unsigned i = 0; i < k; i++) {
		unsigned l = 2u + ((unsigned)t->orig[i][0] << 8 | t->orig[i][1]);

		memset(t->orig[i] + l, 0, b - l);
		orig[i] = t->orig[i];
	}
	for (unsigned j = 0; j < CG_RS_WORK_ENC; j++)
		work[j] = t->work[j];
	if (cg_rs_encode(k, mh, b, orig, work) < 0)
		return 0;
	h->base = t->base;
	h->map = t->map;
	h->kind = CG_REP_RS;
	h->m = (uint8_t)mh;
	h->idx = 0;
	for (unsigned j = 0; j < m; j++)
		sym[j] = t->work[j];
	*bytes = b;
	return m;
}

static void blk_free(struct cg_rs_rx *r, struct cg_rs_blk *b)
{
	for (uint64_t m = b->miss; m; m &= m - 1)
		r->want[(b->base + (unsigned)__builtin_ctzll(m)) & (CG_REP_RING - 1)]--;
	for (uint32_t g = b->got; g; g &= g - 1)
		r->pool_used &= ~(1ull << b->slot[__builtin_ctz(g)]);
	b->miss = 0;
	b->got = 0;
	b->used = 0;
}

/* Decodes block b (as many repairs as missing) into the ring. */
static unsigned blk_decode(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring, struct cg_rs_blk *b,
			   uint32_t *out, unsigned nout)
{
	const uint8_t *orig[CG_RS_KMAX], *rec[CG_RS_MMAX];
	uint8_t *work[CG_RS_WORK_DEC];
	uint32_t seqs[CG_RS_KMAX];
	unsigned i = 0, got = 0;

	for (uint64_t m = b->map; m; m &= m - 1, i++) {
		unsigned d = (unsigned)__builtin_ctzll(m);
		uint32_t s = b->base + d;

		seqs[i] = s;
		if (b->miss >> d & 1) {
			orig[i] = NULL;
		} else {
			uint16_t len;
			const uint8_t *p = cg_rep_data(ring, s, &len);

			if (len + 2u > b->bytes) {
				r->bad++; /* longer than the block's symbols: not a member */
				blk_free(r, b);
				return 0;
			}
			x->orig[i][0] = (uint8_t)(len >> 8);
			x->orig[i][1] = (uint8_t)len;
			memcpy(x->orig[i] + 2, p, len);
			memset(x->orig[i] + 2 + len, 0, b->bytes - 2u - len);
			orig[i] = x->orig[i];
		}
	}
	for (unsigned j = 0; j < b->m; j++)
		rec[j] = b->got >> j & 1 ? r->rep[b->slot[j]] : NULL;
	for (unsigned j = 0; j < CG_RS_WORK_DEC; j++)
		work[j] = x->work[j];
	r->decodes++;
	if (cg_rs_decode(b->k, b->m, b->bytes, orig, rec, work) < 0) {
		r->failed++;
		blk_free(r, b);
		return 0;
	}
	for (i = 0; i < b->k; i++) {
		if (orig[i])
			continue;
		int rc = cg_rep_put_sym(ring, seqs[i], work[i], b->bytes);

		if (rc < 0) {
			if (rc == -1)
				r->bad++;
			else
				r->dropped++;
			continue;
		}
		r->recovered++;
		if (got < nout)
			out[got++] = seqs[i];
	}
	blk_free(r, b);
	return got;
}

static unsigned blk_try(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring, struct cg_rs_blk *b,
			uint32_t *out, unsigned nout)
{
	if (!b->miss) {
		blk_free(r, b);
		return 0;
	}
	if ((unsigned)__builtin_popcountll(b->miss) > (unsigned)__builtin_popcount(b->got))
		return 0;
	return blk_decode(r, x, ring, b, out, nout);
}

unsigned cg_rs_rx_repair(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring,
			 const struct cg_rep_hdr *h, const uint8_t *s, unsigned slen, uint32_t *out, unsigned nout)
{
	struct cg_rs_blk *b = NULL, *old = NULL;
	unsigned k = (unsigned)__builtin_popcountll(h->map), slot;

	r->repairs++;
	if (!cg_rs_ready() || k > CG_RS_KMAX || h->m > k) {
		r->bad++;
		return 0;
	}
	if (!r->have_newest || (int32_t)(h->base - r->newest) > 0) {
		r->newest = h->base;
		r->have_newest = 1;
	}
	for (unsigned i = 0; i < CG_RS_RXB; i++) {
		struct cg_rs_blk *c = &r->b[i];

		if (c->used && cg_rep_stale(c->base, r->newest)) {
			blk_free(r, c);
			r->dropped++;
		}
		if (c->used && c->base == h->base && c->map == h->map && c->m == h->m)
			b = c;
	}
	if (cg_rep_stale(h->base, r->newest)) {
		r->useless++;
		return 0;
	}
	if (!b) {
		uint64_t miss = cg_rep_missing(ring, h->base, h->map);

		if (!miss) {
			r->useless++;
			return 0;
		}
		for (unsigned i = 0; i < CG_RS_RXB && !b; i++) {
			if (!r->b[i].used)
				b = &r->b[i];
			else if (!old || (int32_t)(r->b[i].born - old->born) < 0)
				old = &r->b[i];
		}
		if (!b) {
			blk_free(r, old);
			r->dropped++;
			b = old;
		}
		b->base = h->base;
		b->map = h->map;
		b->m = h->m;
		b->k = (uint8_t)k;
		b->bytes = (uint16_t)slen;
		b->miss = miss;
		b->got = 0;
		b->used = 1;
		b->born = r->born++;
		for (uint64_t m = miss; m; m &= m - 1)
			r->want[(h->base + (unsigned)__builtin_ctzll(m)) & (CG_REP_RING - 1)]++;
	}
	if (slen != b->bytes) {
		r->bad++;
		return 0;
	}
	if (b->got >> h->idx & 1) {
		r->useless++;
		return 0;
	}
	/* a pool slot: a free one, or the oldest other block's */
	while (!~r->pool_used) {
		old = NULL;
		for (unsigned i = 0; i < CG_RS_RXB; i++)
			if (r->b[i].used && &r->b[i] != b && r->b[i].got &&
			    (!old || (int32_t)(r->b[i].born - old->born) < 0))
				old = &r->b[i];
		if (!old) {
			r->useless++;
			return 0;
		}
		blk_free(r, old);
		r->dropped++;
	}
	slot = (unsigned)__builtin_ctzll(~r->pool_used);
	r->pool_used |= 1ull << slot;
	memcpy(r->rep[slot], s, slen);
	b->slot[h->idx] = (uint8_t)slot;
	b->got |= 1u << h->idx;
	return blk_try(r, x, ring, b, out, nout);
}

unsigned cg_rs_rx_data(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring, uint32_t seq,
		       uint32_t *out, unsigned nout)
{
	uint32_t ix = seq & (CG_REP_RING - 1);
	unsigned got = 0;

	if (!r->want[ix])
		return 0;
	for (unsigned i = 0; i < CG_RS_RXB; i++) {
		struct cg_rs_blk *b = &r->b[i];
		uint32_t d = seq - b->base;

		if (!b->used || d >= CG_REP_SPAN || !(b->miss >> d & 1))
			continue;
		b->miss &= ~(1ull << d);
		r->want[ix]--;
		got += blk_try(r, x, ring, b, out + got, nout - got);
	}
	return got;
}
