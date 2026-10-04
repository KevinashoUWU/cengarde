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
	/* The first seen breaks ties. */
	a[0] = nla("2001:db8:1::1", 0, U);
	a[1] = nla("2001:db8:1::2", 0, U);
	CHECK_EQ(pick(a, 2, "2001:db8:ff::1"), 0);

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
}
