/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "gf256.h"
#include "repair.h"
#include "rlc.h"
#include "test.h"

static struct cg_gf G;

/* A receiver's ring, as the server's (fec.h's arrays). */
static uint32_t r_seq[CG_REP_RING];
static uint16_t r_len[CG_REP_RING];
static uint8_t r_have[CG_REP_RING];
static uint8_t r_mem[CG_REP_RING * CG_SYM_PLEN];

static struct cg_rep_ring ring_new(void)
{
	struct cg_rep_ring r = { r_seq, r_len, r_have, r_mem };

	memset(r_have, 0, sizeof(r_have));
	return r;
}

static void ring_put(struct cg_rep_ring *r, uint32_t seq, const uint8_t *p, uint16_t len)
{
	uint32_t i = seq & (CG_REP_RING - 1);

	r->seq[i] = seq;
	r->len[i] = len;
	r->have[i] = 1;
	memcpy(r->mem + (size_t)i * CG_SYM_PLEN, p, len);
}

/* Made-up packets: seq's payload is a pattern of seq, of length 40 + seq % 900. */
static uint16_t pkt(uint32_t seq, uint8_t *p)
{
	uint16_t len = (uint16_t)(40 + seq % 900);

	for (unsigned i = 0; i < len; i++)
		p[i] = (uint8_t)(seq * 31 + i * 7 + (i >> 3));
	return len;
}

static int ring_ok(const struct cg_rep_ring *r, uint32_t seq)
{
	uint8_t p[CG_SYM_PLEN];
	uint16_t len = pkt(seq, p), l2;
	const uint8_t *q;

	if (!cg_rep_has(r, seq))
		return 0;
	q = cg_rep_data(r, seq, &l2);
	return l2 == len && !memcmp(p, q, len);
}

static void test_gf(void)
{
	uint8_t a[16], b[16], c[16], m[4 * 4], inv[4 * 4], t[4 * 4];

	cg_gf_init(&G);
	for (unsigned x = 1; x < 256; x++)
		CHECK_EQ(G.mul[x][G.inv[x]], 1);
	CHECK_EQ(G.mul[2][128], 0x1d); /* x^8 = x^4 + x^3 + x^2 + 1 */
	for (unsigned x = 0; x < 256; x += 17)
		for (unsigned y = 0; y < 256; y += 13)
			for (unsigned z = 0; z < 256; z += 29)
				CHECK_EQ(G.mul[x][y ^ z], G.mul[x][y] ^ G.mul[x][z]);
	for (int i = 0; i < 16; i++) {
		a[i] = (uint8_t)(i * 37 + 5);
		b[i] = (uint8_t)(i * 11 + 1);
	}
	memcpy(c, a, 16);
	cg_gf_madd(&G, c, b, 0x53, 16);
	for (int i = 0; i < 16; i++)
		CHECK_EQ(c[i], a[i] ^ G.mul[0x53][b[i]]);
	/* a Vandermonde matrix and its inverse */
	for (unsigned r = 0; r < 4; r++)
		for (unsigned k = 0, v = 1; k < 4; k++, v = G.mul[v][r + 1])
			m[r * 4 + k] = (uint8_t)v;
	memcpy(t, m, sizeof(t));
	CHECK_EQ(cg_gf_invert(&G, t, inv, 4), 0);
	for (unsigned r = 0; r < 4; r++)
		for (unsigned k = 0; k < 4; k++) {
			uint8_t s = 0;

			for (unsigned j = 0; j < 4; j++)
				s ^= G.mul[m[r * 4 + j]][inv[j * 4 + k]];
			CHECK_EQ(s, r == k);
		}
	memset(t, 0, sizeof(t));
	t[0] = 1;
	CHECK_EQ(cg_gf_invert(&G, t, inv, 4), -1);
}

static void test_rep_hdr(void)
{
	struct cg_rep_hdr h = { 0x01020304u, 0x8000000000000005ull, CG_REP_RS, 3, 2 }, g;
	uint8_t p[CG_REP_HDR + 128];
	unsigned slen;

	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 128, &g, &slen), 0);
	CHECK_EQ(g.base, h.base);
	CHECK(g.map == h.map);
	CHECK_EQ(g.kind, CG_REP_RS);
	CHECK_EQ(g.m, 3);
	CHECK_EQ(g.idx, 2);
	CHECK_EQ(slen, 128);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 100, &g, &slen), -1); /* RS: a multiple of 64 */
	h.m = 4;                                                 /* more repairs than its 3 members */
	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 128, &g, &slen), -1);
	h.m = 3;
	h.idx = 3; /* index past m */
	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 128, &g, &slen), -1);
	h.idx = 0;
	h.map = 6; /* bit 0 not set */
	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 128, &g, &slen), -1);
	h.map = 7;
	h.kind = CG_REP_RLC;
	h.m = 0;
	h.idx = 0xbeef;
	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 100, &g, &slen), 0);
	CHECK_EQ(g.idx, 0xbeef);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR, &g, &slen), -1);
	h.kind = 9;
	cg_rep_put(p, &h);
	CHECK_EQ(cg_rep_get(p, CG_REP_HDR + 100, &g, &slen), -1);
}

static struct cg_rlc_tx T;
static struct cg_rlc_rx R;

/* A stream of n packets from seq0, a repair every `every` over a window of
 * w; lost(seq) drops a packet, rlost(i) the i-th repair. Returns how many
 * of the lost ones came back (checked byte for byte). */
static unsigned rlc_run(uint32_t seq0, unsigned n, unsigned every, unsigned w, int (*lost)(uint32_t),
			int (*rlost)(unsigned), unsigned *nlost)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[CG_SYM_PLEN], sym[CG_SYM_MAX];
	uint32_t out[CG_RLC_EQ];
	unsigned back = 0, nr = 0;

	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	*nlost = 0;
	for (unsigned i = 0; i < n; i++) {
		uint32_t s = seq0 + i;
		uint16_t len = pkt(s, p);

		cg_rlc_tx_add(&T, s, p, len, 1000 * i);
		if (lost(s)) {
			(*nlost)++;
		} else {
			ring_put(&ring, s, p, len);
			back += cg_rlc_rx_data(&G, &R, &ring, s, out, CG_RLC_EQ);
		}
		if ((i + 1) % every == 0) {
			struct cg_rep_hdr h = { 0 };
			unsigned sl = cg_rlc_tx_repair(&G, &T, w, &h, sym), got;

			CHECK(sl > 2);
			if (rlost(nr++))
				continue;
			got = cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, CG_RLC_EQ);
			for (unsigned j = 0; j < got; j++)
				CHECK(ring_ok(&ring, out[j]));
			back += got;
		}
	}
	for (unsigned i = 0; i < n; i++)
		if (lost(seq0 + i) && cg_rep_has(&ring, seq0 + i))
			CHECK(ring_ok(&ring, seq0 + i));
	return back;
}

static int lose_none(uint32_t s)
{
	return 0;
}

static int lose_one_in_13(uint32_t s)
{
	return s % 13 == 5;
}

static int lose_burst(uint32_t s)
{
	return s % 64 >= 20 && s % 64 < 24; /* 4 in a row every 64 */
}

static int rlose_none(unsigned i)
{
	return 0;
}

static int rlose_third(unsigned i)
{
	return i % 3 == 1;
}

static void test_rlc_stream(void)
{
	unsigned nl, back;

	/* scattered loss, a repair every 4 over 16: everything back */
	back = rlc_run(4000000000u, 600, 4, 16, lose_one_in_13, rlose_none, &nl);
	CHECK(nl > 40);
	CHECK_EQ(back, nl);
	CHECK_EQ(R.bad, 0);
	/* the same, a third of the repairs lost too */
	back = rlc_run(77, 600, 4, 16, lose_one_in_13, rlose_third, &nl);
	CHECK_EQ(back, nl);
	/* bursts of 4: a window of 16 with a repair every 2 (8 cover each) */
	back = rlc_run(1000, 640, 2, 16, lose_burst, rlose_none, &nl);
	CHECK_EQ(nl, 40);
	CHECK_EQ(back, nl);
	/* nothing lost: every repair useless, none kept */
	back = rlc_run(5, 200, 4, 16, lose_none, rlose_none, &nl);
	CHECK_EQ(back, 0);
	CHECK_EQ(R.useless, 50);
	CHECK_EQ(R.kept, 0);
}

/* Two lost, one repair: nothing (no wrong rebuild); one of them arrives
 * late: the other comes back from the kept equation. */
static void test_rlc_late(void)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[CG_SYM_PLEN], sym[CG_SYM_MAX];
	uint32_t out[8];
	struct cg_rep_hdr h = { 0 };
	unsigned sl;

	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	for (uint32_t s = 10; s < 18; s++) {
		uint16_t len = pkt(s, p);

		cg_rlc_tx_add(&T, s, p, len, s);
		if (s != 12 && s != 15)
			ring_put(&ring, s, p, len);
	}
	sl = cg_rlc_tx_repair(&G, &T, 8, &h, sym);
	CHECK_EQ(h.base, 10);
	CHECK(h.map == 0xff);
	CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 0);
	CHECK_EQ(R.kept, 1);
	CHECK_EQ(R.want[12], 1);
	CHECK(!cg_rep_has(&ring, 12));
	ring_put(&ring, 15, p, pkt(15, p));
	CHECK_EQ(cg_rlc_rx_data(&G, &R, &ring, 15, out, 8), 1);
	CHECK_EQ(out[0], 12);
	CHECK(ring_ok(&ring, 12));
	CHECK_EQ(R.want[12], 0);
	CHECK_EQ(R.want[15], 0);
	for (unsigned k = 0; k < CG_RLC_EQ; k++)
		CHECK(!R.eq[k].used);
	/* a member longer than the repair: not one (malformed), let go */
	memset(&R, 0, sizeof(R));
	ring.have[12] = 0;
	ring.len[11] = 2000;
	CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 0);
	CHECK_EQ(R.bad, 1);
}

/* The window spans CG_REP_SPAN sequences at most; equations too old are let
 * go when a newer repair comes. */
static void test_rlc_span_age(void)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[CG_SYM_PLEN], sym[CG_SYM_MAX];
	uint32_t out[8];
	struct cg_rep_hdr h = { 0 };
	unsigned sl;

	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	cg_rlc_tx_add(&T, 100, p, pkt(100, p), 0);
	cg_rlc_tx_add(&T, 150, p, pkt(150, p), 0);
	cg_rlc_tx_add(&T, 170, p, pkt(170, p), 0);
	sl = cg_rlc_tx_repair(&G, &T, 8, &h, sym);
	CHECK_EQ(h.base, 150); /* 100 is 70 behind the newest */
	CHECK(h.map == (1ull | 1ull << 20));
	CHECK_EQ(T.fresh, 0);
	/* kept: 150 and 170 both missing */
	CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 0);
	CHECK_EQ(R.kept, 1);
	/* a repair CG_REP_AGE + 1 later lets it go */
	memset(&T, 0, sizeof(T));
	cg_rlc_tx_add(&T, 150 + CG_REP_AGE + 1, p, pkt(150 + CG_REP_AGE + 1, p), 0);
	sl = cg_rlc_tx_repair(&G, &T, 8, &h, sym);
	CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 1); /* its one member, rebuilt */
	CHECK_EQ(out[0], 150 + CG_REP_AGE + 1);
	CHECK_EQ(R.dropped, 1);
	CHECK_EQ(R.want[150 & (CG_REP_RING - 1)], 0);
	/* the credit: 3/8 of a repair a packet */
	memset(&T, 0, sizeof(T));
	{
		unsigned owed = 0;

		for (int i = 0; i < 16; i++)
			owed += cg_rlc_tx_owed(&T, 96);
		CHECK_EQ(owed, 6);
	}
	for (unsigned s = 0; s < 4096; s++)
		CHECK(cg_rlc_coef((uint16_t)s, s % 64));
}


/* The parity asked for again (harq.h, story 023): three lost among 16, one
 * repair kept, so a deficit of 2 over exactly those three; two fresh
 * combinations of only them, built from the sender's copies (not its
 * window), settle all three. An unknown linked through a second equation
 * joins the set; an equation too far apart is left out. */
static void test_rlc_parq(void)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[16][CG_SYM_PLEN], sym[CG_SYM_MAX];
	const uint8_t *mp[CG_REP_SPAN];
	uint16_t ml[CG_REP_SPAN], len[16];
	uint32_t out[8], base = 0;
	uint64_t map = 0;
	struct cg_rep_hdr h = { 0 };
	unsigned sl, k = 0, got = 0;

	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	for (uint32_t s = 100; s < 116; s++) {
		len[s - 100] = pkt(s, p[s - 100]);
		cg_rlc_tx_add(&T, s, p[s - 100], len[s - 100], s);
		if (s != 103 && s != 107 && s != 111)
			ring_put(&ring, s, p[s - 100], len[s - 100]);
	}
	CHECK_EQ(cg_rlc_rx_need(&G, &R, 103, &base, &map), 0); /* no equation yet */
	sl = cg_rlc_tx_repair(&G, &T, 16, &h, sym);
	CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 0);
	CHECK_EQ(cg_rlc_rx_need(&G, &R, 107, &base, &map), 2);
	CHECK_EQ(base, 103);
	CHECK(map == (1ull | 1ull << 4 | 1ull << 8));
	CHECK_EQ(cg_rlc_rx_need(&G, &R, 104, &base, &map), 0); /* here: no equation misses it */
	for (uint64_t m = map; m; m &= m - 1, k++) {
		mp[k] = p[base + (unsigned)__builtin_ctzll(m) - 100];
		ml[k] = len[base + (unsigned)__builtin_ctzll(m) - 100];
	}
	for (unsigned d = 0; d < 2; d++) {
		sl = cg_rlc_combine(&G, base, map, (uint16_t)(0x8000 | (d + 1)), mp, ml, &h, sym);
		CHECK_EQ(h.kind, CG_REP_RLC);
		CHECK(h.map == map);
		got += cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out + got, 8 - got);
	}
	CHECK_EQ(got, 3);
	CHECK(ring_ok(&ring, 103) && ring_ok(&ring, 107) && ring_ok(&ring, 111));
	CHECK_EQ(cg_rlc_rx_need(&G, &R, 107, &base, &map), 0);

	/* linked: 200 and 205 in one equation, 205 and 230 in another; the set
	 * is all three, deficit 1 */
	ring = ring_new();
	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	for (uint32_t s = 200; s < 240; s++) {
		uint8_t q[CG_SYM_PLEN];
		uint16_t l = pkt(s, q);

		cg_rlc_tx_add(&T, s, q, l, s);
		if (s != 200 && s != 205 && s != 230)
			ring_put(&ring, s, q, l);
		if (s == 210 || s == 235) {
			sl = cg_rlc_tx_repair(&G, &T, s == 210 ? 11 : 31, &h, sym);
			CHECK_EQ(cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out, 8), 0);
		}
	}
	CHECK_EQ(cg_rlc_rx_need(&G, &R, 230, &base, &map), 1);
	CHECK_EQ(base, 200);
	CHECK(map == (1ull | 1ull << 5 | 1ull << 30));
}

/* A combination of the given members (bit d of map: base + d), from the
 * made-up packets, kept by the receiver; how many it rebuilt (each checked
 * byte for byte into *wrong when not the packet). */
static unsigned rlc_feed_combo(struct cg_rep_ring *ring, uint32_t base, uint64_t map, uint16_t seed, unsigned *wrong)
{
	static uint8_t p[CG_REP_SPAN][CG_SYM_PLEN];
	const uint8_t *mp[CG_REP_SPAN];
	uint16_t ml[CG_REP_SPAN];
	uint8_t sym[CG_SYM_MAX];
	uint32_t out[CG_RLC_EQ];
	struct cg_rep_hdr h;
	unsigned k = 0, sl, got;

	for (uint64_t m = map; m; m &= m - 1, k++) {
		ml[k] = pkt(base + (unsigned)__builtin_ctzll(m), p[k]);
		mp[k] = p[k];
	}
	sl = cg_rlc_combine(&G, base, map, seed, mp, ml, &h, sym);
	got = cg_rlc_rx_repair(&G, &R, ring, &h, sym, sl, out, CG_RLC_EQ);
	for (unsigned j = 0; j < got; j++)
		*wrong += !ring_ok(ring, out[j]);
	return got;
}

/* The elimination swaps rows: a rebuilt unknown is the sum of the
 * equations its row came from (the combinations kept by row), never of the
 * equations that later sat in those rows. Four losses, equations over
 * {12, 14}, {20, 22}, {14, 20} and {22}: the third column's pivot is found
 * two rows down, a swap; the fourth settles all four, each the packet. */
static void test_rlc_swap(void)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[CG_SYM_PLEN];
	unsigned wrong = 0, got = 0;

	memset(&R, 0, sizeof(R));
	for (uint32_t s = 10; s < 26; s++)
		if (s != 12 && s != 14 && s != 20 && s != 22)
			ring_put(&ring, s, p, pkt(s, p));
	got += rlc_feed_combo(&ring, 12, 1ull | 1ull << 2, 0x8001, &wrong);
	got += rlc_feed_combo(&ring, 20, 1ull | 1ull << 2, 0x8002, &wrong);
	got += rlc_feed_combo(&ring, 14, 1ull | 1ull << 6, 0x8003, &wrong);
	CHECK_EQ(got, 0);
	got += rlc_feed_combo(&ring, 22, 1ull, 0x8004, &wrong);
	CHECK_EQ(got, 4);
	CHECK_EQ(wrong, 0);
	CHECK_EQ(R.bad, 0);
	CHECK(ring_ok(&ring, 12) && ring_ok(&ring, 14) && ring_ok(&ring, 20) && ring_ok(&ring, 22));
}

static uint64_t rlc_rng = 0x9e3779b97f4a7c15ull;

static unsigned rlc_rand(void)
{
	rlc_rng ^= rlc_rng << 13;
	rlc_rng ^= rlc_rng >> 7;
	rlc_rng ^= rlc_rng << 17;
	return (unsigned)(rlc_rng >> 16);
}

/* Bursty loss, lost repairs, a window of w across the 32-bit wrap, and now
 * and then the parity asked for again (cg_rlc_rx_need, the sender's fresh
 * combinations): every packet the receiver rebuilds is the packet, and
 * nothing is counted bad (the lab's thousands of bad rebuilds, story 025). */
static void test_rlc_random(void)
{
	static uint8_t hist[CG_REP_RING][CG_SYM_PLEN];
	static uint16_t hlen[CG_REP_RING];
	struct cg_rep_ring ring = ring_new();
	uint8_t sym[CG_SYM_MAX];
	uint32_t out[CG_RLC_EQ], s = 0xfffe0000u;
	unsigned burst = 0, wrong = 0, back = 0, asked = 0, nlost = 0;
	uint16_t seed = 0;

	memset(&T, 0, sizeof(T));
	memset(&R, 0, sizeof(R));
	for (unsigned i = 0; i < 200000; i++, s++) {
		uint32_t ix = s & (CG_REP_RING - 1);
		unsigned got = 0;

		hlen[ix] = pkt(s, hist[ix]);
		cg_rlc_tx_add(&T, s, hist[ix], hlen[ix], i);
		if (burst)
			burst--;
		else if (rlc_rand() % 1000 < 30)
			burst = 1 + rlc_rand() % 6;
		if (burst) {
			nlost++;
		} else {
			ring_put(&ring, s, hist[ix], hlen[ix]);
			got = cg_rlc_rx_data(&G, &R, &ring, s, out, CG_RLC_EQ);
		}
		if (i % 3 == 2 && rlc_rand() % 4) {
			struct cg_rep_hdr h;
			unsigned sl = cg_rlc_tx_repair(&G, &T, 21, &h, sym);

			got += cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out + got, CG_RLC_EQ - got);
		}
		/* now and then, the deficit of a hole 30 behind, asked for again */
		if (i % 7 == 0 && i > 64) {
			uint32_t q = s - 30, base;
			uint64_t map;
			unsigned d = cg_rlc_rx_need(&G, &R, q, &base, &map);

			for (unsigned j = 0; j < d && got < CG_RLC_EQ; j++) {
				const uint8_t *mp[CG_REP_SPAN];
				uint16_t ml[CG_REP_SPAN];
				struct cg_rep_hdr h;
				unsigned k = 0, sl;

				for (uint64_t m = map; m; m &= m - 1, k++) {
					uint32_t x = (base + (unsigned)__builtin_ctzll(m)) & (CG_REP_RING - 1);

					mp[k] = hist[x];
					ml[k] = hlen[x];
				}
				sl = cg_rlc_combine(&G, base, map, (uint16_t)(0x8000u | ++seed), mp, ml, &h, sym);
				got += cg_rlc_rx_repair(&G, &R, &ring, &h, sym, sl, out + got, CG_RLC_EQ - got);
				asked++;
			}
		}
		for (unsigned j = 0; j < got; j++)
			wrong += !ring_ok(&ring, out[j]);
		back += got;
	}
	CHECK(nlost > 10000);
	CHECK(asked > 100);
	CHECK(back > nlost / 2);
	CHECK_EQ(wrong, 0);
	CHECK_EQ(R.bad, 0);
	CHECK_EQ(R.recovered, back);
}

/* A rebuilt symbol never takes the ring's slot from a newer DATA (an old
 * equation settled late, its sequence a ring's length behind). */
static void test_rlc_ring_newer(void)
{
	struct cg_rep_ring ring = ring_new();
	uint8_t p[CG_SYM_PLEN], sym[CG_SYM_MAX];
	uint16_t len = pkt(5, p);

	ring_put(&ring, 5 + CG_REP_RING, p, pkt(5 + CG_REP_RING, p));
	len = pkt(5, p);
	sym[0] = (uint8_t)(len >> 8);
	sym[1] = (uint8_t)len;
	memcpy(sym + 2, p, len);
	CHECK_EQ(cg_rep_put_sym(&ring, 5, sym, len + 2u), -2);
	CHECK(ring_ok(&ring, 5 + CG_REP_RING));
	CHECK_EQ(cg_rep_put_sym(&ring, 5 + 2 * CG_REP_RING, sym, len + 2u), 0);
}

void test_rlc(void)
{
	test_gf();
	test_rep_hdr();
	test_rlc_stream();
	test_rlc_late();
	test_rlc_span_age();
	test_rlc_parq();
	test_rlc_swap();
	test_rlc_random();
	test_rlc_ring_newer();
}
