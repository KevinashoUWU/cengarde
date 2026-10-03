/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_SOCK_H
#define CG_SOCK_H

#include <sys/socket.h>

/* Non-blocking, close-on-exec UDP socket. */
int cg_udp_socket(int family);

/* Sets receive/send buffers, trying the privileged *FORCE variants first so
 * net.core.rmem_max does not cap them. 0 leaves a buffer alone. Returns the
 * effective receive buffer as reported by the kernel. */
int cg_sock_buffers(int fd, int rcvbuf, int sndbuf);

/* UDP socket bound to addr. v6only applies to IPv6 sockets (0 = dual-stack). */
int cg_udp_bind(const struct sockaddr_storage *addr, int v6only, char *err, size_t errlen);

/* UDP socket for one uplink: SO_BINDTODEVICE(ifname), bound to local (port 0)
 * and connected to remote. */
int cg_udp_link(const char *ifname, const struct sockaddr_storage *local, const struct sockaddr_storage *remote,
		char *err, size_t errlen);

#endif
