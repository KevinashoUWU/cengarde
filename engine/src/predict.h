/* split_predict's damping (story 013, 2026-10-16): when the split may take
 * a link's queue to be where it will be in a second, and by how much.
 *
 * The first split_predict (PR #67) added the queue's smoothed slope plus
 * half its second derivative to the queue the split sees, after 4 trend
 * samples over 10 ms/s, and kept it while the slope stayed over 0. In the
 * field (an RTMP upload of 8 Mbit/s over 4 cellular links, 90 s per
 * condition) it did not keep traffic off links about to queue (0 of 52
 * jumps were followed by a queue rising on the link that gained), it moved
 * traffic more often: 52 jumps of a link's share against 21-35 without it,
 * and each move reorders arrivals, so 773 rescue asks against 312-434 and
 * 36 gaps given up against 6-12. Its term followed every swing of a
 * cellular delay, and led the real queue by 0-0.4 s on the neighbourhood's
 * short ramps (from under 20 to 82-179 ms in 1-4 s). What it is for is the
 * controlled interferer's ramp: a phone loading the same cell grew the
 * queue 35-40 ms each second for 10 s (to 350-500 ms; 2 s at 40 Mbit/s).
 * So here:
 *
 * - A slope that measures (cg_pred_slope): the least-squares slope of the
 *   queue's samples of the last CG_PRED_WIN_MS, not an exponential average
 *   of differences (with a delay swinging over 30 ms every 100 ms, that
 *   average swings by ~45 ms/s, a standard deviation; this one by ~6).
 * - Engaged only on a strong slope (CG_PRED_ON_S1, 30 ms/s: over the 20-28
 *   ms/s of a 3-4 s neighbourhood ramp to ~100 ms, under the interferer's
 *   35-40) for CG_PRED_ON_N samples in a row; in the unit tests, a ramp of
 *   35-40 ms/s under 20 ms of swing engages it in 0.9-1.5 s, random swings
 *   of 30 ms never. A ramp of 1-2 s to ~100 ms is as steep as the
 *   interferer's start (40-85 ms/s): nothing tells them apart within a
 *   second, so it may engage it once; the rest of the damping keeps that to
 *   one step up and one down.
 * - Hysteresis: engaged, it stays so until the slope has been under
 *   CG_PRED_OFF_S1 for CG_PRED_OFF_MS; released, it does not engage again
 *   for CG_PRED_HOLD_MS. An engagement shorter than CG_PRED_SHORT_MS was a
 *   short ramp, not a load: the next hold is twice as long, up to
 *   CG_PRED_FAIL_MAX doublings (20 s), forgotten after CG_PRED_FORGET_MS
 *   without one (a queue swinging 0-50 ms every 2 s engages it every ~6 s
 *   without this, every ~21 s with it).
 * - Rate limit: the queue it adds (add_us) is the slope over a second,
 *   rounded down to CG_PRED_Q_US (at least that, at most CG_PRED_MAX_US),
 *   no second derivative; set when it engages, then changed at most every
 *   CG_PRED_STEP_MS and only by a whole step, 0 on release. Between steps
 *   the split sees the queue move as it does without split_predict, so a
 *   link's share moves only as often as without it, plus at most 2 steps
 *   a second while engaged.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PREDICT_H
#define CG_PREDICT_H

#include <stdint.h>

#define CG_PRED_RING 16        /* queue samples kept (at least 100 ms apart: over the window) */
#define CG_PRED_WIN_MS 1300    /* the slope's window */
#define CG_PRED_MIN_N 6        /* samples in it for a slope, over at least half of it */
#define CG_PRED_ON_S1 30000    /* engage: a slope (us of queue per second) at least this */
#define CG_PRED_ON_N 2         /* for this many samples in a row */
#define CG_PRED_OFF_S1 5000    /* release: a slope under this */
#define CG_PRED_OFF_MS 500u    /* for this long */
#define CG_PRED_HOLD_MS 5000u  /* released: no engaging again for this long */
#define CG_PRED_SHORT_MS 3000u /* an engagement shorter than this: the next hold doubles */
#define CG_PRED_FAIL_MAX 2     /* up to this many times */
#define CG_PRED_FORGET_MS 60000u /* back to CG_PRED_HOLD_MS after this long without */
#define CG_PRED_STEP_MS 500u   /* add_us changes at most this often */
#define CG_PRED_Q_US 20000     /* and in steps of this */
#define CG_PRED_MAX_US 500000  /* up to this */

struct cg_pred {
	uint64_t t[CG_PRED_RING]; /* the samples: when (ms), */
	int32_t q[CG_PRED_RING];  /* and the queue (us) */
	uint8_t n, head;          /* how many, and the next to write */
	uint8_t on;               /* engaged */
	uint8_t up_n;             /* not engaged: samples in a row with a strong slope */
	int32_t add_us;           /* the queue added to the split's, 0 unless on */
	int32_t slope;            /* the last slope (us per second), 0: none yet */
	uint64_t down_ms;         /* on: since when the slope is under CG_PRED_OFF_S1 (0: not) */
	uint64_t on_ms;           /* when it last engaged */
	uint64_t rel_ms;          /* the last release (0: none) */
	uint8_t fail_n;           /* short engagements in a row: the hold's doublings */
	uint64_t step_ms;         /* the last change of add_us */
	uint32_t engaged, steps;  /* times engaged, and changes of add_us (for the status) */
};

/* The least-squares slope (us per second) of the samples of the last
 * CG_PRED_WIN_MS before now_ms into *s; 0: too few of them. */
static inline int cg_pred_slope(const struct cg_pred *p, uint64_t now_ms, int64_t *s)
{
	int64_t n = 0, st = 0, sq = 0, stt = 0, stq = 0, den;
	uint64_t first = now_ms;

	for (unsigned k = 0; k < p->n; k++) {
		unsigned i = (p->head + CG_PRED_RING - 1 - k) % CG_PRED_RING;
		int64_t t;

		if (now_ms - p->t[i] > CG_PRED_WIN_MS)
			break;
		t = (int64_t)(now_ms - p->t[i]); /* ms back: the slope's sign flips */
		first = p->t[i];
		n++;
		st += t;
		sq += p->q[i];
		stt += t * t;
		stq += t * p->q[i];
	}
	if (n < CG_PRED_MIN_N || now_ms - first < CG_PRED_WIN_MS / 2)
		return 0;
	den = n * stt - st * st;
	if (den <= 0)
		return 0;
	*s = -(n * stq - st * sq) * 1000 / den;
	return 1;
}

/* What add_us goes to for a slope: the queue in a second, in whole steps. */
static inline int32_t cg_pred_target(int64_t s1)
{
	if (s1 > CG_PRED_MAX_US)
		s1 = CG_PRED_MAX_US;
	s1 -= s1 % CG_PRED_Q_US;
	return s1 < CG_PRED_Q_US ? CG_PRED_Q_US : (int32_t)s1;
}

/* A queue sample q_us (>= 0) at now_ms, at least 100 ms after the last. */
static inline void cg_pred_update(struct cg_pred *p, int32_t q_us, uint64_t now_ms)
{
	int64_t s1;
	int32_t to;

	p->t[p->head] = now_ms;
	p->q[p->head] = q_us;
	p->head = (uint8_t)((p->head + 1) % CG_PRED_RING);
	if (p->n < CG_PRED_RING)
		p->n++;
	if (!cg_pred_slope(p, now_ms, &s1)) {
		p->up_n = 0;
		return;
	}
	p->slope = (int32_t)(s1 > INT32_MAX ? INT32_MAX : s1 < -INT32_MAX ? -INT32_MAX : s1);
	if (!p->on) {
		p->up_n = s1 >= CG_PRED_ON_S1 ? (uint8_t)(p->up_n < 255 ? p->up_n + 1 : 255) : 0;
		if (p->rel_ms && now_ms - p->rel_ms >= CG_PRED_FORGET_MS)
			p->fail_n = 0;
		if (p->up_n >= CG_PRED_ON_N && (!p->rel_ms || now_ms - p->rel_ms >= (uint64_t)CG_PRED_HOLD_MS << p->fail_n)) {
			p->on = 1;
			p->on_ms = now_ms;
			p->engaged++;
			p->down_ms = 0;
			p->add_us = cg_pred_target(s1);
			p->step_ms = now_ms;
			p->steps++;
		}
		return;
	}
	if (s1 >= CG_PRED_OFF_S1)
		p->down_ms = 0;
	else if (!p->down_ms)
		p->down_ms = now_ms;
	if (p->down_ms && now_ms - p->down_ms >= CG_PRED_OFF_MS) {
		p->on = 0;
		p->up_n = 0;
		p->add_us = 0;
		p->rel_ms = now_ms;
		p->steps++;
		if (now_ms - p->on_ms < CG_PRED_SHORT_MS)
			p->fail_n = p->fail_n < CG_PRED_FAIL_MAX ? (uint8_t)(p->fail_n + 1) : CG_PRED_FAIL_MAX;
		else
			p->fail_n = 0;
		return;
	}
	to = cg_pred_target(s1);
	if (now_ms - p->step_ms >= CG_PRED_STEP_MS && to != p->add_us) {
		p->add_us = to;
		p->step_ms = now_ms;
		p->steps++;
	}
}

#endif
