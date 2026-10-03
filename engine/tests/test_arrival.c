/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "arrival.h"
#include "policy.h"
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

	/* Policy: silent links stop carrying payload, unless every link is silent. */
	CHECK(cg_link_live(1000, 1500, 600));
	CHECK(!cg_link_live(1000, 1700, 600));
	CHECK(!cg_link_live(0, 10, 600)); /* never heard: not live */
	CHECK(cg_link_sends(1000, 1700, 600, 0)); /* nobody live: send everywhere */
	CHECK(!cg_link_sends(1000, 1700, 600, 1));
	CHECK(cg_link_sends(1000, 1500, 600, 1));
}
