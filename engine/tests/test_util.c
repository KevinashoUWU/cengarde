/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "test.h"
#include "util.h"

static int mapped(const struct sockaddr_storage *a)
{
	return a->ss_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((const struct sockaddr_in6 *)a)->sin6_addr);
}

/* IPv4-mapped addresses, and what can be a server. */
static void test_util_peer(void)
{
	static const char *const bad[] = {
		"*:1", "0.0.0.0:1", "[::]:1", "[::ffff:0.0.0.0]:1", /* the wildcard in every spelling */
		"224.0.0.1:1", "239.255.255.255:1", "[ff02::1]:1", "[ff0e::fb]:1", "[::ffff:224.0.0.251]:1",
	};
	static const char *const good[] = {
		"192.0.2.1:1", "223.255.255.255:1", "240.0.0.1:1", "127.0.0.1:1", "[::1]:1",
		"[2001:db8::1]:1", "[fe80::1]:1", "[fd00::1]:1", "[::ffff:10.0.0.1]:1",
	};
	struct sockaddr_storage a[4], b;
	struct sockaddr_in6 m = { .sin6_family = AF_INET6, .sin6_port = htons(9) };
	char buf[64], err[128];
	int n;

	/* Mapped becomes IPv4, port included; native IPv6 stays as it is. */
	CHECK_EQ(cg_addr_parse("[::ffff:192.0.2.1]:59402", 0, a, 4, err, sizeof(err)), 1);
	CHECK_EQ(a[0].ss_family, AF_INET);
	CHECK_EQ(cg_addr_len(&a[0]), sizeof(struct sockaddr_in));
	cg_addr_parse("192.0.2.1:59402", 0, &b, 1, err, sizeof(err));
	CHECK(cg_addr_equal(&a[0], &b));
	CHECK_EQ(cg_addr_parse("[::ffff:c000:201]:7", 0, a, 4, err, sizeof(err)), 1); /* hex spelling */
	CHECK(a[0].ss_family == AF_INET && !strcmp(cg_addr_str(&a[0], buf, sizeof(buf)), "192.0.2.1:7"));
	CHECK_EQ(cg_addr_parse("[::192.0.2.1]:7", 0, a, 4, err, sizeof(err)), 1); /* compatible, not mapped */
	CHECK_EQ(a[0].ss_family, AF_INET6);
	CHECK_EQ(cg_addr_parse("[2001:db8::ffff:c000:201]:7", 0, a, 4, err, sizeof(err)), 1);
	CHECK_EQ(a[0].ss_family, AF_INET6);
	cg_addr_unmap(&a[0]);
	CHECK(!strcmp(cg_addr_str(&a[0], buf, sizeof(buf)), "[2001:db8::ffff:c000:201]:7"));
	cg_addr_unmap(&b); /* IPv4 too */
	CHECK(!strcmp(cg_addr_str(&b, buf, sizeof(buf)), "192.0.2.1:59402"));
	n = cg_addr_parse("localhost:5", 1, a, 4, err, sizeof(err));
	for (int i = 0; i < n; i++) /* whatever the resolver gives */
		CHECK(!mapped(&a[i]));

	/* The way a dual-stack socket reports an IPv4 peer. */
	m.sin6_addr.s6_addr[10] = m.sin6_addr.s6_addr[11] = 0xff;
	m.sin6_addr.s6_addr[12] = 10;
	m.sin6_addr.s6_addr[15] = 1;
	memset(&b, 0, sizeof(b));
	memcpy(&b, &m, sizeof(m));
	CHECK(mapped(&b));
	cg_addr_unmap(&b);
	CHECK(b.ss_family == AF_INET && !strcmp(cg_addr_str(&b, buf, sizeof(buf)), "10.0.0.1:9"));

	/* Not a server: the wildcard and multicast, mapped or not. */
	for (size_t i = 0; i < CG_ARRAY_SIZE(bad); i++) {
		CHECK_EQ(cg_addr_parse(bad[i], 0, a, 4, err, sizeof(err)), 1);
		CHECK(cg_addr_unfit_peer(&a[0]) != NULL);
	}
	for (size_t i = 0; i < CG_ARRAY_SIZE(good); i++) {
		CHECK_EQ(cg_addr_parse(good[i], 0, a, 4, err, sizeof(err)), 1);
		CHECK(cg_addr_unfit_peer(&a[0]) == NULL);
	}
	cg_addr_parse("0.0.0.0:1", 0, a, 1, err, sizeof(err));
	CHECK(!strcmp(cg_addr_unfit_peer(&a[0]), "the wildcard address"));
	cg_addr_parse("[ff02::1]:1", 0, a, 1, err, sizeof(err));
	CHECK(!strcmp(cg_addr_unfit_peer(&a[0]), "a multicast address"));
	m.sin6_addr.s6_addr[12] = 224; /* ::ffff:224.0.0.1, left mapped */
	memcpy(&b, &m, sizeof(m));
	CHECK(!strcmp(cg_addr_unfit_peer(&b), "a multicast address"));
	CHECK(mapped(&b)); /* checked, not changed */
	memset(&m.sin6_addr.s6_addr[12], 0, 4);
	memcpy(&b, &m, sizeof(m));
	CHECK(!strcmp(cg_addr_unfit_peer(&b), "the wildcard address"));
	memset(&b, 0, sizeof(b));
	CHECK(cg_addr_unfit_peer(&b) != NULL); /* AF_UNSPEC */
}

void test_util(void)
{
	uint8_t in[32], out[40];
	char enc[64], buf[64], err[128];
	struct sockaddr_storage a[4], b;

	/* base64 round trips for every length and the RFC 4648 vectors. */
	for (int i = 0; i < 32; i++)
		in[i] = (uint8_t)(i * 37 + 11);
	for (size_t len = 0; len <= 32; len++) {
		cg_base64_encode(enc, in, len);
		CHECK_EQ(cg_base64_decode(out, sizeof(out), enc), (long long)len);
		CHECK(!memcmp(in, out, len));
	}
	cg_base64_encode(enc, (const uint8_t *)"foobar", 6);
	CHECK(!strcmp(enc, "Zm9vYmFy"));
	cg_base64_encode(enc, (const uint8_t *)"fo", 2);
	CHECK(!strcmp(enc, "Zm8="));
	CHECK_EQ(cg_base64_decode(out, sizeof(out), "Zm9v!mFy"), -1);
	CHECK_EQ(cg_base64_decode(out, sizeof(out), "Zm9"), -1);
	CHECK_EQ(cg_base64_decode(out, sizeof(out), "Z==="), -1);
	CHECK_EQ(cg_base64_decode(out, sizeof(out), "Zm=v"), -1);
	CHECK_EQ(cg_base64_decode(out, 2, "Zm9v"), -1); /* does not fit */

	/* Addresses. */
	CHECK_EQ(cg_addr_parse("10.0.1.2:59402", 0, a, 4, err, sizeof(err)), 1);
	CHECK(!strcmp(cg_addr_str(&a[0], buf, sizeof(buf)), "10.0.1.2:59402"));
	CHECK_EQ(cg_addr_parse("[fe80::1]:1", 0, a, 4, err, sizeof(err)), 1);
	CHECK(!strcmp(cg_addr_str(&a[0], buf, sizeof(buf)), "[fe80::1]:1"));
	CHECK_EQ(cg_addr_parse("*:59402", 0, a, 4, err, sizeof(err)), 1);
	CHECK(!strcmp(cg_addr_str(&a[0], buf, sizeof(buf)), "[::]:59402"));
	CHECK_EQ(cg_addr_parse("::1:5", 0, a, 4, err, sizeof(err)), -1); /* IPv6 needs brackets */
	CHECK_EQ(cg_addr_parse("10.0.0.1:0", 0, a, 4, err, sizeof(err)), -1);
	CHECK_EQ(cg_addr_parse("10.0.0.1:70000", 0, a, 4, err, sizeof(err)), -1);
	CHECK_EQ(cg_addr_parse("10.0.0.1", 0, a, 4, err, sizeof(err)), -1);
	CHECK_EQ(cg_addr_parse("example.invalid:5", 0, a, 4, err, sizeof(err)), -1); /* names not allowed */
	CHECK_EQ(cg_addr_parse("localhost:5", 1, a, 4, err, sizeof(err)) >= 1, 1);

	/* Equality, including port. */
	cg_addr_parse("10.0.1.2:59402", 0, a, 1, err, sizeof(err));
	cg_addr_parse("10.0.1.2:59402", 0, &b, 1, err, sizeof(err));
	CHECK(cg_addr_equal(&a[0], &b));
	cg_addr_parse("10.0.1.2:59403", 0, &b, 1, err, sizeof(err));
	CHECK(!cg_addr_equal(&a[0], &b));

	/* Rate limiter: one event per interval, the rest counted. */
	struct cg_ratelimit rl = { 0, 0 };

	CHECK(cg_ratelimit_ok(&rl, 1000, 500));
	CHECK(!cg_ratelimit_ok(&rl, 1200, 500));
	CHECK(!cg_ratelimit_ok(&rl, 1499, 500));
	CHECK(cg_ratelimit_ok(&rl, 1500, 500));
	CHECK_EQ(rl.suppressed, 2);

	test_util_peer();
}
