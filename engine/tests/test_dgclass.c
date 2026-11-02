/* What kind of datagram a TUN packet carries (dgclass.h, story 030).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "dgclass.h"
#include "test.h"

/* An IPv4 (or IPv6) UDP packet with n payload bytes p into b; its length. */
static size_t udp(uint8_t *b, int v6, const uint8_t *p, size_t n)
{
	size_t h = v6 ? CG_IP6_HDR : CG_IP4_HDR, len = h + 8 + n;

	memset(b, 0, h + 8);
	if (v6) {
		b[0] = 0x60;
		b[4] = (uint8_t)((len - CG_IP6_HDR) >> 8);
		b[5] = (uint8_t)(len - CG_IP6_HDR);
		b[6] = 17;
	} else {
		b[0] = 0x45;
		b[2] = (uint8_t)(len >> 8);
		b[3] = (uint8_t)len;
		b[9] = 17;
	}
	memcpy(b + h + 8, p, n);
	return len;
}

void test_dgclass(void)
{
	uint8_t b[1600], p[1400];
	size_t len;

	/* MPEG-TS: 7 packets of 188, each with its sync byte */
	memset(p, 0xff, sizeof(p));
	for (int i = 0; i < 7; i++)
		p[i * 188] = 0x47;
	for (int v6 = 0; v6 < 2; v6++) {
		len = udp(b, v6, p, 1316);
		CHECK_EQ(cg_dg_class(b, len), CG_DG_TS);
	}
	len = udp(b, 0, p, 188);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_TS);
	len = udp(b, 0, p, 1315); /* not a multiple of 188 */
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[3 * 188] = 0x46; /* a sync byte missing */
	len = udp(b, 0, p, 1316);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[3 * 188] = 0x47;
	b[9] = 6; /* TCP, the same bytes */
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	b[9] = 17;
	b[6] = 0x20; /* a fragment (MF) */
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);

	/* RTP: version 2, a dynamic payload type (96), with and without the marker */
	memset(p, 0, sizeof(p));
	p[0] = 0x80;
	p[1] = 96;
	len = udp(b, 0, p, 1328);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_RTP);
	p[1] = 0x80 | 96;
	len = udp(b, 1, p, 172);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_RTP);
	p[1] = 0; /* PCMU */
	len = udp(b, 0, p, 172);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_RTP);
	p[1] = 50; /* unassigned: not taken for RTP */
	len = udp(b, 0, p, 172);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[1] = 200; /* RTCP sender report */
	len = udp(b, 0, p, 28);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_RTP);
	p[0] = 0x8f; /* 15 CSRCs: 72 bytes of header, more than carried */
	p[1] = 96;
	len = udp(b, 0, p, 40);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[0] = 0x40; /* version 1, or a QUIC short header */
	len = udp(b, 0, p, 1200);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[0] = 0xc3; /* a QUIC long header */
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
	p[0] = 0x80;
	len = udp(b, 0, p, 11); /* shorter than RTP's header */
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);

	/* TS inside RTP (RFC 2250) is RTP; not an IP packet is nothing */
	p[0] = 0x80;
	p[1] = 33;
	for (int i = 0; i < 7; i++)
		p[12 + i * 188] = 0x47;
	len = udp(b, 0, p, 12 + 1316);
	CHECK_EQ(cg_dg_class(b, len), CG_DG_RTP);
	CHECK_EQ(cg_dg_class(b, 10), CG_DG_OTHER);
	b[0] = 0x35;
	CHECK_EQ(cg_dg_class(b, len), CG_DG_OTHER);
}
