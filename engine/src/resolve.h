/* Client side: the names of the server lists, looked up by the client
 * itself (names.c does the I/O, dns.h the messages).
 *
 * Not the system's resolver: with all traffic through the tunnel its queries
 * go through the tunnel too, which is down exactly when the server's address
 * changed, and getaddrinfo blocks. So every lookup leaves through a link,
 * bound to its address like the link sockets, and asks that link's own
 * resolvers (the carrier's, from OpenWrt) before the global ones.
 *
 * - A and AAAA are two questions of the name, each with its own lookups.
 * - A lookup is a series of attempts, one at a time, each through one
 *   (link, resolver) pair (cg_rs_pair: the first resolver of every link,
 *   then the second of every link...), CG_RS_TIMEOUT_MS each, at most
 *   CG_RS_ATTEMPTS. An error answer (SERVFAIL, REFUSED, malformed) moves on
 *   to the next pair at once.
 * - An answer with addresses replaces the family's: sorted, at most
 *   CG_NAME_ADDRS, so that a name with more addresses gives the same ones
 *   whatever their order, and links do not move for a new order. The next
 *   lookup comes after its TTL, clamped to [30 s, 15 min].
 * - An answer without addresses (NXDOMAIN, or no record of the family)
 *   takes the family's addresses away only while that family is silent: a
 *   flaky resolver never takes away an address the server answers on. The
 *   next lookup after CG_RS_NEG_MS, or CG_RS_EMPTY_MS when the name is left
 *   with no address at all: a dynamic DNS record may be back in seconds.
 * - Addresses no server has (the wildcard, loopback, multicast, IPv4-mapped
 *   in an AAAA; a resolver that blocks a name may answer 0.0.0.0) are left
 *   out, and an answer left with none counts as having none.
 * - A lookup that fails keeps what the name had (the last good answer) and
 *   tries again: 1 s doubling up to 15 s while the question never had an
 *   answer (a boot), 5 s doubling up to 60 s after one.
 * - Silent: a name with addresses on which no link has been live for
 *   CG_RS_SILENT_MS (its server moved, or is down). While some link still
 *   points at it, it is looked up at once, then again after
 *   CG_RS_SILENT_EVERY_MS, doubling up to CG_RS_SILENT_MAX_MS, whatever
 *   the TTL; a link live again or new addresses end that episode. A name
 *   no link points at (they failed over to a later entry) waits for its
 *   TTL: its server may be down for good, and asking every few seconds
 *   would buy nothing.
 *   When the server's address changes, the cut is the links' stall
 *   detection, plus that, plus what the dynamic DNS and the resolvers'
 *   caches take to give the new address.
 *
 * Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_RESOLVE_H
#define CG_RESOLVE_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "dns.h"
#include "srvpick.h"

#define CG_RS_TTL_MIN_MS 30000u
#define CG_RS_TTL_MAX_MS 900000u
#define CG_RS_NEG_MS 300000u         /* after an answer without addresses, while the name has others */
#define CG_RS_EMPTY_MS 15000u        /* after one that leaves the name with no address at all */
#define CG_RS_SILENT_MS 2000u        /* no link live on a name's addresses: look it up */
#define CG_RS_SILENT_EVERY_MS 15000u /* and again after this, while that lasts ... */
#define CG_RS_SILENT_MAX_MS 60000u   /* ... doubling up to this */
#define CG_RS_USED_MS 1000u          /* a link pointed at it this recently: in use */
#define CG_RS_TIMEOUT_MS 2000u       /* an attempt */
#define CG_RS_ATTEMPTS 6             /* of a lookup */
#define CG_RS_RETRY_FIRST_MS 1000u   /* a failed lookup, no answer ever: 1 s ... */
#define CG_RS_RETRY_FIRST_MAX_MS 15000u
#define CG_RS_RETRY_MS 5000u /* ... and after one: 5 s ... */
#define CG_RS_RETRY_MAX_MS 60000u

enum { CG_RS_A, CG_RS_AAAA }; /* the two questions of a name */

/* One question of a name, A or AAAA. */
struct cg_rsq {
	uint8_t n;                       /* addresses */
	uint8_t addr[CG_NAME_ADDRS][16]; /* ascending; IPv4 in the first 4 bytes, the rest zero */
	uint8_t inflight;                /* a lookup is under way */
	uint8_t attempt;                 /* its attempt in flight, from 0 */
	uint8_t answered;                /* an answer came once, with or without addresses */
	uint8_t outcome;                 /* CG_DNS_* of the newest answer or failed attempt (IGNORE: none) */
	uint16_t id;                     /* of the attempt in flight */
	uint32_t ttl;                    /* seconds, as the newest answer with addresses gave it */
	uint32_t retry_ms;               /* the wait after the next failed lookup; 0: the first one */
	uint64_t due_ms;                 /* the next lookup */
	uint64_t sent_ms;                /* the attempt in flight */
	uint64_t got_ms;                 /* the newest answer; 0: none */
	uint64_t set_ms;                 /* when its addresses last changed */
	uint64_t live_ms;                /* the newest time a link was live on one of them */
	uint64_t lookups, failures;
};

struct cg_rsname {
	struct cg_rsq q[2];     /* CG_RS_A, CG_RS_AAAA */
	uint64_t addrs_ms;      /* when its addresses last changed, either family */
	uint64_t live_ms;       /* the newest time a link was live on one of them */
	uint64_t used_ms;       /* the newest time a link pointed at one of them */
	uint64_t kick_ms;       /* the newest lookup silence asked for; 0: none in this episode */
	uint32_t kick_every_ms; /* the wait before the next one */
};

static inline int cg_rs_alen(int q)
{
	return q == CG_RS_AAAA ? 16 : 4;
}

static inline uint16_t cg_rs_qtype(int q)
{
	return q == CG_RS_AAAA ? CG_DNS_AAAA : CG_DNS_A;
}

/* A TTL in seconds as the time to the next lookup: clamped. */
static inline uint32_t cg_rs_ttl_ms(uint32_t ttl_s)
{
	uint64_t ms = (uint64_t)ttl_s * 1000;

	return ms < CG_RS_TTL_MIN_MS ? CG_RS_TTL_MIN_MS : ms > CG_RS_TTL_MAX_MS ? CG_RS_TTL_MAX_MS : (uint32_t)ms;
}

/* The n addresses of src (alen bytes each) into dst: zero-filled,
 * ascending, each once, the first CG_NAME_ADDRS of them. Returns how many. */
static inline int cg_rs_sorted(const uint8_t (*src)[16], int n, int alen, uint8_t (*dst)[16])
{
	int k = 0;

	memset(dst, 0, sizeof(uint8_t[CG_NAME_ADDRS][16]));
	for (int i = 0; i < n; i++) {
		int at = 0, c = 1;

		while (at < k && (c = memcmp(dst[at], src[i], (size_t)alen)) < 0)
			at++;
		if (at < k && c == 0)
			continue; /* already there */
		if (at == CG_NAME_ADDRS)
			continue; /* after the ones kept */
		if (k == CG_NAME_ADDRS)
			k--; /* the last one goes */
		memmove(dst[at + 1], dst[at], (size_t)(k - at) * 16);
		memset(dst[at], 0, 16);
		memcpy(dst[at], src[i], (size_t)alen);
		k++;
	}
	return k;
}

/* Whether the address a (alen bytes) of question f can be a server's: not
 * the wildcard, loopback (dns: from a DNS; the hosts file may pin a name
 * there) or multicast, and in an AAAA not IPv4-mapped (an IPv6 socket
 * cannot send there). */
static inline int cg_rs_fit(const uint8_t *a, int f, int dns)
{
	static const uint8_t zero[16], mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
	static const uint8_t lo6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	if (f == CG_RS_A)
		return a[0] != 0 && a[0] < 224 && !(dns && a[0] == 127);
	return memcmp(a, zero, 16) && memcmp(a, mapped, 12) && a[0] != 0xff && !(dns && !memcmp(a, lo6, 16));
}

static inline int cg_rs_has(const struct cg_rsname *nm)
{
	return nm->q[CG_RS_A].n || nm->q[CG_RS_AAAA].n;
}

/* Whether no link has been live on the addresses of question q for
 * CG_RS_SILENT_MS. */
static inline int cg_rs_q_silent(const struct cg_rsq *q, uint64_t now_ms)
{
	uint64_t since = q->live_ms > q->set_ms ? q->live_ms : q->set_ms;

	return q->n && now_ms - since >= CG_RS_SILENT_MS;
}

/* The same for the whole name. */
static inline int cg_rs_silent(const struct cg_rsname *nm, uint64_t now_ms)
{
	uint64_t since = nm->live_ms > nm->addrs_ms ? nm->live_ms : nm->addrs_ms;

	return cg_rs_has(nm) && now_ms - since >= CG_RS_SILENT_MS;
}

/* Whether a link points at one of its addresses (cg_rs_seen, each tick). */
static inline int cg_rs_used(const struct cg_rsname *nm, uint64_t now_ms)
{
	return nm->used_ms && now_ms - nm->used_ms <= CG_RS_USED_MS;
}

/* A link points at an address of family f of nm (live: and the server
 * answers it there). */
static inline void cg_rs_seen(struct cg_rsname *nm, int f, int live, uint64_t now_ms)
{
	nm->used_ms = now_ms;
	if (live)
		nm->q[f].live_ms = nm->live_ms = now_ms;
}

/* Makes the lookups of a silent name some link still points at due now:
 * at once when its silence starts, then after CG_RS_SILENT_EVERY_MS,
 * doubling up to CG_RS_SILENT_MAX_MS. Returns 2 for the first lookup of an
 * episode, 1 for a later one, 0 for none. */
static inline int cg_rs_kick(struct cg_rsname *nm, uint64_t now_ms)
{
	int first;

	if (!cg_rs_silent(nm, now_ms) || !cg_rs_used(nm, now_ms))
		return 0;
	/* A link live since the last one: a new episode. */
	first = !nm->kick_ms || nm->live_ms > nm->kick_ms;
	if (first)
		nm->kick_every_ms = CG_RS_SILENT_EVERY_MS;
	else if (now_ms - nm->kick_ms < nm->kick_every_ms)
		return 0;
	else
		nm->kick_every_ms = nm->kick_every_ms * 2 > CG_RS_SILENT_MAX_MS ? CG_RS_SILENT_MAX_MS
										: nm->kick_every_ms * 2;
	nm->kick_ms = now_ms;
	for (int f = 0; f < 2; f++)
		if (!nm->q[f].inflight && nm->q[f].due_ms > now_ms)
			nm->q[f].due_ms = now_ms;
	return first ? 2 : 1;
}

static inline int cg_rs_due(const struct cg_rsq *q, uint64_t now_ms)
{
	return !q->inflight && now_ms >= q->due_ms;
}

/* A lookup starts: its first attempt goes out now, with id. */
static inline void cg_rs_start(struct cg_rsq *q, uint16_t id, uint64_t now_ms)
{
	q->inflight = 1;
	q->attempt = 0;
	q->id = id;
	q->sent_ms = now_ms;
	q->lookups++;
}

static inline int cg_rs_timed_out(const struct cg_rsq *q, uint64_t now_ms)
{
	return q->inflight && now_ms - q->sent_ms >= CG_RS_TIMEOUT_MS;
}

/* The attempt in flight brought nothing usable (outcome: CG_DNS_IGNORE for
 * a timeout or a socket that failed, or the error answer): returns 1 when
 * the next of npairs goes out now with id, or 0 when the lookup is over and
 * failed, keeping the addresses, and the next one waits. */
static inline int cg_rs_next(struct cg_rsq *q, int outcome, int npairs, uint16_t id, uint64_t now_ms)
{
	int max = npairs < CG_RS_ATTEMPTS ? npairs : CG_RS_ATTEMPTS;
	uint32_t cap = q->answered ? CG_RS_RETRY_MAX_MS : CG_RS_RETRY_FIRST_MAX_MS;

	q->outcome = (uint8_t)outcome;
	if (q->inflight && q->attempt + 1 < max) {
		q->attempt++;
		q->id = id;
		q->sent_ms = now_ms;
		return 1;
	}
	q->inflight = 0;
	q->failures++;
	if (!q->retry_ms)
		q->retry_ms = q->answered ? CG_RS_RETRY_MS : CG_RS_RETRY_FIRST_MS;
	q->due_ms = now_ms + q->retry_ms;
	q->retry_ms = q->retry_ms * 2 > cap ? cap : q->retry_ms * 2;
	return 0;
}

/* An answer to question f of nm (CG_DNS_ADDRS, NODATA or NXDOMAIN; the
 * rest go to cg_rs_next). Returns 1 when the family's addresses changed. */
static inline int cg_rs_answer(struct cg_rsname *nm, int f, const struct cg_dns_result *r, uint64_t now_ms)
{
	struct cg_rsq *q = &nm->q[f];
	uint8_t a[CG_NAME_ADDRS][16], fit[CG_DNS_MAX_ADDRS][16];
	int n = 0, outcome = r->outcome;

	for (int i = 0; outcome == CG_DNS_ADDRS && i < r->n && i < CG_DNS_MAX_ADDRS; i++)
		if (cg_rs_fit(r->addr[i], f, 1))
			memcpy(fit[n++], r->addr[i], 16);
	if (outcome == CG_DNS_ADDRS && !n)
		outcome = CG_DNS_NODATA; /* only addresses no server has */
	q->inflight = 0;
	q->answered = 1;
	q->outcome = (uint8_t)outcome;
	q->got_ms = now_ms;
	q->retry_ms = 0;
	if (outcome == CG_DNS_ADDRS) {
		n = cg_rs_sorted((const uint8_t(*)[16])fit, n, cg_rs_alen(f), a);
		q->ttl = r->ttl;
		q->due_ms = now_ms + cg_rs_ttl_ms(r->ttl);
	} else {
		n = cg_rs_q_silent(q, now_ms) ? 0 : q->n;
		memset(a, 0, sizeof(a));
		memcpy(a, q->addr, (size_t)n * 16);
		/* A name left with nothing to send to is asked again soon: its
		 * record may be back in seconds (a dynamic DNS update). */
		q->due_ms = now_ms + (n || nm->q[f ^ 1].n ? CG_RS_NEG_MS : CG_RS_EMPTY_MS);
	}
	if (n == q->n && !memcmp(a, q->addr, sizeof(a)))
		return 0;
	q->n = (uint8_t)n;
	memcpy(q->addr, a, sizeof(a));
	q->set_ms = nm->addrs_ms = now_ms;
	nm->kick_ms = 0; /* new addresses: the next silence is a new one, looked up at once */
	return 1;
}

/* The (path, resolver) pair of attempt k: the first resolver of every
 * path, then the second of every path, and so on (path i has nres[i]).
 * Returns 0, or -1 past the last pair. */
static inline int cg_rs_pair(const uint8_t *nres, int npaths, int k, int *path, int *res)
{
	for (int r = 0;; r++) {
		int any = 0;

		for (int p = 0; p < npaths; p++) {
			if (r >= nres[p])
				continue;
			any = 1;
			if (k-- == 0) {
				*path = p;
				*res = r;
				return 0;
			}
		}
		if (!any)
			return -1;
	}
}

static inline int cg_rs_npairs(const uint8_t *nres, int npaths)
{
	int n = 0;

	for (int p = 0; p < npaths; p++)
		n += nres[p];
	return n;
}

/* The addresses of one family into out, as sockaddrs with port 0. */
static inline void cg_rs_put(struct cg_nameaddrs *out, const uint8_t (*a)[16], int n, int f)
{
	for (int i = 0; i < n && out->n < 2 * CG_NAME_ADDRS; i++) {
		struct sockaddr_storage *s = &out->a[out->n++];

		memset(s, 0, sizeof(*s));
		if (f == CG_RS_AAAA) {
			struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)s;

			s6->sin6_family = AF_INET6;
			memcpy(&s6->sin6_addr, a[i], 16);
		} else {
			struct sockaddr_in *s4 = (struct sockaddr_in *)s;

			s4->sin_family = AF_INET;
			memcpy(&s4->sin_addr, a[i], 4);
		}
	}
}

/* The addresses of nm as srvpick.h takes them: IPv6 first. */
static inline void cg_rs_addrs(const struct cg_rsname *nm, struct cg_nameaddrs *out)
{
	out->n = 0;
	cg_rs_put(out, (const uint8_t(*)[16])nm->q[CG_RS_AAAA].addr, nm->q[CG_RS_AAAA].n, CG_RS_AAAA);
	cg_rs_put(out, (const uint8_t(*)[16])nm->q[CG_RS_A].addr, nm->q[CG_RS_A].n, CG_RS_A);
}

/* Whether a (port aside) is one of the addresses of nm. */
static inline int cg_rs_holds(const struct cg_rsname *nm, const struct sockaddr_storage *a)
{
	int f = a->ss_family == AF_INET6 ? CG_RS_AAAA : CG_RS_A;
	const void *ip = f == CG_RS_AAAA ? (const void *)&((const struct sockaddr_in6 *)a)->sin6_addr
					 : (const void *)&((const struct sockaddr_in *)a)->sin_addr;

	if (a->ss_family != AF_INET && a->ss_family != AF_INET6)
		return 0;
	for (int i = 0; i < nm->q[f].n; i++)
		if (!memcmp(nm->q[f].addr[i], ip, (size_t)cg_rs_alen(f)))
			return 1;
	return 0;
}

/* The addresses the hosts file text (/etc/hosts) gives a normalized name
 * (dns.h), on every line that lists it, ordered as cg_rs_addrs orders a
 * name's. Returns 1 when it gives any, else 0. A scoped IPv6 address
 * (fe80::1%eth0) is no server and is skipped. */
static inline int cg_hosts_find(const char *text, const char *name, struct cg_nameaddrs *out)
{
	uint8_t got[2][2 * CG_NAME_ADDRS][16], sorted[2][CG_NAME_ADDRS][16];
	int n[2] = { 0, 0 }, k[2];

	out->n = 0;
	while (text && *text) {
		const char *end = strchr(text, '\n'), *p = text, *stop;
		char tok[256], ip[16] = { 0 };
		int f = -1, field = 0;

		stop = end ? end : text + strlen(text);
		text = end ? end + 1 : stop;
		while (p < stop) {
			size_t len;

			while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r'))
				p++;
			if (p == stop || *p == '#')
				break;
			len = 0;
			while (p + len < stop && p[len] != ' ' && p[len] != '\t' && p[len] != '\r' && p[len] != '#')
				len++;
			if (len >= sizeof(tok) && field == 0)
				break; /* no address that long: no hosts line */
			if (len < sizeof(tok)) {
				memcpy(tok, p, len);
				tok[len] = '\0';
				if (field == 0) {
					if (inet_pton(AF_INET, tok, ip) == 1)
						f = CG_RS_A;
					else if (inet_pton(AF_INET6, tok, ip) == 1)
						f = CG_RS_AAAA;
					if (f == CG_RS_AAAA && !memcmp(ip, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12)) {
						memmove(ip, ip + 12, 4); /* IPv4-mapped: IPv4 */
						memset(ip + 4, 0, 12);
						f = CG_RS_A;
					}
					if (f < 0 || !cg_rs_fit((const uint8_t *)ip, f, 0))
						break; /* no address, or "0.0.0.0 name" (a block): no hosts line */
				} else {
					if (len && tok[len - 1] == '.')
						tok[len - 1] = '\0';
					if (!strcasecmp(tok, name) && n[f] < 2 * CG_NAME_ADDRS) {
						memcpy(got[f][n[f]++], ip, 16);
						break;
					}
				}
			}
			field++;
			p += len;
		}
	}
	k[0] = cg_rs_sorted((const uint8_t(*)[16])got[CG_RS_A], n[CG_RS_A], 4, sorted[CG_RS_A]);
	k[1] = cg_rs_sorted((const uint8_t(*)[16])got[CG_RS_AAAA], n[CG_RS_AAAA], 16, sorted[CG_RS_AAAA]);
	cg_rs_put(out, (const uint8_t(*)[16])sorted[CG_RS_AAAA], k[1], CG_RS_AAAA);
	cg_rs_put(out, (const uint8_t(*)[16])sorted[CG_RS_A], k[0], CG_RS_A);
	return out->n > 0;
}

#endif
