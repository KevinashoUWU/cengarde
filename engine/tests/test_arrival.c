/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "arrival.h"
#include "test.h"

static struct cg_arrivals a;
static struct cg_link_rx rx[CG_MAX_LINKS];

#define T0 1000000u   /* a first copy's time, us */
#define LATER 1000000u /* reuse of its slot: past every deadline */

/* Reuses the slot of seq a second after T0: seq's packet is judged. */
static void retire(uint32_t seq)
{
	cg_arr_first(&a, rx, seq + CG_ARR_SLOTS, T0 + LATER, 0, 0x1);
}

static uint64_t sum(const uint64_t *h)
{
	uint64_t n = 0;

	for (unsigned i = 0; i < CG_JOINT_MASKS; i++)
		n += h[i];
	return n;
}

/* Joint lateness (story 012): the links late at each deadline, by mask. */
static void test_joint(void)
{
	memset(&a, 0, sizeof(a));
	memset(rx, 0, sizeof(rx));

	/* Deadlines met and missed: 50 ms is within 50, 50.001 is not; a copy
	 * after 150 ms is late at all three, as one that never came. */
	cg_arr_first(&a, rx, 1, T0, 0, 0x1f);
	cg_arr_dup(&a, rx, 1, T0 + 50000, 1);
	cg_arr_dup(&a, rx, 1, T0 + 50001, 2);
	cg_arr_dup(&a, rx, 1, T0 + 150001, 3); /* link 4: never */
	CHECK_EQ(a.evaluated, 0); /* judged only when its slot is reused */
	retire(1);
	CHECK_EQ(a.evaluated, 1);
	CHECK_EQ(a.expect[0x1f], 1);
	CHECK_EQ(a.late[0][0x1c], 1); /* 2, 3 and 4 */
	CHECK_EQ(a.late[1][0x18], 1); /* 3 and 4 */
	CHECK_EQ(a.late[2][0x18], 1);
	CHECK_EQ(rx[4].missed, 1); /* missed as before */
	CHECK_EQ(rx[3].missed, 0);
	/* A copy after its slot was reused changes nothing judged. */
	cg_arr_dup(&a, rx, 1, T0 + LATER + 1, 4);
	CHECK_EQ(rx[4].late, 1);
	CHECK_EQ(a.late[2][0x18], 1);

	/* All in time: bucket 0 at every deadline. */
	cg_arr_first(&a, rx, 2, T0, 1, 0x7);
	cg_arr_dup(&a, rx, 2, T0 + 3000, 0);
	cg_arr_dup(&a, rx, 2, T0 + 9000, 2);
	retire(2);
	CHECK_EQ(a.expect[0x7], 1);
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		CHECK_EQ(a.late[d][0], 1);

	/* Only the expected links count: a copy from a link not in expect
	 * (trickle, a link the client thought muted) is ignored. */
	cg_arr_first(&a, rx, 3, T0, 0, 0x3);
	cg_arr_dup(&a, rx, 3, T0 + 1000, 5);
	retire(3);
	CHECK_EQ(a.expect[0x3], 1);
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		CHECK_EQ(a.late[d][0x2], 1);

	/* Out of order (the hub's rings): link 2's copy, stamped 800 us
	 * before link 1's first, takes every deadline; link 1 keeps its own;
	 * link 0 is timed from the new first: 100 ms, within 100 and 150. */
	cg_arr_first(&a, rx, 4, T0 + 5000, 1, 0x7);
	cg_arr_dup(&a, rx, 4, T0 + 4200, 2);
	cg_arr_dup(&a, rx, 4, T0 + 4200 + 100000, 0);
	retire(4);
	CHECK_EQ(a.expect[0x7], 2);
	CHECK_EQ(a.late[0][0x1], 1);
	CHECK_EQ(a.late[1][0], 2);
	CHECK_EQ(a.late[2][0], 2);

	/* An expected link with an id of 8 or more: other, whatever came. */
	cg_arr_first(&a, rx, 5, T0, 0, 0x101);
	cg_arr_dup(&a, rx, 5, T0 + 100, 8);
	retire(5);
	CHECK_EQ(a.other, 1);
	CHECK_EQ(a.expect[0x01], 0);
	/* A slot reused within 150 ms of its first copy (more than 4096
	 * packets in 150 ms): link 1 could still be on its way, so other. */
	cg_arr_first(&a, rx, 6, T0, 0, 0x3);
	cg_arr_first(&a, rx, 6 + CG_ARR_SLOTS, T0 + 150000, 0, 0x3);
	CHECK_EQ(a.other, 2);
	cg_arr_first(&a, rx, 7, T0, 0, 0x3);
	cg_arr_first(&a, rx, 7 + CG_ARR_SLOTS, T0 + 150001, 0, 0x3);
	CHECK_EQ(a.other, 2);
	CHECK_EQ(a.expect[0x3], 2);
	/* Across the 32-bit wrap of the sequence: 2^32 is a multiple of the
	 * slots, so 0xffffffff and 4095 share one. Link 1 at 60 ms. */
	{
		uint64_t l0 = a.late[0][0x2], l1 = a.late[1][0], l2 = a.late[2][0];

		cg_arr_first(&a, rx, 0xffffffffu, T0, 0, 0x3);
		cg_arr_dup(&a, rx, 0xffffffffu, T0 + 60000, 1);
		retire(0xffffffffu); /* seq 4095 */
		CHECK_EQ(a.slot[CG_ARR_SLOTS - 1].seq, CG_ARR_SLOTS - 1);
		CHECK_EQ(a.expect[0x3], 3);
		CHECK_EQ(a.late[0][0x2], l0 + 1);
		CHECK_EQ(a.late[1][0], l1 + 1);
		CHECK_EQ(a.late[2][0], l2 + 1);
	}

	/* A run of packets against the rule worked out here: expect 0x3 or
	 * 0xf (2 and 3 never come); link 1 never on every fifth, else 0, 60,
	 * 120 or 180 ms after link 0. */
	{
		static struct cg_arrivals want;

		memset(&a, 0, sizeof(a));
		memset(&want, 0, sizeof(want));
		for (uint32_t s = 100; s < 160; s++) {
			uint16_t e = s % 3 ? 0x3 : 0xf;
			uint32_t lag = (s % 4) * 60000;

			cg_arr_first(&a, rx, s, T0, 0, e);
			if (s % 5)
				cg_arr_dup(&a, rx, s, T0 + lag, 1);
			retire(s);
			want.expect[e]++;
			for (unsigned d = 0; d < CG_JOINT_D; d++)
				want.late[d][e & ~(s % 5 && lag <= (d + 1) * 50000u ? 0x3 : 0x1)]++;
		}
		CHECK_EQ(a.evaluated, 60);
		CHECK_EQ(a.expect[0xf], 20);
		CHECK_EQ(a.expect[0x3], 40);
		CHECK(!memcmp(a.expect, want.expect, sizeof(a.expect)));
		CHECK(!memcmp(a.late, want.late, sizeof(a.late)));
		/* Every packet judged in exactly one place: expect and other add
		 * up to the packets, each deadline's late buckets to expect. */
		CHECK_EQ(sum(a.expect) + a.other, a.evaluated);
		for (unsigned d = 0; d < CG_JOINT_D; d++)
			CHECK_EQ(sum(a.late[d]), sum(a.expect));
		/* Later deadlines, fewer late. */
		CHECK(a.late[0][0x2] > a.late[1][0x2] && a.late[1][0x2] > a.late[2][0x2]);
	}
}

/* cg_arr_idle: a link idle for idle_us (no copy since), never with 0. */
static void test_arrival_idle(void)
{
	struct cg_link_rx l;

	memset(&l, 0, sizeof(l));
	CHECK(!cg_arr_idle(&l, 1000000, 0));    /* off */
	CHECK(cg_arr_idle(&l, 1000000, 500000)); /* never brought one */
	l.last_us = 900000;
	CHECK(!cg_arr_idle(&l, 1000000, 500000));
	CHECK(cg_arr_idle(&l, 1500001, 500000));
	l.last_us = 0xfffffff0u;                /* wraps */
	CHECK(!cg_arr_idle(&l, 0x10, 500000));
	/* the newest sequence: a serial max, the first copy sets it */
	memset(&l, 0, sizeof(l));
	cg_arr_top(&l, 0xfffffffeu);
	l.last_us = 1;
	CHECK_EQ(l.top, 0xfffffffeu);
	cg_arr_top(&l, 5); /* past the wrap: newer */
	CHECK_EQ(l.top, 5);
	cg_arr_top(&l, 0xffffffffu); /* older */
	CHECK_EQ(l.top, 5);
}

void test_arrival(void)
{
	test_arrival_idle();
	memset(&a, 0, sizeof(a));
	memset(rx, 0, sizeof(rx));

	/* Link 0 always first, link 1 always 1000 us later, link 2 never. */
	for (uint32_t s = 0; s < 2 * CG_ARR_SLOTS; s++) {
		cg_arr_first(&a, rx, s, s * 10, 0, 0x7);
		cg_arr_dup(&a, rx, s, s * 10 + 1000, 1);
	}
	CHECK_EQ(rx[0].wins, 2 * CG_ARR_SLOTS);
	/* the newest sequence each brought, any copy (story 023) */
	CHECK_EQ(rx[0].top, 2 * CG_ARR_SLOTS - 1);
	CHECK_EQ(rx[1].top, 2 * CG_ARR_SLOTS - 1);
	CHECK_EQ(rx[2].last_us, 0);
	CHECK_EQ(rx[1].wins, 0);
	CHECK_EQ(rx[1].dups, 2 * CG_ARR_SLOTS);
	CHECK_EQ(cg_lag_us(&rx[0]), 0);
	CHECK(cg_lag_us(&rx[1]) >= 990 && cg_lag_us(&rx[1]) <= 1000); /* integer EWMA converges from below */
	/* Missed counts once a slot is reused: the first CG_ARR_SLOTS packets. */
	CHECK_EQ(a.evaluated, CG_ARR_SLOTS);
	/* 4096 packets in 41 ms: too fast to judge their lateness. */
	CHECK_EQ(a.other, CG_ARR_SLOTS);
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

	test_joint();
}
