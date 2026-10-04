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
 *   not by index, so a list that gives the same addresses in another order
 *   (a name resolved again) does not move it.
 * - Back to the first only when the link's local address changes, when it
 *   gains or loses a family with entries in its list (cg_cands_changed:
 *   Starlink's IPv6 comes after its IPv4, and with "[IPv6] IPv4" the link
 *   moves to the IPv6 once it has one), when a reload changes its list
 *   (cg_lists_same: an entry of its families added, removed, edited or
 *   moved; not an entry of another family, nor a name resolved again in
 *   another order), all three seen by the caller, or at the first reply
 *   after a dead round: once the link went through every candidate without
 *   any reply, the link was dead, not the address (a cellular outage, the
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

/* Appends a to the *k candidates in out when it is of a family in families
 * (1 << AF_INET, 1 << AF_INET6), not there yet, and there is room. */
static inline void cg_cand_add(struct sockaddr_storage *out, int *k, const struct sockaddr_storage *a,
			       unsigned families)
{
	int f = a->ss_family;

	if (*k < CG_MAX_CANDS && (f == AF_INET || f == AF_INET6) && (families >> f & 1) && cg_cand_find(out, *k, a) < 0)
		out[(*k)++] = *a;
}

/* The candidates of a link from the n server addresses, in order: those of
 * a family in families, each once, at most CG_MAX_CANDS. Returns how many. */
static inline int cg_cands_build(const struct sockaddr_storage *servers, int n, unsigned families,
				 struct sockaddr_storage *out)
{
	int k = 0;

	for (int i = 0; i < n; i++)
		cg_cand_add(out, &k, &servers[i], families);
	return k;
}

/* A server list as the operator wrote it: n addresses from nentry entries,
 * entry_n[i] of them from the i-th (a name gives several). */
struct cg_srvlist {
	const struct sockaddr_storage *a;
	int n;
	const uint8_t *entry_n;
	int nentry;
};

/* The candidates a link with these families takes from list, as
 * cg_cands_build, grouped by entry: g[i] of them from the i-th entry that
 * gives any (one of another family, or only repeating earlier addresses,
 * gives none). Returns how many groups. */
static inline int cg_cands_groups(const struct cg_srvlist *list, unsigned families, struct sockaddr_storage *out,
				  int *g)
{
	int k = 0, ng = 0, at = 0;

	for (int e = 0; e < list->nentry; at += list->entry_n[e++]) {
		int k0 = k;

		for (int i = at; i < at + list->entry_n[e] && i < list->n; i++)
			cg_cand_add(out, &k, &list->a[i], families);
		if (k > k0)
			g[ng++] = k - k0;
	}
	return ng;
}

/* Whether a reload leaves the list of a link with these families as it was:
 * the same entries in the same order, each giving the same candidates in
 * any order. So a name resolved again in another order, or an entry of
 * another family added or removed, changes nothing; an entry of its
 * families added, removed, edited or moved changes it. */
static inline int cg_lists_same(const struct cg_srvlist *a, const struct cg_srvlist *b, unsigned families)
{
	struct sockaddr_storage ca[CG_MAX_CANDS], cb[CG_MAX_CANDS];
	int ga[CG_MAX_CANDS], gb[CG_MAX_CANDS];
	int na = cg_cands_groups(a, families, ca, ga), nb = cg_cands_groups(b, families, cb, gb);

	if (na != nb)
		return 0;
	for (int i = 0, at = 0; i < na; at += ga[i++]) {
		if (ga[i] != gb[i])
			return 0;
		/* No repeats in a group: the same count and each one found is
		 * the same set. */
		for (int j = at; j < at + ga[i]; j++)
			if (cg_cand_find(cb + at, gb[i], &ca[j]) < 0)
				return 0;
	}
	return 1;
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

/* Whether the candidates of a link from the n server addresses change when
 * the families it has go from old_fam to new_fam: only a family with
 * entries in the list counts, so IPv6 coming and going on an uplink leaves
 * an IPv4-only list alone. */
static inline int cg_cands_changed(const struct sockaddr_storage *servers, int n, unsigned old_fam,
				   unsigned new_fam)
{
	struct sockaddr_storage a[CG_MAX_CANDS], b[CG_MAX_CANDS];
	int na, nb;

	if (old_fam == new_fam)
		return 0;
	na = cg_cands_build(servers, n, old_fam, a);
	nb = cg_cands_build(servers, n, new_fam, b);
	return !cg_cands_same(a, na, b, nb);
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
