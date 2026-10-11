/* SPDX-License-Identifier: GPL-2.0-only */
#include "names.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine.h"
#include "log.h"
#include "sock.h"

void cg_names_init(struct cg_names *s)
{
	memset(s, 0, sizeof(*s));
	for (int i = 0; i < CG_MAX_NAMES; i++)
		s->n[i].fd[0] = s->n[i].fd[1] = s->n[i].adv[0] = s->n[i].adv[1] = -1;
}

static void q_close(struct cg_name *nm, int f, int ep)
{
	if (nm->fd[f] < 0)
		return;
	epoll_ctl(ep, EPOLL_CTL_DEL, nm->fd[f], NULL);
	close(nm->fd[f]);
	nm->fd[f] = -1;
}

static void name_free(struct cg_name *nm, int ep)
{
	q_close(nm, CG_RS_A, ep);
	q_close(nm, CG_RS_AAAA, ep);
	memset(nm, 0, sizeof(*nm));
	nm->fd[0] = nm->fd[1] = nm->adv[0] = nm->adv[1] = -1;
}

void cg_names_set(struct cg_names *s, int ep, const struct cg_config *cfg)
{
	int keep[CG_MAX_NAMES] = { 0 };

	s->nslot = cfg->nnames;
	for (int i = 0; i < cfg->nnames; i++) {
		s->slot[i] = -1;
		for (int k = 0; k < CG_MAX_NAMES; k++)
			if (s->n[k].used && !strcmp(s->n[k].name, cfg->names[i].name)) {
				s->slot[i] = k;
				keep[k] = 1;
			}
	}
	for (int k = 0; k < CG_MAX_NAMES; k++)
		if (s->n[k].used && !keep[k])
			name_free(&s->n[k], ep);
	for (int i = 0; i < cfg->nnames; i++) {
		struct cg_name *nm;

		if (s->slot[i] < 0) {
			for (int k = 0; k < CG_MAX_NAMES; k++)
				if (!s->n[k].used) {
					s->slot[i] = k;
					break;
				}
			nm = &s->n[s->slot[i]]; /* as many slots as names */
			name_free(nm, ep);
			nm->used = 1;
			strcpy(nm->name, cfg->names[i].name);
			nm->err = "";
		}
		nm = &s->n[s->slot[i]];
		/* Into the hosts file or out of it: from now on, by that. */
		nm->hosts = cfg->names[i].hosts;
		nm->hosts_addrs = cfg->names[i].addrs;
		if (nm->hosts) {
			q_close(nm, CG_RS_A, ep);
			q_close(nm, CG_RS_AAAA, ep);
			memset(&nm->rs, 0, sizeof(nm->rs));
			nm->adv[0] = nm->adv[1] = -1;
		}
	}
}

void cg_names_addrs(const struct cg_names *s, struct cg_nameaddrs *a, const struct cg_nameaddrs **p)
{
	for (int i = 0; i < s->nslot; i++) {
		const struct cg_name *nm = &s->n[s->slot[i]];

		if (nm->hosts) {
			p[i] = &nm->hosts_addrs;
		} else {
			cg_rs_addrs(&nm->rs, &a[i]);
			p[i] = &a[i];
		}
	}
}

int cg_names_resolved(const struct cg_names *s, int i)
{
	const struct cg_name *nm;

	if (i < 0 || i >= s->nslot)
		return 0;
	nm = &s->n[s->slot[i]];
	return nm->hosts ? nm->hosts_addrs.n > 0 : cg_rs_has(&nm->rs);
}

void cg_names_live(struct cg_names *s, const struct sockaddr_storage *remote, uint64_t now_ms)
{
	for (int k = 0; k < CG_MAX_NAMES; k++)
		if (s->n[k].used && !s->n[k].hosts && cg_rs_holds(&s->n[k].rs, remote))
			s->n[k].rs.live_ms = now_ms;
}

int cg_names_need(struct cg_names *s, uint64_t now_ms)
{
	int need = 0;

	for (int k = 0; k < CG_MAX_NAMES; k++) {
		struct cg_name *nm = &s->n[k];

		if (!nm->used || nm->hosts)
			continue;
		if (cg_rs_kick(&nm->rs, now_ms))
			cg_info("server name %s: no link is live on its addresses, looking it up again", nm->name);
		for (int f = 0; f < 2; f++)
			need |= nm->rs.q[f].inflight || cg_rs_due(&nm->rs.q[f], now_ms);
	}
	return need;
}

static uint16_t new_id(void)
{
	uint16_t id;

	if (cg_random(&id, sizeof(id)) < 0)
		id = (uint16_t)cg_now_us(); /* never: getrandom does not fail */
	return id;
}

/* Sends attempt q->attempt of question f of name k through its pair.
 * Returns 0, or -1 when that pair cannot be used. */
static int attempt(struct cg_names *s, int ep, int k, int f, const struct cg_names_path *paths, int npaths)
{
	struct cg_name *nm = &s->n[k];
	struct cg_rsq *q = &nm->rs.q[f];
	const struct sockaddr_storage *res;
	struct sockaddr_storage local;
	uint8_t nres[CG_MAX_LINKS], buf[CG_DNS_QUERY_MAX];
	char err[256], a[64];
	int p, r, len, fd;

	for (int i = 0; i < npaths; i++)
		nres[i] = paths[i].n;
	if (cg_rs_pair(nres, npaths, q->attempt, &p, &r) < 0)
		return -1;
	res = paths[p].res[r];
	snprintf(nm->at[f], sizeof(nm->at[f]), "%s %s", paths[p].ifc->name, cg_addr_str(res, a, sizeof(a)));
	if (cg_iface_pick(paths[p].ifc, res, &local) < 0)
		return -1;
	fd = cg_udp_link(paths[p].ifc->name, &local, res, err, sizeof(err));
	if (fd < 0) {
		cg_dbg("server name %s: cannot ask %s: %s", nm->name, nm->at[f], err);
		return -1;
	}
	len = cg_dns_query(buf, sizeof(buf), q->id, nm->name, cg_rs_qtype(f));
	if (len < 0 || send(fd, buf, (size_t)len, 0) != len ||
	    cg_epoll_add(ep, fd, CG_EV(CG_EV_DNS, k * 2 + f)) < 0) {
		cg_dbg("server name %s: cannot ask %s: %s", nm->name, nm->at[f], strerror(errno));
		close(fd);
		return -1;
	}
	nm->fd[f] = fd;
	return 0;
}

/* What a name has, for the log. */
static const char *addrs_str(const struct cg_name *nm, char *buf, size_t len)
{
	struct cg_nameaddrs a;
	size_t at = 0;
	char one[64];

	cg_rs_addrs(&nm->rs, &a);
	buf[0] = '\0';
	for (int i = 0; i < a.n && at < len; i++) {
		cg_addr_str(&a.a[i], one, sizeof(one));
		one[strlen(one) - 2] = '\0'; /* its port, 0 */
		at += (size_t)snprintf(buf + at, len - at, "%s%s", i ? " " : "", one);
	}
	return a.n ? buf : "no address";
}

/* The lookup of question f of name k failed: what it keeps, in the log
 * once a minute at most. */
static void lookup_failed(struct cg_name *nm, int f, uint64_t now_ms)
{
	char buf[512];
	const struct cg_rsq *q = &nm->rs.q[f];

	nm->err = q->outcome == CG_DNS_IGNORE ? "timeout" : cg_dns_outcome_name(q->outcome);
	if (cg_ratelimit_ok(&nm->rl, now_ms, 60000))
		cg_warn("server name %s: its lookup (%s) found no answer through any link (%s, the last through %s); %s %s",
			nm->name, f == CG_RS_AAAA ? "IPv6" : "IPv4", nm->err, nm->at[f],
			cg_rs_has(&nm->rs) ? "keeping" : "still", addrs_str(nm, buf, sizeof(buf)));
}

/* Moves question f of name k on: its next attempt, through the next pair
 * that can be used, or the end of a failed lookup. */
static void advance(struct cg_names *s, int ep, int k, int f, int outcome, const struct cg_names_path *paths,
		    int npaths, uint64_t now_ms)
{
	struct cg_name *nm = &s->n[k];
	uint8_t nres[CG_MAX_LINKS];

	for (int i = 0; i < npaths; i++)
		nres[i] = paths[i].n;
	q_close(nm, f, ep);
	nm->adv[f] = -1;
	while (cg_rs_next(&nm->rs.q[f], outcome, cg_rs_npairs(nres, npaths), new_id(), now_ms)) {
		if (attempt(s, ep, k, f, paths, npaths) == 0)
			return;
		outcome = CG_DNS_IGNORE;
	}
	lookup_failed(nm, f, now_ms);
}

void cg_names_tick(struct cg_names *s, int ep, const struct cg_names_path *paths, int npaths, uint64_t now_ms)
{
	uint8_t nres[CG_MAX_LINKS];
	int npairs;

	for (int i = 0; i < npaths; i++)
		nres[i] = paths[i].n;
	npairs = cg_rs_npairs(nres, npaths);
	for (int k = 0; k < CG_MAX_NAMES; k++) {
		struct cg_name *nm = &s->n[k];

		if (!nm->used || nm->hosts)
			continue;
		for (int f = 0; f < 2; f++) {
			struct cg_rsq *q = &nm->rs.q[f];

			if (q->inflight && nm->adv[f] >= 0)
				advance(s, ep, k, f, nm->adv[f], paths, npaths, now_ms);
			else if (cg_rs_timed_out(q, now_ms))
				advance(s, ep, k, f, CG_DNS_IGNORE, paths, npaths, now_ms);
			if (!cg_rs_due(q, now_ms))
				continue;
			if (!npairs) {
				nm->err = "no link"; /* due until a link can ask */
				continue;
			}
			cg_rs_start(q, new_id(), now_ms);
			if (attempt(s, ep, k, f, paths, npaths) < 0)
				advance(s, ep, k, f, CG_DNS_IGNORE, paths, npaths, now_ms);
		}
	}
}

int cg_names_read(struct cg_names *s, int ep, unsigned idx, uint64_t now_ms)
{
	unsigned k = idx / 2;
	int f = (int)(idx % 2), changed, silent;
	struct cg_name *nm;
	struct cg_rsq *q;
	struct cg_dns_result r;
	uint8_t buf[1500];
	char was[512], now[512];

	if (k >= CG_MAX_NAMES)
		return 0;
	nm = &s->n[k];
	q = &nm->rs.q[f];
	if (!nm->used || nm->fd[f] < 0 || !q->inflight)
		return 0;
	for (int i = 0; i < 8; i++) {
		ssize_t n = recv(nm->fd[f], buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return 0;
			/* ICMP unreachable, mostly: nothing answers DNS there. */
			cg_dbg("server name %s: %s: %s", nm->name, nm->at[f], strerror(errno));
			q_close(nm, f, ep);
			nm->adv[f] = CG_DNS_IGNORE;
			return 0;
		}
		cg_dns_parse(buf, (size_t)n, q->id, nm->name, cg_rs_qtype(f), &r);
		if (r.outcome == CG_DNS_IGNORE)
			continue;
		q_close(nm, f, ep);
		if (r.outcome != CG_DNS_ADDRS && r.outcome != CG_DNS_NODATA && r.outcome != CG_DNS_NXDOMAIN) {
			cg_dbg("server name %s: %s from %s", nm->name, cg_dns_outcome_name(r.outcome), nm->at[f]);
			nm->adv[f] = r.outcome; /* the next pair, at the tick */
			return 0;
		}
		addrs_str(nm, was, sizeof(was));
		silent = cg_rs_silent(&nm->rs, now_ms);
		changed = cg_rs_answer(&nm->rs, f, &r, silent, now_ms);
		memcpy(nm->via, nm->at[f], sizeof(nm->via)); /* the same size */
		/* No error for a family without records while the name has
		 * addresses (an IPv4-only server): it is the usual case. */
		if (r.outcome == CG_DNS_ADDRS || (r.outcome == CG_DNS_NODATA && cg_rs_has(&nm->rs))) {
			nm->err = "";
		} else {
			nm->err = cg_dns_outcome_name(r.outcome);
			if (!changed && q->n && cg_ratelimit_ok(&nm->rl, now_ms, 60000))
				cg_info("server name %s: %s for IPv%c from %s while the server answers there: keeping %s",
					nm->name, nm->err, f == CG_RS_AAAA ? '6' : '4', nm->via, addrs_str(nm, now, sizeof(now)));
		}
		if (changed)
			cg_info("server name %s: %s (was %s; TTL %u s, from %s)", nm->name, addrs_str(nm, now, sizeof(now)),
				was, r.outcome == CG_DNS_ADDRS ? r.ttl : 0, nm->via);
		return changed;
	}
	return 0;
}

static const char *state(const struct cg_name *nm)
{
	const struct cg_rsq *a = &nm->rs.q[CG_RS_A], *b = &nm->rs.q[CG_RS_AAAA];

	if (nm->hosts)
		return "hosts";
	if (cg_rs_has(&nm->rs))
		return "ok";
	if (a->answered && b->answered)
		return "none";
	return a->failures || b->failures ? "failed" : "waiting";
}

void cg_names_json(const struct cg_names *s, struct cg_json *j, uint64_t now_ms)
{
	cg_json_arr(j, "names");
	for (int i = 0; i < s->nslot; i++) {
		const struct cg_name *nm = &s->n[s->slot[i]];
		struct cg_nameaddrs a;
		uint64_t got = 0, next = UINT64_MAX, lookups = 0, failures = 0;
		uint32_t ttl = 0;
		int have_ttl = 0;
		char buf[64];

		if (nm->hosts)
			a = nm->hosts_addrs;
		else
			cg_rs_addrs(&nm->rs, &a);
		for (int f = 0; f < 2; f++) {
			const struct cg_rsq *q = &nm->rs.q[f];

			uint64_t in = q->inflight || q->due_ms <= now_ms ? 0 : q->due_ms - now_ms;

			got = q->got_ms > got ? q->got_ms : got;
			next = in < next ? in : next;
			if (q->n && (!have_ttl || q->ttl < ttl)) {
				ttl = q->ttl;
				have_ttl = 1;
			}
			lookups += q->lookups;
			failures += q->failures;
		}
		cg_json_obj(j, NULL);
		cg_json_str(j, "name", nm->name);
		cg_json_str(j, "state", state(nm));
		cg_json_arr(j, "addresses");
		for (int k = 0; k < a.n; k++) {
			cg_addr_str(&a.a[k], buf, sizeof(buf));
			buf[strlen(buf) - 2] = '\0'; /* ":0" */
			if (buf[0] == '[') {         /* "[ipv6]" */
				memmove(buf, buf + 1, strlen(buf));
				buf[strlen(buf) - 1] = '\0';
			}
			cg_json_str(j, NULL, buf);
		}
		cg_json_end(j, ']');
		if (got)
			cg_json_u64(j, "answered_ms_ago", now_ms - got);
		else
			cg_json_null(j, "answered_ms_ago");
		cg_json_u64(j, "next_lookup_ms", nm->hosts || next == UINT64_MAX ? 0 : next);
		if (have_ttl)
			cg_json_u64(j, "ttl", ttl);
		else
			cg_json_null(j, "ttl");
		cg_json_bool(j, "silent", !nm->hosts && cg_rs_silent(&nm->rs, now_ms));
		cg_json_str(j, "error", nm->hosts ? "" : nm->err ? nm->err : "");
		cg_json_str(j, "via", nm->via);
		cg_json_u64(j, "lookups", lookups);
		cg_json_u64(j, "failures", failures);
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
}

void cg_names_close(struct cg_names *s, int ep)
{
	for (int k = 0; k < CG_MAX_NAMES; k++)
		if (s->n[k].used)
			name_free(&s->n[k], ep);
}
