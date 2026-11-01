/* Starlink's slot clock (slot.h, story 015, task #65): the edges at :12,
 * :27, :42 and :57 of the wall clock and the window around them.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "slot.h"
#include "test.h"

/* 2026-10-20 00:00:00 UTC, a whole minute */
#define T0 1792368000000ull

void test_slot(void)
{
	const struct cg_slot s = { .edge_ms = CG_SLOT_EDGE_MS, .pre_ms = 100, .post_ms = 50 };

	CHECK_EQ(T0 % 60000, 0);
	CHECK_EQ(cg_slot_since(&s, T0 + 12000), 0);
	CHECK_EQ(cg_slot_since(&s, T0 + 27000), 0);
	CHECK_EQ(cg_slot_since(&s, T0 + 57000), 0);
	CHECK_EQ(cg_slot_since(&s, T0 + 11999), 14999);
	CHECK_EQ(cg_slot_since(&s, T0), 3000);
	/* the window: 100 ms before, 50 after */
	CHECK(cg_slot_near(&s, T0 + 12000));
	CHECK(cg_slot_near(&s, T0 + 11900));
	CHECK(!cg_slot_near(&s, T0 + 11899));
	CHECK(cg_slot_near(&s, T0 + 42049));
	CHECK(!cg_slot_near(&s, T0 + 42050));
	CHECK(!cg_slot_near(&s, T0 + 20000));
	/* spans */
	CHECK(!cg_slot_touches(&s, T0 + 12050, T0 + 26899));
	CHECK(cg_slot_touches(&s, T0 + 12050, T0 + 26900));
	CHECK(cg_slot_touches(&s, T0 + 11000, T0 + 11950));
	CHECK(cg_slot_touches(&s, T0 + 12040, T0 + 12500));
	CHECK(cg_slot_touches(&s, T0, T0 + 20000)); /* a period or more */
	CHECK(!cg_slot_touches(&s, T0 + 500, T0 + 400));
	CHECK_EQ(cg_slot_until(&s, T0 + 11000), 900);
	CHECK_EQ(cg_slot_until(&s, T0 + 11950), 0);
	/* the edge's offset moves it */
	{
		const struct cg_slot o = { .edge_ms = 11950, .pre_ms = 0, .post_ms = 10 };

		CHECK(cg_slot_near(&o, T0 + 26955));
		CHECK(!cg_slot_near(&o, T0 + 27000));
	}
}
