/* The clockwork's log: the last CG_DEC_N decisions of one send direction
 * (the client's upload, each server session's download), for the status
 * (story 012's audit of 2026-10-17: "relojería"). Each guard and measure
 * that changes how packets are shared out leaves an entry when it changes
 * its mind: G1 (muting), G3 (a link whose peer went silent), G4 (a link
 * that leaves the split or comes back, and its stress), the copies' budget,
 * the capacity's cuts and fast start, the governor's weight crossing half
 * the top, and the rescue's budget running low.
 *
 * Only transitions are recorded, from the ticks (about ten a second), never
 * per packet; the decision functions stay pure and say what changed (masks,
 * flags, counters), and their callers record it here. The ring keeps the
 * newest CG_DEC_N entries and counts every mechanism since the start.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DECISION_H
#define CG_DECISION_H

#include <stdint.h>
#include <string.h>

#define CG_DEC_N 64 /* entries kept: a power of two */
#define CG_DEC_NOLINK 0xffu /* an entry about the direction, not one link */

_Static_assert(!(CG_DEC_N & (CG_DEC_N - 1)), "CG_DEC_N is a power of two");

/* What decided. from, to and why mean what the comment says; why's unit is
 * cg_dec_why_name's. */
enum cg_dec_mech {
	CG_DEC_MUTE = 0,      /* G1: from/to the health state (0 active, 1 muted); why: behind_us */
	CG_DEC_UNMUTE,        /* G1: an unmute that is a try; the same */
	CG_DEC_UNMUTE_FORCED, /* G1: promoted (too few active) or muting off; the same */
	CG_DEC_STALE,         /* G3: the peer went silent on it: 0 -> 1; why: ms since its last packet */
	CG_DEC_FRESH,         /* G3: it answers again: 1 -> 0; the same */
	CG_DEC_LEAVE_QUEUE,   /* G4: was in, its queue grew; from/to the G4 state (enum cg_g4_state); why: behind_us */
	CG_DEC_LEAVE_STALL,   /* G4: was in, its delay grew as fast as time; the same */
	CG_DEC_LEAVE_BASE,    /* G4: was not in, and is too far behind; the same */
	CG_DEC_REJOIN_SETTLE, /* G4: within 3/4 of the spread for settle_ms; the same */
	CG_DEC_REJOIN_RECOVER, /* G4: it became the fastest (split_stress_rejoin); the same */
	CG_DEC_REJOIN_EMPTY,  /* G4: every fresh link had left: the fastest is taken back; the same */
	CG_DEC_REJOIN_OFF,    /* G4: split_spread_ms went to 0; the same */
	CG_DEC_STRESS_ON,     /* G4: a link that was in left for its queue (cg_sdup copies); why: queue leaves so far */
	CG_DEC_STRESS_OFF,    /* G4: no such link left any more; the same */
	CG_DEC_RECOVER_ON,    /* G4: the fastest link is one that left; why: recoveries so far */
	CG_DEC_RECOVER_OFF,   /* G4: no longer; the same */
	CG_DEC_COPIES_DRY,    /* cg_sdup: under stress and out of budget: 0 -> 1; why: bytes left */
	CG_DEC_COPIES_OK,     /* cg_sdup: copying again, or the stress ended: 1 -> 0; the same */
	CG_DEC_CUT_FULL,      /* capacity: full (a queue above sat_us): from/to the capacity, bps; why: queue_us */
	CG_DEC_CUT_SHORT,     /* capacity: its packets stop arriving; the same */
	CG_DEC_RESTORE_REST,  /* capacity, fast start: not full for 10 s, back to half its best; the same */
	CG_DEC_GOV_DOWN,      /* governor: the weight fell below half the top: from/to the weight; why: queue_us */
	CG_DEC_GOV_UP,        /* governor: back at half the top or above; the same */
	CG_DEC_RESCUE_FULL_ON, /* rescue: its budget ran low (CG_BOND_FULL): 0 -> 1; why: credit, bytes */
	CG_DEC_RESCUE_FULL_OFF, /* rescue: room again: 1 -> 0; the same */
	CG_DEC_CAP_EXPIRE,    /* capacity: out of the send set too long, halved (capacity_expire_ms); from/to bps; why: queue_us */
	CG_DEC_MECHS
};

struct cg_dec {
	uint64_t t_ms;
	int64_t from, to, why;
	uint8_t link; /* CG_DEC_NOLINK: the whole direction */
	uint8_t mech; /* enum cg_dec_mech */
};

struct cg_decring {
	struct cg_dec e[CG_DEC_N];
	uint64_t n; /* entries ever recorded: the next goes at n % CG_DEC_N */
	uint64_t total[CG_DEC_MECHS];
};

static inline void cg_dec_reset(struct cg_decring *r)
{
	memset(r, 0, sizeof(*r));
}

static inline void cg_dec_add(struct cg_decring *r, uint64_t t_ms, unsigned link, unsigned mech, int64_t from,
			      int64_t to, int64_t why)
{
	struct cg_dec *d = &r->e[r->n & (CG_DEC_N - 1)];

	if (mech >= CG_DEC_MECHS)
		return;
	d->t_ms = t_ms;
	d->link = (uint8_t)(link > CG_DEC_NOLINK ? CG_DEC_NOLINK : link);
	d->mech = (uint8_t)mech;
	d->from = from;
	d->to = to;
	d->why = why;
	r->n++;
	r->total[mech]++;
}

/* How many entries the ring holds. */
static inline unsigned cg_dec_count(const struct cg_decring *r)
{
	return r->n < CG_DEC_N ? (unsigned)r->n : CG_DEC_N;
}

/* The k-th newest entry (0: the newest), NULL past the ones held. */
static inline const struct cg_dec *cg_dec_get(const struct cg_decring *r, unsigned k)
{
	if (k >= cg_dec_count(r))
		return NULL;
	return &r->e[(r->n - 1 - k) & (CG_DEC_N - 1)];
}

/* An edge of a 0/1 state, for the callers that keep the previous one:
 * records on_mech (0 -> 1) or off_mech (1 -> 0) and updates *prev; 1 when
 * it recorded. */
static inline int cg_dec_edge(struct cg_decring *r, uint8_t *prev, int now, uint64_t t_ms, unsigned link,
			      unsigned on_mech, unsigned off_mech, int64_t why)
{
	now = !!now;
	if (now == *prev)
		return 0;
	cg_dec_add(r, t_ms, link, now ? on_mech : off_mech, *prev, now, why);
	*prev = (uint8_t)now;
	return 1;
}

static inline const char *cg_dec_name(unsigned mech)
{
	static const char *const n[CG_DEC_MECHS] = {
		"mute",         "unmute",         "unmute_forced", "stale",          "fresh",
		"leave_queue",  "leave_stall",    "leave_base",    "rejoin_settle",  "rejoin_recover",
		"rejoin_empty", "rejoin_off",     "stress_on",     "stress_off",     "recover_on",
		"recover_off",  "copies_dry",     "copies_ok",     "cut_full",       "cut_short",
		"restore_rest", "gov_down",       "gov_up",        "rescue_full_on", "rescue_full_off",
		"cap_expire",
	};

	return mech < CG_DEC_MECHS ? n[mech] : "?";
}

/* The unit of an entry's why, as its key in the status. */
static inline const char *cg_dec_why_name(unsigned mech)
{
	switch (mech) {
	case CG_DEC_MUTE:
	case CG_DEC_UNMUTE:
	case CG_DEC_UNMUTE_FORCED:
	case CG_DEC_LEAVE_QUEUE:
	case CG_DEC_LEAVE_STALL:
	case CG_DEC_LEAVE_BASE:
	case CG_DEC_REJOIN_SETTLE:
	case CG_DEC_REJOIN_RECOVER:
	case CG_DEC_REJOIN_EMPTY:
	case CG_DEC_REJOIN_OFF:
		return "behind_us";
	case CG_DEC_STALE:
	case CG_DEC_FRESH:
		return "silent_ms";
	case CG_DEC_STRESS_ON:
	case CG_DEC_STRESS_OFF:
		return "queue_leaves";
	case CG_DEC_RECOVER_ON:
	case CG_DEC_RECOVER_OFF:
		return "recoveries";
	case CG_DEC_COPIES_DRY:
	case CG_DEC_COPIES_OK:
	case CG_DEC_RESCUE_FULL_ON:
	case CG_DEC_RESCUE_FULL_OFF:
		return "credit_bytes";
	case CG_DEC_CUT_FULL:
	case CG_DEC_CUT_SHORT:
	case CG_DEC_RESTORE_REST:
	case CG_DEC_CAP_EXPIRE:
	case CG_DEC_GOV_DOWN:
	case CG_DEC_GOV_UP:
		return "queue_us";
	default:
		return "why";
	}
}

#endif
