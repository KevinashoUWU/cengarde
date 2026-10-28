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
 * sseq is 32 bits and wraps; the receiver keeps the last CG_SF_WIN of them.
 * A jump past the window gives up whatever it leaves behind (overflow).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SUBFLOW_H
#define CG_SUBFLOW_H

#include <stdint.h>
#include <string.h>

#define CG_SF_WIN 1024u           /* sseqs the receiver remembers per link */
#define CG_SF_REO_STEP_US 4000u   /* a false loss widens the window by this */
#define CG_SF_REO_DECAY_US 200u   /* and a tick (100 ms) narrows it by this */

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
	int started;
	uint8_t st[CG_SF_WIN];   /* enum cg_sf_state, by sseq % CG_SF_WIN */
	uint32_t seen[CG_SF_WIN]; /* a gap: when a later sseq first came (us) */
	uint32_t reo_us;         /* the window: a gap this old behind a later arrival is lost */
	uint32_t floor_us, cap_us;
	uint64_t got, lost, late, fill, dup, overflow;
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

	if (!s->started) {
		s->started = 1;
		s->base = q;
		s->next = q;
	}
	if ((int32_t)(q - s->base) < 0)
		return CG_SF_OLD;
	if ((int32_t)(q - s->next) >= 0) {
		/* past the top: what lies between becomes gaps, noticed now */
		while ((uint32_t)(q - s->base) >= CG_SF_WIN)
			cg_sf_rx_slide(s);
		for (uint32_t g = s->next; g != q; g++) {
			s->st[g % CG_SF_WIN] = CG_SF_GAP;
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
	for (uint32_t g = s->base; g != s->next && n < max; g++) {
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

/* A tick (100 ms): the window narrows back toward its floor. */
static inline void cg_sf_rx_decay(struct cg_sf_rx *s)
{
	s->reo_us = s->reo_us > s->floor_us + CG_SF_REO_DECAY_US ? s->reo_us - CG_SF_REO_DECAY_US : s->floor_us;
}

#endif
