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

#include "linkcc.h"

/* sseqs each end remembers per link: an SFLOSS comes a queue, the window
 * (up to CG_SF_CAP_US) and a way back after its sseq went, and the sender
 * has to still know what it carried; at 1024, ~110 ms at 100 Mbit/s, the
 * field's sender had forgotten two thirds of them, which GRIT then sent
 * again only on its timer. 8192: ~0.9 s there. */
#define CG_SF_WIN 8192u
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
	uint64_t tail; /* gaps a probe opened (cg_sf_rx_sent) */
	/* for SFACK (cg_sf_rx_took): the bytes taken (wrapping), the newest sseq
	 * and when it came (us, low 32 bits) */
	uint32_t gotb, top, top_us;
	uint32_t rep_top, rep_gotb; /* what the last SFACK said of the link */
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

/* An arrival q that cg_sf_rx_in answered r was len bytes at now: what an
 * SFACK says the link delivered (NEW, FILL or LATE) and how far it got. */
static inline void cg_sf_rx_took(struct cg_sf_rx *s, uint32_t q, enum cg_sf_in r, uint32_t len, uint32_t now)
{
	if (r != CG_SF_NEW && r != CG_SF_FILL && r != CG_SF_LATE)
		return;
	s->gotb += len;
	if (r == CG_SF_NEW) {
		s->top = q;
		s->top_us = now;
	}
}

/* Whether the link has news for an SFACK at now (its newest sseq, the bytes
 * it took); if so, takes it as said and gives them, with when the newest
 * sseq came (rts, the receiver's clock: its one-way delay, and the
 * delivery's time) and how long ago (age, us, at most 65535). */
static inline int cg_sf_rx_ack(struct cg_sf_rx *s, uint32_t now, uint32_t *top, uint32_t *gotb,
			       uint32_t *rts, uint16_t *age)
{
	uint32_t a = now - s->top_us;

	if (!s->top || (s->top == s->rep_top && s->gotb == s->rep_gotb))
		return 0;
	*top = s->rep_top = s->top;
	*gotb = s->rep_gotb = s->gotb;
	*rts = s->top_us;
	*age = (uint16_t)(a < 65535 ? a : 65535);
	return 1;
}

/* A probe on the link said every sseq below top went before it (the link
 * keeps order; a probe goes on the same socket as the DATA): what of them
 * has not come is a gap from now. A loss at the end of what the link
 * carried shows no later DATA; this is its tail probe. A top a window or
 * more ahead says nothing. */
static inline void cg_sf_rx_sent(struct cg_sf_rx *s, uint32_t top, uint32_t now)
{
	int32_t d = (int32_t)(top - s->next);

	if (!s->started || d <= 0 || d >= (int32_t)CG_SF_WIN)
		return;
	while ((uint32_t)(top - 1 - s->base) >= CG_SF_WIN)
		cg_sf_rx_slide(s);
	if ((int32_t)(s->lo - s->base) < 0)
		s->lo = s->base;
	for (uint32_t g = s->next; g != top; g++) {
		s->st[g % CG_SF_WIN] = g ? CG_SF_GAP : CG_SF_GOT;
		s->seen[g % CG_SF_WIN] = now;
	}
	s->next = top;
	s->tail += (uint32_t)d;
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
	/* and for each one's delivery sample (BBR's): when it went, its bytes,
	 * the bytes sent up to it, and the bytes the receiver had said it took
	 * by then with when they came (the receiver's clock) */
	uint32_t at_us[CG_SF_WIN], cum[CG_SF_WIN], dgot[CG_SF_WIN], drts[CG_SF_WIN];
	uint16_t len[CG_SF_WIN];
	uint8_t flag[CG_SF_WIN];  /* CG_SF_BASE, CG_SF_APP, CG_SF_STREAM, CG_SF_LOST */
	uint32_t sentb;           /* bytes sent, wrapping */
	uint32_t got, rts, top;   /* what the newest SFACK said */
	uint8_t have_got;
	uint8_t app;              /* set by the caller: what goes now is app-limited */
	struct cg_lc lc;          /* the link's own rate (linkcc.h) */
	uint64_t sent, unsent, lost_rx, stale, repeat, acks, app_n;
};

#define CG_SF_BASE 1 /* an SFACK was known when it went: a sample may start there */
#define CG_SF_APP 2  /* it went app-limited: a smaller rate says nothing */
#define CG_SF_STREAM 4 /* it carried a STREAM (proto.h, 13): seq is that class's */
#define CG_SF_LOST 8   /* an SFLOSS said it already: the next one (its copy on another link) is a repeat */

static inline void cg_sf_tx_init(struct cg_sf_tx *t)
{
	memset(t, 0, sizeof(*t));
	t->next = 1;
	cg_lc_init(&t->lc);
}

/* The sseq for a DATA of tunnel sequence seq and len bytes going on the
 * link at now (us). */
static inline uint32_t cg_sf_tx_take(struct cg_sf_tx *t, uint32_t seq, uint32_t len, uint32_t now)
{
	uint32_t q = t->next++, i = q % CG_SF_WIN;
	int app;

	if (!t->next)
		t->next = 1;
	t->sseq[i] = q;
	t->seq[i] = seq;
	t->at_us[i] = now;
	t->len[i] = (uint16_t)len;
	t->sentb += len;
	t->cum[i] = t->sentb;
	t->dgot[i] = t->got;
	t->drts[i] = t->rts;
	app = t->app || cg_lc_idle(&t->lc);
	t->flag[i] = (uint8_t)((t->have_got ? CG_SF_BASE : 0) | (app ? CG_SF_APP : 0));
	t->app_n += (uint64_t)app;
	t->sent++;
	cg_lc_sent(&t->lc, len);
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
		t->sentb -= t->len[t->next % CG_SF_WIN];
		t->lc.inflight -= t->lc.inflight >= t->len[t->next % CG_SF_WIN] ? t->len[t->next % CG_SF_WIN] : t->lc.inflight;
		t->lc.r_sent -= t->lc.r_sent > 0;
		t->sent--;
		t->unsent++;
	}
}

/* The tunnel sequence sseq q carried, if still remembered: 1 and *seq.
 * The link lost it: its rate learns of it, once. The receiver sends each
 * SFLOSS on its two fastest links: the second report of the same sseq
 * returns 0 (repeat). */
static inline int cg_sf_tx_find_cls(struct cg_sf_tx *t, uint32_t q, uint32_t *seq, int *stream)
{
	if (q && t->sseq[q % CG_SF_WIN] == q && (t->flag[q % CG_SF_WIN] & CG_SF_LOST)) {
		t->repeat++; /* the receiver says each loss on two links: counted once */
		return 0;
	}
	t->lost_rx++;
	if (!q || t->sseq[q % CG_SF_WIN] != q) {
		t->stale++;
		return 0;
	}
	t->flag[q % CG_SF_WIN] |= CG_SF_LOST;
	*seq = t->seq[q % CG_SF_WIN];
	*stream = !!(t->flag[q % CG_SF_WIN] & CG_SF_STREAM);
	cg_lc_lost(&t->lc, t->len[q % CG_SF_WIN]);
	return 1;
}

static inline int cg_sf_tx_find(struct cg_sf_tx *t, uint32_t q, uint32_t *seq)
{
	int stream;

	return cg_sf_tx_find_cls(t, q, seq, &stream);
}

/* sseq q (cg_sf_tx_take's) carried a STREAM: its seq is that class's. */
static inline void cg_sf_tx_stream(struct cg_sf_tx *t, uint32_t q)
{
	if (q)
		t->flag[q % CG_SF_WIN] |= CG_SF_STREAM;
}

/* An SFACK said the link got as far as sseq top and took gotb bytes
 * (wrapping), top at rts on the receiver's clock and age us before it
 * answered; it came at now (us). An answer older than the last
 * (by top) says nothing; one whose bytes went back is a receiver that
 * started again: it is the new base.
 * - In flight: what went after top (the gaps below it are the receiver's
 *   to settle; SFLOSS takes what was lost).
 * - The delivery sample: the bytes taken since top went over the time
 *   between the two answers on the receiver's clock, so the answers' own
 *   delay and batching do not count; app-limited, it counts only if larger
 *   than the estimate (BBR's).
 * - Its round trip: since top went, less the receiver's wait; and top's
 *   one-way delay (rts less its send time, two clocks: only its changes
 *   count), the link's queue (linkcc.h). */
static inline void cg_sf_tx_ack(struct cg_sf_tx *t, uint32_t top, uint32_t gotb, uint32_t rts, uint16_t age,
				uint64_t now)
{
	uint32_t i = top % CG_SF_WIN, delta = 0, rtt = 0;
	int known = top && t->sseq[i] == top;
	uint64_t rate = 0;

	if (t->have_got && ((int32_t)(top - t->top) < 0 || (top == t->top && (int32_t)(gotb - t->got) <= 0)))
		return;
	if (t->have_got && (int32_t)(gotb - t->got) >= 0) {
		delta = gotb - t->got;
		if (known && (t->flag[i] & CG_SF_BASE)) {
			uint32_t dt = rts - t->drts[i];

			if (dt >= 1000 && (int32_t)dt > 0 && (int32_t)(gotb - t->dgot[i]) > 0)
				rate = (uint64_t)(gotb - t->dgot[i]) * 1000000 / dt;
			if ((t->flag[i] & CG_SF_APP) && rate <= t->lc.bw)
				rate = 0;
		}
	}
	if (known) {
		uint32_t r = (uint32_t)now - t->at_us[i];

		rtt = r > age ? r - age : 1;
	}
	t->got = gotb;
	t->rts = rts;
	t->top = top;
	t->have_got = 1;
	t->acks++;
	if (known)
		cg_lc_owd(&t->lc, now, (int32_t)(rts - t->at_us[i]));
	cg_lc_acked(&t->lc, now, delta, rate, rtt);
	if (known)
		t->lc.inflight = (uint32_t)(t->sentb - t->cum[i]);
}

/* An SFACK's payload: a count, then for each link its id, its newest sseq,
 * the bytes it took, when the last came (rts) and the newest's age (15
 * bytes, big-endian). */
#define CG_SFACK_ENTRY 15u
#define CG_SFACK_US 10000u /* at most one SFACK each way this often */
#define CG_SFACK_LEN (1 + 16 * CG_SFACK_ENTRY)

struct cg_sfack_e {
	uint8_t link;
	uint32_t top, gotb, rts;
	uint16_t age;
};

static inline void cg_sfack_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t cg_sfack_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Writes entry k (the count becomes k + 1: entries go in order). */
static inline void cg_sfack_put(uint8_t *out, unsigned k, const struct cg_sfack_e *e)
{
	uint8_t *p = out + 1 + CG_SFACK_ENTRY * k;

	p[0] = e->link;
	cg_sfack_be32(p + 1, e->top);
	cg_sfack_be32(p + 5, e->gotb);
	cg_sfack_be32(p + 9, e->rts);
	p[13] = (uint8_t)(e->age >> 8);
	p[14] = (uint8_t)e->age;
	out[0] = (uint8_t)(k + 1);
}

/* Entry k of an SFACK's payload (k below payload[0]; proto.c checked the
 * length). */
static inline void cg_sfack_get(const uint8_t *in, unsigned k, struct cg_sfack_e *e)
{
	const uint8_t *p = in + 1 + CG_SFACK_ENTRY * k;

	e->link = p[0];
	e->top = cg_sfack_rd32(p + 1);
	e->gotb = cg_sfack_rd32(p + 5);
	e->rts = cg_sfack_rd32(p + 9);
	e->age = (uint16_t)(p[13] << 8 | p[14]);
}

/* A tick (100 ms): the window narrows back toward its floor. */
static inline void cg_sf_rx_decay(struct cg_sf_rx *s)
{
	s->reo_us = s->reo_us > s->floor_us + CG_SF_REO_DECAY_US ? s->reo_us - CG_SF_REO_DECAY_US : s->floor_us;
}

#endif
