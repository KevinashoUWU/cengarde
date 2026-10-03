/* Interface and address tracking over rtnetlink: an initial dump, then
 * events, so links react at once instead of by polling.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_NETLINK_H
#define CG_NETLINK_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define CG_NL_MAX_IFS 64
#define CG_NL_MAX_ADDRS 16

struct cg_nl_addr {
	int family;
	uint8_t addr[16];
	uint32_t flags; /* IFA_F_* */
	uint8_t scope;
};

struct cg_iface {
	int index;
	char name[IFNAMSIZ];
	unsigned flags; /* IFF_* */
	int naddr;
	struct cg_nl_addr addr[CG_NL_MAX_ADDRS];
};

struct cg_nl {
	int fd;
	uint32_t seq;
	int nifs;
	struct cg_iface ifs[CG_NL_MAX_IFS];
	int changed; /* set whenever the table changes; the caller clears it */
};

int cg_nl_open(struct cg_nl *nl, char *err, size_t errlen);
/* Drains pending events without blocking (re-dumps after an overflow). */
void cg_nl_read(struct cg_nl *nl);
void cg_nl_close(struct cg_nl *nl);

const struct cg_iface *cg_nl_find(const struct cg_nl *nl, const char *name);

/* Preferred usable address of family on ifc, with port 0. IPv4: primary,
 * not loopback or link-local. IPv6: global scope, not tentative, deprecated
 * or failed, stable before temporary. Returns 0 or -1 if there is none. */
int cg_iface_pick(const struct cg_iface *ifc, int family, struct sockaddr_storage *out);

/* Pure helper for the IPv4 rule above (address in network order). */
int cg_ipv4_usable(const uint8_t a[4]);

#endif
