/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "fec3.h"
#include "test.h"

/* When it protects: a split over two links or more, a lone link; not
 * redundancy over several. */
static void test_f3_mode(void)
{
	CHECK_EQ(cg_f3_mode_of(1, 4), CG_F3_BOND);
	CHECK_EQ(cg_f3_mode_of(1, 2), CG_F3_BOND);
	CHECK_EQ(cg_f3_mode_of(1, 1), CG_F3_SINGLE);
	CHECK_EQ(cg_f3_mode_of(0, 1), CG_F3_SINGLE);
	CHECK_EQ(cg_f3_mode_of(0, 3), CG_F3_OFF);
	CHECK_EQ(cg_f3_mode_of(1, 0), CG_F3_OFF);
}

/* The share holds: 10 % of 1400-byte DATA in repairs of 1440 bytes is one
 * every ~10.3 packets; a tail repair after the wait, one repair of debt at
 * most. */
static void test_f3_credit(void)
{
	struct cg_f3 f;
	unsigned rep = 0, tail = 0;
	uint64_t bytes = 0;

	memset(&f, 0, sizeof(f));
	CHECK(!cg_f3_due(&f, 1000, 10));
	for (unsigned i = 0; i < 10000; i++) {
		cg_f3_earn(&f, 1400, 10, 1000 + i);
		while (cg_f3_owed(&f)) {
			cg_f3_spend(&f, cg_f3_cost(1400));
			bytes += cg_f3_cost(1400);
			rep++;
		}
	}
	CHECK_EQ(cg_f3_cost(1400), 1440);
	CHECK(rep >= 965 && rep <= 975);
	CHECK(bytes * 1000 / (10000ull * 1400) >= 99 && bytes * 1000 / (10000ull * 1400) <= 100);
	/* a burst of 3 and nothing after: the tail repair at the wait, on
	 * debt; a second burst at once has no credit for another */
	memset(&f, 0, sizeof(f));
	for (unsigned i = 0; i < 3; i++)
		cg_f3_earn(&f, 1400, 10, 5000 + i);
	CHECK(!cg_f3_owed(&f));
	CHECK_EQ(cg_f3_due_at(&f, 10000), 15000);
	CHECK(!cg_f3_due(&f, 14999, 10000));
	CHECK(cg_f3_due(&f, 15000, 10000));
	cg_f3_spend(&f, 1440);
	tail++;
	CHECK(f.credit < 0);
	CHECK(!cg_f3_due(&f, 99999, 10000)); /* nothing waits */
	cg_f3_earn(&f, 1400, 10, 100000);
	CHECK(!cg_f3_due(&f, 200000, 10000)); /* in debt over a repair */
	for (unsigned i = 0; i < 6; i++)
		cg_f3_earn(&f, 1400, 10, 100001 + i);
	CHECK(!cg_f3_due(&f, 200000, 10000));
	cg_f3_earn(&f, 1400, 10, 100007); /* 3 + 8 packets: out of debt */
	CHECK(cg_f3_due(&f, 200000, 10000));
	CHECK_EQ(tail, 1);
	cg_f3_idle(&f);
	CHECK(!f.credit && !f.fresh_us);
}

/* Repairs follow the links' DATA: 3 to 1 over 400 picks; a link out of the
 * candidates takes none; with no weight yet, evenly. */
static void test_f3_pick(void)
{
	struct cg_f3 f;
	unsigned n[CG_F3_LINKS] = { 0 };

	memset(&f, 0, sizeof(f));
	CHECK_EQ(cg_f3_pick(&f, 0), -1);
	for (unsigned i = 0; i < 400; i++)
		n[cg_f3_pick(&f, 0x6)]++;
	CHECK_EQ(n[1], 200);
	CHECK_EQ(n[2], 200);
	memset(&f, 0, sizeof(f));
	memset(n, 0, sizeof(n));
	for (unsigned t = 0; t < 40; t++) { /* 30 kB and 10 kB a tick */
		cg_f3_weigh(&f, 0, 30000ull * t);
		cg_f3_weigh(&f, 3, 10000ull * t);
	}
	CHECK(f.w[0] > 29000 && f.w[0] <= 30000);
	for (unsigned i = 0; i < 400; i++)
		n[cg_f3_pick(&f, 0x9)]++;
	CHECK(n[0] >= 298 && n[0] <= 302);
	CHECK_EQ(n[0] + n[3], 400);
	for (unsigned i = 0; i < 100; i++)
		CHECK_EQ(cg_f3_pick(&f, 0x8), 3);
	/* a counter that went back (a new socket): weight from what follows */
	cg_f3_weigh(&f, 0, 5);
	CHECK(f.w[0] < 30000);
}

/* The guard: a queue past the window, none measured. */
static void test_f3_guard(void)
{
	CHECK(!cg_f3_guard(-1, 100000));
	CHECK(!cg_f3_guard(100000, 100000));
	CHECK(cg_f3_guard(100001, 100000));
	CHECK(!cg_f3_guard(0, 0));
}

/* The lone link's gate: on at 2 per mille (or random loss), off under 1,
 * off in a cut (over half the repair rate), the knob over all of it. */
static void test_f3_gate(void)
{
	struct cg_f3 f;

	memset(&f, 0, sizeof(f));
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10)); /* clean */
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 10));  /* random loss said */
	for (int i = 1; i <= 10; i++)
		cg_f3_loss(&f, 0, 0, (uint64_t)i);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10));
	for (int i = 11; i <= 30; i++)
		cg_f3_loss(&f, 0, 10, (uint64_t)i); /* 1 % */
	CHECK(f.loss16[0] / 16 >= 9);
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10));
	cg_f3_loss(&f, 0, 10, 30); /* the same sample: once */
	for (int i = 31; i <= 33; i++)
		cg_f3_loss(&f, 0, 0, (uint64_t)i);
	CHECK(f.loss16[0] / 16 >= 1);
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10)); /* hysteresis */
	for (int i = 34; i <= 60; i++)
		cg_f3_loss(&f, 0, 0, (uint64_t)i);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 0, 10));
	for (int i = 61; i <= 90; i++)
		cg_f3_loss(&f, 0, 600, (uint64_t)i); /* a microcut, 60 % */
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 10));
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_YES, 0, 10));
	memset(&f, 0, sizeof(f));
	for (int i = 1; i <= 30; i++)
		cg_f3_loss(&f, 0, 10, (uint64_t)i);
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_NO, 1, 10));
	/* 1 % at 2 % of repairs: over half the rate, off */
	CHECK(!cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 1));
	CHECK(cg_f3_gate(&f, 0, CG_F3_SINGLE_AUTO, 1, 5));
	cg_f3_loss(&f, 0, 5000, 31);
	CHECK(f.loss16[0] / 16 <= 1000);
}

void test_fec3(void)
{
	test_f3_mode();
	test_f3_credit();
	test_f3_pick();
	test_f3_guard();
	test_f3_gate();
}
