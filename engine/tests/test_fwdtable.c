/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "fwdtable.h"
#include "test.h"

#define ERRLEN 128

/* contrib/vps/test/fixtures/forward-two-routers, byte for byte: the format
 * the firewall script reads. */
#define TWO_ROUTERS                                                          \
	"# cengarde forward table 1, written by the engine: data only\n"     \
	"pass bravo\n"                                                       \
	"rule alpha tcp 9000 9000 22\n"                                      \
	"rule alpha tcp 7000 7010 7000\n"                                    \
	"rule bravo udp 5000 5010 15000\n"                                   \
	"rule bravo tcp 6000 6000 6000\n"                                    \
	"rule bravo udp 6000 6000 6000\n"                                    \
	"rule alpha tcp 12000 12099 2000\n"

#define HEADER "# cengarde forward table 1, written by the engine: data only\n"

static const char *const names[] = { "alpha", "bravo" };
static const uint8_t both_on[] = { 1, 1 };

/* A buffer size the compiler cannot see through: it would warn at compile
 * time of the truncation some tests make on purpose. */
static size_t opaque(size_t v)
{
	volatile size_t x = v;

	return x;
}

static struct cg_fwd_rule mk(uint8_t proto, uint16_t first, uint16_t last, uint16_t to, uint8_t client)
{
	struct cg_fwd_rule r = { .first = first, .last = last, .to = to, .proto = proto, .client = client };

	return r;
}

static int is(const struct cg_fwd_rule *r, uint8_t proto, unsigned first, unsigned last, unsigned to, uint8_t client)
{
	return r->proto == proto && r->first == first && r->last == last && r->to == to && r->client == client;
}

/* list as client 3 into a new array. */
static int parse(const char *list, struct cg_fwd_rule *r, int *n)
{
	char err[ERRLEN];
	int rc;

	*n = 0;
	rc = cg_fwd_parse(list, 3, r, n, CG_FWD_MAX, err, sizeof(err));
	if (rc)
		fprintf(stderr, "list '%s': unexpected error '%s'\n", list, err);
	return rc;
}

/* list is refused with exactly this message, and adds no rule. */
static void bad(const char *list, const char *msg)
{
	struct cg_fwd_rule r[CG_FWD_MAX];
	char err[ERRLEN];
	int n = 0;

	CHECK_EQ(cg_fwd_parse(list, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK_EQ(n, 0);
	if (strcmp(err, msg))
		fprintf(stderr, "list '%s': error '%s', wanted '%s'\n", list, err, msg);
	CHECK(!strcmp(err, msg));
}

static void test_parse_valid(void)
{
	struct cg_fwd_rule r[CG_FWD_MAX];
	int n;

	/* One port: it goes to itself. The client index is the one given. */
	CHECK_EQ(parse("tcp:9000", r, &n), 0);
	CHECK_EQ(n, 1);
	CHECK(is(&r[0], CG_FWD_TCP, 9000, 9000, 9000, 3));

	CHECK_EQ(parse("udp:53", r, &n), 0);
	CHECK_EQ(n, 1);
	CHECK(is(&r[0], CG_FWD_UDP, 53, 53, 53, 3));

	/* A range keeps its offsets: to defaults to the first port. */
	CHECK_EQ(parse("udp:5000-5010", r, &n), 0);
	CHECK_EQ(n, 1);
	CHECK(is(&r[0], CG_FWD_UDP, 5000, 5010, 5000, 3));

	/* =TO */
	CHECK_EQ(parse("tcp:9000=22", r, &n), 0);
	CHECK(n == 1 && is(&r[0], CG_FWD_TCP, 9000, 9000, 22, 3));
	CHECK_EQ(parse("udp:5000-5010=15000", r, &n), 0);
	CHECK(n == 1 && is(&r[0], CG_FWD_UDP, 5000, 5010, 15000, 3));

	/* both: a tcp rule and a udp rule, tcp first. */
	CHECK_EQ(parse("both:6000", r, &n), 0);
	CHECK_EQ(n, 2);
	CHECK(is(&r[0], CG_FWD_TCP, 6000, 6000, 6000, 3));
	CHECK(is(&r[1], CG_FWD_UDP, 6000, 6000, 6000, 3));
	CHECK_EQ(parse("both:6000-6010=7000", r, &n), 0);
	CHECK_EQ(n, 2);
	CHECK(is(&r[0], CG_FWD_TCP, 6000, 6010, 7000, 3));
	CHECK(is(&r[1], CG_FWD_UDP, 6000, 6010, 7000, 3));

	/* A list: in order, each one's own TO. */
	CHECK_EQ(parse("tcp:9000=22 udp:5000-5010=15000 both:6000 tcp:7000-7010", r, &n), 0);
	CHECK_EQ(n, 5);
	CHECK(is(&r[0], CG_FWD_TCP, 9000, 9000, 22, 3));
	CHECK(is(&r[1], CG_FWD_UDP, 5000, 5010, 15000, 3));
	CHECK(is(&r[2], CG_FWD_TCP, 6000, 6000, 6000, 3));
	CHECK(is(&r[3], CG_FWD_UDP, 6000, 6000, 6000, 3));
	CHECK(is(&r[4], CG_FWD_TCP, 7000, 7010, 7000, 3));

	/* Spaces, tabs and commas, in any number, also at both ends. */
	CHECK_EQ(parse("tcp:1,udp:2", r, &n), 0);
	CHECK_EQ(n, 2);
	CHECK_EQ(parse("  tcp:1 \t,, udp:2\t\t,\t both:3  ,", r, &n), 0);
	CHECK_EQ(n, 4);
	CHECK(is(&r[0], CG_FWD_TCP, 1, 1, 1, 3));
	CHECK(is(&r[1], CG_FWD_UDP, 2, 2, 2, 3));
	CHECK(is(&r[2], CG_FWD_TCP, 3, 3, 3, 3));
	CHECK(is(&r[3], CG_FWD_UDP, 3, 3, 3, 3));

	/* An empty list is valid: no rules. */
	CHECK_EQ(parse("", r, &n), 0);
	CHECK_EQ(n, 0);
	CHECK_EQ(parse(" \t,, ,\t", r, &n), 0);
	CHECK_EQ(n, 0);
	n = 0;
	CHECK_EQ(cg_fwd_parse(NULL, 3, r, &n, CG_FWD_MAX, NULL, 0), 0);
	CHECK_EQ(n, 0);

	/* The ends of the range of ports, and the target at the very end. */
	CHECK_EQ(parse("tcp:1 tcp:65535 tcp:1-65535 tcp:1-65535=1 tcp:65535=1 tcp:1=65535", r, &n), 0);
	CHECK_EQ(n, 6);
	CHECK(is(&r[1], CG_FWD_TCP, 65535, 65535, 65535, 3));
	CHECK(is(&r[2], CG_FWD_TCP, 1, 65535, 1, 3));
	CHECK(is(&r[5], CG_FWD_TCP, 1, 1, 65535, 3));
	CHECK_EQ(parse("tcp:65000-65010=65525", r, &n), 0); /* 65525 + 10 = 65535 */
	CHECK(n == 1 && is(&r[0], CG_FWD_TCP, 65000, 65010, 65525, 3));

	/* Up to five digits, leading zeros included. */
	CHECK_EQ(parse("tcp:00080 tcp:00001-00002=00010", r, &n), 0);
	CHECK_EQ(n, 2);
	CHECK(is(&r[0], CG_FWD_TCP, 80, 80, 80, 3));
	CHECK(is(&r[1], CG_FWD_TCP, 1, 2, 10, 3));

	/* Rules are appended after the n there are, with the client given. */
	{
		char err[ERRLEN];

		n = 2;
		r[0] = mk(CG_FWD_UDP, 1, 1, 1, 0);
		r[1] = mk(CG_FWD_UDP, 2, 2, 2, 1);
		CHECK_EQ(cg_fwd_parse("tcp:10 both:20", 7, r, &n, CG_FWD_MAX, err, sizeof(err)), 0);
		CHECK_EQ(n, 5);
		CHECK(is(&r[0], CG_FWD_UDP, 1, 1, 1, 0));
		CHECK(is(&r[1], CG_FWD_UDP, 2, 2, 2, 1));
		CHECK(is(&r[2], CG_FWD_TCP, 10, 10, 10, 7));
		CHECK(is(&r[3], CG_FWD_TCP, 20, 20, 20, 7));
		CHECK(is(&r[4], CG_FWD_UDP, 20, 20, 20, 7));
		CHECK_EQ(err[0], 0);
	}
}

static void test_parse_errors(void)
{
	/* Each message names the entry as it was written. */
	bad("tcp:70000", "'tcp:70000': not a port (1 to 65535)");
	bad("tcp:65536", "'tcp:65536': not a port (1 to 65535)");
	bad("tcp:0", "'tcp:0': not a port (1 to 65535)");
	bad("tcp:", "'tcp:': not a port (1 to 65535)");
	bad("tcp:-5", "'tcp:-5': not a port (1 to 65535)");
	bad("tcp:+80", "'tcp:+80': not a port (1 to 65535)");
	bad("tcp:80-", "'tcp:80-': not a port (1 to 65535)");
	bad("tcp:80=", "'tcp:80=': not a port (1 to 65535)");
	bad("tcp:=80", "'tcp:=80': not a port (1 to 65535)");
	bad("tcp:-", "'tcp:-': not a port (1 to 65535)");
	bad("tcp:http", "'tcp:http': not a port (1 to 65535)");
	bad("tcp:8o", "'tcp:8o': not a port (1 to 65535)");
	bad("tcp:5-6-7", "'tcp:5-6-7': not a port (1 to 65535)");
	bad("tcp:1=2=3", "'tcp:1=2=3': not a port (1 to 65535)");
	bad("tcp:1=-3", "'tcp:1=-3': not a port (1 to 65535)");
	bad("tcp:000080", "'tcp:000080': not a port (1 to 65535)"); /* six digits */
	bad("tcp:1-70000", "'tcp:1-70000': not a port (1 to 65535)");
	bad("tcp:1-2=70000", "'tcp:1-2=70000': not a port (1 to 65535)");
	bad("tcp:9000=0", "'tcp:9000=0': not a port (1 to 65535)");
	bad("both:99999", "'both:99999': not a port (1 to 65535)");

	bad("tcp:9005-9001", "'tcp:9005-9001': the first port is after the last");
	bad("udp:2-1", "'udp:2-1': the first port is after the last");
	bad("both:9005-9001=22", "'both:9005-9001=22': the first port is after the last");

	bad("tcp:65000-65010=65530", "'tcp:65000-65010=65530': the target range goes past 65535");
	bad("tcp:1-2=65535", "'tcp:1-2=65535': the target range goes past 65535");
	bad("udp:1-65535=2", "'udp:1-65535=2': the target range goes past 65535");

	bad("sctp:9000", "'sctp:9000': expected tcp, udp or both");
	bad("TCP:9000", "'TCP:9000': expected tcp, udp or both");
	bad(":9000", "':9000': expected tcp, udp or both");
	bad("tcpp:9000", "'tcpp:9000': expected tcp, udp or both");
	bad("tc:9000", "'tc:9000': expected tcp, udp or both");
	bad("bot:9000", "'bot:9000': expected tcp, udp or both");
	bad("9000", "'9000': expected PROTO:PORT[-PORT][=TO], as in tcp:9000=22");
	bad("tcp", "'tcp': expected PROTO:PORT[-PORT][=TO], as in tcp:9000=22");

	/* The protocol is judged before the ports, and the syntax of the ports
	 * before how they fit together. */
	bad("sctp:70000", "'sctp:70000': expected tcp, udp or both");
	bad("tcp:9005-9001=70000", "'tcp:9005-9001=70000': not a port (1 to 65535)");
	bad("tcp:9005-9001=65535", "'tcp:9005-9001=65535': the first port is after the last");
}

static void test_parse_report(void)
{
	struct cg_fwd_rule r[CG_FWD_MAX];
	char err[ERRLEN], entry[160], want[ERRLEN];
	int n;

	/* The offending entry is named wherever it stands, the good ones before
	 * it do not matter, and a failed list adds nothing. */
	n = 0;
	CHECK_EQ(cg_fwd_parse("tcp:80 udp:abc tcp:90", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "'udp:abc': not a port (1 to 65535)"));
	CHECK_EQ(n, 0);
	CHECK_EQ(cg_fwd_parse("tcp:80, ,udp:81\t both:82 sctp:83", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "'sctp:83': expected tcp, udp or both"));
	CHECK_EQ(n, 0);

	/* Rules already there stay as they were. */
	r[0] = mk(CG_FWD_TCP, 5, 6, 7, 1);
	n = 1;
	CHECK_EQ(cg_fwd_parse("both:10 tcp:2-1", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "'tcp:2-1': the first port is after the last"));
	CHECK_EQ(n, 1);
	CHECK(is(&r[0], CG_FWD_TCP, 5, 6, 7, 1));

	/* An entry is quoted whole up to 40 characters, then cut and followed
	 * by "...". */
	for (size_t k = 0; k < 4; k++) {
		static const size_t lens[] = { 39, 40, 41, 159 };
		size_t len = lens[k];

		memset(entry, '9', sizeof(entry));
		memcpy(entry, "tcp:", 4);
		entry[len] = '\0';
		n = 0;
		CHECK_EQ(cg_fwd_parse(entry, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
		if (len <= 40)
			snprintf(want, sizeof(want), "'%s': not a port (1 to 65535)", entry);
		else
			snprintf(want, sizeof(want), "'%.40s...': not a port (1 to 65535)", entry);
		CHECK(!strcmp(err, want));
		CHECK_EQ(n, 0);
	}

	/* A small buffer is cut and ended, a missing one is no harm. */
	CHECK_EQ(cg_fwd_parse("tcp:70000", 3, r, &n, CG_FWD_MAX, err, opaque(8)), -1);
	CHECK_EQ(strlen(err), 7);
	CHECK(!memcmp(err, "'tcp:70", 7));
	CHECK_EQ(cg_fwd_parse("tcp:70000", 3, r, &n, CG_FWD_MAX, err, opaque(1)), -1);
	CHECK_EQ(err[0], 0);
	CHECK_EQ(cg_fwd_parse("tcp:70000", 3, r, &n, CG_FWD_MAX, err, 0), -1);
	CHECK_EQ(cg_fwd_parse("tcp:70000", 3, r, &n, CG_FWD_MAX, NULL, 0), -1);
	CHECK_EQ(cg_fwd_parse("tcp:70000", 3, r, &n, CG_FWD_MAX, NULL, 64), -1);
	CHECK_EQ(cg_fwd_parse("tcp:1 tcp:2", 3, r, &n, 1, NULL, 0), -1);
}

/* count entries "PROTO:1" .. "PROTO:count", separated by spaces, at the end
 * of buf. */
static void append(char *buf, size_t size, const char *proto, int count)
{
	size_t len = strlen(buf);

	for (int i = 1; i <= count; i++)
		len += (size_t)snprintf(buf + len, size - len, "%s:%d ", proto, i);
}

static void test_parse_limit(void)
{
	struct cg_fwd_rule r[CG_FWD_MAX];
	char buf[16384], err[ERRLEN];
	int n;

	CHECK_EQ(CG_FWD_MAX, 1024);

	/* Exactly at the limit is fine, one past it is not. */
	buf[0] = '\0';
	append(buf, sizeof(buf), "tcp", CG_FWD_MAX);
	n = 0;
	CHECK_EQ(cg_fwd_parse(buf, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), 0);
	CHECK_EQ(n, CG_FWD_MAX);
	CHECK(is(&r[CG_FWD_MAX - 1], CG_FWD_TCP, CG_FWD_MAX, CG_FWD_MAX, CG_FWD_MAX, 3));
	strcat(buf, "udp:1");
	n = 0;
	CHECK_EQ(cg_fwd_parse(buf, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "too many rules (at most 1024)"));
	CHECK_EQ(n, 0);

	/* A "both" entry counts as two. */
	buf[0] = '\0';
	append(buf, sizeof(buf), "both", CG_FWD_MAX / 2);
	n = 0;
	CHECK_EQ(cg_fwd_parse(buf, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), 0);
	CHECK_EQ(n, CG_FWD_MAX);
	CHECK(is(&r[CG_FWD_MAX - 2], CG_FWD_TCP, 512, 512, 512, 3));
	CHECK(is(&r[CG_FWD_MAX - 1], CG_FWD_UDP, 512, 512, 512, 3));
	strcat(buf, "tcp:1");
	n = 0;
	CHECK_EQ(cg_fwd_parse(buf, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "too many rules (at most 1024)"));
	CHECK_EQ(n, 0);

	/* One slot left: a tcp entry takes it, a both entry does not and fails
	 * whole. */
	buf[0] = '\0';
	append(buf, sizeof(buf), "both", CG_FWD_MAX / 2 - 1);
	append(buf, sizeof(buf), "tcp", 1);
	n = 0;
	CHECK_EQ(cg_fwd_parse(buf, 3, r, &n, CG_FWD_MAX, err, sizeof(err)), 0);
	CHECK_EQ(n, CG_FWD_MAX - 1);
	CHECK_EQ(cg_fwd_parse("tcp:2000", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), 0);
	CHECK_EQ(n, CG_FWD_MAX);
	n = CG_FWD_MAX - 1;
	CHECK_EQ(cg_fwd_parse("both:2000", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "too many rules (at most 1024)"));
	CHECK_EQ(n, CG_FWD_MAX - 1);
	n = CG_FWD_MAX;
	CHECK_EQ(cg_fwd_parse("tcp:2000", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK_EQ(n, CG_FWD_MAX);
	CHECK_EQ(cg_fwd_parse("", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), 0); /* nothing to add always fits */

	/* The limit is the caller's. */
	n = 0;
	CHECK_EQ(cg_fwd_parse("both:1 both:2", 3, r, &n, 3, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "too many rules (at most 3)"));
	CHECK_EQ(n, 0);
	CHECK_EQ(cg_fwd_parse("both:1 tcp:2", 3, r, &n, 3, err, sizeof(err)), 0);
	CHECK_EQ(n, 3);
	n = 0;
	CHECK_EQ(cg_fwd_parse("tcp:1", 3, r, &n, 0, err, sizeof(err)), -1);

	/* A bad entry is reported as such before the limit is considered. */
	n = CG_FWD_MAX;
	CHECK_EQ(cg_fwd_parse("tcp:0", 3, r, &n, CG_FWD_MAX, err, sizeof(err)), -1);
	CHECK(!strcmp(err, "'tcp:0': not a port (1 to 65535)"));
}

static void test_clash(void)
{
	struct cg_fwd_rule t100 = mk(CG_FWD_TCP, 100, 199, 100, 0), t150 = mk(CG_FWD_TCP, 150, 250, 1, 1),
			   t200 = mk(CG_FWD_TCP, 200, 300, 100, 0), t199 = mk(CG_FWD_TCP, 199, 199, 1, 0),
			   t120 = mk(CG_FWD_TCP, 120, 130, 1, 2), u100 = mk(CG_FWD_UDP, 100, 199, 100, 0),
			   t1 = mk(CG_FWD_TCP, 1, 1, 1, 0), t2 = mk(CG_FWD_TCP, 2, 2, 2, 0),
			   t65535 = mk(CG_FWD_TCP, 65535, 65535, 1, 0), tall = mk(CG_FWD_TCP, 1, 65535, 1, 0);

	/* Overlapping ranges of one protocol, in either order. */
	CHECK(cg_fwd_clash(&t100, &t150));
	CHECK(cg_fwd_clash(&t150, &t100));
	CHECK(cg_fwd_clash(&t150, &t200));
	/* One inside the other. */
	CHECK(cg_fwd_clash(&t100, &t120));
	CHECK(cg_fwd_clash(&t120, &t100));
	/* Touching at a single port. */
	CHECK(cg_fwd_clash(&t100, &t199));
	CHECK(cg_fwd_clash(&t199, &t100));
	/* The same rule. */
	CHECK(cg_fwd_clash(&t100, &t100));
	CHECK(cg_fwd_clash(&t1, &t1));
	/* Adjacent ranges do not clash. */
	CHECK(!cg_fwd_clash(&t100, &t200));
	CHECK(!cg_fwd_clash(&t200, &t100));
	CHECK(!cg_fwd_clash(&t199, &t200));
	CHECK(!cg_fwd_clash(&t1, &t2));
	CHECK(!cg_fwd_clash(&t2, &t1));
	CHECK(!cg_fwd_clash(&t120, &t200));
	/* The ends of the port space. */
	CHECK(cg_fwd_clash(&tall, &t1));
	CHECK(cg_fwd_clash(&t65535, &tall));
	CHECK(!cg_fwd_clash(&t65535, &t1));
	/* tcp and udp are separate, on the same port too. */
	CHECK(!cg_fwd_clash(&t100, &u100));
	CHECK(!cg_fwd_clash(&u100, &t100));
	CHECK(cg_fwd_clash(&u100, &u100));
	/* The target and the client are of no matter: it is the server's port
	 * that cannot go to two places. */
	CHECK(t100.to != t150.to && t100.client != t150.client);
	CHECK(cg_fwd_clash(&t100, &t150));
}

static void test_overlap(void)
{
	struct cg_fwd_rule r[8];
	int i = -7, j = -7;

	CHECK(!cg_fwd_overlap(r, 0, &i, &j));
	r[0] = mk(CG_FWD_TCP, 100, 110, 100, 0);
	CHECK(!cg_fwd_overlap(r, 1, &i, &j));

	/* None: both left alone. */
	r[1] = mk(CG_FWD_UDP, 100, 110, 100, 1); /* the other protocol */
	r[2] = mk(CG_FWD_TCP, 111, 120, 111, 0); /* adjacent */
	r[3] = mk(CG_FWD_TCP, 99, 99, 99, 1);
	CHECK(!cg_fwd_overlap(r, 4, &i, &j));
	CHECK_EQ(i, -7);
	CHECK_EQ(j, -7);

	/* One pair, with i < j. */
	r[4] = mk(CG_FWD_UDP, 105, 105, 5, 2);
	CHECK(cg_fwd_overlap(r, 5, &i, &j));
	CHECK_EQ(i, 1);
	CHECK_EQ(j, 4);
	/* Only what is within n counts. */
	i = j = -7;
	CHECK(!cg_fwd_overlap(r, 4, &i, &j));
	CHECK_EQ(i, -7);

	/* The first pair is by the later rule: the first rule that takes a port
	 * an earlier one has (the one the script drops), and the first earlier
	 * rule it clashes with. Here rule 3 clashes with rule 0 and rule 5 with
	 * rule 4; rule 3 comes first. */
	r[0] = mk(CG_FWD_TCP, 100, 110, 100, 0);
	r[1] = mk(CG_FWD_TCP, 200, 210, 200, 0);
	r[2] = mk(CG_FWD_TCP, 300, 310, 300, 0);
	r[3] = mk(CG_FWD_TCP, 105, 105, 105, 1);
	r[4] = mk(CG_FWD_TCP, 400, 410, 400, 1);
	r[5] = mk(CG_FWD_TCP, 405, 406, 405, 1);
	CHECK(cg_fwd_overlap(r, 6, &i, &j));
	CHECK_EQ(i, 0);
	CHECK_EQ(j, 3);
	/* Rule 2 overlapping rule 1 (j = 2) outranks rule 0 with rule 3, though
	 * rule 0 is the earlier of the two. */
	r[2] = mk(CG_FWD_TCP, 205, 206, 205, 2);
	CHECK(cg_fwd_overlap(r, 6, &i, &j));
	CHECK_EQ(i, 1);
	CHECK_EQ(j, 2);
	/* Rule 3 on both rule 0 and rule 1: the earliest of them. */
	r[2] = mk(CG_FWD_TCP, 300, 310, 300, 0);
	r[3] = mk(CG_FWD_TCP, 110, 200, 110, 1);
	CHECK(cg_fwd_overlap(r, 6, &i, &j));
	CHECK_EQ(i, 0);
	CHECK_EQ(j, 3);
	/* Rules of different clients clash all the same. */
	r[0] = mk(CG_FWD_TCP, 22, 22, 22, 0);
	r[1] = mk(CG_FWD_TCP, 22, 22, 2222, 1);
	CHECK(cg_fwd_overlap(r, 2, &i, &j));
	CHECK(i == 0 && j == 1);

	/* A "both" entry never clashes with itself. */
	{
		struct cg_fwd_rule p[CG_FWD_MAX];
		char err[ERRLEN];
		int n = 0;

		CHECK_EQ(cg_fwd_parse("both:6000-6010 tcp:7000 udp:7000", 0, p, &n, CG_FWD_MAX, err, sizeof(err)), 0);
		CHECK(!cg_fwd_overlap(p, n, &i, &j));
		CHECK_EQ(cg_fwd_parse("tcp:6010", 1, p, &n, CG_FWD_MAX, err, sizeof(err)), 0);
		CHECK(cg_fwd_overlap(p, n, &i, &j));
		CHECK(i == 0 && j == 4);
	}
}

static void test_holder(void)
{
	struct cg_pass_cand c[4];

	/* Nobody: -1, with or without a holder to keep. */
	CHECK_EQ(cg_pass_holder(c, 0, -1), -1);
	CHECK_EQ(cg_pass_holder(c, 0, 0), -1);
	memset(c, 0, sizeof(c));
	CHECK_EQ(cg_pass_holder(c, 4, -1), -1);
	for (int i = 0; i < 4; i++) {
		c[i].eligible = 1;
		c[i].wish = -1;
	}
	CHECK_EQ(cg_pass_holder(c, 4, -1), -1); /* nobody has said anything */
	c[1].wish = 0;
	c[2].wish = 0;
	c[2].wish_ms = 5;
	CHECK_EQ(cg_pass_holder(c, 4, -1), -1); /* nobody asks */

	/* Whoever asks first. */
	c[0].wish = 1;
	c[0].wish_ms = 300;
	c[1].wish = 1;
	c[1].wish_ms = 100;
	c[2].wish = 1;
	c[2].wish_ms = 200;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 1);
	CHECK_EQ(cg_pass_holder(c, 3, -1), 1);
	CHECK_EQ(cg_pass_holder(c, 1, -1), 0);
	/* The time of a wish that is not on does not count. */
	c[3].wish = 0;
	c[3].wish_ms = 1;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 1);
	c[3].wish = -1;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 1);

	/* A tie goes to the smallest index. */
	c[0].wish_ms = 100;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 0);
	c[0].wish = 0;
	c[2].wish_ms = 100;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 1);
	c[1].wish = -1;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 2);

	/* An asker that cannot hold it is skipped. */
	c[0].wish = 1;
	c[0].wish_ms = 50;
	c[0].eligible = 0;
	c[1].wish = 1;
	c[1].wish_ms = 100;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 1);
	c[1].eligible = 0;
	c[2].wish = 1;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 2);
	c[2].eligible = 0;
	CHECK_EQ(cg_pass_holder(c, 4, -1), -1);

	/* The holder keeps the range with its wish on, and with it not known
	 * yet, whoever asked before. */
	memset(c, 0, sizeof(c));
	for (int i = 0; i < 4; i++)
		c[i].eligible = 1;
	c[0].wish = 1;
	c[0].wish_ms = 100;
	c[1].wish = 1;
	c[1].wish_ms = 200;
	c[2].wish = 1;
	c[2].wish_ms = 50;
	CHECK_EQ(cg_pass_holder(c, 4, -1), 2);
	CHECK_EQ(cg_pass_holder(c, 4, 1), 1);
	CHECK_EQ(cg_pass_holder(c, 4, 0), 0);
	c[1].wish = -1;
	CHECK_EQ(cg_pass_holder(c, 4, 1), 1); /* the server restarted: nothing known yet */
	c[1].wish = 1;
	c[1].wish_ms = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), 1);

	/* ... and loses it with its wish off or when it may not hold it. */
	c[1].wish = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), 2); /* the one that asked first of the rest */
	c[1].wish = 1;
	c[1].eligible = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), 2);
	c[2].eligible = 0;
	c[0].eligible = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), -1);
	c[0].eligible = 1;
	c[0].wish = 0;
	c[1].eligible = 1;
	c[1].wish = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), -1);
	/* An unknown wish of a holder that is no longer eligible is not kept. */
	c[1].wish = -1;
	c[1].eligible = 0;
	CHECK_EQ(cg_pass_holder(c, 4, 1), -1);

	/* A holder that is not among the candidates is no holder. */
	c[1].eligible = 1;
	c[1].wish = 1;
	CHECK_EQ(cg_pass_holder(c, 2, 2), 1);
	CHECK_EQ(cg_pass_holder(c, 2, 7), 1);
	CHECK_EQ(cg_pass_holder(c, 2, -1), 1);

	/* The range stays where it is while the router keeps asking: client 1
	 * has it, client 0 turns its wish on later, and has the smaller index. */
	memset(c, 0, sizeof(c));
	for (int i = 0; i < 4; i++) {
		c[i].eligible = 1;
		c[i].wish = -1;
	}
	{
		int h = -1;

		c[1].wish = 1;
		c[1].wish_ms = 200;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, 1);
		c[0].wish = 1;
		c[0].wish_ms = 300;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, 1);
		c[2].wish = 1;
		c[2].wish_ms = 250;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, 1);
		/* It asks no more: the earliest of the others takes it. */
		c[1].wish = 0;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, 2);
		/* The first asks again, later than all: the range stays. */
		c[1].wish = 1;
		c[1].wish_ms = 900;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, 2);
		/* Nobody asks: it is free. */
		c[0].wish = 0;
		c[1].wish = 0;
		c[2].wish = 0;
		h = cg_pass_holder(c, 4, h);
		CHECK_EQ(h, -1);
	}
}

static void test_table(void)
{
	struct cg_fwd_rule r[8];
	static const uint8_t only_bravo[] = { 0, 1 }, only_alpha[] = { 1, 0 }, none[] = { 0, 0 };
	char buf[512];
	size_t total = strlen(TWO_ROUTERS);

	/* Two routers, as the fixture of the script. */
	r[0] = mk(CG_FWD_TCP, 9000, 9000, 22, 0);
	r[1] = mk(CG_FWD_TCP, 7000, 7010, 7000, 0);
	r[2] = mk(CG_FWD_UDP, 5000, 5010, 15000, 1);
	r[3] = mk(CG_FWD_TCP, 6000, 6000, 6000, 1);
	r[4] = mk(CG_FWD_UDP, 6000, 6000, 6000, 1);
	r[5] = mk(CG_FWD_TCP, 12000, 12099, 2000, 0);
	memset(buf, 0x55, sizeof(buf));
	CHECK_EQ(cg_fwd_table(buf, sizeof(buf), 1, names, r, 6, both_on), (int)total);
	CHECK_EQ(total, 253); /* wc -c of the fixture */
	CHECK(!strcmp(buf, TWO_ROUTERS));
	CHECK_EQ(buf[total], 0);

	/* No holder: no "pass" line. */
	CHECK_EQ(cg_fwd_table(buf, sizeof(buf), -1, names, r, 6, both_on), (int)(total - strlen("pass bravo\n")));
	CHECK(!strcmp(buf, HEADER "rule alpha tcp 9000 9000 22\n"
			   "rule alpha tcp 7000 7010 7000\n"
			   "rule bravo udp 5000 5010 15000\n"
			   "rule bravo tcp 6000 6000 6000\n"
			   "rule bravo udp 6000 6000 6000\n"
			   "rule alpha tcp 12000 12099 2000\n"));

	/* Only the clients in writes: the rules of the others are left out, the
	 * order of the rest kept. The holder is written as given. */
	CHECK(cg_fwd_table(buf, sizeof(buf), 1, names, r, 6, only_bravo) > 0);
	CHECK(!strcmp(buf, HEADER "pass bravo\n"
			   "rule bravo udp 5000 5010 15000\n"
			   "rule bravo tcp 6000 6000 6000\n"
			   "rule bravo udp 6000 6000 6000\n"));
	CHECK(cg_fwd_table(buf, sizeof(buf), 0, names, r, 6, only_alpha) > 0);
	CHECK(!strcmp(buf, HEADER "pass alpha\n"
			   "rule alpha tcp 9000 9000 22\n"
			   "rule alpha tcp 7000 7010 7000\n"
			   "rule alpha tcp 12000 12099 2000\n"));
	CHECK(cg_fwd_table(buf, sizeof(buf), -1, names, r, 6, only_alpha) > 0);
	CHECK(!strcmp(buf, HEADER "rule alpha tcp 9000 9000 22\n"
			   "rule alpha tcp 7000 7010 7000\n"
			   "rule alpha tcp 12000 12099 2000\n"));

	/* Nothing to write: the header, and the holder if any. */
	CHECK_EQ(cg_fwd_table(buf, sizeof(buf), -1, names, r, 6, none), (int)strlen(HEADER));
	CHECK(!strcmp(buf, HEADER));
	CHECK_EQ(cg_fwd_table(buf, sizeof(buf), -1, names, r, 0, both_on), (int)strlen(HEADER));
	CHECK(!strcmp(buf, HEADER));
	CHECK_EQ(cg_fwd_table(buf, sizeof(buf), 0, names, r, 0, both_on), (int)(strlen(HEADER) + strlen("pass alpha\n")));
	CHECK(!strcmp(buf, HEADER "pass alpha\n"));

	/* The ends of the port space, and a rule written whole. */
	r[0] = mk(CG_FWD_UDP, 1, 65535, 1, 1);
	r[1] = mk(CG_FWD_TCP, 65535, 65535, 1, 0);
	CHECK(cg_fwd_table(buf, sizeof(buf), -1, names, r, 2, both_on) > 0);
	CHECK(!strcmp(buf, HEADER "rule bravo udp 1 65535 1\n"
			   "rule alpha tcp 65535 65535 1\n"));

	/* It fits in outlen bytes only with the NUL: at the exact length it does
	 * not, and out is empty; nothing is written past outlen. */
	r[0] = mk(CG_FWD_TCP, 9000, 9000, 22, 0);
	r[1] = mk(CG_FWD_TCP, 7000, 7010, 7000, 0);
	r[2] = mk(CG_FWD_UDP, 5000, 5010, 15000, 1);
	r[3] = mk(CG_FWD_TCP, 6000, 6000, 6000, 1);
	r[4] = mk(CG_FWD_UDP, 6000, 6000, 6000, 1);
	r[5] = mk(CG_FWD_TCP, 12000, 12099, 2000, 0);
	for (size_t outlen = 0; outlen <= total + 1; outlen++) {
		int rc, clean = 1;

		memset(buf, 0x55, sizeof(buf));
		rc = cg_fwd_table(buf, outlen, 1, names, r, 6, both_on);
		if (outlen <= total) {
			CHECK_EQ(rc, -1);
			if (outlen)
				CHECK_EQ(buf[0], 0);
		} else {
			CHECK_EQ(rc, (int)total);
			CHECK(!strcmp(buf, TWO_ROUTERS));
		}
		for (size_t k = outlen; k < sizeof(buf); k++)
			if (buf[k] != 0x55)
				clean = 0;
		CHECK(clean);
	}
	/* The same boundary at each kind of line. */
	CHECK_EQ(cg_fwd_table(buf, opaque(strlen(HEADER)), -1, names, r, 0, both_on), -1);
	CHECK_EQ(cg_fwd_table(buf, opaque(strlen(HEADER) + 1), -1, names, r, 0, both_on), (int)strlen(HEADER));
	CHECK_EQ(cg_fwd_table(buf, opaque(strlen(HEADER "pass bravo\n")), 1, names, r, 0, both_on), -1);
	CHECK_EQ(cg_fwd_table(buf, opaque(strlen(HEADER "pass bravo\n") + 1), 1, names, r, 0, both_on),
		 (int)strlen(HEADER "pass bravo\n"));
}

static void test_table_holder(void)
{
	char name[16];
	char text[512];
	struct cg_fwd_rule r[2];

	/* Found, wherever it stands. */
	memset(name, 'x', sizeof(name));
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, name, sizeof(name)), 1);
	CHECK(!strcmp(name, "bravo"));
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass alpha\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "alpha"));
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass alpha", name, sizeof(name)), 1); /* no last newline */
	CHECK(!strcmp(name, "alpha"));
	CHECK_EQ(cg_fwd_table_holder(HEADER "rule alpha tcp 9000 9000 22\n"
					    "rule bravo udp 5000 5010 15000\n"
					    "pass bravo\n"
					    "rule alpha tcp 12000 12099 2000\n",
				     name, sizeof(name)),
		 1);
	CHECK(!strcmp(name, "bravo"));
	/* The first "pass" is the one: a second is the script's to refuse. */
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass alpha\npass bravo\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "alpha"));
	/* Fields are split by blanks, as the script does. */
	CHECK_EQ(cg_fwd_table_holder(HEADER "\n\n  pass \t bravo  \t\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "bravo"));
	/* Comments are not lines. */
	CHECK_EQ(cg_fwd_table_holder(HEADER "# pass nobody\npass bravo\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "bravo"));
	/* The tail of the header is of no matter, its version is. */
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table 1\npass bravo\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "bravo"));

	/* None. */
	memset(name, 'x', sizeof(name));
	CHECK_EQ(cg_fwd_table_holder(HEADER, name, sizeof(name)), 0);
	CHECK_EQ(name[0], 0);
	CHECK_EQ(cg_fwd_table_holder(HEADER "rule alpha tcp 9000 9000 22\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table 1", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder(NULL, name, sizeof(name)), 0);
	/* Lines that are not "pass NAME" are skipped, a good one after them is found. */
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass\npass a b\npassx bravo\nrule pass tcp 1 1 1\n", name, sizeof(name)),
		 0);
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass\npass a b\npassx bravo\npass charlie\n", name, sizeof(name)), 1);
	CHECK(!strcmp(name, "charlie"));

	/* Wrong header: another version, none, or not at the top. */
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table 2, written by the engine: data only\npass bravo\n", name,
				     sizeof(name)),
		 0);
	CHECK_EQ(name[0], 0);
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table 10\npass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table 11\npass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table \npass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("# cengarde forward table\npass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("pass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("\n" HEADER "pass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder("# another table 1\npass bravo\n", name, sizeof(name)), 0);
	CHECK_EQ(cg_fwd_table_holder(" # cengarde forward table 1\npass bravo\n", name, sizeof(name)), 0);

	/* A name that does not fit: it takes namelen - 1 characters at most. */
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, name, 6), 1);
	CHECK(!strcmp(name, "bravo"));
	memset(name, 'x', sizeof(name));
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, name, 5), 0);
	CHECK_EQ(name[0], 0);
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, name, 1), 0);
	CHECK_EQ(name[0], 0);
	memset(name, 'x', sizeof(name));
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, name, 0), 0); /* no room at all */
	CHECK_EQ(name[0], 'x');
	CHECK_EQ(cg_fwd_table_holder(TWO_ROUTERS, NULL, 0), 0);
	/* The first "pass" is the answer even if it is the one that is too long. */
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass bravo\npass al\n", name, 5), 0);
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass al\npass bravo\n", name, 5), 1);
	CHECK(!strcmp(name, "al"));
	/* Nothing is written past namelen. */
	memset(name, 'x', sizeof(name));
	CHECK_EQ(cg_fwd_table_holder(HEADER "pass charlie\n", name, 8), 1);
	CHECK(!strcmp(name, "charlie"));
	CHECK_EQ(name[8], 'x');

	/* A round trip: what cg_fwd_table writes, cg_fwd_table_holder reads. */
	r[0] = mk(CG_FWD_TCP, 9000, 9000, 22, 0);
	r[1] = mk(CG_FWD_UDP, 5000, 5010, 15000, 1);
	for (int holder = 0; holder < 2; holder++) {
		CHECK(cg_fwd_table(text, sizeof(text), holder, names, r, 2, both_on) > 0);
		CHECK_EQ(cg_fwd_table_holder(text, name, sizeof(name)), 1);
		CHECK(!strcmp(name, names[holder]));
	}
	CHECK(cg_fwd_table(text, sizeof(text), -1, names, r, 2, both_on) > 0);
	CHECK_EQ(cg_fwd_table_holder(text, name, sizeof(name)), 0);
	CHECK_EQ(name[0], 0);
}

void test_fwdtable(void)
{
	test_parse_valid();
	test_parse_errors();
	test_parse_report();
	test_parse_limit();
	test_clash();
	test_overlap();
	test_holder();
	test_table();
	test_table_holder();
}
