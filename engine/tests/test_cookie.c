/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <string.h>

#include "cookie.h"
#include "test.h"

static struct sockaddr_storage v4(const char *ip, uint16_t port)
{
	struct sockaddr_storage s;
	struct sockaddr_in *a = (struct sockaddr_in *)&s;

	memset(&s, 0, sizeof(s));
	a->sin_family = AF_INET;
	a->sin_port = htons(port);
	inet_pton(AF_INET, ip, &a->sin_addr);
	return s;
}

static struct sockaddr_storage v6(const char *ip, uint16_t port)
{
	struct sockaddr_storage s;
	struct sockaddr_in6 *a = (struct sockaddr_in6 *)&s;

	memset(&s, 0, sizeof(s));
	a->sin6_family = AF_INET6;
	a->sin6_port = htons(port);
	inet_pton(AF_INET6, ip, &a->sin6_addr);
	return s;
}

void test_cookie(void)
{
	struct cg_cookie_keys k;
	struct cg_bucket b;
	uint8_t f1[16], f2[16], f3[16];
	struct sockaddr_storage a = v4("192.0.2.1", 40000), a2 = v4("192.0.2.1", 40001), a3 = v4("192.0.2.2", 40000),
				a6 = v6("2001:db8::1", 40000);
	uint64_t t0 = 1000000; /* epoch 33, 10 s in */
	uint32_t c;
	int n;

	memset(&k, 0, sizeof(k));
	memset(f1, 1, sizeof(f1));
	memset(f2, 2, sizeof(f2));
	memset(f3, 3, sizeof(f3));

	/* No keys: due, and nothing verifies. */
	CHECK(cg_cookie_due(&k, t0));
	CHECK(!cg_cookie_ok(&k, 12345, 0, 7, 1, &a));
	cg_cookie_rotate(&k, t0, f1);
	CHECK(!cg_cookie_due(&k, t0));
	CHECK(!cg_cookie_due(&k, t0 + 19999));
	CHECK(cg_cookie_due(&k, t0 + 20000)); /* next epoch */

	/* Bound to client, session, link, family, address and port; never 0. */
	c = cg_cookie_make(&k, 0, 7, 1, &a);
	CHECK(c != 0);
	CHECK(cg_cookie_ok(&k, c, 0, 7, 1, &a));
	CHECK(!cg_cookie_ok(&k, 0, 0, 7, 1, &a));
	CHECK(!cg_cookie_ok(&k, c, 1, 7, 1, &a));
	CHECK(!cg_cookie_ok(&k, c, 0, 8, 1, &a));
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 2, &a));
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 1, &a2));
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 1, &a3));
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 1, &a6));
	CHECK(cg_cookie_make(&k, 0, 7, 1, &a6) != 0);
	CHECK(cg_cookie_ok(&k, cg_cookie_make(&k, 0, 7, 1, &a6), 0, 7, 1, &a6));

	/* The next epoch: the old key still verifies, new cookies differ. */
	cg_cookie_rotate(&k, t0 + 20000, f2);
	CHECK_EQ(k.have, 2);
	CHECK(cg_cookie_ok(&k, c, 0, 7, 1, &a));
	CHECK(cg_cookie_make(&k, 0, 7, 1, &a) != c);
	/* Two epochs on: gone. */
	cg_cookie_rotate(&k, t0 + 50000, f3);
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 1, &a));
	/* A skipped epoch drops the previous key too: never older than 60 s. */
	c = cg_cookie_make(&k, 0, 7, 1, &a);
	cg_cookie_rotate(&k, t0 + 110000, f1);
	CHECK_EQ(k.have, 1);
	CHECK(!cg_cookie_ok(&k, c, 0, 7, 1, &a));

	/* The bucket: a burst, then the rate. */
	memset(&b, 0, sizeof(b));
	n = 0;
	for (int i = 0; i < 100; i++)
		n += cg_bucket_take(&b, 5000, 20, 40);
	CHECK_EQ(n, 40);
	CHECK(!cg_bucket_take(&b, 5049, 20, 40)); /* 0.98 token */
	CHECK(cg_bucket_take(&b, 5050, 20, 40));  /* one every 50 ms */
	CHECK(!cg_bucket_take(&b, 5050, 20, 40));
	CHECK(!cg_bucket_take(&b, 4000, 20, 40)); /* a clock that goes back adds nothing */
	n = 0;
	for (int i = 0; i < 100; i++)
		n += cg_bucket_take(&b, 100000, 20, 40);
	CHECK_EQ(n, 40); /* never more than the burst */
}
