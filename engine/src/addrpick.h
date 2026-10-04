/* Client side: which local address a link sends from.
 *
 * IPv4: the primary address of the interface before its secondaries, never
 * loopback, link-local or 0.0.0.0; the first seen among equals.
 *
 * IPv6 follows RFC 6724 (source address selection) as far as a link can
 * tell. Linux gives ULAs (fc00::/7) global scope, so netlink does not tell
 * them from global addresses: by scope alone, a ULA listed before the GUA
 * was picked, and the link sent toward a global server from an address with
 * no route there. Ranks (cg_ip6_src_rank), the highest wins and the first
 * seen breaks ties:
 *
 *   0  unusable: tentative, failed DAD, or not global scope (link-local, host)
 *   1  a ULA toward a destination that is not a ULA: last but not banned (in
 *      RFC 6724 the label mismatch is Rule 6, a preference); a CPE doing
 *      NAT66 for a ULA-only LAN still works, and server failover
 *      (srvpick.h) covers a ULA with no way out
 *   2  deprecated (Rule 3)
 *   3  temporary
 *   4  stable: before temporary, as an application may ask (RFC 5014,
 *      IPV6_PREFER_SRC_PUBLIC), because a temporary address expires within
 *      a day and would move the path with it
 *
 * Pure functions over the addresses netlink reported, in the order it did
 * (netlink.c keeps that order); no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_ADDRPICK_H
#define CG_ADDRPICK_H

#include <linux/if_addr.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>

#include "netlink.h"

/* An IPv4 address (network order) a link can send from. */
static inline int cg_ipv4_usable(const uint8_t a[4])
{
	return a[0] != 127 && !(a[0] == 169 && a[1] == 254) && a[0] != 0;
}

static inline int cg_ip6_ula(const uint8_t a[16])
{
	return (a[0] & 0xfe) == 0xfc;
}

/* An IPv6 address a link can send from at all, whatever the destination. */
static inline int cg_ip6_src_usable(uint32_t ifa_flags, uint8_t scope)
{
	return scope == RT_SCOPE_UNIVERSE && !(ifa_flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED));
}

/* Rank of addr (IFA_F_* flags, RT_SCOPE_* scope) as the source toward dst,
 * from 0 (unusable) to 4 (stable): the table above. */
static inline int cg_ip6_src_rank(const uint8_t addr[16], uint32_t ifa_flags, uint8_t scope, const uint8_t dst[16])
{
	if (!cg_ip6_src_usable(ifa_flags, scope))
		return 0;
	if (cg_ip6_ula(addr) && !cg_ip6_ula(dst))
		return 1;
	if (ifa_flags & IFA_F_DEPRECATED)
		return 2;
	return ifa_flags & IFA_F_TEMPORARY ? 3 : 4;
}

/* The source for dst (its family and, for IPv6, its address) among the n
 * addresses of an interface: an index into a, or -1 when none is usable. */
static inline int cg_src_pick(const struct cg_nl_addr *a, int n, const struct sockaddr_storage *dst)
{
	const uint8_t *d6 = ((const struct sockaddr_in6 *)dst)->sin6_addr.s6_addr;
	int best = -1, best_rank = 0;

	for (int i = 0; i < n; i++) {
		int r;

		if (a[i].family != dst->ss_family)
			continue;
		if (a[i].family == AF_INET)
			r = !cg_ipv4_usable(a[i].addr) ? 0 : a[i].flags & IFA_F_SECONDARY ? 1 : 2;
		else if (a[i].family == AF_INET6)
			r = cg_ip6_src_rank(a[i].addr, a[i].flags, a[i].scope, d6);
		else
			continue;
		if (r > best_rank) {
			best = i;
			best_rank = r;
		}
	}
	return best;
}

/* The families an interface has a usable address of, as 1 << AF_INET and
 * 1 << AF_INET6 (what srvpick.h filters the server list by). */
static inline unsigned cg_src_families(const struct cg_nl_addr *a, int n)
{
	unsigned m = 0;

	for (int i = 0; i < n; i++) {
		if (a[i].family == AF_INET && cg_ipv4_usable(a[i].addr))
			m |= 1u << AF_INET;
		else if (a[i].family == AF_INET6 && cg_ip6_src_usable(a[i].flags, a[i].scope))
			m |= 1u << AF_INET6;
	}
	return m;
}

#endif
