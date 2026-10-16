/* The native tunnel's routes on the server: which client an IP address
 * belongs to.
 *
 * The server has one TUN device for every client (tunnel = native). A
 * packet the kernel routes into it goes to the client whose prefixes hold
 * its destination, by longest prefix, as WireGuard's AllowedIPs: each
 * [client NAME] lists its tunnel address and whatever networks sit behind
 * the router (tun_route = 10.79.12.34/32 192.168.1.0/24 fd12:3456::/64).
 * The same table checks the other way, as AllowedIPs does: a client's
 * packet goes into the TUN only when its source is one of that client's,
 * so a router cannot pass itself off as another (the MAC says which client
 * sent it, not which addresses it may use). A server with one client and
 * no tun_route takes 0.0.0.0/0 and ::/0 for it: everything is its.
 *
 * Entries are kept longest prefix first, so a lookup is a walk that stops
 * at the first match: fine for the tens of entries a server has. Two
 * clients with the same prefix are refused when the configuration loads
 * (cg_rt_clash).
 *
 * Pure functions, no I/O but inet_pton.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TUNROUTE_H
#define CG_TUNROUTE_H

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "clients.h" /* CG_MAX_CLIENTS */

#define CG_TUN_ROUTES 8 /* prefixes per client */
#define CG_RT_MAX (CG_MAX_CLIENTS * CG_TUN_ROUTES)

/* A prefix (or an address with its prefix length, for tun_addr). */
struct cg_pfx {
	uint8_t family; /* AF_INET or AF_INET6 */
	uint8_t len;    /* prefix length */
	uint8_t a[16];  /* the address, 4 or 16 bytes, network order */
};

/* Parses "ADDR/LEN" or a bare ADDR (a host: /32 or /128) into p. With
 * host_bits 0 the bits past the length must be zero (a route); with 1 they
 * may be anything (an interface address, 10.79.0.1/16). 0 or -1. */
static inline int cg_pfx_parse(const char *s, struct cg_pfx *p, int host_bits)
{
	char buf[INET6_ADDRSTRLEN + 8];
	const char *slash = strchr(s, '/');
	size_t alen = slash ? (size_t)(slash - s) : strlen(s);
	unsigned max;

	memset(p, 0, sizeof(*p));
	if (!alen || alen >= INET6_ADDRSTRLEN)
		return -1;
	memcpy(buf, s, alen);
	buf[alen] = '\0';
	if (inet_pton(AF_INET, buf, p->a) == 1)
		p->family = AF_INET, max = 32;
	else if (inet_pton(AF_INET6, buf, p->a) == 1)
		p->family = AF_INET6, max = 128;
	else
		return -1;
	p->len = (uint8_t)max;
	if (slash) {
		char *end;
		unsigned long l;

		if (!slash[1] || slash[1] < '0' || slash[1] > '9')
			return -1;
		l = strtoul(slash + 1, &end, 10);
		if (*end || l > max)
			return -1;
		p->len = (uint8_t)l;
	}
	if (!host_bits)
		for (unsigned b = p->len; b < max; b++)
			if (p->a[b / 8] >> (7 - b % 8) & 1)
				return -1;
	return 0;
}

/* Whether addr (family af) is inside p. */
static inline int cg_pfx_match(const struct cg_pfx *p, int af, const uint8_t *addr)
{
	unsigned full = p->len / 8, rest = p->len % 8;

	if (p->family != af || memcmp(p->a, addr, full))
		return 0;
	return !rest || !((p->a[full] ^ addr[full]) & (uint8_t)(0xff << (8 - rest)));
}

static inline int cg_pfx_equal(const struct cg_pfx *x, const struct cg_pfx *y)
{
	return x->family == y->family && x->len == y->len && !memcmp(x->a, y->a, x->family == AF_INET ? 4 : 16);
}

/* "ADDR/LEN" of p in out. */
static inline const char *cg_pfx_str(const struct cg_pfx *p, char *out, size_t outlen)
{
	char a[INET6_ADDRSTRLEN];

	if (!inet_ntop(p->family, p->a, a, sizeof(a)))
		snprintf(a, sizeof(a), "?");
	snprintf(out, outlen, "%s/%u", a, p->len);
	return out;
}

struct cg_rt {
	int n;
	struct cg_pfx p[CG_RT_MAX];
	uint8_t client[CG_RT_MAX];
};

/* Adds p for client, after the entries of a longer prefix and those of the
 * same length added before it. 0, or -1 when the table is full. */
static inline int cg_rt_add(struct cg_rt *rt, const struct cg_pfx *p, uint8_t client)
{
	int at = rt->n;

	if (rt->n == CG_RT_MAX)
		return -1;
	while (at > 0 && rt->p[at - 1].len < p->len) {
		rt->p[at] = rt->p[at - 1];
		rt->client[at] = rt->client[at - 1];
		at--;
	}
	rt->p[at] = *p;
	rt->client[at] = client;
	rt->n++;
	return 0;
}

/* The client addr (family af) belongs to, by longest prefix; -1: none. */
static inline int cg_rt_lookup(const struct cg_rt *rt, int af, const uint8_t *addr)
{
	for (int i = 0; i < rt->n; i++)
		if (cg_pfx_match(&rt->p[i], af, addr))
			return rt->client[i];
	return -1;
}

/* The first entry of a different client with the same prefix as entry i,
 * or -1: two clients cannot own one prefix. */
static inline int cg_rt_clash(const struct cg_rt *rt, int i)
{
	for (int j = 0; j < rt->n; j++)
		if (j != i && rt->client[j] != rt->client[i] && cg_pfx_equal(&rt->p[i], &rt->p[j]))
			return j;
	return -1;
}

#endif
