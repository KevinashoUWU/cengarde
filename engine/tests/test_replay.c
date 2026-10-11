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

/* Accepts from..to (wrapping), leaving out every sequence that is a
 * multiple of `every` (0: none); returns how many it left out. */
static uint32_t run(uint32_t from, uint32_t to, uint32_t every)
{
	uint32_t skipped = 0;

	for (uint32_t s = from;; s++) {
		if (every && s % every == 0)
			skipped++;
		else
			accept(s);
		if (s == to)
			break;
	}
	return skipped;
}

/* Lost on every link: what leaves the window unmarked (replay.h). */
static void test_lost(void)
{
	uint32_t miss;

	/* Nothing lost in an unbroken stream, from wherever it starts. */
	cg_replay_reset(&r);
	run(1000, 1000 + 3 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, 0);
	/* Nor in one that starts marked (a server's new session). */
	cg_replay_reset(&r);
	cg_replay_init_marked(&r, 0x7010);
	run(0x7011, 0x7011 + 3 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, 0);

	/* Holes count once the window is past them, not before: 1..64 with
	 * 10, 20, .. 60 missing, counted when the top enters the block 128
	 * blocks after theirs (block 0 here). */
	cg_replay_reset(&r);
	miss = run(1, 64 * 128 - 1, 10);
	CHECK_EQ(miss, (64 * 128 - 1) / 10);
	CHECK_EQ(r.lost, 0); /* all of it still in the ring */
	accept(64 * 128);
	CHECK_EQ(r.lost, 6); /* block 0: 10, 20, .. 60; 0 came before the first */
	accept(64 * 129);
	CHECK_EQ(r.lost, 6 + 6); /* block 1: 70, .. 120 */
	run(64 * 129 + 1, 64 * 140, 0);
	CHECK_EQ(r.lost, 83); /* blocks 0-12: 10, 20, .. 830 */
	/* A late copy that came back inside the window is no loss. */
	cg_replay_reset(&r);
	run(100, 200, 0);
	run(202, 300, 0);
	CHECK_EQ(accept(201), CG_RP_NEW);
	run(301, 301 + CG_REPLAY_BITS + 64, 0);
	CHECK_EQ(r.lost, 0);
	/* Before the first sequence: never ours. 990 arrives after 1000, 980
	 * never: nothing counted, in its block or before. */
	cg_replay_reset(&r);
	accept(1000);
	CHECK_EQ(accept(990), CG_RP_NEW);
	run(1001, 1001 + 2 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, 0);
	/* The floor in the middle of its block: holes after it count. */
	cg_replay_reset(&r);
	accept(1000); /* block 15: 960-1023 */
	run(1002, 1023, 0);
	run(1025, 1025 + 2 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, 2); /* 1001 and 1024 */

	/* Across the wrap of the 32-bit sequence. */
	cg_replay_reset(&r);
	miss = run(0xffffe000u, 0x2000, 7);
	run(0x2001, 0x2001 + CG_REPLAY_BITS + 64, 0);
	CHECK_EQ(r.lost, miss);

	/* A jump past the whole ring (a server that took the session up goes
	 * on 2^20 ahead): what the old window held below its top counts, what
	 * the jump skipped does not, then or later. */
	cg_replay_reset(&r);
	run(5000, 5100, 0);
	run(5102, 5110, 0); /* 5101 lost */
	accept(5110 + CG_SEQ_LEAP_TEST);
	CHECK_EQ(r.lost, 1);
	run(5111 + CG_SEQ_LEAP_TEST, 5111 + CG_SEQ_LEAP_TEST + 3 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, 1);
	/* Just short of the ring: the window holds the hole, which counts once
	 * it leaves. */
	cg_replay_reset(&r);
	accept(64 * 10);
	accept(64 * 10 + CG_REPLAY_WINDOW - 1);
	CHECK_EQ(accept(64 * 10 + 1), CG_RP_NEW);
	run(64 * 10 + CG_REPLAY_WINDOW, 64 * 10 + 3 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, CG_REPLAY_WINDOW - 3);
	/* After init_marked, holes after its top count as usual. */
	cg_replay_reset(&r);
	cg_replay_init_marked(&r, 0x7010);
	miss = run(0x7011, 0x7011 + 2 * CG_REPLAY_BITS, 5);
	run(0x7012 + 2 * CG_REPLAY_BITS, 0x7012 + 4 * CG_REPLAY_BITS, 0);
	CHECK_EQ(r.lost, miss);
	/* A long session (2^31 sequences: 2.5 days at 10 kpps): the floor must
	 * stop applying once the window passed it, or serial order would put it
	 * ahead again. Steps just short of the ring, 8099 holes each. */
	cg_replay_reset(&r);
	{
		uint32_t s = 0, n = 0;

		for (; n < (1u << 31) / 8100 + 1000; n++, s += 8100)
			accept(s);
		run(s, s + CG_REPLAY_BITS + 128, 0);
		CHECK_EQ(r.lost, (uint64_t)n * 8099);
	}
	/* reset() starts it again. */
	cg_replay_reset(&r);
	CHECK_EQ(r.lost, 0);
}

/* replay.h as it was before it counted lost, verbatim but for the names:
 * the verdicts must not have moved. */
struct ref_replay {
	uint32_t top;
	uint8_t init;
	uint64_t ring[CG_REPLAY_BLOCKS];
};

static void ref_reset(struct ref_replay *w)
{
	memset(w, 0, sizeof(*w));
}

static int ref_check(const struct ref_replay *w, uint32_t seq)
{
	int32_t d;

	if (!w->init)
		return CG_RP_NEW;
	d = (int32_t)(seq - w->top);
	if (d > 0)
		return CG_RP_NEW;
	if ((uint32_t)-(int64_t)d >= CG_REPLAY_WINDOW)
		return CG_RP_OLD;
	return (w->ring[(seq >> 6) % CG_REPLAY_BLOCKS] >> (seq & 63)) & 1 ? CG_RP_DUP : CG_RP_NEW;
}

static void ref_init_marked(struct ref_replay *w, uint32_t top)
{
	unsigned bit = top & 63;

	w->init = 1;
	w->top = top;
	memset(w->ring, 0xff, sizeof(w->ring));
	w->ring[(top >> 6) % CG_REPLAY_BLOCKS] = bit == 63 ? ~0ULL : (1ULL << (bit + 1)) - 1;
}

static void ref_mark(struct ref_replay *w, uint32_t seq)
{
	int32_t d;

	if (!w->init) {
		ref_reset(w);
		w->init = 1;
		w->top = seq;
	} else {
		d = (int32_t)(seq - w->top);
		if (d > 0) {
			uint32_t cur = w->top >> 6, blocks = ((seq >> 6) - cur) & ((1u << 26) - 1);

			if (blocks > CG_REPLAY_BLOCKS)
				blocks = CG_REPLAY_BLOCKS;
			for (uint32_t i = 1; i <= blocks; i++)
				w->ring[(cur + i) % CG_REPLAY_BLOCKS] = 0;
			w->top = seq;
		} else if ((uint32_t)-(int64_t)d >= CG_REPLAY_WINDOW) {
			return;
		}
	}
	w->ring[(seq >> 6) % CG_REPLAY_BLOCKS] |= 1ULL << (seq & 63);
}

/* The model of lost: sequences as 64-bit numbers that never wrap, a map of
 * those marked, and the stretches [from, to] that count. */
#define MODEL_SPAN (1u << 22)
static uint8_t marked[MODEL_SPAN / 8];

static int is_marked(uint64_t base, uint64_t s)
{
	return s >= base && s - base < MODEL_SPAN && (marked[(s - base) / 8] >> ((s - base) % 8) & 1);
}

static uint64_t unmarked(uint64_t base, uint64_t from, uint64_t to)
{
	uint64_t n = 0;

	for (uint64_t s = from; s <= to; s++)
		n += !is_marked(base, s);
	return n;
}

static uint32_t rnd_state = 0x9e3779b9u;

static uint32_t rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

/* Random streams (reordered, with holes, small and large jumps, sequences
 * behind the window) through the window and the one before lost: the same
 * verdict for every packet, and lost as the model says once the window is
 * past it all. Each trial starts near the 32-bit wrap. */
static void test_verdicts(void)
{
	static struct ref_replay ref;
	unsigned bad = 0;

	for (int trial = 0; trial < 24; trial++) {
		uint64_t base = 0xffff0000u - (uint64_t)(rnd() % 4096) - 20000, top, from, expect = 0;
		uint64_t s = base + 20000;
		int born_marked = trial % 3 == 2;

		memset(marked, 0, sizeof(marked));
		cg_replay_reset(&r);
		ref_reset(&ref);
		if (born_marked) {
			cg_replay_init_marked(&r, (uint32_t)s);
			ref_init_marked(&ref, (uint32_t)s);
			for (uint64_t x = base; x <= s; x++) /* everything behind it is marked */
				marked[(x - base) / 8] |= (uint8_t)(1u << ((x - base) % 8));
		}
		top = s;
		from = s;
		for (int op = 0; op < 40000; op++) {
			uint32_t k = rnd() % 1000;
			int v, vr;

			if (op == 20000 && trial % 2)
				s = top + CG_SEQ_LEAP_TEST; /* a server back from a restart */
			else if (k < 700)
				s = top + 1 + (rnd() % 3 == 0); /* in order, a hole now and then */
			else if (k < 900)
				s = top - rnd() % 200; /* reordered, or a copy */
			else if (k < 960)
				s = top - (CG_REPLAY_WINDOW - 100) - rnd() % 200; /* around the window's edge */
			else if (k < 998)
				s = top + 1 + rnd() % 300; /* a burst lost */
			else
				s = top + CG_REPLAY_WINDOW - 100 + rnd() % 300; /* around the ring */
			if (s < base || s - base >= MODEL_SPAN - 2 * CG_REPLAY_BITS)
				break;
			v = cg_replay_check(&r, (uint32_t)s);
			vr = ref_check(&ref, (uint32_t)s);
			bad += v != vr;
			if (v != CG_RP_NEW)
				continue;
			if (!r.init) {
				from = top = s;
			} else if (s > top && (s >> 6) - (top >> 6) >= CG_REPLAY_BLOCKS) {
				expect += unmarked(base, from, top);
				from = s;
			}
			cg_replay_mark(&r, (uint32_t)s);
			ref_mark(&ref, (uint32_t)s);
			marked[(s - base) / 8] |= (uint8_t)(1u << ((s - base) % 8));
			if (s > top)
				top = s;
		}
		/* Unbroken to the end: everything up to top leaves the ring. */
		expect += unmarked(base, from, top);
		for (uint64_t x = top + 1; x <= top + CG_REPLAY_BITS + 128; x++) {
			bad += (int)cg_replay_check(&r, (uint32_t)x) != ref_check(&ref, (uint32_t)x);
			cg_replay_mark(&r, (uint32_t)x);
			ref_mark(&ref, (uint32_t)x);
		}
		bad += memcmp(r.ring, ref.ring, sizeof(r.ring)) != 0 || r.top != ref.top || r.init != ref.init;
		CHECK_EQ(r.lost, expect);
	}
	CHECK_EQ(bad, 0);
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

	test_lost();
	test_verdicts();
}
