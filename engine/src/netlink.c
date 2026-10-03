/* SPDX-License-Identifier: GPL-2.0-only */
#include "netlink.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

static struct cg_iface *find_index(struct cg_nl *nl, int index)
{
	for (int i = 0; i < nl->nifs; i++)
		if (nl->ifs[i].index == index)
			return &nl->ifs[i];
	return NULL;
}

const struct cg_iface *cg_nl_find(const struct cg_nl *nl, const char *name)
{
	for (int i = 0; i < nl->nifs; i++)
		if (!strcmp(nl->ifs[i].name, name))
			return &nl->ifs[i];
	return NULL;
}

static struct cg_iface *get_index(struct cg_nl *nl, int index)
{
	struct cg_iface *ifc = find_index(nl, index);

	if (ifc || nl->nifs == CG_NL_MAX_IFS)
		return ifc;
	ifc = &nl->ifs[nl->nifs++];
	memset(ifc, 0, sizeof(*ifc));
	ifc->index = index;
	return ifc;
}

static void del_index(struct cg_nl *nl, int index)
{
	struct cg_iface *ifc = find_index(nl, index);

	if (!ifc)
		return;
	*ifc = nl->ifs[--nl->nifs];
	nl->changed = 1;
}

static void on_link(struct cg_nl *nl, struct nlmsghdr *nh)
{
	struct ifinfomsg *ifi = NLMSG_DATA(nh);
	int len = (int)nh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifi));
	struct cg_iface *ifc;
	struct rtattr *rta;

	if (len < 0)
		return;
	if (nh->nlmsg_type == RTM_DELLINK) {
		del_index(nl, ifi->ifi_index);
		return;
	}
	ifc = get_index(nl, ifi->ifi_index);
	if (!ifc)
		return;
	if (ifc->flags != ifi->ifi_flags)
		nl->changed = 1;
	ifc->flags = ifi->ifi_flags;
	for (rta = IFLA_RTA(ifi); RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
		if (rta->rta_type == IFLA_IFNAME) {
			char name[IFNAMSIZ];

			snprintf(name, sizeof(name), "%.*s", (int)RTA_PAYLOAD(rta), (const char *)RTA_DATA(rta));
			if (strcmp(name, ifc->name)) {
				strcpy(ifc->name, name);
				nl->changed = 1;
			}
		}
}

static void on_addr(struct cg_nl *nl, struct nlmsghdr *nh)
{
	struct ifaddrmsg *ifa = NLMSG_DATA(nh);
	int len = (int)nh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifa));
	struct cg_nl_addr a = { .family = ifa->ifa_family, .flags = ifa->ifa_flags, .scope = ifa->ifa_scope };
	const void *addr = NULL, *local = NULL;
	size_t alen = ifa->ifa_family == AF_INET ? 4 : ifa->ifa_family == AF_INET6 ? 16 : 0;
	struct cg_iface *ifc;
	struct rtattr *rta;
	int i;

	if (len < 0 || !alen)
		return;
	for (rta = IFA_RTA(ifa); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type == IFA_ADDRESS && RTA_PAYLOAD(rta) == alen)
			addr = RTA_DATA(rta);
		else if (rta->rta_type == IFA_LOCAL && RTA_PAYLOAD(rta) == alen)
			local = RTA_DATA(rta);
		else if (rta->rta_type == IFA_FLAGS && RTA_PAYLOAD(rta) == 4)
			memcpy(&a.flags, RTA_DATA(rta), 4);
	}
	/* IFA_LOCAL is ours; IFA_ADDRESS is the peer on point-to-point links. */
	if (local)
		addr = local;
	if (!addr)
		return;
	memcpy(a.addr, addr, alen);

	ifc = nh->nlmsg_type == RTM_NEWADDR ? get_index(nl, (int)ifa->ifa_index) : find_index(nl, (int)ifa->ifa_index);
	if (!ifc)
		return;
	for (i = 0; i < ifc->naddr; i++)
		if (ifc->addr[i].family == a.family && !memcmp(ifc->addr[i].addr, a.addr, alen))
			break;
	if (nh->nlmsg_type == RTM_DELADDR) {
		if (i < ifc->naddr) {
			ifc->addr[i] = ifc->addr[--ifc->naddr];
			nl->changed = 1;
		}
		return;
	}
	if (i == ifc->naddr) {
		if (ifc->naddr == CG_NL_MAX_ADDRS)
			return;
		ifc->naddr++;
	}
	if (memcmp(&ifc->addr[i], &a, sizeof(a))) {
		ifc->addr[i] = a;
		nl->changed = 1;
	}
}

/* NLMSG_OK without the signed/unsigned comparison clang warns about. */
static int nl_ok(const struct nlmsghdr *nh, int len)
{
	return len >= (int)sizeof(*nh) && nh->nlmsg_len >= sizeof(*nh) && nh->nlmsg_len <= (unsigned)len;
}

/* Processes one buffer of messages; returns 1 when it saw NLMSG_DONE for seq. */
static int process(struct cg_nl *nl, char *buf, ssize_t n, uint32_t seq)
{
	int done = 0, len = (int)n;

	for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; nl_ok(nh, len); nh = NLMSG_NEXT(nh, len)) {
		switch (nh->nlmsg_type) {
		case NLMSG_DONE:
		case NLMSG_ERROR:
			if (seq && nh->nlmsg_seq == seq)
				done = 1;
			break;
		case RTM_NEWLINK:
		case RTM_DELLINK:
			on_link(nl, nh);
			break;
		case RTM_NEWADDR:
		case RTM_DELADDR:
			on_addr(nl, nh);
			break;
		}
	}
	return done;
}

static int dump(struct cg_nl *nl, int type)
{
	struct {
		struct nlmsghdr nh;
		struct rtgenmsg g;
	} req = {
		.nh = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
			.nlmsg_type = (uint16_t)type,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
			.nlmsg_seq = ++nl->seq },
		.g = { .rtgen_family = AF_UNSPEC },
	};
	char buf[32768];

	if (send(nl->fd, &req, req.nh.nlmsg_len, 0) < 0)
		return -1;
	for (;;) {
		struct pollfd p = { .fd = nl->fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, 2000) <= 0)
			return -1;
		n = recv(nl->fd, buf, sizeof(buf), 0);
		if (n < 0 && errno == EAGAIN)
			continue;
		if (n < 0)
			return -1;
		if (process(nl, buf, n, req.nh.nlmsg_seq))
			return 0;
	}
}

static int sync_all(struct cg_nl *nl)
{
	nl->nifs = 0;
	nl->changed = 1;
	return dump(nl, RTM_GETLINK) < 0 || dump(nl, RTM_GETADDR) < 0 ? -1 : 0;
}

int cg_nl_open(struct cg_nl *nl, char *err, size_t errlen)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
				  .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR };
	int big = 1 << 20;

	memset(nl, 0, sizeof(*nl));
	nl->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nl->fd < 0 || bind(nl->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		snprintf(err, errlen, "netlink: %s", strerror(errno));
		cg_nl_close(nl);
		return -1;
	}
	if (setsockopt(nl->fd, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof(big)) < 0)
		setsockopt(nl->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
	if (sync_all(nl) < 0) {
		snprintf(err, errlen, "netlink dump failed: %s", strerror(errno));
		cg_nl_close(nl);
		return -1;
	}
	return 0;
}

void cg_nl_read(struct cg_nl *nl)
{
	char buf[32768];

	for (;;) {
		ssize_t n = recv(nl->fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0) {
			if (errno == ENOBUFS) {
				/* The kernel dropped events: start over from a fresh dump. */
				cg_warn("netlink event overflow, resyncing interfaces");
				if (sync_all(nl) < 0)
					cg_err("netlink resync failed: %s", strerror(errno));
				continue;
			}
			return;
		}
		process(nl, buf, n, 0);
	}
}

void cg_nl_close(struct cg_nl *nl)
{
	if (nl->fd >= 0)
		close(nl->fd);
	nl->fd = -1;
}

int cg_ipv4_usable(const uint8_t a[4])
{
	return a[0] != 127 && !(a[0] == 169 && a[1] == 254) && !(a[0] == 0);
}

int cg_iface_pick(const struct cg_iface *ifc, int family, struct sockaddr_storage *out)
{
	const struct cg_nl_addr *best = NULL;

	for (int i = 0; i < ifc->naddr; i++) {
		const struct cg_nl_addr *a = &ifc->addr[i];

		if (a->family != family)
			continue;
		if (family == AF_INET) {
			if (!cg_ipv4_usable(a->addr))
				continue;
			if (!best || ((best->flags & IFA_F_SECONDARY) && !(a->flags & IFA_F_SECONDARY)))
				best = a;
		} else {
			if (a->scope != RT_SCOPE_UNIVERSE ||
			    (a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED | IFA_F_DEPRECATED)))
				continue;
			if (!best || ((best->flags & IFA_F_TEMPORARY) && !(a->flags & IFA_F_TEMPORARY)))
				best = a;
		}
	}
	if (!best)
		return -1;
	memset(out, 0, sizeof(*out));
	if (family == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)out;

		s->sin_family = AF_INET;
		memcpy(&s->sin_addr, best->addr, 4);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)out;

		s->sin6_family = AF_INET6;
		memcpy(&s->sin6_addr, best->addr, 16);
	}
	return 0;
}
