/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "arrival.h"
#include "test.h"

static struct cg_arrivals a;
static struct cg_link_rx rx[CG_MAX_LINKS];

void test_arrival(void)
{
	memset(&a, 0, sizeof(a));
	memset(rx, 0, sizeof(rx));

	/* Link 0 always first, link 1 always 1000 us later, link 2 never. */
	for (uint32_t s = 0; s < 2 * CG_ARR_SLOTS; s++) {
		cg_arr_first(&a, rx, s, s * 10, 0, 0x7);
		cg_arr_dup(&a, rx, s, s * 10 + 1000, 1);
	}
	CHECK_EQ(rx[0].wins, 2 * CG_ARR_SLOTS);
	CHECK_EQ(rx[1].wins, 0);
	CHECK_EQ(rx[1].dups, 2 * CG_ARR_SLOTS);
	CHECK_EQ(cg_lag_us(&rx[0]), 0);
	CHECK(cg_lag_us(&rx[1]) >= 990 && cg_lag_us(&rx[1]) <= 1000); /* integer EWMA converges from below */
	/* Missed counts once a slot is reused: the first CG_ARR_SLOTS packets. */
	CHECK_EQ(a.evaluated, CG_ARR_SLOTS);
	CHECK_EQ(rx[2].missed, CG_ARR_SLOTS);
	CHECK_EQ(rx[0].missed, 0);
	CHECK_EQ(rx[1].missed, 0);

	/* A second copy from the same link is a replay: ignored. */
	cg_arr_dup(&a, rx, 2 * CG_ARR_SLOTS - 1, 0, 1);
	CHECK_EQ(rx[1].dups, 2 * CG_ARR_SLOTS);

	/* A copy whose slot was already reused is late. */
	cg_arr_dup(&a, rx, 0, 0, 2);
	CHECK_EQ(rx[2].late, 1);

	/* Out of order (the hub takes pumps' rings in turn): a copy stamped
	 * before the recorded first one takes the first place and its win. */
	memset(&a, 0, sizeof(a));
	memset(rx, 0, sizeof(rx));
	cg_arr_first(&a, rx, 7, 5000, 1, 0x7);
	cg_arr_dup(&a, rx, 7, 4200, 2);
	CHECK_EQ(rx[1].wins, 0);
	CHECK_EQ(rx[1].dups, 1);
	CHECK_EQ(rx[2].wins, 1);
	CHECK_EQ(rx[2].dups, 0);
	CHECK(cg_lag_us(&rx[1]) > 0); /* 800 us behind, smoothed */
	CHECK_EQ(cg_lag_us(&rx[2]), 0);
	/* A third copy compares with the new first. */
	cg_arr_dup(&a, rx, 7, 4600, 0);
	CHECK_EQ(rx[0].dups, 1);
	CHECK_EQ(rx[0].wins, 0);
	CHECK_EQ(rx[2].wins, 1);
	/* Equal stamps: no move. */
	cg_arr_first(&a, rx, 8, 9000, 0, 0x7);
	cg_arr_dup(&a, rx, 8, 9000, 1);
	CHECK_EQ(rx[0].wins, 1);
	CHECK_EQ(rx[1].wins, 0);
	CHECK_EQ(rx[1].dups, 2);
	/* A replayed copy on the link that already delivered stays ignored,
	 * earlier stamp or not. */
	cg_arr_dup(&a, rx, 8, 8000, 0);
	cg_arr_dup(&a, rx, 8, 8000, 1);
	CHECK_EQ(rx[0].wins, 1);
	CHECK_EQ(rx[0].dups, 1);
	CHECK_EQ(rx[1].dups, 2);
	/* Across the 32-bit wrap of the microsecond clock. */
	cg_arr_first(&a, rx, 9, 10, 0, 0x7);
	cg_arr_dup(&a, rx, 9, 0xfffffff0u, 2);
	CHECK_EQ(rx[2].wins, 2);
	CHECK_EQ(rx[0].wins, 1);
}
