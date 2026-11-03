/* fair.h: the protocontroller's sends shared by host before flow (story
 * 031). A round of sends gives each host a segment, and each host's flows
 * take its turns in rotation: a device with twenty connections (a download
 * manager, an update) gets the share of one device, not twenty, and a
 * stream that cannot drain (a far, slow server) takes only its host's
 * turns. The host is the router's LAN device that opened the flow (the VPS
 * has the router's session: one host a router).
 *
 * cg_fair_group sorts the flows in use into groups of one host each, in
 * the order the rotation (rr) puts their first flow, the flows of a group
 * in that order too; the caller walks the groups in turn and, in each, its
 * flows from the group's cursor.
 *
 * Pure: arrays the caller owns, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_FAIR_H
#define CG_FAIR_H

#include <stddef.h>
#include <stdint.h>

/* A host's key from its address (16 bytes, IPv4 in the first 4): FNV-1a. */
static inline uint32_t cg_fair_key(const uint8_t a[16])
{
	uint32_t h = 2166136261u;

	for (int i = 0; i < 16; i++)
		h = (h ^ a[i]) * 16777619u;
	return h;
}

/* The n flows (slot numbers in used, hosts in host[slot]) starting at rr
 * in turn, grouped by host into ord (n slots): group g is
 * ord[start[g]..start[g+1]), start has room for n + 1. Returns the number
 * of groups. Stable: a host's flows, and the hosts by their first flow,
 * keep the rotation's order. O(n^2) at worst, n the flows in use (at most a
 * few hundred). */
static inline uint32_t cg_fair_group(const uint32_t *used, uint32_t n, uint32_t rr, const uint32_t *host,
				     uint32_t *ord, uint32_t *start)
{
	uint32_t ng = 0, k = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t s = used[(rr + i) % n], h = host[s], seen = 0;

		/* a host already grouped: its flows were all taken with it */
		for (uint32_t g = 0; g < ng && !seen; g++)
			seen = host[ord[start[g]]] == h;
		if (seen)
			continue;
		start[ng++] = k;
		for (uint32_t j = i; j < n; j++) {
			uint32_t t = used[(rr + j) % n];

			if (host[t] == h)
				ord[k++] = t;
		}
	}
	start[ng] = k;
	return ng;
}

#endif
