/* Client side: which server address each link sends to.
 *
 * "server" is an ordered list, the operator's preference (a name stands for
 * its addresses). A link's candidates are the list's addresses of the
 * families it has a usable address of, in order and without repeats
 * (cg_cands_build). It uses one at a time: one socket, one family, never
 * IPv4 and IPv6 at once through the same modem, which would double the
 * traffic on the same radio with no diversity (story 004).
 *
 * - Start: the first candidate.
 * - Failover: when the socket goes failover_ms with no verified reply (since
 *   it opened, or since the last reply), the link moves to the next
 *   candidate, wrapping around. 10 s does not mistake a server restart for
 *   an outage. A link with a single candidate never moves.
 * - Sticky: it stays wherever replies come. The choice is kept by address,
 *   not by index, so the same addresses in another order change nothing.
 * - Back to the first only when the link's local address changes, when its
 *   list changes (both seen by the caller), or at the first reply after a
 *   dead round: once the link went through every candidate without any
 *   reply, the link was dead, not the address (a cellular outage, the
 *   modem's lease kept), and the operator's order wins again
 *   (cg_srv_on_reply).
 * - No periodic retry of a preferred address: each try would cost
 *   failover_ms of loss while it stays broken.
 *
 * Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_SRVPICK_H
#define CG_SRVPICK_H

#include <stdint.h>
#include <sys/socket.h>

#include "util.h"

#define CG_MAX_CANDS 32 /* candidates of a link, names expanded */

/* Where a is among the n candidates, or -1. */
static inline int cg_cand_find(const struct sockaddr_storage *cands, int n, const struct sockaddr_storage *a)
{
	for (int i = 0; i < n; i++)
		if (cg_addr_equal(&cands[i], a))
			return i;
	return -1;
}

/* The candidates of a link from the n server addresses, in order: those of
 * a family in families (1 << AF_INET, 1 << AF_INET6), each once, at most
 * CG_MAX_CANDS. Returns how many. */
static inline int cg_cands_build(const struct sockaddr_storage *servers, int n, unsigned families,
				 struct sockaddr_storage *out)
{
	int k = 0;

	for (int i = 0; i < n && k < CG_MAX_CANDS; i++) {
		int f = servers[i].ss_family;

		if ((f != AF_INET && f != AF_INET6) || !(families >> f & 1) || cg_cand_find(out, k, &servers[i]) >= 0)
			continue;
		out[k++] = servers[i];
	}
	return k;
}

/* Whether two lists hold the same addresses in the same order. */
static inline int cg_cands_same(const struct sockaddr_storage *a, int na, const struct sockaddr_storage *b, int nb)
{
	if (na != nb)
		return 0;
	for (int i = 0; i < na; i++)
		if (!cg_addr_equal(&a[i], &b[i]))
			return 0;
	return 1;
}

/* Whether a link's socket, opened at opened_ms, went failover_ms without a
 * verified reply (last_reply_ms: the newest, 0 for none). 0 turns failover
 * off. */
static inline int cg_srv_due(uint64_t now_ms, uint64_t opened_ms, uint64_t last_reply_ms, uint32_t failover_ms)
{
	uint64_t since = last_reply_ms > opened_ms ? last_reply_ms : opened_ms;

	return failover_ms && now_ms - since >= failover_ms;
}

/* The candidate after i of n, wrapping around: a move with no reply in
 * between, counted in *silent (the moves since the last reply). */
static inline int cg_srv_next(int i, int n, uint32_t *silent)
{
	(*silent)++;
	return (i + 1) % n;
}

/* Whether the link went through every one of its n candidates without a
 * reply. */
static inline int cg_srv_dead_round(uint32_t silent, int n)
{
	return n > 1 && silent >= (uint32_t)n;
}

/* The candidate a link uses now, an index into cands (n of them; -1 when
 * n is 0): the one at cur while it answers, the next once it is due
 * (cg_srv_due), and the first when cur is NULL or not a candidate (a new
 * link, a new list, a family gone), which starts *silent again. */
static inline int cg_srv_pick(const struct sockaddr_storage *cands, int n, const struct sockaddr_storage *cur,
			      uint64_t now_ms, uint64_t opened_ms, uint64_t last_reply_ms, uint32_t failover_ms,
			      uint32_t *silent)
{
	int i = cur ? cg_cand_find(cands, n, cur) : -1;

	if (n <= 0)
		return -1;
	if (i < 0) {
		*silent = 0;
		return 0;
	}
	if (n > 1 && cg_srv_due(now_ms, opened_ms, last_reply_ms, failover_ms))
		return cg_srv_next(i, n, silent);
	return i;
}

/* A verified reply on a link using candidate cur_idx of n. Returns 1 when it
 * ends a dead round away from the first candidate: the link goes back to
 * the first. Either way the count of silent moves starts again. */
static inline int cg_srv_on_reply(uint32_t *silent, int n, int cur_idx)
{
	int back = cg_srv_dead_round(*silent, n) && cur_idx > 0;

	*silent = 0;
	return back;
}

#endif
