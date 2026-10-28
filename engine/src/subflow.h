/* subflow.h: one link as a subflow of MPGRIT, the receiver's side (story
 * 029, step U1). Pure, no I/O.
 *
 * Each link numbers what it carries (sseq, its own sequence), so a gap is
 * judged against that link alone, never against the mix of the links: the
 * reordering between links, which in the field read as loss to both the
 * tunnel's reorderer and GRIT's RACK (story 028, «Terreno»: everything the
 * tunnel gave up came late), is no longer evidence of anything. Within a
 * link a gap is lost when a later sseq of that same link arrived more than
 * reo_us before (RACK on one path, RFC 8985); a gap that still comes after
 * it was called lost was a false verdict: the link reorders that much, so
 * its window widens (as a DSACK widens TCP's), up to cap_us, and narrows
 * again by itself (cg_sf_rx_decay). What a link loses goes to the sender
 * as that link's loss (the sender retransmits that data on any link, and
 * that link's rate learns of it): the stream above only orders.
 *
 * sseq is 32 bits and wraps; the receiver keeps the last CG_SF_WIN of them,
 * and what slides out of them unfilled is given up unsaid (overflow). A
 * jump of a whole window or more, ahead of the top or behind the bottom, is
 * a new start (resync: the other end began again, a link took a slot
 * another left, or the link lost all of it): it says no loss, since an
 * SFLOSS has to be true (the sender takes it as a certain loss).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SUBFLOW_H
#define CG_SUBFLOW_H

#include <stdint.h>
#include <string.h>

#define CG_SF_WIN 1024u           /* sseqs the receiver remembers per link */
#define CG_SF_REO_STEP_US 4000u   /* a false loss widens the window by this */
#define CG_SF_REO_DECAY_US 200u   /* and a tick (100 ms) narrows it by this */
#define CG_SF_FLOOR_US 10000u     /* the engine's window: from 10 ms */
#define CG_SF_CAP_US 300000u      /* up to the latency budget (streamcc.h's CG_SCC_REO_MAX_US) */

enum cg_sf_state { CG_SF_NONE, CG_SF_GAP, CG_SF_GOT, CG_SF_LOST };

enum cg_sf_in {
	CG_SF_NEW,   /* in order, or past a gap */
	CG_SF_FILL,  /* fills a gap in time: reordering within the window */
	CG_SF_LATE,  /* came after it was called lost: a false loss */
	CG_SF_DUP,   /* already here */
	CG_SF_OLD,   /* behind the window: nothing to say */
};

struct cg_sf_rx {
	uint32_t base;           /* the oldest sseq still kept */
	uint32_t next;           /* one past the highest sseq seen */
	uint32_t lo;             /* no gap below it (where cg_sf_rx_poll starts) */
	int started;
	uint8_t st[CG_SF_WIN];   /* enum cg_sf_state, by sseq % CG_SF_WIN */
	uint32_t seen[CG_SF_WIN]; /* a gap: when a later sseq first came (us) */
	uint32_t reo_us;         /* the window: a gap this old behind a later arrival is lost */
	uint32_t floor_us, cap_us;
	uint64_t got, lost, late, fill, dup, overflow, resync;
};

static inline void cg_sf_rx_init(struct cg_sf_rx *s, uint32_t floor_us, uint32_t cap_us)
{
	memset(s, 0, sizeof(*s));
	s->floor_us = floor_us;
	s->cap_us = cap_us > floor_us ? cap_us : floor_us;
	s->reo_us = floor_us;
}

/* Gives up the oldest kept sseq (counting it lost if it was still a gap). */
static inline void cg_sf_rx_slide(struct cg_sf_rx *s)
{
	uint8_t *e = &s->st[s->base % CG_SF_WIN];

	if (*e == CG_SF_GAP) {
		s->lost++;
		s->overflow++;
	}
	*e = CG_SF_NONE;
	s->base++;
}

/* One arrival of sseq q at now (us). */
static inline enum cg_sf_in cg_sf_rx_in(struct cg_sf_rx *s, uint32_t q, uint32_t now)
{
	uint8_t *e;

	if (!s->started || (int32_t)(q - s->next) >= (int32_t)CG_SF_WIN ||
	    (int32_t)(q - s->base) < -(int32_t)CG_SF_WIN) {
		s->resync += s->started;
		s->started = 1;
		memset(s->st, 0, sizeof(s->st));
		s->base = s->next = s->lo = q;
	}
	if ((int32_t)(q - s->base) < 0)
		return CG_SF_OLD;
	if ((int32_t)(q - s->next) >= 0) {
		/* past the top: what lies between becomes gaps, noticed now */
		while ((uint32_t)(q - s->base) >= CG_SF_WIN)
			cg_sf_rx_slide(s); /* stops at next at the latest: q - next < CG_SF_WIN */
		if ((int32_t)(s->lo - s->base) < 0)
			s->lo = s->base;
		for (uint32_t g = s->next; g != q; g++) {
			s->st[g % CG_SF_WIN] = g ? CG_SF_GAP : CG_SF_GOT; /* the sender skips 0 */
			s->seen[g % CG_SF_WIN] = now;
		}
		s->st[q % CG_SF_WIN] = CG_SF_GOT;
		s->next = q + 1;
		s->got++;
		return CG_SF_NEW;
	}
	e = &s->st[q % CG_SF_WIN];
	if (*e == CG_SF_GOT)
		return (s->dup++, CG_SF_DUP);
	if (*e == CG_SF_LOST) {
		/* a false loss: this link reorders more than the window */
		*e = CG_SF_GOT;
		s->late++;
		s->got++;
		s->reo_us += CG_SF_REO_STEP_US;
		if (s->reo_us > s->cap_us)
			s->reo_us = s->cap_us;
		return CG_SF_LATE;
	}
	*e = CG_SF_GOT;
	s->got++;
	s->fill++;
	return CG_SF_FILL;
}

/* The gaps that are lost at now: a later sseq of this link came more than
 * reo_us ago. Writes up to max of them to out (oldest first), marks them
 * lost; returns how many. */
static inline unsigned cg_sf_rx_poll(struct cg_sf_rx *s, uint32_t now, uint32_t *out, unsigned max)
{
	unsigned n = 0;

	if (!s->started)
		return 0;
	if ((int32_t)(s->lo - s->base) < 0)
		s->lo = s->base;
	while (s->lo != s->next && s->st[s->lo % CG_SF_WIN] != CG_SF_GAP)
		s->lo++;
	for (uint32_t g = s->lo; g != s->next && n < max; g++) {
		uint8_t *e = &s->st[g % CG_SF_WIN];

		if (*e != CG_SF_GAP)
			continue;
		if ((int32_t)(now - s->seen[g % CG_SF_WIN]) < (int32_t)s->reo_us)
			break; /* noticed later than the ones after it: none further is due */
		*e = CG_SF_LOST;
		s->lost++;
		out[n++] = g;
	}
	/* what is settled at the bottom is no longer needed */
	while (s->base != s->next && s->st[s->base % CG_SF_WIN] == CG_SF_GOT)
		s->st[s->base++ % CG_SF_WIN] = CG_SF_NONE;
	return n;
}

/* When the oldest gap is due, in us from now (0: already), or -1: none. */
static inline int32_t cg_sf_rx_due_us(const struct cg_sf_rx *s, uint32_t now)
{
	uint32_t g = (int32_t)(s->lo - s->base) < 0 ? s->base : s->lo;

	if (!s->started)
		return -1;
	for (; g != s->next; g++)
		if (s->st[g % CG_SF_WIN] == CG_SF_GAP) {
			int32_t d = (int32_t)(s->seen[g % CG_SF_WIN] + s->reo_us - now);

			return d > 0 ? d : 0;
		}
	return -1;
}

/* The sender's side of a link: it numbers each DATA it puts on the link
 * (sseq 0 means "not numbered", so the count skips it) and remembers which
 * tunnel sequence each sseq carried, to send that again when the receiver
 * says the link lost it. What the socket would not take is taken back
 * (cg_sf_tx_unsend), or the receiver would read a false loss. */
struct cg_sf_tx {
	uint32_t next;            /* the next sseq to give; 0 until the first */
	uint32_t sseq[CG_SF_WIN]; /* by sseq % CG_SF_WIN: which sseq the slot holds */
	uint32_t seq[CG_SF_WIN];  /* and the tunnel sequence it carried */
	uint64_t sent, unsent, lost_rx, stale;
};

static inline void cg_sf_tx_init(struct cg_sf_tx *t)
{
	memset(t, 0, sizeof(*t));
	t->next = 1;
}

/* The sseq for a DATA of tunnel sequence seq going on the link now. */
static inline uint32_t cg_sf_tx_take(struct cg_sf_tx *t, uint32_t seq)
{
	uint32_t q = t->next++;

	if (!t->next)
		t->next = 1;
	t->sseq[q % CG_SF_WIN] = q;
	t->seq[q % CG_SF_WIN] = seq;
	t->sent++;
	return q;
}

/* The last n taken did not go out: their numbers are given again. */
static inline void cg_sf_tx_unsend(struct cg_sf_tx *t, uint32_t n)
{
	while (n--) {
		if (t->next == 1)
			t->next = 0; /* step back over the skipped 0 */
		t->next--;
		t->sseq[t->next % CG_SF_WIN] = 0;
		t->sent--;
		t->unsent++;
	}
}

/* The tunnel sequence sseq q carried, if still remembered: 1 and *seq. */
static inline int cg_sf_tx_find(struct cg_sf_tx *t, uint32_t q, uint32_t *seq)
{
	t->lost_rx++;
	if (!q || t->sseq[q % CG_SF_WIN] != q) {
		t->stale++;
		return 0;
	}
	*seq = t->seq[q % CG_SF_WIN];
	return 1;
}

/* A tick (100 ms): the window narrows back toward its floor. */
static inline void cg_sf_rx_decay(struct cg_sf_rx *s)
{
	s->reo_us = s->reo_us > s->floor_us + CG_SF_REO_DECAY_US ? s->reo_us - CG_SF_REO_DECAY_US : s->floor_us;
}

#endif
