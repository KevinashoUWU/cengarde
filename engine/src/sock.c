/* SPDX-License-Identifier: GPL-2.0-only */
#include "sock.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int cg_udp_socket(int family)
{
	return socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

int cg_sock_buffers(int fd, int rcvbuf, int sndbuf)
{
	int v = 0;
	socklen_t len = sizeof(v);

	if (rcvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	if (sndbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &sndbuf, sizeof(sndbuf)) < 0)
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &len);
	return v;
}

int cg_udp_bind(const struct sockaddr_storage *addr, int v6only, char *err, size_t errlen)
{
	char buf[64];
	struct sockaddr_storage any4;
	int fd = cg_udp_socket(addr->ss_family);

	/* "*" means dual-stack; on a kernel booted without IPv6 fall back to IPv4. */
	if (fd < 0 && errno == EAFNOSUPPORT && addr->ss_family == AF_INET6 &&
	    IN6_IS_ADDR_UNSPECIFIED(&((const struct sockaddr_in6 *)addr)->sin6_addr)) {
		struct sockaddr_in *a4 = (struct sockaddr_in *)&any4;

		memset(&any4, 0, sizeof(any4));
		a4->sin_family = AF_INET;
		a4->sin_addr.s_addr = htonl(INADDR_ANY);
		a4->sin_port = ((const struct sockaddr_in6 *)addr)->sin6_port;
		addr = &any4;
		fd = cg_udp_socket(AF_INET);
	}
	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	if (addr->ss_family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
	if (bind(fd, (const struct sockaddr *)addr, cg_addr_len(addr)) < 0) {
		snprintf(err, errlen, "bind %s: %s", cg_addr_str(addr, buf, sizeof(buf)), strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int cg_udp_link(const char *ifname, const struct sockaddr_storage *local, const struct sockaddr_storage *remote,
		char *err, size_t errlen)
{
	char buf[64];
	int fd = cg_udp_socket(local->ss_family);

	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname)) < 0) {
		snprintf(err, errlen, "SO_BINDTODEVICE %s: %s (needs CAP_NET_RAW on old kernels)", ifname,
			 strerror(errno));
		goto fail;
	}
	if (bind(fd, (const struct sockaddr *)local, cg_addr_len(local)) < 0) {
		snprintf(err, errlen, "bind %s: %s", cg_addr_str(local, buf, sizeof(buf)), strerror(errno));
		goto fail;
	}
	/* Connected: the route is cached and only the server's replies get in. */
	if (connect(fd, (const struct sockaddr *)remote, cg_addr_len(remote)) < 0) {
		snprintf(err, errlen, "connect %s: %s", cg_addr_str(remote, buf, sizeof(buf)), strerror(errno));
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return -1;
}
