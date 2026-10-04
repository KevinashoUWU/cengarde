/* cengarde client: one socket per uplink, every WireGuard datagram goes out on
 * every carrying uplink, and the first copy of each server datagram goes back
 * to WireGuard.
 *
 * Upload: recvmmsg from WireGuard, MAC each packet once, then one
 * non-blocking sendmmsg per uplink, patching only the link byte. A full
 * uplink drops its own copies; it never stalls the others. Link health
 * (health.h) mutes uplinks that lag far behind the fastest one, from the
 * delays the server reports in its probe replies.
 * Download: duplicates are recognised by sequence number before the MAC is
 * checked, so only the first copy of each packet pays for verification. A
 * server that started over with a sequence behind the window is recognised
 * from its probe replies (epoch.h), and the window starts again.
 * Server addresses: each link sends to one address of the server list, of a
 * family it has, from its best source address for it (addrpick.h), and
 * moves to the next one when the server stops answering there (srvpick.h).
 *
 * A reload (SIGHUP or "cengarde ctl reload") applies the new configuration
 * in place, keeping the session; what was set up only once makes the process
 * start again (cg_config_restart_needed).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "addrpick.h"
#include "arrival.h"
#include "ctl.h"
#include "engine.h"
#include "epoch.h"
#include "health.h"
#include "log.h"
#include "netlink.h"
#include "replay.h"
#include "sock.h"
#include "srvpick.h"
#include "status.h"
#include "util.h"

#define RECONCILE_MS 5000

struct link {
	int used;
	char ifname[IFNAMSIZ];
	int fd;
	int seen; /* still wanted in the current reconcile pass */
	int why;  /* WHY_*: why it carries the tunnel or not, as of the last reconcile */
	/* Of the last socket; kept once it closes, to tell a new local address. */
	struct sockaddr_storage local, remote;
	/* The server address it uses (srvpick.h), kept by address; none: the
	 * first candidate, as after an outage of the link (down, without an
	 * address, or gone) or a new list. */
	struct sockaddr_storage cand;
	int cand_idx, ncand;    /* where cand is among the link's candidates, and how many there are */
	unsigned families;      /* 1 << AF_* of the interface's usable addresses, as of the last reconcile */
	uint32_t silent_moves;  /* failovers since the last reply: a dead round once every candidate had one */
	int back_to_first;      /* a reply ended a dead round: the first candidate at the next reconcile */
	uint64_t cand_since_ms; /* when its socket to cand opened */
	uint64_t failovers;
	uint32_t path_mtu; /* IP_MTU or IPV6_MTU of its socket, read every RECONCILE_MS */
	uint64_t up_since_ms, last_rx_ms, last_reply_ms, last_probe_ms, retry_ms;
	uint64_t srtt8_us; /* 8 x smoothed RTT */
	uint32_t rtt_us;
	uint32_t unanswered; /* probes sent since the last reply */
	uint64_t first_unanswered_ms;
	uint32_t probe_announced; /* interval_ms of the last probe */
	uint32_t down_owd;        /* trip of the newest reply, for the next probe */
	int have_down_owd;
	int peer_muted; /* the server carries no download on this link */
	uint64_t tx_pkts, tx_bytes, tx_drops, tx_errors;
	struct cg_probe_info peer_view; /* server's view of this path (upload) */
	uint64_t peer_view_ms;
	struct cg_echo probes; /* probes sent, for the echoes in replies (epoch.h) */
	struct cg_ratelimit rl, rl_move, rl_mtu;
};

struct client {
	struct cg_config *cfg; /* replaced by a reload */
	const struct cg_run *run;
	const uint8_t *k_tx, *k_rx;
	uint32_t session, tx_seq;
	int ep, wg_fd, tfd;
	struct sockaddr_storage wg_peer;
	int have_peer;
	struct cg_nl nl;
	struct link link[CG_MAX_LINKS];
	struct cg_hlink uh[CG_MAX_LINKS]; /* upload health, by link id */
	struct cg_hcfg hcfg;
	struct cg_replay replay;
	struct cg_arrivals arr;
	struct cg_link_rx rx[CG_MAX_LINKS];
	struct cg_status_writer sw;
	struct cg_ctl ctl;
	struct cg_ovr_table ovr; /* links paused or forced on by hand */
	struct cg_loader loader;
	int reload_again;
	int server_pass; /* IP pass in the server's replies: -2 no reply yet, -1 none */
	char config_error[600]; /* why the last reload was refused */
	uint64_t start_ms, next_status_ms, next_reconcile_ms, next_path_ms, last_traffic_ms;

	uint64_t up_pkts, up_bytes, up_toobig;
	uint32_t up_max;     /* largest WireGuard datagram since the last path check */
	uint32_t up_largest; /* the same over the last check period, for the status */
	uint64_t down_pkts, down_bytes, down_wg_drops, down_no_peer;
	uint64_t rx_malformed, rx_foreign, rx_auth_fail, rx_old, rx_dups, rx_trunc;
	uint64_t window_resets;      /* the server started over (epoch.h) */
	uint64_t new_before_open_ms; /* newest last_rx_ms of a link before its socket was reopened */
	struct cg_ratelimit rl_auth, rl_big, rl_wg, rl_full, rl_restart;

	struct cg_rxbatch in;
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
};

/* Why an interface does or does not carry the tunnel. */
enum { WHY_OK, WHY_PAUSED, WHY_EXCLUDED, WHY_DOWN, WHY_NOADDR, WHY_NOFAMILY, WHY_UNUSABLE, WHY_GONE };
static const char *const why_name[] = {
	"ok", "paused", "excluded", "down", "no address", "no address of the server's family", "unusable", "absent",
};

/* What a link may send to: its candidates (srvpick.h). */
struct cands {
	unsigned families; /* 1 << AF_* of the interface's usable addresses */
	int n;
	struct sockaddr_storage a[CG_MAX_CANDS];
};

static uint32_t srtt_ms(const struct link *l)
{
	return (uint32_t)(l->srtt8_us / 8000);
}

/* Live: the server answers our probes on it, so it works both ways. */
static int link_live(const struct link *l, uint64_t now_ms)
{
	return l->fd >= 0 && l->last_reply_ms &&
	       !cg_probes_stalled(l->unanswered, l->first_unanswered_ms, now_ms, srtt_ms(l));
}

static const char *link_state(const struct client *c, const struct link *l, uint64_t now_ms)
{
	if (l->fd < 0)
		return cg_ovr_get(&c->ovr, l->ifname) == CG_OVR_OFF ? "paused" : "down";
	if (!l->last_reply_ms)
		return "waiting";
	return link_live(l, now_ms) ? "live" : "stalled";
}

static void link_masks(const struct client *c, uint64_t now_ms, uint16_t *present, uint16_t *live)
{
	*present = *live = 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (c->link[i].fd < 0)
			continue;
		*present |= (uint16_t)(1u << i);
		if (link_live(&c->link[i], now_ms))
			*live |= (uint16_t)(1u << i);
	}
}

/* Links the server should be sending each download packet on. */
static uint16_t expect_mask(const struct client *c)
{
	uint16_t m = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].fd >= 0 && !c->link[i].peer_muted)
			m |= (uint16_t)(1u << i);
	return m;
}

/* Probe fast while there is traffic, slowly when the tunnel is idle. */
static uint32_t probe_interval(const struct client *c, uint64_t now_ms)
{
	return c->last_traffic_ms && now_ms - c->last_traffic_ms < c->cfg->probe_idle_ms ? c->cfg->probe_interval_ms
											: c->cfg->probe_idle_ms;
}

static int same_ip(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return 0;
	if (a->ss_family == AF_INET)
		return ((const struct sockaddr_in *)a)->sin_addr.s_addr == ((const struct sockaddr_in *)b)->sin_addr.s_addr;
	return !memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr, &((const struct sockaddr_in6 *)b)->sin6_addr, 16);
}

/* why: for the log, NULL for none. */
static void link_close(struct client *c, struct link *l, const char *why)
{
	if (l->fd < 0)
		return;
	epoll_ctl(c->ep, EPOLL_CTL_DEL, l->fd, NULL);
	close(l->fd);
	l->fd = -1;
	if (why)
		cg_info("link %s down: %s", l->ifname, why);
}

static struct link *link_find(struct client *c, const char *ifname)
{
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].used && !strcmp(c->link[i].ifname, ifname))
			return &c->link[i];
	return NULL;
}

static struct link *link_get(struct client *c, const char *ifname)
{
	struct link *l = link_find(c, ifname);

	if (l)
		return l;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (c->link[i].used)
			continue;
		l = &c->link[i];
		memset(l, 0, sizeof(*l));
		l->used = 1;
		l->fd = -1;
		snprintf(l->ifname, sizeof(l->ifname), "%s", ifname);
		return l;
	}
	return NULL;
}

/* Whether the configuration wants an interface: a [link] section overrides
 * the interfaces and exclude patterns. */
static int configured(const struct cg_config *cfg, const char *ifname)
{
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifname);

	return lc ? lc->enabled
		  : cg_match_any(ifname, cfg->include, cfg->ninclude) && !cg_match_any(ifname, cfg->exclude, cfg->nexclude);
}

/* The server list a link sends to: its [link] one, or the global one. */
static struct cg_srvlist servers_of(const struct cg_config *cfg, const char *ifname)
{
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifname);

	if (lc && lc->nserver)
		return (struct cg_srvlist){ lc->server, lc->nserver, lc->entry_n, lc->nentry };
	return (struct cg_srvlist){ cfg->server, cfg->nserver, cfg->entry_n, cfg->nentry };
}

/* Whether ifc carries the tunnel and, when it does, which server addresses
 * it may send to. A manual override (ctl.h) goes before the configuration.
 * No side effects: reconcile applies the choice. */
static int eligible(struct client *c, const struct cg_iface *ifc, struct cands *e)
{
	const struct cg_config *cfg = c->cfg;
	enum cg_ovr o = cg_ovr_get(&c->ovr, ifc->name);
	struct cg_srvlist list;

	e->families = 0;
	e->n = 0;
	if (!cg_ovr_wanted(o, configured(cfg, ifc->name)))
		return o == CG_OVR_OFF ? WHY_PAUSED : WHY_EXCLUDED;
	if (!(ifc->flags & IFF_UP) || !(ifc->flags & IFF_RUNNING))
		return WHY_DOWN;
	e->families = cg_src_families(ifc->addr, ifc->naddr);
	if (!e->families)
		return WHY_NOADDR;
	list = servers_of(cfg, ifc->name);
	e->n = cg_cands_build(list.a, list.n, e->families, e->a);
	return e->n ? WHY_OK : WHY_NOFAMILY;
}

static uint32_t sock_path_mtu(int fd, int family)
{
	int v = 0;
	socklen_t len = sizeof(v);

	if (family == AF_INET6 ? getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &v, &len)
			       : getsockopt(fd, IPPROTO_IP, IP_MTU, &v, &len))
		return 0;
	return v > 0 ? (uint32_t)v : 0;
}

/* Whether to log a move: each one in the first round, then once a minute
 * while no address answers (an outage would fill the log). */
static int move_loud(struct link *l, int n, uint64_t now_ms)
{
	return l->silent_moves <= (uint32_t)n || cg_ratelimit_ok(&l->rl_move, now_ms, 60000);
}

/* Points l at its server address among the candidates e of its interface
 * (srvpick.h) and opens its socket there, or keeps the one it has. When a
 * socket cannot be opened (no route to that address, say), it tries the
 * next candidate at once. */
static void link_open(struct client *c, struct link *l, const struct cg_iface *ifc, const struct cands *e,
		      uint64_t now_ms)
{
	const struct cg_config *cfg = c->cfg;
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifc->name);
	const struct sockaddr_storage *cur = &l->cand;
	struct cg_srvlist list = servers_of(cfg, ifc->name);
	struct sockaddr_storage local;
	char err[256], a[64], b[64];
	int i, k, quiet = 0, skipped = 0;

	/* Back to the first candidate when a family the link gained or lost
	 * changes its candidates, when its local address toward the server
	 * changed (a new lease or prefix), and when a reply ended a dead round.
	 * A new list goes there from apply_config. */
	if (cg_cands_changed(list.a, list.n, l->families, e->families)) {
		cur = NULL;
	} else if (l->back_to_first) {
		cur = NULL;
		cg_info("link %s: the server answers again after a round of its addresses without replies, back to %s",
			l->ifname, cg_addr_str(&e->a[0], a, sizeof(a)));
	} else if (l->local.ss_family && cg_addr_equal(&l->remote, &l->cand) &&
		   cg_iface_pick(ifc, &l->cand, &local) == 0 && !same_ip(&local, &l->local)) {
		cur = NULL;
	}
	l->families = e->families;
	l->back_to_first = 0;
	i = cur ? cg_cand_find(e->a, e->n, cur) : -1;
	k = cg_srv_pick(e->a, e->n, cur, now_ms, l->cand_since_ms, l->last_reply_ms,
			l->fd >= 0 ? cfg->server_failover_ms : 0, &l->silent_moves);
	if (i >= 0 && k != i) {
		uint64_t since = l->last_reply_ms > l->cand_since_ms ? l->last_reply_ms : l->cand_since_ms;

		l->failovers++;
		quiet = !move_loud(l, e->n, now_ms);
		if (!quiet)
			cg_info("link %s: no reply from %s for %u s, trying %s", l->ifname,
				cg_addr_str(&e->a[i], a, sizeof(a)), (unsigned)((now_ms - since) / 1000),
				cg_addr_str(&e->a[k], b, sizeof(b)));
	}
	for (int tries = 1;; tries++) {
		l->cand = e->a[k];
		l->cand_idx = k;
		l->ncand = e->n;
		if (cg_iface_pick(ifc, &l->cand, &local) < 0)
			return; /* never: the candidates are of the families ifc has */
		if (l->fd >= 0 && same_ip(&l->local, &local) && cg_addr_equal(&l->remote, &l->cand))
			return;
		if (l->fd >= 0 && quiet)
			link_close(c, l, NULL);
		else if (l->fd >= 0)
			link_close(c, l, same_ip(&l->local, &local) ? "server address changed" : "address changed");
		if (now_ms < l->retry_ms)
			return;
		l->fd = cg_udp_link(ifc->name, &local, &l->cand, err, sizeof(err));
		if (l->fd >= 0)
			break;
		if (e->n > 1 && cfg->server_failover_ms && tries < e->n) {
			int next = cg_srv_next(k, e->n, &l->silent_moves);

			skipped++;
			quiet = !move_loud(l, e->n, now_ms);
			if (!quiet)
				cg_info("link %s: cannot use %s (%s), trying %s", l->ifname, cg_addr_str(&e->a[k], a, sizeof(a)),
					err, cg_addr_str(&e->a[next], b, sizeof(b)));
			k = next;
			continue;
		}
		if (cg_ratelimit_ok(&l->rl, now_ms, 30000))
			cg_warn("link %s unusable: %s", ifc->name, err);
		l->retry_ms = now_ms + RECONCILE_MS;
		return;
	}
	/* Counted only now: a link that can open none of them is unusable, and
	 * its retries every RECONCILE_MS are no failovers. The silent moves
	 * count all the same, so the first reply still ends a dead round. */
	l->failovers += (uint64_t)skipped;
	cg_sock_buffers(l->fd, cfg->rcvbuf, cfg->sndbuf);
	{
		socklen_t len = sizeof(local);

		getsockname(l->fd, (struct sockaddr *)&local, &len);
	}
	l->local = local;
	l->remote = l->cand;
	l->up_since_ms = l->cand_since_ms = now_ms;
	l->path_mtu = sock_path_mtu(l->fd, l->remote.ss_family);
	if (l->last_rx_ms > c->new_before_open_ms)
		c->new_before_open_ms = l->last_rx_ms;
	l->last_rx_ms = l->last_reply_ms = l->last_probe_ms = 0;
	memset(&l->probes, 0, sizeof(l->probes));
	l->srtt8_us = 0;
	l->rtt_us = l->unanswered = l->probe_announced = 0;
	l->have_down_owd = l->peer_muted = 0;
	cg_health_reset(&c->uh[l - c->link], now_ms);
	if (cg_epoll_add(c->ep, l->fd, CG_EV(CG_EV_LINK, l - c->link)) < 0) {
		link_close(c, l, "epoll");
		return;
	}
	if (quiet)
		return;
	cg_info("link %s%s%s%s up: %s -> %s (id %d)", l->ifname, lc && lc->label[0] ? " (" : "",
		lc && lc->label[0] ? lc->label : "", lc && lc->label[0] ? ")" : "", cg_addr_str(&l->local, a, sizeof(a)),
		cg_addr_str(&l->remote, b, sizeof(b)), (int)(l - c->link));
}

/* Brings sockets in line with the interfaces netlink reports. */
static void reconcile(struct client *c, uint64_t now_ms)
{
	struct cands e;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		c->link[i].seen = 0;
		c->link[i].why = WHY_GONE;
	}
	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		struct link *l;
		int why;

		if (!ifc->name[0])
			continue;
		why = eligible(c, ifc, &e);
		/* A slot all the same when paused, or when its [link] section
		 * wants it and it has no address of the server's family, so
		 * that the status shows why. */
		if (why == WHY_OK || why == WHY_PAUSED || (why == WHY_NOFAMILY && cg_config_link(c->cfg, ifc->name)))
			l = link_get(c, ifc->name);
		else
			l = link_find(c, ifc->name);
		if (!l) {
			if (why == WHY_OK && cg_ratelimit_ok(&c->rl_full, now_ms, 60000))
				cg_warn("more than %d links, ignoring %s", CG_MAX_LINKS, ifc->name);
			continue;
		}
		l->why = why;
		if (why == WHY_PAUSED)
			link_close(c, l, "paused");
		if (why != WHY_OK)
			continue;
		l->seen = 1;
		link_open(c, l, ifc, &e, now_ms);
		if (l->fd < 0)
			l->why = WHY_UNUSABLE;
	}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		/* Down, gone, or without an address of the server's family: an
		 * outage of the link, which comes back through its first server
		 * address, with the same local address or a new one (srvpick.h).
		 * Not a pause or an exclusion. */
		if (l->used && !l->seen && l->why != WHY_PAUSED && l->why != WHY_EXCLUDED) {
			memset(&l->cand, 0, sizeof(l->cand));
			l->back_to_first = 0;
		}
		if (l->used && l->fd >= 0 && !l->seen)
			link_close(c, l,
				   l->why == WHY_GONE   ? "interface gone"
				   : l->why == WHY_DOWN ? "interface down"
							: why_name[l->why]);
	}
	c->next_reconcile_ms = now_ms + RECONCILE_MS;
}

/* Every RECONCILE_MS: the path MTU of each socket, and a warning when the
 * largest WireGuard datagram since the last check does not fit it. */
static void path_check(struct client *c, uint64_t now_ms)
{
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		int f = l->remote.ss_family;
		uint32_t need = cg_outer_len(f, c->up_max), wrap = cg_outer_len(f, CG_WG_OVERHEAD);

		if (l->fd < 0)
			continue;
		l->path_mtu = sock_path_mtu(l->fd, f);
		if (c->up_max && l->path_mtu && need > l->path_mtu && cg_ratelimit_ok(&l->rl_mtu, now_ms, 600000))
			cg_warn("link %s: WireGuard datagrams of %u bytes make %u-byte packets over IPv%c, more than its path "
				"MTU of %u: lower the WireGuard MTU to %u",
				l->ifname, c->up_max, need, f == AF_INET6 ? '6' : '4', l->path_mtu,
				l->path_mtu > wrap ? l->path_mtu - wrap : 0);
	}
	c->up_largest = c->up_max;
	c->up_max = 0;
	c->next_path_ms = now_ms + RECONCILE_MS;
}

/* Sends the m packets prepared in hdr/oiov: all of them on every carrying
 * link (health.h), one in mute_trickle on the other live links. */
static void send_links(struct client *c, int m, uint64_t now_ms)
{
	uint16_t present, live, carry;

	link_masks(c, now_ms, &present, &live);
	carry = cg_health_carriers(c->uh, CG_MAX_LINKS, present, live);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		int sel[CG_BATCH], k = 0, s;

		if (carry >> i & 1) {
			for (int j = 0; j < m; j++)
				sel[k++] = j;
		} else if ((live >> i & 1) && c->cfg->mute_trickle) {
			for (int j = 0; j < m; j++)
				if (cg_trickle(&c->uh[i], c->cfg->mute_trickle))
					sel[k++] = j;
		}
		if (!k)
			continue;
		for (int x = 0; x < k; x++) {
			cg_hdr_set_link(c->hdr[sel[x]], (uint8_t)i);
			memset(&c->out[x].msg_hdr, 0, sizeof(c->out[x].msg_hdr));
			c->out[x].msg_hdr.msg_iov = c->oiov[sel[x]];
			c->out[x].msg_hdr.msg_iovlen = 2;
		}
		s = sendmmsg(l->fd, c->out, (unsigned)k, MSG_DONTWAIT);
		if (s < 0) {
			if (errno == EAGAIN || errno == ENOBUFS) {
				l->tx_drops += (uint64_t)k;
			} else {
				l->tx_errors += (uint64_t)k;
				if (cg_ratelimit_ok(&l->rl, now_ms, 10000))
					cg_warn("link %s send: %s", l->ifname, strerror(errno));
			}
			continue;
		}
		l->tx_drops += (uint64_t)(k - s); /* the rest would not fit: drop, never wait */
		l->tx_pkts += (uint64_t)s;
		for (int x = 0; x < s; x++)
			l->tx_bytes += c->oiov[sel[x]][1].iov_len;
	}
}

static void wg_read(struct client *c)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(c->wg_fd, &c->in), m = 0;
		uint64_t now_us, now_ms;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		for (int i = 0; i < n; i++) {
			size_t len = c->in.msg[i].msg_len;
			struct cg_hdr h = { .type = CG_T_DATA, .session = c->session, .ts = (uint32_t)now_us };

			if (!len)
				continue;
			if ((c->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) || len > CG_MAX_PAYLOAD) {
				c->up_toobig++;
				if (cg_ratelimit_ok(&c->rl_big, now_ms, 30000))
					cg_warn("WireGuard datagram larger than %d bytes dropped: lower the WireGuard MTU",
						CG_MAX_PAYLOAD);
				continue;
			}
			if (len > c->up_max)
				c->up_max = (uint32_t)len; /* for the path MTU check */
			/* Downstream goes back to whoever last sent something shaped like
			 * WireGuard, not to any local sender. */
			if (cg_looks_like_wg(c->in.buf[i], len)) {
				c->wg_peer = c->in.from[i];
				c->have_peer = 1;
			}
			h.seq = c->tx_seq++;
			cg_hdr_write(c->hdr[m], &h, c->k_tx, c->in.buf[i], len);
			c->oiov[m][0].iov_base = c->hdr[m];
			c->oiov[m][0].iov_len = CG_HDR_LEN;
			c->oiov[m][1].iov_base = c->in.buf[i];
			c->oiov[m][1].iov_len = len;
			c->up_pkts++;
			c->up_bytes += len;
			m++;
		}
		if (m) {
			c->last_traffic_ms = now_ms;
			send_links(c, m, now_ms);
		}
		if (n < CG_BATCH)
			return;
	}
}

static void on_probe_reply(struct client *c, struct link *l, const struct cg_hdr *h, const uint8_t *payload,
			   uint32_t now32, uint64_t now_ms)
{
	struct cg_probe_info pi;
	uint32_t rtt;

	cg_probe_info_read(&pi, payload);
	/* Each probe is answered once: a copy of this reply replayed later
	 * cannot count for epoch.h. */
	cg_echo_take(&l->probes, pi.echo_ts, now_ms, CG_ECHO_MAX_AGE_MS);
	rtt = now32 - pi.echo_ts;
	if (rtt < 10u * 1000 * 1000) {
		l->rtt_us = rtt;
		l->srtt8_us = l->srtt8_us ? l->srtt8_us - l->srtt8_us / 8 + rtt : (uint64_t)rtt * 8;
	}
	l->last_reply_ms = now_ms;
	l->unanswered = 0;
	switch (cg_srv_on_reply(&l->silent_moves, l->ncand, l->cand_idx)) {
	case CG_SRV_BACK:
		l->back_to_first = 1;
		c->next_reconcile_ms = 0; /* at the next tick, not while this socket is being read */
		break;
	case CG_SRV_RESUMED: {
		char a[64];

		/* Once per dead round; its later moves were not logged. */
		cg_info("link %s: the server answers again after a round of its addresses without replies, at %s",
			l->ifname, cg_addr_str(&l->remote, a, sizeof(a)));
		break;
	}
	}
	l->peer_view = pi;
	l->peer_view_ms = now_ms;
	l->peer_muted = !!(h->flags & CG_F_MUTED);
	c->server_pass = cg_pass_get(h->flags);
	/* The server timed our probe's trip up this link; we time the reply's
	 * trip down and report it in the next probe. */
	if (h->flags & CG_F_OWD)
		cg_health_report(&c->uh[l - c->link], pi.owd, now_ms, CG_STALL_PROBES * c->cfg->probe_idle_ms);
	l->down_owd = now32 - h->ts;
	l->have_down_owd = 1;
}

/* Since the newest verified NEW packet on any link: for the restart check
 * only, never per packet. */
static uint64_t ms_since_new(const struct client *c, uint64_t now_ms)
{
	uint64_t last = c->new_before_open_ms;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].last_rx_ms > last)
			last = c->link[i].last_rx_ms;
	return now_ms - last;
}

/* A packet the window judged OLD: when it shows that the server started over
 * (epoch.h), the window starts again and the packet goes on as NEW, its MAC
 * verified (returns 1). Only a probe reply can show that, so only those pay
 * for a MAC here, and the echo is taken only from a verified one. */
static int started_over(struct client *c, struct link *l, const struct cg_hdr *h, const uint8_t *b, size_t len,
			uint64_t now_ms)
{
	struct cg_probe_info pi;
	int mac_ok, echo = 0;

	if (h->type != CG_T_PROBE_REPLY)
		return 0;
	mac_ok = cg_hdr_verify(b, len, c->k_rx);
	if (mac_ok) {
		cg_probe_info_read(&pi, b + CG_HDR_LEN);
		echo = cg_echo_take(&l->probes, pi.echo_ts, now_ms, CG_ECHO_MAX_AGE_MS);
	}
	if (!cg_restart_reply(CG_RP_OLD, h->type, mac_ok, echo, ms_since_new(c, now_ms), 2 * c->cfg->probe_idle_ms))
		return 0;
	cg_replay_reset(&c->replay);
	c->window_resets++;
	if (cg_ratelimit_ok(&c->rl_restart, now_ms, 10000))
		cg_info("link %s: the server started over, download window reset", l->ifname);
	return 1;
}

static void link_read(struct client *c, struct link *l)
{
	unsigned id = (unsigned)(l - c->link);

	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(l->fd, &c->in), q = 0;
		uint64_t now_us, now_ms;
		uint32_t now32;

		if (n < 0 && (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH))
			continue; /* an ICMP error from an earlier send; the queue may hold more */
		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		now32 = (uint32_t)now_us;
		for (int i = 0; i < n; i++) {
			uint8_t *b = c->in.buf[i];
			size_t len = c->in.msg[i].msg_len;
			struct cg_hdr h;
			int verified = 0;

			if (c->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) {
				c->rx_trunc++;
				continue;
			}
			if (cg_hdr_parse(&h, b, len) < 0 || (h.type != CG_T_DATA && h.type != CG_T_PROBE_REPLY)) {
				c->rx_malformed++;
				continue;
			}
			if (h.session != c->session) {
				c->rx_foreign++;
				continue;
			}
			switch (cg_replay_check(&c->replay, h.seq)) {
			case CG_RP_OLD:
				if (started_over(c, l, &h, b, len, now_ms)) {
					verified = 1;
					break;
				}
				c->rx_old++;
				continue;
			case CG_RP_DUP:
				c->rx_dups++;
				if (h.type == CG_T_DATA)
					cg_arr_dup(&c->arr, c->rx, h.seq, now32, id);
				continue;
			default:
				break;
			}
			if (!verified && !cg_hdr_verify(b, len, c->k_rx)) {
				c->rx_auth_fail++;
				if (cg_ratelimit_ok(&c->rl_auth, now_ms, 10000))
					cg_warn("link %s: packet failed authentication (wrong key?)", l->ifname);
				continue;
			}
			cg_replay_mark(&c->replay, h.seq);
			l->last_rx_ms = now_ms;
			if (h.type == CG_T_PROBE_REPLY) {
				on_probe_reply(c, l, &h, b + CG_HDR_LEN, now32, now_ms);
				continue;
			}
			cg_arr_first(&c->arr, c->rx, h.seq, now32, id, expect_mask(c));
			c->oiov[q][0].iov_base = b + CG_HDR_LEN;
			c->oiov[q][0].iov_len = len - CG_HDR_LEN;
			memset(&c->out[q].msg_hdr, 0, sizeof(c->out[q].msg_hdr));
			c->out[q].msg_hdr.msg_name = &c->wg_peer;
			c->out[q].msg_hdr.msg_namelen = cg_addr_len(&c->wg_peer);
			c->out[q].msg_hdr.msg_iov = c->oiov[q];
			c->out[q].msg_hdr.msg_iovlen = 1;
			c->down_pkts++;
			c->down_bytes += len - CG_HDR_LEN;
			q++;
		}
		if (q)
			c->last_traffic_ms = now_ms;
		if (q && !c->have_peer) {
			c->down_no_peer += (uint64_t)q; /* WireGuard has not sent anything yet */
		} else if (q) {
			int s = sendmmsg(c->wg_fd, c->out, (unsigned)q, MSG_DONTWAIT);

			if (s < q) {
				c->down_wg_drops += (uint64_t)(q - (s < 0 ? 0 : s));
				if (s < 0 && errno != EAGAIN && cg_ratelimit_ok(&c->rl_wg, now_ms, 10000))
					cg_warn("send to WireGuard: %s", strerror(errno));
			}
		}
		if (n < CG_BATCH)
			return;
	}
}

static void send_probe(struct client *c, struct link *l, uint64_t now_us, uint32_t interval)
{
	unsigned id = (unsigned)(l - c->link);
	uint64_t now_ms = now_us / 1000;
	uint8_t pkt[CG_HDR_LEN + CG_PROBE_INFO_LEN];
	struct cg_probe_info pi = { .echo_ts = 0,
				    .owd = l->have_down_owd ? l->down_owd : 0,
				    .interval_ms = interval,
				    .rx = (uint32_t)(c->rx[id].wins + c->rx[id].dups),
				    .wins = (uint32_t)c->rx[id].wins,
				    .lag_us = cg_lag_us(&c->rx[id]) };
	struct cg_hdr h = { .type = CG_T_PROBE,
			    .flags = (uint8_t)((l->have_down_owd ? CG_F_OWD : 0) |
					       (c->uh[id].state == CG_H_MUTED ? CG_F_MUTED : 0) |
					       cg_pass_flags(c->cfg->passthrough)),
			    .link = (uint8_t)id,
			    .session = c->session,
			    .seq = c->tx_seq++,
			    .ts = (uint32_t)now_us };

	cg_probe_info_write(pkt + CG_HDR_LEN, &pi);
	cg_hdr_write(pkt, &h, c->k_tx, pkt + CG_HDR_LEN, CG_PROBE_INFO_LEN);
	/* A full queue drops the probe like any other packet: that is a measurement, not an error. */
	if (send(l->fd, pkt, sizeof(pkt), MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ENOBUFS &&
	    cg_ratelimit_ok(&l->rl, now_ms, 10000))
		cg_warn("link %s probe: %s", l->ifname, strerror(errno));
	cg_echo_push(&l->probes, h.ts, now_ms);
	l->have_down_owd = 0;
	l->last_probe_ms = now_ms;
	l->probe_announced = interval;
	if (!l->unanswered++)
		l->first_unanswered_ms = now_ms;
}

static void health_tick(struct client *c, uint64_t now_ms)
{
	uint16_t present, live, changed;

	link_masks(c, now_ms, &present, &live);
	changed = cg_health_eval(c->uh, CG_MAX_LINKS, live, now_ms, &c->hcfg);
	for (int i = 0; changed; i++, changed >>= 1) {
		const struct cg_hlink *h = &c->uh[i];

		if (!(changed & 1))
			continue;
		if (h->state == CG_H_MUTED)
			cg_info("link %s: upload muted, %d ms behind the fastest link", c->link[i].ifname,
				h->behind_us / 1000);
		else if (h->unmuted_ms == now_ms)
			cg_info("link %s: upload unmuted, within %d ms of the fastest link", c->link[i].ifname,
				h->behind_us > 0 ? h->behind_us / 1000 : 0);
		else
			cg_info("link %s: upload unmuted, too few active links", c->link[i].ifname);
	}
}

/* -2 (no reply yet) and, unless none is set, -1 give null; -1: "none". */
static void json_pass(struct cg_json *j, const char *key, int pass, int none)
{
	if (pass < -1 || (pass < 0 && !none))
		cg_json_null(j, key);
	else
		cg_json_str(j, key, pass < 0 ? "none" : pass ? "on" : "off");
}

static void status_json(struct client *c, uint64_t now_ms, struct cg_json *j)
{
	char buf[64];

	cg_json_obj(j, NULL);
	cg_json_str(j, "mode", "client");
	cg_json_str(j, "version", CG_VERSION);
	cg_json_str(j, "description", c->cfg->description);
	cg_json_u64(j, "uptime_ms", now_ms - c->start_ms);
	snprintf(buf, sizeof(buf), "%08x", c->session);
	cg_json_str(j, "session", buf);
	cg_json_str(j, "wireguard", c->have_peer ? cg_addr_str(&c->wg_peer, buf, sizeof(buf)) : "");
	cg_json_u64(j, "probe_interval_ms", probe_interval(c, now_ms));
	cg_json_str(j, "config_error", c->config_error);
	cg_json_obj(j, "passthrough");
	json_pass(j, "requested", c->cfg->passthrough, 0);
	/* "none": the server does not apply IP pass (no passthrough_file). */
	json_pass(j, "server", c->server_pass, 1);
	cg_json_end(j, '}');
	cg_json_obj(j, "upload");
	cg_json_u64(j, "packets", c->up_pkts);
	cg_json_u64(j, "bytes", c->up_bytes);
	cg_json_u64(j, "too_big", c->up_toobig);
	cg_json_u64(j, "largest", c->up_largest);
	cg_json_end(j, '}');
	cg_json_obj(j, "download");
	cg_json_u64(j, "packets", c->down_pkts);
	cg_json_u64(j, "bytes", c->down_bytes);
	cg_json_u64(j, "duplicates", c->rx_dups);
	cg_json_u64(j, "too_old", c->rx_old);
	cg_json_u64(j, "window_resets", c->window_resets);
	cg_json_u64(j, "auth_failures", c->rx_auth_fail);
	cg_json_u64(j, "malformed", c->rx_malformed + c->rx_trunc);
	cg_json_u64(j, "foreign_session", c->rx_foreign);
	cg_json_u64(j, "wireguard_drops", c->down_wg_drops + c->down_no_peer);
	cg_json_end(j, '}');
	cg_json_arr(j, "overrides");
	for (int i = 0; i < c->ovr.n; i++) {
		cg_json_obj(j, NULL);
		cg_json_str(j, "name", c->ovr.e[i].name);
		cg_json_str(j, "override", cg_ovr_name(c->ovr.e[i].v));
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_arr(j, "links");
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];
		const struct cg_hlink *h = &c->uh[i];
		const struct cg_link_cfg *lc;

		/* Not the links a reload took out, unless set by hand. */
		if (!l->used || (l->fd < 0 && cg_ovr_get(&c->ovr, l->ifname) == CG_OVR_AUTO &&
				 !configured(c->cfg, l->ifname)))
			continue;
		lc = cg_config_link(c->cfg, l->ifname);
		cg_json_obj(j, NULL);
		cg_json_u64(j, "id", (uint64_t)i);
		cg_json_str(j, "name", l->ifname);
		cg_json_str(j, "label", lc ? lc->label : "");
		cg_json_str(j, "state", link_state(c, l, now_ms));
		cg_json_str(j, "reason", l->fd >= 0 ? "" : why_name[l->why]);
		cg_json_str(j, "override", cg_ovr_name(cg_ovr_get(&c->ovr, l->ifname)));
		cg_json_str(j, "local", l->fd >= 0 ? cg_addr_str(&l->local, buf, sizeof(buf)) : "");
		cg_json_str(j, "remote", l->fd >= 0 ? cg_addr_str(&l->remote, buf, sizeof(buf)) : "");
		cg_json_str(j, "family", l->fd < 0 ? "" : l->remote.ss_family == AF_INET6 ? "ipv6" : "ipv4");
		/* Its place in the server list, as filtered by family: 0 is the
		 * first, a later one means it failed over (srvpick.h). */
		if (l->fd >= 0)
			cg_json_u64(j, "candidate", (uint64_t)l->cand_idx);
		else
			cg_json_null(j, "candidate");
		cg_json_u64(j, "candidates", (uint64_t)l->ncand);
		cg_json_u64(j, "failovers", l->failovers);
		cg_json_u64(j, "path_mtu", l->fd >= 0 ? l->path_mtu : 0);
		cg_json_ms(j, "rtt_ms", l->srtt8_us / 8);
		cg_json_u64(j, "last_rx_ms_ago", l->last_rx_ms ? now_ms - l->last_rx_ms : 0);
		cg_json_str(j, "upload", h->state == CG_H_MUTED ? "muted" : "active");
		if (h->have_behind)
			cg_json_ms_signed(j, "upload_behind_ms", h->behind_us);
		else
			cg_json_null(j, "upload_behind_ms");
		cg_json_u64(j, "upload_mutes", h->mutes);
		cg_json_u64(j, "upload_state_ms", now_ms - h->changed_ms);
		cg_json_bool(j, "download_muted", l->peer_muted);
		cg_json_u64(j, "tx_packets", l->tx_pkts);
		cg_json_u64(j, "tx_bytes", l->tx_bytes);
		cg_json_u64(j, "tx_drops", l->tx_drops);
		cg_json_u64(j, "tx_errors", l->tx_errors);
		cg_json_u64(j, "rx_first", c->rx[i].wins);
		cg_json_u64(j, "rx_duplicate", c->rx[i].dups);
		cg_json_u64(j, "rx_late", c->rx[i].late);
		cg_json_u64(j, "rx_missed", c->rx[i].missed);
		cg_json_ms(j, "rx_lag_ms", cg_lag_us(&c->rx[i]));
		cg_json_obj(j, "server_view");
		cg_json_u64(j, "age_ms", l->peer_view_ms ? now_ms - l->peer_view_ms : 0);
		cg_json_u64(j, "rx", l->peer_view.rx);
		cg_json_u64(j, "first", l->peer_view.wins);
		cg_json_ms(j, "lag_ms", l->peer_view.lag_us);
		cg_json_end(j, '}');
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_end(j, '}');
}

static void write_status(struct client *c, uint64_t now_ms)
{
	struct cg_json j;

	cg_json_init(&j);
	status_json(c, now_ms, &j);
	cg_status_writer_submit(&c->sw, &j);
	cg_json_free(&j);
}

/* The STATE of an interface in "ctl links"; *up: its link when that carries
 * the tunnel, else NULL. */
static const char *row_state(struct client *c, const struct cg_iface *ifc, uint64_t now_ms, const struct link **up)
{
	struct cands e;
	int why = eligible(c, ifc, &e);
	const struct link *l = link_find(c, ifc->name);

	*up = why == WHY_OK && l && l->fd >= 0 ? l : NULL;
	return *up ? link_state(c, *up, now_ms) : why == WHY_OK ? "unusable" : why_name[why];
}

/* "cengarde ctl links": every interface netlink knows, as a table. The
 * addresses go last; STATE and LOCAL are as wide as their widest value (a
 * long reason, an IPv6 address). */
#define LINK_ROW "%-15s %-12s %-*s %-6s %-6s %9s %5s %9s  %-*s  %s\n"
static void links_text(struct client *c, uint64_t now_ms, struct cg_json *j)
{
	char a[64], b[64], remote[96], rtt[24], mtu[16], moves[24];
	const struct link *l;
	int ws = 10, w = 5;

	for (int i = 0; i < c->nl.nifs; i++) {
		int len;

		if (!c->nl.ifs[i].name[0])
			continue;
		len = (int)strlen(row_state(c, &c->nl.ifs[i], now_ms, &l));
		ws = len > ws ? len : ws;
		if (l) {
			len = (int)strlen(cg_addr_str(&l->local, a, sizeof(a)));
			w = len > w ? len : w;
		}
	}
	cg_json_raw(j, LINK_ROW, "INTERFACE", "LABEL", ws, "STATE", "MANUAL", "UPLOAD", "RTT", "MTU", "FAILOVERS", w,
		    "LOCAL", "REMOTE");
	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		const struct cg_link_cfg *lc;
		const struct link *slot;
		const char *state;
		enum cg_ovr o;

		if (!ifc->name[0])
			continue;
		lc = cg_config_link(c->cfg, ifc->name);
		o = cg_ovr_get(&c->ovr, ifc->name);
		state = row_state(c, ifc, now_ms, &l);
		if (l && l->srtt8_us)
			snprintf(rtt, sizeof(rtt), "%u.%u ms", (unsigned)(l->srtt8_us / 8000),
				 (unsigned)(l->srtt8_us / 800 % 10));
		else
			strcpy(rtt, "-");
		if (l && l->path_mtu)
			snprintf(mtu, sizeof(mtu), "%u", l->path_mtu);
		else
			strcpy(mtu, "-");
		slot = link_find(c, ifc->name);
		if (slot)
			snprintf(moves, sizeof(moves), "%llu", (unsigned long long)slot->failovers);
		else
			strcpy(moves, "-");
		/* With more than one candidate, which one: 1 is the first. */
		if (l && l->ncand > 1)
			snprintf(remote, sizeof(remote), "%s (%d/%d)", cg_addr_str(&l->remote, b, sizeof(b)), l->cand_idx + 1,
				 l->ncand);
		else
			snprintf(remote, sizeof(remote), "%s", l ? cg_addr_str(&l->remote, b, sizeof(b)) : "-");
		cg_json_raw(j, LINK_ROW, ifc->name, lc && lc->label[0] ? lc->label : "-", ws, state,
			    o == CG_OVR_AUTO ? "-" : cg_ovr_name(o),
			    l ? (c->uh[l - c->link].state == CG_H_MUTED ? "muted" : "active") : "-", rtt, mtu, moves, w,
			    l ? cg_addr_str(&l->local, a, sizeof(a)) : "-", remote);
	}
	for (int i = 0; i < c->ovr.n; i++)
		if (!cg_nl_find(&c->nl, c->ovr.e[i].name))
			cg_json_raw(j, LINK_ROW, c->ovr.e[i].name, "-", ws, "absent", cg_ovr_name(c->ovr.e[i].v), "-", "-",
				    "-", "-", w, "-", "-");
}

/* ---- reload ---- */

static void reload_start(struct client *c)
{
	if (cg_loader_start(&c->loader, c->run->path) < 0)
		c->reload_again = 1; /* once the load under way is done */
}

/* Puts next in place of the running configuration, keeping the session. */
static void apply_config(struct client *c, struct cg_config *next, uint64_t now_ms)
{
	struct cg_config *old = c->cfg;

	c->cfg = next;
	c->k_tx = next->key;
	c->k_rx = next->key + CG_SIPHASH_KEY_LEN;
	c->hcfg = cg_hcfg_of(next);
	cg_log_level = c->run->verbose ? CG_LOG_DEBUG : next->log_level;
	if (strcmp(old->status_file, next->status_file)) {
		cg_status_writer_stop(&c->sw);
		if (next->status_file[0] && cg_status_writer_start(&c->sw, next->status_file) < 0)
			cg_warn("status file %s: cannot start the writer thread", next->status_file);
	}
	if (old->rcvbuf != next->rcvbuf || old->sndbuf != next->sndbuf) {
		cg_sock_buffers(c->wg_fd, next->rcvbuf, next->rcvbuf);
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if (c->link[i].fd >= 0)
				cg_sock_buffers(c->link[i].fd, next->rcvbuf, next->sndbuf);
	}
	if (old->passthrough != next->passthrough && next->passthrough >= 0)
		cg_info("asking the server for IP pass %s", next->passthrough ? "on" : "off");
	/* A link whose own list changed starts again from its first address,
	 * as seen with the families it last opened with: not for an entry of
	 * another family, nor for a name resolved again in another order
	 * (srvpick.h). A link never opened has none, and no address to keep. */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		struct cg_srvlist a, b;

		if (!l->used)
			continue;
		a = servers_of(old, l->ifname);
		b = servers_of(next, l->ifname);
		if (!cg_lists_same(&a, &b, l->families))
			memset(&l->cand, 0, sizeof(l->cand));
	}
	cg_config_free(old);
	free(old);
	reconcile(c, now_ms);
}

/* A load finished. Returns -1 when the process has to stop (a restart that
 * could not happen). */
static int reload_done(struct client *c)
{
	struct cg_config *next;
	const char *why = NULL;
	char msg[700];

	if (!cg_loader_done(&c->loader, &next))
		return 0;
	if (!next) {
		cg_err("reload refused, still running the previous configuration: %s", c->loader.err);
		snprintf(c->config_error, sizeof(c->config_error), "%s", c->loader.err);
		snprintf(msg, sizeof(msg), "error: %s\n", c->loader.err);
	} else {
		cg_log_warnings(c->run->path, c->loader.warn);
		c->config_error[0] = '\0';
		why = cg_config_restart_needed(c->cfg, next);
		if (why) {
			snprintf(msg, sizeof(msg), "ok: %s changed, restarting\n", why);
		} else {
			apply_config(c, next, cg_now_ms());
			cg_info("reload: configuration applied");
			snprintf(msg, sizeof(msg), "ok\n");
		}
	}
	if (!why && c->reload_again) {
		/* Asked again while loading: whoever waits gets the newer outcome. */
		c->reload_again = 0;
		reload_start(c);
		return 0;
	}
	cg_ctl_reply_waiting(&c->ctl, c->ep, msg);
	if (!why)
		return 0;
	cg_info("reload: %s changed, restarting", why);
	cg_config_free(next);
	free(next);
	cg_status_writer_stop(&c->sw);
	cg_ctl_close(&c->ctl, c->ep);
	cg_reexec(c->run->argv);
	cg_err("restart: %s", strerror(errno));
	return -1;
}

/* ---- control socket ---- */

static void ctl_command(struct client *c, int k, uint64_t now_ms)
{
	struct cg_ctl_cmd cmd;
	struct cg_json j;
	char err[256];

	cg_json_init(&j);
	if (cg_ctl_parse(c->ctl.c[k].in, &cmd, err, sizeof(err)) < 0) {
		cg_json_raw(&j, "error: %s\n", err);
	} else {
		switch (cmd.op) {
		case CG_CTL_STATUS:
			status_json(c, now_ms, &j);
			cg_json_raw(&j, "\n");
			break;
		case CG_CTL_LINKS:
			links_text(c, now_ms, &j);
			break;
		case CG_CTL_LINK:
			if (cg_ovr_set(&c->ovr, cmd.ifname, cmd.ovr) < 0) {
				cg_json_raw(&j, "error: more than %d links set by hand; undo some with auto or reset\n",
					    CG_CTL_OVERRIDES);
				break;
			}
			cg_info("link %s: %s by hand", cmd.ifname,
				cmd.ovr == CG_OVR_OFF  ? "paused"
				: cmd.ovr == CG_OVR_ON ? "forced on"
						       : "back to the configuration");
			reconcile(c, now_ms);
			cg_json_raw(&j, "ok\n");
			break;
		case CG_CTL_RESET:
			cg_ovr_reset(&c->ovr);
			cg_info("every link back to the configuration");
			reconcile(c, now_ms);
			cg_json_raw(&j, "ok\n");
			break;
		case CG_CTL_RELOAD:
			cg_info("reloading %s", c->run->path);
			c->ctl.c[k].waiting = 1;
			c->ctl.c[k].deadline_ms = now_ms + CG_CTL_RELOAD_TIMEOUT_MS;
			reload_start(c);
			cg_json_free(&j);
			return; /* the reply goes out when the load is done */
		}
	}
	if (j.failed) {
		cg_json_free(&j);
		cg_ctl_reply(&c->ctl, c->ep, k, NULL, 0);
	} else {
		cg_ctl_reply(&c->ctl, c->ep, k, j.buf, j.len); /* takes the buffer */
	}
}

static void tick(struct client *c)
{
	uint64_t now_us = cg_now_us(), now_ms = now_us / 1000;
	uint32_t interval = probe_interval(c, now_ms);
	int failover = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		/* A new interval goes out at once, so the server never waits for a
		 * probe at the old rate. */
		if (l->fd >= 0 &&
		    (interval != l->probe_announced || now_ms - l->last_probe_ms + CG_TICK_MS / 2 >= interval))
			send_probe(c, l, now_us, interval);
		/* Its server address went silent: reconcile moves it (srvpick.h). */
		if (l->fd >= 0 && l->ncand > 1 &&
		    cg_srv_due(now_ms, l->cand_since_ms, l->last_reply_ms, c->cfg->server_failover_ms))
			failover = 1;
	}
	health_tick(c, now_ms);
	if (failover || now_ms >= c->next_reconcile_ms)
		reconcile(c, now_ms);
	if (now_ms >= c->next_path_ms)
		path_check(c, now_ms);
	if (c->sw.running && now_ms >= c->next_status_ms) {
		write_status(c, now_ms);
		c->next_status_ms = now_ms + c->cfg->status_interval_ms;
	}
	cg_ctl_expire(&c->ctl, c->ep, now_ms);
}

/* Reads the signals: 1 to stop, 0 to go on (SIGHUP starts a reload). */
static int signals(struct client *c)
{
	struct signalfd_siginfo si;
	int stop = 0;

	while (read(c->run->sigfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		if (si.ssi_signo != SIGHUP) {
			stop = 1;
		} else {
			cg_info("SIGHUP: reloading %s", c->run->path);
			reload_start(c);
		}
	}
	return stop;
}

int cg_client_run(struct cg_config *cfg, const struct cg_run *run)
{
	struct client *c = calloc(1, sizeof(*c));
	char err[256], buf[64];
	uint64_t last_traffic_us = 0;
	uint32_t busy;
	int rc = 1, rcv;

	if (!c) {
		cg_err("out of memory");
		cg_config_free(cfg);
		free(cfg);
		return 1;
	}
	c->cfg = cfg;
	c->run = run;
	c->k_tx = cfg->key;
	c->k_rx = cfg->key + CG_SIPHASH_KEY_LEN;
	c->ep = c->wg_fd = c->tfd = -1;
	c->nl.fd = -1;
	c->server_pass = -2;
	c->hcfg = cg_hcfg_of(cfg);
	cg_ctl_init(&c->ctl);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		c->link[i].fd = -1;
	if (cg_loader_init(&c->loader) < 0) {
		cg_err("eventfd: %s", strerror(errno));
		goto out;
	}
	cg_replay_reset(&c->replay);
	do {
		if (cg_random(&c->session, sizeof(c->session)) < 0 || cg_random(&c->tx_seq, sizeof(c->tx_seq)) < 0) {
			cg_err("getrandom: %s", strerror(errno));
			goto out;
		}
	} while (!c->session);
	c->start_ms = cg_now_ms();

	c->wg_fd = cg_udp_bind(&cfg->listen, 1, err, sizeof(err));
	if (c->wg_fd < 0) {
		cg_err("%s", err);
		goto out;
	}
	rcv = cg_sock_buffers(c->wg_fd, cfg->rcvbuf, cfg->rcvbuf);
	if (rcv < (1 << 20))
		cg_warn("receive buffer is only %d bytes; raise net.core.rmem_max or run with CAP_NET_ADMIN", rcv);
	if (cg_nl_open(&c->nl, err, sizeof(err)) < 0) {
		cg_err("%s", err);
		goto out;
	}
	c->ep = epoll_create1(EPOLL_CLOEXEC);
	c->tfd = cg_timerfd(CG_TICK_MS);
	if (c->ep < 0 || c->tfd < 0 || cg_epoll_add(c->ep, c->wg_fd, CG_EV(CG_EV_WG, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->nl.fd, CG_EV(CG_EV_NL, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->tfd, CG_EV(CG_EV_TIMER, 0)) < 0 ||
	    cg_epoll_add(c->ep, run->sigfd, CG_EV(CG_EV_SIG, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->loader.efd, CG_EV(CG_EV_LOAD, 0)) < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	/* The control socket is a convenience: the tunnel runs without it. */
	if (cfg->control_socket[0] && cg_ctl_open(&c->ctl, cfg->control_socket, c->ep, err, sizeof(err)) < 0)
		cg_warn("control socket: %s", err);
	if (cfg->status_file[0] && cg_status_writer_start(&c->sw, cfg->status_file) < 0)
		cg_warn("status file %s: cannot start the writer thread", cfg->status_file);
	cg_info("client %s: session %08x, WireGuard endpoint %s", CG_VERSION, c->session,
		cg_addr_str(&cfg->listen, buf, sizeof(buf)));
	if (cfg->passthrough >= 0)
		cg_info("asking the server for IP pass %s", cfg->passthrough ? "on" : "off");
	busy = cg_tune(cfg);
	reconcile(c, c->start_ms);

	for (;;) {
		struct epoll_event ev[32];
		int n = cg_wait(c->ep, ev, 32, busy, last_traffic_us), traffic = 0;

		if (n < 0 && errno != EINTR) {
			cg_err("epoll_wait: %s", strerror(errno));
			goto out;
		}
		for (int i = 0; i < n; i++) {
			uint32_t kind = (uint32_t)(ev[i].data.u64 >> 32), idx = (uint32_t)ev[i].data.u64;

			switch (kind) {
			case CG_EV_WG:
				wg_read(c);
				traffic = 1;
				break;
			case CG_EV_LINK:
				if (idx < CG_MAX_LINKS && c->link[idx].fd >= 0)
					link_read(c, &c->link[idx]);
				traffic = 1;
				break;
			case CG_EV_NL:
				cg_nl_read(&c->nl);
				if (c->nl.changed) {
					c->nl.changed = 0;
					reconcile(c, cg_now_ms());
				}
				break;
			case CG_EV_TIMER: {
				uint64_t expirations;

				if (read(c->tfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
					cg_warn("timerfd: %s", strerror(errno));
				tick(c);
				break;
			}
			case CG_EV_CTL: {
				uint64_t now_ms = cg_now_ms();
				int k = cg_ctl_event(&c->ctl, c->ep, idx, now_ms);

				if (k >= 0)
					ctl_command(c, k, now_ms);
				break;
			}
			case CG_EV_LOAD:
				if (reload_done(c) < 0)
					goto out;
				break;
			case CG_EV_SIG:
				if (signals(c)) {
					cg_info("client stopping");
					rc = 0;
					goto out;
				}
				break;
			}
		}
		if (traffic && busy)
			last_traffic_us = cg_now_us();
	}
out:
	cg_status_writer_stop(&c->sw);
	cg_ctl_close(&c->ctl, c->ep);
	cg_loader_free(&c->loader);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].fd >= 0)
			close(c->link[i].fd);
	if (c->wg_fd >= 0)
		close(c->wg_fd);
	if (c->tfd >= 0)
		close(c->tfd);
	if (c->ep >= 0)
		close(c->ep);
	cg_nl_close(&c->nl);
	cg_config_free(c->cfg);
	free(c->cfg);
	free(c);
	return rc;
}
