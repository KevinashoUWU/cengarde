/* The rescue (story 013, the XLINK-like ARQ of story 012): the server keeps
 * the DATA it sent lately, and when the client's reorderer is about to give
 * a gap up it asks for it (CG_T_NACK); the server sends it again on its
 * fastest path, so that it comes before the reorderer gives up and TCP
 * never sees the loss. On the testnet 1-2 % of the download was lost on
 * every link, and the reorderer could only give those gaps up.
 *
 * The ring: entries of CG_RESC_ENTRY bytes (a header and a packet), in the
 * order they were sent, the oldest overwritten. Each
 * session keeps an index by sequence (CG_RESC_IDX entries, seq modulo it)
 * of where its packets went; a lookup checks the entry's session and
 * sequence, so a packet overwritten since (or a sequence that wrapped the
 * index) is simply not there, never another one.
 *
 * Congestion is not rescued: the ring keeps the path each packet went on,
 * and the server does not send again one lost on a path whose queue is
 * full (capacity.h's sat_us). That loss is how TCP learns it sends too
 * much; hidden, it never slowed down, the full link lost more, and in the
 * lab (a 10 Mbit/s link beside 60 and 30) held packets waited seconds.
 *
 * The budget: what is sent again comes out of a credit that each packet
 * sent down adds pct % of its size to, up to CG_RESC_CREDIT_MAX: rescues
 * cost at most that share of the download, and a burst of losses cannot
 * flood the links (XLINK: re-injection without control added 15 %; with
 * it, 2 %).
 *
 * Evidence (story 029): what is known of a kept packet's loss, from two
 * sides that are each unsure alone. Its one link lost it (subflow.h's
 * SFLOSS): the link is sure, but a code or a copy may have rebuilt it at
 * the receiver. The receiver asked for it (a NACK): sure it lacks it, but
 * maybe only because it is late on another link (in the field all the
 * tunnel gave up came later). Both: a certain loss. Done: handed over as
 * one, not again. The link's word alone where a code may rebuild it: a
 * suspect, lost if the code has not rebuilt it by the repair horizon (the
 * receiver's NACK may never come: it waits for the code, then gives up).
 * A copied packet (redundancy): its links each lost their copy, every one
 * of them, is the same certain word as one link losing a packet that went
 * on it alone (cg_resc_lost_on). Before, a copy lost on every link was
 * never told to GRIT, whose RACK is off with subflows: only its timer, 300
 * ms past the round trip, found it (story 029, red1: thousands of
 * retransmissions in fade, every one of them by the timer).
 *
 * Pure: no clock, no I/O, no allocation (the caller owns the memory).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_RESCUE_H
#define CG_RESCUE_H

#include <stdint.h>
#include <string.h>

#define CG_RESC_ENTRY 2048u          /* bytes per entry: header + datagram (CG_BUF) */
#define CG_RESC_IDX 32768u           /* a session's index, by sequence: a power of two */
#define CG_RESC_CREDIT_MAX 1048576ull /* bytes: the most a burst of rescues may send */
#define CG_RESC_NOLINK 0xffu
#define CG_RESC_DUP_MS 20u /* a sequence asked for again within this: the same NACK on another path */
#define CG_RESC_EV_LINK 1u  /* its one link lost it (an SFLOSS) */
#define CG_RESC_EV_ASKED 2u /* the receiver asked for it (a NACK) */
#define CG_RESC_EV_DONE 4u  /* handed over as a certain loss */
#define CG_RESC_EV_SUSPECT 8u /* handed over as a suspect: lost unless the code rebuilds it in time */

_Static_assert(!(CG_RESC_IDX & (CG_RESC_IDX - 1)), "CG_RESC_IDX is a power of two");

struct cg_resc_ring {
	uint8_t *mem;   /* n x CG_RESC_ENTRY */
	uint32_t *sess; /* each entry's session id */
	uint32_t *seq;  /* and sequence */
	uint16_t *len;  /* and length, 0: empty */
	uint8_t *link;  /* and the one path it went on, CG_RESC_NOLINK: several */
	uint32_t *sent; /* and when it was last sent again (ms, low bits | 1), 0: never */
	uint8_t *ev;    /* and the evidence of its loss (CG_RESC_EV_*); NULL: none kept */
	uint16_t *copies; /* and the links its copies went on (0: not known); NULL: none kept */
	uint16_t *lost;   /* of them, those whose SFLOSS said they lost it */
	uint32_t n, next;
};

struct cg_resc_budget {
	uint64_t credit; /* bytes that may still be sent again */
};

/* Takes the next entry for session sess's sequence seq, len bytes (at most
 * CG_RESC_ENTRY), and points idx (the session's index) at it; returns where
 * to copy the packet. */
static inline uint8_t *cg_resc_put(struct cg_resc_ring *r, uint32_t *idx, uint32_t sess, uint32_t seq, uint16_t len,
				   uint8_t link)
{
	uint32_t i = r->next;

	r->next = i + 1 == r->n ? 0 : i + 1;
	r->sess[i] = sess;
	r->seq[i] = seq;
	r->len[i] = len;
	r->link[i] = link;
	r->sent[i] = 0;
	if (r->ev)
		r->ev[i] = 0;
	if (r->copies)
		r->copies[i] = r->lost[i] = 0;
	idx[seq & (CG_RESC_IDX - 1)] = i + 1;
	return r->mem + (size_t)i * CG_RESC_ENTRY;
}

/* Session sess's packet seq, with *len, or NULL when it is not kept. */
static inline const uint8_t *cg_resc_get(const struct cg_resc_ring *r, const uint32_t *idx, uint32_t sess,
					 uint32_t seq, uint16_t *len, uint8_t *link)
{
	uint32_t e = idx[seq & (CG_RESC_IDX - 1)];

	if (!e || e > r->n)
		return NULL;
	e--;
	if (!r->len[e] || r->sess[e] != sess || r->seq[e] != seq)
		return NULL;
	*len = r->len[e];
	*link = r->link[e];
	return r->mem + (size_t)e * CG_RESC_ENTRY;
}

/* Adds the evidence ev (CG_RESC_EV_*) to session sess's packet seq; returns
 * all its evidence so far, 0 when it is not kept. */
static inline unsigned cg_resc_ev(struct cg_resc_ring *r, const uint32_t *idx, uint32_t sess, uint32_t seq,
				  unsigned ev)
{
	uint32_t e = idx[seq & (CG_RESC_IDX - 1)];

	if (!r->ev || !e || e > r->n)
		return 0;
	e--;
	if (!r->len[e] || r->sess[e] != sess || r->seq[e] != seq)
		return 0;
	r->ev[e] |= (uint8_t)ev;
	return r->ev[e];
}

/* The links session sess's packet seq (kept) went on, when several: its
 * copies (cg_resc_lost_on). links 0: not known (a copy may go where the
 * ring does not see it: mute_trickle). */
static inline void cg_resc_copies(struct cg_resc_ring *r, const uint32_t *idx, uint32_t sess, uint32_t seq,
				  uint16_t links)
{
	uint32_t e = idx[seq & (CG_RESC_IDX - 1)];

	if (!r->copies || !e || e > r->n)
		return;
	e--;
	if (r->len[e] && r->sess[e] == sess && r->seq[e] == seq)
		r->copies[e] = links;
}

/* Link's SFLOSS said it lost session sess's packet seq, a copied one: 1
 * when now every link its copies went on has lost it (a certain loss on
 * the links, as one link's word on a packet that went on it alone), 0
 * while a copy may still come on another or its links are not known. */
static inline int cg_resc_lost_on(struct cg_resc_ring *r, const uint32_t *idx, uint32_t sess, uint32_t seq,
				  unsigned link)
{
	uint32_t e = idx[seq & (CG_RESC_IDX - 1)];
	uint16_t m;

	if (!r->copies || !e || e > r->n || link >= 16)
		return 0;
	e--;
	if (!r->len[e] || r->sess[e] != sess || r->seq[e] != seq)
		return 0;
	m = r->copies[e];
	if (!(m >> link & 1))
		return 0;
	r->lost[e] |= (uint16_t)(1u << link);
	return (r->lost[e] & m) == m;
}

/* How sure a loss is, from its evidence ev (CG_RESC_EV_*): coded, a code
 * (or a copy the ring does not know of) may rebuild what the link lost;
 * asked_is_lost, the receiver's ask alone is taken as a loss
 * (protocontroller_nack yes). CG_RESC_SURE_LATER: the link's word alone
 * with a code, a suspect (once). CG_RESC_UNSURE also when it was handed
 * over already. */
enum { CG_RESC_UNSURE, CG_RESC_SURE_ASKED, CG_RESC_SURE_LINK, CG_RESC_SURE_LATER };

static inline int cg_resc_sure(unsigned ev, int coded, int asked_is_lost)
{
	if (ev & CG_RESC_EV_DONE)
		return CG_RESC_UNSURE;
	if (ev & CG_RESC_EV_LINK) {
		if (!coded || (ev & CG_RESC_EV_ASKED))
			return CG_RESC_SURE_LINK;
		if (!(ev & CG_RESC_EV_SUSPECT))
			return CG_RESC_SURE_LATER;
	}
	return (ev & CG_RESC_EV_ASKED) && asked_is_lost ? CG_RESC_SURE_ASKED : CG_RESC_UNSURE;
}

/* Whether session sess's packet seq (kept) was sent again within
 * CG_RESC_DUP_MS: the NACK goes on two paths, and its second copy asks for
 * nothing new. If not, it is marked sent at now_ms. */
static inline int cg_resc_dup(struct cg_resc_ring *r, const uint32_t *idx, uint32_t seq, uint32_t now_ms)
{
	uint32_t e = idx[seq & (CG_RESC_IDX - 1)] - 1;

	if (r->sent[e] && now_ms - r->sent[e] < CG_RESC_DUP_MS)
		return 1;
	r->sent[e] = now_ms | 1;
	return 0;
}

/* A packet of bytes sent down: pct % of it to the credit. */
static inline void cg_resc_earn(struct cg_resc_budget *b, uint32_t bytes, uint32_t pct)
{
	b->credit += (uint64_t)bytes * pct / 100;
	if (b->credit > CG_RESC_CREDIT_MAX)
		b->credit = CG_RESC_CREDIT_MAX;
}

/* Whether the budget has room for about a burst of rescues: below this
 * the server says so (CG_BOND_FULL), and the client stops asking. */
#define CG_RESC_CREDIT_LOW 65536ull

static inline int cg_resc_low(const struct cg_resc_budget *b)
{
	return b->credit < CG_RESC_CREDIT_LOW;
}

/* Whether len bytes may be sent again now; they are taken from the credit. */
static inline int cg_resc_spend(struct cg_resc_budget *b, uint32_t len)
{
	if (b->credit < len)
		return 0;
	b->credit -= len;
	return 1;
}

#endif
