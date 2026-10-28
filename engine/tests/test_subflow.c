/* subflow.h: one link as a subflow of MPGRIT, the receiver's side.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "subflow.h"
#include "test.h"

#define MS 1000u

/* In order: nothing lost; a gap filled within the window is reordering,
 * not loss; one left past it is lost, once. */
static void test_sf_basic(void)
{
	struct cg_sf_rx s;
	uint32_t out[8];

	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	for (uint32_t q = 100; q < 110; q++)
		CHECK_EQ(cg_sf_rx_in(&s, q, q * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_poll(&s, 200 * MS, out, 8), 0);
	/* 110 missing, 111 came: filled 10 ms later, within 20 */
	CHECK_EQ(cg_sf_rx_in(&s, 111, 300 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_poll(&s, 305 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_in(&s, 110, 310 * MS), CG_SF_FILL);
	CHECK_EQ(cg_sf_rx_poll(&s, 400 * MS, out, 8), 0);
	/* 112 missing behind 113: lost 20 ms after 113 came */
	CHECK_EQ(cg_sf_rx_in(&s, 113, 500 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_poll(&s, 519 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_poll(&s, 520 * MS, out, 8), 1);
	CHECK_EQ(out[0], 112);
	CHECK_EQ(cg_sf_rx_poll(&s, 600 * MS, out, 8), 0);
	CHECK_EQ(s.lost, 1);
	CHECK_EQ(cg_sf_rx_in(&s, 113, 601 * MS), CG_SF_DUP);
	CHECK_EQ(cg_sf_rx_in(&s, 50, 602 * MS), CG_SF_OLD);
}

/* A loss that comes after all widens the window, up to its cap; the window
 * narrows back to its floor by itself. */
static void test_sf_late(void)
{
	struct cg_sf_rx s;
	uint32_t out[8], q = 1, t = 100 * MS;

	cg_sf_rx_init(&s, 20 * MS, 40 * MS);
	cg_sf_rx_in(&s, 0, 0); /* the subflow starts at 0 */
	for (int i = 0; i < 10; i++) {
		/* q missing, q+1 here; called lost at +20; q comes at +30 */
		cg_sf_rx_in(&s, q + 1, t);
		CHECK_EQ(cg_sf_rx_poll(&s, t + s.reo_us, out, 8), 1);
		CHECK_EQ(cg_sf_rx_in(&s, q, t + 30 * MS), CG_SF_LATE);
		q += 2;
		t += 100 * MS;
	}
	CHECK_EQ(s.late, 10);
	CHECK_EQ(s.reo_us, 40 * MS); /* 20 + 10 × 4, capped at 40 */
	/* now a 30 ms reordering is within it */
	cg_sf_rx_in(&s, q + 1, t);
	CHECK_EQ(cg_sf_rx_poll(&s, t + 30 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_in(&s, q, t + 30 * MS), CG_SF_FILL);
	for (int i = 0; i < 1000; i++)
		cg_sf_rx_decay(&s);
	CHECK_EQ(s.reo_us, 20 * MS);
}

/* A jump past the window gives up what it leaves behind; the sequence
 * wraps. */
static void test_sf_edges(void)
{
	struct cg_sf_rx s;
	uint32_t out[8];

	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	cg_sf_rx_in(&s, 0, 0);
	cg_sf_rx_in(&s, 2, 1 * MS); /* 1 is a gap */
	CHECK_EQ(cg_sf_rx_in(&s, 2 + CG_SF_WIN + 5, 2 * MS), CG_SF_NEW);
	CHECK_EQ(s.overflow, 1);
	CHECK_EQ(s.lost, 1);
	/* the wrap */
	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	CHECK_EQ(cg_sf_rx_in(&s, 0xFFFFFFFEu, 0), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_in(&s, 0xFFFFFFFFu, 1 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_in(&s, 1, 2 * MS), CG_SF_NEW); /* 0 is a gap */
	CHECK_EQ(cg_sf_rx_poll(&s, 30 * MS, out, 8), 1);
	CHECK_EQ(out[0], 0);
	CHECK_EQ(cg_sf_rx_in(&s, 0, 40 * MS), CG_SF_LATE);
}

void test_subflow(void)
{
	test_sf_basic();
	test_sf_late();
	test_sf_edges();
}
