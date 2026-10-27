/* The guard on rebuilt packets (rebuilt.h, story 025's wrong rebuilds).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "rebuilt.h"
#include "test.h"

/* The Internet checksum of n bytes at p onto a partial sum, written out
 * independently of rebuilt.h. */
static uint32_t t_sum(uint32_t s, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		s += i & 1 ? p[i] : (uint32_t)p[i] << 8;
	return s;
}

static uint16_t t_csum(uint32_t s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (uint16_t)~s;
}

static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* An IPv4 packet of proto with n bytes of transport (filled), its sums
 * right; at csum_at in the transport, the transport's sum (-1: none). */
static size_t mk4(uint8_t *p, uint8_t proto, size_t n, int csum_at, int pseudo)
{
	size_t len = 20 + n;
	uint32_t s = 0;

	memset(p, 0, len);
	p[0] = 0x45;
	put16(p + 2, (unsigned)len);
	p[8] = 64;
	p[9] = proto;
	memcpy(p + 12, "\x0a\x4f\x00\x02\xc6\x33\x64\x07", 8);
	for (size_t i = 0; i < n; i++)
		p[20 + i] = (uint8_t)(i * 7 + 3);
	if (csum_at >= 0) {
		put16(p + 20 + csum_at, 0);
		if (pseudo) {
			s = t_sum(s, p + 12, 8);
			s += proto + (uint32_t)n;
		}
		put16(p + 20 + csum_at, t_csum(t_sum(s, p + 20, n)));
	}
	put16(p + 10, t_csum(t_sum(0, p, 20)));
	return len;
}

static size_t mk6(uint8_t *p, uint8_t nh, size_t n, int csum_at)
{
	size_t len = 40 + n;
	uint32_t s = 0;

	memset(p, 0, len);
	p[0] = 0x60;
	put16(p + 4, (unsigned)n);
	p[6] = nh;
	p[7] = 64;
	for (int i = 0; i < 32; i++)
		p[8 + i] = (uint8_t)(0xfd + i * 11);
	for (size_t i = 0; i < n; i++)
		p[40 + i] = (uint8_t)(i * 5 + 1);
	if (csum_at >= 0) {
		put16(p + 40 + csum_at, 0);
		s = t_sum(s, p + 8, 32);
		s += nh + (uint32_t)n;
		put16(p + 40 + csum_at, t_csum(t_sum(s, p + 40, n)));
	}
	return len;
}

void test_rebuilt(void)
{
	static uint8_t p[2048];
	size_t len;

	/* IPv4 TCP, UDP, ICMP, right; one byte off anywhere: bad */
	len = mk4(p, 6, 1380, 16, 1);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[700] ^= 0x10;
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	len = mk4(p, 6, 1381, 16, 1); /* odd length */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[5] ^= 1; /* the header: its sum */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	len = mk4(p, 17, 900, 6, 1);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[len - 1] ^= 0x80;
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	len = mk4(p, 17, 900, -1, 1); /* UDP without a checksum: the header's */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD); /* the pattern where its sum goes */
	p[20 + 6] = p[20 + 7] = 0;
	put16(p + 10, 0);
	put16(p + 10, t_csum(t_sum(0, p, 20)));
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	len = mk4(p, 1, 64, 2, 0);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[30] ^= 4;
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	/* a fragment and another protocol: the header's sum */
	len = mk4(p, 6, 600, -1, 0);
	p[6] = 0x20; /* MF */
	put16(p + 10, 0);
	put16(p + 10, t_csum(t_sum(0, p, 20)));
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	len = mk4(p, 47, 300, -1, 0); /* GRE */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	len = mk4(p, 6, 10, -1, 0); /* TCP shorter than its header */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	/* the IP length field against what was rebuilt */
	len = mk4(p, 6, 1380, 16, 1);
	CHECK_EQ(cg_rebuilt_check(p, len - 1), CG_RB_BAD);
	/* IPv6 TCP, UDP, ICMPv6; another next header: nothing to check it with */
	len = mk6(p, 6, 1300, 16);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[100] ^= 2;
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	len = mk6(p, 17, 333, 6);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[9] ^= 2; /* the source address: the pseudo header */
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	len = mk6(p, 58, 72, 2);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	len = mk6(p, 44, 200, -1);
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_UNSURE);
	/* garbage */
	for (size_t i = 0; i < 1400; i++)
		p[i] = (uint8_t)(i * 131 + 17);
	CHECK_EQ(cg_rebuilt_check(p, 1400), CG_RB_BAD);
	/* a protocontroller message: its tag */
	len = cg_pctl_seal(p, cg_pctl_put_rst(p, 0x10002, 3));
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_OK);
	p[3] ^= 1;
	CHECK_EQ(cg_rebuilt_check(p, len), CG_RB_BAD);
	CHECK_EQ(cg_rebuilt_check(p, 3), CG_RB_BAD);
}
