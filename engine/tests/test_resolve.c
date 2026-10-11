/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <string.h>

#include "resolve.h"
#include "test.h"
#include "util.h"

static struct sockaddr_storage addr(const char *s)
{
	struct sockaddr_storage a;
	char err[128];

	CHECK_EQ(cg_addr_parse(s, 0, &a, 1, err, sizeof(err)), 1);
	return a;
}

/* An answer of n IPv4 addresses 192.0.2.X, X from xs. */
static struct cg_dns_result answer4(const int *xs, int n, uint32_t ttl)
{
	struct cg_dns_result r;

	memset(&r, 0, sizeof(r));
	r.outcome = CG_DNS_ADDRS;
	r.ttl = ttl;
	r.n = n;
	for (int i = 0; i < n; i++) {
		r.addr[i][0] = 192;
		r.addr[i][2] = 2;
		r.addr[i][3] = (uint8_t)xs[i];
	}
	return r;
}

static struct cg_dns_result empty(int outcome)
{
	struct cg_dns_result r;

	memset(&r, 0, sizeof(r));
	r.outcome = outcome;
	return r;
}

static void test_ttl_and_sort(void)
{
	uint8_t src[6][16] = { { 9 }, { 3 }, { 7 }, { 3 }, { 1 }, { 5 } }, dst[CG_NAME_ADDRS][16];
	uint8_t rot[6][16] = { { 5 }, { 1 }, { 9 }, { 7 }, { 3 }, { 3 } }, dst2[CG_NAME_ADDRS][16];

	CHECK_EQ(cg_rs_ttl_ms(0), CG_RS_TTL_MIN_MS);
	CHECK_EQ(cg_rs_ttl_ms(29), CG_RS_TTL_MIN_MS);
	CHECK_EQ(cg_rs_ttl_ms(60), 60000);
	CHECK_EQ(cg_rs_ttl_ms(900), 900000);
	CHECK_EQ(cg_rs_ttl_ms(86400), CG_RS_TTL_MAX_MS);
	CHECK_EQ(cg_rs_ttl_ms(UINT32_MAX), CG_RS_TTL_MAX_MS);

	/* Ascending, each once, the smallest CG_NAME_ADDRS: the same subset
	 * whatever the order, so a round-robin name moves no link. */
	CHECK_EQ(cg_rs_sorted((const uint8_t(*)[16])src, 6, 4, dst), 4);
	CHECK(dst[0][0] == 1 && dst[1][0] == 3 && dst[2][0] == 5 && dst[3][0] == 7);
	CHECK_EQ(cg_rs_sorted((const uint8_t(*)[16])rot, 6, 4, dst2), 4);
	CHECK(!memcmp(dst, dst2, sizeof(dst)));
	CHECK_EQ(cg_rs_sorted((const uint8_t(*)[16])src, 0, 4, dst), 0);
}

static void test_answers(void)
{
	struct cg_rsname nm;
	struct cg_dns_result r;
	struct cg_nameaddrs a;
	int one[] = { 10 }, two[] = { 20, 10 }, back[] = { 10, 20 }, other[] = { 30 };
	uint64_t t = 1000000;

	memset(&nm, 0, sizeof(nm));
	CHECK(cg_rs_due(&nm.q[CG_RS_A], t)); /* a new name: at once */
	cg_rs_start(&nm.q[CG_RS_A], 7, t);
	CHECK(!cg_rs_due(&nm.q[CG_RS_A], t));
	CHECK(!cg_rs_timed_out(&nm.q[CG_RS_A], t + CG_RS_TIMEOUT_MS - 1));
	CHECK(cg_rs_timed_out(&nm.q[CG_RS_A], t + CG_RS_TIMEOUT_MS));

	r = answer4(one, 1, 60);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 0, t), 1);
	CHECK(!nm.q[CG_RS_A].inflight && nm.q[CG_RS_A].answered && nm.q[CG_RS_A].n == 1);
	CHECK_EQ(nm.q[CG_RS_A].due_ms, t + 60000);
	CHECK_EQ(nm.addrs_ms, t);
	CHECK(cg_rs_has(&nm));
	cg_rs_addrs(&nm, &a);
	CHECK(a.n == 1 && a.a[0].ss_family == AF_INET);
	CHECK(cg_rs_holds(&nm, &(struct sockaddr_storage){ 0 }) == 0);
	{
		struct sockaddr_storage x = addr("192.0.2.10:7"), y = addr("192.0.2.11:7");

		CHECK(cg_rs_holds(&nm, &x)); /* the port aside */
		CHECK(!cg_rs_holds(&nm, &y));
	}
	/* TTL 0: no lookup before 30 s. */
	r = answer4(two, 2, 0);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 0, t + 1), 1);
	CHECK_EQ(nm.q[CG_RS_A].due_ms, t + 1 + CG_RS_TTL_MIN_MS);
	/* The same addresses in another order change nothing. */
	r = answer4(back, 2, 60);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 0, t + 2), 0);
	CHECK_EQ(nm.addrs_ms, t + 1);

	/* An empty answer while the server answers there keeps them... */
	r = empty(CG_DNS_NXDOMAIN);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 0, t + 3), 0);
	CHECK_EQ(nm.q[CG_RS_A].n, 2);
	CHECK_EQ(nm.q[CG_RS_A].outcome, CG_DNS_NXDOMAIN);
	CHECK_EQ(nm.q[CG_RS_A].due_ms, t + 3 + CG_RS_NEG_MS);
	r = empty(CG_DNS_NODATA);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 0, t + 4), 0);
	CHECK_EQ(nm.q[CG_RS_A].n, 2);
	/* ... and takes them away once the name is silent. */
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 1, t + 5), 1);
	CHECK_EQ(nm.q[CG_RS_A].n, 0);
	CHECK(!cg_rs_has(&nm));
	/* ... and a name left with no address is asked again soon, not in 5 minutes. */
	CHECK_EQ(nm.q[CG_RS_A].due_ms, t + 5 + CG_RS_EMPTY_MS);
	/* A new address replaces them all. */
	r = answer4(other, 1, 300);
	CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 1, t + 6), 1);
	CHECK(nm.q[CG_RS_A].n == 1 && nm.q[CG_RS_A].addr[0][3] == 30);
}

static void test_failures(void)
{
	struct cg_rsq q;
	struct cg_rsname nm;
	struct cg_dns_result r;
	int one[] = { 1 };
	uint32_t first[] = { 1000, 2000, 4000, 8000, 15000, 15000 }, after[] = { 5000, 10000, 20000, 40000, 60000, 60000 };
	uint64_t t = 5000;

	/* Attempts: at most CG_RS_ATTEMPTS, and no more than there are pairs. */
	memset(&q, 0, sizeof(q));
	cg_rs_start(&q, 1, t);
	CHECK_EQ(cg_rs_next(&q, CG_DNS_SERVFAIL, 3, 2, t), 1);
	CHECK(q.attempt == 1 && q.id == 2 && q.inflight);
	CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 3, 3, t), 1);
	CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 3, 4, t), 0); /* the lookup failed */
	CHECK(!q.inflight && q.failures == 1 && q.due_ms == t + 1000);
	memset(&q, 0, sizeof(q));
	cg_rs_start(&q, 1, t);
	for (int i = 1; i < CG_RS_ATTEMPTS; i++)
		CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 40, 9, t), 1);
	CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 40, 9, t), 0);
	CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 0, 9, t), 0); /* no pairs: over at once */

	/* Never answered: 1 s doubling up to 15 s. */
	memset(&q, 0, sizeof(q));
	for (int i = 0; i < 6; i++) {
		cg_rs_start(&q, 1, t);
		CHECK_EQ(cg_rs_next(&q, CG_DNS_IGNORE, 1, 1, t), 0);
		CHECK_EQ(q.due_ms - t, first[i]);
	}
	/* After an answer: 5 s doubling up to 60 s, and each answer starts it
	 * again. */
	memset(&nm, 0, sizeof(nm));
	r = answer4(one, 1, 60);
	for (int round = 0; round < 2; round++) {
		cg_rs_start(&nm.q[CG_RS_A], 1, t);
		cg_rs_answer(&nm, CG_RS_A, &r, 0, t);
		for (int i = 0; i < 6; i++) {
			cg_rs_start(&nm.q[CG_RS_A], 1, t);
			CHECK_EQ(cg_rs_next(&nm.q[CG_RS_A], CG_DNS_IGNORE, 1, 1, t), 0);
			CHECK_EQ(nm.q[CG_RS_A].due_ms - t, after[i]);
		}
		CHECK_EQ(nm.q[CG_RS_A].n, 1); /* the last good answer stays */
	}
}

static void test_silence(void)
{
	struct cg_rsname nm;
	struct cg_dns_result r;
	int one[] = { 1 };
	uint64_t t = 100000;

	memset(&nm, 0, sizeof(nm));
	CHECK(!cg_rs_silent(&nm, t)); /* nothing to be silent on */
	CHECK(!cg_rs_kick(&nm, t));
	r = answer4(one, 1, 600);
	cg_rs_start(&nm.q[CG_RS_A], 1, t);
	cg_rs_answer(&nm, CG_RS_A, &r, 0, t);
	nm.q[CG_RS_AAAA].due_ms = t + 600000;
	CHECK(!cg_rs_silent(&nm, t + CG_RS_SILENT_MS - 1));
	CHECK(cg_rs_silent(&nm, t + CG_RS_SILENT_MS));
	/* Live links keep it from being silent. */
	nm.live_ms = t + 5000;
	CHECK(!cg_rs_silent(&nm, t + 5000 + CG_RS_SILENT_MS - 1));
	CHECK(cg_rs_silent(&nm, t + 5000 + CG_RS_SILENT_MS));
	/* Silent: both questions due at once, then every 15 s at most. */
	t += 5000 + CG_RS_SILENT_MS;
	CHECK(!cg_rs_due(&nm.q[CG_RS_A], t));
	CHECK(cg_rs_kick(&nm, t));
	CHECK(cg_rs_due(&nm.q[CG_RS_A], t) && cg_rs_due(&nm.q[CG_RS_AAAA], t));
	cg_rs_start(&nm.q[CG_RS_A], 2, t);
	cg_rs_answer(&nm, CG_RS_A, &r, 1, t + 50); /* the same address */
	cg_rs_start(&nm.q[CG_RS_AAAA], 3, t);
	cg_rs_answer(&nm, CG_RS_AAAA, &(struct cg_dns_result){ .outcome = CG_DNS_NODATA }, 1, t + 50);
	CHECK(!cg_rs_kick(&nm, t + CG_RS_SILENT_EVERY_MS - 1));
	CHECK(!cg_rs_due(&nm.q[CG_RS_A], t + CG_RS_SILENT_EVERY_MS - 1));
	CHECK(cg_rs_kick(&nm, t + CG_RS_SILENT_EVERY_MS));
	CHECK(cg_rs_due(&nm.q[CG_RS_A], t + CG_RS_SILENT_EVERY_MS));
	/* A kick never cuts a lookup under way. */
	cg_rs_start(&nm.q[CG_RS_A], 4, t + CG_RS_SILENT_EVERY_MS);
	CHECK(cg_rs_kick(&nm, t + 2 * CG_RS_SILENT_EVERY_MS));
	CHECK(nm.q[CG_RS_A].inflight && nm.q[CG_RS_A].id == 4);
	/* New addresses end the episode: the next silence is looked up at once,
	 * not 15 s after the last kick. */
	{
		int two[] = { 2 };
		uint64_t u = t + 3 * CG_RS_SILENT_EVERY_MS;

		CHECK(nm.kick_ms != 0);
		r = answer4(two, 1, 600);
		CHECK_EQ(cg_rs_answer(&nm, CG_RS_A, &r, 1, u), 1);
		CHECK_EQ(nm.kick_ms, 0);
		CHECK(cg_rs_kick(&nm, u + CG_RS_SILENT_MS));
	}
}

static void test_pairs(void)
{
	uint8_t nres[] = { 2, 0, 3 };
	int p = -1, r = -1, want[][2] = { { 0, 0 }, { 2, 0 }, { 0, 1 }, { 2, 1 }, { 2, 2 } };

	/* The first resolver of every link, then the second of every link... */
	for (int k = 0; k < 5; k++) {
		CHECK_EQ(cg_rs_pair(nres, 3, k, &p, &r), 0);
		CHECK(p == want[k][0] && r == want[k][1]);
	}
	CHECK_EQ(cg_rs_pair(nres, 3, 5, &p, &r), -1);
	CHECK_EQ(cg_rs_npairs(nres, 3), 5);
	CHECK_EQ(cg_rs_pair(nres, 0, 0, &p, &r), -1);
	CHECK_EQ(cg_rs_npairs(nres, 0), 0);
}

static void test_hosts(void)
{
	static const char hosts[] = "# comment vps.example.com\n"
				    "127.0.0.1\tlocalhost\n"
				    "::1 localhost ip6-localhost\n"
				    "192.0.2.9 other VPS.Example.COM. # pinned\n"
				    "192.0.2.5 vps.example.com\n"
				    "fe80::1%eth0 vps.example.com\n"
				    "::ffff:192.0.2.7 vps.example.com\n"
				    "2001:db8::5 vps.example.com\n"
				    "notanaddress vps.example.com\n"
				    "192.0.2.5 vps.example.com\n"
				    "198.51.100.1 vps.example.com.evil\r\n"
				    "198.51.100.2 #vps.example.com\n"
				    "198.51.100.3";
	struct cg_nameaddrs a;
	char buf[64];

	CHECK_EQ(cg_hosts_find(hosts, "vps.example.com", &a), 1);
	CHECK_EQ(a.n, 4); /* IPv6 first, then IPv4 in order, each once */
	CHECK(!strcmp(cg_addr_str(&a.a[0], buf, sizeof(buf)), "[2001:db8::5]:0"));
	CHECK(!strcmp(cg_addr_str(&a.a[1], buf, sizeof(buf)), "192.0.2.5:0"));
	CHECK(!strcmp(cg_addr_str(&a.a[2], buf, sizeof(buf)), "192.0.2.7:0"));
	CHECK(!strcmp(cg_addr_str(&a.a[3], buf, sizeof(buf)), "192.0.2.9:0"));
	CHECK_EQ(cg_hosts_find(hosts, "localhost", &a), 1);
	CHECK_EQ(a.n, 2);
	CHECK_EQ(cg_hosts_find(hosts, "example.com", &a), 0);
	CHECK_EQ(a.n, 0);
	CHECK_EQ(cg_hosts_find(NULL, "localhost", &a), 0);
	CHECK_EQ(cg_hosts_find("", "localhost", &a), 0);
}

static void test_build(void)
{
	struct cg_srvent e[CG_MAX_SERVERS];
	struct cg_nameaddrs big, none = { 0 };
	const struct cg_nameaddrs *names[1];
	struct sockaddr_storage out[CG_MAX_CANDS];
	uint8_t en[CG_MAX_SERVERS];
	char buf[64];
	int n;

	/* A name without addresses yet gives nothing; the address entries
	 * keep their place. */
	memset(e, 0, sizeof(e));
	e[0].name = 0;
	e[0].port = 59402;
	e[1].a = addr("192.0.2.1:7");
	e[1].name = -1;
	names[0] = &none;
	n = cg_srv_build(e, 2, names, out, en);
	CHECK(n == 1 && en[0] == 0 && en[1] == 1);
	names[0] = NULL;
	CHECK_EQ(cg_srv_build(e, 2, names, out, en), 1);

	/* A name's addresses take the entry's port, at most CG_NAME_ADDRS of
	 * each family. */
	memset(&big, 0, sizeof(big));
	for (int i = 0; i < 6; i++) {
		char s[64];

		snprintf(s, sizeof(s), "[2001:db8::%d]:1", i + 1);
		big.a[big.n++] = addr(s);
	}
	big.a[big.n++] = addr("198.51.100.1:1");
	names[0] = &big;
	n = cg_srv_build(e, 2, names, out, en);
	CHECK(n == 6 && en[0] == 5 && en[1] == 1);
	CHECK(!strcmp(cg_addr_str(&out[0], buf, sizeof(buf)), "[2001:db8::1]:59402"));
	CHECK(!strcmp(cg_addr_str(&out[4], buf, sizeof(buf)), "198.51.100.1:59402"));
	CHECK(!strcmp(cg_addr_str(&out[5], buf, sizeof(buf)), "192.0.2.1:7"));

	/* Eight names of 8 addresses: 32 slots, one kept for each later
	 * entry, so the last entries still give one each. */
	for (int i = 0; i < CG_MAX_SERVERS; i++) {
		e[i].a.ss_family = AF_UNSPEC;
		e[i].name = 0;
		e[i].port = 1;
	}
	{
		struct cg_nameaddrs eight = big;

		eight.n = 0;
		for (int i = 0; i < 4; i++) {
			char s[64];

			snprintf(s, sizeof(s), "[2001:db8::%d]:1", i + 1);
			eight.a[eight.n++] = addr(s);
			snprintf(s, sizeof(s), "192.0.2.%d:1", i + 1);
			eight.a[eight.n++] = addr(s);
		}
		names[0] = &eight;
		n = cg_srv_build(e, CG_MAX_SERVERS, names, out, en);
		CHECK_EQ(n, CG_MAX_CANDS);
		CHECK(en[0] == 8 && en[1] == 8 && en[2] == 8 && en[3] == 4);
		for (int i = 4; i < CG_MAX_SERVERS; i++)
			CHECK_EQ(en[i], 1);
	}
}

void test_resolve(void)
{
	test_ttl_and_sort();
	test_answers();
	test_failures();
	test_silence();
	test_pairs();
	test_hosts();
	test_build();
}
