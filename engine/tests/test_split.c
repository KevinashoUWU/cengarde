/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "health.h" /* cg_popcount16 */
#include "split.h"
#include "test.h"
#include "util.h" /* CG_ARRAY_SIZE */

#define BIG 1000      /* a packet well above split_small */
#define SMALL_LEN 256 /* split_small, as configured by default */

/* One packet of BIG bytes. */
static int pick(struct cg_split *s, uint16_t eligible, uint16_t dup0, const uint16_t *w)
{
	return cg_split_pick(s, (uint16_t)(eligible | dup0), eligible, dup0, w, BIG, SMALL_LEN, NULL);
}

static uint16_t bit(unsigned i)
{
	return (uint16_t)(1u << i);
}

/* Counts per link over n packets. */
static void run(struct cg_split *s, uint16_t eligible, const uint16_t *w, unsigned n, unsigned *count)
{
	memset(count, 0, CG_MAX_LINKS * sizeof(*count));
	for (unsigned k = 0; k < n; k++) {
		int p = pick(s, eligible, 0, w);

		CHECK(p >= 0 && p < CG_MAX_LINKS);
		if (p >= 0 && p < CG_MAX_LINKS)
			count[p]++;
	}
}

/* The shares follow the weights. */
static void test_split_proportions(void)
{
	static const uint16_t w11[CG_MAX_LINKS] = { 1, 1 };
	static const uint16_t w13[CG_MAX_LINKS] = { 1, 3 };
	static const uint16_t w235[CG_MAX_LINKS] = { 2, 0, 3, 0, 0, 5 };
	static const uint16_t w124[CG_MAX_LINKS] = { 1, 2, 4 };
	static const uint16_t wskew[CG_MAX_LINKS] = { 1, 1000 };
	struct cg_split s;
	unsigned n[CG_MAX_LINKS];

	/* Whole periods are exact: 10000 packets are 5000 periods of 1:1, 2500
	 * of 1:3 and 1000 of 2:3:5. */
	cg_split_reset(&s);
	run(&s, bit(0) | bit(1), w11, 10000, n);
	CHECK_EQ(n[0], 5000);
	CHECK_EQ(n[1], 5000);
	cg_split_reset(&s);
	run(&s, bit(0) | bit(1), w13, 10000, n);
	CHECK_EQ(n[0], 2500);
	CHECK_EQ(n[1], 7500);
	cg_split_reset(&s);
	run(&s, bit(0) | bit(2) | bit(5), w235, 10000, n); /* links 1, 3 and 4: weight 0, not asked */
	CHECK_EQ(n[0], 2000);
	CHECK_EQ(n[2], 3000);
	CHECK_EQ(n[5], 5000);
	CHECK_EQ(n[1] + n[3] + n[4], 0);

	/* A period that does not divide 10000: every prefix stays within one
	 * packet of the exact share, not only the end. */
	{
		unsigned m[3] = { 0, 0, 0 };
		int off = 0;

		cg_split_reset(&s);
		for (unsigned k = 1; k <= 10000; k++) {
			int p = pick(&s, bit(0) | bit(1) | bit(2), 0, w124);

			CHECK(p >= 0 && p < 3);
			if (p < 0 || p > 2)
				return;
			m[p]++;
			for (int i = 0; i < 3; i++) {
				long long d = (long long)m[i] * 7 - (long long)k * w124[i]; /* in 7ths of a packet */

				off |= d > 7 || d < -7;
			}
		}
		CHECK(!off);
		CHECK_EQ(m[0] + m[1] + m[2], 10000);
	}

	/* A very uneven pair still gives the small one its packet. */
	cg_split_reset(&s);
	run(&s, bit(0) | bit(1), wskew, 10010, n);
	CHECK_EQ(n[0], 10);
	CHECK_EQ(n[1], 10000);

	/* The biggest weights of all 16 links neither overflow nor skew. */
	{
		uint16_t wmax[CG_MAX_LINKS];

		for (int i = 0; i < CG_MAX_LINKS; i++)
			wmax[i] = 65535;
		cg_split_reset(&s);
		run(&s, 0xffff, wmax, 16 * 100, n);
		for (int i = 0; i < CG_MAX_LINKS; i++)
			CHECK_EQ(n[i], 100);
	}
}

/* The order of the picks: the heavy link's share is spread out. */
static void test_split_interleave(void)
{
	static const uint16_t w511[CG_MAX_LINKS] = { 5, 1, 1 };
	static const uint16_t w13[CG_MAX_LINKS] = { 1, 3 };
	static const uint16_t w11[CG_MAX_LINKS] = { 1, 1 };
	/* nginx's example, a a b a c a a, twice. */
	static const int want511[14] = { 0, 0, 1, 0, 2, 0, 0, 0, 0, 1, 0, 2, 0, 0 };
	static const int want13[8] = { 1, 0, 1, 1, 1, 0, 1, 1 };
	struct cg_split s;
	int run5 = 0, longest = 0;

	cg_split_reset(&s);
	for (int k = 0; k < 14; k++)
		CHECK_EQ(pick(&s, bit(0) | bit(1) | bit(2), 0, w511), want511[k]);
	cg_split_reset(&s);
	for (int k = 0; k < 8; k++)
		CHECK_EQ(pick(&s, bit(0) | bit(1), 0, w13), want13[k]);
	cg_split_reset(&s);
	for (int k = 0; k < 6; k++)
		CHECK_EQ(pick(&s, bit(0) | bit(1), 0, w11), k & 1); /* a tie goes to the lowest id */

	/* 5 of every 7 packets go to link 0, and never more than 4 in a row
	 * (a plain weighted round-robin would send all 5, back to back). */
	cg_split_reset(&s);
	for (int k = 0; k < 7000; k++) {
		run5 = pick(&s, bit(0) | bit(1) | bit(2), 0, w511) == 0 ? run5 + 1 : 0;
		longest = run5 > longest ? run5 : longest;
	}
	CHECK_EQ(longest, 4);
}

/* Only eligible links are picked, whatever else the weights say. */
static void test_split_eligible(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 1, 1, 0, 1 };
	struct cg_split s;
	unsigned n[CG_MAX_LINKS];

	/* Link 1 is not eligible (muted, stalled or stale): it gets nothing. */
	cg_split_reset(&s);
	run(&s, bit(0) | bit(2) | bit(4), w, 9000, n);
	CHECK_EQ(n[0], 3000);
	CHECK_EQ(n[1], 0);
	CHECK_EQ(n[2], 3000);
	CHECK_EQ(n[4], 3000);

	/* A weight-0 link named eligible is not picked either, nor one named
	 * in dup0: those carry every packet and have no share to take. */
	cg_split_reset(&s);
	run(&s, bit(0) | bit(2) | bit(3), w, 6000, n);
	CHECK_EQ(n[3], 0);
	CHECK_EQ(n[0] + n[2], 6000);
	cg_split_reset(&s);
	for (int k = 0; k < 600; k++) {
		int p = pick(&s, bit(0) | bit(1) | bit(2), bit(1), w);

		CHECK(p == 0 || p == 2);
	}
	/* Links beyond the weights given have none. */
	cg_split_reset(&s);
	run(&s, 0xffff, w, 8000, n);
	CHECK_EQ(n[3], 0);
	for (int i = 5; i < CG_MAX_LINKS; i++)
		CHECK_EQ(n[i], 0);
	CHECK_EQ(n[0] + n[1] + n[2] + n[4], 8000);

	/* The set changes under the round-robin: it follows. */
	cg_split_reset(&s);
	for (int k = 0; k < 100; k++) {
		int p = pick(&s, bit(0) | bit(1), 0, w);

		CHECK(p == 0 || p == 1);
	}
	for (int k = 0; k < 100; k++) {
		int p = pick(&s, bit(2) | bit(4), 0, w);

		CHECK(p == 2 || p == 4);
	}
}

/* A link that leaves the set starts even when it comes back. */
static void test_split_credits(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 3, 1, 2 };
	struct cg_split s, fresh;

	cg_split_reset(&s);
	for (int k = 0; k < 5; k++)
		pick(&s, bit(0) | bit(1) | bit(2), 0, w);
	CHECK(s.cur[0] != 0 || s.cur[1] != 0 || s.cur[2] != 0);
	CHECK(s.cur[2] != 0);
	CHECK_EQ(s.cur[0] + s.cur[1] + s.cur[2], 0); /* the credits of a set balance */
	for (int i = 3; i < CG_MAX_LINKS; i++)
		CHECK_EQ(s.cur[i], 0); /* a link that was never eligible has none */

	/* Link 2 leaves: its credit goes at the next call, even one that
	 * duplicates the packet (a small one, here). */
	CHECK_EQ(cg_split_pick(&s, bit(0) | bit(1), bit(0) | bit(1), 0, w, 40, SMALL_LEN, NULL), -1);
	CHECK_EQ(s.cur[2], 0);

	/* And it comes back at zero: its first turn is a plain 0 + weight. */
	pick(&s, bit(0) | bit(1) | bit(2), 0, w);
	CHECK(s.cur[2] == 2 || s.cur[2] == 2 - 6);

	/* Fewer than 2 links: nothing is kept, so a second link joins even. */
	cg_split_reset(&s);
	for (int k = 0; k < 5; k++)
		pick(&s, bit(0) | bit(1) | bit(2), 0, w);
	CHECK_EQ(pick(&s, bit(1), 0, w), -1);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		CHECK_EQ(s.cur[i], 0);
	cg_split_reset(&fresh);
	for (int k = 0; k < 24; k++)
		CHECK_EQ(pick(&s, bit(0) | bit(2), 0, w), pick(&fresh, bit(0) | bit(2), 0, w));

	/* Reset. */
	cg_split_reset(&s);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		CHECK_EQ(s.cur[i], 0);

	/* Churn: the set changing at every packet keeps the credits bounded. */
	{
		static const uint16_t wc[CG_MAX_LINKS] = { 1000, 1, 500, 20, 3, 1000, 1, 1, 7, 100, 0, 5, 900, 2, 40, 1000 };
		uint32_t x = 12345, total = 0;
		int32_t most = 0;

		for (int i = 0; i < CG_MAX_LINKS; i++)
			total += wc[i];
		cg_split_reset(&s);
		for (int k = 0; k < 200000; k++) {
			x = x * 1664525u + 1013904223u;
			pick(&s, (uint16_t)(x >> 12), 0, wc);
			for (int i = 0; i < CG_MAX_LINKS; i++) {
				int32_t a = s.cur[i] < 0 ? -s.cur[i] : s.cur[i];

				most = a > most ? a : most;
			}
		}
		CHECK((uint32_t)most < total);
	}
}

/* Duplicated packets: -1, flags untouched, no turn taken. */
static void test_split_duplicate(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 1, 1, 1 };
	struct cg_split s, ref;
	uint8_t flags = CG_F_TUNHOLE;

	cg_split_reset(&s);

	/* Fewer than 2 eligible links: everything is duplicated. */
	CHECK_EQ(pick(&s, 0, 0, w), -1);
	CHECK_EQ(pick(&s, bit(2), 0, w), -1);
	CHECK_EQ(pick(&s, bit(0) | bit(1), bit(0) | bit(1), w), -1); /* both are weight 0 */
	CHECK_EQ(pick(&s, bit(0) | bit(1), bit(1), w), -1);          /* one is */
	{
		/* A weight of 0 is not a share either, named in dup0 or not. */
		static const uint16_t wz[CG_MAX_LINKS] = { 1, 0, 1 };

		CHECK_EQ(pick(&s, bit(0) | bit(1), 0, wz), -1);
		CHECK_EQ(pick(&s, bit(1), 0, wz), -1);
		CHECK(pick(&s, bit(0) | bit(1) | bit(2), 0, wz) >= 0);
	}
	CHECK_EQ(cg_split_pick(&s, bit(2), bit(2), 0, w, BIG, SMALL_LEN, &flags), -1);
	CHECK_EQ(flags, CG_F_TUNHOLE); /* untouched */
	CHECK(pick(&s, bit(0) | bit(1), 0, w) >= 0);

	/* Small packets (empty ACKs, handshakes): up to split_small, inclusive,
	 * the flags left alone. */
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 64, SMALL_LEN, &flags), -1);
	CHECK_EQ(flags, CG_F_TUNHOLE);
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 0, SMALL_LEN, NULL), -1);
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 40, SMALL_LEN, NULL), -1); /* an empty ACK over IPv4 */
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 72, SMALL_LEN, NULL), -1); /* over IPv6, with timestamps */
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, SMALL_LEN, SMALL_LEN, NULL), -1);
	CHECK(cg_split_pick(&s, 0xf, 0xf, 0, w, SMALL_LEN + 1, SMALL_LEN, NULL) >= 0);
	CHECK(cg_split_pick(&s, 0xf, 0xf, 0, w, 1440, SMALL_LEN, NULL) >= 0);
	/* split_small 0 duplicates only the empty packet; 2048, everything. */
	CHECK(cg_split_pick(&s, 0xf, 0xf, 0, w, 1, 0, NULL) >= 0);
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 0, 0, NULL), -1);
	CHECK_EQ(cg_split_pick(&s, 0xf, 0xf, 0, w, 1440, 2048, NULL), -1);

	/* A duplicated packet takes no turn: the picks that follow are the same
	 * as without it. */
	cg_split_reset(&s);
	cg_split_reset(&ref);
	for (int k = 0; k < 50; k++) {
		if (k % 3 == 0) {
			cg_split_pick(&s, 0xf, 0xf, 0, w, 40, SMALL_LEN, NULL);
		}
		CHECK_EQ(pick(&s, 0xf, 0, w), pick(&ref, 0xf, 0, w));
	}
}

/* Where a packet goes, and the weight-0 links in it. */
static void test_split_dst(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 1, 1, 0, 1 };
	/* Links 0, 1 and 4 are eligible; 3 has weight 0; 2 carries but has no
	 * share (it is stale). */
	const uint16_t eligible = bit(0) | bit(1) | bit(4), dup0 = bit(3), carriers = eligible | dup0 | bit(2);
	struct cg_split s;
	unsigned to[CG_MAX_LINKS] = { 0 }, dup = 0, n = 4500;

	CHECK_EQ(cg_split_dst(-1, carriers, dup0), carriers);
	CHECK_EQ(cg_split_dst(1, carriers, dup0), bit(1) | bit(3));
	CHECK_EQ(cg_split_dst(0, carriers, 0), bit(0));
	CHECK_EQ(cg_split_dst(2, bit(2) | bit(3), bit(3) | bit(5)), bit(2) | bit(3)); /* link 5 does not carry */
	/* The one link a packet goes on, for the next packet's CG_F_PREV. */
	CHECK_EQ(cg_split_only(0, carriers, 0), 0);
	CHECK_EQ(cg_split_only(4, carriers, bit(5)), 4); /* link 5 does not carry */
	CHECK_EQ(cg_split_only(1, carriers, dup0), -1);  /* and a copy on link 3 */
	CHECK_EQ(cg_split_only(-1, carriers, dup0), -1); /* every carrier */
	CHECK_EQ(cg_split_only(-1, bit(2), 0), 2);       /* one carrier only */
	CHECK_EQ(cg_split_only(3, bit(1), 0), -1);       /* on none */

	cg_split_reset(&s);
	for (unsigned k = 0; k < n; k++) {
		/* Every third packet is a small one. */
		uint32_t len = k % 3 ? BIG : 40;
		int p = cg_split_pick(&s, (uint16_t)(eligible | dup0), eligible, dup0, w, len, SMALL_LEN, NULL);
		uint16_t dst = cg_split_dst(p, carriers, dup0);

		CHECK(dst & dup0); /* the weight-0 link has a copy of every packet */
		for (int i = 0; i < CG_MAX_LINKS; i++)
			to[i] += dst >> i & 1;
		if (p < 0) {
			dup++;
			CHECK_EQ(dst, carriers); /* the stale link too */
		} else {
			CHECK(!(dst & bit(2)));
			CHECK_EQ(cg_popcount16(dst), 2); /* the picked link and the weight-0 one */
		}
	}
	CHECK_EQ(dup, n / 3);
	CHECK_EQ(to[3], n); /* every packet */
	CHECK_EQ(to[2], dup);
	/* The rest are shared out evenly among 0, 1 and 4, besides the copies of
	 * the duplicated ones. */
	CHECK_EQ(to[0] - dup, (n - dup) / 3);
	CHECK_EQ(to[1] - dup, (n - dup) / 3);
	CHECK_EQ(to[4] - dup, (n - dup) / 3);

	/* One eligible link and a weight-0 one: nothing to share, every
	 * carrier has every packet. */
	cg_split_reset(&s);
	CHECK_EQ(cg_split_pick(&s, bit(0) | bit(3), bit(0), bit(3), w, BIG, SMALL_LEN, NULL), -1);
	/* G2: one eligible link among more carriers (the others late, muted or
	 * not answering) takes every split packet, flagged; the small ones
	 * still go everywhere. None eligible: everything everywhere. */
	{
		uint8_t f = 0;

		CHECK_EQ(cg_split_pick(&s, bit(0) | bit(1) | bit(2), bit(1), 0, w, BIG, SMALL_LEN, &f), 1);
		CHECK_EQ(f, CG_F_SPLIT);
		CHECK_EQ(cg_split_dst(1, bit(0) | bit(1) | bit(2), 0), bit(1));
		CHECK_EQ(cg_split_pick(&s, bit(0) | bit(1) | bit(3), bit(1), bit(3), w, BIG, SMALL_LEN, NULL),
			 1);
		CHECK_EQ(cg_split_dst(1, bit(0) | bit(1) | bit(3), bit(3)), bit(1) | bit(3));
		CHECK_EQ(cg_split_pick(&s, bit(0) | bit(1), bit(1), 0, w, 40, SMALL_LEN, NULL), -1);
		CHECK_EQ(cg_split_pick(&s, bit(0) | bit(1), 0, 0, w, BIG, SMALL_LEN, NULL), -1);
		/* The one link that carries anything: nothing to skip. */
		f = 0;
		CHECK_EQ(cg_split_pick(&s, bit(2), bit(2), 0, w, BIG, SMALL_LEN, &f), -1);
		CHECK_EQ(f, 0);
	}
}

/* CG_F_SPLIT: set when the packet skips a carrier, for every shape of the
 * links and of the packet; the other bits are the caller's. */
static void test_split_flag(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 2, 3, 4, 5 };
	static const struct {
		uint8_t flags;
		uint32_t len;
	} kind[] = {
		{ 0, BIG },
		{ CG_F_TUNHOLE, BIG }, /* the sender's bits stay */
		{ CG_F_TUNHOLE, 92 },  /* a small one, marked */
		{ 0, 40 },             /* a small one */
	};
	struct cg_split s;
	unsigned flagged = 0, plain = 0;

	CHECK_EQ(CG_F_SPLIT, 0x04);
	CHECK_EQ(CG_F_SPLIT & CG_F_TUNHOLE, 0);

	cg_split_reset(&s);
	/* Links 0 to 4, every way: which are eligible, which have weight 0 and
	 * which carry with no share; each kind of packet. */
	for (unsigned elig = 0; elig < 32; elig++)
		for (unsigned zero = 0; zero < 32; zero++)
			for (unsigned idle = 0; idle < 32; idle++) {
				uint16_t carriers = (uint16_t)(elig | zero | idle);

				if ((elig & zero) || (elig & idle) || (zero & idle))
					continue;
				for (unsigned t = 0; t < CG_ARRAY_SIZE(kind); t++) {
					uint8_t flags = kind[t].flags;
					int p = cg_split_pick(&s, carriers, (uint16_t)elig, (uint16_t)zero, w, kind[t].len,
							      SMALL_LEN, &flags);
					uint16_t dst = cg_split_dst(p, carriers, (uint16_t)zero);
					int skipped = (carriers & ~dst) != 0;

					/* The flag is the skipping, nothing else... */
					CHECK_EQ(!!(flags & CG_F_SPLIT), skipped);
					CHECK_EQ(!!(flags & CG_F_SPLIT), p >= 0);
					/* ...and the caller's bits stay. */
					CHECK_EQ(flags & ~CG_F_SPLIT, kind[t].flags);
					/* A packet never goes nowhere, and a split one goes to
					 * the link picked. */
					CHECK(dst != 0 || carriers == 0);
					CHECK(p < 0 || (dst >> p & 1));
					flagged += (unsigned)skipped;
					plain += (unsigned)!skipped;
				}
			}
	CHECK(flagged > 0 && plain > 0);

	/* Without a flags byte to write, the picks are the same. */
	{
		struct cg_split a, b;
		uint8_t f = 0;

		cg_split_reset(&a);
		cg_split_reset(&b);
		for (int k = 0; k < 40; k++)
			CHECK_EQ(cg_split_pick(&a, 0x1f, 0x1f, 0, w, BIG, SMALL_LEN, NULL),
				 cg_split_pick(&b, 0x1f, 0x1f, 0, w, BIG, SMALL_LEN, &f));
	}
}

/* Guard G3: the peer's silence on a link. */
static void test_split_stale(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 1, 1 };
	const uint32_t iv = 100; /* probe_interval_ms */
	const uint64_t now = 10300;
	uint64_t last[3] = { 10300, 9999, 10250 }; /* link 1: 301 ms of silence */
	uint16_t eligible = 0;
	struct cg_split s;

	CHECK(cg_split_stale(0, 5000, iv)); /* never heard from */
	CHECK(cg_split_stale(0, 0, iv));
	CHECK(!cg_split_stale(5000, 5000, iv));
	CHECK(!cg_split_stale(5000, 5000 + 3 * iv, iv)); /* exactly three intervals: still fresh */
	CHECK(cg_split_stale(5000, 5000 + 3 * iv + 1, iv));
	CHECK(!cg_split_stale(5000, 5001, iv));
	CHECK(!cg_split_stale(5000, 4999, iv)); /* a clock that went back is not silence */
	CHECK(!cg_split_stale(1, 1 + 3 * 1000, 1000));
	CHECK(cg_split_stale(1, 2 + 3 * 1000, 1000));
	/* No overflow at the ends of the ranges. */
	CHECK(!cg_split_stale(UINT64_MAX - 5, UINT64_MAX, UINT32_MAX));
	CHECK(!cg_split_stale(1, (uint64_t)3 * UINT32_MAX + 1, UINT32_MAX));
	CHECK(cg_split_stale(1, (uint64_t)3 * UINT32_MAX + 2, UINT32_MAX));

	/* How the caller uses it: a link that stopped answering leaves the set
	 * 300 ms later, and the shares go to the others. */
	for (unsigned i = 0; i < 3; i++)
		if (!cg_split_stale(last[i], now, iv))
			eligible |= bit(i);
	CHECK_EQ(eligible, bit(0) | bit(2));
	cg_split_reset(&s);
	for (int k = 0; k < 100; k++)
		CHECK(pick(&s, eligible, 0, w) != 1);
	last[1] = now - 300; /* a reply at the limit brings it back */
	CHECK(!cg_split_stale(last[1], now, iv));
}

/* Which end splits (cg_split_active), from its setting and the peer's
 * tail, and the tails each end writes. */
static void test_split_modes(void)
{
	static const int modes[] = { CG_SPLIT_OFF, CG_SPLIT_ON, CG_SPLIT_CLIENT, CG_SPLIT_FORCE };
	struct cg_probe_bond b, r;
	uint8_t pl[CG_PROBE_BOND_LEN] = { 0 };

	for (unsigned m = 0; m < CG_ARRAY_SIZE(modes); m++)
		for (int ro = 0; ro < 2; ro++)
			for (int ask = 0; ask < 2; ask++) {
				int want = modes[m] == CG_SPLIT_FORCE || (modes[m] == CG_SPLIT_ON && ro) ||
					   (modes[m] == CG_SPLIT_CLIENT && ro && ask);

				CHECK_EQ(cg_split_active(modes[m], ro, ask), want);
			}
	CHECK_EQ(cg_split_active(CG_SPLIT_ON, 2, 0), 1); /* any true value */
	CHECK_EQ(cg_split_active(99, 1, 1), 0);           /* an unknown mode never splits */

	/* The client's probe tail: its weight, its reorder, and with on (only)
	 * the request. */
	b = cg_split_probe_tail(CG_SPLIT_ON, 1, 3);
	CHECK(b.weight == 3 && b.reorders && b.split_me);
	b = cg_split_probe_tail(CG_SPLIT_FORCE, 1, 0);
	CHECK(b.weight == 0 && b.reorders && !b.split_me); /* force asks nothing of the peer */
	b = cg_split_probe_tail(CG_SPLIT_OFF, 0, 2000);
	CHECK(b.weight == CG_BOND_WEIGHT_MAX && !b.reorders && !b.split_me);
	/* The server's reply tail: its reorder only. */
	b = cg_split_reply_tail(1);
	CHECK(b.weight == 0 && b.reorders && !b.split_me);
	b = cg_split_reply_tail(0);
	CHECK(!b.weight && !b.reorders && !b.split_me);

	/* End to end through the wire: a server with split = client splits
	 * the download only for a client that asks and reorders. */
	b = cg_split_probe_tail(CG_SPLIT_ON, 1, 1);
	cg_probe_bond_write(pl, &b);
	CHECK_EQ(cg_probe_bond_read(&r, pl, sizeof(pl)), 1);
	CHECK(cg_split_active(CG_SPLIT_CLIENT, r.reorders, r.split_me));
	CHECK(!cg_split_active(CG_SPLIT_OFF, r.reorders, r.split_me)); /* a server with split off */
	b = cg_split_probe_tail(CG_SPLIT_ON, 0, 1); /* asks, but does not reorder */
	cg_probe_bond_write(pl, &b);
	cg_probe_bond_read(&r, pl, sizeof(pl));
	CHECK(!cg_split_active(CG_SPLIT_CLIENT, r.reorders, r.split_me));
	b = cg_split_probe_tail(CG_SPLIT_OFF, 1, 1); /* reorders, does not ask */
	cg_probe_bond_write(pl, &b);
	cg_probe_bond_read(&r, pl, sizeof(pl));
	CHECK(!cg_split_active(CG_SPLIT_CLIENT, r.reorders, r.split_me));
	CHECK(cg_split_active(CG_SPLIT_FORCE, r.reorders, r.split_me));
	/* An older client (a 40-byte probe): nothing. */
	CHECK_EQ(cg_probe_bond_read(&r, pl, CG_PROBE_INFO_LEN), 0);
	CHECK(!cg_split_active(CG_SPLIT_CLIENT, r.reorders, r.split_me));
	/* The client's upload with on: once a reply says the server reorders,
	 * never from an older server or one with reorder off. */
	b = cg_split_reply_tail(1);
	cg_probe_bond_write(pl, &b);
	CHECK_EQ(cg_probe_bond_read(&r, pl, sizeof(pl)), 1);
	CHECK(cg_split_active(CG_SPLIT_ON, r.reorders, r.split_me));
	b = cg_split_reply_tail(0);
	cg_probe_bond_write(pl, &b);
	cg_probe_bond_read(&r, pl, sizeof(pl));
	CHECK(!cg_split_active(CG_SPLIT_ON, r.reorders, r.split_me));
	cg_probe_bond_read(&r, pl, CG_PROBE_INFO_LEN);
	CHECK(!cg_split_active(CG_SPLIT_ON, r.reorders, r.split_me));
}

/* Which tail an end follows (cg_split_tail_newer): the newest sent, by the
 * control sequence, whatever the order of arrival. */
static void test_split_tail(void)
{
	/* A peer's message: its control sequence, and whether its tail says it
	 * reorders. The end keeps what the message it took says. */
	struct msg {
		uint32_t seq;
		int reorders;
	};
	/* A reload at the peer between 101 and 102 turns reordering off; the
	 * messages sent before it on a slow link (99..101) arrive after the
	 * newer ones on a fast link. */
	static const struct msg after_reload[] = { { 100, 1 }, { 102, 0 }, { 99, 1 }, { 103, 0 }, { 101, 1 }, { 104, 0 } };
	struct cg_split_tail t = { 0 };
	int reorders = 0, flips = 0;

	for (unsigned i = 0; i < CG_ARRAY_SIZE(after_reload); i++) {
		if (cg_split_tail_newer(&t, after_reload[i].seq)) {
			flips += reorders != after_reload[i].reorders && i;
			reorders = after_reload[i].reorders;
		}
	}
	CHECK_EQ(reorders, 0);
	CHECK_EQ(flips, 1); /* one clean turn, no flapping back */
	CHECK_EQ(t.seq, 104);

	/* The first message is taken, whatever its sequence; a repeat is not newer. */
	memset(&t, 0, sizeof(t));
	CHECK(cg_split_tail_newer(&t, 0xfffffff0u));
	CHECK(!cg_split_tail_newer(&t, 0xfffffff0u));
	CHECK(!cg_split_tail_newer(&t, 0xffffffe0u));
	/* Serial arithmetic across the wrap, as the replay window. */
	CHECK(cg_split_tail_newer(&t, 0x10u));
	CHECK(!cg_split_tail_newer(&t, 0xfffffff8u));
	CHECK_EQ(t.seq, 0x10u);
	/* A server back from a restart starts 2^20 (CG_SEQ_LEAP) past the newest
	 * the client took: newer. A message from before it, still on its way, is
	 * not. */
	CHECK(cg_split_tail_newer(&t, 0x10u + CG_SEQ_LEAP));
	CHECK(!cg_split_tail_newer(&t, 0x11u));
	/* Anything within 2^31 ahead is newer (the ctl window takes 8128 behind). */
	CHECK(cg_split_tail_newer(&t, 0x10u + CG_SEQ_LEAP + 0x7fffffffu));
}

/* A link's send-direction delay as health.h holds it, fresh for an hour
 * (reports that stop are tested apart); owd in us, from any origin (only
 * differences mean anything). */
static void delay(struct cg_hlink *h, unsigned link, uint32_t owd, uint64_t now_ms)
{
	h[link].owd = owd;
	h[link].have_owd = 1;
	h[link].fresh_until_ms = now_ms + 3600000;
}

#define MSUS 1000u
#define SPREAD (30 * MSUS) /* split_spread_ms 30 */
#define SETTLE 2000        /* mute_settle_ms 2000 */

/* Guard G4 (cg_split_spread_eval): only links within split_spread_ms of the
 * fastest share the split; the others carry only the duplicated packets. */
/* G4 never empties the split: a link that left and became the fastest
 * waits settle_ms to come back, and the links in are measured behind the
 * fastest of them, not behind it (they all left with it, and every packet
 * went on every link as the network filled); with every fresh link out,
 * the fastest comes back at once. */
static void test_split_spread_lead(void)
{
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;

	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	delay(h, 2, 5000000 + 50 * MSUS, t); /* 50 ms behind: leaves */
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(sp.left, 0x4);
	CHECK_EQ(sp.in, 0x3);
	CHECK(!sp.recover);
	/* 2 is now 40 ms ahead of 0 (its queue drained, theirs filled): stress */
	delay(h, 2, 5000000 - 40 * MSUS, t);
	for (int k = 0; k < 20; k++) { /* 2 s: the wait starts at the first */
		cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100);
		CHECK(sp.recover);
		CHECK_EQ(sp.in, 0x3); /* 0 and 1 stay; 2 holds */
		CHECK(sp.in != 0);
	}
	CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100), 0x4);
	CHECK_EQ(sp.in, 0x7); /* 2 back after settle */
	/* now 0 and 1 are 40 and 50 ms behind the fastest link in: they leave */
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100);
	CHECK_EQ(sp.in, 0x4);
	CHECK_EQ(sp.left, 0x3);
	CHECK(!sp.recover); /* the fastest is in again */
	/* 2 loses its delay (not fresh) and 0, 1 are out: the fastest of them is
	 * taken in at once, the split is never empty */
	h[2].fresh_until_ms = t;
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100);
	CHECK_EQ(sp.in, 0x1);
	CHECK_EQ(sp.left, 0x2);
}

/* cg_sdup: copies only under stress, only on its budget (pct % of the
 * split bytes), never with pct 0 or one link. */
static void test_split_sdup(void)
{
	struct cg_sdup b;
	int n = 0;

	memset(&b, 0, sizeof(b));
	for (int k = 0; k < 100; k++) /* no stress: no copies, the budget grows */
		CHECK(!cg_sdup(&b, 0, 1000, 4, 50));
	CHECK_EQ(b.tokens, 50000);
	for (int k = 0; k < 1000; k++) /* stress, 4 links: 3000 bytes a copy, 500 earned a packet */
		n += cg_sdup(&b, 1, 1000, 4, 50);
	/* 50000 saved + 1000 x 500 earned over 3000 a copy: about 183 */
	CHECK(n >= 180 && n <= 186);
	CHECK_EQ(b.pkts, (uint64_t)n);
	CHECK_EQ(b.bytes, (uint64_t)n * 3000);
	CHECK(b.bytes <= 50000 + 1000 * 500);
	CHECK(!cg_sdup(&b, 1, 1000, 4, 0)); /* off */
	memset(&b, 0, sizeof(b));
	for (int k = 0; k < 100; k++)
		CHECK(!cg_sdup(&b, 1, 1000, 1, 400)); /* one link: nothing to copy to */
	for (int k = 0; k < 100000; k++)
		cg_sdup(&b, 0, 1500, 4, 400);
	CHECK_EQ(b.tokens, CG_SDUP_CAP); /* capped */
}

/* The two kinds of G4 stress, told apart: a link that was in and left
 * (its queue grew) arms the copies (stress, qleft) until it comes back; a
 * link slow by its base delay, never in, does not. A recovery (the fastest
 * link is one that left) with rejoin brings it back at once. */
static void test_split_spread_kinds(void)
{
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;

	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	delay(h, 2, 5000000 + 80 * MSUS, t); /* slow by base delay: never in */
	cg_split_spread_eval_r(&sp, h, 0x7, SPREAD, SETTLE, t, 1);
	CHECK_EQ(sp.in, 0x3);
	CHECK_EQ(sp.left, 0x4);
	CHECK(!sp.stress);
	CHECK_EQ(sp.qleft, 0);
	/* 1's queue grows: it leaves, having been in -> copies */
	delay(h, 1, 5000000 + 60 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x7, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x1);
	CHECK_EQ(sp.qleft, 0x2);
	CHECK(sp.stress);
	CHECK_EQ(sp.n_qleave, 1);
	/* still out a while later: still stress */
	cg_split_spread_eval_r(&sp, h, 0x7, SPREAD, SETTLE, t += 100, 1);
	CHECK(sp.stress);
	/* its queue drains and it becomes the fastest while 0 queues: a
	 * recovery, and with rejoin it is back at once, stress over */
	delay(h, 1, 5000000 - 10 * MSUS, t);
	delay(h, 0, 5000000 + 40 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x7, SPREAD, SETTLE, t += 100, 1);
	CHECK(sp.recover);
	CHECK_EQ(sp.n_recover, 1);
	CHECK(sp.in & 0x2);      /* back without settle_ms */
	CHECK(!(sp.qleft & 0x2));
	/* 0 was in and is now 50 ms behind 1: it left for its queue */
	CHECK(sp.left & 0x1);
	CHECK(sp.stress);
	/* without rejoin the fastest waits settle_ms (test_split_spread_lead) */
}

/* Audit C7: a link of qleft out of the candidates for a short G3 gap keeps
 * the stress; out for CG_SPLIT_QLEFT_KEEP_MS it is gone. And C6: a delay
 * report is fresh for CG_FRESH_PROBES of the probe interval. */
static void test_split_qleft_gap(void)
{
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;

	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t, 1);
	delay(h, 1, 5000000 + 60 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.qleft, 0x2);
	/* 300 ms without replies on 1 (G3 drops it from the candidates) */
	for (int k = 0; k < 3; k++)
		cg_split_spread_eval_r(&sp, h, 0x1, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.qleft, 0x2);
	CHECK(sp.stress);
	/* back as a candidate, still behind: still stress */
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK(sp.stress);
	/* gone for good */
	cg_split_spread_eval_r(&sp, h, 0x1, SPREAD, SETTLE, t += 100, 1);
	cg_split_spread_eval_r(&sp, h, 0x1, SPREAD, SETTLE, t += CG_SPLIT_QLEFT_KEEP_MS, 1);
	CHECK_EQ(sp.qleft, 0);
	CHECK(!sp.stress);

	CHECK_EQ(cg_health_valid_ms(100, 3000), 500);
	CHECK_EQ(cg_health_valid_ms(1000, 3000), 3000);
	CHECK_EQ(cg_health_valid_ms(0, 3000), 3000);
}

/* Audit C3: with dwell_ms, a link back by a recovery stays in that long
 * while within twice the spread; beyond it, or later, it leaves; with
 * dwell_oq_us, only while its own queue is at most that. */
static void test_split_dwell(void)
{
	for (int guard = 0; guard < 3; guard++) {
		struct cg_hlink h[CG_MAX_LINKS];
		struct cg_spread sp;
		uint64_t t = 1000000;
		/* guard 2: link 1's own queue grows 30 ms after it is back */
		int32_t own = guard == 2 ? 30 * MSUS : 0;

		memset(h, 0, sizeof(h));
		memset(&sp, 0, sizeof(sp));
		sp.dwell_ms[1] = 500; /* link 1's (its model's dwell_ms) */
		sp.dwell_oq_us[1] = guard ? 20 * MSUS : 0;
		delay(h, 0, 5000000, t);
		delay(h, 1, 5000000 + 10 * MSUS, t);
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t, 1);
		delay(h, 1, 5000000 + 60 * MSUS, t); /* 1 leaves for its queue */
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
		CHECK(sp.left & 0x2);
		delay(h, 1, 5000000 - 10 * MSUS, t); /* 1 drains, 0 queues: 1 back */
		delay(h, 0, 5000000 + 40 * MSUS, t);
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
		CHECK(sp.in & 0x2);
		/* 0 drains, 1 is now 40 ms behind (over the spread, under twice
		 * it): it stays in while it dwells, unless its own queue grew */
		delay(h, 0, 5000000 - 50 * MSUS + own, t);
		delay(h, 1, 5000000 - 10 * MSUS + own, t);
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
		if (guard == 2) {
			CHECK(sp.behind_us[1] == 40 * MSUS); /* within twice the spread */
			CHECK(sp.left & 0x2);
			continue;
		}
		CHECK(sp.in & 0x2);
		CHECK(!(sp.left & 0x2));
		CHECK(sp.n_dwell == 1);
		/* past the dwell: it leaves */
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 500, 1);
		CHECK(sp.left & 0x2);
	}
}

/* A link that leaves in a stall (its delay grows as fast as time, or
 * jumps) arms no copies; one whose queue grows slower than time does. */
static void test_split_spread_stall(void)
{
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;

	CHECK(cg_split_stalled(5000000, 5000000 + 300 * MSUS, 100)); /* jump */
	CHECK(cg_split_stalled(5000000, 5000000 + 80 * MSUS, 100));  /* as fast as time */
	CHECK(!cg_split_stalled(5000000, 5000000 + 40 * MSUS, 100)); /* a queue */
	CHECK(!cg_split_stalled(5000000, 5000000 - 40 * MSUS, 100));
	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t, 1);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x3);
	/* 1 jumps by 300 ms in 100 ms: leaves, stalled, no copies */
	delay(h, 1, 5000000 + 310 * MSUS, t);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.left, 0x2);
	CHECK(!sp.stress);
	CHECK_EQ(sp.n_stall, 1);
	CHECK_EQ(sp.qleft, 0);
	/* back; then its queue grows 10 ms per 100 ms: leaves past the spread, copies */
	delay(h, 1, 5000000 + 10 * MSUS, t);
	for (int k = 0; k < 25; k++)
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x3);
	for (int k = 1; k <= 4 && !sp.stress; k++) {
		delay(h, 1, 5000000 + (10 + 10 * (uint32_t)k) * MSUS, t);
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	}
	CHECK(sp.stress);
	CHECK_EQ(sp.qleft, 0x2);
	/* under stress the split takes the lead only (0 here), or as given */
	CHECK_EQ(sp.lead, 0);
	CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 1), 0x1);
	CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 0), 0x3);
	CHECK_EQ(cg_split_stress_elig(&sp, 0x2, 1), 0x2); /* lead not eligible */
	sp.stress = sp.recover = 0;
	CHECK_EQ(cg_split_stress_elig(&sp, 0x3, 1), 0x3); /* no stress */
}

/* Audit C1 (2026-10-17): a link slow by its base delay (40 ms more) gets in
 * while the fast link queues, and when the fast one drains it is behind
 * again: that is not its queue growing, and it must not leave as a queue
 * (qleft, copies) nor keep G4 in stress. */
static void test_split_spread_base(void)
{
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	uint64_t t = 1000000;

	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);              /* base 0 */
	delay(h, 1, 5000000 + 40 * MSUS, t);  /* base +40 ms: out */
	for (int k = 0; k < 50; k++)          /* 5 s of minima */
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x1);
	/* 0 queues 25 ms: 1 within 15 ms of it, in after settle */
	delay(h, 0, 5000000 + 25 * MSUS, t);
	for (int k = 0; k < 25; k++)
		cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x3);
	/* 0 drains: 1 is 40 ms behind again, its own delay unchanged */
	delay(h, 0, 5000000, t);
	cg_split_spread_eval_r(&sp, h, 0x3, SPREAD, SETTLE, t += 100, 1);
	CHECK_EQ(sp.in, 0x1);
	CHECK_EQ(sp.qleft, 0);   /* not a queue that grew */
	CHECK(!sp.stress);
}

static void test_split_spread(void)
{
	static const uint16_t w1[CG_MAX_LINKS] = { 1, 1, 1, 1 };
	struct cg_hlink h[CG_MAX_LINKS];
	struct cg_spread sp;
	struct cg_split s;
	uint64_t t = 1000000;
	uint16_t ch, out;
	unsigned count[CG_MAX_LINKS];
	int changes;

	memset(h, 0, sizeof(h));
	memset(&sp, 0, sizeof(sp));
	/* Off (0): every candidate is in, with or without a delay. */
	CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, 0, SETTLE, t), 0);
	CHECK_EQ(sp.in, 0x7);
	CHECK_EQ(cg_split_spread_out(&sp, 0x7, 0, 0), 0);
	CHECK_EQ(cg_split_share(&sp, 2, 0x7, 0, 0), CG_SHARE_YES);

	/* No delay yet: out, waiting, and in at once when it comes within the
	 * spread (only a link that left waits to come back). */
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(sp.in, 0);
	CHECK_EQ(cg_split_share(&sp, 0, 0x7, 0, SPREAD), CG_SHARE_WAITING);
	CHECK_EQ(cg_split_spread_out(&sp, 0x7, 0, SPREAD), 0x7);
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 10 * MSUS, t); /* 10 ms behind: stays */
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(sp.in, 0x3);
	CHECK_EQ(cg_split_share(&sp, 2, 0x7, 0, SPREAD), CG_SHARE_WAITING);
	CHECK(sp.behind == 0x3 && sp.behind_us[0] == 0 && sp.behind_us[1] == 10 * (int32_t)MSUS);
	/* 50 ms behind: leaves at once; 30 exactly is within. */
	delay(h, 2, 5000000 + 50 * MSUS, t);
	ch = cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(ch, 0x4);
	CHECK_EQ(sp.left, 0x4);
	CHECK_EQ(sp.in, 0x3);
	CHECK_EQ(cg_split_share(&sp, 2, 0x7, 0, SPREAD), CG_SHARE_SPREAD);
	CHECK_EQ(cg_split_share(&sp, 1, 0x7, 0, SPREAD), CG_SHARE_YES);
	out = cg_split_spread_out(&sp, 0x7, 0, SPREAD);
	CHECK_EQ(out, 0x4);
	/* The share goes to 0 and 1 only; without split_spread_copy 2 carries
	 * none of the split packets, only the duplicated ones (with it, a copy
	 * of each: test_split_spread_copy). */
	cg_split_reset(&s);
	memset(count, 0, sizeof(count));
	for (int k = 0; k < 1000; k++) {
		int p = cg_split_pick(&s, 0x7, (uint16_t)(0x7 & ~out), 0, w1, BIG, SMALL_LEN, NULL);

		CHECK(p == 0 || p == 1);
		if (p >= 0 && p < CG_MAX_LINKS)
			count[p]++;
		CHECK_EQ(cg_split_dst(p, 0x7, 0), bit((unsigned)p));
	}
	CHECK(count[0] == 500 && count[1] == 500);
	CHECK_EQ(cg_split_pick(&s, 0x7, (uint16_t)(0x7 & ~out), 0, w1, 40, SMALL_LEN, NULL), -1);
	delay(h, 1, 5000000 + SPREAD, t);
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(sp.in, 0x3);

	/* Comes back only after settle within 3/4 of the spread (22.5 ms). */
	delay(h, 2, 5000000 + 25 * MSUS, t);
	for (int k = 0; k < 50; k++) /* 5 s at 25 ms: not close enough */
		CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100), 0);
	CHECK_EQ(sp.left, 0x4);
	delay(h, 2, 5000000 + 22 * MSUS, t);
	for (int k = 0; k < 20; k++) /* 1.9 s within: not yet */
		CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += k ? 100 : 0), 0);
	CHECK_EQ(sp.in, 0x3);
	CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100), 0x4); /* 2 s */
	CHECK_EQ(sp.in, 0x7);
	CHECK_EQ(sp.left, 0);
	/* Back in, it stays up to the spread itself. */
	delay(h, 2, 5000000 + 29 * MSUS, t);
	CHECK_EQ(cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t += 100), 0);
	CHECK_EQ(sp.in, 0x7);

	/* No flapping: a link whose delay swings across the edge (20 to 40 ms
	 * behind, 300 ms each way) leaves once and stays out; one that swings
	 * within 25 to 35 the same. The wait to come back starts over each time
	 * it goes above 3/4 of the spread. */
	for (int lo = 20; lo <= 25; lo += 5) {
		memset(&sp, 0, sizeof(sp));
		changes = 0;
		for (int k = 0; k < 600; k++) { /* 60 s */
			int up = (k / 3) % 2;

			t += 100;
			delay(h, 0, 5000000, t);
			delay(h, 1, 5000000 + (uint32_t)(up ? (lo == 20 ? 40 : 35) : lo) * MSUS, t);
			changes += cg_popcount16(cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t));
		}
		CHECK_EQ(changes, 1);
		CHECK_EQ(sp.left, 0x2);
		/* Only two links, one out: the other takes every split packet (G2,
		 * without split_spread_copy). */
		CHECK_EQ(sp.in, 0x1);
		out = cg_split_spread_out(&sp, 0x3, 0, SPREAD);
		CHECK_EQ(cg_split_pick(&s, 0x3, (uint16_t)(0x3 & ~out), 0, w1, BIG, SMALL_LEN, NULL), 0);
	}
	/* Once it settles within 3/4 it comes back, and only once. */
	changes = 0;
	for (int k = 0; k < 100; k++) {
		t += 100;
		delay(h, 0, 5000000, t);
		delay(h, 1, 5000000 + 15 * MSUS, t);
		changes += cg_popcount16(cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t));
	}
	CHECK_EQ(changes, 1);
	CHECK_EQ(sp.in, 0x3);

	/* All but one out: the fastest takes every split packet (G2, without
	 * split_spread_copy). */
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 60 * MSUS, t);
	delay(h, 2, 5000000 + 150 * MSUS, t);
	cg_split_spread_eval(&sp, h, 0x7, SPREAD, SETTLE, t);
	CHECK_EQ(sp.in, 0x1);
	out = cg_split_spread_out(&sp, 0x7, 0, SPREAD);
	CHECK_EQ(out, 0x6);
	CHECK_EQ(cg_split_pick(&s, 0x7, (uint16_t)(0x7 & ~out), 0, w1, BIG, SMALL_LEN, NULL), 0);
	CHECK_EQ(cg_split_dst(0, 0x7, 0), 0x1);

	/* The fastest is the fastest candidate: when it is no longer one (stale,
	 * muted, weight 0), the others are measured from the next. 2 left at 150;
	 * from 1 it is 90 behind and stays out; 1 is now the fastest and comes
	 * back after settle. */
	ch = 0;
	for (int k = 0; k <= SETTLE / 100; k++)
		ch |= cg_split_spread_eval(&sp, h, 0x6, SPREAD, SETTLE, t += k ? 100 : 0);
	CHECK_EQ(ch, 0x2);
	CHECK_EQ(sp.in, 0x2);
	CHECK_EQ(sp.behind_us[2], 90 * (int32_t)MSUS);
	/* A delay that stops coming (stale) is no delay: out, and a wait to come
	 * back starts over. */
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 5000000, t);
	delay(h, 1, 5000000 + 5 * MSUS, t);
	cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t);
	CHECK_EQ(sp.in, 0x3);
	h[1].fresh_until_ms = t; /* its last report runs out */
	cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t + 1);
	CHECK_EQ(sp.in, 0x1);
	CHECK_EQ(cg_split_share(&sp, 1, 0x3, 0, SPREAD), CG_SHARE_WAITING);

	/* Wrap-safe: raw delays around 2^32. */
	memset(&sp, 0, sizeof(sp));
	delay(h, 0, 0xfffff000u, t);
	delay(h, 1, 0xfffff000u + 40 * MSUS, t); /* wraps */
	cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t);
	CHECK_EQ(sp.left, 0x2);
	CHECK_EQ(sp.behind_us[1], 40 * (int32_t)MSUS);

	/* A link that restarts has not left; one turned off (spread 0) neither. */
	cg_split_spread_reset(&sp, 1);
	CHECK_EQ(sp.left, 0);
	cg_split_spread_eval(&sp, h, 0x3, SPREAD, SETTLE, t);
	CHECK_EQ(sp.left, 0x2);
	CHECK_EQ(cg_split_spread_eval(&sp, h, 0x3, 0, SETTLE, t), 0x2);
	CHECK(sp.left == 0 && sp.in == 0x3);

	/* The other reasons: not eligible, weight 0 (never measured, never
	 * taken out by the guard: it carries copies anyway). */
	CHECK_EQ(cg_split_share(&sp, 3, 0x7, 0, SPREAD), CG_SHARE_NO);
	CHECK_EQ(cg_split_share(&sp, 2, 0x7, 0x4, SPREAD), CG_SHARE_WEIGHT0);
	CHECK_EQ(cg_split_spread_out(&sp, 0x7, 0x4, SPREAD) & 0x4, 0);
	CHECK(!strcmp(cg_split_share_name(CG_SHARE_SPREAD), "spread"));
	CHECK(!strcmp(cg_split_share_name(CG_SHARE_YES), "yes") && !strcmp(cg_split_share_name(CG_SHARE_NO), "no"));
	CHECK(!strcmp(cg_split_share_name(CG_SHARE_WAITING), "waiting"));
	CHECK(!strcmp(cg_split_share_name(CG_SHARE_WEIGHT0), "weight 0") && !strcmp(cg_split_share_name(99), "?"));
}

/* The pick of 0.6.2 (git 412b785^), the reference for split_spread_copy:
 * the caller put the links G4 kept out in dup0, and fewer than 2 links in
 * the share duplicated everything. */
static int pick_062(struct cg_split *s, uint16_t eligible, uint16_t dup0, const uint16_t w[CG_MAX_LINKS],
		    uint32_t len, uint8_t *flags)
{
	uint16_t set = 0, keep;
	int32_t total = 0;
	int best = -1;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((eligible >> i & 1) && !(dup0 >> i & 1) && w[i])
			set |= (uint16_t)(1u << i);
	keep = set & (set - 1) ? set : 0;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (!(keep >> i & 1))
			s->cur[i] = 0;
	if (len <= SMALL_LEN || !keep)
		return -1;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!(keep >> i & 1))
			continue;
		s->cur[i] += w[i];
		total += w[i];
		if (best < 0 || s->cur[i] > s->cur[best])
			best = i;
	}
	s->cur[best] -= total;
	if (flags)
		*flags |= CG_F_SPLIT;
	return best;
}

/* split_spread_copy (cg_split_spread_apply): with it, every pick and every
 * packet's links are 0.6.2's, for every shape of the links; without it, the
 * links G4 keeps out get none of the split packets and one link left takes
 * them (G2). */
static void test_split_spread_copy(void)
{
	static const uint16_t w[CG_MAX_LINKS] = { 1, 2, 0, 3, 1 };
	static const struct {
		uint32_t len;
	} kind[] = { { BIG }, { BIG }, { 40 }, { 148 }, { BIG } };
	struct cg_split a, b, c;
	struct cg_split_masks m;
	unsigned shapes = 0, split_old = 0, split_new = 0, lone = 0;

	m = cg_split_spread_apply(1, 0x4, 0xf, 0x7, 0x8);
	CHECK(m.carriers == 0 && m.eligible == 0x7 && m.dup == 0xc);
	m = cg_split_spread_apply(0, 0x4, 0xf, 0x7, 0x8);
	CHECK(m.carriers == 0xf && m.eligible == 0x3 && m.dup == 0x8);
	m = cg_split_spread_apply(1, 0, 0xf, 0x7, 0x8);
	CHECK(m.carriers == 0 && m.eligible == 0x7 && m.dup == 0x8);
	m = cg_split_spread_apply(0, 0, 0xf, 0x7, 0x8);
	CHECK(m.carriers == 0xf && m.eligible == 0x7 && m.dup == 0x8);

	/* Links 0 to 4, each one of 7 ways: no carrier; a carrier without a
	 * share; weight 0 (dup0), carrying or not; eligible with dup0; eligible
	 * and kept out by G4; eligible and in. a follows the copy, b 0.6.2 and c
	 * the copy's absence, each with its own credits. */
	cg_split_reset(&a);
	cg_split_reset(&b);
	cg_split_reset(&c);
	for (unsigned code = 0; code < 7 * 7 * 7 * 7 * 7; code++) {
		uint16_t carry = 0, elig = 0, dup0 = 0, out = 0;

		for (unsigned i = 0, x = code; i < 5; i++, x /= 7) {
			uint16_t one = bit(i);

			switch (x % 7) {
			case 1: carry |= one; break;
			case 2: dup0 |= one; break;
			case 3: carry |= one, dup0 |= one; break;
			case 4: carry |= one, elig |= one, dup0 |= one; break;
			case 5: carry |= one, elig |= one, out |= one; break;
			case 6: carry |= one, elig |= one; break;
			default: break;
			}
		}
		shapes++;
		for (unsigned t = 0; t < CG_ARRAY_SIZE(kind); t++) {
			struct cg_split_masks y = cg_split_spread_apply(1, out, carry, elig, dup0);
			struct cg_split_masks n = cg_split_spread_apply(0, out, carry, elig, dup0);
			uint8_t fa = 0, fb = 0, fc = 0;
			int pa = cg_split_pick(&a, y.carriers, y.eligible, y.dup, w, kind[t].len, SMALL_LEN, &fa);
			int pb = pick_062(&b, elig, (uint16_t)(dup0 | out), w, kind[t].len, &fb);
			int pc = cg_split_pick(&c, n.carriers, n.eligible, n.dup, w, kind[t].len, SMALL_LEN, &fc);
			uint16_t share = (uint16_t)(elig & ~dup0 & ~out), dc = cg_split_dst(pc, carry, n.dup);

			/* The copy is 0.6.2, pick, flag and links. */
			CHECK_EQ(pa, pb);
			CHECK_EQ(fa, fb);
			CHECK_EQ(cg_split_dst(pa, carry, y.dup), cg_split_dst(pb, carry, (uint16_t)(dup0 | out)));
			split_old += pa >= 0;
			/* Without it: a split packet goes to one link of the share
			 * (and weight 0), never to one G4 keeps out... */
			CHECK_EQ(!!fc, pc >= 0);
			if (pc >= 0) {
				CHECK(share >> pc & 1);
				CHECK(w[pc] != 0);
				CHECK_EQ(dc & out, 0);
				CHECK_EQ(dc, (bit((unsigned)pc) | dup0) & carry);
				split_new++;
			} else {
				CHECK_EQ(dc, carry);
			}
			/* ...and one link left in the share takes every big data
			 * packet, when that skips a carrier (G2). */
			if (kind[t].len > SMALL_LEN) {
				uint16_t weighted = 0;

				for (unsigned i = 0; i < 5; i++)
					if ((share >> i & 1) && w[i])
						weighted |= bit(i);
				if (weighted && !(weighted & (weighted - 1)) &&
				    (carry & ~(weighted | dup0 | (elig & ~out & ~dup0)))) {
					CHECK_EQ(dc, (uint16_t)((weighted | dup0) & carry));
					lone++;
				}
				/* With 2 links or more to share, both split it. */
				if (weighted & (weighted - 1))
					CHECK(pc >= 0 && pa >= 0);
			}
		}
	}
	CHECK_EQ(shapes, 16807u);
	CHECK(split_old > 0 && split_new > split_old && lone > 0);
}

void test_split(void)
{
	test_split_proportions();
	test_split_interleave();
	test_split_eligible();
	test_split_credits();
	test_split_duplicate();
	test_split_dst();
	test_split_flag();
	test_split_stale();
	test_split_modes();
	test_split_tail();
	test_split_spread();
	test_split_spread_lead();
	test_split_spread_kinds();
	test_split_dwell();
	test_split_qleft_gap();
	test_split_spread_stall();
	test_split_spread_base();
	test_split_sdup();
	test_split_spread_copy();
}
