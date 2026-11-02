/* What a flow is, learned from the packets that say it (flowlearn.h,
 * story 031, step 3e).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "flowlearn.h"
#include "test.h"

/* An IPv4 UDP packet from s:sp to d:dp with n payload bytes p into b. */
static size_t udp4(uint8_t *b, const char *s, uint16_t sp, const char *d, uint16_t dp, const uint8_t *p, size_t n)
{
	size_t len = 28 + n;

	memset(b, 0, 28);
	b[0] = 0x45;
	b[2] = (uint8_t)(len >> 8);
	b[3] = (uint8_t)len;
	b[8] = 64;
	b[9] = 17;
	inet_pton(AF_INET, s, b + 12);
	inet_pton(AF_INET, d, b + 16);
	b[20] = (uint8_t)(sp >> 8);
	b[21] = (uint8_t)sp;
	b[22] = (uint8_t)(dp >> 8);
	b[23] = (uint8_t)dp;
	memcpy(b + 28, p, n);
	return len;
}

void test_flowlearn(void)
{
	struct cg_fl *t = calloc(1, sizeof(*t));
	uint8_t b[1600], p[1400];
	size_t len;

	/* QUIC: the client's Initial (long header, version 1) teaches the
	 * flow; the short-header packets after, either way, are known by it */
	memset(p, 0, sizeof(p));
	p[0] = 0xc3;
	p[4] = 1;
	len = udp4(b, "192.168.1.5", 50000, "142.250.0.1", 443, p, 1200);
	CHECK_EQ(cg_fl_observe(t, b, len, 1000), CG_FL_QUIC);
	p[0] = 0x43; /* short header */
	len = udp4(b, "142.250.0.1", 443, "192.168.1.5", 50000, p, 1200);
	CHECK_EQ(cg_fl_observe(t, b, len, 1100), CG_FL_NONE); /* says nothing itself */
	CHECK_EQ(cg_fl_kind(t, b, len, 1100), CG_FL_QUIC);    /* the way back: the same flow */
	len = udp4(b, "192.168.1.5", 50001, "142.250.0.1", 443, p, 1200);
	CHECK_EQ(cg_fl_kind(t, b, len, 1100), CG_FL_NONE); /* another port, another flow */
	/* QUIC version 2 and a version negotiation; a long header of no known version is not QUIC */
	p[0] = 0xc0;
	p[1] = 0x6b, p[2] = 0x33, p[3] = 0x43, p[4] = 0xcf;
	CHECK_EQ(cg_fl_says(p, 1200), CG_FL_QUIC);
	memset(p + 1, 0, 4);
	CHECK_EQ(cg_fl_says(p, 1200), CG_FL_QUIC);
	p[1] = 0x12, p[2] = 0x34, p[3] = 0x56, p[4] = 0x78;
	CHECK_EQ(cg_fl_says(p, 1200), CG_FL_NONE);

	/* remembered while it carries: a packet every minute keeps it, two
	 * minutes of nothing forget it */
	len = udp4(b, "142.250.0.1", 443, "192.168.1.5", 50000, p, 100);
	CHECK_EQ(cg_fl_kind(t, b, len, 61000), CG_FL_QUIC);
	CHECK_EQ(cg_fl_kind(t, b, len, 121000), CG_FL_QUIC);
	CHECK_EQ(cg_fl_kind(t, b, len, 121000 + CG_FL_TTL_MS + 1), CG_FL_NONE);

	/* SRT: a handshake (control type 0, UDT version 5) */
	memset(p, 0, sizeof(p));
	p[0] = 0x80;
	p[19] = 5;
	len = udp4(b, "192.168.1.6", 9000, "203.0.113.9", 9000, p, 64);
	CHECK_EQ(cg_fl_observe(t, b, len, 2000), CG_FL_SRT);
	memset(p, 0, 64); /* a data packet after */
	len = udp4(b, "203.0.113.9", 9000, "192.168.1.6", 9000, p, 1316);
	CHECK_EQ(cg_fl_kind(t, b, len, 2100), CG_FL_SRT);
	p[0] = 0x80; /* version 3: not SRT's */
	p[19] = 3;
	CHECK_EQ(cg_fl_says(p, 64), CG_FL_NONE);

	/* RIST: the receiver's NACK on RTCP (port + 1) teaches the RTP flow too */
	memset(p, 0, sizeof(p));
	p[0] = 0x81; /* V=2, FMT 1 */
	p[1] = 205;
	len = udp4(b, "203.0.113.9", 5001, "192.168.1.7", 5001, p, 16);
	CHECK_EQ(cg_fl_observe(t, b, len, 3000), CG_FL_NACK);
	memset(p, 0, 12);
	p[0] = 0x80;
	p[1] = 33; /* MP2T over RTP */
	len = udp4(b, "192.168.1.7", 5000, "203.0.113.9", 5000, p, 1328);
	CHECK_EQ(cg_fl_kind(t, b, len, 3100), CG_FL_NACK);
	p[0] = 0x81;
	p[1] = 206; /* a payload-specific feedback (PLI): not a NACK */
	CHECK_EQ(cg_fl_says(p, 16), CG_FL_NONE);

	/* TCP and a fragment say nothing */
	p[0] = 0xc3;
	p[1] = 0, p[2] = 0, p[3] = 0, p[4] = 1;
	len = udp4(b, "192.168.1.8", 50000, "142.250.0.1", 443, p, 1200);
	b[9] = 6;
	CHECK_EQ(cg_fl_observe(t, b, len, 4000), CG_FL_NONE);
	b[9] = 17;
	b[6] = 0x20;
	CHECK_EQ(cg_fl_observe(t, b, len, 4000), CG_FL_NONE);

	/* the counts by kind */
	CHECK_EQ(t->learned[CG_FL_QUIC], 1);
	CHECK_EQ(t->learned[CG_FL_SRT], 1);
	CHECK_EQ(t->learned[CG_FL_NACK], 2); /* RTCP's flow and its RTP's */

	/* a full table: the oldest of a probe makes room, nothing breaks */
	for (unsigned i = 0; i < 4 * CG_FL_SLOTS; i++) {
		char a[32];

		snprintf(a, sizeof(a), "10.%u.%u.1", i >> 8, i & 0xff);
		p[0] = 0xc3;
		p[4] = 1;
		len = udp4(b, a, 40000, "142.250.0.1", 443, p, 1200);
		cg_fl_observe(t, b, len, 5000 + i);
	}
	len = udp4(b, "10.7.255.1", 40000, "142.250.0.1", 443, p, 1200);
	CHECK_EQ(cg_fl_kind(t, b, len, 5000 + 4 * CG_FL_SLOTS), CG_FL_QUIC); /* the newest stays */

	/* split is the sender's mode: unheld only CG_FL_QUIET_MS after the
	 * last split packet */
	CHECK(cg_fl_unsplit(t, 1)); /* never split */
	cg_fl_split(t, 10000);
	CHECK(!cg_fl_unsplit(t, 10000));
	CHECK(!cg_fl_unsplit(t, 10000 + CG_FL_QUIET_MS));
	CHECK(cg_fl_unsplit(t, 10001 + CG_FL_QUIET_MS));
	free(t);
}
