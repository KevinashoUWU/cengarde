/* Log2 histograms of small durations (the router's hop stamps: how long a
 * batch waited between the thread that read it and the one that took it).
 *
 * Each octave [2^e, 2^(e+1)) is split into 4 buckets, so a value is known
 * within 25 %; values below 8 are exact, and anything from 2^24 up (16.7 s
 * in microseconds) lands in the last bucket. Counters are 32-bit and wrap:
 * percentiles are taken over a window, the difference of two snapshots
 * (cg_hist_window, wrap-safe through cg_delta32), never over the running
 * totals. Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_HIST_H
#define CG_HIST_H

#include <stdint.h>

#include "util.h"

#define CG_HIST_TOP_E 23                       /* highest octave kept */
#define CG_HIST_N (4 * CG_HIST_TOP_E)          /* 92 buckets */

struct cg_hist {
	uint32_t b[CG_HIST_N];
};

static inline unsigned cg_hist_bucket(uint32_t v)
{
	unsigned e;

	if (v < 4)
		return v;
	if (v >= 1u << (CG_HIST_TOP_E + 1))
		return CG_HIST_N - 1;
	e = 31u - (unsigned)__builtin_clz(v);
	return 4 * (e - 1) + ((v >> (e - 2)) & 3);
}

/* Smallest value of bucket i. */
static inline uint32_t cg_hist_low(unsigned i)
{
	unsigned e;

	if (i < 4)
		return i;
	e = i / 4 + 1;
	return (uint32_t)(4 + i % 4) << (e - 2);
}

/* What a bucket stands for in a percentile: the middle of its range. */
static inline uint32_t cg_hist_mid(unsigned i)
{
	unsigned e;

	if (i < 8)
		return i;
	e = i / 4 + 1;
	return cg_hist_low(i) + ((1u << (e - 2)) - 1) / 2;
}

static inline void cg_hist_add(struct cg_hist *h, uint32_t v)
{
	h->b[cg_hist_bucket(v)]++;
}

/* win = cur - snap, bucket by bucket (counters wrap), then snap = cur. */
static inline void cg_hist_window(const uint32_t cur[CG_HIST_N], uint32_t snap[CG_HIST_N], uint32_t win[CG_HIST_N])
{
	for (unsigned i = 0; i < CG_HIST_N; i++) {
		win[i] = cg_delta32(cur[i], snap[i]);
		snap[i] = cur[i];
	}
}

/* The pct-th percentile (1-100) of the counts in c: *out gets the middle of
 * the bucket where it falls. Returns 0 when c is empty. */
static inline int cg_hist_pct(const uint32_t c[CG_HIST_N], unsigned pct, uint32_t *out)
{
	uint64_t total = 0, rank, sum = 0;

	for (unsigned i = 0; i < CG_HIST_N; i++)
		total += c[i];
	if (!total)
		return 0;
	if (pct > 100)
		pct = 100;
	rank = (total * pct + 99) / 100;
	if (!rank)
		rank = 1;
	for (unsigned i = 0; i < CG_HIST_N; i++) {
		sum += c[i];
		if (sum >= rank) {
			*out = cg_hist_mid(i);
			return 1;
		}
	}
	*out = cg_hist_mid(CG_HIST_N - 1); /* never: sum reaches total */
	return 1;
}

#endif
