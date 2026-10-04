/* Server side: replying from the address each packet arrived at.
 *
 * A server with several addresses (a secondary IPv4, a reserved IP, IPv4 and
 * IPv6 at once, SLAAC and temporary addresses) listens on one wildcard
 * socket. Its replies would leave from whichever address the route prefers,
 * and the client's link socket, connected to the address it sends to, drops
 * them. So the socket asks for the arrival address of every datagram
 * (IPV6_RECVPKTINFO on a dual-stack socket, which reports IPv4 arrivals as
 * v4-mapped, or IP_PKTINFO on an IPv4 one); each path keeps the one its
 * verified packets arrive at, and what goes back to it carries that address
 * as the source: IPV6_PKTINFO with ifindex 0, or IP_PKTINFO's ipi_spec_dst.
 * Any address works this way, including one added while the server runs.
 *
 * Control messages are read and written with memcpy at offsets computed
 * here: musl's CMSG_NXTHDR warns under -Wextra, and nothing says a control
 * buffer is aligned for the structures in it. Pure functions, no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PKTINFO_H
#define CG_PKTINFO_H

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "util.h"

/* An address of ours, IPv4 stored as ::ffff:a.b.c.d. */
struct cg_local {
	uint8_t known;
	uint8_t addr[16];
};

/* Receive control buffer of one datagram: the arrival address takes
 * CMSG_SPACE(20), 40 bytes (32 on 32-bit). */
#define CG_CTL_RX_LEN 64
union cg_ctl_rx {
	struct cmsghdr align;
	uint8_t b[CG_CTL_RX_LEN];
};

/* Send control buffer: one IPV6_PKTINFO or IP_PKTINFO. */
union cg_ctl_tx {
	struct cmsghdr align;
	uint8_t b[CMSG_SPACE(sizeof(struct in6_pktinfo))];
};

_Static_assert(CMSG_SPACE(sizeof(struct in6_pktinfo)) <= CG_CTL_RX_LEN, "arrival address does not fit");
/* net/socket.c (____sys_sendmsg, v5.10 and v6.12) copies up to
 * sizeof(struct cmsghdr) + 20 bytes of control into a buffer on its stack;
 * a longer msg_controllen costs a sock_kmalloc on every sendmsg. The
 * control message for IPv6 is sent with exactly CMSG_LEN(20) for that. */
_Static_assert(CMSG_LEN(sizeof(struct in6_pktinfo)) <= sizeof(struct cmsghdr) + 20,
	       "IPV6_PKTINFO no longer fits the kernel's stack buffer");

/* Where the data of a control message starts, past its header. */
#define CG_CMSG_DATA CMSG_ALIGN(sizeof(struct cmsghdr))

/* Is a (16 bytes) ::ffff:a.b.c.d? */
static inline int cg_is_v4mapped(const uint8_t *a)
{
	static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

	return !memcmp(a, prefix, sizeof(prefix));
}

static inline int cg_local_is4(const struct cg_local *l)
{
	return cg_is_v4mapped(l->addr);
}

/* Can a datagram leave from this address with no interface given? Not from
 * the unspecified address, a multicast or broadcast one, nor an IPv6
 * link-local one (it needs an interface): for those the kernel picks. */
static inline int cg_local_usable(const uint8_t a[16])
{
	if (cg_is_v4mapped(a)) {
		uint32_t ip = (uint32_t)a[12] << 24 | (uint32_t)a[13] << 16 | (uint32_t)a[14] << 8 | a[15];

		return ip != 0 && (ip & 0xf0000000u) != 0xe0000000u && ip != 0xffffffffu;
	}
	if (a[0] == 0xff || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80))
		return 0;
	for (int i = 0; i < 16; i++)
		if (a[i])
			return 1;
	return 0;
}

/* One control message: is it the arrival address? data: dlen bytes. */
static inline int cg_cmsg_local(int level, int type, const uint8_t *data, size_t dlen, struct cg_local *out)
{
	if (level == IPPROTO_IPV6 && type == IPV6_PKTINFO && dlen >= sizeof(struct in6_pktinfo)) {
		struct in6_pktinfo pi;

		memcpy(&pi, data, sizeof(pi));
		memcpy(out->addr, &pi.ipi6_addr, 16);
	} else if (level == IPPROTO_IP && type == IP_PKTINFO && dlen >= sizeof(struct in_pktinfo)) {
		struct in_pktinfo pi;

		/* ipi_addr is the packet's destination, what the client sends
		 * to (ipi_spec_dst is the same for a local unicast one). */
		memcpy(&pi, data, sizeof(pi));
		memset(out->addr, 0, 10);
		out->addr[10] = out->addr[11] = 0xff;
		memcpy(out->addr + 12, &pi.ipi_addr, 4);
	} else {
		return 0;
	}
	out->known = (uint8_t)cg_local_usable(out->addr);
	return 1;
}

/* The address a datagram arrived at, from the control messages recvmsg
 * left in m (msg_control, msg_controllen and msg_flags): an IPV6_PKTINFO,
 * native or v4-mapped, or an IP_PKTINFO. Returns 1 when out->known. A cut
 * control buffer (MSG_CTRUNC) or a malformed one teaches nothing.
 *
 * The arrival address is the only control message the server asks for, so
 * the first one is normally it; the walk goes on past any other. */
static inline int cg_local_from_msg(const struct msghdr *m, struct cg_local *out)
{
	const uint8_t *c = m->msg_control;
	size_t end = m->msg_controllen, off = 0;

	out->known = 0;
	if (!c || (m->msg_flags & MSG_CTRUNC))
		return 0;
	while (off < end && end - off >= sizeof(struct cmsghdr)) {
		struct cmsghdr h;
		size_t len;

		memcpy(&h, c + off, sizeof(h));
		len = (size_t)h.cmsg_len;
		if (len < CG_CMSG_DATA || len > end - off)
			return 0;
		if (cg_cmsg_local(h.cmsg_level, h.cmsg_type, c + off + CG_CMSG_DATA, len - CG_CMSG_DATA, out))
			return out->known;
		off += CMSG_ALIGN(len);
	}
	return 0;
}

/* Writes the control message that makes a datagram leave from l on a socket
 * of family, and returns the msg_controllen to send it with: IPV6_PKTINFO
 * (ifindex 0) on an AF_INET6 socket, IP_PKTINFO (ipi_spec_dst) on an
 * AF_INET one. 0, no control message (the kernel picks the source), when l
 * is unknown or IPv6 on an IPv4 socket. */
static inline size_t cg_local_cmsg(const struct cg_local *l, int family, union cg_ctl_tx *out)
{
	struct cmsghdr h;

	memset(out, 0, sizeof(*out));
	memset(&h, 0, sizeof(h)); /* musl pads cmsg_len to the kernel's size_t */
	if (!l->known)
		return 0;
	if (family == AF_INET6) {
		struct in6_pktinfo pi;

		memset(&pi, 0, sizeof(pi));
		memcpy(&pi.ipi6_addr, l->addr, 16);
		h.cmsg_len = CMSG_LEN(sizeof(pi));
		h.cmsg_level = IPPROTO_IPV6;
		h.cmsg_type = IPV6_PKTINFO;
		memcpy(out->b, &h, sizeof(h));
		memcpy(out->b + CG_CMSG_DATA, &pi, sizeof(pi));
		return CMSG_LEN(sizeof(pi));
	}
	if (family == AF_INET && cg_local_is4(l)) {
		struct in_pktinfo pi;

		/* The kernel sends from ipi_spec_dst and ignores ipi_addr, set
		 * too so that the message reads back as the same address. */
		memset(&pi, 0, sizeof(pi));
		memcpy(&pi.ipi_spec_dst, l->addr + 12, 4);
		memcpy(&pi.ipi_addr, l->addr + 12, 4);
		h.cmsg_len = CMSG_LEN(sizeof(pi));
		h.cmsg_level = IPPROTO_IP;
		h.cmsg_type = IP_PKTINFO;
		memcpy(out->b, &h, sizeof(h));
		memcpy(out->b + CG_CMSG_DATA, &pi, sizeof(pi));
		return CMSG_LEN(sizeof(pi));
	}
	return 0;
}

/* l with port (network order) as a socket address, for cg_addr_str (which
 * prints a v4-mapped one as IPv4). */
static inline void cg_local_sockaddr(const struct cg_local *l, uint16_t port, struct sockaddr_storage *out)
{
	struct sockaddr_in6 *a = (struct sockaddr_in6 *)out;

	memset(out, 0, sizeof(*out));
	a->sin6_family = AF_INET6;
	a->sin6_port = port;
	memcpy(&a->sin6_addr, l->addr, 16);
}

/* 1 for IPv4 (plain or v4-mapped), 0 for IPv6, -1 for no address yet. */
static inline int cg_addr_is4(const struct sockaddr_storage *a)
{
	if (a->ss_family == AF_INET)
		return 1;
	if (a->ss_family != AF_INET6)
		return -1;
	return cg_is_v4mapped(((const struct sockaddr_in6 *)a)->sin6_addr.s6_addr);
}

#define CG_PATH_NEW_ADDR 1u   /* the client's address or port changed (or the path is new) */
#define CG_PATH_NEW_LOCAL 2u  /* the address of ours it sends to changed */
#define CG_PATH_NEW_FAMILY 4u /* the client went from IPv4 to IPv6 or back */

/* What a verified packet from `from`, which arrived at got, changes on a
 * path where the client is at *addr and sends to *local; nothing is written.
 * An unknown arrival address never replaces a known one, except one of the
 * other family, which the path could not send from any more: that one is
 * forgotten. */
static inline unsigned cg_path_diff(const struct sockaddr_storage *addr, const struct cg_local *local,
				    const struct sockaddr_storage *from, const struct cg_local *got)
{
	unsigned ch = 0;
	int was4 = cg_addr_is4(addr), is4 = cg_addr_is4(from);

	if (!cg_addr_equal(addr, from))
		ch |= CG_PATH_NEW_ADDR;
	if (was4 >= 0 && was4 != is4)
		ch |= CG_PATH_NEW_FAMILY;
	if (got->known) {
		if (!local->known || memcmp(local->addr, got->addr, 16))
			ch |= CG_PATH_NEW_LOCAL;
	} else if (local->known && cg_local_is4(local) != is4) {
		ch |= CG_PATH_NEW_LOCAL;
	}
	return ch;
}

/* Applies cg_path_diff and returns what it said. */
static inline unsigned cg_path_learn(struct sockaddr_storage *addr, struct cg_local *local,
				     const struct sockaddr_storage *from, const struct cg_local *got)
{
	unsigned ch = cg_path_diff(addr, local, from, got);

	if (ch & CG_PATH_NEW_ADDR)
		*addr = *from;
	if (ch & CG_PATH_NEW_LOCAL) {
		if (got->known)
			*local = *got;
		else
			local->known = 0;
	}
	return ch;
}

/* What a failed send says about the rest of a batch. */
enum cg_send_err {
	CG_SEND_STOP,  /* the socket is full: the rest would fail too */
	CG_SEND_LOCAL, /* our address is gone or there is no route: this path only */
	CG_SEND_OTHER, /* something about this datagram (a firewall, its size) */
};

static inline enum cg_send_err cg_send_err_kind(int err)
{
	switch (err) {
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case ENOBUFS:
		return CG_SEND_STOP;
	case EINVAL:
	case ENETUNREACH:
	case EADDRNOTAVAIL:
		return CG_SEND_LOCAL;
	default:
		return CG_SEND_OTHER;
	}
}

#endif
