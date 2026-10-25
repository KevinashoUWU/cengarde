/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "tcpack.h"
#include "tcpfree.h"
#include "test.h"

/* An IPv4 TCP segment into b, 10.0.0.2:sport -> 1.0.0.1:443 (the data
 * direction), sequence seq, flags fl (ACK always), plen bytes of data. */
static size_t dpkt(uint8_t *b, uint16_t sport, uint32_t seq, uint8_t fl, size_t plen)
{
	size_t len = 40 + plen;

	memset(b, 0, len);
	b[0] = 0x45;
	b[2] = (uint8_t)(len >> 8);
	b[3] = (uint8_t)len;
	b[8] = 64;
	b[9] = 6;
	b[12] = 10;
	b[15] = 2;
	b[16] = 1;
	b[19] = 1;
	b[20] = (uint8_t)(sport >> 8);
	b[21] = (uint8_t)sport;
	b[22] = 443 >> 8;
	b[23] = 443 & 0xff;
	for (int i = 0; i < 4; i++)
		b[24 + i] = (uint8_t)(seq >> (24 - 8 * i));
	b[32] = 5 << 4;
	b[33] = (uint8_t)(fl | 0x10);
	return len;
}

/* Its reply: 1.0.0.1:443 -> 10.0.0.2:sport, cumulative ack, flags fl (ACK
 * always), n SACK blocks [l[i], r[i]) (up to 4, in the order given). */
static size_t apkt(uint8_t *b, uint16_t sport, uint32_t ack, uint8_t fl, int n, const uint32_t *l, const uint32_t *r)
{
	size_t opt = n ? (size_t)(4 + 8 * n) : 0, len;

	opt = (opt + 3) & ~(size_t)3;
	len = 40 + opt;
	memset(b, 0, len);
	b[0] = 0x45;
	b[2] = (uint8_t)(len >> 8);
	b[3] = (uint8_t)len;
	b[8] = 64;
	b[9] = 6;
	b[12] = 1;
	b[15] = 1;
	b[16] = 10;
	b[19] = 2;
	b[20] = 443 >> 8;
	b[21] = 443 & 0xff;
	b[22] = (uint8_t)(sport >> 8);
	b[23] = (uint8_t)sport;
	for (int i = 0; i < 4; i++)
		b[28 + i] = (uint8_t)(ack >> (24 - 8 * i));
	b[32] = (uint8_t)((20 + opt) / 4 << 4);
	b[33] = (uint8_t)(fl | 0x10);
	if (n) {
		uint8_t *o = b + 40;

		o[0] = 1;
		o[1] = 1;
		o[2] = 5;
		o[3] = (uint8_t)(2 + 8 * n);
		for (int k = 0; k < n; k++)
			for (int i = 0; i < 4; i++) {
				o[4 + 8 * k + i] = (uint8_t)(l[k] >> (24 - 8 * i));
				o[8 + 8 * k + i] = (uint8_t)(r[k] >> (24 - 8 * i));
			}
	}
	return len;
}

static struct cg_tcpe E;
static struct cg_tcpa A;
static uint32_t TS;                      /* the tunnel sequence of the next segment written */
static int PREV = -1;                    /* and its header's CG_F_PREV link */
static const struct cg_tcpe_view *VIEW; /* the receiver's links, NULL: as if all had passed */

/* The exit end writes a data segment out. */
static void out(uint16_t sport, uint32_t seq, uint8_t fl, size_t plen)
{
	uint8_t b[2048];
	size_t len = dpkt(b, sport, seq, fl, plen);
	struct cg_flow f;

	if (fl & CG_TCP_SYN)
		b[33] &= (uint8_t)~0x10; /* a bare SYN */
	CHECK(cg_flow_of(b, len, 7, &f) == 4);
	cg_tcpe_out(&E, &f, b, len, 1000, TS++, PREV);
}

/* The exit end reads an ACK in: its verdict. */
static int in(uint16_t sport, uint32_t ack, int n, const uint32_t *l, const uint32_t *r)
{
	uint8_t b[128];
	size_t len = apkt(b, sport, ack, 0, n, l, r);
	struct cg_flow f;

	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	return cg_tcpe_in(&E, &f, b, len, 2000, VIEW);
}

static int in1(uint16_t sport, uint32_t ack, uint32_t l, uint32_t r)
{
	return in(sport, ack, 1, &l, &r);
}

/* The sending end reads an ACK that came out of the tunnel. */
static void ack(uint16_t sport, uint32_t a, uint8_t fl, int n, const uint32_t *l, const uint32_t *r, int mark,
		int marks)
{
	uint8_t b[128];
	size_t len = apkt(b, sport, a, fl, n, l, r);
	struct cg_flow f;

	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	cg_tcpa_ack(&A, &f, b, len, mark, marks);
}

static void ack1(uint16_t sport, uint32_t a, uint32_t l, uint32_t r, int mark, int marks)
{
	ack(sport, a, 0, r ? 1 : 0, &l, &r, mark, marks);
}

/* The row of a retransmission [seq, seq + plen). */
static int row(uint16_t sport, uint32_t seq, size_t plen)
{
	uint8_t b[2048];
	size_t len = dpkt(b, sport, seq, 0, plen);
	struct cg_flow f;

	CHECK(cg_flow_of(b, len, 7, &f) == 4);
	return cg_tcpa_retx(&A, &f, b, len);
}

/* Where TCP leaves the tunnel: its holes trimmed, split and forgotten,
 * and its verdict on each ACK. */
static void test_exit(void)
{
	uint32_t l[4], r[4];

	memset(&E, 0, sizeof(E));
	out(1000, 1000, CG_TCP_SYN, 0);
	for (uint32_t s = 1001; s < 5001; s += 1000)
		out(1000, s, 0, 1000);
	out(1000, 6001, 0, 1000); /* skipped 5001..6001: the tunnel's */
	out(1000, 7001, 0, 1000);
	/* a hole the tunnel delivered: through */
	CHECK_EQ(in1(1000, 2001, 3001, 8001), CG_TCPE_THROUGH);
	CHECK_EQ(E.beyond, 1);
	CHECK_EQ(E.marked, 0);
	/* the tunnel's own */
	CHECK_EQ(in1(1000, 5001, 6001, 8001), CG_TCPE_OWN);
	CHECK_EQ(E.own, 1);
	CHECK_EQ(E.marked, 1);
	/* the same ACK again: marked again, counted once */
	CHECK_EQ(in1(1000, 5001, 6001, 8001), CG_TCPE_OWN);
	CHECK_EQ(E.holes, 2);
	CHECK_EQ(E.marked, 2);
	/* no SACK block: no verdict */
	CHECK_EQ(in(1000, 5001, 0, NULL, NULL), CG_TCPE_NONE);
	/* a late copy fills half of it: the other half is still the tunnel's */
	out(1000, 5001, 0, 500);
	CHECK_EQ(in1(1000, 5501, 6001, 8001), CG_TCPE_OWN);
	CHECK_EQ(in1(1000, 5001, 5501, 8001), CG_TCPE_THROUGH); /* the late half was delivered */
	/* the retransmission fills the rest: delivered, what is missing was lost beyond */
	out(1000, 5501, 0, 500);
	CHECK_EQ(in1(1000, 5001, 5501, 8001), CG_TCPE_THROUGH);
	CHECK_EQ(in1(1000, 4001, 5001, 8001), CG_TCPE_THROUGH);

	/* a segment in the middle of a hole splits it */
	out(1000, 8001, 0, 1000);
	out(1000, 12001, 0, 1000); /* the tunnel's: 9001..12001 */
	out(1000, 10001, 0, 1000); /* a late one: 9001..10001 and 11001..12001 left */
	l[0] = 12001, r[0] = 13001, l[1] = 10001, r[1] = 11001; /* out of order on purpose */
	CHECK_EQ(in(1000, 9001, 2, l, r), CG_TCPE_OWN);
	out(1000, 9001, 0, 1000);
	CHECK_EQ(in(1000, 9001, 2, l, r), CG_TCPE_OWN); /* the far host still misses 9001, but 11001..12001 is the tunnel's */
	CHECK_EQ(in1(1000, 11001, 12001, 13001), CG_TCPE_OWN);
	out(1000, 11001, 0, 1000);
	CHECK_EQ(in1(1000, 11001, 12001, 13001), CG_TCPE_THROUGH);
	CHECK_EQ(in(1000, 9001, 2, l, r), CG_TCPE_THROUGH);
	/* a hole between two SACK blocks, the tunnel's */
	out(1000, 14001, 0, 1000); /* 13001..14001 skipped */
	out(1000, 15001, 0, 1000);
	l[0] = 12001, r[0] = 13001, l[1] = 14001, r[1] = 16001;
	CHECK_EQ(in(1000, 11001, 2, l, r), CG_TCPE_OWN);
	/* SACKed above what the tunnel wrote: it cannot vouch, and does not mark */
	{
		uint64_t mk = E.marked;

		CHECK_EQ(in1(1000, 16001, 17001, 18001), CG_TCPE_UNVOUCHED);
		CHECK(E.unvouched >= 1);
		CHECK_EQ(E.marked, mk);
	}
	/* a D-SACK (below the cumulative ACK) alone: no verdict */
	CHECK_EQ(in1(1000, 16001, 14001, 15001), CG_TCPE_NONE);

	/* a flow first seen mid-stream: nothing below its first segment is vouched for */
	out(2000, 50000, 0, 1000);
	out(2000, 51000, 0, 1000);
	CHECK_EQ(in1(2000, 40000, 50000, 52000), CG_TCPE_UNVOUCHED);
	CHECK_EQ(in1(2000, 50000, 51000, 52000), CG_TCPE_THROUGH);
	/* a flow it does not track: not vouched for, not counted */
	{
		uint64_t h = E.holes;

		CHECK_EQ(in1(3000, 100, 200, 300), CG_TCPE_UNVOUCHED);
		CHECK_EQ(E.holes, h);
	}

	/* more holes than it keeps: past them it only knows the range they
	 * lie in, never judged lost nor delivered */
	memset(&E, 0, sizeof(E));
	out(4000, 0, CG_TCP_SYN, 0);
	out(4000, 1, 0, 100);
	for (uint32_t k = 0; k < CG_TCPE_HOLES + 1; k++)
		out(4000, 1001 + 1000 * k, 0, 100); /* holes [101 + 1000k, 1001 + 1000k) */
	CHECK_EQ(in1(4000, 101, 1001, 1101), CG_TCPE_OWN);        /* the first, kept */
	CHECK_EQ(in1(4000, 7101, 8001, 8101), CG_TCPE_OWN);       /* the eighth, kept */
	CHECK_EQ(in1(4000, 8101, 9001, 9101), CG_TCPE_UNVOUCHED); /* the ninth: no slot */
	CHECK_EQ(in1(4000, 5001, 5101, 9101), CG_TCPE_THROUGH);   /* written between two */
	CHECK_EQ(in1(4000, 1001, 1101, 1201), CG_TCPE_THROUGH);
	{ /* a far host's holes in a kept one and in the unknown range: not known */
		uint32_t l2[2] = { 1001, 9001 }, r2[2] = { 8101, 9101 };

		CHECK_EQ(in(4000, 101, 2, l2, r2), CG_TCPE_OWN); /* a lost one first */
		l2[0] = 101, r2[0] = 8101;
		CHECK_EQ(in(4000, 1, 2, l2, r2), CG_TCPE_UNVOUCHED); /* 1..101 written; 8101..9001 not known */
	}
	/* holes below the cumulative ACK free their slots */
	CHECK_EQ(in(4000, 7101, 0, NULL, NULL), CG_TCPE_NONE);
	out(4000, 11001, 0, 100);
	out(4000, 12001, 0, 100);
	CHECK_EQ(in1(4000, 11101, 12001, 12101), CG_TCPE_OWN);
	CHECK_EQ(in1(4000, 9101, 11001, 11101), CG_TCPE_OWN);
	CHECK_EQ(in1(4000, 7101, 8001, 8101), CG_TCPE_OWN);
	CHECK_EQ(in1(4000, 8001, 8101, 9101), CG_TCPE_THROUGH); /* the unknown range SACKed */
	/* the cumulative ACK went past it (11101 above): known again */
	CHECK_EQ(in(4000, 12101, 0, NULL, NULL), CG_TCPE_NONE);
	out(4000, 14001, 0, 100); /* 12101..14001 */
	CHECK_EQ(in1(4000, 12101, 14001, 14101), CG_TCPE_OWN);
}

/* The sending end: rows from the ACKs and the marks. */
static void test_rows(void)
{
	uint32_t l[4], r[4];

	memset(&A, 0, sizeof(A));
	/* nothing known yet */
	CHECK_EQ(row(1000, 1001, 1000), CG_TCPA_UNKNOWN);
	ack(1000, 1001, CG_TCP_SYN, 0, NULL, NULL, 0, 1); /* the SYN-ACK */
	CHECK_EQ(row(1000, 1001, 1000), CG_TCPA_UNKNOWN);
	/* ACKed already: spurious */
	ack1(1000, 5001, 0, 0, 0, 1);
	CHECK_EQ(row(1000, 1001, 1000), CG_TCPA_SPURIOUS);
	CHECK_EQ(row(1000, 4001, 1000), CG_TCPA_SPURIOUS);
	CHECK_EQ(row(1000, 5001, 1000), CG_TCPA_UNKNOWN); /* no ACK yet is not lost */
	/* a SACK hole the exit end marked: the tunnel's */
	ack1(1000, 5001, 6001, 8001, 1, 1);
	CHECK_EQ(A.marked, 1);
	CHECK_EQ(row(1000, 5001, 1000), CG_TCPA_TUNNEL);
	CHECK_EQ(row(1000, 4501, 1000), CG_TCPA_TUNNEL);   /* partly ACKed: the rest */
	CHECK_EQ(row(1000, 6001, 1000), CG_TCPA_SPURIOUS); /* SACKed */
	CHECK_EQ(row(1000, 8001, 1000), CG_TCPA_UNKNOWN);  /* past the scoreboard */
	/* not marked by a peer that marks: delivered, lost beyond */
	ack1(1000, 5001, 6001, 8001, 0, 1);
	CHECK_EQ(row(1000, 5001, 1000), CG_TCPA_DELIVERED);
	/* a peer that does not say it marks: not known */
	ack1(1000, 5001, 6001, 8001, 0, 0);
	CHECK_EQ(row(1000, 5001, 1000), CG_TCPA_UNKNOWN);
	CHECK_EQ(row(1000, 6001, 1000), CG_TCPA_SPURIOUS); /* the SACK still counts */
	/* several blocks, a hole between them */
	l[0] = 9001, r[0] = 10001, l[1] = 6001, r[1] = 8001;
	ack(1000, 5001, 0, 2, l, r, 1, 1);
	CHECK_EQ(row(1000, 8001, 1000), CG_TCPA_TUNNEL);
	CHECK_EQ(row(1000, 9001, 1000), CG_TCPA_SPURIOUS);
	CHECK_EQ(row(1000, 10001, 1000), CG_TCPA_UNKNOWN);
	/* touching blocks merge: a segment across them is SACKed */
	ack1(1000, 5001, 8001, 9001, 1, 1);
	CHECK_EQ(row(1000, 7001, 2000), CG_TCPA_SPURIOUS); /* 7001..9001, across 8001 */
	/* the cumulative ACK past the scoreboard: holes filled, blocks gone */
	ack1(1000, 10001, 0, 0, 0, 1);
	CHECK_EQ(row(1000, 10001, 1000), CG_TCPA_UNKNOWN);
	CHECK_EQ(row(1000, 9001, 1000), CG_TCPA_SPURIOUS);
	/* an older ACK (reordered) changes nothing below the newest */
	ack1(1000, 6001, 0, 0, 0, 1);
	CHECK_EQ(row(1000, 9001, 1000), CG_TCPA_SPURIOUS);
	/* a D-SACK block is not a block above the ACK */
	ack1(1000, 10001, 3001, 4001, 1, 1);
	CHECK_EQ(row(1000, 10001, 1000), CG_TCPA_UNKNOWN);
	/* a new connection on the same tuple forgets */
	ack(1000, 77, CG_TCP_SYN, 0, NULL, NULL, 0, 1);
	CHECK_EQ(row(1000, 9001, 1000), CG_TCPA_UNKNOWN);
	/* another flow is its own */
	CHECK_EQ(row(2000, 9001, 1000), CG_TCPA_UNKNOWN);
	CHECK_EQ(A.acks, 11);
	CHECK_EQ(A.sacked, 5);

	/* the rule */
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_SPURIOUS, CG_TCPF_RETX1), 0);
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_SPURIOUS, CG_TCPF_RETXN), 0);
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_DELIVERED, CG_TCPF_RETX1), 0);
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_DELIVERED, CG_TCPF_RETXN), 1); /* "on its way" cannot be told apart */
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_TUNNEL, CG_TCPF_RETX1), 1);
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_UNKNOWN, CG_TCPF_RETX1), 0);
	CHECK_EQ(cg_tcpa_copy(CG_TCPA_UNKNOWN, CG_TCPF_RETXN), 1);
}

/* End to end: the exit end's verdict travels as the mark, the sending
 * end's row follows it. */
static void test_e2e(void)
{
	uint8_t b[128];
	size_t len;
	struct cg_flow f;
	int v;
	uint32_t l = 6001, r = 7001;

	memset(&E, 0, sizeof(E));
	memset(&A, 0, sizeof(A));
	out(5000, 1000, CG_TCP_SYN, 0);
	for (uint32_t q = 1001; q < 5001; q += 1000)
		out(5000, q, 0, 1000);
	out(5000, 6001, 0, 1000); /* 5001..6001 lost on every link */
	len = apkt(b, 5000, 5001, 0, 1, &l, &r);
	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	v = cg_tcpe_in(&E, &f, b, len, 0, NULL);
	CHECK_EQ(v, CG_TCPE_OWN);
	cg_tcpa_ack(&A, &f, b, len, v >= CG_TCPE_OWN, 1);
	CHECK_EQ(row(5000, 5001, 1000), CG_TCPA_TUNNEL);
	/* the retransmission goes out: the far host's next ACK has no hole */
	out(5000, 5001, 0, 1000);
	len = apkt(b, 5000, 7001, 0, 0, NULL, NULL);
	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	v = cg_tcpe_in(&E, &f, b, len, 0, NULL);
	CHECK_EQ(v, CG_TCPE_NONE);
	cg_tcpa_ack(&A, &f, b, len, 0, 1);
	CHECK_EQ(row(5000, 5001, 1000), CG_TCPA_SPURIOUS);
	/* beyond: the tunnel wrote 7001..8001, the far host lost it */
	out(5000, 7001, 0, 1000);
	out(5000, 8001, 0, 1000);
	l = 8001, r = 9001;
	len = apkt(b, 5000, 7001, 0, 1, &l, &r);
	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	v = cg_tcpe_in(&E, &f, b, len, 0, NULL);
	CHECK_EQ(v, CG_TCPE_THROUGH);
	cg_tcpa_ack(&A, &f, b, len, v >= CG_TCPE_OWN, 1);
	CHECK_EQ(row(5000, 7001, 1000), CG_TCPA_DELIVERED);
}

/* A hole of the tunnel's is lost only once every link that may bring it
 * went past it: the one link the hint names, or every link waited for. */
static void test_inflight(void)
{
	struct cg_tcpe_view v = { .rset = 3, .hw_ok = 3 };

	memset(&E, 0, sizeof(E));
	TS = 1;
	PREV = -1;
	out(7000, 1000, CG_TCP_SYN, 0); /* tunnel sequence 1 */
	for (uint32_t q = 1001; q < 5001; q += 1000)
		out(7000, q, 0, 1000); /* 2..5 */
	TS++;                           /* 6, with 5001..6001, did not come */
	PREV = 0;                       /* 7 says 6 went on link 0 alone */
	out(7000, 6001, 0, 1000);
	PREV = -1;
	VIEW = &v;
	/* link 0 has brought nothing past 5: on its way (maybe stalled), not marked */
	v.hw[0] = 5, v.hw[1] = 7;
	CHECK_EQ(in1(7000, 5001, 6001, 7001), CG_TCPE_INFLIGHT);
	CHECK_EQ(E.inflight, 1);
	CHECK_EQ(E.marked, 0);
	/* link 0 brought 8: lost on the one link it went on */
	v.hw[0] = 8;
	CHECK_EQ(in1(7000, 5001, 6001, 7001), CG_TCPE_OWN);
	CHECK_EQ(E.marked, 1);
	/* link 0 not heard from at all: on its way */
	v.hw_ok = 2;
	CHECK_EQ(in1(7000, 5001, 6001, 7001), CG_TCPE_INFLIGHT);
	v.hw_ok = 3;
	/* no hint (two tunnel sequences missing): every link waited for */
	out(7000, 7001, 0, 1000); /* 8 */
	TS += 2;                  /* 9 and 10 missing */
	PREV = 1;                 /* a hint about 10 only: not used */
	out(7000, 9001, 0, 1000); /* 11, the hole 8001..9001 */
	PREV = -1;
	v.hw[0] = 11, v.hw[1] = 9;
	CHECK_EQ(in1(7000, 8001, 9001, 10001), CG_TCPE_INFLIGHT); /* link 1 still before 10 */
	v.hw[1] = 11;
	CHECK_EQ(in1(7000, 8001, 9001, 10001), CG_TCPE_OWN);
	v.rset = 1; /* link 1 brought nothing lately: link 0 alone decides */
	v.hw[1] = 9;
	CHECK_EQ(in1(7000, 8001, 9001, 10001), CG_TCPE_OWN);
	v.rset = 3;
	/* without a view (no reorderer): lost, as before */
	VIEW = NULL;
	CHECK_EQ(in1(7000, 8001, 9001, 10001), CG_TCPE_OWN);
	/* a split hole keeps its tunnel sequence and hint */
	VIEW = &v;
	TS = 20;
	out(7000, 10001, 0, 1000); /* 20 */
	TS++;                      /* 21 missing: 11001..14001 */
	PREV = 0;
	out(7000, 14001, 0, 1000); /* 22 says 21 went on link 0 */
	PREV = -1;
	out(7000, 12001, 0, 1000); /* a piece of it: 11001..12001 and 13001..14001 left */
	v.hw[0] = 20, v.hw[1] = 30;
	CHECK_EQ(in1(7000, 13001, 14001, 15001), CG_TCPE_INFLIGHT);
	v.hw[0] = 22;
	CHECK_EQ(in1(7000, 13001, 14001, 15001), CG_TCPE_OWN);
	/* a retransmission written in between counts as the flow's newest
	 * tunnel sequence: the hint of the next hole is still used */
	out(7000, 11001, 0, 1000); /* 24: fills 11001..12001 */
	out(7000, 15001, 0, 1000); /* 25 */
	TS++;                      /* 26 missing: 16001..17001 */
	PREV = 1;
	out(7000, 17001, 0, 1000); /* 27 says 26 went on link 1 */
	PREV = -1;
	v.hw[0] = 30, v.hw[1] = 25;
	CHECK_EQ(in1(7000, 16001, 17001, 18001), CG_TCPE_INFLIGHT); /* link 0 does not count */
	v.hw[1] = 27;
	CHECK_EQ(in1(7000, 16001, 17001, 18001), CG_TCPE_OWN);
	VIEW = NULL;
}

void test_tcpack(void)
{
	test_exit();
	test_rows();
	test_e2e();
	test_inflight();
}
