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
}
