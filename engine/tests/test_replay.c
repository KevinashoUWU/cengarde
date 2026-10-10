/* SPDX-License-Identifier: GPL-2.0-only */
#include "replay.h"

#define CG_SEQ_LEAP_TEST (1u << 20)
#include "test.h"

static struct cg_replay r;

/* check-then-mark, as the receive path does after a valid MAC. */
static int accept(uint32_t seq)
{
	int v = cg_replay_check(&r, seq);

	if (v == CG_RP_NEW)
		cg_replay_mark(&r, seq);
	return v;
}

void test_replay(void)
{
	cg_replay_reset(&r);

	/* First packet anchors the window wherever it lands. */
	CHECK_EQ(accept(1000), CG_RP_NEW);
	CHECK_EQ(accept(1000), CG_RP_DUP);
	CHECK_EQ(accept(1001), CG_RP_NEW);
	CHECK_EQ(accept(999), CG_RP_NEW); /* reordered, inside the window */
	CHECK_EQ(accept(999), CG_RP_DUP);

	/* check() alone never marks: a forged packet must not shadow the real one. */
	CHECK_EQ(cg_replay_check(&r, 2000), CG_RP_NEW);
	CHECK_EQ(cg_replay_check(&r, 2000), CG_RP_NEW);

	/* Window edge: top - (WINDOW-1) is still tracked, top - WINDOW is OLD. */
	cg_replay_reset(&r);
	CHECK_EQ(accept(100000), CG_RP_NEW);
	CHECK_EQ(accept(100000 - (CG_REPLAY_WINDOW - 1)), CG_RP_NEW);
	CHECK_EQ(accept(100000 - CG_REPLAY_WINDOW), CG_RP_OLD);

	/* Sliding forward clears stale bits: a sequence seen a full ring ago is NEW
	 * again only once it is back inside the window, i.e. never for real
	 * traffic; here we check that bits of reused blocks were cleared. */
	cg_replay_reset(&r);
	for (uint32_t s = 0; s < 3 * CG_REPLAY_BITS; s++)
		CHECK_EQ(accept(s), CG_RP_NEW);
	for (uint32_t s = 3 * CG_REPLAY_BITS - CG_REPLAY_WINDOW + 1; s < 3 * CG_REPLAY_BITS; s += 97)
		CHECK_EQ(accept(s), CG_RP_DUP);

	/* Big jump forward empties the window. */
	CHECK_EQ(accept(5000000), CG_RP_NEW);
	CHECK_EQ(accept(5000000 - 1), CG_RP_NEW);
	CHECK_EQ(accept(5000000 - 1), CG_RP_DUP);

	/* Wrap-around of the 32-bit sequence space. */
	cg_replay_reset(&r);
	CHECK_EQ(accept(0xfffffff0u), CG_RP_NEW);
	for (uint32_t s = 0xfffffff1u; s != 0x20; s++)
		CHECK_EQ(accept(s), CG_RP_NEW);
	CHECK_EQ(accept(0xfffffffeu), CG_RP_DUP);
	CHECK_EQ(accept(0x05), CG_RP_DUP);
	CHECK_EQ(accept(0x20), CG_RP_NEW);
	CHECK_EQ(cg_replay_check(&r, 0x20 - CG_REPLAY_WINDOW), CG_RP_OLD);

	/* A window born marked: everything up to top is DUP or OLD, later NEW. */
	cg_replay_reset(&r);
	cg_replay_init_marked(&r, 0x7000);
	CHECK_EQ(cg_replay_check(&r, 0x7000), CG_RP_DUP);
	CHECK_EQ(cg_replay_check(&r, 0x7000 - 100), CG_RP_DUP);
	CHECK_EQ(cg_replay_check(&r, 0x7000 - CG_REPLAY_WINDOW), CG_RP_OLD);
	CHECK_EQ(accept(0x7001), CG_RP_NEW);
	CHECK_EQ(accept(0x7000 + CG_SEQ_LEAP_TEST), CG_RP_NEW);
	CHECK_EQ(accept(0x7002), CG_RP_OLD); /* the window moved past it */
	/* Out of order right after: the rest of top's block is not marked. */
	cg_replay_reset(&r);
	cg_replay_init_marked(&r, 0x7010);
	CHECK_EQ(accept(0x7013), CG_RP_NEW);
	CHECK_EQ(accept(0x7011), CG_RP_NEW);
	CHECK_EQ(accept(0x7012), CG_RP_NEW);
	CHECK_EQ(accept(0x7010), CG_RP_DUP);
	CHECK_EQ(accept(0x700f), CG_RP_DUP);
	cg_replay_reset(&r);
	cg_replay_init_marked(&r, 0x703f); /* the last bit of a block */
	CHECK_EQ(accept(0x7041), CG_RP_NEW);
	CHECK_EQ(accept(0x7040), CG_RP_NEW);
	CHECK_EQ(accept(0x703f), CG_RP_DUP);
}
