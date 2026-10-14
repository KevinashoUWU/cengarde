/* SPDX-License-Identifier: GPL-2.0-only */
#include "pace.h"
#include "test.h"

static void test_pace_rate(void)
{
	/* no capacity: no limit */
	CHECK(cg_pace_rate(0) == CG_PACE_NONE);
	/* 40 Mbit/s: twice, 10 MB/s */
	CHECK_EQ(cg_pace_rate(40000000), 10000000);
	/* an idle link's near-nothing: the floor, 20 Mbit/s */
	CHECK_EQ(cg_pace_rate(50000), CG_PACE_FLOOR);
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
