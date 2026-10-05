/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "hist.h"
#include "test.h"

void test_hist(void)
{
	struct cg_hist h;
	uint32_t snap[CG_HIST_N], win[CG_HIST_N], v = 0;

	/* Buckets: exact below 8, then four per octave, the last one open. */
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(cg_hist_bucket(i), i);
	CHECK_EQ(cg_hist_bucket(8), 8);
	CHECK_EQ(cg_hist_bucket(9), 8);
	CHECK_EQ(cg_hist_bucket(10), 9);
	CHECK_EQ(cg_hist_bucket(15), 11);
	CHECK_EQ(cg_hist_bucket(16), 12);
	CHECK_EQ(cg_hist_bucket(60), cg_hist_bucket(56));
	CHECK(cg_hist_bucket(55) < cg_hist_bucket(56));
	CHECK_EQ(cg_hist_bucket((1u << 24) - 1), CG_HIST_N - 1);
	CHECK_EQ(cg_hist_bucket(1u << 24), CG_HIST_N - 1);
	CHECK_EQ(cg_hist_bucket(UINT32_MAX), CG_HIST_N - 1);
	/* Every bucket's range: its low value maps to it, the one before to
	 * the previous bucket, and the middle lies inside. */
	for (unsigned i = 1; i < CG_HIST_N; i++) {
		CHECK_EQ(cg_hist_bucket(cg_hist_low(i)), i);
		CHECK_EQ(cg_hist_bucket(cg_hist_low(i) - 1), i - 1);
		CHECK_EQ(cg_hist_bucket(cg_hist_mid(i)), i);
	}
	CHECK_EQ(cg_hist_low(12), 16);
	CHECK_EQ(cg_hist_mid(12), 17); /* [16, 20): 16 + (4 - 1) / 2 */
	CHECK_EQ(cg_hist_mid(20), 71); /* [64, 80) */

	/* Percentiles: 90 values of 10, 9 of 100, one of 5000. */
	memset(&h, 0, sizeof(h));
	CHECK_EQ(cg_hist_pct(h.b, 50, &v), 0);
	for (int i = 0; i < 90; i++)
		cg_hist_add(&h, 10);
	for (int i = 0; i < 9; i++)
		cg_hist_add(&h, 100);
	cg_hist_add(&h, 5000);
	CHECK_EQ(cg_hist_pct(h.b, 50, &v), 1);
	CHECK_EQ(v, 10);
	CHECK_EQ(cg_hist_pct(h.b, 90, &v), 1);
	CHECK_EQ(v, 10);
	CHECK_EQ(cg_hist_pct(h.b, 99, &v), 1);
	CHECK(v >= 96 && v < 112); /* 100's bucket: [96, 112) */
	CHECK_EQ(cg_hist_pct(h.b, 100, &v), 1);
	CHECK(v >= 4096 && v < 5120);
	CHECK_EQ(cg_hist_pct(h.b, 1, &v), 1);
	CHECK_EQ(v, 10);

	/* Windows across the 32-bit wrap of the counters. */
	memset(&h, 0, sizeof(h));
	memset(snap, 0, sizeof(snap));
	h.b[cg_hist_bucket(40)] = 0xfffffff0u;
	cg_hist_window(h.b, snap, win);
	h.b[cg_hist_bucket(40)] += 0x20; /* wraps to 0x10 */
	h.b[cg_hist_bucket(3)] += 2;
	cg_hist_window(h.b, snap, win);
	CHECK_EQ(win[cg_hist_bucket(40)], 0x20);
	CHECK_EQ(win[cg_hist_bucket(3)], 2);
	CHECK_EQ(cg_hist_pct(win, 50, &v), 1);
	CHECK(v >= 40 && v < 48);
	cg_hist_window(h.b, snap, win); /* nothing new */
	CHECK_EQ(cg_hist_pct(win, 50, &v), 0);
}
