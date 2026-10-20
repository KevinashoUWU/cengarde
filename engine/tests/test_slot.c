/* Starlink's slot clock (slot.h, story 015, task #65): the edges at :12,
 * :27, :42 and :57 of the wall clock, the window around them, and the
 * copies of starlink_edge_copies.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "proto.h" /* CG_F_SPLIT */
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
	/* the copies: outside a window, on the fit Starlink links of carry only */
	CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x1, 0x7), 0x1);
	CHECK_EQ(cg_slot_solo(&s, T0 + 11950, 0x1, 0x1, 0x7), 0); /* in a window: every carrier */
	CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x0, 0x7), 0); /* not fit */
	CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x1, 0x1, 0x6), 0); /* not carrying */
	CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x5, 0x4, 0x7), 0x4);
	CHECK_EQ(cg_slot_solo(&s, T0 + 20000, 0x0, 0x7, 0x7), 0); /* no Starlink */
	/* as split DATA on the first of them, small packets copied */
	{
		uint8_t fl = 0;

		CHECK_EQ(cg_slot_pick(0x4, 1200, 300, &fl), 2);
		CHECK_EQ(fl, CG_F_SPLIT);
		fl = 0;
		CHECK_EQ(cg_slot_pick(0x6, 1200, 300, &fl), 1);
		fl = 0;
		CHECK_EQ(cg_slot_pick(0x4, 100, 300, &fl), -1);
		CHECK_EQ(cg_slot_pick(0, 1200, 300, &fl), -1);
		CHECK_EQ(fl, 0);
	}
}
