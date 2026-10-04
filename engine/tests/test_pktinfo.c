/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <string.h>

#include "pktinfo.h"
#include "test.h"

/* A control buffer as recvmsg leaves it, written by hand: off bytes into
 * buf (to try one that is not aligned), each message at a CMSG_ALIGN step. */
struct ctl {
	uint8_t buf[160];
	size_t off, len;
};

static void put(struct ctl *c, int level, int type, const void *data, size_t dlen, size_t cmsg_len)
{
	struct cmsghdr h;

	memset(&h, 0, sizeof(h));
	h.cmsg_len = cmsg_len;
	h.cmsg_level = level;
	h.cmsg_type = type;
	memcpy(c->buf + c->off + c->len, &h, sizeof(h));
	memcpy(c->buf + c->off + c->len + CG_CMSG_DATA, data, dlen);
	c->len += CMSG_ALIGN(cmsg_len);
}

static void put6(struct ctl *c, const char *ip)
{
	struct in6_pktinfo pi = { .ipi6_ifindex = 3 };

	inet_pton(AF_INET6, ip, &pi.ipi6_addr);
	put(c, IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof(pi), CMSG_LEN(sizeof(pi)));
}

/* What the kernel writes for IPv4: spec_dst the route's local address, addr
 * the packet's destination. */
static void put4(struct ctl *c, const char *spec_dst, const char *addr)
{
	struct in_pktinfo pi = { .ipi_ifindex = 3 };

	inet_pton(AF_INET, spec_dst, &pi.ipi_spec_dst);
	inet_pton(AF_INET, addr, &pi.ipi_addr);
	put(c, IPPROTO_IP, IP_PKTINFO, &pi, sizeof(pi), CMSG_LEN(sizeof(pi)));
}

static int parse(struct ctl *c, int flags, struct cg_local *out)
{
	struct msghdr m;

	memset(&m, 0, sizeof(m));
	m.msg_control = c->buf + c->off;
	m.msg_controllen = c->len;
	m.msg_flags = flags;
	return cg_local_from_msg(&m, out);
}

/* l as "a.b.c.d" or an IPv6 string, via cg_addr_str. */
static const char *str(const struct cg_local *l, char *buf, size_t len)
{
	struct sockaddr_storage a;

	cg_local_sockaddr(l, htons(59402), &a);
	return cg_addr_str(&a, buf, len);
}

static struct cg_local local(const char *ip)
{
	struct cg_local l = { .known = 1 };

	if (inet_pton(AF_INET6, ip, l.addr) != 1) {
		l.addr[10] = l.addr[11] = 0xff;
		inet_pton(AF_INET, ip, l.addr + 12);
	}
	return l;
}

static struct sockaddr_storage peer(const char *ip, int port)
{
	struct sockaddr_storage a;
	char err[128];

	cg_addr_parse(ip, 0, &a, 1, err, sizeof(err));
	if (port) {
		if (a.ss_family == AF_INET)
			((struct sockaddr_in *)&a)->sin_port = htons((uint16_t)port);
		else
			((struct sockaddr_in6 *)&a)->sin6_port = htons((uint16_t)port);
	}
	return a;
}

/* An IPv4 peer as a dual-stack socket reports it, ::ffff:a.b.c.d. */
static struct sockaddr_storage mapped(const char *ip, int port)
{
	struct sockaddr_storage a;
	struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;

	memset(&a, 0, sizeof(a));
	a6->sin6_family = AF_INET6;
	a6->sin6_port = htons((uint16_t)port);
	a6->sin6_addr.s6_addr[10] = a6->sin6_addr.s6_addr[11] = 0xff;
	inet_pton(AF_INET, ip, &a6->sin6_addr.s6_addr[12]);
	return a;
}

static void test_parse(void)
{
	struct ctl c;
	struct cg_local l;
	char buf[64];
	int ttl = 64;

	/* Native and v4-mapped IPV6_PKTINFO, IP_PKTINFO (the destination). */
	memset(&c, 0, sizeof(c));
	put6(&c, "2001:db8::7");
	CHECK(parse(&c, 0, &l) && l.known);
	CHECK(!strcmp(str(&l, buf, sizeof(buf)), "[2001:db8::7]:59402"));
	CHECK(!cg_local_is4(&l));
	memset(&c, 0, sizeof(c));
	put6(&c, "::ffff:198.51.100.7");
	CHECK(parse(&c, 0, &l) && cg_local_is4(&l));
	CHECK(!strcmp(str(&l, buf, sizeof(buf)), "198.51.100.7:59402"));
	memset(&c, 0, sizeof(c));
	put4(&c, "10.0.1.2", "10.0.1.20");
	CHECK(parse(&c, 0, &l) && cg_local_is4(&l));
	CHECK(!strcmp(str(&l, buf, sizeof(buf)), "10.0.1.20:59402"));

	/* An unrelated message first, in a buffer that is not aligned. */
	memset(&c, 0, sizeof(c));
	c.off = 1;
	put(&c, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), CMSG_LEN(sizeof(ttl)));
	put4(&c, "10.0.3.2", "10.0.3.2");
	CHECK(parse(&c, 0, &l) && !strcmp(str(&l, buf, sizeof(buf)), "10.0.3.2:59402"));
	memset(&c, 0, sizeof(c));
	c.off = 3;
	put(&c, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), CMSG_LEN(sizeof(ttl)));
	put6(&c, "2001:db8::9");
	CHECK(parse(&c, 0, &l) && !strcmp(str(&l, buf, sizeof(buf)), "[2001:db8::9]:59402"));

	/* MSG_CTRUNC teaches nothing, even with a whole message in place. */
	CHECK(!parse(&c, MSG_CTRUNC, &l) && !l.known);
	/* No control at all. */
	memset(&c, 0, sizeof(c));
	CHECK(!parse(&c, 0, &l) && !l.known);

	/* A cmsg_len too short for the address, shorter than a header, past
	 * the end of the buffer, or a buffer cut inside the header. */
	memset(&c, 0, sizeof(c));
	{
		struct in6_pktinfo pi = { .ipi6_ifindex = 1 };

		inet_pton(AF_INET6, "2001:db8::7", &pi.ipi6_addr);
		put(&c, IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof(pi), CMSG_LEN(sizeof(pi) - 1));
	}
	CHECK(!parse(&c, 0, &l) && !l.known);
	memset(&c, 0, sizeof(c));
	put6(&c, "2001:db8::7");
	{
		struct cmsghdr h;

		memcpy(&h, c.buf, sizeof(h));
		h.cmsg_len = sizeof(h) - 1;
		memcpy(c.buf, &h, sizeof(h));
		CHECK(!parse(&c, 0, &l) && !l.known);
		h.cmsg_len = c.len + 1;
		memcpy(c.buf, &h, sizeof(h));
		CHECK(!parse(&c, 0, &l) && !l.known);
	}
	memset(&c, 0, sizeof(c));
	put6(&c, "2001:db8::7");
	c.len = sizeof(struct cmsghdr) - 1;
	CHECK(!parse(&c, 0, &l) && !l.known);
	/* An unrelated message alone. */
	memset(&c, 0, sizeof(c));
	put(&c, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), CMSG_LEN(sizeof(ttl)));
	CHECK(!parse(&c, 0, &l) && !l.known);

	/* Addresses a reply cannot leave from without an interface. */
	static const char *const bad6[] = { "::", "fe80::1", "febf::1", "ff02::1", "::ffff:0.0.0.0",
					    "::ffff:224.0.0.1", "::ffff:255.255.255.255" };
	for (unsigned i = 0; i < CG_ARRAY_SIZE(bad6); i++) {
		memset(&c, 0, sizeof(c));
		put6(&c, bad6[i]);
		CHECK(!parse(&c, 0, &l) && !l.known);
	}
	memset(&c, 0, sizeof(c));
	put4(&c, "10.0.1.2", "255.255.255.255");
	CHECK(!parse(&c, 0, &l) && !l.known);
	memset(&c, 0, sizeof(c));
	put6(&c, "fec0::1"); /* site-local (deprecated), not link-local */
	CHECK(parse(&c, 0, &l) && l.known);
	memset(&c, 0, sizeof(c));
	put6(&c, "fd00::1"); /* ULA */
	CHECK(parse(&c, 0, &l) && l.known);
}

static void test_build(void)
{
	union cg_ctl_tx tx;
	struct cmsghdr h;
	struct cg_local l, back;
	struct ctl c;
	char buf[64];
	size_t n;

	/* IPv6 on an AF_INET6 socket: exactly CMSG_LEN(20), within the
	 * kernel's stack buffer, ifindex 0. */
	l = local("2001:db8::7");
	n = cg_local_cmsg(&l, AF_INET6, &tx);
	CHECK_EQ(n, CMSG_LEN(sizeof(struct in6_pktinfo)));
	CHECK(n <= sizeof(struct cmsghdr) + 20);
	memcpy(&h, tx.b, sizeof(h));
	CHECK_EQ(h.cmsg_len, n);
	CHECK_EQ(h.cmsg_level, IPPROTO_IPV6);
	CHECK_EQ(h.cmsg_type, IPV6_PKTINFO);
	{
		struct in6_pktinfo pi;

		memcpy(&pi, tx.b + CG_CMSG_DATA, sizeof(pi));
		CHECK_EQ(pi.ipi6_ifindex, 0);
		CHECK(!memcmp(&pi.ipi6_addr, l.addr, 16));
	}
	memset(&c, 0, sizeof(c));
	memcpy(c.buf, tx.b, n);
	c.len = n;
	CHECK(parse(&c, 0, &back) && !memcmp(back.addr, l.addr, 16));

	/* IPv4 on a dual-stack socket: IPV6_PKTINFO with the v4-mapped one. */
	l = local("198.51.100.7");
	n = cg_local_cmsg(&l, AF_INET6, &tx);
	CHECK_EQ(n, CMSG_LEN(sizeof(struct in6_pktinfo)));
	memset(&c, 0, sizeof(c));
	memcpy(c.buf, tx.b, n);
	c.len = n;
	CHECK(parse(&c, 0, &back) && !strcmp(str(&back, buf, sizeof(buf)), "198.51.100.7:59402"));

	/* IPv4 on an AF_INET socket: IP_PKTINFO from ipi_spec_dst. */
	n = cg_local_cmsg(&l, AF_INET, &tx);
	CHECK_EQ(n, CMSG_LEN(sizeof(struct in_pktinfo)));
	memcpy(&h, tx.b, sizeof(h));
	CHECK_EQ(h.cmsg_len, CMSG_LEN(12));
	CHECK_EQ(h.cmsg_level, IPPROTO_IP);
	CHECK_EQ(h.cmsg_type, IP_PKTINFO);
	{
		struct in_pktinfo pi;
		char ip[INET_ADDRSTRLEN];

		memcpy(&pi, tx.b + CG_CMSG_DATA, sizeof(pi));
		CHECK_EQ(pi.ipi_ifindex, 0);
		CHECK(!strcmp(inet_ntop(AF_INET, &pi.ipi_spec_dst, ip, sizeof(ip)), "198.51.100.7"));
	}
	memset(&c, 0, sizeof(c));
	memcpy(c.buf, tx.b, n);
	c.len = n;
	CHECK(parse(&c, 0, &back) && !memcmp(back.addr, l.addr, 16));

	/* Nothing to send: unknown, or IPv6 on an IPv4 socket. */
	l.known = 0;
	CHECK_EQ(cg_local_cmsg(&l, AF_INET6, &tx), 0);
	CHECK_EQ(cg_local_cmsg(&l, AF_INET, &tx), 0);
	l = local("2001:db8::7");
	CHECK_EQ(cg_local_cmsg(&l, AF_INET, &tx), 0);
	CHECK_EQ(cg_local_cmsg(&l, AF_UNSPEC, &tx), 0);
}

static void test_learn(void)
{
	struct sockaddr_storage addr, from;
	struct cg_local cur, got;

	/* A new path: the client's address and ours, no family flip. */
	memset(&addr, 0, sizeof(addr));
	memset(&cur, 0, sizeof(cur));
	from = mapped("10.0.1.1", 40000);
	got = local("10.0.1.20");
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR | CG_PATH_NEW_LOCAL);
	CHECK(cg_addr_equal(&addr, &from) && cur.known && !memcmp(cur.addr, got.addr, 16));
	/* The same again: nothing. */
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), 0);
	/* NAT gave the client another port. */
	from = mapped("10.0.1.1", 40001);
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR);
	CHECK(cg_addr_equal(&addr, &from));
	/* The client sends to another address of ours. */
	got = local("10.0.1.2");
	CHECK_EQ(cg_path_diff(&addr, &cur, &from, &got), CG_PATH_NEW_LOCAL);
	CHECK(memcmp(cur.addr, got.addr, 16)); /* diff writes nothing */
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_LOCAL);
	CHECK(!memcmp(cur.addr, got.addr, 16));
	/* No arrival address (MSG_CTRUNC, or no pktinfo): the known one stays,
	 * also when the port moves. */
	got.known = 0;
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), 0);
	from = mapped("10.0.1.1", 40002);
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR);
	CHECK(cur.known && !memcmp(cur.addr, local("10.0.1.2").addr, 16));

	/* IPv4 to IPv6: a new family, a new address of ours. */
	from = peer("[2001:db8:1::5]:40000", 0);
	got = local("2001:db8::7");
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR | CG_PATH_NEW_LOCAL | CG_PATH_NEW_FAMILY);
	CHECK(!cg_local_is4(&cur));
	/* Back to IPv4 without an arrival address: the IPv6 one of ours is of
	 * no use towards an IPv4 client, so it is forgotten. */
	from = mapped("10.0.2.1", 40000);
	got.known = 0;
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR | CG_PATH_NEW_LOCAL | CG_PATH_NEW_FAMILY);
	CHECK(!cur.known);
	/* Learning it later is a new local address, not a move of the client. */
	got = local("198.51.100.7");
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_LOCAL);
	/* An IPv4 socket (plain AF_INET peers) counts the same families. */
	memset(&addr, 0, sizeof(addr));
	memset(&cur, 0, sizeof(cur));
	from = peer("10.0.3.1:40000", 0);
	got = local("10.0.3.2");
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), CG_PATH_NEW_ADDR | CG_PATH_NEW_LOCAL);
	from = peer("10.0.3.1:40000", 0);
	CHECK_EQ(cg_path_learn(&addr, &cur, &from, &got), 0);
	CHECK_EQ(cg_addr_is4(&from), 1);
	from = mapped("10.0.3.1", 40000);
	CHECK_EQ(cg_addr_is4(&from), 1);
	from = peer("[2001:db8::1]:1", 0);
	CHECK_EQ(cg_addr_is4(&from), 0);
	memset(&from, 0, sizeof(from));
	CHECK_EQ(cg_addr_is4(&from), -1);
}

void test_pktinfo(void)
{
	test_parse();
	test_build();
	test_learn();

	/* A full socket stops the batch; our address gone or no route is that
	 * path's; anything else is that datagram's. */
	CHECK_EQ(cg_send_err_kind(EAGAIN), CG_SEND_STOP);
	CHECK_EQ(cg_send_err_kind(EWOULDBLOCK), CG_SEND_STOP);
	CHECK_EQ(cg_send_err_kind(ENOBUFS), CG_SEND_STOP);
	CHECK_EQ(cg_send_err_kind(EINVAL), CG_SEND_LOCAL);
	CHECK_EQ(cg_send_err_kind(ENETUNREACH), CG_SEND_LOCAL);
	CHECK_EQ(cg_send_err_kind(EADDRNOTAVAIL), CG_SEND_LOCAL);
	CHECK_EQ(cg_send_err_kind(EPERM), CG_SEND_OTHER);
	CHECK_EQ(cg_send_err_kind(EMSGSIZE), CG_SEND_OTHER);
}
