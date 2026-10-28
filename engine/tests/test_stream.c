/* stream.h: the reliable byte stream of the protocontroller (variant B) (story 025).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "stream.h"
#include "test.h"

#define NCH 64

static uint8_t mem[NCH * CG_ST_CHUNK];
static uint32_t stack[NCH];

static void test_pool_buf(void)
{
	struct cg_st_pool p;
	struct cg_st_buf b;
	uint8_t src[40000], dst[40000];
	uint32_t room;

	cg_st_pool_init(&p, mem, stack, NCH);
	cg_st_buf_init(&b);
	for (unsigned i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 7 + 3);
	/* across chunks, then read back */
	CHECK_EQ(cg_st_buf_write(&b, &p, 1000, src, sizeof(src)), 0);
	CHECK_EQ(b.nch, 3); /* bytes 1000 .. 40999: chunks 0, 1, 2 */
	CHECK_EQ(cg_st_pool_used(&p), 3);
	CHECK_EQ(cg_st_buf_read(&b, &p, 1000, dst, sizeof(dst)), 0);
	CHECK(!memcmp(src, dst, sizeof(src)));
	CHECK(cg_st_buf_at(&b, &p, 16383, 0, &room) != NULL);
	CHECK_EQ(room, 1);
	/* a missing chunk is not read */
	CHECK_EQ(cg_st_buf_read(&b, &p, 60000, dst, 10), -1);
	/* released below 32768: chunks 0 and 1 go back */
	cg_st_buf_release(&b, &p, 32768);
	CHECK_EQ(b.nch, 1);
	CHECK_EQ(cg_st_pool_used(&p), 1);
	CHECK(cg_st_buf_at(&b, &p, 1000, 0, NULL) == NULL); /* under lo */
	/* the window ends CG_ST_SLOTS chunks from lo's */
	CHECK(cg_st_buf_in(&b, 32768 + (uint64_t)(CG_ST_SLOTS - 1) * CG_ST_CHUNK));
	CHECK(!cg_st_buf_in(&b, 32768 + (uint64_t)CG_ST_SLOTS * CG_ST_CHUNK));
	cg_st_buf_free(&b, &p);
	CHECK_EQ(cg_st_pool_used(&p), 0);
	CHECK_EQ(p.peak, 3);
	/* out of chunks: the write fails */
	cg_st_buf_init(&b);
	{
		static uint8_t big[(NCH + 1) * CG_ST_CHUNK];

		CHECK_EQ(cg_st_buf_write(&b, &p, 0, big, sizeof(big)), -1);
		CHECK_EQ(cg_st_pool_used(&p), NCH);
		cg_st_buf_free(&b, &p);
		CHECK_EQ(cg_st_pool_used(&p), 0);
	}
}

static void test_unwrap(void)
{
	CHECK_EQ(cg_st_unwrap(0, 5), 5);
	CHECK_EQ(cg_st_unwrap(0x1fffffff0ull, 0x10), 0x200000010ull);
	CHECK_EQ(cg_st_unwrap(0x200000010ull, 0xfffffff0u), 0x1fffffff0ull);
	CHECK_EQ(cg_st_unwrap(10, 0xfffffff0u), 0); /* never under 0 */
}

static void test_rx(void)
{
	struct cg_st_rx r;
	struct cg_st_ack a;

	cg_st_rx_init(&r);
	CHECK_EQ(cg_st_rx_judge(&r, 0, 100, 0, 1000), CG_ST_RX_NEW);
	CHECK_EQ(cg_st_rx_add(&r, 0, 100, 0), 1);
	CHECK_EQ(r.nxt, 100);
	CHECK_EQ(cg_st_rx_judge(&r, 0, 100, 0, 1000), CG_ST_RX_DUP);
	/* a hole: 200..300 and 400..500 above it */
	CHECK_EQ(cg_st_rx_add(&r, 200, 100, 0), 0);
	CHECK_EQ(cg_st_rx_add(&r, 400, 100, 0), 0);
	CHECK_EQ(r.nr, 2);
	CHECK_EQ(cg_st_rx_judge(&r, 200, 100, 0, 1000), CG_ST_RX_DUP);
	cg_st_rx_ack(&r, 2000, 7, &a);
	CHECK_EQ(a.cum, 100);
	CHECK_EQ(a.n, 2);
	CHECK_EQ(a.s[0], 400); /* the newest frame's range first */
	CHECK_EQ(a.e[0], 500);
	CHECK_EQ(a.s[1], 200);
	CHECK_EQ(a.e[1], 300);
	CHECK_EQ(a.aseq, 7);
	/* filling 300..400 joins the two ranges */
	cg_st_rx_add(&r, 300, 100, 0);
	CHECK_EQ(r.nr, 1);
	CHECK_EQ(r.rs[0], 200);
	CHECK_EQ(r.re[0], 500);
	/* and the hole: all in order */
	cg_st_rx_add(&r, 100, 100, 0);
	CHECK_EQ(r.nr, 0);
	CHECK_EQ(r.nxt, 500);
	CHECK_EQ(cg_st_rx_ready(&r, 0), 500);
	/* beyond the window */
	CHECK_EQ(cg_st_rx_judge(&r, 900, 200, 0, 1000), CG_ST_RX_DROP);
	/* the FIN after 600 bytes, then the bytes before it */
	CHECK_EQ(cg_st_rx_judge(&r, 550, 50, 1, 1000), CG_ST_RX_NEW);
	cg_st_rx_add(&r, 550, 50, 1);
	CHECK_EQ(r.fin, 600);
	CHECK(!cg_st_rx_ended(&r));
	CHECK_EQ(cg_st_rx_judge(&r, 700, 10, 0, 1000), CG_ST_RX_DROP); /* past the FIN */
	cg_st_rx_add(&r, 500, 50, 0);
	CHECK(cg_st_rx_ended(&r));
	CHECK_EQ(r.nxt, 601); /* the FIN's byte */
	CHECK_EQ(cg_st_rx_ready(&r, 500), 100);
	/* a FIN alone, at nxt */
	cg_st_rx_init(&r);
	cg_st_rx_add(&r, 0, 0, 1);
	CHECK(cg_st_rx_ended(&r));
	CHECK_EQ(cg_st_rx_ready(&r, 0), 0);
	/* out of ranges: a new one is dropped, one that touches is not */
	cg_st_rx_init(&r);
	for (unsigned i = 0; i < CG_ST_RANGES; i++)
		cg_st_rx_add(&r, 100 + 20 * i, 10, 0);
	CHECK_EQ(r.nr, CG_ST_RANGES);
	CHECK_EQ(cg_st_rx_judge(&r, 50000, 10, 0, 100000), CG_ST_RX_DROP);
	CHECK_EQ(cg_st_rx_judge(&r, 5195, 10, 0, 100000), CG_ST_RX_NEW); /* touches the last */
	CHECK_EQ(cg_st_rx_judge(&r, 110, 10, 0, 100000), CG_ST_RX_NEW);
	CHECK_EQ(cg_st_rx_judge(&r, 0, 10, 0, 100000), CG_ST_RX_NEW); /* at nxt */
	/* a merge with every range taken keeps them all (the count once went
	 * through 8 bits: 256 ranges and a merge forgot them all) */
	cg_st_rx_add(&r, 5195, 10, 0);
	CHECK_EQ(r.nr, CG_ST_RANGES);
	CHECK_EQ(r.rs[CG_ST_RANGES - 1], 5195);
	cg_st_rx_add(&r, 110, 10, 0); /* joins the first two */
	CHECK_EQ(r.nr, CG_ST_RANGES - 1);
	CHECK_EQ(r.nxt, 0);
}

/* A receiver that forgot what it SACKed: at the timeout, the oldest segment
 * SACKed while the cumulative offset waits at it, every SACKed one goes
 * again. */
static void test_renege(void)
{
	struct cg_st_seg seg[16];
	struct cg_st_tx t;
	struct cg_st_ack a;
	struct cg_st_ackres r;
	uint32_t idx = 0, len = 0;
	uint64_t off = 0;
	int fin = 0;

	cg_st_tx_init(&t, seg, 16, 10000);
	t.tail = 3000;
	for (int i = 0; i < 3; i++) {
		cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin);
		cg_st_tx_sent(&t, idx, off, len, fin, 100 + i, 0, 0, 0, 0, 0, 0);
	}
	memset(&a, 0, sizeof(a));
	a.aseq = 1;
	a.cum = 0;
	a.edge = 10000;
	a.n = 1;
	a.s[0] = 0;
	a.e[0] = 2000; /* SACKs the first two, the cumulative offset stays */
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(cg_st_tx_seg(&t, 0)->st, CG_ST_SACKED);
	CHECK_EQ(t.inflight, 1000);
	CHECK_EQ(cg_st_tx_rto(&t, 1000000, 500000, NULL), 1000); /* only the SENT one counts as lost */
	CHECK_EQ(t.reneged, 1);
	CHECK_EQ(cg_st_tx_seg(&t, 0)->st, CG_ST_LOST);
	CHECK_EQ(cg_st_tx_seg(&t, 1)->st, CG_ST_LOST);
	CHECK_EQ(t.nlost, 3);
	CHECK_EQ(t.lost_from, 0);
	CHECK_EQ(t.inflight, 0);
	/* they go again, oldest first */
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(idx, 0);
	CHECK_EQ(off, 0);
}

static void test_tx(void)
{
	struct cg_st_seg seg[16];
	struct cg_st_tx t;
	struct cg_st_ack a;
	struct cg_st_ackres r;
	uint32_t idx, len;
	uint64_t off;
	int fin;

	cg_st_tx_init(&t, seg, 16, 10000);
	CHECK(!cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin)); /* nothing buffered */
	t.tail = 2500;
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(idx, 0);
	CHECK_EQ(off, 0);
	CHECK_EQ(len, 1000);
	CHECK_EQ(cg_st_tx_sent(&t, idx, off, len, fin, 100, 0, 0, 0, 0, 0, 0), 1000);
	cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin);
	cg_st_tx_sent(&t, idx, off, len, fin, 200, 0, 0, 0, 0, 0, 0);
	cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin);
	CHECK_EQ(len, 500);
	CHECK(!fin);
	cg_st_tx_sent(&t, idx, off, len, fin, 300, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(t.inflight, 2500);
	CHECK_EQ(t.n, 3);
	CHECK(!cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	/* the second segment SACKed: the first, sent 100 us before it, is
	 * lost for a window of 50 */
	memset(&a, 0, sizeof(a));
	a.aseq = 1;
	a.cum = 0;
	a.edge = 10000;
	a.n = 1;
	a.s[0] = 1000;
	a.e[0] = 2000;
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(r.delivered, 1000);
	CHECK(!r.progress);
	CHECK(r.rs.have);
	CHECK_EQ(r.rs.sent_us, 200);
	CHECK_EQ(t.inflight, 1500);
	CHECK_EQ(cg_st_tx_rack(&t, 150, NULL), 0);
	CHECK_EQ(cg_st_tx_rack(&t, 50, NULL), 1000);
	CHECK_EQ(t.nlost, 1);
	/* the lost one goes first, again */
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(idx, 0);
	CHECK_EQ(off, 0);
	cg_st_tx_sent(&t, idx, off, len, fin, 400, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(cg_st_tx_seg(&t, 0)->tx, 2);
	CHECK_EQ(t.retx_segs, 1);
	/* cumulative to 2000: two segments go, una moves */
	a.aseq = 2;
	a.cum = 2000;
	a.n = 0;
	cg_st_tx_ack(&t, &a, &r);
	CHECK(r.progress);
	CHECK_EQ(r.freed, 2000);
	CHECK_EQ(t.una, 2000);
	CHECK_EQ(t.n, 1);
	CHECK_EQ(t.inflight, 500);
	/* an older ACK does not move the edge back */
	a.aseq = 1;
	a.edge = 5;
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(t.edge, 10000);
	/* the timer: the oldest SENT is lost */
	CHECK_EQ(cg_st_tx_rto(&t, 1000000, 500000, NULL), 500);
	CHECK(cg_st_tx_busy(&t));
	cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin);
	cg_st_tx_sent(&t, idx, off, len, fin, 1000000, 0, 0, 0, 0, 0, 0);
	/* the socket ends: the FIN alone, past the window too */
	t.eof = 1;
	t.edge = 2500;
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(len, 0);
	CHECK(fin);
	CHECK_EQ(off, 2500);
	cg_st_tx_sent(&t, idx, off, len, fin, 1000100, 0, 0, 0, 0, 0, 0);
	CHECK(t.fin_sent);
	CHECK(!cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK(!cg_st_tx_done(&t));
	a.aseq = 3;
	a.cum = 2501;
	cg_st_tx_ack(&t, &a, &r);
	CHECK(cg_st_tx_done(&t));
	CHECK_EQ(t.inflight, 0);
	CHECK_EQ(t.n, 0);
	/* the window holds new bytes back */
	cg_st_tx_init(&t, seg, 16, 1500);
	t.tail = 5000;
	cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin);
	cg_st_tx_sent(&t, idx, off, len, fin, 1, 0, 0, 0, 0, 0, 0);
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(len, 500);
	cg_st_tx_sent(&t, idx, off, len, fin, 2, 0, 0, 0, 0, 0, 0);
	CHECK(!cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
}

/* Two ends through a channel that loses, duplicates and reorders: every
 * byte arrives, in order, and the FIN; the sender never gives up. */
/* A suspect (cg_st_tx_suspect, story 029): its link lost it, a code may
 * still rebuild it. Lost at its deadline if still SENT; delivered first
 * (SACKed), never; sent again, its deadline goes. */
static void test_suspect(void)
{
	struct cg_st_seg seg[16];
	struct cg_st_tx t;
	struct cg_st_ack a;
	struct cg_st_ackres r;
	uint32_t idx = 0, len = 0;
	uint64_t off = 0, next = 0, lq = 0;
	int fin = 0;

	cg_st_tx_init(&t, seg, 16, 1u << 20);
	t.tail = 4000;
	for (int i = 0; i < 4; i++) {
		CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
		cg_st_tx_sent(&t, idx, off, len, fin, 1000 * (uint64_t)(i + 1), 0, 0, 0, 0, 0, 0);
	}
	/* 1 and 2 suspects (deadlines 50 and 40 ms); not sent there, stale */
	CHECK_EQ(cg_st_tx_suspect(&t, 1000, 2000, 50000), CG_ST_NACK_SUSPECT);
	CHECK_EQ(cg_st_tx_suspect(&t, 2000, 3000, 40000), CG_ST_NACK_SUSPECT);
	CHECK_EQ(cg_st_tx_suspect(&t, 2500, 3000, 40000), CG_ST_NACK_UNKNOWN);
	CHECK_EQ(cg_st_tx_suspect(&t, 3000, 1, 40000), CG_ST_NACK_STALE);
	CHECK_EQ(t.nlost, 0);
	/* 2 is delivered (rebuilt by the code): SACKed, no longer a suspect */
	memset(&a, 0, sizeof(a));
	a.aseq = 1;
	a.edge = t.edge;
	a.n = 1;
	a.s[0] = 2000;
	a.e[0] = 3000;
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(cg_st_tx_suspects(&t, 39000, &next, &lq), 0);
	CHECK_EQ(next, 50000);
	CHECK_EQ(cg_st_tx_suspects(&t, 45000, &next, &lq), 0);
	/* 1 at its deadline: lost, by the NACK (the tunnel's word) */
	CHECK_EQ(cg_st_tx_suspects(&t, 50000, &next, &lq), 1000);
	CHECK_EQ(next, 0);
	CHECK_EQ(t.nlost, 1);
	CHECK_EQ(cg_st_tx_seg(&t, 1)->by, CG_ST_BY_NACK);
	CHECK_EQ(cg_st_tx_suspect(&t, 1000, 2000, 90000), CG_ST_NACK_PENDING);
	/* sent again: a new suspect only for the new copy */
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(idx, 1);
	cg_st_tx_sent(&t, idx, off, len, fin, 60000, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(cg_st_tx_seg(&t, 1)->sus_us, 0);
	CHECK_EQ(cg_st_tx_suspects(&t, 1000000, &next, &lq), 0);
}

/* The tunnel's NACK of a DATA (cg_st_tx_nack): a loss only for the
 * segment's latest transmission, a no-op for what was delivered, sent
 * again since or lost already; its retransmission counted as the NACK's. */
static void test_nack(void)
{
	struct cg_st_seg seg[16];
	struct cg_st_tx t;
	struct cg_st_ack a;
	struct cg_st_ackres r;
	uint32_t idx = 0, len = 0, b;
	uint64_t off = 0, lq = 0;
	int fin = 0;

	cg_st_tx_init(&t, seg, 16, 0xFFFFFC00ull + (1u << 20));
	t.una = t.nxt = t.tail = 0xFFFFFC00ull; /* across the wire's 32 bits */
	t.tail += 4000;
	for (int i = 0; i < 4; i++) {
		CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
		cg_st_tx_sent(&t, idx, off, len, fin, 0x100000000ull + 1000 * (uint64_t)i, 0, 0, 0, 0, 0, i == 1);
	}
	CHECK_EQ(t.inflight, 4000);
	/* no segment starts there */
	CHECK_EQ(cg_st_tx_nack(&t, 0xFFFFFC00u + 500, 0, &b, NULL), CG_ST_NACK_UNKNOWN);
	CHECK_EQ(cg_st_tx_nack(&t, (uint32_t)(0xFFFFFC00ull + 5000), 0, &b, NULL), CG_ST_NACK_UNKNOWN);
	/* segment 1 asked for with its own clock (sent with a queue): lost now */
	CHECK_EQ(cg_st_tx_nack(&t, (uint32_t)(0xFFFFFC00ull + 1000), 1000, &b, &lq), CG_ST_NACK_LOST);
	CHECK_EQ(b, 1000);
	CHECK_EQ(lq, 1000);
	CHECK_EQ(t.nlost, 1);
	CHECK_EQ(t.inflight, 3000);
	CHECK_EQ(t.nack_lost, 1000);
	/* its second copy of the NACK: lost already */
	CHECK_EQ(cg_st_tx_nack(&t, (uint32_t)(0xFFFFFC00ull + 1000), 1000, &b, NULL), CG_ST_NACK_PENDING);
	CHECK_EQ(b, 0);
	/* it goes again first, counted as the NACK's */
	CHECK(cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin));
	CHECK_EQ(idx, 1);
	cg_st_tx_sent(&t, idx, off, len, fin, 0x100000000ull + 9000, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(t.retx_by[CG_ST_BY_NACK], 1);
	CHECK_EQ(t.retx_by[CG_ST_BY_RACK], 0);
	/* a NACK of the first copy now: a newer one is in flight */
	CHECK_EQ(cg_st_tx_nack(&t, (uint32_t)(0xFFFFFC00ull + 1000), 1000, &b, NULL), CG_ST_NACK_STALE);
	CHECK_EQ(t.nlost, 0);
	/* the third segment's offset is past 2^32: its low bits unwrap */
	CHECK(cg_st_tx_seg(&t, 2)->off > 0xFFFFFFFFull);
	/* SACKed: a DSACK, nothing */
	memset(&a, 0, sizeof(a));
	a.aseq = 1;
	a.cum = t.una;
	a.edge = t.edge;
	a.n = 1;
	a.s[0] = 0xFFFFFC00ull + 2000;
	a.e[0] = 0xFFFFFC00ull + 3000;
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(cg_st_tx_nack(&t, (uint32_t)(0xFFFFFC00ull + 2000), 2000, &b, NULL), CG_ST_NACK_DELIVERED);
	/* acknowledged: below una, nothing */
	a.aseq = 2;
	a.cum = 0xFFFFFC00ull + 1000;
	a.n = 0;
	cg_st_tx_ack(&t, &a, &r);
	CHECK_EQ(cg_st_tx_nack(&t, 0xFFFFFC00u, 0, &b, NULL), CG_ST_NACK_DELIVERED);
	/* the timer marks the two still SENT (the NACK's retransmission and
	 * the last one): their retransmissions counted as the timer's */
	CHECK_EQ(cg_st_tx_rto(&t, 0x100000000ull + 100000, 50000, NULL), 2000);
	while (cg_st_tx_next(&t, 1000, &idx, &off, &len, &fin) && idx < t.n)
		cg_st_tx_sent(&t, idx, off, len, fin, 0x100000000ull + 100000, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(t.retx_by[CG_ST_BY_RTO], 2);
	CHECK_EQ(t.retx_segs, 3);
}

#define E2E_BYTES 300000u
#define E2E_MSS 1200u

struct pkt {
	uint64_t at;
	int ack;
	int nack;    /* the tunnel's NACK of a DATA lost on every copy (off, ts) */
	uint32_t ts; /* the sender's clock in that DATA */
	uint64_t off;
	uint32_t len;
	int fin;
	struct cg_st_ack a;
};

static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* A repeat goes as a copy from its second retransmission, and only among
 * the CG_ST_COPY_HEAD oldest segments (the holes that hold the stream). */
static void test_copy(void)
{
	struct cg_st_seg seg[32];
	struct cg_st_tx t;

	cg_st_tx_init(&t, seg, 32, 1 << 20);
	t.n = 20;
	for (uint32_t i = 0; i < 20; i++) {
		seg[i].off = 1000 * i;
		seg[i].tx = 1;
	}
	CHECK(!cg_st_tx_copy(&t, 0));
	seg[0].tx = 2;
	CHECK(cg_st_tx_copy(&t, 0));
	seg[CG_ST_COPY_HEAD - 1].tx = 3;
	CHECK(cg_st_tx_copy(&t, CG_ST_COPY_HEAD - 1));
	seg[CG_ST_COPY_HEAD].tx = 3;
	CHECK(!cg_st_tx_copy(&t, CG_ST_COPY_HEAD)); /* past the head */
	CHECK(!cg_st_tx_copy(&t, 20));             /* new bytes */
}

/* nack: the tunnel asks for 80 % of the DATA lost on every copy, the
 * stream takes its NACK (cg_st_tx_nack), RACK's window wide (as when the
 * code's repairs come late) so that most losses go by NACK. */
static void e2e_run(int nack)
{
	static struct pkt q[4096];
	static struct cg_st_seg seg[512];
	static uint8_t src[E2E_BYTES], dst[E2E_BYTES];
	struct cg_st_pool p;
	struct cg_st_buf sb, rb;
	struct cg_st_tx t;
	struct cg_st_rx r;
	uint32_t nq = 0, seed = 12345, sent = 0, steps = 0;
	uint64_t now = 0, consumed = 0, rto_at = 0;
	uint16_t aseq = 0;
	const uint64_t cap = 200000;
	uint64_t nacks[CG_ST_NACK_N] = { 0 };

	cg_st_pool_init(&p, mem, stack, NCH);
	cg_st_buf_init(&sb);
	cg_st_buf_init(&rb);
	cg_st_rx_init(&r);
	cg_st_tx_init(&t, seg, 512, cap);
	for (unsigned i = 0; i < E2E_BYTES; i++)
		src[i] = (uint8_t)rnd(&seed);
	while (!(cg_st_tx_done(&t) && cg_st_rx_ended(&r)) && steps < 2000000) {
		uint32_t idx, len;
		uint64_t off;
		int fin;

		steps++;
		now += 50;
		/* the socket: what fits in the sender's buffer */
		while (t.tail < E2E_BYTES && t.tail - t.una < cap) {
			uint32_t k = E2E_BYTES - (uint32_t)t.tail, room = 0;
			uint8_t *d = cg_st_buf_at(&sb, &p, t.tail, 1, &room);

			if (k > room)
				k = room;
			memcpy(d, src + t.tail, k);
			t.tail += k;
		}
		if (t.tail == E2E_BYTES)
			t.eof = 1;
		/* send what may go, a few per step (the pacer's job) */
		for (int k = 0; k < 4 && nq < 4000 && cg_st_tx_next(&t, E2E_MSS, &idx, &off, &len, &fin); k++) {
			struct pkt *x;
			uint32_t dup = rnd(&seed) % 100 < 2 ? 2 : 1, arrived = 0;

			cg_st_tx_sent(&t, idx, off, len, fin, now, 0, 0, 0, 0, 0, 0);
			sent++;
			if (!rto_at)
				rto_at = now + 20000;
			for (uint32_t d = 0; d < dup; d++) {
				if (rnd(&seed) % 100 < 10)
					continue; /* lost: 10 % */
				x = &q[nq++];
				memset(x, 0, sizeof(*x));
				x->at = now + 5000 + rnd(&seed) % 3000; /* reordered by up to 3 ms */
				x->off = off;
				x->len = len;
				x->fin = fin;
				arrived++;
			}
			if (nack && !arrived && rnd(&seed) % 100 < 80) {
				x = &q[nq++];
				memset(x, 0, sizeof(*x));
				x->nack = 1;
				x->at = now + 10000 + rnd(&seed) % 3000; /* a round trip and the gap's detection */
				x->off = off;
				x->ts = (uint32_t)now;
			}
		}
		/* deliver what is due */
		for (uint32_t i = 0; i < nq;) {
			struct pkt *x = &q[i];

			if (x->at > now) {
				i++;
				continue;
			}
			if (x->nack) {
				uint32_t b;
				int v = cg_st_tx_nack(&t, (uint32_t)x->off, x->ts, &b, NULL);

				nacks[v]++;
				/* the DATA asked for was the segment's latest: it is lost now */
				if (v == CG_ST_NACK_LOST)
					CHECK(b > 0);
			} else if (x->ack) {
				struct cg_st_ackres res;

				cg_st_tx_ack(&t, &x->a, &res);
				if (res.progress) {
					cg_st_buf_release(&sb, &p, t.una < t.tail ? t.una : t.tail);
					rto_at = cg_st_tx_busy(&t) ? now + 20000 : 0;
				}
				cg_st_tx_rack(&t, nack ? 40000 : 1000, NULL);
			} else {
				int v = cg_st_rx_judge(&r, x->off, x->len, x->fin, consumed + cap);

				if (v == CG_ST_RX_NEW) {
					uint8_t tmp[E2E_MSS];

					CHECK_EQ(cg_st_buf_read(&sb, &p, x->off, tmp, x->len), 0); /* still held */
					if (cg_st_buf_write(&rb, &p, x->off, tmp, x->len) == 0)
						cg_st_rx_add(&r, x->off, x->len, x->fin);
				}
				/* an ACK back, lost 10 % too */
				if (rnd(&seed) % 100 >= 10 && nq < 4000) {
					struct pkt *y = &q[nq++];

					memset(y, 0, sizeof(*y));
					y->ack = 1;
					y->at = now + 5000 + rnd(&seed) % 3000;
					cg_st_rx_ack(&r, consumed + cap, ++aseq, &y->a);
				}
				x = &q[i]; /* q may have moved: no, fixed array */
			}
			q[i] = q[--nq];
		}
		/* the receiver's socket takes everything in order */
		{
			uint64_t k = cg_st_rx_ready(&r, consumed);

			if (k) {
				CHECK_EQ(cg_st_buf_read(&rb, &p, consumed, dst + consumed, (uint32_t)k), 0);
				consumed += k;
				cg_st_buf_release(&rb, &p, consumed);
			}
		}
		if (rto_at && now >= rto_at) {
			cg_st_tx_rto(&t, now, 20000, NULL);
			rto_at = now + 20000;
		}
	}
	CHECK(cg_st_tx_done(&t));
	CHECK(cg_st_rx_ended(&r));
	CHECK_EQ(consumed, E2E_BYTES);
	CHECK(!memcmp(src, dst, E2E_BYTES));
	CHECK(t.retx_segs > 0);
	CHECK(sent > E2E_BYTES / E2E_MSS);
	CHECK_EQ(t.retx_segs, t.retx_by[CG_ST_BY_RACK] + t.retx_by[CG_ST_BY_RTO] + t.retx_by[CG_ST_BY_NACK]);
	if (nack) {
		/* most losses went by NACK, and a NACK never marked a segment
		 * whose newer copy was in flight */
		CHECK(t.retx_by[CG_ST_BY_NACK] > t.retx_by[CG_ST_BY_RACK] + t.retx_by[CG_ST_BY_RTO]);
		CHECK(nacks[CG_ST_NACK_LOST] > 0);
		CHECK_EQ(nacks[CG_ST_NACK_UNKNOWN], 0);
	} else {
		CHECK_EQ(t.retx_by[CG_ST_BY_NACK], 0);
	}
	cg_st_buf_free(&sb, &p);
	cg_st_buf_free(&rb, &p);
	CHECK_EQ(cg_st_pool_used(&p), 0);
}

static void test_e2e(void)
{
	e2e_run(0);
	e2e_run(1);
}

void test_stream(void)
{
	test_renege();
	test_copy();
	test_pool_buf();
	test_unwrap();
	test_rx();
	test_tx();
	test_nack();
	test_suspect();
	test_e2e();
}
