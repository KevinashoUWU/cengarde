/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "capacity.h"
#include "eta.h"
#include "split.h"
#include "test.h"

#define MS 1000u

/* The basics: no rate, no pick; the earliest arrival; the backlog drains
 * at the rate and is set again by a tick. */
static void test_eta_basics(void)
{
	struct cg_eta e;

	cg_eta_reset(&e);
	CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 0), -1);
	cg_eta_set(&e, 0, 8000000, 0, 0, 0);       /* 8 Mbit/s: 1000 B take 1 ms */
	cg_eta_set(&e, 1, 8000000, 5 * MS, 0, 0);  /* 5 ms further */
	CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 0), 0); /* 1 ms against 6 */
	CHECK_EQ(e.l[0].backlog, 8000ull * 1000000);
	/* five more at once: the last ties with link 1 (6 ms each) and stays */
	for (int i = 0; i < 5; i++)
		CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 0), 0);
	CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 0), 1); /* 0 would arrive at 7 ms, 1 at 6 */
	/* 10 ms later link 0 has drained */
	CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 10 * MS), 0);
	CHECK_EQ(e.l[0].backlog, 8000ull * 1000000);
	/* a tick says link 0 holds 20 ms of queue: link 1 now */
	cg_eta_set(&e, 0, 8000000, 0, 20 * MS, 10 * MS);
	CHECK_EQ(cg_eta_pick(&e, 0x3, 1000, 10 * MS), 1);
	/* a link of the set without a rate: the caller decides */
	CHECK_EQ(cg_eta_pick(&e, 0x7, 1000, 10 * MS), -1);
}

/* Two links, 30 Mbit/s at 20 ms and 8 Mbit/s at 70 ms, fed 36 Mbit/s of
 * 1400-byte packets for 10 s: by expected arrival the packets arrive
 * nearly in order and both links are used; by weights 30:8 (the smooth
 * round-robin of split.h) most packets come behind a later one. */
static void sim(int eta, double *share1, double *ooo, double *late_ms)
{
	struct cg_eta e;
	struct cg_split sp;
	const double rate[2] = { 30e6, 8e6 }, base[2] = { 20000, 70000 };
	const uint16_t w[CG_MAX_LINKS] = { 30, 8 };
	double free_at[2] = { 0, 0 }, newest = 0, worst = 0;
	long n1 = 0, out = 0, n = 0;

	cg_eta_reset(&e);
	cg_split_reset(&sp);
	for (double t = 0; t < 10e6; t += 1400 * 8 / 36e6 * 1e6, n++) {
		uint8_t flags = 0;
		int k;
		double arr;

		if (((long)t / 100000) != ((long)(t - 311) / 100000)) /* a tick each 100 ms */
			for (unsigned i = 0; i < 2; i++)
				cg_eta_set(&e, i, (uint64_t)rate[i], (uint32_t)(base[i] - base[0]),
					   (uint32_t)(free_at[i] > t ? free_at[i] - t : 0), (uint64_t)t);
		k = eta ? cg_eta_pick(&e, 0x3, 1400, (uint64_t)t) : -1;
		if (k < 0)
			k = cg_split_pick(&sp, 0x3, 0x3, 0, w, CG_WGS_DATA, 1400, 256, &flags);
		free_at[k] = (free_at[k] > t ? free_at[k] : t) + 1400 * 8 / rate[k] * 1e6;
		arr = free_at[k] + base[k];
		if (arr < newest) {
			out++;
			if (newest - arr > worst)
				worst = newest - arr;
		} else {
			newest = arr;
		}
		n1 += k == 1;
	}
	*share1 = (double)n1 / (double)n;
	*ooo = (double)out / (double)n;
	*late_ms = worst / 1000;
}

static void test_eta_order(void)
{
	double s1, ooo, late, w_s1, w_ooo, w_late;

	sim(1, &s1, &ooo, &late);
	sim(0, &w_s1, &w_ooo, &w_late);
	CHECK(s1 > 0.05 && s1 < 0.3);  /* the slow link is used */
	CHECK(ooo < 0.02);             /* and nearly in order (1.2 %) */
	CHECK(w_ooo > 0.5 && w_s1 > 0.15); /* by weights, 79 % behind a later one */
	(void)late;
	(void)w_late;
}

/* The tick's feed (capacity.h): rates, delays over the smallest minimum,
 * queues; a link without a measure, rate 0. */
static void test_eta_feed(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_eta e;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	cg_eta_reset(&e);
	l[0].have_rate = l[1].have_rate = 1;
	l[0].max_cur = l[0].max_prev = 30000000;
	l[1].max_cur = l[1].max_prev = 8000000;
	l[0].q.have_min = l[1].q.have_min = l[2].q.have_min = 1;
	l[0].q.min_cur = l[0].q.min_prev = 0xfffff000u; /* raw delays across the wrap */
	l[1].q.min_cur = 0x0000c350u;                   /* 50 ms + 4096 us later */
	l[1].q.min_prev = 0x0000d000u;
	l[0].queue_us = 2000;
	l[1].queue_us = -1;
	cg_cap_eta(&e, l, 0x7, 1000, 1000); /* probe under 1 ms: link 0 (2 ms) is not probed */
	CHECK(e.l[0].bps == 30000000 && e.l[0].base_us == 0 && e.l[0].backlog == 30000000ull * 2000);
	CHECK(e.l[1].bps == 8000000 && e.l[1].base_us == 0xc350u + 0x1000u && e.l[1].backlog == 0);
	CHECK(e.l[2].bps == 0); /* no rate yet */
	CHECK_EQ(cg_eta_pick(&e, 0x7, 1400, 1000), -1);
	CHECK(cg_eta_pick(&e, 0x3, 1400, 1000) == 0);
	/* with room (queue under 10 ms): 5/4 of its own and 1/16 of the largest */
	cg_cap_eta(&e, l, 0x7, 10000, 1000);
	CHECK(e.l[0].bps == 30000000 + 7500000 + 1875000);
	CHECK(e.l[1].bps == 8000000); /* its queue unknown: as measured */
}

void test_eta(void)
{
	test_eta_feed();
	test_eta_basics();
	test_eta_order();
}
