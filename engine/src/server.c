/* cengarde server: one listening socket for every client path, one socket
 * towards WireGuard per session.
 *
 * A session is created only by a packet whose MAC verifies, and a path (a
 * client uplink, keyed by session and link id) only learns or changes its
 * address from a verified packet, so nobody can make the server send traffic
 * to an address of their choosing. Each session talks to WireGuard from its
 * own socket, so WireGuard sees every client as a distinct endpoint and many
 * clients can share one port. Link health (health.h) mutes the download on
 * paths that lag far behind the fastest one, from the delays the client
 * reports in its probes.
 *
 * IP pass: the client's probes ask for it on or off (CG_F_PASS_SET); the
 * newest session decides, and the server writes its wish to passthrough_file
 * for the system to apply (contrib/vps), from a thread so the loop never
 * waits on the disk. A reload applies in place, as on the client.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "arrival.h"
#include "ctl.h"
#include "engine.h"
#include "health.h"
#include "idmap.h"
#include "log.h"
#include "replay.h"
#include "sock.h"
#include "status.h"
#include "util.h"

/* Probe interval assumed for a path until its client announces one. */
#define DEFAULT_INTERVAL_MS 1000

struct path {
	int used;
	struct sockaddr_storage addr;
	uint64_t since_ms, last_rx_ms;
	uint64_t last_owd_ms; /* last probe saying the client hears our replies */
	uint32_t interval_ms; /* the client's probe interval on this path */
	int peer_muted;       /* the client carries no upload on this path */
	uint64_t tx_pkts, tx_bytes, tx_drops;
	struct cg_probe_info peer_view; /* client's view of this path (download) */
	uint64_t peer_view_ms;
};

struct session {
	int used;
	uint32_t id;
	int wg_fd;
	uint32_t tx_seq;
	int pass; /* IP pass its probes ask for: -1 nothing */
	uint64_t created_ms, last_rx_ms;
	struct path path[CG_MAX_LINKS];
	struct cg_hlink dh[CG_MAX_LINKS]; /* download health, by link id */
	struct cg_replay replay;
	struct cg_arrivals arr;
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t up_pkts, up_bytes, down_pkts, down_bytes, wg_drops, toobig;
};

struct server {
	struct cg_config *cfg; /* replaced by a reload */
	const struct cg_run *run;
	const uint8_t *k_tx, *k_rx;
	int ep, lfd, tfd;
	struct session *s;
	uint32_t max;
	int32_t newest; /* index of the newest session, -1: none */
	struct cg_idmap ids; /* session id -> index into s */
	struct cg_hcfg hcfg;
	struct cg_status_writer sw;
	struct cg_status_writer pw; /* passthrough_file */
	int pass;         /* IP pass the newest session asks for: -1 nothing yet */
	int pass_written; /* last one handed to pw: -1 none */
	struct cg_ctl ctl;
	struct cg_loader loader;
	int reload_again;
	char config_error[600];
	uint64_t start_ms, next_status_ms, next_sweep_ms;

	uint64_t rx_malformed, rx_auth_fail, rx_old, rx_dups, rx_trunc, sessions_full;
	struct cg_ratelimit rl_auth, rl_full, rl_send;

	struct cg_rxbatch in;
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
	/* per listen batch: payloads for WireGuard and probe replies */
	uint32_t q_sess[CG_BATCH];
	uint8_t reply[CG_BATCH][CG_HDR_LEN + CG_PROBE_INFO_LEN];
	struct sockaddr_storage reply_to[CG_BATCH];
	struct mmsghdr rmsg[CG_BATCH];
	struct iovec riov[CG_BATCH];
};

/* Live: the client's packets arrive on it and its probes say our replies do
 * too, so it works both ways (a dead download is not used for either). */
static int path_live(const struct path *P, uint64_t now_ms)
{
	return P->used && !cg_path_stalled(P->last_rx_ms, now_ms, P->interval_ms) &&
	       !cg_path_stalled(P->last_owd_ms, now_ms, P->interval_ms);
}

static void path_masks(const struct session *S, uint64_t now_ms, uint16_t *present, uint16_t *live)
{
	*present = *live = 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!S->path[i].used)
			continue;
		*present |= (uint16_t)(1u << i);
		if (path_live(&S->path[i], now_ms))
			*live |= (uint16_t)(1u << i);
	}
}

/* Paths the client should be sending each upload packet on. */
static uint16_t expect_mask(const struct session *S, uint64_t now_ms)
{
	uint16_t m = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (path_live(&S->path[i], now_ms) && !S->path[i].peer_muted)
			m |= (uint16_t)(1u << i);
	return m;
}

/* ---- session table ---- */

static struct session *lookup(struct server *s, uint32_t id)
{
	int32_t i = cg_idmap_get(&s->ids, id);

	return i < 0 ? NULL : &s->s[i];
}

static struct session *session_create(struct server *s, uint32_t id, const struct sockaddr_storage *from,
				      uint64_t now_ms)
{
	char a[64];
	struct session *S = NULL;
	int fd;

	for (uint32_t i = 0; i < s->max; i++)
		if (!s->s[i].used) {
			S = &s->s[i];
			break;
		}
	if (!S) {
		s->sessions_full++;
		if (cg_ratelimit_ok(&s->rl_full, now_ms, 10000))
			cg_warn("session limit (%u) reached, refusing %08x", s->max, id);
		return NULL;
	}
	fd = cg_udp_socket(s->cfg->wireguard.ss_family);
	if (fd < 0 || connect(fd, (const struct sockaddr *)&s->cfg->wireguard, cg_addr_len(&s->cfg->wireguard)) < 0) {
		cg_err("session %08x: socket to WireGuard: %s", id, strerror(errno));
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	cg_sock_buffers(fd, s->cfg->rcvbuf, s->cfg->rcvbuf);
	memset(S, 0, sizeof(*S));
	S->used = 1;
	S->id = id;
	S->wg_fd = fd;
	if (cg_random(&S->tx_seq, sizeof(S->tx_seq)) < 0)
		S->tx_seq = (uint32_t)now_ms;
	S->created_ms = S->last_rx_ms = now_ms;
	S->pass = -1;
	cg_replay_reset(&S->replay);
	if (cg_epoll_add(s->ep, fd, CG_EV(CG_EV_WG, S - s->s)) < 0) {
		close(fd);
		S->used = 0;
		return NULL;
	}
	cg_idmap_put(&s->ids, id, (int32_t)(S - s->s));
	s->newest = (int32_t)(S - s->s);
	cg_info("session %08x: new client from %s", id, cg_addr_str(from, a, sizeof(a)));
	return S;
}

static void session_destroy(struct server *s, struct session *S, const char *why)
{
	cg_info("session %08x closed: %s", S->id, why);
	epoll_ctl(s->ep, EPOLL_CTL_DEL, S->wg_fd, NULL);
	close(S->wg_fd);
	cg_idmap_del(&s->ids, S->id);
	S->used = 0;
	if (s->newest != (int32_t)(S - s->s))
		return;
	/* IP pass stays as it was until the next newest session asks. */
	s->newest = -1;
	for (uint32_t i = 0; i < s->max; i++)
		if (s->s[i].used && (s->newest < 0 || s->s[i].created_ms > s->s[s->newest].created_ms))
			s->newest = (int32_t)i;
}

/* ---- client -> WireGuard ---- */

/* up_owd: the probe's trip up this path, timed here, for the client's link
 * health. */
static void queue_reply(struct server *s, int *nr, struct session *S, unsigned link, const struct cg_hdr *probe,
			uint32_t up_owd, uint64_t now_us)
{
	uint8_t *r = s->reply[*nr];
	struct cg_probe_info pi = { .echo_ts = probe->ts,
				    .owd = up_owd,
				    .rx = (uint32_t)(S->rx[link].wins + S->rx[link].dups),
				    .wins = (uint32_t)S->rx[link].wins,
				    .lag_us = cg_lag_us(&S->rx[link]) };
	struct cg_hdr h = { .type = CG_T_PROBE_REPLY,
			    .flags = (uint8_t)(CG_F_OWD | (S->dh[link].state == CG_H_MUTED ? CG_F_MUTED : 0) |
				       cg_pass_flags(s->pw.running ? s->pass_written : -1)),
			    .link = (uint8_t)link,
			    .session = S->id,
			    .seq = S->tx_seq++,
			    .ts = (uint32_t)now_us };

	cg_probe_info_write(r + CG_HDR_LEN, &pi);
	cg_hdr_write(r, &h, s->k_tx, r + CG_HDR_LEN, CG_PROBE_INFO_LEN);
	s->reply_to[*nr] = S->path[link].addr;
	s->riov[*nr].iov_base = r;
	s->riov[*nr].iov_len = CG_HDR_LEN + CG_PROBE_INFO_LEN;
	memset(&s->rmsg[*nr].msg_hdr, 0, sizeof(s->rmsg[*nr].msg_hdr));
	s->rmsg[*nr].msg_hdr.msg_name = &s->reply_to[*nr];
	s->rmsg[*nr].msg_hdr.msg_namelen = cg_addr_len(&s->reply_to[*nr]);
	s->rmsg[*nr].msg_hdr.msg_iov = &s->riov[*nr];
	s->rmsg[*nr].msg_hdr.msg_iovlen = 1;
	(*nr)++;
}

static void on_probe(struct server *s, struct session *S, struct path *P, unsigned link, const struct cg_hdr *h,
		     const uint8_t *payload, uint64_t now_ms)
{
	S->pass = cg_pass_get(h->flags);
	if (S->pass >= 0 && s->newest == (int32_t)(S - s->s))
		s->pass = S->pass;
	cg_probe_info_read(&P->peer_view, payload);
	P->peer_view_ms = now_ms;
	if (P->peer_view.interval_ms >= 100 && P->peer_view.interval_ms <= 600000)
		P->interval_ms = P->peer_view.interval_ms;
	P->peer_muted = !!(h->flags & CG_F_MUTED);
	/* The client timed our last reply's trip down this path. */
	if (h->flags & CG_F_OWD) {
		P->last_owd_ms = now_ms;
		cg_health_report(&S->dh[link], P->peer_view.owd, now_ms,
				 CG_STALL_PROBES * (P->interval_ms > DEFAULT_INTERVAL_MS ? P->interval_ms : DEFAULT_INTERVAL_MS));
	}
}

/* Hands queued payloads to WireGuard, one sendmmsg per session, in order. */
static void flush_wg(struct server *s, int nq)
{
	for (int i = 0; i < nq; i++) {
		uint32_t idx = s->q_sess[i];
		struct session *S;
		int m = 0, sent;

		if (idx == UINT32_MAX)
			continue;
		S = &s->s[idx];
		for (int j = i; j < nq; j++) {
			if (s->q_sess[j] != idx)
				continue;
			memset(&s->out[m].msg_hdr, 0, sizeof(s->out[m].msg_hdr));
			s->out[m].msg_hdr.msg_iov = s->oiov[j];
			s->out[m].msg_hdr.msg_iovlen = 1;
			m++;
			s->q_sess[j] = UINT32_MAX;
		}
		sent = sendmmsg(S->wg_fd, s->out, (unsigned)m, MSG_DONTWAIT);
		if (sent < m)
			S->wg_drops += (uint64_t)(m - (sent < 0 ? 0 : sent));
	}
}

static void listen_read(struct server *s)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(s->lfd, &s->in), nq = 0, nr = 0;
		uint64_t now_us, now_ms;
		uint32_t now32;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		now32 = (uint32_t)now_us;
		for (int i = 0; i < n; i++) {
			uint8_t *b = s->in.buf[i];
			size_t len = s->in.msg[i].msg_len;
			const struct sockaddr_storage *from = &s->in.from[i];
			struct session *S;
			struct path *P;
			struct cg_hdr h;
			int verified = 0;
			char a[64], o[64];

			if (s->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) {
				s->rx_trunc++;
				continue;
			}
			if (cg_hdr_parse(&h, b, len) < 0 || (h.type != CG_T_DATA && h.type != CG_T_PROBE) ||
			    h.link >= CG_MAX_LINKS) {
				s->rx_malformed++;
				if (cg_ratelimit_ok(&s->rl_auth, now_ms, 10000))
					cg_warn("malformed packet from %s (not cengarde protocol v%d?)",
						cg_addr_str(from, a, sizeof(a)), CG_PROTO_VERSION);
				continue;
			}
			S = lookup(s, h.session);
			if (!S) {
				/* Nothing is created for a packet that does not authenticate. */
				if (!cg_hdr_verify(b, len, s->k_rx)) {
					s->rx_auth_fail++;
					if (cg_ratelimit_ok(&s->rl_auth, now_ms, 10000))
						cg_warn("unauthenticated packet from %s (wrong key or not cengarde)",
							cg_addr_str(from, a, sizeof(a)));
					continue;
				}
				verified = 1;
				S = session_create(s, h.session, from, now_ms);
				if (!S)
					continue;
			}
			switch (cg_replay_check(&S->replay, h.seq)) {
			case CG_RP_OLD:
				s->rx_old++;
				continue;
			case CG_RP_DUP:
				s->rx_dups++;
				if (h.type == CG_T_DATA)
					cg_arr_dup(&S->arr, S->rx, h.seq, now32, h.link);
				continue;
			default:
				break;
			}
			if (!verified && !cg_hdr_verify(b, len, s->k_rx)) {
				s->rx_auth_fail++;
				if (cg_ratelimit_ok(&s->rl_auth, now_ms, 10000))
					cg_warn("session %08x: packet from %s failed authentication", S->id,
						cg_addr_str(from, a, sizeof(a)));
				continue;
			}
			cg_replay_mark(&S->replay, h.seq);
			S->last_rx_ms = now_ms;
			P = &S->path[h.link];
			if (!P->used) {
				memset(P, 0, sizeof(*P));
				P->used = 1;
				P->addr = *from;
				P->since_ms = now_ms;
				P->interval_ms = DEFAULT_INTERVAL_MS;
				cg_health_reset(&S->dh[h.link], now_ms);
				cg_info("session %08x: link %u via %s", S->id, h.link, cg_addr_str(from, a, sizeof(a)));
			} else if (!cg_addr_equal(&P->addr, from)) {
				cg_info("session %08x: link %u moved %s -> %s", S->id, h.link,
					cg_addr_str(&P->addr, o, sizeof(o)), cg_addr_str(from, a, sizeof(a)));
				P->addr = *from;
			}
			P->last_rx_ms = now_ms;
			if (h.type == CG_T_PROBE) {
				on_probe(s, S, P, h.link, &h, b + CG_HDR_LEN, now_ms);
				queue_reply(s, &nr, S, h.link, &h, now32 - h.ts, now_us);
				continue;
			}
			cg_arr_first(&S->arr, S->rx, h.seq, now32, h.link, expect_mask(S, now_ms));
			S->up_pkts++;
			S->up_bytes += len - CG_HDR_LEN;
			s->q_sess[nq] = (uint32_t)(S - s->s);
			s->oiov[nq][0].iov_base = b + CG_HDR_LEN;
			s->oiov[nq][0].iov_len = len - CG_HDR_LEN;
			nq++;
		}
		if (nq)
			flush_wg(s, nq);
		if (nr) {
			int sent = sendmmsg(s->lfd, s->rmsg, (unsigned)nr, MSG_DONTWAIT);

			if (sent < 0 && errno != EAGAIN && cg_ratelimit_ok(&s->rl_send, now_ms, 10000))
				cg_warn("probe reply: %s", strerror(errno));
		}
		if (n < CG_BATCH)
			return;
	}
}

/* ---- WireGuard -> client ---- */

static void wg_read(struct server *s, struct session *S)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(S->wg_fd, &s->in), m = 0;
		uint16_t present, live, carry;
		uint64_t now_us, now_ms;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		for (int i = 0; i < n; i++) {
			size_t len = s->in.msg[i].msg_len;
			struct cg_hdr h = { .type = CG_T_DATA, .session = S->id, .ts = (uint32_t)now_us };

			if ((s->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) || len > CG_MAX_PAYLOAD || !len) {
				S->toobig++;
				continue;
			}
			h.seq = S->tx_seq++;
			cg_hdr_write(s->hdr[m], &h, s->k_tx, s->in.buf[i], len);
			s->oiov[m][0].iov_base = s->hdr[m];
			s->oiov[m][0].iov_len = CG_HDR_LEN;
			s->oiov[m][1].iov_base = s->in.buf[i];
			s->oiov[m][1].iov_len = len;
			S->down_pkts++;
			S->down_bytes += len;
			m++;
		}
		path_masks(S, now_ms, &present, &live);
		carry = cg_health_carriers(S->dh, CG_MAX_LINKS, present, live);
		for (int p = 0; m && p < CG_MAX_LINKS; p++) {
			struct path *P = &S->path[p];
			int sel[CG_BATCH], k = 0, sent;

			if (carry >> p & 1) {
				for (int j = 0; j < m; j++)
					sel[k++] = j;
			} else if ((live >> p & 1) && s->cfg->mute_trickle) {
				for (int j = 0; j < m; j++)
					if (cg_trickle(&S->dh[p], s->cfg->mute_trickle))
						sel[k++] = j;
			}
			if (!k)
				continue;
			for (int x = 0; x < k; x++) {
				cg_hdr_set_link(s->hdr[sel[x]], (uint8_t)p);
				memset(&s->out[x].msg_hdr, 0, sizeof(s->out[x].msg_hdr));
				s->out[x].msg_hdr.msg_name = &P->addr;
				s->out[x].msg_hdr.msg_namelen = cg_addr_len(&P->addr);
				s->out[x].msg_hdr.msg_iov = s->oiov[sel[x]];
				s->out[x].msg_hdr.msg_iovlen = 2;
			}
			sent = sendmmsg(s->lfd, s->out, (unsigned)k, MSG_DONTWAIT);
			if (sent < 0) {
				P->tx_drops += (uint64_t)k;
				if (errno != EAGAIN && errno != ENOBUFS && cg_ratelimit_ok(&s->rl_send, now_ms, 10000))
					cg_warn("session %08x link %d send: %s", S->id, p, strerror(errno));
				continue;
			}
			P->tx_drops += (uint64_t)(k - sent);
			P->tx_pkts += (uint64_t)sent;
			for (int x = 0; x < sent; x++)
				P->tx_bytes += s->oiov[sel[x]][1].iov_len;
		}
		if (n < CG_BATCH)
			return;
	}
}

/* ---- housekeeping ---- */

static void health_tick(struct server *s, uint64_t now_ms)
{
	for (uint32_t i = 0; i < s->max; i++) {
		struct session *S = &s->s[i];
		uint16_t present, live, changed;

		if (!S->used)
			continue;
		path_masks(S, now_ms, &present, &live);
		changed = cg_health_eval(S->dh, CG_MAX_LINKS, live, now_ms, &s->hcfg);
		for (int p = 0; changed; p++, changed >>= 1) {
			const struct cg_hlink *h = &S->dh[p];

			if (!(changed & 1))
				continue;
			if (h->state == CG_H_MUTED)
				cg_info("session %08x link %d: download muted, %d ms behind the fastest link", S->id, p,
					h->behind_us / 1000);
			else if (h->unmuted_ms == now_ms)
				cg_info("session %08x link %d: download unmuted, within %d ms of the fastest link", S->id, p,
					h->behind_us > 0 ? h->behind_us / 1000 : 0);
			else
				cg_info("session %08x link %d: download unmuted, too few active links", S->id, p);
		}
	}
}

static void sweep(struct server *s, uint64_t now_ms)
{
	char a[64];

	for (uint32_t i = 0; i < s->max; i++) {
		struct session *S = &s->s[i];

		if (!S->used)
			continue;
		if (now_ms - S->last_rx_ms > s->cfg->session_timeout_ms) {
			session_destroy(s, S, "idle");
			continue;
		}
		for (int p = 0; p < CG_MAX_LINKS; p++)
			if (S->path[p].used && now_ms - S->path[p].last_rx_ms > s->cfg->path_timeout_ms) {
				cg_info("session %08x: link %d via %s expired", S->id, p,
					cg_addr_str(&S->path[p].addr, a, sizeof(a)));
				S->path[p].used = 0;
			}
	}
}

static void json_pass(struct cg_json *j, const char *key, int pass)
{
	if (pass < 0)
		cg_json_null(j, key);
	else
		cg_json_str(j, key, pass ? "on" : "off");
}

static void status_json(struct server *s, uint64_t now_ms, struct cg_json *j)
{
	char buf[64];

	cg_json_obj(j, NULL);
	cg_json_str(j, "mode", "server");
	cg_json_str(j, "version", CG_VERSION);
	cg_json_str(j, "description", s->cfg->description);
	cg_json_u64(j, "uptime_ms", now_ms - s->start_ms);
	cg_json_str(j, "config_error", s->config_error);
	json_pass(j, "passthrough", s->pw.running ? s->pass_written : -1);
	cg_json_obj(j, "rx");
	cg_json_u64(j, "duplicates", s->rx_dups);
	cg_json_u64(j, "too_old", s->rx_old);
	cg_json_u64(j, "auth_failures", s->rx_auth_fail);
	cg_json_u64(j, "malformed", s->rx_malformed + s->rx_trunc);
	cg_json_u64(j, "sessions_refused", s->sessions_full);
	cg_json_end(j, '}');
	cg_json_arr(j, "sessions");
	for (uint32_t i = 0; i < s->max; i++) {
		const struct session *S = &s->s[i];

		if (!S->used)
			continue;
		cg_json_obj(j, NULL);
		snprintf(buf, sizeof(buf), "%08x", S->id);
		cg_json_str(j, "id", buf);
		cg_json_bool(j, "newest", s->newest == (int32_t)i);
		json_pass(j, "passthrough", S->pass);
		cg_json_u64(j, "age_ms", now_ms - S->created_ms);
		cg_json_u64(j, "last_rx_ms_ago", now_ms - S->last_rx_ms);
		cg_json_u64(j, "upload_packets", S->up_pkts);
		cg_json_u64(j, "upload_bytes", S->up_bytes);
		cg_json_u64(j, "download_packets", S->down_pkts);
		cg_json_u64(j, "download_bytes", S->down_bytes);
		cg_json_u64(j, "wireguard_drops", S->wg_drops);
		cg_json_u64(j, "too_big", S->toobig);
		cg_json_arr(j, "links");
		for (int p = 0; p < CG_MAX_LINKS; p++) {
			const struct path *P = &S->path[p];
			const struct cg_hlink *h = &S->dh[p];

			if (!P->used)
				continue;
			cg_json_obj(j, NULL);
			cg_json_u64(j, "id", (uint64_t)p);
			cg_json_str(j, "address", cg_addr_str(&P->addr, buf, sizeof(buf)));
			cg_json_str(j, "state", path_live(P, now_ms) ? "live" : "stalled");
			cg_json_u64(j, "last_rx_ms_ago", now_ms - P->last_rx_ms);
			cg_json_u64(j, "probe_interval_ms", P->interval_ms);
			cg_json_str(j, "download", h->state == CG_H_MUTED ? "muted" : "active");
			if (h->have_behind)
				cg_json_ms_signed(j, "download_behind_ms", h->behind_us);
			else
				cg_json_null(j, "download_behind_ms");
			cg_json_u64(j, "download_mutes", h->mutes);
			cg_json_u64(j, "download_state_ms", now_ms - h->changed_ms);
			cg_json_bool(j, "upload_muted", P->peer_muted);
			cg_json_u64(j, "tx_packets", P->tx_pkts);
			cg_json_u64(j, "tx_bytes", P->tx_bytes);
			cg_json_u64(j, "tx_drops", P->tx_drops);
			cg_json_u64(j, "rx_first", S->rx[p].wins);
			cg_json_u64(j, "rx_duplicate", S->rx[p].dups);
			cg_json_u64(j, "rx_late", S->rx[p].late);
			cg_json_u64(j, "rx_missed", S->rx[p].missed);
			cg_json_ms(j, "rx_lag_ms", cg_lag_us(&S->rx[p]));
			cg_json_obj(j, "client_view");
			cg_json_u64(j, "age_ms", P->peer_view_ms ? now_ms - P->peer_view_ms : 0);
			cg_json_u64(j, "rx", P->peer_view.rx);
			cg_json_u64(j, "first", P->peer_view.wins);
			cg_json_ms(j, "lag_ms", P->peer_view.lag_us);
			cg_json_end(j, '}');
			cg_json_end(j, '}');
		}
		cg_json_end(j, ']');
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_end(j, '}');
}

static void write_status(struct server *s, uint64_t now_ms)
{
	struct cg_json j;

	cg_json_init(&j);
	status_json(s, now_ms, &j);
	cg_status_writer_submit(&s->sw, &j);
	cg_json_free(&j);
}

/* "cengarde ctl links": the sessions and their links, as a table. */
#define PATH_ROW "%-9s %-6s %-5s %-30s %-8s %-8s %s\n"
static void links_text(struct server *s, uint64_t now_ms, struct cg_json *j)
{
	char id[16], link[8], ago[24], a[64];

	cg_json_raw(j, PATH_ROW, "SESSION", "NEWEST", "LINK", "ADDRESS", "STATE", "DOWNLOAD", "LAST RX");
	for (uint32_t i = 0; i < s->max; i++) {
		const struct session *S = &s->s[i];

		if (!S->used)
			continue;
		snprintf(id, sizeof(id), "%08x", S->id);
		for (int p = 0; p < CG_MAX_LINKS; p++) {
			const struct path *P = &S->path[p];
			uint64_t ms = now_ms - P->last_rx_ms;

			if (!P->used)
				continue;
			snprintf(link, sizeof(link), "%d", p);
			snprintf(ago, sizeof(ago), "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms / 100 % 10));
			cg_json_raw(j, PATH_ROW, id, s->newest == (int32_t)i ? "yes" : "", link,
				    cg_addr_str(&P->addr, a, sizeof(a)), path_live(P, now_ms) ? "live" : "stalled",
				    S->dh[p].state == CG_H_MUTED ? "muted" : "active", ago);
		}
	}
}

/* Hands the IP pass the newest session asks for to the writer; tried again
 * on the next tick when the writer is busy that instant. */
static void pass_sync(struct server *s)
{
	struct cg_json j;

	if (!s->pw.running || s->pass < 0 || s->pass == s->pass_written)
		return;
	cg_json_init(&j);
	cg_json_raw(&j, "%s\n", s->pass ? "on" : "off");
	if (cg_status_writer_submit(&s->pw, &j) == 0) {
		cg_info("IP pass %s, as the newest session asks: %s", s->pass ? "on" : "off", s->cfg->passthrough_file);
		s->pass_written = s->pass;
	}
	cg_json_free(&j);
}

static void tick(struct server *s)
{
	uint64_t now_ms = cg_now_ms();

	health_tick(s, now_ms);
	if (now_ms >= s->next_sweep_ms) {
		sweep(s, now_ms);
		s->next_sweep_ms = now_ms + 1000;
	}
	pass_sync(s);
	if (s->sw.running && now_ms >= s->next_status_ms) {
		write_status(s, now_ms);
		s->next_status_ms = now_ms + s->cfg->status_interval_ms;
	}
	cg_ctl_expire(&s->ctl, s->ep, now_ms);
}

/* ---- reload ---- */

static void reload_start(struct server *s)
{
	if (cg_loader_start(&s->loader, s->run->path) < 0)
		s->reload_again = 1; /* once the load under way is done */
}

static void writer_restart(struct cg_status_writer *w, const char *path)
{
	cg_status_writer_stop(w);
	if (path[0] && cg_status_writer_start(w, path) < 0)
		cg_warn("%s: cannot start the writer thread", path);
}

/* Puts next in place of the running configuration, keeping the sessions. */
static void apply_config(struct server *s, struct cg_config *next)
{
	struct cg_config *old = s->cfg;

	s->cfg = next;
	s->k_rx = next->key;
	s->k_tx = next->key + CG_SIPHASH_KEY_LEN;
	s->hcfg = cg_hcfg_of(next);
	cg_log_level = s->run->verbose ? CG_LOG_DEBUG : next->log_level;
	if (strcmp(old->status_file, next->status_file))
		writer_restart(&s->sw, next->status_file);
	if (strcmp(old->passthrough_file, next->passthrough_file)) {
		writer_restart(&s->pw, next->passthrough_file);
		s->pass_written = -1; /* write it again, there */
	}
	if (old->rcvbuf != next->rcvbuf) {
		cg_sock_buffers(s->lfd, next->rcvbuf, next->rcvbuf);
		for (uint32_t i = 0; i < s->max; i++)
			if (s->s[i].used)
				cg_sock_buffers(s->s[i].wg_fd, next->rcvbuf, next->rcvbuf);
	}
	cg_config_free(old);
	free(old);
}

/* A load finished. Returns -1 when the process has to stop (a restart that
 * could not happen). */
static int reload_done(struct server *s)
{
	struct cg_config *next;
	const char *why = NULL;
	char msg[700];

	if (!cg_loader_done(&s->loader, &next))
		return 0;
	if (!next) {
		cg_err("reload refused, still running the previous configuration: %s", s->loader.err);
		snprintf(s->config_error, sizeof(s->config_error), "%s", s->loader.err);
		snprintf(msg, sizeof(msg), "error: %s\n", s->loader.err);
	} else {
		cg_log_warnings(s->run->path, s->loader.warn);
		s->config_error[0] = '\0';
		why = cg_config_restart_needed(s->cfg, next);
		if (why) {
			snprintf(msg, sizeof(msg), "ok: %s changed, restarting\n", why);
		} else {
			apply_config(s, next);
			cg_info("reload: configuration applied");
			snprintf(msg, sizeof(msg), "ok\n");
		}
	}
	if (!why && s->reload_again) {
		/* Asked again while loading: whoever waits gets the newer outcome. */
		s->reload_again = 0;
		reload_start(s);
		return 0;
	}
	cg_ctl_reply_waiting(&s->ctl, s->ep, msg);
	if (!why)
		return 0;
	cg_info("reload: %s changed, restarting", why);
	cg_config_free(next);
	free(next);
	cg_status_writer_stop(&s->sw);
	cg_status_writer_stop(&s->pw);
	cg_ctl_close(&s->ctl, s->ep);
	cg_reexec(s->run->argv);
	cg_err("restart: %s", strerror(errno));
	return -1;
}

/* ---- control socket ---- */

static void ctl_command(struct server *s, int k, uint64_t now_ms)
{
	struct cg_ctl_cmd cmd;
	struct cg_json j;
	char err[256];

	cg_json_init(&j);
	if (cg_ctl_parse(s->ctl.c[k].in, &cmd, err, sizeof(err)) < 0) {
		cg_json_raw(&j, "error: %s\n", err);
	} else {
		switch (cmd.op) {
		case CG_CTL_STATUS:
			status_json(s, now_ms, &j);
			cg_json_raw(&j, "\n");
			break;
		case CG_CTL_LINKS:
			links_text(s, now_ms, &j);
			break;
		case CG_CTL_LINK:
		case CG_CTL_RESET:
			cg_json_raw(&j, "error: links are paused on the client, not on the server\n");
			break;
		case CG_CTL_RELOAD:
			cg_info("reloading %s", s->run->path);
			s->ctl.c[k].waiting = 1;
			s->ctl.c[k].deadline_ms = now_ms + CG_CTL_RELOAD_TIMEOUT_MS;
			reload_start(s);
			cg_json_free(&j);
			return; /* the reply goes out when the load is done */
		}
	}
	if (j.failed) {
		cg_json_free(&j);
		cg_ctl_reply(&s->ctl, s->ep, k, NULL, 0);
	} else {
		cg_ctl_reply(&s->ctl, s->ep, k, j.buf, j.len); /* takes the buffer */
	}
}

/* Reads the signals: 1 to stop, 0 to go on (SIGHUP starts a reload). */
static int signals(struct server *s)
{
	struct signalfd_siginfo si;
	int stop = 0;

	while (read(s->run->sigfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		if (si.ssi_signo != SIGHUP) {
			stop = 1;
		} else {
			cg_info("SIGHUP: reloading %s", s->run->path);
			reload_start(s);
		}
	}
	return stop;
}

int cg_server_run(struct cg_config *cfg, const struct cg_run *run)
{
	struct server *s = calloc(1, sizeof(*s));
	char err[256], buf[64], wg[64];
	uint64_t last_traffic_us = 0;
	uint32_t busy;
	int rc = 1, rcv;

	if (!s) {
		cg_err("out of memory");
		cg_config_free(cfg);
		free(cfg);
		return 1;
	}
	s->cfg = cfg;
	s->run = run;
	s->k_rx = cfg->key;
	s->k_tx = cfg->key + CG_SIPHASH_KEY_LEN;
	s->ep = s->lfd = s->tfd = -1;
	s->max = cfg->max_sessions;
	s->newest = -1;
	s->pass = s->pass_written = -1;
	s->hcfg = cg_hcfg_of(cfg);
	cg_ctl_init(&s->ctl);
	if (cg_loader_init(&s->loader) < 0) {
		cg_err("eventfd: %s", strerror(errno));
		goto out;
	}
	s->s = calloc(s->max, sizeof(*s->s));
	if (!s->s || cg_idmap_init(&s->ids, s->max) < 0) {
		cg_err("out of memory for %u sessions", s->max);
		goto out;
	}
	s->start_ms = cg_now_ms();

	s->lfd = cg_udp_bind(&cfg->listen, 0, err, sizeof(err));
	if (s->lfd < 0) {
		cg_err("%s", err);
		goto out;
	}
	rcv = cg_sock_buffers(s->lfd, cfg->rcvbuf, cfg->rcvbuf);
	if (rcv < (1 << 20))
		cg_warn("receive buffer is only %d bytes; raise net.core.rmem_max or run with CAP_NET_ADMIN", rcv);
	s->ep = epoll_create1(EPOLL_CLOEXEC);
	s->tfd = cg_timerfd(CG_TICK_MS);
	if (s->ep < 0 || s->tfd < 0 || cg_epoll_add(s->ep, s->lfd, CG_EV(CG_EV_LISTEN, 0)) < 0 ||
	    cg_epoll_add(s->ep, s->tfd, CG_EV(CG_EV_TIMER, 0)) < 0 ||
	    cg_epoll_add(s->ep, run->sigfd, CG_EV(CG_EV_SIG, 0)) < 0 ||
	    cg_epoll_add(s->ep, s->loader.efd, CG_EV(CG_EV_LOAD, 0)) < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	/* The control socket is a convenience: the tunnel runs without it. */
	if (cfg->control_socket[0] && cg_ctl_open(&s->ctl, cfg->control_socket, s->ep, err, sizeof(err)) < 0)
		cg_warn("control socket: %s", err);
	if (cfg->status_file[0] && cg_status_writer_start(&s->sw, cfg->status_file) < 0)
		cg_warn("status file %s: cannot start the writer thread", cfg->status_file);
	if (cfg->passthrough_file[0] && cg_status_writer_start(&s->pw, cfg->passthrough_file) < 0)
		cg_warn("%s: cannot start the writer thread", cfg->passthrough_file);
	cg_info("server %s: listening on %s, WireGuard at %s, up to %u sessions", CG_VERSION,
		cg_addr_str(&cfg->listen, buf, sizeof(buf)), cg_addr_str(&cfg->wireguard, wg, sizeof(wg)), s->max);
	busy = cg_tune(cfg);

	for (;;) {
		struct epoll_event ev[32];
		int n = cg_wait(s->ep, ev, 32, busy, last_traffic_us), traffic = 0;

		if (n < 0 && errno != EINTR) {
			cg_err("epoll_wait: %s", strerror(errno));
			goto out;
		}
		for (int i = 0; i < n; i++) {
			uint32_t kind = (uint32_t)(ev[i].data.u64 >> 32), idx = (uint32_t)ev[i].data.u64;

			switch (kind) {
			case CG_EV_LISTEN:
				listen_read(s);
				traffic = 1;
				break;
			case CG_EV_WG:
				if (idx < s->max && s->s[idx].used)
					wg_read(s, &s->s[idx]);
				traffic = 1;
				break;
			case CG_EV_TIMER: {
				uint64_t expirations;

				if (read(s->tfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
					cg_warn("timerfd: %s", strerror(errno));
				tick(s);
				break;
			}
			case CG_EV_CTL: {
				uint64_t now_ms = cg_now_ms();
				int k = cg_ctl_event(&s->ctl, s->ep, idx, now_ms);

				if (k >= 0)
					ctl_command(s, k, now_ms);
				break;
			}
			case CG_EV_LOAD:
				if (reload_done(s) < 0)
					goto out;
				break;
			case CG_EV_SIG:
				if (signals(s)) {
					cg_info("server stopping");
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
	cg_status_writer_stop(&s->sw);
	cg_status_writer_stop(&s->pw);
	cg_ctl_close(&s->ctl, s->ep);
	cg_loader_free(&s->loader);
	for (uint32_t i = 0; s->s && i < s->max; i++)
		if (s->s[i].used)
			close(s->s[i].wg_fd);
	if (s->lfd >= 0)
		close(s->lfd);
	if (s->tfd >= 0)
		close(s->tfd);
	if (s->ep >= 0)
		close(s->ep);
	free(s->s);
	cg_idmap_free(&s->ids);
	cg_config_free(s->cfg);
	free(s->cfg);
	free(s);
	return rc;
}
