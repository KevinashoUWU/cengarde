/* The operator's flow classes (flowrule.h, story 031).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "flowrule.h"
#include "test.h"

/* A UDP (proto 17) or TCP (6) packet from s:sp to d:dp into b; its length. */
static size_t pkt4(uint8_t *b, const char *s, uint16_t sp, const char *d, uint16_t dp, uint8_t proto)
{
	memset(b, 0, 60);
	b[0] = 0x45;
	b[3] = 48;
	b[8] = 64;
	b[9] = proto;
	inet_pton(AF_INET, s, b + 12);
	inet_pton(AF_INET, d, b + 16);
	b[20] = (uint8_t)(sp >> 8);
	b[21] = (uint8_t)sp;
	b[22] = (uint8_t)(dp >> 8);
	b[23] = (uint8_t)dp;
	return 48;
}

static size_t pkt6(uint8_t *b, const char *s, uint16_t sp, const char *d, uint16_t dp)
{
	memset(b, 0, 80);
	b[0] = 0x60;
	b[5] = 16;
	b[6] = 17;
	b[7] = 64;
	inet_pton(AF_INET6, s, b + 8);
	inet_pton(AF_INET6, d, b + 24);
	b[40] = (uint8_t)(sp >> 8);
	b[41] = (uint8_t)sp;
	b[42] = (uint8_t)(dp >> 8);
	b[43] = (uint8_t)dp;
	return 56;
}

static void add(struct cg_fc *t, const char *rule)
{
	CHECK_EQ(cg_fc_parse(&t->r[t->n], rule), 0);
	t->n++;
}

void test_flowrule(void)
{
	struct cg_fc t;
	struct cg_fc_rule r;
	uint8_t b[96];
	size_t len;

	/* parsing: the forms, and what is refused */
	CHECK_EQ(cg_fc_parse(&r, "192.168.50.158:9330 ordered"), 0);
	CHECK_EQ(r.fam, 4);
	CHECK_EQ(r.len, 32);
	CHECK_EQ(r.port, 9330);
	CHECK_EQ(r.cls, CG_FC_ORDERED);
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.0/8 deadline"), 0);
	CHECK_EQ(r.len, 8);
	CHECK_EQ(r.port, 0);
	CHECK_EQ(cg_fc_parse(&r, ":5004 direct"), 0);
	CHECK_EQ(r.fam, 0);
	CHECK_EQ(r.port, 5004);
	CHECK_EQ(cg_fc_parse(&r, "[2001:db8::7]:5000 direct"), 0);
	CHECK_EQ(r.fam, 6);
	CHECK_EQ(r.len, 128);
	CHECK_EQ(r.port, 5000);
	CHECK_EQ(cg_fc_parse(&r, "[2001:db8::]/32:443 auto"), 0);
	CHECK_EQ(r.len, 32);
	CHECK_EQ(r.port, 443);
	CHECK_EQ(r.cls, CG_FC_AUTO);
	CHECK_EQ(cg_fc_parse(&r, "[2001:db8::]/48 ordered"), 0);
	CHECK_EQ(r.len, 48);
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.1:9330 fast"), -1);   /* no such class */
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.1:0 direct"), -1);    /* port 0 */
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.1:70000 direct"), -1);
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.0/33 direct"), -1);
	CHECK_EQ(cg_fc_parse(&r, "300.0.0.1 direct"), -1);
	CHECK_EQ(cg_fc_parse(&r, "2001:db8::7 direct"), -1);   /* IPv6 needs brackets */
	CHECK_EQ(cg_fc_parse(&r, ": direct"), -1);
	CHECK_EQ(cg_fc_parse(&r, "10.0.0.1"), -1);             /* no class */

	/* matching: source or destination, the first rule, ports of UDP and TCP */
	memset(&t, 0, sizeof(t));
	len = pkt4(b, "192.168.50.158", 9330, "64.176.19.43", 7000, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE); /* no rules */
	add(&t, "192.168.50.158:9330 ordered");
	add(&t, "192.168.50.0/24 direct");
	add(&t, ":5004 deadline");
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_ORDERED);       /* its source */
	len = pkt4(b, "64.176.19.43", 7000, "192.168.50.158", 9330, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_ORDERED);       /* the way back: its destination */
	len = pkt4(b, "192.168.50.158", 9331, "64.176.19.43", 7000, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_DIRECT);        /* another port: the prefix */
	len = pkt4(b, "192.168.51.9", 1000, "8.8.8.8", 5004, 6);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_DEADLINE);      /* any address, TCP's port */
	len = pkt4(b, "192.168.51.9", 1000, "8.8.8.8", 53, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);
	len = pkt4(b, "192.168.51.9", 0, "8.8.8.8", 0, 1); /* ICMP: no ports */
	b[20] = 0x13;
	b[21] = 0x8c; /* bytes that would read 5004 */
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);
	len = pkt4(b, "192.168.51.9", 1000, "8.8.8.8", 5004, 17);
	b[6] = 0x00;
	b[7] = 0x10; /* a later fragment: no ports */
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);
	len = pkt4(b, "192.168.50.7", 1000, "8.8.8.8", 53, 17);
	b[6] = 0x20; /* the first fragment (MF): its ports are there */
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_DIRECT);

	/* IPv6, and an auto rule that keeps a flow out of a broader one */
	memset(&t, 0, sizeof(t));
	add(&t, "[2001:db8::7]:5000 auto");
	add(&t, "[2001:db8::]/32 direct");
	len = pkt6(b, "2001:db8::7", 5000, "2001:4860::8888", 443);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_AUTO);
	len = pkt6(b, "2001:db8:1::9", 5000, "2001:4860::8888", 443);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_DIRECT);
	len = pkt6(b, "2001:db9::9", 5000, "2001:4860::8888", 443);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);
	len = pkt4(b, "10.0.0.1", 5000, "10.0.0.2", 5000, 17); /* an IPv4 packet never matches an IPv6 rule */
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);

	/* the reorderer's word: the rule over the guess */
	{
		uint8_t w, nh;

		cg_fc_hold(CG_FC_NONE, 1, &w, &nh); /* no rule, MPEG-TS: it waits */
		CHECK(w && !nh);
		cg_fc_hold(CG_FC_NONE, 0, &w, &nh);
		CHECK(!w && !nh);
		cg_fc_hold(CG_FC_AUTO, 1, &w, &nh); /* auto: the guess */
		CHECK(w && !nh);
		cg_fc_hold(CG_FC_ORDERED, 0, &w, &nh); /* ordered whatever its bytes */
		CHECK(w && !nh);
		cg_fc_hold(CG_FC_DEADLINE, 1, &w, &nh); /* a TS told to keep the deadline rule */
		CHECK(!w && !nh);
		cg_fc_hold(CG_FC_DIRECT, 1, &w, &nh); /* never held */
		CHECK(!w && nh);
	}

	/* a prefix not on a byte boundary */
	memset(&t, 0, sizeof(t));
	add(&t, "10.1.16.0/20 ordered");
	len = pkt4(b, "10.1.31.255", 1, "1.1.1.1", 1, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_ORDERED);
	len = pkt4(b, "10.1.32.0", 1, "1.1.1.1", 1, 17);
	CHECK_EQ(cg_fc_match(&t, b, len), CG_FC_NONE);
}
