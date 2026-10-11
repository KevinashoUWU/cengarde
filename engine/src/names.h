/* Client side: the lookups of the server lists' names (resolve.h), as
 * sockets. Each attempt opens a UDP socket bound to the link it goes
 * through and to that link's address (cg_udp_link), connected to the
 * resolver, and closes it with the answer or after CG_RS_TIMEOUT_MS; at most
 * two per name in flight (A and AAAA), never on the data path.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_NAMES_H
#define CG_NAMES_H

#include <net/if.h>
#include <stdint.h>

#include "config.h"
#include "netlink.h"
#include "resolve.h"
#include "status.h"
#include "util.h"

/* A link the names may be looked up through: its interface, and the
 * resolvers it asks in order (its own, then the global ones), of the
 * families it has an address of. */
#define CG_NAMES_RES (2 * CG_MAX_RESOLVERS)
struct cg_names_path {
	const struct cg_iface *ifc;
	uint8_t n;
	const struct sockaddr_storage *res[CG_NAMES_RES];
};

struct cg_name {
	int used;
	char name[CG_DNS_NAME_MAX + 1];
	int hosts;                     /* from the hosts file: never looked up */
	struct cg_nameaddrs hosts_addrs;
	struct cg_rsname rs;
	int fd[2];                     /* each question's attempt in flight; -1: none */
	int adv[2];                    /* an error answer came: the next attempt goes out at the tick; -1: none */
	char at[2][IFNAMSIZ + 64];     /* where each question's attempt in flight went */
	char via[IFNAMSIZ + 64];       /* where the newest answer came from */
	const char *err;               /* why its newest lookup brought no address, "" */
	struct cg_ratelimit rl;
};

struct cg_names {
	struct cg_name n[CG_MAX_NAMES];
	int slot[CG_MAX_NAMES]; /* by the configuration's name index */
	int nslot;
};

void cg_names_init(struct cg_names *s);
/* After a configuration loads (cfg->names): the names still there keep
 * their state, new ones are looked up at once, the others go. */
void cg_names_set(struct cg_names *s, int ep, const struct cg_config *cfg);
/* The addresses of the configuration's names now, by index, for
 * cg_config_build: p[i] points into a. */
void cg_names_addrs(const struct cg_names *s, struct cg_nameaddrs *a, const struct cg_nameaddrs **p);
/* Whether the configuration's name i has any address. */
int cg_names_resolved(const struct cg_names *s, int i);
/* A link is live on remote: the name that holds it is not silent. */
void cg_names_live(struct cg_names *s, const struct sockaddr_storage *remote, uint64_t now_ms);
/* Silence turned into lookups; whether any lookup is due or in flight. */
int cg_names_need(struct cg_names *s, uint64_t now_ms);
/* Starts the lookups that are due and moves on from the attempts that
 * timed out or got an error, through paths. Addresses only change with an
 * answer (cg_names_read). */
void cg_names_tick(struct cg_names *s, int ep, const struct cg_names_path *paths, int npaths, uint64_t now_ms);
/* The socket CG_EV_DNS idx is readable. Returns 1 when the addresses of a
 * name changed. */
int cg_names_read(struct cg_names *s, int ep, unsigned idx, uint64_t now_ms);
/* "names" of the status, in the configuration's order. */
void cg_names_json(const struct cg_names *s, struct cg_json *j, uint64_t now_ms);
void cg_names_close(struct cg_names *s, int ep);

#endif
