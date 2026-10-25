/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "tcpfree.h"
#include "test.h"

static struct cg_tcpf T;

/* An IPv4 packet into b: TCP from port sport to 443 (or UDP), sequence seq,
 * flags fl, plen bytes of data; returns its length. */
static size_t pkt(uint8_t *b, int udp, uint16_t sport, uint32_t seq, uint8_t fl, size_t plen)
{
	size_t l4 = udp ? 8 : 20, len = 20 + l4 + plen;

	memset(b, 0, len);
	b[0] = 0x45;
	b[2] = (uint8_t)(len >> 8);
	b[3] = (uint8_t)len;
	b[8] = 64;
	b[9] = udp ? 17 : 6;
	b[12] = 10;
	b[15] = 2;
	b[16] = 1;
	b[19] = 1;
	b[20] = (uint8_t)(sport >> 8);
	b[21] = (uint8_t)sport;
	b[22] = 443 >> 8;
	b[23] = 443 & 0xff;
	if (!udp) {
		b[24] = (uint8_t)(seq >> 24);
		b[25] = (uint8_t)(seq >> 16);
		b[26] = (uint8_t)(seq >> 8);
		b[27] = (uint8_t)seq;
		b[32] = 5 << 4;
		b[33] = (uint8_t)(fl | 0x10); /* ACK always */
	} else {
		b[24] = (uint8_t)((8 + plen) >> 8);
		b[25] = (uint8_t)(8 + plen);
	}
	return len;
}

static int is_free(int udp, uint16_t sport, uint32_t seq, uint8_t fl, size_t plen)
{
	uint8_t b[2048];
	size_t len = pkt(b, udp, sport, seq, fl, plen);
	struct cg_flow f;

	CHECK(cg_flow_of(b, len, 7, &f) == 4);
	return cg_tcpf_free(&T, &f, b, len);
}


static int klass(uint16_t sport, uint32_t seq, uint8_t fl, size_t plen)
{
	uint8_t b[2048];
	size_t len = pkt(b, 0, sport, seq, fl, plen);
	struct cg_flow f;

	CHECK(cg_flow_of(b, len, 7, &f) == 4);
	return cg_tcpf_class(&T, &f, b, len);
}

/* The reply direction of pkt's flow: from port 443 back to sport, ack,
 * flags fl (ACK always), and a SACK block [sl, sr) when sr; its length. */
static size_t rpkt(uint8_t *b, uint16_t sport, uint32_t ack, uint8_t fl, uint32_t sl, uint32_t sr)
{
	size_t opt = sr ? 12 : 0, len = 40 + opt;

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
	b[28] = (uint8_t)(ack >> 24);
	b[29] = (uint8_t)(ack >> 16);
	b[30] = (uint8_t)(ack >> 8);
	b[31] = (uint8_t)ack;
	b[32] = (uint8_t)((20 + opt) / 4 << 4);
	b[33] = (uint8_t)(fl | 0x10);
	if (sr) {
		uint8_t *o = b + 40;

		o[0] = 1;
		o[1] = 1;
		o[2] = 5;
		o[3] = 10;
		for (int i = 0; i < 4; i++) {
			o[4 + i] = (uint8_t)(sl >> (24 - 8 * i));
			o[8 + i] = (uint8_t)(sr >> (24 - 8 * i));
		}
	}
	return len;
}

static struct cg_tcpe E;
static uint32_t ETS; /* the tunnel sequence of each segment written */

static void e_out(uint16_t sport, uint32_t seq, uint8_t fl, size_t plen, uint32_t t)
{
	uint8_t b[2048];
	size_t len = pkt(b, 0, sport, seq, fl, plen);
	struct cg_flow f;

	if (!(fl & 0x10)) /* pkt sets ACK: a bare SYN clears it */
		b[33] &= (uint8_t)~0x10;
	CHECK(cg_flow_of(b, len, 7, &f) == 4);
	cg_tcpe_out(&E, &f, b, len, t, ++ETS, -1);
}

static void e_in(uint16_t sport, uint32_t ack, uint8_t fl, uint32_t sl, uint32_t sr, uint32_t t)
{
	uint8_t b[128];
	size_t len = rpkt(b, sport, ack, fl, sl, sr);
	struct cg_flow f;

	CHECK(cg_flow_of_dir(b, len, 7, 1, &f) == 4);
	cg_tcpe_in(&E, &f, b, len, t, NULL);
}

/* Story 023: the classes of a retransmission (first, again) and where TCP
 * leaves the tunnel (the far leg, the SACK holes beyond it). */
static void test_tcp_story23(void)
{
	uint8_t a[128], b[128];
	struct cg_flow f, r;

	memset(&T, 0, sizeof(T));
	CHECK_EQ(klass(1000, 100, CG_TCP_SYN, 0), CG_TCPF_CTL);
	CHECK_EQ(klass(1000, 101, 0, 1000), CG_TCPF_NO);
	CHECK_EQ(klass(1000, 1101, 0, 1000), CG_TCPF_NO);
	CHECK_EQ(klass(1000, 2101, 0, 1000), CG_TCPF_NO);
	CHECK_EQ(klass(1000, 101, 0, 1000), CG_TCPF_RETX1);  /* first time */
	CHECK_EQ(klass(1000, 1101, 0, 1000), CG_TCPF_RETX1); /* the next one, first time too */
	CHECK_EQ(klass(1000, 101, 0, 1000), CG_TCPF_RETXN);  /* again: the one copy did not do */
	CHECK_EQ(klass(1000, 3101, 0, 1000), CG_TCPF_NO);
	CHECK_EQ(klass(1000, 2101, 0, 1000), CG_TCPF_RETX1);
	CHECK_EQ(klass(1000, 3101, 0, 0), CG_TCPF_CTL);
	CHECK_EQ(T.retx, 4);
	CHECK_EQ(T.retx_again, 1);
	CHECK_EQ(klass(1000, 9000, CG_TCP_SYN, 0), CG_TCPF_CTL); /* a new flow forgets */
	CHECK_EQ(klass(1000, 9001, 0, 500), CG_TCPF_NO);
	CHECK_EQ(klass(1000, 9001, 0, 500), CG_TCPF_RETX1);

	/* the reply's reversed hash is the data's */
	CHECK(cg_flow_of(a, pkt(a, 0, 1234, 1, 0, 10), 99, &f) == 4);
	CHECK(cg_flow_of_dir(b, rpkt(b, 1234, 1, 0, 0, 0), 99, 1, &r) == 4);
	CHECK_EQ(f.hash, r.hash);
	CHECK(cg_flow_of(b, rpkt(b, 1234, 1, 0, 0, 0), 99, &r) == 4);
	CHECK(f.hash != r.hash);

	/* the far leg: a SYN out at 1 ms, its SYN-ACK in at 31 ms */
	memset(&E, 0, sizeof(E));
	CHECK_EQ(cg_tcpe_rtt(&E, 20000, 0, 10000000), 0); /* no TCP yet */
	e_out(5000, 1000, CG_TCP_SYN, 0, 1000);
	e_in(5000, 1001, CG_TCP_SYN, 0, 0, 31000);
	CHECK_EQ(E.legs, 1);
	CHECK(E.leg_us >= 29999 && E.leg_us <= 30001);
	/* data out (the tunnel delivers 1001..5001, then skips 5001..6001) */
	for (uint32_t s = 1001; s < 5001; s += 1000)
		e_out(5000, s, 0, 1000, 40000);
	CHECK_EQ(cg_tcpe_rtt(&E, 20000, 40000, 10000000), E.leg_us + 20000);
	CHECK_EQ(cg_tcpe_rtt(&E, 20000, 40000 + 20000000, 10000000), 0); /* idle: no TCP */
	e_out(5000, 6001, 0, 1000, 41000); /* a hole of the tunnel's: 5001..6001 */
	e_out(5000, 7001, 0, 1000, 41000);
	/* the far host misses 2001..3001, which the tunnel delivered: beyond */
	e_in(5000, 2001, 0, 3001, 8001, 60000);
	CHECK_EQ(E.holes, 1);
	CHECK_EQ(E.beyond, 1);
	e_in(5000, 2001, 0, 3001, 8001, 61000); /* the same ACK: counted once */
	CHECK_EQ(E.holes, 1);
	/* it misses 5001..6001: the tunnel's own hole, not beyond */
	e_in(5000, 5001, 0, 6001, 8001, 62000);
	CHECK_EQ(E.holes, 2);
	CHECK_EQ(E.own, 1);
	CHECK_EQ(E.beyond, 1);
	/* a hole above what the tunnel delivered: not counted beyond */
	e_in(5000, 8001, 0, 9001, 9501, 63000);
	CHECK_EQ(E.beyond, 1);
	/* no SACK: nothing */
	e_in(5000, 9501, 0, 0, 0, 64000);
	CHECK_EQ(E.holes, 3);
	/* an ACK of an unknown flow: nothing */
	e_in(6000, 2001, 0, 3001, 8001, 65000);
	CHECK_EQ(E.holes, 3);
}

void test_tcpfree(void)
{
	memset(&T, 0, sizeof(T));
	/* SYN: control, marked; it starts the flow at seq + 1 */
	CHECK(is_free(0, 1000, 100, CG_TCP_SYN, 0));
	/* new data: not marked, each further segment neither */
	CHECK(!is_free(0, 1000, 101, 0, 1000));
	CHECK(!is_free(0, 1000, 1101, 0, 1000));
	CHECK(!is_free(0, 1000, 2101, 0, 1000));
	/* the second segment again: TCP's retransmission, marked */
	CHECK(is_free(0, 1000, 1101, 0, 1000));
	/* a retransmission of part of what was sent, too */
	CHECK(is_free(0, 1000, 2101, 0, 500));
	/* and the stream goes on unmarked */
	CHECK(!is_free(0, 1000, 3101, 0, 1000));
	CHECK_EQ(T.retx, 2);
	/* a pure ACK: marked */
	CHECK(is_free(0, 1000, 4101, 0, 0));
	CHECK_EQ(T.ctl, 2);
	/* another flow is its own */
	CHECK(!is_free(0, 2000, 4101, 0, 1000));
	CHECK(!is_free(0, 2000, 5101, 0, 1000));
	CHECK(is_free(0, 2000, 4101, 0, 1000));
	/* UDP: never */
	CHECK(!is_free(1, 1000, 0, 0, 1000));
	CHECK(!is_free(1, 1000, 0, 0, 1000));
	/* the sequence wrapping: still new, and its retransmission caught */
	CHECK(!is_free(0, 3000, 0xfffffc00u, 0, 512));
	CHECK(!is_free(0, 3000, 0xfffffe00u, 0, 1024)); /* crosses 0 */
	CHECK(!is_free(0, 3000, 0x00000200u, 0, 1024));
	CHECK(is_free(0, 3000, 0xfffffe00u, 0, 1024));
	/* FIN with data: new, and its end counts the FIN */
	CHECK(!is_free(0, 4000, 10, 0, 100));
	CHECK(!is_free(0, 4000, 110, CG_TCP_FIN, 100));
	CHECK(is_free(0, 4000, 110, CG_TCP_FIN, 100));
	/* RST: marked, the flow forgotten */
	CHECK(is_free(0, 4000, 211, CG_TCP_RST, 0));
	CHECK(!is_free(0, 4000, 0, 0, 100));
	/* a fragment or a short header: never */
	{
		uint8_t b[2048];
		size_t len = pkt(b, 0, 5000, 1, 0, 100);
		struct cg_flow f;

		b[6] = 0x20; /* more fragments */
		CHECK(cg_flow_of(b, len, 7, &f) == 4 && f.frag);
		CHECK(!cg_tcpf_free(&T, &f, b, len));
		b[6] = 0;
		CHECK(cg_flow_of(b, len, 7, &f) == 4);
		b[32] = 4 << 4; /* data offset under 20 */
		CHECK(!cg_tcpf_free(&T, &f, b, len));
	}

	/* The receiver's side: TCP data (a retransmission when marked free)
	 * or not (an ACK, SYN, RST, anything else). */
	{
		uint8_t b[2048];
		size_t len;

		CHECK(cg_tcp_data(b, pkt(b, 0, 1000, 1, 0, 1000)));
		CHECK(cg_tcp_data(b, pkt(b, 0, 1000, 1, CG_TCP_FIN, 1)));
		CHECK(!cg_tcp_data(b, pkt(b, 0, 1000, 1, 0, 0)));          /* pure ACK */
		CHECK(!cg_tcp_data(b, pkt(b, 0, 1000, 1, CG_TCP_FIN, 0))); /* FIN alone */
		CHECK(!cg_tcp_data(b, pkt(b, 0, 1000, 1, CG_TCP_SYN, 100))); /* SYN with data (TFO) */
		CHECK(!cg_tcp_data(b, pkt(b, 0, 1000, 1, CG_TCP_RST, 10)));
		CHECK(!cg_tcp_data(b, pkt(b, 1, 1000, 0, 0, 1000))); /* UDP */
		len = pkt(b, 0, 1000, 1, 0, 100);
		b[6] = 0x20; /* a fragment */
		CHECK(!cg_tcp_data(b, len));
		len = pkt(b, 0, 1000, 1, 0, 100);
		b[32] = 15 << 4; /* a 60-byte header: 60 bytes of data left */
		CHECK(cg_tcp_data(b, len));
		len = pkt(b, 0, 1000, 1, 0, 40);
		b[32] = 15 << 4; /* 60-byte header: all of it, no data */
		CHECK(!cg_tcp_data(b, len));
		len = pkt(b, 0, 1000, 1, 0, 20);
		b[32] = 15 << 4; /* past the end */
		CHECK(!cg_tcp_data(b, len));
		len = pkt(b, 0, 1000, 1, 0, 100);
		b[32] = 4 << 4; /* under 20 */
		CHECK(!cg_tcp_data(b, len));
		CHECK(!cg_tcp_data(b, 10));
		len = pkt(b, 0, 1000, 1, 0, 100);
		CHECK(!cg_tcp_data(b, len - 1)); /* the IP length says otherwise */

		/* IPv6, TCP with 100 bytes of data, then without */
		memset(b, 0, 40 + 20 + 100);
		b[0] = 0x60;
		b[5] = 120;
		b[6] = 6;
		b[7] = 64;
		b[40 + 12] = 5 << 4;
		b[40 + 13] = 0x10;
		CHECK(cg_tcp_data(b, 160));
		b[5] = 20;
		CHECK(!cg_tcp_data(b, 60));
	}
	test_tcp_story23();
}
