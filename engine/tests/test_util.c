/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "test.h"
#include "util.h"

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
}
