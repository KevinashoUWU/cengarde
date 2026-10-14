/* SPDX-License-Identifier: GPL-2.0-only */
#include "pace.h"
#include "test.h"

static void test_pace_rate(void)
{
	/* no capacity: no limit */
	CHECK(cg_pace_rate(0, 5000, 20000) == CG_PACE_NONE);
	/* 40 Mbit/s: 5 MB/s, 5/4 of it with room, itself with a queue or none known */
	CHECK_EQ(cg_pace_rate(40000000, 5000, 20000), 6250000);
	CHECK_EQ(cg_pace_rate(40000000, 15000, 20000), 5000000);
	CHECK_EQ(cg_pace_rate(40000000, 90000, 20000), 5000000);
	CHECK_EQ(cg_pace_rate(40000000, -1, 20000), 5000000);
	CHECK_EQ(cg_pace_rate(1, 0, 20000), 1); /* never 0: that would stop it */
}

static void test_pace_changed(void)
{
	CHECK(!cg_pace_changed(1000, 1000));
	CHECK(!cg_pace_changed(1600, 1700) && !cg_pace_changed(1700, 1600));
	CHECK(cg_pace_changed(1600, 1800) && cg_pace_changed(1800, 1600));
	CHECK(cg_pace_changed(CG_PACE_NONE, 1000) && cg_pace_changed(1000, CG_PACE_NONE));
}

void test_pace(void)
{
	test_pace_rate();
	test_pace_changed();
}
