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
 * checked, so only the first copy of each packet pays for verification.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "arrival.h"
#include "engine.h"
#include "health.h"
#include "log.h"
#include "netlink.h"
#include "replay.h"
#include "sock.h"
#include "status.h"
#include "util.h"

#define RECONCILE_MS 5000

struct link {
	int used;
	char ifname[IFNAMSIZ];
	int fd;
	int seen; /* still wanted in the current reconcile pass */
	struct sockaddr_storage local, remote;
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
	struct cg_ratelimit rl;
};

struct client {
	const struct cg_config *cfg;
	const uint8_t *k_tx, *k_rx;
	uint32_t session, tx_seq;
	int ep, wg_fd, tfd, sigfd;
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
	uint64_t start_ms, next_status_ms, next_reconcile_ms, last_traffic_ms;

	uint64_t up_pkts, up_bytes, up_toobig;
	uint64_t down_pkts, down_bytes, down_wg_drops, down_no_peer;
	uint64_t rx_malformed, rx_foreign, rx_auth_fail, rx_old, rx_dups, rx_trunc;
	struct cg_ratelimit rl_auth, rl_big, rl_wg, rl_full;

	struct cg_rxbatch in;
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
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

static void link_close(struct client *c, struct link *l, const char *why)
{
	if (l->fd < 0)
		return;
	epoll_ctl(c->ep, EPOLL_CTL_DEL, l->fd, NULL);
	close(l->fd);
	l->fd = -1;
	cg_info("link %s down: %s", l->ifname, why);
}

static struct link *link_get(struct client *c, const char *ifname)
{
	struct link *free_slot = NULL;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (c->link[i].used && !strcmp(c->link[i].ifname, ifname))
			return &c->link[i];
		if (!c->link[i].used && !free_slot)
			free_slot = &c->link[i];
	}
	if (!free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = 1;
	free_slot->fd = -1;
	snprintf(free_slot->ifname, sizeof(free_slot->ifname), "%s", ifname);
	return free_slot;
}

/* Brings sockets in line with the interfaces netlink reports. */
static void reconcile(struct client *c, uint64_t now_ms)
{
	const struct cg_config *cfg = c->cfg;
	char err[256], a[64], b[64];

	for (int i = 0; i < CG_MAX_LINKS; i++)
		c->link[i].seen = 0;
	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		const struct cg_link_cfg *lc;
		const struct sockaddr_storage *servers;
		struct sockaddr_storage local, remote;
		int nservers, found = 0, wanted;
		struct link *l;

		if (!ifc->name[0])
			continue;
		lc = cg_config_link(cfg, ifc->name);
		/* A [link] section overrides the interfaces/exclude patterns. */
		wanted = lc ? lc->enabled
			    : cg_match_any(ifc->name, cfg->include, cfg->ninclude) &&
				      !cg_match_any(ifc->name, cfg->exclude, cfg->nexclude);
		if (!wanted || !(ifc->flags & IFF_UP) || !(ifc->flags & IFF_RUNNING))
			continue;
		servers = lc && lc->nserver ? lc->server : cfg->server;
		nservers = lc && lc->nserver ? lc->nserver : cfg->nserver;
		for (int k = 0; k < nservers && !found; k++)
			if (cg_iface_pick(ifc, servers[k].ss_family, &local) == 0) {
				remote = servers[k];
				found = 1;
			}
		if (!found)
			continue;
		l = link_get(c, ifc->name);
		if (!l) {
			if (cg_ratelimit_ok(&c->rl_full, now_ms, 60000))
				cg_warn("more than %d links, ignoring %s", CG_MAX_LINKS, ifc->name);
			continue;
		}
		l->seen = 1;
		if (l->fd >= 0 && same_ip(&l->local, &local) && cg_addr_equal(&l->remote, &remote))
			continue;
		if (l->fd >= 0)
			link_close(c, l, "address changed");
		if (now_ms < l->retry_ms)
			continue;
		l->fd = cg_udp_link(ifc->name, &local, &remote, err, sizeof(err));
		if (l->fd < 0) {
			if (cg_ratelimit_ok(&l->rl, now_ms, 30000))
				cg_warn("link %s unusable: %s", ifc->name, err);
			l->retry_ms = now_ms + RECONCILE_MS;
			continue;
		}
		cg_sock_buffers(l->fd, cfg->rcvbuf, cfg->sndbuf);
		{
			socklen_t len = sizeof(local);

			getsockname(l->fd, (struct sockaddr *)&local, &len);
		}
		l->local = local;
		l->remote = remote;
		l->up_since_ms = now_ms;
		l->last_rx_ms = l->last_reply_ms = l->last_probe_ms = 0;
		l->srtt8_us = 0;
		l->rtt_us = l->unanswered = l->probe_announced = 0;
		l->have_down_owd = l->peer_muted = 0;
		cg_health_reset(&c->uh[l - c->link], now_ms);
		if (cg_epoll_add(c->ep, l->fd, CG_EV(CG_EV_LINK, l - c->link)) < 0) {
			link_close(c, l, "epoll");
			continue;
		}
		cg_info("link %s%s%s%s up: %s -> %s (id %d)", l->ifname, lc && lc->label[0] ? " (" : "",
			lc && lc->label[0] ? lc->label : "", lc && lc->label[0] ? ")" : "",
			cg_addr_str(&l->local, a, sizeof(a)), cg_addr_str(&l->remote, b, sizeof(b)), (int)(l - c->link));
	}
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].used && c->link[i].fd >= 0 && !c->link[i].seen)
			link_close(c, &c->link[i], "interface gone, down or not eligible");
	c->next_reconcile_ms = now_ms + RECONCILE_MS;
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
	rtt = now32 - pi.echo_ts;
	if (rtt < 10u * 1000 * 1000) {
		l->rtt_us = rtt;
		l->srtt8_us = l->srtt8_us ? l->srtt8_us - l->srtt8_us / 8 + rtt : (uint64_t)rtt * 8;
	}
	l->last_reply_ms = now_ms;
	l->unanswered = 0;
	l->peer_view = pi;
	l->peer_view_ms = now_ms;
	l->peer_muted = !!(h->flags & CG_F_MUTED);
	/* The server timed our probe's trip up this link; we time the reply's
	 * trip down and report it in the next probe. */
	if (h->flags & CG_F_OWD)
		cg_health_report(&c->uh[l - c->link], pi.owd, now_ms, CG_STALL_PROBES * c->cfg->probe_idle_ms);
	l->down_owd = now32 - h->ts;
	l->have_down_owd = 1;
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
			if (!cg_hdr_verify(b, len, c->k_rx)) {
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
					       (c->uh[id].state == CG_H_MUTED ? CG_F_MUTED : 0)),
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

static void write_status(struct client *c, uint64_t now_ms)
{
	struct cg_json j;
	char buf[64];

	cg_json_init(&j);
	cg_json_obj(&j, NULL);
	cg_json_str(&j, "mode", "client");
	cg_json_str(&j, "version", CG_VERSION);
	cg_json_str(&j, "description", c->cfg->description);
	cg_json_u64(&j, "uptime_ms", now_ms - c->start_ms);
	snprintf(buf, sizeof(buf), "%08x", c->session);
	cg_json_str(&j, "session", buf);
	cg_json_str(&j, "wireguard", c->have_peer ? cg_addr_str(&c->wg_peer, buf, sizeof(buf)) : "");
	cg_json_u64(&j, "probe_interval_ms", probe_interval(c, now_ms));
	cg_json_obj(&j, "upload");
	cg_json_u64(&j, "packets", c->up_pkts);
	cg_json_u64(&j, "bytes", c->up_bytes);
	cg_json_u64(&j, "too_big", c->up_toobig);
	cg_json_end(&j, '}');
	cg_json_obj(&j, "download");
	cg_json_u64(&j, "packets", c->down_pkts);
	cg_json_u64(&j, "bytes", c->down_bytes);
	cg_json_u64(&j, "duplicates", c->rx_dups);
	cg_json_u64(&j, "too_old", c->rx_old);
	cg_json_u64(&j, "auth_failures", c->rx_auth_fail);
	cg_json_u64(&j, "malformed", c->rx_malformed + c->rx_trunc);
	cg_json_u64(&j, "foreign_session", c->rx_foreign);
	cg_json_u64(&j, "wireguard_drops", c->down_wg_drops + c->down_no_peer);
	cg_json_end(&j, '}');
	cg_json_arr(&j, "links");
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];
		const struct cg_hlink *h = &c->uh[i];
		const struct cg_link_cfg *lc;

		if (!l->used)
			continue;
		lc = cg_config_link(c->cfg, l->ifname);
		cg_json_obj(&j, NULL);
		cg_json_u64(&j, "id", (uint64_t)i);
		cg_json_str(&j, "name", l->ifname);
		cg_json_str(&j, "label", lc ? lc->label : "");
		cg_json_str(&j, "state", l->fd < 0 ? "down" : link_live(l, now_ms) ? "live" : "stalled");
		cg_json_str(&j, "local", l->fd >= 0 ? cg_addr_str(&l->local, buf, sizeof(buf)) : "");
		cg_json_str(&j, "remote", l->fd >= 0 ? cg_addr_str(&l->remote, buf, sizeof(buf)) : "");
		cg_json_ms(&j, "rtt_ms", l->srtt8_us / 8);
		cg_json_u64(&j, "last_rx_ms_ago", l->last_rx_ms ? now_ms - l->last_rx_ms : 0);
		cg_json_str(&j, "upload", h->state == CG_H_MUTED ? "muted" : "active");
		if (h->have_behind)
			cg_json_ms_signed(&j, "upload_behind_ms", h->behind_us);
		else
			cg_json_null(&j, "upload_behind_ms");
		cg_json_u64(&j, "upload_mutes", h->mutes);
		cg_json_u64(&j, "upload_state_ms", now_ms - h->changed_ms);
		cg_json_bool(&j, "download_muted", l->peer_muted);
		cg_json_u64(&j, "tx_packets", l->tx_pkts);
		cg_json_u64(&j, "tx_bytes", l->tx_bytes);
		cg_json_u64(&j, "tx_drops", l->tx_drops);
		cg_json_u64(&j, "tx_errors", l->tx_errors);
		cg_json_u64(&j, "rx_first", c->rx[i].wins);
		cg_json_u64(&j, "rx_duplicate", c->rx[i].dups);
		cg_json_u64(&j, "rx_late", c->rx[i].late);
		cg_json_u64(&j, "rx_missed", c->rx[i].missed);
		cg_json_ms(&j, "rx_lag_ms", cg_lag_us(&c->rx[i]));
		cg_json_obj(&j, "server_view");
		cg_json_u64(&j, "age_ms", l->peer_view_ms ? now_ms - l->peer_view_ms : 0);
		cg_json_u64(&j, "rx", l->peer_view.rx);
		cg_json_u64(&j, "first", l->peer_view.wins);
		cg_json_ms(&j, "lag_ms", l->peer_view.lag_us);
		cg_json_end(&j, '}');
		cg_json_end(&j, '}');
	}
	cg_json_end(&j, ']');
	cg_json_end(&j, '}');
	cg_status_writer_submit(&c->sw, &j);
	cg_json_free(&j);
}

static void tick(struct client *c)
{
	uint64_t now_us = cg_now_us(), now_ms = now_us / 1000;
	uint32_t interval = probe_interval(c, now_ms);

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		/* A new interval goes out at once, so the server never waits for a
		 * probe at the old rate. */
		if (l->fd >= 0 &&
		    (interval != l->probe_announced || now_ms - l->last_probe_ms + CG_TICK_MS / 2 >= interval))
			send_probe(c, l, now_us, interval);
	}
	health_tick(c, now_ms);
	if (now_ms >= c->next_reconcile_ms)
		reconcile(c, now_ms);
	if (c->sw.running && now_ms >= c->next_status_ms) {
		write_status(c, now_ms);
		c->next_status_ms = now_ms + c->cfg->status_interval_ms;
	}
}

int cg_client_run(const struct cg_config *cfg, int sigfd)
{
	struct client *c = calloc(1, sizeof(*c));
	char err[256], buf[64];
	uint64_t last_traffic_us = 0;
	uint32_t busy;
	int rc = 1, rcv;

	if (!c) {
		cg_err("out of memory");
		return 1;
	}
	c->cfg = cfg;
	c->k_tx = cfg->key;
	c->k_rx = cfg->key + CG_SIPHASH_KEY_LEN;
	c->sigfd = sigfd;
	c->ep = c->wg_fd = c->tfd = -1;
	c->nl.fd = -1;
	c->hcfg = (struct cg_hcfg){ .mute_behind_us = cfg->mute_behind_ms * 1000,
				    .unmute_behind_us = cfg->unmute_behind_ms * 1000,
				    .settle_ms = cfg->mute_settle_ms,
				    .min_active = cfg->min_active_links };
	for (int i = 0; i < CG_MAX_LINKS; i++)
		c->link[i].fd = -1;
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
	    cg_epoll_add(c->ep, sigfd, CG_EV(CG_EV_SIG, 0)) < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	if (cfg->status_file[0] && cg_status_writer_start(&c->sw, cfg->status_file) < 0)
		cg_warn("status file %s: cannot start the writer thread", cfg->status_file);
	cg_info("client %s: session %08x, WireGuard endpoint %s", CG_VERSION, c->session,
		cg_addr_str(&cfg->listen, buf, sizeof(buf)));
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
			case CG_EV_SIG:
				cg_info("client stopping");
				rc = 0;
				goto out;
			}
		}
		if (traffic && busy)
			last_traffic_us = cg_now_us();
	}
out:
	cg_status_writer_stop(&c->sw);
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
	free(c);
	return rc;
}
