/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <string.h>

#include "addrpick.h"
#include "test.h"

/* An address as netlink reports it. */
static struct cg_nl_addr nla(const char *ip, uint32_t flags, uint8_t scope)
{
	struct cg_nl_addr a = { .flags = flags, .scope = scope };

	a.family = strchr(ip, ':') ? AF_INET6 : AF_INET;
	inet_pton(a.family, ip, a.addr);
	return a;
}

static struct sockaddr_storage dst(const char *ip)
{
	struct sockaddr_storage s;

	memset(&s, 0, sizeof(s));
	if (strchr(ip, ':')) {
		s.ss_family = AF_INET6;
		inet_pton(AF_INET6, ip, &((struct sockaddr_in6 *)&s)->sin6_addr);
	} else {
		s.ss_family = AF_INET;
		inet_pton(AF_INET, ip, &((struct sockaddr_in *)&s)->sin_addr);
	}
	return s;
}

static int pick(const struct cg_nl_addr *a, int n, const char *to)
{
	struct sockaddr_storage d = dst(to);

	return cg_src_pick(a, n, &d);
}

/* Whether the source toward to is ip. */
static int picked(const struct cg_nl_addr *a, int n, const char *to, const char *ip)
{
	int k = pick(a, n, to);

	return k >= 0 && !memcmp(a[k].addr, nla(ip, 0, 0).addr, 16);
}

/* Stores an address the interface did not have, as netlink.c does. */
static void learn(struct cg_nl_addr *a, int *n, struct cg_nl_addr x, int from_dump)
{
	int at = cg_addr_slot(x.family, from_dump, *n);

	memmove(&a[at + 1], &a[at], sizeof(a[0]) * (size_t)(*n - at));
	a[at] = x;
	(*n)++;
}

static int rank(const char *ip, uint32_t flags, uint8_t scope, const char *to)
{
	struct cg_nl_addr a = nla(ip, flags, scope);
	uint8_t d[16];

	inet_pton(AF_INET6, to, d);
	return cg_ip6_src_rank(a.addr, a.flags, a.scope, d);
}

void test_addrpick(void)
{
	const uint8_t U = RT_SCOPE_UNIVERSE;
	struct cg_nl_addr a[8];
	int n;

	/* The ranks of decision 4. */
	CHECK_EQ(rank("2001:db8::1", 0, U, "2001:db8:9::1"), 4);
	CHECK_EQ(rank("2001:db8::1", IFA_F_TEMPORARY, U, "2001:db8:9::1"), 3);
	CHECK_EQ(rank("2001:db8::1", IFA_F_DEPRECATED, U, "2001:db8:9::1"), 2);
	CHECK_EQ(rank("2001:db8::1", IFA_F_DEPRECATED | IFA_F_TEMPORARY, U, "2001:db8:9::1"), 2);
	CHECK_EQ(rank("fd00:1::1", 0, U, "2001:db8:9::1"), 1);
	CHECK_EQ(rank("fc00:1::1", 0, U, "2001:db8:9::1"), 1); /* all of fc00::/7 */
	CHECK_EQ(rank("fd00:1::1", IFA_F_DEPRECATED, U, "2001:db8:9::1"), 1);
	CHECK_EQ(rank("fd00:1::1", 0, U, "fd00:9::1"), 4); /* ULA to ULA: a stable address */
	CHECK_EQ(rank("fe00::1", 0, U, "2001:db8:9::1"), 4); /* fe00::/8 is not a ULA */
	CHECK_EQ(rank("2001:db8::1", IFA_F_TENTATIVE, U, "2001:db8:9::1"), 0);
	CHECK_EQ(rank("2001:db8::1", IFA_F_DADFAILED, U, "2001:db8:9::1"), 0);
	CHECK_EQ(rank("fe80::1", 0, RT_SCOPE_LINK, "2001:db8:9::1"), 0);
	CHECK_EQ(rank("::1", 0, RT_SCOPE_HOST, "::1"), 0);

	/* A ULA listed before the GUA: the GUA toward a global server, the ULA
	 * toward a ULA one. */
	a[0] = nla("fd12:3456::10", 0, U);
	a[1] = nla("2001:db8:1::10", 0, U);
	CHECK_EQ(pick(a, 2, "2001:db8:ff::1"), 1);
	CHECK_EQ(pick(a, 2, "fd99::1"), 0);
	/* A ULA-only link toward a global server is still usable, last. */
	CHECK_EQ(pick(a, 1, "2001:db8:ff::1"), 0);
	/* Deprecated goes last, stable before temporary, and a deprecated GUA
	 * still beats a ULA toward a GUA. */
	a[0] = nla("2001:db8:1::1", IFA_F_DEPRECATED, U);
	a[1] = nla("2001:db8:1::2", IFA_F_TEMPORARY, U);
	a[2] = nla("2001:db8:1::3", 0, U);
	a[3] = nla("fd12::4", 0, U);
	CHECK_EQ(pick(a, 4, "2001:db8:ff::1"), 2);
	CHECK_EQ(pick(a, 2, "2001:db8:ff::1"), 1);
	CHECK_EQ(pick(a, 1, "2001:db8:ff::1"), 0);
	a[1] = a[3];
	CHECK_EQ(pick(a, 2, "2001:db8:ff::1"), 0);
	/* Tentative, failed DAD and link-local are never picked. */
	a[0] = nla("2001:db8:1::1", IFA_F_TENTATIVE, U);
	a[1] = nla("2001:db8:1::2", IFA_F_DADFAILED, U);
	a[2] = nla("fe80::1", 0, RT_SCOPE_LINK);
	CHECK_EQ(pick(a, 3, "2001:db8:ff::1"), -1);
	CHECK_EQ(cg_src_families(a, 3), 0);
	a[3] = nla("2001:db8:1::4", IFA_F_TEMPORARY, U);
	CHECK_EQ(pick(a, 4, "2001:db8:ff::1"), 3);
	/* Ties go to the first in the kernel's order, the newest address, the
	 * same at startup as at runtime: two stable GUAs, the old prefix still
	 * preferred when a new one comes (flash renumbering). */
	n = 0;
	learn(a, &n, nla("2a0d:3344:bbbb::2", 0, U), 1); /* a dump lists the new one first */
	learn(a, &n, nla("2a0d:3344:aaaa::2", 0, U), 1);
	CHECK(picked(a, n, "2001:19f0::1", "2a0d:3344:bbbb::2"));
	n = 0;
	learn(a, &n, nla("10.0.0.1", 0, U), 1);
	learn(a, &n, nla("10.0.0.2", 0, U), 1);
	learn(a, &n, nla("2a0d:3344:aaaa::2", 0, U), 1); /* running with the old one */
	learn(a, &n, nla("2a0d:3344:bbbb::2", 0, U), 0); /* an event brings the new one */
	CHECK(picked(a, n, "2001:19f0::1", "2a0d:3344:bbbb::2"));
	/* IPv4 keeps the kernel's order both ways, the oldest primary first. */
	CHECK(picked(a, n, "203.0.113.1", "10.0.0.1"));
	learn(a, &n, nla("10.0.0.3", 0, U), 0);
	CHECK(picked(a, n, "203.0.113.1", "10.0.0.1"));
	CHECK_EQ(cg_addr_slot(AF_INET, 0, 3), 3);
	CHECK_EQ(cg_addr_slot(AF_INET6, 1, 3), 3);
	CHECK_EQ(cg_addr_slot(AF_INET6, 0, 3), 0);

	/* IPv4: the primary before secondaries, never loopback, link-local or
	 * 0.0.0.0, and never an address of the other family. */
	a[0] = nla("169.254.1.1", 0, U);
	a[1] = nla("10.0.0.5", IFA_F_SECONDARY, U);
	a[2] = nla("2001:db8:1::1", 0, U);
	a[3] = nla("10.0.0.6", 0, U);
	a[4] = nla("10.0.0.7", 0, U);
	CHECK_EQ(pick(a, 5, "203.0.113.1"), 3);
	CHECK_EQ(pick(a, 3, "203.0.113.1"), 1);
	CHECK_EQ(pick(a, 1, "203.0.113.1"), -1);
	CHECK_EQ(pick(a, 5, "2001:db8:ff::1"), 2);
	a[0] = nla("127.0.0.1", 0, RT_SCOPE_HOST);
	a[1] = nla("0.1.2.3", 0, U);
	CHECK_EQ(pick(a, 2, "203.0.113.1"), -1);
	CHECK(!cg_ipv4_usable((const uint8_t[4]){ 127, 0, 0, 1 }));
	CHECK(!cg_ipv4_usable((const uint8_t[4]){ 169, 254, 9, 9 }));
	CHECK(cg_ipv4_usable((const uint8_t[4]){ 169, 253, 9, 9 }));
	CHECK(cg_ipv4_usable((const uint8_t[4]){ 100, 64, 0, 1 })); /* CGNAT is fine */

	/* The families a link has. */
	a[0] = nla("10.0.0.5", 0, U);
	a[1] = nla("fe80::1", 0, RT_SCOPE_LINK);
	CHECK_EQ(cg_src_families(a, 2), 1u << AF_INET);
	a[2] = nla("fd12::1", 0, U);
	CHECK_EQ(cg_src_families(a, 3), 1u << AF_INET | 1u << AF_INET6);
	CHECK_EQ(cg_src_families(a + 1, 2), 1u << AF_INET6);
	CHECK_EQ(cg_src_families(a, 0), 0);

	/* With a source left out (a socket found no route from it): the next
	 * by rank, ties to the kernel's order. eth1 of the e2e run that failed
	 * (2026-10-11): SLAAC addresses of the first prefixes, whose source
	 * routes netifd put on eth3, newest first, and the DHCPv6 ones, routed
	 * through eth1, after them. */
	{
		struct sockaddr_storage d = dst("2001:db8::4");
		uint32_t skip = 0;
		int k;

		a[0] = nla("fd00:1::5054:ff:fe00:101", 0, U);
		a[1] = nla("2001:db8:1:0:5054:ff:fe00:101", 0, U);
		a[2] = nla("2001:db8:100:0:5054:ff:fe00:101", 0, U);
		a[3] = nla("fd00:1::acd", 0, U);
		a[4] = nla("2001:db8:1::acd", 0, U);
		a[5] = nla("2001:db8:100::acd", 0, U);
		a[6] = nla("fe80::5054:ff:fe00:101", 0, RT_SCOPE_LINK);
		a[7] = nla("10.1.1.104", 0, U);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 0), 1);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 1u << 1), 2);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 1u << 1 | 1u << 2), 4);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 1u << 1 | 1u << 2 | 1u << 4), 5);
		/* the global ones gone, the ULAs last; never link-local or IPv4 */
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 0x36), 0);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 0x37), 3);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 0x3f), -1);
		/* every usable one in turn, each once, as link_socket goes */
		for (int i = 0; i < 7 && (k = cg_src_pick_skip(a, 8, &d, skip)) >= 0; i++) {
			CHECK(!(skip >> k & 1));
			skip |= 1u << k;
		}
		CHECK_EQ(skip, 0x3fu);
		CHECK_EQ(cg_src_pick_skip(a, 8, &d, 0), cg_src_pick(a, 8, &d));
		/* IPv4 the same way: a secondary after the primary */
		d = dst("1.2.3.4");
		a[0] = nla("10.0.0.5", IFA_F_SECONDARY, U);
		a[1] = nla("10.0.0.6", 0, U);
		CHECK_EQ(cg_src_pick_skip(a, 2, &d, 0), 1);
		CHECK_EQ(cg_src_pick_skip(a, 2, &d, 1u << 1), 0);
		CHECK_EQ(cg_src_pick_skip(a, 2, &d, 3), -1);
	}
	/* What another source may fix, and what it cannot. */
	CHECK(cg_src_noroute(ENETUNREACH));
	CHECK(cg_src_noroute(EHOSTUNREACH));
	CHECK(cg_src_noroute(EADDRNOTAVAIL));
	CHECK(!cg_src_noroute(EPERM));
	CHECK(!cg_src_noroute(EACCES));
	CHECK(!cg_src_noroute(ENODEV));
	CHECK(!cg_src_noroute(EMFILE));
	CHECK(!cg_src_noroute(0));
}
