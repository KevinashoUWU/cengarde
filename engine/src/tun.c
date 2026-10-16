/* The TUN device of the native tunnel: see tun.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tun.h"

/* <linux/ipv6.h>'s struct in6_ifreq, which clashes with <netinet/in.h>. */
struct cg_in6_ifreq {
	struct in6_addr addr;
	uint32_t prefixlen;
	int ifindex;
};

static int tun_addr(const char *name, const struct cg_pfx *p, char *err, size_t errlen)
{
	char a[64];
	int s = socket(p->family, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = -1;

	if (s < 0) {
		snprintf(err, errlen, "tun_addr %s: socket: %s", cg_pfx_str(p, a, sizeof(a)), strerror(errno));
		return -1;
	}
	if (p->family == AF_INET) {
		struct ifreq ifr;
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		uint32_t mask = p->len ? htonl(~0u << (32 - p->len)) : 0;

		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
		sin->sin_family = AF_INET;
		memcpy(&sin->sin_addr, p->a, 4);
		if (ioctl(s, SIOCSIFADDR, &ifr) == 0) {
			sin->sin_addr.s_addr = mask;
			if (ioctl(s, SIOCSIFNETMASK, &ifr) == 0)
				rc = 0;
		}
	} else {
		struct cg_in6_ifreq r;

		memset(&r, 0, sizeof(r));
		memcpy(&r.addr, p->a, 16);
		r.prefixlen = p->len;
		r.ifindex = (int)if_nametoindex(name);
		if (r.ifindex && ioctl(s, SIOCSIFADDR, &r) == 0)
			rc = 0;
	}
	if (rc)
		snprintf(err, errlen, "tun_addr %s on %s: %s", cg_pfx_str(p, a, sizeof(a)), name, strerror(errno));
	close(s);
	return rc;
}

int cg_tun_open(const struct cg_config *cfg, char *err, size_t errlen)
{
	struct ifreq ifr;
	char name[IFNAMSIZ];
	int fd, s = -1;

	fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "/dev/net/tun: %s", strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cfg->tun);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		snprintf(err, errlen, "tun %s: %s", cfg->tun, strerror(errno));
		goto fail;
	}
	memcpy(name, ifr.ifr_name, sizeof(name)); /* the kernel's, should tun have a %d */
	name[IFNAMSIZ - 1] = '\0';
	s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0) {
		snprintf(err, errlen, "tun %s: socket: %s", name, strerror(errno));
		goto fail;
	}
	ifr.ifr_mtu = (int)cfg->tun_mtu;
	if (ioctl(s, SIOCSIFMTU, &ifr) < 0) {
		snprintf(err, errlen, "tun %s: MTU %u: %s", name, cfg->tun_mtu, strerror(errno));
		goto fail;
	}
	/* IPv4 addresses before it is up; IPv6 ones after, on a device that
	 * has IPv6 by then. */
	for (int i = 0; i < cfg->ntun_addr; i++)
		if (cfg->tun_addr[i].family == AF_INET && tun_addr(name, &cfg->tun_addr[i], err, errlen) < 0)
			goto fail;
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		snprintf(err, errlen, "tun %s: flags: %s", name, strerror(errno));
		goto fail;
	}
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		snprintf(err, errlen, "tun %s: up: %s", name, strerror(errno));
		goto fail;
	}
	for (int i = 0; i < cfg->ntun_addr; i++)
		if (cfg->tun_addr[i].family == AF_INET6 && tun_addr(name, &cfg->tun_addr[i], err, errlen) < 0)
			goto fail;
	close(s);
	return fd;
fail:
	if (s >= 0)
		close(s);
	close(fd);
	return -1;
}

int cg_tun_rx(int fd, struct cg_rxbatch *b)
{
	int n = 0;

	while (n < CG_BATCH) {
		ssize_t r = read(fd, b->buf[n], CG_BUF);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || n)
				break;
			return -1;
		}
		b->msg[n].msg_len = (unsigned)r;
		b->msg[n].msg_hdr.msg_flags = r >= CG_BUF ? MSG_TRUNC : 0;
		n++;
	}
	return n;
}

int cg_tun_tx(int fd, const void *pkt, size_t len)
{
	return write(fd, pkt, len) == (ssize_t)len ? 0 : -1;
}
