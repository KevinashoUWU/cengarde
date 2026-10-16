/* Spatial FEC (story 013): a single XOR parity over a group of DATA sent
 * one per link, so that a link that stalls or loses costs no wait. The
 * links are the code's second dimension: a group holds at most one packet
 * of each link and its parity goes on a link outside it, so the stall of
 * any one link, however long, leaves each group one packet short, which
 * its parity rebuilds as soon as the rest arrive (an (n+1, n) single
 * parity code: MDS, any n of its n+1 packets give them all). On the
 * testnet's four modems, replayed from a 120 s trace (one packet per link
 * every 4 ms), the split's 29 waits over 100 ms went to none, at a third
 * more bytes; duplicating every packet costs three times more.
 *
 * The sender sends every DATA packet as it comes (no wait for the parity)
 * and adds it to the oldest open group that has nothing of its link yet;
 * up to CG_FEC_OPEN groups are open at once, interleaved, and a group may
 * take sequences up to CG_FEC_SPAN apart: the split sends runs of tens of
 * packets on one link (a batch read from WireGuard goes where it arrives
 * first), and groups of consecutive sequences closed with one packet each,
 * a parity per packet, as many bytes as duplicating (story 013). A group
 * closes, and its parity goes, when it holds k packets (k: the links that
 * carry, less the one for the parity), when it would span more than
 * CG_FEC_SPAN, when a packet finds every group open holding its link, or at
 * its deadline: at a low packet rate the deadline closes small groups, at a
 * high one they fill in a few ms. A packet on several links needs none,
 * and a group closed with one packet sends none: its parity would be a
 * copy, and when the split sends most on one link (88 % in the lab's
 * lossy case) that was most groups (the rescue covers them).
 *
 * The receiver keeps a copy of the DATA it received lately (a ring by
 * sequence) and the parities it has not used; a group with all but one of
 * its packets and its parity gives the missing one, which then goes on as
 * if it had arrived. It comes before the rescue would ask for it
 * (rescue_wait_ms is longer than a group's deadline and the links' spread),
 * so the rescue only asks for what the parity cannot rebuild: two of a
 * group lost, or the parity itself.
 *
 * Parity payload (CG_T_FEC): a big-endian base sequence (the group's
 * first), the count n (1 to CG_FEC_KMAX), the XOR of the lengths
 * (big-endian 16 bits), CG_FEC_KMAX - 1 big-endian 16-bit distances from
 * the base, of the members after the first (the unused ones 0), then the
 * XOR of the payloads, each padded with zeros to the longest.
 *
 * Pure: no clock, no I/O, no allocation (the caller owns the memory).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FEC_H
#define CG_FEC_H

#include <stdint.h>
#include <string.h>

#define CG_FEC_PLEN_MAX 2024 /* the largest DATA payload (engine.h's CG_MAX_PAYLOAD) */
#define CG_FEC_KMAX 15       /* packets in a group, at most */
#define CG_FEC_HDR (7 + 2 * (CG_FEC_KMAX - 1)) /* base, n, lengths' XOR, distances */
#define CG_FEC_SPAN 512      /* a group's sequences, from its first: within this */
#define CG_FEC_OPEN 32       /* groups the sender has open at once */
#define CG_FEC_RING 1024     /* DATA kept by the receiver: a power of two, over CG_FEC_SPAN */
#define CG_FEC_PAR 64        /* parities kept by the receiver */

_Static_assert(!(CG_FEC_RING & (CG_FEC_RING - 1)), "CG_FEC_RING is a power of two");
_Static_assert(CG_FEC_SPAN < CG_FEC_RING && CG_FEC_SPAN <= 65536, "a group fits the ring and 16-bit distances");

/* One of the sender's open groups. */
struct cg_fec_grp {
	uint32_t base;              /* its first sequence */
	uint16_t d[CG_FEC_KMAX];    /* its members' distances from base (d[0] = 0) */
	uint8_t n;                  /* packets in it, 0: not open */
	uint16_t mask;              /* the links they went on */
	uint16_t lenx;              /* their lengths' XOR */
	uint16_t maxlen;            /* the longest */
	uint64_t t0_us;             /* when its first packet went */
	uint8_t x[CG_FEC_PLEN_MAX];
};

/* The sender: its open groups. */
struct cg_fec_tx {
	struct cg_fec_grp g[CG_FEC_OPEN];
};

static inline void cg_fec_grp_add(struct cg_fec_grp *g, uint32_t seq, int link, const uint8_t *p, uint16_t len,
				  uint64_t now_us)
{
	if (!g->n) {
		g->base = seq;
		g->mask = 0;
		g->lenx = 0;
		g->maxlen = 0;
		g->t0_us = now_us;
	}
	if (len > CG_FEC_PLEN_MAX)
		len = CG_FEC_PLEN_MAX;
	if (len > g->maxlen) {
		memset(g->x + g->maxlen, 0, (size_t)(len - g->maxlen));
		g->maxlen = len;
	}
	for (uint16_t i = 0; i < len; i++)
		g->x[i] ^= p[i];
	g->lenx ^= len;
	g->d[g->n] = (uint16_t)(seq - g->base);
	g->mask |= (uint16_t)(1u << link);
	g->n++;
}

/* Closes group g: its parity payload into out (at least CG_FEC_HDR +
 * CG_FEC_PLEN_MAX bytes); returns its length, 0 if it was not open. */
static inline unsigned cg_fec_grp_close(struct cg_fec_grp *g, uint8_t *out)
{
	if (!g->n)
		return 0;
	out[0] = (uint8_t)(g->base >> 24);
	out[1] = (uint8_t)(g->base >> 16);
	out[2] = (uint8_t)(g->base >> 8);
	out[3] = (uint8_t)g->base;
	out[4] = g->n;
	out[5] = (uint8_t)(g->lenx >> 8);
	out[6] = (uint8_t)g->lenx;
	for (unsigned j = 1; j < CG_FEC_KMAX; j++) {
		uint16_t d = j < g->n ? g->d[j] : 0;

		out[7 + 2 * (j - 1)] = (uint8_t)(d >> 8);
		out[8 + 2 * (j - 1)] = (uint8_t)d;
	}
	memcpy(out + CG_FEC_HDR, g->x, g->maxlen);
	g->n = 0;
	return CG_FEC_HDR + g->maxlen;
}

/* The open group that must close before a packet seq can go anywhere (it
 * would span more than CG_FEC_SPAN), or the oldest open when every slot is
 * open and none takes link; -1: none. Call until -1, closing each. */
static inline int cg_fec_tx_evict(const struct cg_fec_tx *t, uint32_t seq, int link)
{
	int old = -1, room = 0;

	for (int i = 0; i < CG_FEC_OPEN; i++) {
		const struct cg_fec_grp *g = &t->g[i];

		if (!g->n) {
			room = 1;
			continue;
		}
		if (seq - g->base >= CG_FEC_SPAN)
			return i;
		if (!(g->mask >> link & 1))
			room = 1;
		if (old < 0 || g->t0_us < t->g[old].t0_us)
			old = i;
	}
	return room ? -1 : old;
}

/* Adds a packet (seq, on link alone, len bytes at p) at now_us to the oldest
 * open group without link, or a new one (after cg_fec_tx_evict found
 * nothing); returns its group, to close once it holds k. */
static inline int cg_fec_tx_add(struct cg_fec_tx *t, uint32_t seq, int link, const uint8_t *p, uint16_t len,
				uint64_t now_us)
{
	int best = -1, fresh = -1;

	for (int i = 0; i < CG_FEC_OPEN; i++) {
		const struct cg_fec_grp *g = &t->g[i];

		if (!g->n) {
			if (fresh < 0)
				fresh = i;
		} else if (!(g->mask >> link & 1) && (best < 0 || g->t0_us < t->g[best].t0_us)) {
			best = i;
		}
	}
	if (best < 0)
		best = fresh;
	cg_fec_grp_add(&t->g[best], seq, link, p, len, now_us);
	return best;
}

/* An open group due at now_us (deadline_us after its first packet), -1:
 * none. */
static inline int cg_fec_tx_due(const struct cg_fec_tx *t, uint64_t now_us, uint32_t deadline_us)
{
	for (int i = 0; i < CG_FEC_OPEN; i++)
		if (t->g[i].n && now_us - t->g[i].t0_us >= deadline_us)
			return i;
	return -1;
}

/* When the oldest open group is due (its first packet's time plus
 * deadline_us); 0: none open. */
static inline uint64_t cg_fec_tx_next_us(const struct cg_fec_tx *t, uint32_t deadline_us)
{
	uint64_t t0 = 0;

	for (int i = 0; i < CG_FEC_OPEN; i++)
		if (t->g[i].n && (!t0 || t->g[i].t0_us < t0))
			t0 = t->g[i].t0_us;
	return t0 ? t0 + deadline_us : 0;
}

/* A parity the receiver keeps until its group is whole or rebuilt. */
struct cg_fec_par {
	uint32_t base;
	uint16_t d[CG_FEC_KMAX]; /* the members' distances from base */
	uint8_t n;
	uint8_t used;
	uint16_t lenx;
	uint16_t plen; /* bytes of x */
	uint8_t x[CG_FEC_PLEN_MAX];
};

/* The receiver: the DATA it got lately, by sequence (CG_FEC_RING entries
 * the caller allocates: seq, len, have, mem of CG_FEC_RING x
 * CG_FEC_PLEN_MAX, pidx), and the parities. */
struct cg_fec_rx {
	uint32_t *seq;
	uint16_t *len;
	uint8_t *have;
	uint8_t *mem;
	uint8_t *pidx; /* per ring entry: the parity slot covering it, + 1; 0: none */
	struct cg_fec_par par[CG_FEC_PAR];
	uint32_t next; /* the parity slot taken next */
	uint64_t recovered, unrecovered, parities, bad;
};

static inline int cg_fec_rx_has(const struct cg_fec_rx *r, uint32_t seq)
{
	uint32_t i = seq & (CG_FEC_RING - 1);

	return r->have[i] && r->seq[i] == seq;
}

/* Whether parity q covers seq. */
static inline int cg_fec_par_has(const struct cg_fec_par *q, uint32_t seq)
{
	uint32_t d = seq - q->base;

	if (d >= CG_FEC_SPAN)
		return 0;
	for (unsigned j = 0; j < q->n; j++)
		if (q->d[j] == d)
			return 1;
	return 0;
}

/* The parity slot that covers seq, -1: none known. */
static inline int cg_fec_rx_slot(const struct cg_fec_rx *r, uint32_t seq)
{
	int k = r->pidx[seq & (CG_FEC_RING - 1)] - 1;

	if (k < 0 || !r->par[k].used || !cg_fec_par_has(&r->par[k], seq))
		return -1;
	return k;
}

/* Keeps a copy of DATA seq (len bytes at p); returns the parity slot that
 * covers it (to try, cg_fec_rx_try), -1: none yet. */
static inline int cg_fec_rx_put(struct cg_fec_rx *r, uint32_t seq, const uint8_t *p, uint16_t len)
{
	uint32_t i = seq & (CG_FEC_RING - 1);

	if (len > CG_FEC_PLEN_MAX)
		len = CG_FEC_PLEN_MAX;
	r->seq[i] = seq;
	r->len[i] = len;
	r->have[i] = 1;
	memcpy(r->mem + (size_t)i * CG_FEC_PLEN_MAX, p, len);
	return cg_fec_rx_slot(r, seq);
}

/* Takes a parity payload (plen bytes at p); returns its slot, -1 if it is
 * malformed (counted in bad). A slot still holding an unrebuilt group is
 * counted in unrecovered when taken over. */
static inline int cg_fec_rx_parity(struct cg_fec_rx *r, const uint8_t *p, unsigned plen)
{
	struct cg_fec_par *q;
	unsigned n, k;

	if (plen < CG_FEC_HDR || plen > CG_FEC_HDR + CG_FEC_PLEN_MAX) {
		r->bad++;
		return -1;
	}
	n = p[4];
	if (!n || n > CG_FEC_KMAX) {
		r->bad++;
		return -1;
	}
	for (unsigned j = 1; j < n; j++) {
		unsigned d = (unsigned)p[7 + 2 * (j - 1)] << 8 | p[8 + 2 * (j - 1)];

		if (!d || d >= CG_FEC_SPAN) {
			r->bad++;
			return -1;
		}
	}
	k = r->next++ % CG_FEC_PAR;
	q = &r->par[k];
	if (q->used)
		r->unrecovered++;
	q->base = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	q->n = (uint8_t)n;
	q->lenx = (uint16_t)(p[5] << 8 | p[6]);
	q->d[0] = 0;
	for (unsigned j = 1; j < n; j++)
		q->d[j] = (uint16_t)(p[7 + 2 * (j - 1)] << 8 | p[8 + 2 * (j - 1)]);
	q->plen = (uint16_t)(plen - CG_FEC_HDR);
	memcpy(q->x, p + CG_FEC_HDR, q->plen);
	q->used = 1;
	r->parities++;
	for (unsigned j = 0; j < n; j++)
		r->pidx[(q->base + q->d[j]) & (CG_FEC_RING - 1)] = (uint8_t)(k + 1);
	return (int)k;
}

/* Tries parity slot k: when all but one of its group are kept, rebuilds
 * that one into out (CG_FEC_PLEN_MAX bytes), its sequence and length in
 * *seq and *len, and returns 1; 0: two or more still missing (the parity
 * stays); -1: nothing to rebuild (whole, or the slot is not in use; the
 * parity is let go). */
static inline int cg_fec_rx_try(struct cg_fec_rx *r, int k, uint8_t *out, uint32_t *seq, uint16_t *len)
{
	struct cg_fec_par *q;
	uint32_t miss = 0;
	unsigned nmiss = 0;
	uint16_t l;

	if (k < 0 || k >= CG_FEC_PAR || !r->par[k].used)
		return -1;
	q = &r->par[k];
	for (unsigned j = 0; j < q->n; j++)
		if (!cg_fec_rx_has(r, q->base + q->d[j])) {
			miss = q->base + q->d[j];
			if (++nmiss > 1)
				return 0;
		}
	if (!nmiss) {
		q->used = 0;
		return -1;
	}
	l = q->lenx;
	memcpy(out, q->x, q->plen);
	memset(out + q->plen, 0, CG_FEC_PLEN_MAX - q->plen);
	for (unsigned j = 0; j < q->n; j++) {
		uint32_t s = q->base + q->d[j], i = s & (CG_FEC_RING - 1);
		const uint8_t *m = r->mem + (size_t)i * CG_FEC_PLEN_MAX;

		if (s == miss)
			continue;
		l ^= r->len[i];
		for (uint16_t b = 0; b < r->len[i]; b++)
			out[b] ^= m[b];
	}
	q->used = 0;
	if (!l || l > q->plen) {
		r->bad++;
		return -1;
	}
	*seq = miss;
	*len = l;
	r->recovered++;
	return 1;
}

#endif
