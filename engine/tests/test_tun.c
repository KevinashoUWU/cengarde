/* The native tunnel's pure parts: IP checks (ippkt.h), routes (tunroute.h)
 * and flows (flow.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "flow.h"
#include "ippkt.h"
#include "test.h"
#include "tunroute.h"

/* A minimal IPv4 packet of len bytes, protocol proto, ports sport/dport
 * after a 20-byte header (when there is room). */
static void ip4(uint8_t *b, size_t len, uint8_t proto, const char *src, const char *dst, uint16_t sport,
		uint16_t dport)
{
	memset(b, 0, len);
	b[0] = 0x45;
	b[2] = (uint8_t)(len >> 8);
	b[3] = (uint8_t)len;
	b[8] = 64;
	b[9] = proto;
	inet_pton(AF_INET, src, b + 12);
	inet_pton(AF_INET, dst, b + 16);
	if (len >= 24) {
		b[20] = (uint8_t)(sport >> 8);
		b[21] = (uint8_t)sport;
		b[22] = (uint8_t)(dport >> 8);
		b[23] = (uint8_t)dport;
	}
}

static void ip6(uint8_t *b, size_t len, uint8_t nh, const char *src, const char *dst)
{
	memset(b, 0, len);
	b[0] = 0x60;
	b[4] = (uint8_t)((len - 40) >> 8);
	b[5] = (uint8_t)(len - 40);
	b[6] = nh;
	b[7] = 64;
	inet_pton(AF_INET6, src, b + 8);
	inet_pton(AF_INET6, dst, b + 24);
}

void test_ippkt(void)
{
	uint8_t b[200];

	ip4(b, 84, 1, "10.79.0.2", "10.79.0.1", 0, 0);
	CHECK_EQ(cg_ip_check(b, 84), 4);
	CHECK(!memcmp(cg_ip_src(b, 4), (uint8_t[]){ 10, 79, 0, 2 }, 4));
	CHECK(!memcmp(cg_ip_dst(b, 4), (uint8_t[]){ 10, 79, 0, 1 }, 4));
	/* the length field says exactly what came */
	CHECK_EQ(cg_ip_check(b, 83), 0);
	CHECK_EQ(cg_ip_check(b, 85), 0);
	CHECK_EQ(cg_ip_check(b, 0), 0);
	CHECK_EQ(cg_ip_check(b, 19), 0);
	/* a header length below 5 words, or past the packet */
	b[0] = 0x44;
	CHECK_EQ(cg_ip_check(b, 84), 0);
	b[0] = 0x4f;
	ip4(b, 40, 1, "1.2.3.4", "5.6.7.8", 0, 0);
	b[0] = 0x4f; /* 60 bytes of header in 40 */
	CHECK_EQ(cg_ip_check(b, 40), 0);
	/* neither 4 nor 6: a WireGuard data message, say */
	b[0] = 0x04;
	CHECK_EQ(cg_ip_check(b, 40), 0);

	ip6(b, 104, 58, "fd00::2", "fd00::1");
	CHECK_EQ(cg_ip_check(b, 104), 6);
	CHECK_EQ(cg_ip_src(b, 6)[15], 2);
	CHECK_EQ(cg_ip_dst(b, 6)[15], 1);
	CHECK_EQ(cg_ip_check(b, 103), 0);
	CHECK_EQ(cg_ip_check(b, 39), 0);
	ip6(b, 40, 59, "::1", "::1"); /* no payload at all */
	CHECK_EQ(cg_ip_check(b, 40), 6);
}

static struct cg_pfx P(const char *s)
{
	struct cg_pfx p;

	CHECK_EQ(cg_pfx_parse(s, &p, 0), 0);
	return p;
}

void test_tunroute(void)
{
	static struct cg_rt rt;
	struct cg_pfx p;
	uint8_t a4[4], a6[16];
	char buf[64];

	/* parsing */
	CHECK_EQ(cg_pfx_parse("10.79.0.0/16", &p, 0), 0);
	CHECK_EQ(p.family, AF_INET);
	CHECK_EQ(p.len, 16);
	CHECK_EQ(cg_pfx_parse("10.79.0.2", &p, 0), 0);
	CHECK_EQ(p.len, 32);
	CHECK_EQ(cg_pfx_parse("fd12:3456::/64", &p, 0), 0);
	CHECK_EQ(p.family, AF_INET6);
	CHECK_EQ(p.len, 64);
	CHECK_EQ(cg_pfx_parse("::/0", &p, 0), 0);
	CHECK_EQ(p.len, 0);
	CHECK(!strcmp(cg_pfx_str(&p, buf, sizeof(buf)), "::/0"));
	CHECK_EQ(cg_pfx_parse("2001:db8::1", &p, 0), 0);
	CHECK_EQ(p.len, 128);
	/* host bits: refused in a route, taken in an interface address */
	CHECK_EQ(cg_pfx_parse("10.79.0.1/16", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("10.79.0.1/16", &p, 1), 0);
	CHECK(!strcmp(cg_pfx_str(&p, buf, sizeof(buf)), "10.79.0.1/16"));
	CHECK_EQ(cg_pfx_parse("10.79.0.0/33", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("fd00::/129", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("10.79.0.0/", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("10.79.0.0/-1", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("10.79.0.0/1x", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("/8", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("vps.example.com", &p, 0), -1);
	CHECK_EQ(cg_pfx_parse("", &p, 0), -1);

	/* matching, including a length that is not a whole byte */
	p = P("10.79.128.0/17");
	inet_pton(AF_INET, "10.79.200.1", a4);
	CHECK(cg_pfx_match(&p, AF_INET, a4));
	inet_pton(AF_INET, "10.79.127.255", a4);
	CHECK(!cg_pfx_match(&p, AF_INET, a4));
	CHECK(!cg_pfx_match(&p, AF_INET6, a4));

	/* longest prefix wins, whatever the order they came in */
	memset(&rt, 0, sizeof(rt));
	p = P("0.0.0.0/0");
	CHECK_EQ(cg_rt_add(&rt, &p, 0), 0);
	p = P("10.79.0.0/16");
	CHECK_EQ(cg_rt_add(&rt, &p, 1), 0);
	p = P("10.79.12.34/32");
	CHECK_EQ(cg_rt_add(&rt, &p, 2), 0);
	p = P("192.168.1.0/24");
	CHECK_EQ(cg_rt_add(&rt, &p, 2), 0);
	p = P("fd12:3456::/64");
	CHECK_EQ(cg_rt_add(&rt, &p, 3), 0);
	inet_pton(AF_INET, "10.79.12.34", a4);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET, a4), 2);
	inet_pton(AF_INET, "10.79.12.35", a4);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET, a4), 1);
	inet_pton(AF_INET, "192.168.1.77", a4);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET, a4), 2);
	inet_pton(AF_INET, "8.8.8.8", a4);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET, a4), 0);
	inet_pton(AF_INET6, "fd12:3456::9", a6);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET6, a6), 3);
	inet_pton(AF_INET6, "fd12:3457::9", a6);
	CHECK_EQ(cg_rt_lookup(&rt, AF_INET6, a6), -1); /* no ::/0 */
	for (int i = 1; i < rt.n; i++)
		CHECK(rt.p[i - 1].len >= rt.p[i].len);
	CHECK_EQ(cg_rt_clash(&rt, 0), -1);

	/* one prefix, two clients: a clash; the same client twice is not */
	p = P("192.168.1.0/24");
	CHECK_EQ(cg_rt_add(&rt, &p, 2), 0);
	for (int i = 0; i < rt.n; i++)
		CHECK_EQ(cg_rt_clash(&rt, i), -1);
	CHECK_EQ(cg_rt_add(&rt, &p, 4), 0);
	{
		int clashes = 0;

		for (int i = 0; i < rt.n; i++)
			clashes += cg_rt_clash(&rt, i) >= 0;
		CHECK_EQ(clashes, 3);
	}

	/* full */
	memset(&rt, 0, sizeof(rt));
	p = P("10.0.0.0/8");
	for (int i = 0; i < CG_RT_MAX; i++)
		CHECK_EQ(cg_rt_add(&rt, &p, 0), 0);
	CHECK_EQ(cg_rt_add(&rt, &p, 0), -1);
}

void test_flow(void)
{
	uint8_t b[1500], c[1500];
	struct cg_flow f, g;

	/* TCP: ports read, and the flow the same whatever the length */
	ip4(b, 60, 6, "10.79.0.2", "10.79.0.1", 40000, 5201);
	CHECK_EQ(cg_flow_of(b, 60, 7, &f), 4);
	CHECK(f.tcp);
	CHECK_EQ(f.proto, 6);
	CHECK_EQ(f.sport, 40000);
	CHECK_EQ(f.dport, 5201);
	CHECK(!f.frag);
	ip4(c, 1400, 6, "10.79.0.2", "10.79.0.1", 40000, 5201);
	CHECK_EQ(cg_flow_of(c, 1400, 7, &g), 4);
	CHECK_EQ(f.hash, g.hash);
	/* another port, another flow; another seed, another hash */
	ip4(c, 60, 6, "10.79.0.2", "10.79.0.1", 40001, 5201);
	cg_flow_of(c, 60, 7, &g);
	CHECK(f.hash != g.hash);
	cg_flow_of(b, 60, 8, &g);
	CHECK(f.hash != g.hash);
	/* UDP is not TCP; ICMP has no ports */
	ip4(b, 60, 17, "10.79.0.2", "10.79.0.1", 53, 53);
	cg_flow_of(b, 60, 7, &f);
	CHECK(!f.tcp);
	CHECK_EQ(f.dport, 53);
	ip4(b, 84, 1, "10.79.0.2", "10.79.0.1", 0x0800, 0);
	cg_flow_of(b, 84, 7, &f);
	CHECK(!f.tcp);
	CHECK_EQ(f.sport, 0);
	/* a fragment, first or not: no ports, so all of them share a flow */
	ip4(b, 60, 6, "10.79.0.2", "10.79.0.1", 40000, 5201);
	b[6] = 0x20; /* MF */
	cg_flow_of(b, 60, 7, &f);
	CHECK(f.frag);
	CHECK(!f.tcp);
	ip4(c, 60, 6, "10.79.0.2", "10.79.0.1", 1, 2);
	c[7] = 10; /* offset 80 */
	cg_flow_of(c, 60, 7, &g);
	CHECK(g.frag);
	CHECK_EQ(f.hash, g.hash);
	/* the header claims ports past its end: none read */
	ip4(b, 22, 6, "10.79.0.2", "10.79.0.1", 0, 0);
	cg_flow_of(b, 22, 7, &f);
	CHECK(!f.tcp);
	/* not IP */
	memset(&f, 0xaa, sizeof(f));
	b[0] = 0x04;
	CHECK_EQ(cg_flow_of(b, 22, 7, &f), 0);
	CHECK_EQ(f.proto, 0xaa);

	/* IPv6: TCP straight after the header, and after a hop-by-hop */
	ip6(b, 80, 6, "fd00::2", "fd00::1");
	b[40] = 0x9c;
	b[41] = 0x40; /* 40000 */
	b[42] = 0x14;
	b[43] = 0x51; /* 5201 */
	CHECK_EQ(cg_flow_of(b, 80, 7, &f), 6);
	CHECK(f.tcp);
	CHECK_EQ(f.sport, 40000);
	CHECK_EQ(f.dport, 5201);
	ip6(c, 88, 0, "fd00::2", "fd00::1");
	c[40] = 6; /* next: TCP, length 0: 8 bytes */
	memcpy(c + 48, b + 40, 4);
	CHECK_EQ(cg_flow_of(c, 88, 7, &g), 6);
	CHECK(g.tcp);
	CHECK_EQ(g.dport, 5201);
	CHECK_EQ(f.hash, g.hash);
	/* a fragment header: no ports */
	ip6(c, 88, 44, "fd00::2", "fd00::1");
	c[40] = 6;
	cg_flow_of(c, 88, 7, &g);
	CHECK(g.frag);
	CHECK(!g.tcp);
	CHECK_EQ(g.proto, 6);
	/* an extension header running past the packet: stops there */
	ip6(c, 44, 60, "fd00::2", "fd00::1");
	cg_flow_of(c, 44, 7, &g);
	CHECK_EQ(g.proto, 60);
	CHECK(!g.tcp);
}
