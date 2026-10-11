/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "dns.h"
#include "test.h"

#define ID 0x5a17
#define QR 0x8000
#define TC 0x0200
#define RD 0x0100
#define RA 0x0080
#define CH 3    /* class CHAOS */
#define TXT 16
#define Q "vps.example.com" /* the question; at offset 12, "example.com" at 16 */

static uint64_t xs(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* A message being built. */
struct msg {
	uint8_t b[4096];
	size_t n;
};

static void put8(struct msg *m, unsigned v)
{
	if (m->n >= sizeof(m->b))
		abort();
	m->b[m->n++] = (uint8_t)v;
}

static void put16(struct msg *m, unsigned v)
{
	put8(m, v >> 8 & 0xff);
	put8(m, v & 0xff);
}

static void put32(struct msg *m, uint32_t v)
{
	put16(m, v >> 16);
	put16(m, v & 0xffff);
}

static void set16(struct msg *m, size_t at, size_t v)
{
	m->b[at] = (uint8_t)(v >> 8);
	m->b[at + 1] = (uint8_t)v;
}

/* Appends a name: dotted labels, then "@N" for a pointer to offset N or
 * else the root: "www.example.com", "vps@16", "@12", "" (the root alone).
 * Returns where it starts. */
static size_t name(struct msg *m, const char *s)
{
	size_t at = m->n;

	while (*s && *s != '@') {
		size_t l = strcspn(s, ".@");

		put8(m, (unsigned)l);
		for (size_t i = 0; i < l; i++)
			put8(m, (uint8_t)s[i]);
		s += l;
		if (*s == '.')
			s++;
	}
	if (*s == '@')
		put16(m, 0xc000u | (unsigned)atoi(s + 1));
	else
		put8(m, 0);
	return at;
}

/* "LABELS@OFF" into buf. */
static const char *ptr(char *buf, size_t cap, const char *labels, size_t off)
{
	snprintf(buf, cap, "%s@%zu", labels, off);
	return buf;
}

static void hdr(struct msg *m, unsigned id, unsigned flags, unsigned qd, unsigned an)
{
	m->n = 0;
	put16(m, id);
	put16(m, flags);
	put16(m, qd);
	put16(m, an);
	put16(m, 0);
	put16(m, 0);
}

static void question(struct msg *m, const char *qname, unsigned qtype, unsigned qclass)
{
	name(m, qname);
	put16(m, qtype);
	put16(m, qclass);
}

/* A response to qname, qtype with flags (TC, an rcode) and an records. */
static void answer(struct msg *m, unsigned flags, unsigned an, const char *qname, unsigned qtype)
{
	hdr(m, ID, QR | RD | RA | flags, 1, an);
	question(m, qname, qtype, CG_DNS_IN);
}

/* A record up to its rdlength, 0 for now: returns where that is (the rdata
 * follows it), for rdend. */
static size_t rrhead(struct msg *m, const char *owner, unsigned type, unsigned cls, uint32_t ttl)
{
	name(m, owner);
	put16(m, type);
	put16(m, cls);
	put32(m, ttl);
	put16(m, 0);
	return m->n - 2;
}

static void rdend(struct msg *m, size_t at)
{
	set16(m, at, m->n - at - 2);
}

static void raw(struct msg *m, const char *owner, unsigned type, unsigned cls, uint32_t ttl, const void *d,
		size_t len)
{
	size_t at = rrhead(m, owner, type, cls, ttl);

	for (size_t i = 0; i < len; i++)
		put8(m, ((const uint8_t *)d)[i]);
	rdend(m, at);
}

/* An A or AAAA record, by the address's family, of class cls. */
static void addr_cls(struct msg *m, const char *owner, unsigned cls, uint32_t ttl, const char *ip)
{
	uint8_t a[16];

	if (inet_pton(AF_INET, ip, a) == 1)
		raw(m, owner, CG_DNS_A, cls, ttl, a, 4);
	else if (inet_pton(AF_INET6, ip, a) == 1)
		raw(m, owner, CG_DNS_AAAA, cls, ttl, a, 16);
	else
		abort();
}

static void addr(struct msg *m, const char *owner, uint32_t ttl, const char *ip)
{
	addr_cls(m, owner, CG_DNS_IN, ttl, ip);
}

/* A CNAME record: returns where its target starts. */
static size_t cname(struct msg *m, const char *owner, uint32_t ttl, const char *target)
{
	size_t at = rrhead(m, owner, CG_DNS_CNAME, CG_DNS_IN, ttl), t = name(m, target);

	rdend(m, at);
	return t;
}

/* Parses b from a heap copy of exactly n octets, so that ASan sees any read
 * past the end. */
static int parse_as(const uint8_t *b, size_t n, uint16_t id, const char *qname, uint16_t qtype,
		    struct cg_dns_result *r)
{
	uint8_t *copy = malloc(n ? n : 1);

	if (!copy)
		abort();
	memcpy(copy, b, n);
	cg_dns_parse(copy, n, id, qname, qtype, r);
	free(copy);
	return r->outcome;
}

static int parse(const struct msg *m, uint16_t qtype, struct cg_dns_result *r)
{
	return parse_as(m->b, m->n, ID, Q, qtype, r);
}

static int has(const struct cg_dns_result *r, const char *ip)
{
	uint8_t a[16] = { 0 };

	if (inet_pton(AF_INET, ip, a) != 1 && inet_pton(AF_INET6, ip, a) != 1)
		abort();
	for (int i = 0; i < r->n; i++)
		if (!memcmp(r->addr[i], a, 16))
			return 1;
	return 0;
}

/* The i-th name along a chain: Q, then h1.example, h2.example... */
static const char *hop(char *buf, size_t cap, int i)
{
	if (!i)
		snprintf(buf, cap, "%s", Q);
	else
		snprintf(buf, cap, "h%d.example", i);
	return buf;
}

/* An answer for Q through k CNAMEs to one A record, in order or reversed. */
static void chain(struct msg *m, int k, int reversed)
{
	char o[32], t[32];

	answer(m, 0, (unsigned)k + 1, Q, CG_DNS_A);
	for (int j = 0; j <= k; j++) {
		int i = reversed ? k - j : j;

		if (i < k)
			cname(m, hop(o, sizeof(o), i), 60, hop(t, sizeof(t), i + 1));
		else
			addr(m, hop(o, sizeof(o), i), 60, "198.51.100.7");
	}
}

static void fill(char *s, size_t n, char c)
{
	memset(s, c, n);
	s[n] = 0;
}

static int norm_is(const char *in, const char *want)
{
	char out[CG_DNS_NAME_MAX + 1];

	return !cg_dns_name_norm(in, out, sizeof(out)) && !strcmp(out, want);
}

static int norm_bad(const char *in)
{
	char out[CG_DNS_NAME_MAX + 1];

	return cg_dns_name_norm(in, out, sizeof(out)) == -1;
}

static void name_norm(void)
{
	char l63[64], l64[65], l61[62], l62[63], s[300], out[CG_DNS_NAME_MAX + 1];

	CHECK(norm_is("vps.example.com", "vps.example.com"));
	CHECK(norm_is("vps.example.com.", "vps.example.com"));
	CHECK(norm_is("VPS.Example.COM.", "vps.example.com"));
	CHECK(norm_is("localhost", "localhost"));
	CHECK(norm_is("_srv.x", "_srv.x"));
	CHECK(norm_is("a-b.c_d.e9", "a-b.c_d.e9"));
	CHECK(norm_is("123.example", "123.example")); /* digits, but not the last label */
	CHECK(norm_is("1.2.3.4a", "1.2.3.4a"));
	CHECK(norm_is("a", "a"));

	CHECK(norm_bad(""));
	CHECK(norm_bad("."));
	CHECK(norm_bad(".."));
	CHECK(norm_bad("a..b"));
	CHECK(norm_bad("a.."));
	CHECK(norm_bad(".a"));
	CHECK(norm_bad("a b"));
	CHECK(norm_bad("a\tb"));
	CHECK(norm_bad("a/b"));
	CHECK(norm_bad("a:53"));
	CHECK(norm_bad("::1"));
	CHECK(norm_bad("[2001:db8::1]"));
	CHECK(norm_bad("caf\xc3\xa9.example"));
	/* IPv4-looking: the last label all digits. */
	CHECK(norm_bad("10.0.0.256"));
	CHECK(norm_bad("1.2.3.4"));
	CHECK(norm_bad("1.2.3.4."));
	CHECK(norm_bad("127.1"));
	CHECK(norm_bad("0"));
	CHECK(norm_bad("4294967295"));
	CHECK(norm_bad("a.123"));

	/* Labels of 63, not 64. */
	fill(l63, 63, 'a');
	fill(l64, 64, 'a');
	fill(l61, 61, 'c');
	fill(l62, 62, 'c');
	CHECK(norm_is(l63, l63));
	CHECK(norm_bad(l64));
	snprintf(s, sizeof(s), "x.%s.y", l64);
	CHECK(norm_bad(s));

	/* 253 characters, not 254; a trailing dot does not count. */
	snprintf(s, sizeof(s), "%s.%s.%s.%s", l63, l63, l63, l61);
	CHECK_EQ(strlen(s), 253);
	CHECK(norm_is(s, s));
	strcat(s, ".");
	CHECK(!cg_dns_name_norm(s, out, sizeof(out)) && strlen(out) == 253);
	snprintf(s, sizeof(s), "%s.%s.%s.%s", l63, l63, l63, l62);
	CHECK(norm_bad(s));
	strcat(s, ".");
	CHECK(norm_bad(s));

	/* out must hold the name and its terminator. */
	CHECK(!cg_dns_name_norm("Abc.D.", out, 6) && !strcmp(out, "abc.d"));
	CHECK_EQ(cg_dns_name_norm("abc.d", out, 5), -1);
	CHECK_EQ(cg_dns_name_norm("a", out, 0), -1);
}

static void query(void)
{
	static const uint8_t want_a[] = {
		0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		3, 'v', 'p', 's', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
		0x00, 0x01, 0x00, 0x01,
	};
	static const uint8_t want_aaaa[] = {
		0xfe, 0xdc, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0,
		0x00, 0x1c, 0x00, 0x01,
	};
	uint8_t buf[CG_DNS_QUERY_MAX + 8];
	char l63[64], l61[62], s[300];
	int untouched = 1;

	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 0x1234, Q, CG_DNS_A), sizeof(want_a));
	CHECK(!memcmp(buf, want_a, sizeof(want_a)));
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 0xfedc, "localhost", CG_DNS_AAAA), sizeof(want_aaaa));
	CHECK(!memcmp(buf, want_aaaa, sizeof(want_aaaa)));

	/* cap: exactly the query, not one octet less (nothing written then). */
	CHECK_EQ(cg_dns_query(buf, sizeof(want_a), 0x1234, Q, CG_DNS_A), sizeof(want_a));
	memset(buf, 0xee, sizeof(buf));
	CHECK_EQ(cg_dns_query(buf, sizeof(want_a) - 1, 0x1234, Q, CG_DNS_A), -1);
	for (size_t i = 0; i < sizeof(buf); i++)
		untouched &= buf[i] == 0xee;
	CHECK(untouched);

	/* The longest name gives CG_DNS_QUERY_MAX. */
	fill(l63, 63, 'a');
	fill(l61, 61, 'b');
	snprintf(s, sizeof(s), "%s.%s.%s.%s", l63, l63, l63, l61);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, s, CG_DNS_A), CG_DNS_QUERY_MAX);
	CHECK_EQ(buf[12], 63);
	CHECK_EQ(buf[12 + 3 * 64], 61);
	CHECK_EQ(buf[12 + 254], 0);
	CHECK_EQ(cg_dns_query(buf, CG_DNS_QUERY_MAX - 1, 1, s, CG_DNS_A), -1);

	/* Only a normalized, valid name, and only A or AAAA. */
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, "Vps.example.com", CG_DNS_A), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, "vps.example.com.", CG_DNS_A), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, "", CG_DNS_A), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, "a..b", CG_DNS_A), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, "10.0.0.1", CG_DNS_A), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, Q, TXT), -1);
	CHECK_EQ(cg_dns_query(buf, sizeof(buf), 1, Q, CG_DNS_CNAME), -1);

	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_IGNORE), ""));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_ADDRS), ""));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_NODATA), "nodata"));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_NXDOMAIN), "nxdomain"));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_SERVFAIL), "servfail"));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_REFUSED), "refused"));
	CHECK(!strcmp(cg_dns_outcome_name(CG_DNS_MALFORMED), "malformed"));
	CHECK(!strcmp(cg_dns_outcome_name(99), ""));
}

static void plain(void)
{
	static const uint8_t zero[12];
	struct cg_dns_result r;
	struct msg m;
	uint8_t q[CG_DNS_QUERY_MAX];
	int qn;

	/* A, the owner compressed to the question. */
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@12", 300, "203.0.113.5");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "203.0.113.5"));
	CHECK(!memcmp(r.addr[0] + 4, zero, 12));
	CHECK_EQ(r.ttl, 300);
	CHECK_EQ(r.rcode, 0);
	CHECK_EQ(r.tc, 0);
	CHECK_EQ(parse(&m, CG_DNS_AAAA, &r), CG_DNS_MALFORMED); /* another question */
	CHECK_EQ(r.n, 0);

	/* Not ours: another id, not a response, shorter than a header. */
	CHECK_EQ(parse_as(m.b, m.n, ID + 1, Q, CG_DNS_A, &r), CG_DNS_IGNORE);
	CHECK(r.n == 0 && r.rcode == 0 && r.ttl == 0);
	for (size_t len = 0; len < 12; len++)
		CHECK_EQ(parse_as(m.b, len, ID, Q, CG_DNS_A, &r), CG_DNS_IGNORE);
	CHECK_EQ(parse_as(m.b, 12, ID, Q, CG_DNS_A, &r), CG_DNS_MALFORMED); /* a header only */
	m.b[2] &= 0x7f;
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_IGNORE);
	qn = cg_dns_query(q, sizeof(q), ID, Q, CG_DNS_A); /* our own query, looped back */
	CHECK_EQ(parse_as(q, (size_t)qn, ID, Q, CG_DNS_A, &r), CG_DNS_IGNORE);
	m.b[2] |= 0x80 | 0x08; /* opcode 1 */
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* Owner and question in another case; the owner uncompressed. */
	answer(&m, 0, 1, "VPS.Example.COM", CG_DNS_A);
	addr(&m, "vps.EXAMPLE.com", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK(has(&r, "192.0.2.1"));

	/* A pointer to a suffix of the question. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	addr(&m, "vps@16", 60, "192.0.2.1");
	addr(&m, "www@16", 60, "192.0.2.2"); /* www.example.com: not the name */
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "192.0.2.1"));

	/* AAAA; an A record in it is of another type. */
	answer(&m, 0, 2, Q, CG_DNS_AAAA);
	addr(&m, "@12", 60, "203.0.113.5");
	addr(&m, "@12", 45, "2001:db8::5");
	CHECK_EQ(parse(&m, CG_DNS_AAAA, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "2001:db8::5"));
	CHECK_EQ(r.ttl, 45);

	/* NOERROR without an address of the type. */
	answer(&m, 0, 1, Q, CG_DNS_AAAA);
	addr(&m, "@12", 60, "203.0.113.5");
	CHECK_EQ(parse(&m, CG_DNS_AAAA, &r), CG_DNS_NODATA);
	CHECK(r.n == 0 && r.ttl == 0);
	answer(&m, 0, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NODATA);

	/* A name or qtype the parser cannot match. */
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@12", 60, "203.0.113.5");
	CHECK_EQ(parse_as(m.b, m.n, ID, "VPS.example.com", CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(parse_as(m.b, m.n, ID, "a..b", CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(parse_as(m.b, m.n, ID, Q, TXT, &r), CG_DNS_MALFORMED);
}

static void cnames(void)
{
	struct cg_dns_result r;
	struct msg m;
	char o[64], t[64];
	size_t rd, rd2;

	/* In order, each compressed into the one before. */
	answer(&m, 0, 3, Q, CG_DNS_A);
	rd = cname(&m, "@12", 600, "edge.cdn.net");
	rd2 = cname(&m, ptr(o, sizeof(o), "", rd), 120, ptr(t, sizeof(t), "x1", rd + 5));
	addr(&m, ptr(o, sizeof(o), "", rd2), 300, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "198.51.100.7"));
	CHECK_EQ(r.ttl, 120);

	/* Out of order; an address of the name itself next to its CNAME is not
	 * the end of the chain. */
	answer(&m, 0, 4, Q, CG_DNS_A);
	addr(&m, "x1.cdn.net", 300, "198.51.100.7");
	cname(&m, "edge.cdn.net", 120, "x1.cdn.net");
	addr(&m, Q, 30, "192.0.2.99");
	cname(&m, "@12", 600, "edge.cdn.net");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "198.51.100.7"));
	CHECK(!has(&r, "192.0.2.99"));
	CHECK_EQ(r.ttl, 120);

	/* To a name without addresses. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	cname(&m, "@12", 600, "edge.cdn.net");
	addr(&m, "other.cdn.net", 300, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NODATA);

	/* A CNAME of another class is not followed. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	rd = rrhead(&m, "@12", CG_DNS_CNAME, CH, 60);
	name(&m, "edge.cdn.net");
	rdend(&m, rd);
	addr(&m, "edge.cdn.net", 300, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NODATA);

	/* Loops: two names, and one to itself. */
	answer(&m, 0, 3, Q, CG_DNS_A);
	cname(&m, "@12", 60, "b.example");
	cname(&m, "b.example", 60, Q);
	addr(&m, "b.example", 60, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(r.n, 0);
	answer(&m, 0, 2, Q, CG_DNS_A);
	cname(&m, "@12", 60, "@12");
	addr(&m, "@12", 60, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* 8 hops, not 9, in either order. */
	for (int rev = 0; rev < 2; rev++) {
		chain(&m, 8, rev);
		CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
		CHECK(has(&r, "198.51.100.7"));
		chain(&m, 9, rev);
		CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	}

	/* The target fills the rdata: no octet after it, none past it. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	rd = rrhead(&m, "@12", CG_DNS_CNAME, CG_DNS_IN, 60);
	name(&m, "x.example");
	put8(&m, 0);
	rdend(&m, rd);
	addr(&m, "x.example", 60, "198.51.100.7");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 1, Q, CG_DNS_A);
	rd = rrhead(&m, "@12", CG_DNS_CNAME, CG_DNS_IN, 60);
	name(&m, "x.example");
	set16(&m, rd, 4); /* the rest is past the answer section */
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	set16(&m, rd, 11);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NODATA);
}

static void rcodes(void)
{
	struct cg_dns_result r;
	struct msg m;

	answer(&m, 3, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NXDOMAIN);
	CHECK(r.rcode == 3 && r.n == 0);
	answer(&m, 3, 1, Q, CG_DNS_A); /* with the CNAME to the missing name */
	cname(&m, "@12", 60, "gone.example");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NXDOMAIN);
	answer(&m, 2, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_SERVFAIL);
	CHECK_EQ(r.rcode, 2);
	answer(&m, 4, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_SERVFAIL);
	CHECK_EQ(r.rcode, 4);
	answer(&m, 5, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_REFUSED);
	CHECK_EQ(r.rcode, 5);

	/* An error without the question; NOERROR needs it. */
	hdr(&m, ID, QR | 5, 0, 0);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_REFUSED);
	hdr(&m, ID, QR | 3, 0, 0);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NXDOMAIN);
	hdr(&m, ID, QR, 0, 0);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(r.rcode, 0);

	/* An error with another question, or two. */
	answer(&m, 3, 0, "vps.example.org", CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(r.rcode, 3);
	hdr(&m, ID, QR | 5, 2, 0);
	question(&m, Q, CG_DNS_A, CG_DNS_IN);
	question(&m, Q, CG_DNS_AAAA, CG_DNS_IN);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
}

static void questions(void)
{
	struct cg_dns_result r;
	struct msg m;
	size_t full;

	answer(&m, 0, 1, "vps.example.org", CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 0, "vps.example", CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 0, "vps.example.com.x", CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 0, Q, CG_DNS_AAAA);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	hdr(&m, ID, QR, 1, 0);
	question(&m, Q, CG_DNS_A, CH);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	hdr(&m, ID, QR, 2, 0);
	question(&m, Q, CG_DNS_A, CG_DNS_IN);
	question(&m, Q, CG_DNS_A, CG_DNS_IN);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* A question cut anywhere, also with TC. */
	answer(&m, TC, 0, Q, CG_DNS_A);
	full = m.n;
	for (size_t len = 12; len < full; len++)
		CHECK_EQ(parse_as(m.b, len, ID, Q, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(parse_as(m.b, full, ID, Q, CG_DNS_A, &r), CG_DNS_MALFORMED); /* whole, TC, no address */
	m.b[2] &= (uint8_t)~(TC >> 8);
	CHECK_EQ(parse_as(m.b, full, ID, Q, CG_DNS_A, &r), CG_DNS_NODATA);

	/* A pointer in the question: before it is only the header. */
	hdr(&m, ID, QR, 1, 0);
	question(&m, "@12", CG_DNS_A, CG_DNS_IN);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	hdr(&m, ID, QR, 1, 0);
	question(&m, "vps@2", CG_DNS_A, CG_DNS_IN);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
}

static void records(void)
{
	static const uint8_t three[3] = { 192, 0, 2 }, txt[4] = { 3, 'a', 'b', 'c' };
	struct cg_dns_result r;
	struct msg m;
	char ip[32];

	/* Other names, classes and types are skipped, also malformed for an
	 * address of ours. */
	answer(&m, 0, 6, Q, CG_DNS_A);
	addr(&m, "www.example.com", 60, "192.0.2.1");
	addr_cls(&m, "@12", CH, 60, "192.0.2.2");
	raw(&m, "@12", TXT, CG_DNS_IN, 60, txt, sizeof(txt));
	raw(&m, "other.example", CG_DNS_A, CG_DNS_IN, 60, three, sizeof(three));
	raw(&m, "@12", CG_DNS_A, CH, 60, three, sizeof(three));
	addr(&m, "@12", 60, "192.0.2.3");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(has(&r, "192.0.2.3"));

	/* An address of ours of the wrong length; nothing kept before it
	 * stays. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.3");
	raw(&m, "@12", CG_DNS_A, CG_DNS_IN, 60, three, sizeof(three));
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK(r.n == 0 && r.ttl == 0 && r.addr[0][0] == 0);
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@12", 60, "2001:db8::1"); /* type AAAA: skipped */
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_NODATA);
	answer(&m, 0, 1, Q, CG_DNS_AAAA);
	raw(&m, "@12", CG_DNS_AAAA, CG_DNS_IN, 60, three, sizeof(three));
	CHECK_EQ(parse(&m, CG_DNS_AAAA, &r), CG_DNS_MALFORMED);

	/* 20 addresses: the first 16 kept, the TTL of those only. */
	answer(&m, 0, 20, Q, CG_DNS_A);
	for (int i = 1; i <= 20; i++) {
		snprintf(ip, sizeof(ip), "10.0.0.%d", i);
		addr(&m, "@12", i <= 16 ? 300 : 5, ip);
	}
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, CG_DNS_MAX_ADDRS);
	CHECK(has(&r, "10.0.0.1") && has(&r, "10.0.0.16") && !has(&r, "10.0.0.17"));
	CHECK(r.addr[15][3] == 16);
	CHECK_EQ(r.ttl, 300);
	/* A repeat of a kept one after them counts for the TTL. */
	addr(&m, "@12", 7, "10.0.0.3");
	set16(&m, 6, 21);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, CG_DNS_MAX_ADDRS);
	CHECK_EQ(r.ttl, 7);

	/* Repeats kept once. */
	answer(&m, 0, 4, Q, CG_DNS_A);
	addr(&m, "@12", 300, "192.0.2.1");
	addr(&m, "@12", 300, "192.0.2.2");
	addr(&m, "@12", 100, "192.0.2.1");
	addr(&m, "@12", 300, "192.0.2.2");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 2);
	CHECK_EQ(r.ttl, 100);

	/* TTL: the smallest along the chain; the high bit set counts as 0. */
	answer(&m, 0, 3, Q, CG_DNS_A);
	cname(&m, "@12", 600, "x.example");
	addr(&m, "x.example", 90, "192.0.2.1");
	addr(&m, "x.example", 45, "192.0.2.2");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.ttl, 45);
	answer(&m, 0, 2, Q, CG_DNS_A);
	cname(&m, "@12", 0x80000000u, "x.example");
	addr(&m, "x.example", 300, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.ttl, 0);
	answer(&m, 0, 2, Q, CG_DNS_A);
	addr(&m, "@12", 300, "192.0.2.1");
	addr(&m, "@12", 0xffffffffu, "192.0.2.2");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.ttl, 0);
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@12", 0x7fffffffu, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.ttl, 0x7fffffffu);

	/* The authority and additional sections are never read. */
	answer(&m, 0, 1, Q, CG_DNS_A);
	set16(&m, 8, 1);
	set16(&m, 10, 2);
	addr(&m, "@12", 60, "192.0.2.1");
	addr(&m, "@12", 60, "192.0.2.2");
	put8(&m, 0xc0);
	put8(&m, 0xff);
	put8(&m, 0x41);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
	CHECK(!has(&r, "192.0.2.2"));
}

static void truncated(void)
{
	struct cg_dns_result r;
	struct msg m;
	size_t mark;

	/* TC: whole, with addresses or without. */
	answer(&m, TC, 1, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK(r.tc == 1 && r.n == 1);
	answer(&m, TC, 0, Q, CG_DNS_A);
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	CHECK_EQ(r.tc, 1);
	answer(&m, TC, 1, Q, CG_DNS_A);
	addr(&m, "other.example", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* ANCOUNT beyond the message. */
	answer(&m, 0, 3, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	m.b[2] |= TC >> 8;
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);

	/* Cut anywhere in the second record: what came whole counts with TC. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	mark = m.n;
	addr(&m, "@12", 30, "192.0.2.2");
	for (size_t len = mark; len < m.n; len++) {
		CHECK_EQ(parse_as(m.b, len, ID, Q, CG_DNS_A, &r), CG_DNS_MALFORMED);
		m.b[2] |= TC >> 8;
		CHECK_EQ(parse_as(m.b, len, ID, Q, CG_DNS_A, &r), CG_DNS_ADDRS);
		CHECK(r.n == 1 && r.ttl == 60);
		m.b[2] &= (uint8_t)~(TC >> 8);
	}
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK(r.n == 2 && r.ttl == 30);

	/* A record cut in its owner, uncompressed. */
	answer(&m, TC, 2, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	mark = m.n;
	addr(&m, "a.long.owner.name.example", 60, "192.0.2.2");
	CHECK_EQ(parse_as(m.b, mark + 7, ID, Q, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 1);
}

static void pointers(void)
{
	static const uint8_t lbl40[] = { 0x41, 'x', 0 }, lbl80[] = { 0x81, 'x', 0 };
	char l63[64], l61[62], l62[63], o[128];
	struct cg_dns_result r;
	struct msg m;
	size_t at, start, o1, o2, o3, prev;

	/* An owner pointing at itself. */
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, ptr(o, sizeof(o), "", m.n), 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, ptr(o, sizeof(o), "x", m.n), 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* Two pointers in a cycle: a record's data points at the next owner,
	 * which points back at it. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	at = rrhead(&m, "@12", TXT, CG_DNS_IN, 60);
	put16(&m, 0xc000u | (unsigned)(at + 4));
	rdend(&m, at);
	addr(&m, ptr(o, sizeof(o), "", at + 2), 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* Forward: at a name further on, inside the message. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	start = m.n;
	addr(&m, ptr(o, sizeof(o), "", start + 16), 60, "192.0.2.1");
	CHECK_EQ(m.n, start + 16);
	addr(&m, Q, 60, "192.0.2.2");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	/* ...and the same name backwards is fine. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	start = m.n;
	addr(&m, Q, 60, "192.0.2.1");
	addr(&m, ptr(o, sizeof(o), "", start), 60, "192.0.2.2");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	CHECK_EQ(r.n, 2);

	/* Beyond the message, or into the header. */
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@4000", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@11", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 1, Q, CG_DNS_A);
	addr(&m, "@0", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);

	/* Label types 0x40 and 0x80, in an owner or in the question. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	addr(&m, "@12", 60, "192.0.2.1");
	at = m.n;
	addr(&m, "y", 60, "192.0.2.2");
	memcpy(m.b + at, lbl40, sizeof(lbl40));
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	memcpy(m.b + at, lbl80, sizeof(lbl80));
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	answer(&m, 0, 0, Q, CG_DNS_A);
	m.b[12] = 0x43;
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	m.b[12] = 0x83;
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	/* ...also as the last octets of a truncated answer: malformed, not cut
	 * (read as a length, it would run past the end). */
	for (int t = 0; t < 2; t++) {
		answer(&m, TC, 2, Q, CG_DNS_A);
		addr(&m, "@12", 60, "192.0.2.1");
		put8(&m, t ? 0x81 : 0x41);
		put8(&m, 'x');
		CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
		m.b[m.n - 2] = 0x3f; /* a plain label cut there is truncation */
		CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);
	}

	/* A name a pointer leads to never reaches the end of a truncated
	 * message: it was whole before the pointer. Here the pointer is at an
	 * address that reads as a label of 63. */
	answer(&m, TC, 2, Q, CG_DNS_A);
	at = m.n + 2 + 10;
	addr(&m, "@12", 60, "63.0.2.1");
	name(&m, ptr(o, sizeof(o), "", at));
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_MALFORMED);
	/* A bad name inside data that is never read as one is no concern. */
	answer(&m, 0, 2, Q, CG_DNS_A);
	raw(&m, "@12", TXT, CG_DNS_IN, 60, lbl40, sizeof(lbl40));
	addr(&m, "@12", 60, "192.0.2.1");
	CHECK_EQ(parse(&m, CG_DNS_A, &r), CG_DNS_ADDRS);

	/* Names built from pointers: 255 octets, not 256. */
	fill(l63, 63, 'a');
	fill(l61, 61, 'b');
	fill(l62, 62, 'b');
	for (int over = 0; over < 2; over++) {
		answer(&m, 0, 5, Q, CG_DNS_A);
		o1 = m.n;
		raw(&m, l63, TXT, CG_DNS_IN, 60, NULL, 0); /* 65 */
		o2 = m.n;
		raw(&m, ptr(o, sizeof(o), l63, o1), TXT, CG_DNS_IN, 60, NULL, 0); /* 129 */
		o3 = m.n;
		raw(&m, ptr(o, sizeof(o), l63, o2), TXT, CG_DNS_IN, 60, NULL, 0); /* 193 */
		raw(&m, ptr(o, sizeof(o), over ? l62 : l61, o3), TXT, CG_DNS_IN, 60, NULL, 0);
		addr(&m, "@12", 60, "192.0.2.1");
		CHECK_EQ(parse(&m, CG_DNS_A, &r), over ? CG_DNS_MALFORMED : CG_DNS_ADDRS);
	}

	/* 128 pointers in a name, not 129: a run of pointers, each to the one
	 * before, the first to the question. */
	for (int over = 0; over < 2; over++) {
		int k = 126 + over;

		answer(&m, 0, 2, Q, CG_DNS_A);
		at = rrhead(&m, "@12", TXT, CG_DNS_IN, 60);
		prev = 12;
		for (int i = 0; i <= k; i++) {
			size_t here = m.n;

			put16(&m, 0xc000u | (unsigned)prev);
			prev = here;
		}
		rdend(&m, at);
		addr(&m, ptr(o, sizeof(o), "", prev), 60, "192.0.2.1"); /* k + 2 pointers */
		CHECK_EQ(parse(&m, CG_DNS_A, &r), over ? CG_DNS_MALFORMED : CG_DNS_ADDRS);
	}
}

/* The i-th name along a chain as an owner, at random: compressed to the
 * question, or in another case. */
static const char *hop_owner(char *buf, size_t cap, int i, uint64_t *s)
{
	hop(buf, cap, i);
	if (!i && xs(s) & 1)
		snprintf(buf, cap, "@12");
	else if (xs(s) & 1)
		buf[0] = (char)(buf[0] - 'a' + 'A');
	return buf;
}

/* Well-formed answers in random order: the chain followed, the first 16
 * addresses kept in the order they come, the TTL of those. */
static void shuffled(void)
{
	uint64_t s = 0x2545f4914f6cdd1dULL;
	int bad = 0;

	for (int it = 0; it < 3000; it++) {
		int hops = (int)(xs(&s) % 9), naddr = 1 + (int)(xs(&s) % 20), ndecoy = (int)(xs(&s) % 4);
		int nrec = hops + naddr + ndecoy, order[40], kept = 0;
		uint16_t qtype = xs(&s) & 1 ? CG_DNS_A : CG_DNS_AAAA;
		uint32_t ttl = UINT32_MAX;
		struct cg_dns_result r;
		char o[64], t[64], ip[64];
		struct msg m;

		for (int i = 0; i < nrec; i++)
			order[i] = i;
		for (int i = nrec - 1; i > 0; i--) {
			int j = (int)(xs(&s) % (uint64_t)(i + 1)), x = order[i];

			order[i] = order[j];
			order[j] = x;
		}
		answer(&m, 0, (unsigned)nrec, Q, qtype);
		for (int j = 0; j < nrec; j++) {
			int i = order[j];
			uint32_t rt = 1 + (uint32_t)(xs(&s) % 100000);

			if (i < hops) {
				cname(&m, hop_owner(o, sizeof(o), i, &s), rt, hop(t, sizeof(t), i + 1));
				ttl = rt < ttl ? rt : ttl;
			} else if (i < hops + naddr) {
				if (qtype == CG_DNS_A)
					snprintf(ip, sizeof(ip), "10.0.0.%d", i - hops);
				else
					snprintf(ip, sizeof(ip), "2001:db8::%x", i - hops);
				addr(&m, hop_owner(o, sizeof(o), hops, &s), rt, ip);
				if (kept < CG_DNS_MAX_ADDRS) {
					kept++;
					ttl = rt < ttl ? rt : ttl;
				}
			} else if (i % 2) {
				addr(&m, "decoy.example", 1, qtype == CG_DNS_A ? "192.0.2.1" : "2001:db8::1");
			} else {
				cname(&m, "decoy.example", 1, hop(t, sizeof(t), hops));
			}
		}
		if (parse(&m, qtype, &r) != CG_DNS_ADDRS || r.n != (naddr < 16 ? naddr : 16) || r.ttl != ttl)
			bad++;
	}
	CHECK_EQ(bad, 0);
}

/* Whether a result is one the caller can take: an outcome in range,
 * addresses only with CG_DNS_ADDRS, a TTL without the high bit. */
static int sane(const struct cg_dns_result *r)
{
	if (r->outcome < CG_DNS_IGNORE || r->outcome > CG_DNS_MALFORMED || r->n < 0 || r->n > CG_DNS_MAX_ADDRS)
		return 0;
	if ((r->outcome == CG_DNS_ADDRS) != (r->n > 0) || r->ttl > 0x7fffffffu)
		return 0;
	return r->outcome == CG_DNS_ADDRS || r->ttl == 0;
}

/* Valid answers mutated: cut at every length, octets flipped, inserted and
 * deleted, at random but always the same (xorshift). Only that nothing
 * breaks and the result is sane: ASan and UBSan watch the rest. */
static void robust(void)
{
	struct msg base[6], *b;
	struct cg_dns_result r, r2;
	uint8_t buf[sizeof(base[0].b) + 16];
	uint64_t s = 0x9e3779b97f4a7c15ULL;
	int nb = 0, insane = 0, unstable = 0, seen[CG_DNS_MALFORMED + 1] = { 0 };
	long parses = 0;
	char o[64], ip[32];
	size_t rd, prev;

	b = &base[nb++];
	answer(b, 0, 2, Q, CG_DNS_A);
	addr(b, "@12", 300, "192.0.2.1");
	addr(b, "vps@16", 60, "192.0.2.2");
	b = &base[nb++];
	answer(b, 0, 4, Q, CG_DNS_AAAA);
	addr(b, "x1.cdn.net", 300, "2001:db8::7");
	rd = cname(b, "@12", 600, "edge.cdn.net");
	cname(b, ptr(o, sizeof(o), "", rd), 120, ptr(ip, sizeof(ip), "x1", rd + 5));
	addr(b, "x1.cdn.net", 30, "2001:db8::8");
	b = &base[nb++];
	answer(b, 0, 20, Q, CG_DNS_A);
	for (int i = 1; i <= 20; i++) {
		snprintf(ip, sizeof(ip), "10.0.0.%d", i);
		addr(b, "@12", 300, ip);
	}
	b = &base[nb++];
	answer(b, 3, 0, Q, CG_DNS_A);
	b = &base[nb++];
	answer(b, TC, 3, Q, CG_DNS_A);
	addr(b, "@12", 60, "192.0.2.1");
	addr(b, "@12", 60, "192.0.2.2");
	b = &base[nb++];
	answer(b, 0, 2, Q, CG_DNS_A);
	rd = rrhead(b, "@12", TXT, CG_DNS_IN, 60);
	prev = 12;
	for (int i = 0; i < 40; i++) {
		size_t here = b->n;

		put16(b, 0xc000u | (unsigned)prev);
		prev = here;
	}
	rdend(b, rd);
	addr(b, ptr(o, sizeof(o), "", prev), 60, "192.0.2.1");

	for (int i = 0; i < nb; i++) {
		CHECK_EQ(parse(&base[i], i == 1 ? CG_DNS_AAAA : CG_DNS_A, &r), i == 3 ? CG_DNS_NXDOMAIN : CG_DNS_ADDRS);
		for (size_t len = 0; len <= base[i].n; len++, parses++) {
			parse_as(base[i].b, len, ID, Q, i == 1 ? CG_DNS_AAAA : CG_DNS_A, &r);
			insane += !sane(&r);
			seen[r.outcome < 0 || r.outcome > CG_DNS_MALFORMED ? 0 : r.outcome]++;
		}
	}

	for (int it = 0; it < 220000; it++, parses++) {
		int i = (int)(xs(&s) % (uint64_t)nb), kind = (int)(xs(&s) % 4);
		uint16_t qtype = i == 1 ? CG_DNS_AAAA : CG_DNS_A;
		size_t n = base[i].n, at, k;

		memcpy(buf, base[i].b, n);
		switch (kind) {
		case 0: /* flips */
		case 1:
			for (int f = 1 + (int)(xs(&s) % 4); f > 0; f--)
				buf[xs(&s) % n] ^= (uint8_t)(1 + xs(&s) % 255);
			break;
		case 2: /* an insertion */
			at = (size_t)(xs(&s) % (n + 1));
			k = 1 + (size_t)(xs(&s) % 8);
			memmove(buf + at + k, buf + at, n - at);
			for (size_t j = 0; j < k; j++)
				buf[at + j] = (uint8_t)xs(&s);
			n += k;
			break;
		default: /* a deletion */
			at = (size_t)(xs(&s) % n);
			k = 1 + (size_t)(xs(&s) % 8);
			k = k < n - at ? k : n - at;
			memmove(buf + at, buf + at + k, n - at - k);
			n -= k;
			break;
		}
		if (xs(&s) % 4 == 0)
			n = (size_t)(xs(&s) % (n + 1));
		if (n >= 3 && xs(&s) % 8) { /* mostly ours, so that the rest is read */
			buf[0] = ID >> 8;
			buf[1] = ID & 0xff;
			buf[2] |= 0x80;
		}
		parse_as(buf, n, ID, Q, qtype, &r);
		insane += !sane(&r);
		seen[r.outcome < 0 || r.outcome > CG_DNS_MALFORMED ? 0 : r.outcome]++;
		if (it % 16 == 0) {
			parse_as(buf, n, ID, Q, qtype, &r2);
			unstable += memcmp(&r, &r2, sizeof(r)) != 0;
		}
	}

	/* Random octets after a header of ours. */
	for (int it = 0; it < 20000; it++, parses++) {
		size_t n = 12 + (size_t)(xs(&s) % 200);

		for (size_t j = 0; j < n; j++)
			buf[j] = (uint8_t)xs(&s);
		buf[0] = ID >> 8;
		buf[1] = ID & 0xff;
		buf[2] = (uint8_t)((buf[2] | 0x80) & ~0x78);
		buf[4] = 0;
		buf[5] = 1;
		buf[6] = 0;
		parse_as(buf, n, ID, Q, CG_DNS_A, &r);
		insane += !sane(&r);
	}

	CHECK(parses >= 200000);
	CHECK_EQ(insane, 0);
	CHECK_EQ(unstable, 0);
	/* The mutations reach every kind of answer. */
	for (int i = CG_DNS_IGNORE; i <= CG_DNS_MALFORMED; i++)
		CHECK(seen[i] > 0);
}

/* Random strings: whatever cg_dns_name_norm takes is a fixed point of it
 * and makes a query. */
static void names_random(void)
{
	static const char alpha[] = "abcXYZ019.-_ :\x80";
	uint64_t s = 0x1234567887654321ULL;
	char in[300], out[CG_DNS_NAME_MAX + 1], again[CG_DNS_NAME_MAX + 1];
	uint8_t q[CG_DNS_QUERY_MAX];
	int ok = 0, bad = 0;

	for (int it = 0; it < 50000; it++) {
		size_t n = (size_t)(xs(&s) % 260);

		for (size_t j = 0; j < n; j++)
			in[j] = xs(&s) % 3 ? alpha[xs(&s) % 3] : alpha[xs(&s) % (sizeof(alpha) - 1)];
		in[n] = 0;
		if (cg_dns_name_norm(in, out, sizeof(out)))
			continue;
		ok++;
		bad += cg_dns_name_norm(out, again, sizeof(again)) || strcmp(out, again) ||
		       cg_dns_query(q, sizeof(q), 7, out, CG_DNS_A) != (int)(12 + strlen(out) + 2 + 4);
	}
	CHECK(ok > 1000);
	CHECK_EQ(bad, 0);
}

void test_dns(void)
{
	name_norm();
	query();
	plain();
	cnames();
	rcodes();
	questions();
	records();
	truncated();
	pointers();
	shuffled();
	robust();
	names_random();
}
