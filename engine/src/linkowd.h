/* What each link adds to a GRIT message's one-way delay (story 025, task #77:
 * GRIT and the engine as one). The receiving engine says by which link each
 * protocontroller message came (protoctl.c's cg_pctl_rx); GRIT keeps, per
 * link, the least delay of its messages over the last 5 to 10 s (two
 * buckets), and takes off each message's delay its link's least over the
 * least of every link's: what is left is the queue, not the path.
 *
 * Before, the least delay of each ACK came from whichever links its DATA had
 * taken, and the sender read the gap between a 10 ms link and a 25 ms one as
 * a queue: a download whose first bytes went by the shorter link sat at the
 * floor (the collapse, cg_scc_stuck's rebases), and STARTUP ended at the
 * first packet by the longer one. A queue on every link the data takes still
 * shows (each link's samples rise over their own least); a link only some
 * data takes cannot lift the least of the others (the engine's split moves
 * data off a queued link, GRIT slows when all of them queue).
 *
 * A message rebuilt by the tunnel's codes, or one whose link is not known,
 * gives no sample: its delay is the repair's, not a path's.
 *
 * Delays are the receiver's clock minus the sender's (any offset): only
 * their differences mean anything, compared as signed 32-bit.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LINKOWD_H
#define CG_LINKOWD_H

#include <stdint.h>
#include <string.h>

#define CG_LOWD_LINKS 16
#define CG_LOWD_NOLINK 0xffu
#define CG_LOWD_WIN_US 5000000u /* a bucket: each link's least is over the last 5 to 10 s */

struct cg_lowd {
	int32_t m[CG_LOWD_LINKS][2]; /* each link's least delay: this bucket's, the last one's */
	uint8_t have[CG_LOWD_LINKS]; /* bit 0: this bucket has one, bit 1: the last one */
	uint64_t t;                  /* this bucket's start, 0: none yet */
	uint64_t samples, corrected; /* messages with a link, and those whose delay it lowered */
};

static inline void cg_lowd_init(struct cg_lowd *o)
{
	memset(o, 0, sizeof(*o));
}

static inline void cg_lowd_roll(struct cg_lowd *o, uint64_t now)
{
	if (!o->t) {
		o->t = now ? now : 1;
		return;
	}
	if (now - o->t < CG_LOWD_WIN_US)
		return;
	for (unsigned i = 0; i < CG_LOWD_LINKS; i++) {
		/* a whole bucket gone by: the last one says nothing either */
		o->m[i][1] = o->m[i][0];
		o->have[i] = now - o->t >= 2ull * CG_LOWD_WIN_US ? 0 : (uint8_t)((o->have[i] & 1) << 1);
	}
	o->t = now;
}

/* Link i's least delay over both buckets; 0 when it has none (*ok 0). */
static inline int32_t cg_lowd_base(const struct cg_lowd *o, unsigned i, int *ok)
{
	int32_t b = 0;

	*ok = 0;
	if (i >= CG_LOWD_LINKS)
		return 0;
	if (o->have[i] & 1) {
		b = o->m[i][0];
		*ok = 1;
	}
	if ((o->have[i] & 2) && (!*ok || (int32_t)((uint32_t)o->m[i][1] - (uint32_t)b) < 0)) {
		b = o->m[i][1];
		*ok = 1;
	}
	return b;
}

/* What link i adds over the shortest link with a least, us (0: unknown). */
static inline uint32_t cg_lowd_extra(const struct cg_lowd *o, unsigned i)
{
	int32_t bi, b, lo = 0;
	int ok, any = 0;

	bi = cg_lowd_base(o, i, &ok);
	if (!ok)
		return 0;
	for (unsigned j = 0; j < CG_LOWD_LINKS; j++) {
		b = cg_lowd_base(o, j, &ok);
		if (ok && (!any || (int32_t)((uint32_t)b - (uint32_t)lo) < 0)) {
			lo = b;
			any = 1;
		}
	}
	return (int32_t)((uint32_t)bi - (uint32_t)lo) > 0 ? (uint32_t)bi - (uint32_t)lo : 0;
}

/* A message that came by link at now with delay d: into the link's least,
 * and d less what the link adds (d itself without a link). */
static inline int32_t cg_lowd_take(struct cg_lowd *o, unsigned link, int32_t d, uint64_t now)
{
	uint32_t x;

	if (link >= CG_LOWD_LINKS)
		return d;
	cg_lowd_roll(o, now);
	if (!(o->have[link] & 1) || (int32_t)((uint32_t)d - (uint32_t)o->m[link][0]) < 0)
		o->m[link][0] = d;
	o->have[link] |= 1;
	o->samples++;
	x = cg_lowd_extra(o, link);
	if (x)
		o->corrected++;
	return (int32_t)((uint32_t)d - x);
}

/* What one ACK says of the delay: of the messages since the last one, each
 * link's least (already less what the link adds), and of the links that
 * brought at least a quarter of them, the largest. The least of all, as
 * before, let a link that carried a few messages with no queue hide the
 * queue of the link that carried the rest: with the paths taken off, the
 * stream filled its main links and the 1200-byte ping's p99 rose from
 * ~90 to 250-360 ms in half the runs of lossy (task #77). */
struct cg_lowd_ack {
	int32_t m[CG_LOWD_LINKS];
	uint16_t n[CG_LOWD_LINKS];
	uint32_t total;
};

static inline void cg_lowd_ack_add(struct cg_lowd_ack *a, unsigned link, int32_t d)
{
	if (link >= CG_LOWD_LINKS)
		return;
	if (!a->n[link] || (int32_t)((uint32_t)d - (uint32_t)a->m[link]) < 0)
		a->m[link] = d;
	if (a->n[link] < UINT16_MAX)
		a->n[link]++;
	a->total++;
}

/* 1 and *d set when a link brought a quarter or more since the last ACK;
 * then the state starts again. */
static inline int cg_lowd_ack_get(struct cg_lowd_ack *a, int32_t *d)
{
	int got = 0;

	for (unsigned i = 0; i < CG_LOWD_LINKS; i++)
		if (a->n[i] && (uint32_t)a->n[i] * 4 >= a->total &&
		    (!got || (int32_t)((uint32_t)a->m[i] - (uint32_t)*d) > 0)) {
			*d = a->m[i];
			got = 1;
		}
	memset(a, 0, sizeof(*a));
	return got;
}

#endif
