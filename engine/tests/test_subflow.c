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
	CHECK_EQ(cg_sf_rx_due_us(&s, 200 * MS), -1);
	/* 110 missing, 111 came: filled 10 ms later, within 20 */
	CHECK_EQ(cg_sf_rx_in(&s, 111, 300 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_poll(&s, 305 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_in(&s, 110, 310 * MS), CG_SF_FILL);
	CHECK_EQ(cg_sf_rx_poll(&s, 400 * MS, out, 8), 0);
	/* 112 missing behind 113: lost 20 ms after 113 came */
	CHECK_EQ(cg_sf_rx_in(&s, 113, 500 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_due_us(&s, 505 * MS), 15 * MS);
	CHECK_EQ(cg_sf_rx_poll(&s, 519 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_due_us(&s, 521 * MS), 0);
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

/* What slides out of the window unfilled is given up unsaid; a jump of a
 * window or more, either way, is a new start that says no loss; the
 * sequence wraps. */
static void test_sf_edges(void)
{
	struct cg_sf_rx s;
	uint32_t out[8];

	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	cg_sf_rx_in(&s, 0, 0);
	cg_sf_rx_in(&s, 2, 1 * MS); /* 1 is a gap */
	CHECK_EQ(cg_sf_rx_in(&s, CG_SF_WIN + 1, 2 * MS), CG_SF_NEW);
	CHECK_EQ(s.overflow, 1);
	CHECK_EQ(s.lost, 1);
	CHECK_EQ(s.base, 2);
	CHECK_EQ(cg_sf_rx_poll(&s, 100 * MS, out, 8), 8); /* the gaps 3.. it opened, oldest first */
	CHECK_EQ(out[0], 3);
	/* far ahead: a new start, nothing said */
	CHECK_EQ(cg_sf_rx_in(&s, 0x80000000u, 200 * MS), CG_SF_NEW);
	CHECK_EQ(s.resync, 1);
	CHECK_EQ(cg_sf_rx_poll(&s, 400 * MS, out, 8), 0);
	/* far behind: a sender that began again from 1 */
	CHECK_EQ(cg_sf_rx_in(&s, 1, 401 * MS), CG_SF_NEW);
	CHECK_EQ(s.resync, 2);
	CHECK_EQ(cg_sf_rx_in(&s, 0, 402 * MS), CG_SF_OLD); /* only just behind: old */
	CHECK_EQ(cg_sf_rx_poll(&s, 500 * MS, out, 8), 0);
	/* the wrap, where the sender skips 0 */
	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	CHECK_EQ(cg_sf_rx_in(&s, 0xFFFFFFFEu, 0), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_in(&s, 0xFFFFFFFFu, 1 * MS), CG_SF_NEW);
	CHECK_EQ(cg_sf_rx_in(&s, 1, 2 * MS), CG_SF_NEW); /* 0 is no gap */
	CHECK_EQ(cg_sf_rx_in(&s, 3, 3 * MS), CG_SF_NEW); /* 2 is */
	CHECK_EQ(cg_sf_rx_poll(&s, 30 * MS, out, 8), 1);
	CHECK_EQ(out[0], 2);
	CHECK_EQ(cg_sf_rx_in(&s, 2, 40 * MS), CG_SF_LATE);
	CHECK_EQ(s.resync, 0);
}

/* A probe says how far the link sent: the tail it did not deliver is lost
 * after the window, and still a late arrival if it comes after all. */
static void test_sf_tail(void)
{
	struct cg_sf_rx s;
	uint32_t out[8];

	cg_sf_rx_init(&s, 20 * MS, 300 * MS);
	cg_sf_rx_sent(&s, 50, 0); /* nothing seen yet: nothing to say */
	CHECK_EQ(s.tail, 0);
	for (uint32_t q = 1; q < 10; q++)
		cg_sf_rx_in(&s, q, q * MS);
	cg_sf_rx_sent(&s, 10, 50 * MS); /* all came */
	CHECK_EQ(cg_sf_rx_due_us(&s, 50 * MS), -1);
	cg_sf_rx_sent(&s, 13, 100 * MS); /* 10, 11 and 12 went and did not come */
	CHECK_EQ(s.tail, 3);
	CHECK_EQ(cg_sf_rx_poll(&s, 119 * MS, out, 8), 0);
	CHECK_EQ(cg_sf_rx_poll(&s, 120 * MS, out, 8), 3);
	CHECK_EQ(out[0], 10);
	CHECK_EQ(out[2], 12);
	CHECK_EQ(cg_sf_rx_in(&s, 11, 130 * MS), CG_SF_LATE);
	CHECK_EQ(cg_sf_rx_in(&s, 13, 131 * MS), CG_SF_NEW);
	cg_sf_rx_sent(&s, 12, 140 * MS); /* behind: nothing */
	cg_sf_rx_sent(&s, 14 + CG_SF_WIN, 140 * MS); /* a window ahead: nothing */
	CHECK_EQ(s.tail, 3);
}

/* The sender numbers from 1, skips 0 when it wraps, takes back what did not
 * go out, and finds what a lost sseq carried while it remembers it. */
static void test_sf_tx(void)
{
	struct cg_sf_tx t;
	uint32_t seq = 0;

	cg_sf_tx_init(&t);
	CHECK_EQ(cg_sf_tx_take(&t, 500, 1400, 0), 1);
	CHECK_EQ(cg_sf_tx_take(&t, 501, 1400, 0), 2);
	CHECK_EQ(cg_sf_tx_take(&t, 502, 1400, 0), 3);
	cg_sf_tx_unsend(&t, 2); /* 2 and 3 did not go */
	CHECK_EQ(cg_sf_tx_take(&t, 503, 1400, 0), 2);
	CHECK(cg_sf_tx_find(&t, 2, &seq));
	CHECK_EQ(seq, 503);
	CHECK(!cg_sf_tx_find(&t, 3, &seq)); /* never went */
	CHECK(cg_sf_tx_find(&t, 1, &seq));
	CHECK_EQ(seq, 500);
	for (uint32_t i = 0; i < CG_SF_WIN; i++)
		cg_sf_tx_take(&t, 1000 + i, 1400, 0);
	CHECK(!cg_sf_tx_find(&t, 1, &seq)); /* forgotten */
	CHECK(!cg_sf_tx_find(&t, 0, &seq));
	/* the wrap */
	t.next = 0xFFFFFFFFu;
	CHECK_EQ(cg_sf_tx_take(&t, 7, 1400, 0), 0xFFFFFFFFu);
	CHECK_EQ(cg_sf_tx_take(&t, 8, 1400, 0), 1);
	cg_sf_tx_unsend(&t, 2);
	CHECK_EQ(t.next, 0xFFFFFFFFu);
}

/* The receiver counts what each link delivered and says it once; the
 * sender turns the answer into the link's flight, a delivery rate on the
 * receiver's clock and a round trip less the receiver's wait. */
static void test_sf_ack(void)
{
	struct cg_sf_rx r;
	struct cg_sf_tx t;
	struct cg_sfack_e e = { 0 }, g;
	uint8_t pay[CG_SFACK_LEN];
	uint32_t q;

	cg_sf_rx_init(&r, 10 * MS, 300 * MS);
	CHECK(!cg_sf_rx_ack(&r, 0, &e.top, &e.gotb, &e.rts, &e.age));
	cg_sf_tx_init(&t);
	/* 20 packets of 1000 bytes, one a ms; the first 10 come 40 ms later */
	for (uint32_t i = 0; i < 20; i++)
		cg_sf_tx_take(&t, 100 + i, 1000, i * MS);
	CHECK_EQ(t.lc.inflight, 20000);
	for (q = 1; q <= 10; q++)
		cg_sf_rx_took(&r, q, cg_sf_rx_in(&r, q, (q + 39) * MS), 1000, (q + 39) * MS);
	cg_sf_rx_took(&r, 3, cg_sf_rx_in(&r, 3, 50 * MS), 1000, 50 * MS); /* a copy: not counted */
	CHECK(cg_sf_rx_ack(&r, 52 * MS, &e.top, &e.gotb, &e.rts, &e.age));
	CHECK_EQ(e.top, 10);
	CHECK_EQ(e.gotb, 10000);
	CHECK_EQ(e.rts, 49 * MS);
	CHECK_EQ(e.age, 3 * MS);
	CHECK(!cg_sf_rx_ack(&r, 53 * MS, &e.top, &e.gotb, &e.rts, &e.age)); /* said already */
	/* on the wire and back */
	e.link = 3;
	cg_sfack_put(pay, 0, &e);
	CHECK_EQ(pay[0], 1);
	cg_sfack_get(pay, 0, &g);
	CHECK_EQ(g.link, 3);
	CHECK_EQ(g.top, 10);
	CHECK_EQ(g.gotb, 10000);
	CHECK_EQ(g.rts, 49 * MS);
	CHECK_EQ(g.age, 3 * MS);
	/* the first answer is the base: no rate, the round trip less the
	 * receiver's wait (sseq 10 went at 9 ms, the answer came at 70) */
	cg_sf_tx_ack(&t, g.top, g.gotb, g.rts, g.age, 70 * MS);
	CHECK_EQ(t.lc.inflight, 10000);
	CHECK_EQ(t.lc.rtprop, 58 * MS);
	CHECK_EQ(t.lc.bw, 0);
	/* 10 more go after the base was known, and come one a ms */
	for (uint32_t i = 0; i < 10; i++)
		cg_sf_tx_take(&t, 200 + i, 1000, (70 + i) * MS);
	for (q = 11; q <= 30; q++)
		cg_sf_rx_took(&r, q, cg_sf_rx_in(&r, q, (q + 89) * MS), 1000, (q + 89) * MS);
	CHECK(cg_sf_rx_ack(&r, 120 * MS, &e.top, &e.gotb, &e.rts, &e.age));
	CHECK_EQ(e.gotb, 30000);
	cg_sf_tx_ack(&t, e.top, e.gotb, e.rts, e.age, 135 * MS);
	CHECK_EQ(t.lc.inflight, 0);
	/* 20000 bytes from rts 49 ms to 119 ms */
	CHECK_EQ(t.lc.bw, 20000ull * 1000000 / (70 * MS));
	/* an older answer says nothing */
	cg_sf_tx_ack(&t, 20, 20000, 109 * MS, 0, 140 * MS);
	CHECK_EQ(t.acks, 2);
	/* the receiver started again: its bytes went back, a new base */
	cg_sf_tx_take(&t, 300, 1000, 140 * MS);
	cg_sf_tx_ack(&t, 31, 1000, 5 * MS, 0, 190 * MS);
	CHECK_EQ(t.acks, 3);
	CHECK_EQ(t.got, 1000);
	/* app-limited, a smaller rate says nothing */
	t.app = 1;
	cg_sf_tx_take(&t, 301, 1000, 200 * MS);
	CHECK_EQ(t.app_n, 1);
	cg_sf_tx_ack(&t, 32, 2000, 1005 * MS, 0, 250 * MS);
	CHECK_EQ(t.lc.bw, 20000ull * 1000000 / (70 * MS));
}

void test_subflow(void)
{
	test_sf_ack();
	test_sf_tx();
	test_sf_basic();
	test_sf_late();
	test_sf_edges();
	test_sf_tail();
}
