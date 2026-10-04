/* The server's receive memory budget.
 *
 * Every UDP socket of the machine draws its receive queue from one pool,
 * net.ipv4.udp_mem (pages: min, pressure, max). Past max, every UDP socket
 * drops what arrives, kernel WireGuard and DNS included. The kernel derives
 * it from RAM (max is about 19 % of it), so on a small VPS the server's
 * lanes, each with its own receive buffer, could fill it together: the
 * coupling the lanes exist to remove, moved to the whole machine.
 *
 * So the sockets that receive share half of the pressure threshold:
 *   budget = udp_mem[1] x page / 2
 * and each gets min(rcvbuf, budget / (2 x sockets)) as its SO_RCVBUF,
 * since the kernel doubles that value. The sockets are the lanes of the
 * groups that hold a router (an empty group receives nothing), one
 * WireGuard socket per router (its active session's) and the junk socket.
 * On a 1 GB VPS with one router: 8 lanes + 1 + 1 = 10 sockets, about
 * 3.2 MiB each. udp_mem itself is left alone: raising it on a small box
 * trades UDP drops for the OOM killer.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_RCVBUDGET_H
#define CG_RCVBUDGET_H

#include <stdint.h>
#include <stdlib.h>

/* The smallest value it hands out: the kernel's own floor is about this. */
#define CG_RCVBUF_FLOOR 4096

struct cg_rcvbudget {
	uint64_t budget;  /* bytes the receiving sockets may hold in all; 0: unknown, no cap */
	uint32_t sockets; /* how many share it */
	int per_socket;   /* SO_RCVBUF for each (the kernel doubles it); 0: the kernel's default */
	int capped;       /* per_socket is below the configured rcvbuf */
};

/* Parses net.ipv4.udp_mem ("383415\t511221\t766830\n"). 0, or -1 when it
 * is not three numbers, or the pressure threshold is 0. */
static inline int cg_udp_mem_parse(const char *text, uint64_t mem[3])
{
	const char *p = text;

	for (int i = 0; i < 3; i++) {
		char *end;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p < '0' || *p > '9')
			return -1;
		mem[i] = strtoull(p, &end, 10);
		p = end;
	}
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	if (*p || !mem[1])
		return -1;
	return 0;
}

/* net.ipv4.udp_mem as the kernel sets it at boot, from RAM (udp_init: an
 * eighth of the free buffer pages as the pressure threshold, at least 128
 * pages, 3/4 of it as min and twice that as max), for when the sysctl
 * cannot be read: it is global and lives in the first network namespace
 * only, so a server in a namespace of its own (a container, the lab) does
 * not see it. RAM is a little more than the free buffer pages, so the
 * estimate is a little high: about 1 % on 16 GB. */
static inline void cg_udp_mem_estimate(uint64_t ram, uint64_t page, uint64_t mem[3])
{
	uint64_t limit = page ? ram / page / 8 : 0;

	if (limit < 128)
		limit = 128;
	mem[0] = limit / 4 * 3;
	mem[1] = limit;
	mem[2] = mem[0] * 2;
}

/* Receiving sockets: the lanes of the groups that hold at least one router,
 * one WireGuard socket per router, and the junk socket when there is one. */
static inline uint32_t cg_rcvbudget_sockets(uint32_t lanes, uint32_t groups_used, uint32_t routers, int junk)
{
	return lanes * groups_used + routers + (junk ? 1 : 0);
}

/* udp_mem: NULL when unknown (no cap). page: bytes per page. rcvbuf: the
 * configured value, 0 for the kernel's default (never capped). */
static inline struct cg_rcvbudget cg_rcvbudget(const uint64_t *udp_mem, uint64_t page, uint32_t sockets, int rcvbuf)
{
	struct cg_rcvbudget b = { .sockets = sockets, .per_socket = rcvbuf };
	uint64_t per;

	if (!udp_mem || !sockets || !page)
		return b;
	b.budget = udp_mem[1] * page / 2;
	per = b.budget / (2 * (uint64_t)sockets);
	if (per < CG_RCVBUF_FLOOR)
		per = CG_RCVBUF_FLOOR;
	if (rcvbuf > 0 && per < (uint64_t)rcvbuf) {
		b.per_socket = (int)per;
		b.capped = 1;
	}
	return b;
}

#endif
