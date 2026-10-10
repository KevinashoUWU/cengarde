/* cengarde server: listening sockets shared by every client path, one socket
 * towards WireGuard per session.
 *
 * Clients: one, with the global key, or several, a [client NAME] section
 * each, with its own key, WireGuard and sessions (at most
 * CG_CLIENT_SESSIONS). A packet's client hint picks the clients whose keys
 * it may be under (clients.h), so a server with many tries one key, two or
 * three at worst, never all of them. A client's slot, and with it its
 * sessions, stays while its name is configured: a reload that adds,
 * changes, turns off or removes one leaves the others alone.
 *
 * Lanes: the listen address is a SO_REUSEPORT group of `lanes` sockets plus
 * a junk socket, and a classic BPF program (steer.h) puts each datagram on
 * the lane of its link id, so the copies of one packet wait in different
 * receive queues and one queue's overflow rarely takes a whole packet. The
 * download of path p leaves from lane p & (lanes - 1), the socket its link
 * arrives on: each path has its own send buffer and its own probe reply
 * batch. With lanes = 1, or a kernel that cannot steer, one socket as
 * before. The receive buffers share a budget below net.ipv4.udp_mem
 * (rcvbudget.h), so that the extra queues cannot starve every other UDP
 * socket of the machine.
 *
 * A session is created, a path (a client uplink, keyed by session and link
 * id) is learned and moves to another address only by a probe whose MAC
 * verifies and that echoes a cookie this server handed to that address in a
 * HELLO (cookie.h): nobody can make the server send traffic to an address of
 * their choosing, not even with a probe captured earlier and sent again from
 * elsewhere or after a restart. A session the server takes up that way goes
 * on past the sequences the client's probe says it received, and its upload
 * windows start with everything the client sent before marked (proto.h).
 * DATA from an address the path does not know still goes to WireGuard (it
 * verified, and its sequence is new) but never moves the path. Each session
 * talks to WireGuard from its own socket, so WireGuard sees every client as
 * a distinct endpoint and many clients can share one port. Link health
 * (health.h) mutes the download on paths that lag far behind the fastest
 * one, from the delays the client reports in its probes.
 *
 * Several addresses: on a wildcard listen address the server learns, per
 * path and from verified packets only, which of its addresses the client
 * sends to, and everything it sends back on that path leaves from it
 * (pktinfo.h). Any address of the machine works, including one added while
 * the server runs; an address that goes away is counted on the paths that
 * used it (local_errors) and never cuts the others.
 *
 * IP pass: the client's probes ask for it on or off (CG_F_PASS_SET); the
 * newest session decides, and the server writes its wish to passthrough_file
 * for the system to apply (contrib/vps), from a thread so the loop never
 * waits on the disk. A reload applies in place, as on the client. With
 * several clients, the first that asks holds it until it lets go, and the
 * server writes that and every client's forward rules to forward_file
 * (fwdtable.h), which contrib/vps applies the same way.
 *
 * A client whose clock went back (a router without a battery-backed clock
 * that restarted) is ignored by WireGuard, which takes only handshakes newer
 * than the last one. The server watches the handshakes it hands on and pokes
 * WireGuard into starting one of its own (wgwatch.h); WireGuard sends it to
 * the session it knows, so after a restart it goes down the newest one, and
 * the port WireGuard knows passes on to the newest: when its older session
 * closes, or when it starts alone.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "arrival.h"
#include "cookie.h"
#include "ctl.h"
#include "engine.h"
#include "health.h"
#include "idmap.h"
#include "log.h"
#include "pair.h"
#include "pktinfo.h"
#include "rcvbudget.h"
#include "replay.h"
#include "sock.h"
#include "status.h"
#include "steer.h"
#include "thrplan.h" /* cg_cpuwin */
#include "util.h"
#include "wgwatch.h"

/* Probe interval assumed for a path until its client announces one. */
#define DEFAULT_INTERVAL_MS 1000
/* The junk socket's receive buffer: what a flood of garbage can fill. */
#define JUNK_RCVBUF (256 * 1024)
/* HELLOs per second on average, and at once: a client starting with 16
 * links needs 16, each again when its cookie moves (cookie.h). */
#define HELLO_RATE 50
#define HELLO_BURST 100

struct path {
	int used;
	struct sockaddr_storage addr;
	struct cg_local local; /* the address of ours its packets arrive at */
	union cg_ctl_tx ctl;   /* the control message that sends from it (loop thread only) */
	size_t ctl_len;        /* 0: none, the route picks the source */
	uint64_t since_ms, last_rx_ms;
	uint64_t last_owd_ms; /* last probe saying the client hears our replies */
	uint32_t interval_ms; /* the client's probe interval on this path */
	int peer_muted;       /* the client carries no upload on this path */
	uint64_t tx_pkts, tx_bytes, tx_drops;
	uint64_t moves;        /* changes of address, at either end */
	uint64_t local_errors; /* datagrams that could not leave: our address gone, no route */
	struct cg_probe_info peer_view; /* client's view of this path (download) */
	uint64_t peer_view_ms;
};

struct session {
	int used;
	uint32_t id;
	int client; /* its client's slot in server.c */
	int wg_fd;
	uint16_t wg_port; /* wg_fd's own port (network order): WireGuard knows the client by it */
	struct cg_wgw wgw; /* the client's handshakes WireGuard has not answered */
	uint32_t tx_seq; /* next DATA sequence down */
	uint32_t tx_ctl; /* next probe reply sequence */
	int pass; /* IP pass its probes ask for: -1 nothing */
	uint64_t created_ms, last_rx_ms;
	struct path path[CG_MAX_LINKS];
	struct cg_hlink dh[CG_MAX_LINKS]; /* download health, by link id */
	struct cg_replay replay; /* DATA up */
	struct cg_replay ctl;    /* probes */
	struct cg_arrivals arr;
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t up_pkts, up_bytes, down_pkts, down_bytes, wg_drops, toobig;
	uint32_t rot; /* download batches sent: the first path of each rotates */
};

/* A client: a [client NAME] section, or the global key of a server with one
 * client (name ""). It keeps its slot while its name stays configured, so
 * its sessions follow it across reloads. */
struct client {
	int used;
	char name[CG_CLIENT_NAME];
	char tag[CG_CLIENT_NAME + 2]; /* "NAME: " in front of its log lines; "" with one client */
	char label[64];
	uint8_t key[CG_KEY_LEN]; /* client -> server, then server -> client */
	uint8_t hint;            /* pair.h: picks it before any MAC (clients.h) */
	int enabled, passthrough;
	struct sockaddr_storage wireguard, poke; /* poke AF_UNSPEC: none */
	uint32_t mix;     /* its sessions' keys in the map (cg_sesskey) */
	uint32_t nsess;   /* its sessions */
	int32_t newest;   /* index of its newest session, -1: none */
	uint16_t wg_port; /* the newest session's wg_port, kept after it closes */
	int pass;         /* IP pass its newest session asks for: -1 nothing yet */
	uint64_t pass_on_ms; /* when that turned on */
	int pass_seen;       /* the wish last logged */
	struct cg_bucket hello_budget; /* each its own: one client's HELLOs never starve another's */
	uint64_t refused;              /* probes refused: disabled, or no room */
	struct cg_ratelimit rl_poke, rl_refused;
};

/* A listen socket of the group (steer.h). */
struct lane {
	int fd;
	uint64_t rx;    /* datagrams read */
	uint16_t links; /* link ids of the verified packets read from it */
};

/* Forward tables are small: cengarde-nat refuses one over 64 KiB. */
#define FWD_TABLE_MAX 65536

struct server {
	struct cg_config *cfg; /* replaced by a reload */
	const struct cg_run *run;
	struct client c[CG_MAX_CLIENTS];
	struct cg_hintidx hx; /* hint -> clients (clients.h) */
	int multi;            /* [client] sections: their names in the logs, the forward table */
	struct cg_cookie_keys ck;
	int ep, tfd;
	struct lane lane[CG_MAX_LANES];
	uint32_t nlanes;        /* listen sockets: 1 without a group */
	int jfd;                /* the group's junk socket, -1 without a group */
	char steer_error[160];  /* why there is no group although lanes > 1 ("": none) */
	int lfamily;     /* the listen sockets' family */
	int pktinfo;     /* they report arrival addresses: replies leave from them */
	uint16_t lport;  /* their port, network order */
	char laddr[64];  /* their address as bound, for the log and the status */
	struct cg_rcvbudget rb; /* receive buffers (rcvbudget.h) */
	int udp_mem_from;       /* its udp_mem: 0 the sysctl, 1 estimated from RAM, -1 unknown */
	int rcv_eff;            /* SO_RCVBUF the kernel reports for lane 0 */
	struct session *s;
	uint32_t cap;   /* sessions s has room for: grows on a reload */
	uint32_t max;   /* max_sessions */
	uint32_t nsess; /* in use */
	struct cg_idmap ids; /* cg_sesskey(client's mix, session id) -> index into s */
	struct cg_hcfg hcfg;
	struct cg_status_writer sw;
	struct cg_status_writer pw; /* passthrough_file */
	struct cg_status_writer fw; /* forward_file */
	struct cg_cpuwin cpu[4];    /* this loop, sw, pw and fw, for "ctl threads" */
	uint64_t next_cpu_ms;
	int pass_written; /* one client: last IP pass handed to pw, -1 none */
	/* Several clients: the forward table (fwdtable.h). */
	int holder;      /* slot holding IP pass, -1: none */
	int fw_holder;   /* the holder in the table last handed on (or found on disk), -1: none */
	int fw_written;  /* a table was handed on, or the one on disk is current */
	size_t fw_len;   /* length of fw_text */
	char fw_text[FWD_TABLE_MAX]; /* that table */
	char fw_next[FWD_TABLE_MAX];
	struct cg_ctl ctl;
	struct cg_loader loader;
	int reload_again;
	char config_error[600];
	uint64_t start_ms, next_status_ms, next_sweep_ms;

	uint64_t rx_malformed, rx_auth_fail, rx_old, rx_dups, rx_trunc, rx_ctrunc, sessions_full;
	uint64_t rx_hint, rx_no_session; /* no client's hint; DATA for no session */
	uint64_t hellos, hellos_refused, hellos_over; /* sent, of those refusing, not sent (budget) */
	uint64_t rx_junk, rx_short, rx_bad_version; /* read from the junk socket */
	uint64_t wg_pokes, wg_redirects, id_clashes;
	struct cg_ratelimit rl_auth, rl_full, rl_send, rl_local, rl_junk, rl_version, rl_redirect, rl_hint, rl_fw;

	struct cg_rxbatch in;
	union cg_ctl_rx rxctl[CG_BATCH]; /* arrival addresses of a listen batch */
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
	/* per listen batch: payloads for WireGuard and probe replies */
	uint32_t q_sess[CG_BATCH];
	uint8_t reply[CG_BATCH][CG_HDR_LEN + CG_PROBE_INFO_LEN];
	struct sockaddr_storage reply_to[CG_BATCH];
	union cg_ctl_tx reply_ctl[CG_BATCH];
	struct session *reply_sess[CG_BATCH]; /* whose path each reply is for */
	uint8_t reply_link[CG_BATCH];
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

/* An address of ours as "a.b.c.d:port" or "[v6]:port", "" when unknown. */
static const char *local_str(const struct server *s, const struct cg_local *l, char *buf, size_t len)
{
	struct sockaddr_storage a;

	if (!l->known) {
		buf[0] = '\0';
		return buf;
	}
	cg_local_sockaddr(l, s->lport, &a);
	return cg_addr_str(&a, buf, len);
}

/* ---- session table ---- */

static struct client *client_of(struct server *s, const struct session *S)
{
	return &s->c[S->client];
}

/* The session of a packet with header h: one of a client with h's hint (a
 * few at worst, clients.h) whose session it is. NULL when there is none. */
static struct session *lookup(struct server *s, const struct cg_hdr *h)
{
	for (int c = s->hx.first[h->hint]; c >= 0; c = s->hx.next[c]) {
		int32_t i = cg_idmap_get(&s->ids, cg_sesskey(s->c[c].mix, h->session));

		if (i >= 0 && s->s[i].client == c && s->s[i].id == h->session)
			return &s->s[i];
	}
	return NULL;
}

/* The port of a bound socket (network order), 0 when unknown. */
static uint16_t local_port(int fd)
{
	struct sockaddr_storage a;
	socklen_t len = sizeof(a);

	if (getsockname(fd, (struct sockaddr *)&a, &len) < 0)
		return 0;
	if (a.ss_family == AF_INET)
		return ((struct sockaddr_in *)&a)->sin_port;
	return a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : 0;
}

/* A socket connected to C's WireGuard with the session buffers, bound to
 * port when it is not 0 and free (else to any other); -1 with errno set. */
static int wg_socket(struct server *s, const struct client *C, uint16_t port)
{
	int fd = cg_udp_socket(C->wireguard.ss_family), e;

	if (fd < 0)
		return -1;
	if (port) {
		struct sockaddr_storage b;

		memset(&b, 0, sizeof(b));
		b.ss_family = C->wireguard.ss_family;
		if (b.ss_family == AF_INET)
			((struct sockaddr_in *)&b)->sin_port = port;
		else
			((struct sockaddr_in6 *)&b)->sin6_port = port;
		(void)!bind(fd, (const struct sockaddr *)&b, cg_addr_len(&b));
	}
	if (connect(fd, (const struct sockaddr *)&C->wireguard, cg_addr_len(&C->wireguard)) < 0) {
		e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	cg_sock_buffers(fd, s->rb.per_socket, s->cfg->rcvbuf);
	return fd;
}

/* N, knocking in vain, takes over port, which an older session of its
 * client just freed: WireGuard may still know the client by it, and send
 * there the handshake it starts (wgwatch.h). When the port is no longer
 * free, N keeps its socket. */
static void session_rehome(struct server *s, struct session *N, uint16_t port)
{
	struct client *C = client_of(s, N);
	int fd = wg_socket(s, C, port);

	if (fd < 0)
		return;
	if (local_port(fd) != port || cg_epoll_add(s->ep, fd, CG_EV(CG_EV_WG, N - s->s)) < 0) {
		close(fd);
		return;
	}
	epoll_ctl(s->ep, EPOLL_CTL_DEL, N->wg_fd, NULL);
	close(N->wg_fd);
	N->wg_fd = fd;
	N->wg_port = C->wg_port = port;
	cg_info("%ssession %08x: takes over the port WireGuard knew its client by", C->tag, N->id);
}

static void session_destroy(struct server *s, struct session *S, const char *why, uint64_t now_ms)
{
	struct client *C = client_of(s, S);
	int32_t idx = (int32_t)(S - s->s);
	struct session *N;

	cg_info("%ssession %08x closed: %s", C->tag, S->id, why);
	epoll_ctl(s->ep, EPOLL_CTL_DEL, S->wg_fd, NULL);
	close(S->wg_fd);
	cg_idmap_del(&s->ids, cg_sesskey(C->mix, S->id));
	S->used = 0;
	C->nsess--;
	s->nsess--;
	/* A session replaced in the middle of a listen batch (session_create)
	 * leaves nothing queued behind: its slot may take the new session. */
	for (int j = 0; j < CG_BATCH; j++) {
		if (s->q_sess[j] == (uint32_t)idx)
			s->q_sess[j] = UINT32_MAX;
		if (s->reply_sess[j] == S)
			s->reply_sess[j] = NULL;
	}
	if (C->newest != idx) {
		N = C->newest >= 0 ? &s->s[C->newest] : NULL;
		if (N && N->created_ms > S->created_ms && cg_wgw_knocking(&N->wgw, now_ms))
			session_rehome(s, N, S->wg_port);
		return;
	}
	/* IP pass stays as it was until the next newest session asks. */
	C->newest = -1;
	for (uint32_t i = 0; i < s->cap; i++)
		if (s->s[i].used && s->s[i].client == S->client &&
		    (C->newest < 0 || s->s[i].created_ms > s->s[C->newest].created_ms))
			C->newest = (int32_t)i;
	if (C->newest >= 0)
		C->wg_port = s->s[C->newest].wg_port;
}

/* Closes every session of client C. */
static void client_close(struct server *s, struct client *C, const char *why, uint64_t now_ms)
{
	for (uint32_t i = 0; i < s->cap && C->nsess; i++)
		if (s->s[i].used && &s->c[s->s[i].client] == C)
			session_destroy(s, &s->s[i], why, now_ms);
}

/* A session for C's probe h with info pi from `from`, its cookie checked:
 * it goes on past what the client received and its upload windows start
 * with what the client sent marked (proto.h). A client holds at most
 * CG_CLIENT_SESSIONS: a new one replaces the one heard from least recently
 * (a router restarting in a loop leaves one behind each time). */
static struct session *session_create(struct server *s, struct client *C, const struct cg_hdr *h,
				      const struct cg_probe_info *pi, const struct sockaddr_storage *from,
				      uint64_t now_ms)
{
	uint32_t id = h->session, key = cg_sesskey(C->mix, id);
	struct session *S = NULL, *old = NULL;
	char a[64];
	int fd, alone;

	if (C->nsess >= CG_CLIENT_SESSIONS) {
		for (uint32_t i = 0; i < s->cap; i++)
			if (s->s[i].used && &s->c[s->s[i].client] == C && (!old || s->s[i].last_rx_ms < old->last_rx_ms))
				old = &s->s[i];
		if (old)
			session_destroy(s, old, "replaced by a newer one of the same client", now_ms);
	}
	for (uint32_t i = 0; i < s->cap && !S && s->nsess < s->max; i++)
		if (!s->s[i].used)
			S = &s->s[i];
	if (!S) {
		s->sessions_full++;
		C->refused++;
		if (cg_ratelimit_ok(&s->rl_full, now_ms, 10000))
			cg_warn("%ssession limit (%u) reached, refusing %08x", C->tag, s->max, id);
		return NULL;
	}
	/* Another client's session already has this key: a 2^-32 chance. */
	if (cg_idmap_get(&s->ids, key) >= 0) {
		s->id_clashes++;
		cg_warn("%ssession %08x: its key clashes with another client's session, refused", C->tag, id);
		return NULL;
	}
	/* WireGuard knows the client by the port of its session's socket. One
	 * that starts while no other of its client is left takes the newest
	 * one's port again: what WireGuard sends still arrives, its own
	 * handshakes included, which a router that came back with its clock
	 * behind needs (wgwatch.h). */
	alone = !C->nsess;
	fd = wg_socket(s, C, alone ? C->wg_port : 0);
	if (fd < 0) {
		cg_err("%ssession %08x: socket to WireGuard: %s", C->tag, id, strerror(errno));
		return NULL;
	}
	memset(S, 0, sizeof(*S));
	S->used = 1;
	S->id = id;
	S->client = (int)(C - s->c);
	S->wg_fd = fd;
	S->wg_port = local_port(fd);
	S->tx_seq = pi->rx_top + CG_SEQ_LEAP;
	S->tx_ctl = pi->rx_top_ctl + CG_SEQ_LEAP;
	S->created_ms = S->last_rx_ms = now_ms;
	S->pass = -1;
	cg_replay_init_marked(&S->replay, pi->tx_next - 1);
	cg_replay_init_marked(&S->ctl, h->seq);
	if (cg_epoll_add(s->ep, fd, CG_EV(CG_EV_WG, S - s->s)) < 0) {
		close(fd);
		S->used = 0;
		return NULL;
	}
	cg_idmap_put(&s->ids, key, (int32_t)(S - s->s));
	C->nsess++;
	s->nsess++;
	C->newest = (int32_t)(S - s->s);
	C->wg_port = S->wg_port;
	cg_info("%ssession %08x: new client from %s", C->tag, id, cg_addr_str(from, a, sizeof(a)));
	return S;
}

/* ---- paths ---- */

/* A verified packet from `from` on link of S, already marked in the replay
 * window: the path learns where the client is and, from the control
 * messages in m, which address of ours it sends to. Only a probe whose
 * cookie this address got moves the client's end (path_may_move); DATA only
 * tells which address of ours it went to. A forged or replayed packet never
 * gets here, so nobody else can move a path. Only the loop thread touches a
 * path: the control message is rewritten in place while
 * the old ctl_len is still set, and wg_read hands P->ctl.b to sendmmsg as
 * is. A multithreaded server (design decision 28) has to build it in a
 * second buffer and publish that buffer and its length with one release
 * store, read with acquire; the order of the stores alone is not enough. */
static struct path *path_update(struct server *s, struct session *S, unsigned link,
				const struct sockaddr_storage *from, const struct msghdr *m, uint64_t now_ms)
{
	const char *tag = client_of(s, S)->tag;
	struct path *P = &S->path[link];
	struct sockaddr_storage old;
	struct cg_local got = { .known = 0 }, old_local;
	unsigned ch;
	char a[64], o[64], l[64], ol[64];

	if (s->pktinfo && !cg_local_from_msg(m, &got) && (m->msg_flags & MSG_CTRUNC))
		s->rx_ctrunc++;
	if (!P->used) {
		memset(P, 0, sizeof(*P));
		P->used = 1;
		P->since_ms = now_ms;
		P->interval_ms = DEFAULT_INTERVAL_MS;
		cg_health_reset(&S->dh[link], now_ms);
		cg_path_learn(&P->addr, &P->local, from, &got);
		P->ctl_len = cg_local_cmsg(&P->local, s->lfamily, &P->ctl);
		cg_info("%ssession %08x: link %u via %s%s%s", tag, S->id, link, cg_addr_str(from, a, sizeof(a)),
			P->local.known ? " to " : "", local_str(s, &P->local, l, sizeof(l)));
		return P;
	}
	ch = cg_path_diff(&P->addr, &P->local, from, &got);
	if (!ch)
		return P;
	old = P->addr;
	old_local = P->local;
	cg_path_learn(&P->addr, &P->local, from, &got);
	if (ch & CG_PATH_NEW_LOCAL)
		P->ctl_len = cg_local_cmsg(&P->local, s->lfamily, &P->ctl);
	/* Its delays over the other family say nothing about this one. */
	if (ch & CG_PATH_NEW_FAMILY)
		cg_health_reset(&S->dh[link], now_ms);
	P->moves++;
	if (s->pktinfo)
		cg_info("%ssession %08x: link %u moved %s (to %s) -> %s (to %s)", tag, S->id, link,
			cg_addr_str(&old, o, sizeof(o)), old_local.known ? local_str(s, &old_local, ol, sizeof(ol)) : "?",
			cg_addr_str(from, a, sizeof(a)), P->local.known ? local_str(s, &P->local, l, sizeof(l)) : "?");
	else
		cg_info("%ssession %08x: link %u moved %s -> %s", tag, S->id, link, cg_addr_str(&old, o, sizeof(o)),
			cg_addr_str(from, a, sizeof(a)));
	return P;
}

/* n datagrams for link of S could not leave, with errno err. When our
 * address is gone or there is no route, the path counts it and stays: the
 * address may come back, and the client moves to another one on its own. */
static void send_failed(struct server *s, struct session *S, unsigned link, int err, unsigned n, uint64_t now_ms)
{
	struct path *P;
	char a[64], l[64];

	if (!S) /* a HELLO: no path to count it on */
		return;
	P = &S->path[link];

	switch (cg_send_err_kind(err)) {
	case CG_SEND_LOCAL:
		P->local_errors += n;
		if (cg_ratelimit_ok(&s->rl_local, now_ms, 10000))
			cg_warn("%ssession %08x link %u: cannot send to %s from %s: %s (address removed or no route)",
				client_of(s, S)->tag, S->id, link, cg_addr_str(&P->addr, a, sizeof(a)),
				P->local.known ? local_str(s, &P->local, l, sizeof(l)) : "the route's address",
				strerror(err));
		break;
	case CG_SEND_OTHER:
		if (cg_ratelimit_ok(&s->rl_send, now_ms, 10000))
			cg_warn("%ssession %08x link %u send: %s", client_of(s, S)->tag, S->id, link, strerror(err));
		break;
	case CG_SEND_STOP:
		break;
	}
}

/* ---- client -> WireGuard ---- */

/* The IP pass the server has handed on for C to apply, as reply flags: with
 * one client, what went to passthrough_file; with several, whether C holds
 * it in the forward table. Nothing (-1) until something was handed on. */
static int pass_told(const struct server *s, const struct client *C)
{
	if (!s->multi)
		return s->pw.running ? s->pass_written : -1;
	if (!s->fw.running || !s->fw_written)
		return -1;
	return s->fw_holder == (int)(C - s->c);
}

/* up_owd: the probe's trip up this path, timed here, for the client's link
 * health. */
static void queue_reply(struct server *s, int *nr, struct session *S, unsigned link, const struct cg_hdr *probe,
			uint32_t up_owd, uint64_t now_us)
{
	const struct client *C = client_of(s, S);
	const struct path *P = &S->path[link];
	uint8_t *r = s->reply[*nr];
	struct cg_probe_info pi = { .echo_ts = probe->ts,
				    .owd = up_owd,
				    .rx = (uint32_t)(S->rx[link].wins + S->rx[link].dups),
				    .wins = (uint32_t)S->rx[link].wins,
				    .lag_us = cg_lag_us(&S->rx[link]),
				    .rx_top = S->replay.top,
				    .rx_top_ctl = S->ctl.top,
				    .tx_next = S->tx_seq };
	struct cg_hdr h = { .type = CG_T_PROBE_REPLY,
			    .flags = (uint8_t)(CG_F_OWD | (S->dh[link].state == CG_H_MUTED ? CG_F_MUTED : 0) |
				       cg_pass_flags(pass_told(s, C))),
			    .hint = C->hint,
			    .link = (uint8_t)link,
			    .session = S->id,
			    .seq = S->tx_ctl++,
			    .ts = (uint32_t)now_us };

	cg_probe_info_write(r + CG_HDR_LEN, &pi);
	cg_hdr_write(r, &h, C->key + CG_SIPHASH_KEY_LEN, r + CG_HDR_LEN, CG_PROBE_INFO_LEN);
	s->reply_to[*nr] = P->addr;
	s->riov[*nr].iov_base = r;
	s->riov[*nr].iov_len = CG_HDR_LEN + CG_PROBE_INFO_LEN;
	memset(&s->rmsg[*nr].msg_hdr, 0, sizeof(s->rmsg[*nr].msg_hdr));
	s->rmsg[*nr].msg_hdr.msg_name = &s->reply_to[*nr];
	s->rmsg[*nr].msg_hdr.msg_namelen = cg_addr_len(&s->reply_to[*nr]);
	s->rmsg[*nr].msg_hdr.msg_iov = &s->riov[*nr];
	s->rmsg[*nr].msg_hdr.msg_iovlen = 1;
	/* A copy, like the address: a later packet of the batch may move the
	 * path before the replies go out. */
	if (P->ctl_len) {
		memcpy(s->reply_ctl[*nr].b, P->ctl.b, P->ctl_len);
		s->rmsg[*nr].msg_hdr.msg_control = s->reply_ctl[*nr].b;
		s->rmsg[*nr].msg_hdr.msg_controllen = P->ctl_len;
	}
	s->reply_sess[*nr] = S;
	s->reply_link[*nr] = (uint8_t)link;
	(*nr)++;
}

/* A HELLO to C's probe h from `from` (its datagram m), which this server
 * cannot take yet: a cookie for this address, or a refusal. Never larger
 * than the probe, only to a probe whose MAC verified, and within C's
 * HELLO_RATE. */
static void queue_hello(struct server *s, int *nr, struct client *C, const struct cg_hdr *probe,
			const struct sockaddr_storage *from, const struct msghdr *m, int refused, uint64_t now_us)
{
	uint64_t now_ms = now_us / 1000;
	uint8_t *r = s->reply[*nr], fresh[CG_SIPHASH_KEY_LEN];
	struct cg_local got = { .known = 0 };
	struct cg_hello hl = { .echo_ts = probe->ts };
	struct cg_hdr h = { .type = CG_T_HELLO,
			    .flags = refused ? CG_F_REFUSED : 0,
			    .hint = C->hint,
			    .link = probe->link,
			    .session = probe->session,
			    .seq = 0,
			    .ts = (uint32_t)now_us };
	size_t ctl_len;

	if (!cg_bucket_take(&C->hello_budget, now_ms, HELLO_RATE, HELLO_BURST)) {
		s->hellos_over++;
		return;
	}
	if (cg_cookie_due(&s->ck, now_ms)) {
		if (cg_random(fresh, sizeof(fresh)) < 0)
			return;
		cg_cookie_rotate(&s->ck, now_ms, fresh);
	}
	hl.cookie = cg_cookie_make(&s->ck, (uint32_t)(C - s->c), probe->session, probe->link, from);
	cg_hello_write(r + CG_HDR_LEN, &hl);
	cg_hdr_write(r, &h, C->key + CG_SIPHASH_KEY_LEN, r + CG_HDR_LEN, CG_HELLO_LEN);
	s->reply_to[*nr] = *from;
	s->riov[*nr].iov_base = r;
	s->riov[*nr].iov_len = CG_HDR_LEN + CG_HELLO_LEN;
	memset(&s->rmsg[*nr].msg_hdr, 0, sizeof(s->rmsg[*nr].msg_hdr));
	s->rmsg[*nr].msg_hdr.msg_name = &s->reply_to[*nr];
	s->rmsg[*nr].msg_hdr.msg_namelen = cg_addr_len(&s->reply_to[*nr]);
	s->rmsg[*nr].msg_hdr.msg_iov = &s->riov[*nr];
	s->rmsg[*nr].msg_hdr.msg_iovlen = 1;
	/* From the address it arrived at, like everything else. */
	if (s->pktinfo)
		cg_local_from_msg(m, &got);
	ctl_len = cg_local_cmsg(&got, s->lfamily, &s->reply_ctl[*nr]);
	if (ctl_len) {
		s->rmsg[*nr].msg_hdr.msg_control = s->reply_ctl[*nr].b;
		s->rmsg[*nr].msg_hdr.msg_controllen = ctl_len;
	}
	s->reply_sess[*nr] = NULL;
	s->reply_link[*nr] = probe->link;
	s->hellos++;
	s->hellos_refused += !!refused;
	(*nr)++;
}

/* Whether a probe of C from `from` with cookie may move the client's end of
 * path P (or learn it): the same address needs nothing, another needs the
 * cookie of a HELLO sent there. */
static int path_may_move(struct server *s, const struct client *C, const struct path *P, uint32_t session,
			 unsigned link, const struct sockaddr_storage *from, uint32_t cookie, uint64_t now_ms)
{
	struct cg_local none = { .known = 0 };
	uint8_t fresh[CG_SIPHASH_KEY_LEN];

	if (P && P->used && !(cg_path_diff(&P->addr, &P->local, from, &none) & CG_PATH_NEW_ADDR))
		return 1;
	if (cg_cookie_due(&s->ck, now_ms)) {
		if (cg_random(fresh, sizeof(fresh)) < 0)
			return 0;
		cg_cookie_rotate(&s->ck, now_ms, fresh);
	}
	return cg_cookie_ok(&s->ck, cookie, (uint32_t)(C - s->c), session, (uint8_t)link, from);
}

/* Sends the probe replies of a batch read from one lane, on that lane, the
 * socket their paths' downloads leave from. sendmmsg stops at the first
 * datagram that fails, so one path whose address of ours went away would
 * take the replies of every later path with it: the batch goes on past the
 * one that failed. A full socket stops it, the rest would fail too; with
 * lanes, that is only the paths of this lane. */
static void flush_replies(struct server *s, int fd, int nr, uint64_t now_ms)
{
	for (int i = 0; i < nr;) {
		int sent = sendmmsg(fd, s->rmsg + i, (unsigned)(nr - i), MSG_DONTWAIT);

		if (sent > 0) {
			i += sent;
			continue;
		}
		if (cg_send_err_kind(errno) == CG_SEND_STOP)
			return;
		send_failed(s, s->reply_sess[i], s->reply_link[i], errno, 1, now_ms);
		i++;
	}
}

static void on_probe(struct server *s, struct session *S, struct path *P, unsigned link, const struct cg_hdr *h,
		     const struct cg_probe_info *pi, uint64_t now_ms)
{
	struct client *C = client_of(s, S);

	S->pass = cg_pass_get(h->flags);
	if (S->pass >= 0 && C->newest == (int32_t)(S - s->s)) {
		if (S->pass && C->pass != 1)
			C->pass_on_ms = now_ms;
		C->pass = S->pass;
	}
	P->peer_view = *pi;
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

static void listen_read(struct server *s, struct lane *ln)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx_ctl(ln->fd, &s->in, s->rxctl), nq = 0, nr = 0;
		uint64_t now_us, now_ms;
		uint32_t now32;

		if (n <= 0)
			return;
		ln->rx += (uint64_t)n;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		now32 = (uint32_t)now_us;
		for (int i = 0; i < n; i++) {
			uint8_t *b = s->in.buf[i];
			size_t len = s->in.msg[i].msg_len;
			const struct sockaddr_storage *from = &s->in.from[i];
			struct cg_probe_info pi;
			struct cg_replay *w;
			struct session *S;
			struct client *C;
			struct path *P;
			struct cg_hdr h;
			int verified = 0, fresh = 0, c;
			char a[64];

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
			/* No client's hint: not worth a MAC. */
			if (s->hx.first[h.hint] < 0) {
				s->rx_hint++;
				if (cg_ratelimit_ok(&s->rl_hint, now_ms, 10000))
					cg_warn("packet from %s for another server (client hint %02x is no client's: "
						"wrong key?)", cg_addr_str(from, a, sizeof(a)), h.hint);
				continue;
			}
			if (h.type == CG_T_PROBE)
				cg_probe_info_read(&pi, b + CG_HDR_LEN);
			S = lookup(s, &h);
			if (!S) {
				/* Only a probe takes a session up, and only with its
				 * cookie; the DATA waits for it. Which client it is:
				 * the one of its hint whose key verifies it. */
				if (h.type != CG_T_PROBE) {
					s->rx_no_session++;
					continue;
				}
				for (c = s->hx.first[h.hint]; c >= 0; c = s->hx.next[c])
					if (cg_hdr_verify(b, len, s->c[c].key))
						break;
				if (c < 0) {
					s->rx_auth_fail++;
					if (cg_ratelimit_ok(&s->rl_auth, now_ms, 10000))
						cg_warn("unauthenticated packet from %s (wrong key or not cengarde)",
							cg_addr_str(from, a, sizeof(a)));
					continue;
				}
				C = &s->c[c];
				if (!C->enabled) {
					C->refused++;
					if (cg_ratelimit_ok(&C->rl_refused, now_ms, 60000))
						cg_info("%sdisabled: refusing session %08x from %s", C->tag, h.session,
							cg_addr_str(from, a, sizeof(a)));
					queue_hello(s, &nr, C, &h, from, &s->in.msg[i].msg_hdr, 1, now_us);
					continue;
				}
				if (!path_may_move(s, C, NULL, h.session, h.link, from, pi.cookie, now_ms)) {
					queue_hello(s, &nr, C, &h, from, &s->in.msg[i].msg_hdr, 0, now_us);
					continue;
				}
				S = session_create(s, C, &h, &pi, from, now_ms);
				if (!S) {
					queue_hello(s, &nr, C, &h, from, &s->in.msg[i].msg_hdr, 1, now_us);
					continue;
				}
				verified = fresh = 1;
			}
			C = client_of(s, S);
			w = h.type == CG_T_DATA ? &S->replay : &S->ctl;
			switch (fresh ? CG_RP_NEW : cg_replay_check(w, h.seq)) {
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
			if (!verified && !cg_hdr_verify(b, len, C->key)) {
				s->rx_auth_fail++;
				if (cg_ratelimit_ok(&s->rl_auth, now_ms, 10000))
					cg_warn("%ssession %08x: packet from %s failed authentication", C->tag, S->id,
						cg_addr_str(from, a, sizeof(a)));
				continue;
			}
			if (!fresh)
				cg_replay_mark(w, h.seq);
			ln->links |= (uint16_t)(1u << h.link);
			S->last_rx_ms = now_ms;
			P = &S->path[h.link];
			if (h.type == CG_T_PROBE) {
				/* A new link, or an address the path never used:
				 * only with the cookie of a HELLO sent there. */
				if (!path_may_move(s, C, P, S->id, h.link, from, pi.cookie, now_ms)) {
					queue_hello(s, &nr, C, &h, from, &s->in.msg[i].msg_hdr, 0, now_us);
					continue;
				}
				P = path_update(s, S, h.link, from, &s->in.msg[i].msg_hdr, now_ms);
				P->last_rx_ms = now_ms;
				on_probe(s, S, P, h.link, &h, &pi, now_ms);
				queue_reply(s, &nr, S, h.link, &h, now32 - h.ts, now_us);
				continue;
			}
			/* DATA: to WireGuard wherever it came from; it tells the path
			 * only which address of ours it went to. */
			if (P->used && path_may_move(s, C, P, S->id, h.link, from, 0, now_ms)) {
				P = path_update(s, S, h.link, from, &s->in.msg[i].msg_hdr, now_ms);
				P->last_rx_ms = now_ms;
			}
			cg_arr_first(&S->arr, S->rx, h.seq, now32, h.link, expect_mask(S, now_ms));
			cg_wgw_from_client(&S->wgw, cg_wg_handshake(b + CG_HDR_LEN, len - CG_HDR_LEN), now_ms);
			S->up_pkts++;
			S->up_bytes += len - CG_HDR_LEN;
			s->q_sess[nq] = (uint32_t)(S - s->s);
			s->oiov[nq][0].iov_base = b + CG_HDR_LEN;
			s->oiov[nq][0].iov_len = len - CG_HDR_LEN;
			nq++;
		}
		if (nq)
			flush_wg(s, nq);
		if (nr)
			flush_replies(s, ln->fd, nr, now_ms);
		if (n < CG_BATCH)
			return;
	}
}

/* The junk socket: what the steering program found not to be protocol 4
 * (steer.h). Counted and now and then logged, never parsed further; one
 * batch per loop pass, so a flood fills only its small buffer and never
 * starves the lanes. */
static void junk_read(struct server *s)
{
	int n = cg_rx(s->jfd, &s->in);
	uint64_t now_ms;
	char a[64];

	if (n <= 0)
		return;
	now_ms = cg_now_ms();
	for (int i = 0; i < n; i++) {
		const uint8_t *b = s->in.buf[i];
		size_t len = s->in.msg[i].msg_len;

		s->rx_junk++;
		if (len < CG_HDR_LEN) {
			s->rx_short++;
			if (cg_ratelimit_ok(&s->rl_junk, now_ms, 10000))
				cg_warn("short packet (%zu bytes) from %s: not cengarde", len,
					cg_addr_str(&s->in.from[i], a, sizeof(a)));
		} else if (b[0] >> 4 != CG_PROTO_VERSION) {
			s->rx_bad_version++;
			if (cg_ratelimit_ok(&s->rl_version, now_ms, 10000))
				cg_warn("protocol v%d packet from %s: update the router or the VPS (this server speaks "
					"v%d), or it is not cengarde",
					b[0] >> 4, cg_addr_str(&s->in.from[i], a, sizeof(a)), CG_PROTO_VERSION);
		}
	}
}

/* ---- WireGuard -> client ---- */

/* Datagram buf from WireGuard as entry m of a batch down S's paths. */
static void down_prepare(struct server *s, struct session *S, int m, uint8_t *buf, size_t len, uint64_t now_us)
{
	const struct client *C = client_of(s, S);
	struct cg_hdr h = { .type = CG_T_DATA, .hint = C->hint, .session = S->id, .ts = (uint32_t)now_us };

	h.seq = S->tx_seq++;
	cg_hdr_write(s->hdr[m], &h, C->key + CG_SIPHASH_KEY_LEN, buf, len);
	s->oiov[m][0].iov_base = s->hdr[m];
	s->oiov[m][0].iov_len = CG_HDR_LEN;
	s->oiov[m][1].iov_base = buf;
	s->oiov[m][1].iov_len = len;
	S->down_pkts++;
	S->down_bytes += len;
}

/* Sends the batch of m entries down the paths of S. */
static void down_send(struct server *s, struct session *S, int m, uint64_t now_ms)
{
	int order[CG_MAX_LINKS], no = 0;
	uint16_t present, live, carry;

	path_masks(S, now_ms, &present, &live);
	carry = cg_health_carriers(S->dh, CG_MAX_LINKS, present, live);
	/* The batch goes out path after path, and the path sent first
	 * delivers first: the first path rotates from batch to batch, so
	 * that none wins the client's first arrivals by its position. */
	for (int p = 0; p < CG_MAX_LINKS; p++)
		if ((carry >> p & 1) || ((live >> p & 1) && s->cfg->mute_trickle))
			order[no++] = p;
	for (int o = 0; o < no; o++) {
		int p = order[(o + S->rot) % (unsigned)no];
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
			if (P->ctl_len) {
				s->out[x].msg_hdr.msg_control = P->ctl.b;
				s->out[x].msg_hdr.msg_controllen = P->ctl_len;
			}
		}
		/* One sendmmsg per path, on the lane its link arrives on: a
		 * path that cannot send never holds up the others, and with
		 * lanes it does not fill their send buffer either. */
		sent = sendmmsg(s->lane[(unsigned)p & (s->nlanes - 1)].fd, s->out, (unsigned)k, MSG_DONTWAIT);
		if (sent < 0) {
			P->tx_drops += (uint64_t)k;
			send_failed(s, S, (unsigned)p, errno, (unsigned)k, now_ms);
			continue;
		}
		P->tx_drops += (uint64_t)(k - sent);
		P->tx_pkts += (uint64_t)sent;
		for (int x = 0; x < sent; x++)
			P->tx_bytes += s->oiov[sel[x]][1].iov_len;
	}
	S->rot++;
}

static void wg_read(struct server *s, struct session *S)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(S->wg_fd, &s->in), m = 0, redir[CG_BATCH], nr = 0;
		const struct client *C = client_of(s, S);
		struct session *N = NULL;
		uint64_t now_us, now_ms;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		/* WireGuard still knows a router that restarted by its old
		 * session: the handshakes it starts go down the client's newest
		 * one while that one knocks in vain (wgwatch.h). */
		if (C->newest >= 0 && &s->s[C->newest] != S &&
		    cg_wgw_redirect(&s->s[C->newest].wgw, S->last_rx_ms, now_ms))
			N = &s->s[C->newest];
		for (int i = 0; i < n; i++) {
			size_t len = s->in.msg[i].msg_len;
			int t;

			if ((s->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) || len > CG_MAX_PAYLOAD || !len) {
				S->toobig++;
				continue;
			}
			t = cg_wg_handshake(s->in.buf[i], len);
			cg_wgw_from_wireguard(&S->wgw, t);
			if (N && t == CG_WG_INITIATION)
				redir[nr++] = i;
			else
				down_prepare(s, S, m++, s->in.buf[i], len, now_us);
		}
		if (m)
			down_send(s, S, m, now_ms);
		if (nr) {
			for (int r = 0; r < nr; r++)
				down_prepare(s, N, r, s->in.buf[redir[r]], s->in.msg[redir[r]].msg_len, now_us);
			down_send(s, N, nr, now_ms);
			s->wg_redirects += (uint64_t)nr;
			if (cg_ratelimit_ok(&s->rl_redirect, now_ms, 60000))
				cg_info("%ssession %08x: a handshake WireGuard started goes down session %08x, the "
					"client's newest", client_of(s, S)->tag, S->id, N->id);
		}
		if (n < CG_BATCH)
			return;
	}
}

/* ---- housekeeping ---- */

static void health_tick(struct server *s, uint64_t now_ms)
{
	for (uint32_t i = 0; i < s->cap; i++) {
		struct session *S = &s->s[i];
		const char *tag = client_of(s, S)->tag;
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
				cg_info("%ssession %08x link %d: download muted, %d ms behind the fastest link", tag, S->id,
					p, h->behind_us / 1000);
			else if (h->unmuted_ms == now_ms)
				cg_info("%ssession %08x link %d: download unmuted, within %d ms of the fastest link", tag,
					S->id, p, h->behind_us > 0 ? h->behind_us / 1000 : 0);
			else if (!s->hcfg.mute_behind_us)
				cg_info("%ssession %08x link %d: download unmuted, muting is off", tag, S->id, p);
			else
				cg_info("%ssession %08x link %d: download unmuted, too few active links", tag, S->id, p);
		}
	}
}

static void sweep(struct server *s, uint64_t now_ms)
{
	char a[64];

	for (uint32_t i = 0; i < s->cap; i++) {
		struct session *S = &s->s[i];

		if (!S->used)
			continue;
		if (now_ms - S->last_rx_ms > s->cfg->session_timeout_ms) {
			session_destroy(s, S, "idle", now_ms);
			continue;
		}
		for (int p = 0; p < CG_MAX_LINKS; p++)
			if (S->path[p].used && now_ms - S->path[p].last_rx_ms > s->cfg->path_timeout_ms) {
				cg_info("%ssession %08x: link %d via %s expired", client_of(s, S)->tag, S->id, p,
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

/* Drops the kernel counted on fd's receive queue, or null. */
static void json_drops(struct cg_json *j, int fd)
{
	uint32_t drops;

	if (cg_sock_drops(fd, &drops) == 0)
		cg_json_u64(j, "drops", drops);
	else
		cg_json_null(j, "drops");
}

/* The listen sockets, the junk socket and the receive budget. */
static void lanes_json(struct server *s, struct cg_json *j)
{
	cg_json_str(j, "steering_error", s->steer_error);
	cg_json_arr(j, "lanes");
	for (uint32_t i = 0; i < s->nlanes; i++) {
		const struct lane *ln = &s->lane[i];

		cg_json_obj(j, NULL);
		cg_json_u64(j, "index", i);
		cg_json_u64(j, "group", 0);
		cg_json_u64(j, "rx", ln->rx);
		json_drops(j, ln->fd);
		cg_json_arr(j, "links");
		for (unsigned l = 0; l < CG_MAX_LINKS; l++)
			if (ln->links >> l & 1)
				cg_json_u64(j, NULL, l);
		cg_json_end(j, ']');
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	if (s->jfd >= 0) {
		cg_json_obj(j, "junk");
		cg_json_u64(j, "index", s->nlanes);
		cg_json_u64(j, "rx", s->rx_junk);
		json_drops(j, s->jfd);
		cg_json_end(j, '}');
	} else {
		cg_json_null(j, "junk");
	}
	cg_json_obj(j, "rcvbuf");
	cg_json_u64(j, "configured", (uint64_t)s->cfg->rcvbuf);
	if (s->rb.budget) {
		cg_json_u64(j, "budget", s->rb.budget);
		cg_json_str(j, "budget_from", s->udp_mem_from ? "RAM" : "net.ipv4.udp_mem");
	} else {
		cg_json_null(j, "budget");
		cg_json_null(j, "budget_from");
	}
	cg_json_u64(j, "sockets", s->rb.sockets);
	cg_json_u64(j, "per_socket", (uint64_t)s->rb.per_socket);
	cg_json_u64(j, "effective", (uint64_t)(s->rcv_eff > 0 ? s->rcv_eff : 0));
	cg_json_end(j, '}');
	cg_json_bool(j, "rcvbuf_capped", s->rb.capped);
}

/* A client as status: its name, null for the one client of a server
 * without [client] sections. */
static void json_client_name(struct server *s, struct cg_json *j, const char *key, const struct client *C)
{
	if (s->multi)
		cg_json_str(j, key, C->name);
	else
		cg_json_null(j, key);
}

static void clients_json(struct server *s, struct cg_json *j)
{
	char buf[64];

	cg_json_arr(j, "clients");
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		const struct client *C = &s->c[i];

		if (!C->used)
			continue;
		cg_json_obj(j, NULL);
		json_client_name(s, j, "name", C);
		cg_json_str(j, "label", C->label);
		cg_json_bool(j, "enabled", C->enabled);
		snprintf(buf, sizeof(buf), "%02x", C->hint);
		cg_json_str(j, "hint", buf);
		cg_json_str(j, "wireguard", cg_addr_str(&C->wireguard, buf, sizeof(buf)));
		cg_json_u64(j, "sessions", C->nsess);
		if (C->newest >= 0) {
			snprintf(buf, sizeof(buf), "%08x", s->s[C->newest].id);
			cg_json_str(j, "newest", buf);
		} else {
			cg_json_null(j, "newest");
		}
		json_pass(j, "passthrough", C->pass);
		cg_json_bool(j, "passthrough_allowed", C->passthrough);
		if (s->multi)
			cg_json_bool(j, "ip_pass", s->holder == i);
		else
			cg_json_null(j, "ip_pass");
		cg_json_u64(j, "refused", C->refused);
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
}

static void status_json(struct server *s, uint64_t now_ms, struct cg_json *j)
{
	char buf[64];

	cg_json_obj(j, NULL);
	cg_json_str(j, "mode", "server");
	cg_json_str(j, "version", CG_VERSION);
	cg_json_str(j, "description", s->cfg->description);
	cg_json_u64(j, "uptime_ms", now_ms - s->start_ms);
	cg_json_u64(j, "time_ms", cg_wall_ms());
	cg_json_str(j, "config_error", s->config_error);
	cg_json_str(j, "listen", s->laddr);
	cg_json_bool(j, "reply_from_arrival", s->pktinfo);
	json_pass(j, "passthrough", s->multi ? -1 : s->pw.running ? s->pass_written : -1);
	if (s->multi && s->holder >= 0)
		cg_json_str(j, "ip_pass_holder", s->c[s->holder].name);
	else
		cg_json_null(j, "ip_pass_holder");
	cg_json_u64(j, "max_sessions", s->max);
	lanes_json(s, j);
	cg_json_obj(j, "rx");
	cg_json_u64(j, "duplicates", s->rx_dups);
	cg_json_u64(j, "too_old", s->rx_old);
	cg_json_u64(j, "auth_failures", s->rx_auth_fail);
	cg_json_u64(j, "malformed", s->rx_malformed + s->rx_trunc);
	cg_json_u64(j, "ctrunc", s->rx_ctrunc);
	cg_json_u64(j, "sessions_refused", s->sessions_full);
	cg_json_u64(j, "junk", s->rx_junk);
	cg_json_u64(j, "short", s->rx_short);
	cg_json_u64(j, "bad_version", s->rx_bad_version);
	cg_json_u64(j, "other_hint", s->rx_hint);
	cg_json_u64(j, "no_session", s->rx_no_session);
	cg_json_u64(j, "id_clashes", s->id_clashes);
	cg_json_end(j, '}');
	/* Probes it could not take yet: a cookie sent back, or a refusal. */
	cg_json_obj(j, "hellos");
	cg_json_u64(j, "sent", s->hellos);
	cg_json_u64(j, "refused", s->hellos_refused);
	cg_json_u64(j, "over_budget", s->hellos_over);
	cg_json_end(j, '}');
	cg_json_obj(j, "wireguard");
	cg_json_u64(j, "pokes", s->wg_pokes);
	cg_json_u64(j, "redirected_handshakes", s->wg_redirects);
	cg_json_end(j, '}');
	clients_json(s, j);
	cg_json_arr(j, "sessions");
	for (uint32_t i = 0; i < s->cap; i++) {
		const struct session *S = &s->s[i];

		if (!S->used)
			continue;
		cg_json_obj(j, NULL);
		snprintf(buf, sizeof(buf), "%08x", S->id);
		cg_json_str(j, "id", buf);
		json_client_name(s, j, "client", client_of(s, S));
		cg_json_bool(j, "newest", client_of(s, S)->newest == (int32_t)i);
		json_pass(j, "passthrough", S->pass);
		cg_json_u64(j, "age_ms", now_ms - S->created_ms);
		cg_json_u64(j, "last_rx_ms_ago", now_ms - S->last_rx_ms);
		cg_json_u64(j, "upload_packets", S->up_pkts);
		cg_json_u64(j, "upload_bytes", S->up_bytes);
		cg_json_u64(j, "download_packets", S->down_pkts);
		cg_json_u64(j, "download_bytes", S->down_bytes);
		cg_json_u64(j, "wireguard_drops", S->wg_drops);
		cg_json_u64(j, "wireguard_unanswered", cg_wgw_knocking(&S->wgw, now_ms) ? S->wgw.unanswered : 0);
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
			if (P->local.known)
				cg_json_str(j, "local", local_str(s, &P->local, buf, sizeof(buf)));
			else
				cg_json_null(j, "local");
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
			cg_json_u64(j, "moves", P->moves);
			cg_json_u64(j, "local_errors", P->local_errors);
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

/* "cengarde ctl links": the sessions and their links, as a table. The
 * addresses go last, ADDRESS wide enough for "[IPv6]:port". With several
 * clients, a CLIENT column first. */
#define PATH_ROW "%-9s %-6s %-5s %-8s %-8s %-8s %-47s %s\n"
static void links_text(struct server *s, uint64_t now_ms, struct cg_json *j)
{
	char id[16], link[8], ago[24], a[64], l[64];

	if (s->multi)
		cg_json_raw(j, "%-16s ", "CLIENT");
	cg_json_raw(j, PATH_ROW, "SESSION", "NEWEST", "LINK", "STATE", "DOWNLOAD", "LAST RX", "ADDRESS", "LOCAL");
	for (uint32_t i = 0; i < s->cap; i++) {
		const struct session *S = &s->s[i];
		const struct client *C = client_of(s, S);

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
			if (s->multi)
				cg_json_raw(j, "%-16s ", C->name);
			cg_json_raw(j, PATH_ROW, id, C->newest == (int32_t)i ? "yes" : "", link,
				    path_live(P, now_ms) ? "live" : "stalled", S->dh[p].state == CG_H_MUTED ? "muted" : "active",
				    ago, cg_addr_str(&P->addr, a, sizeof(a)),
				    P->local.known ? local_str(s, &P->local, l, sizeof(l)) : "-");
		}
	}
}

/* One client: hands the IP pass its newest session asks for to the writer;
 * tried again on the next tick when the writer is busy that instant. */
static void pass_sync(struct server *s)
{
	int pass = s->c[0].pass;
	struct cg_json j;

	if (!s->pw.running || pass < 0 || pass == s->pass_written)
		return;
	cg_json_init(&j);
	cg_json_raw(&j, "%s\n", pass ? "on" : "off");
	if (cg_status_writer_submit(&s->pw, &j) == 0) {
		cg_info("IP pass %s, as the newest session asks: %s", pass ? "on" : "off", s->cfg->passthrough_file);
		s->pass_written = pass;
	}
	cg_json_free(&j);
}

/* Several clients: who holds IP pass, the whole range (fwdtable.h), and
 * the forward table, handed to its writer when it changed; tried again on
 * the next tick when the writer is busy that instant. */
static void fwd_sync(struct server *s, uint64_t now_ms)
{
	const struct cg_config *cfg = s->cfg;
	struct cg_pass_cand cand[CG_MAX_CLIENTS];
	const char *names[CG_MAX_CLIENTS];
	uint8_t writes[CG_MAX_CLIENTS];
	int holder, hc = -1, len, rules = 0;
	struct cg_json j;

	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		const struct client *C = &s->c[i];

		cand[i].eligible = C->used && C->enabled && C->passthrough;
		cand[i].wish = C->used ? C->pass : -1;
		cand[i].wish_ms = C->pass_on_ms;
	}
	holder = cg_pass_holder(cand, CG_MAX_CLIENTS, s->holder);
	if (holder != s->holder) {
		if (holder >= 0)
			cg_info("IP pass: the whole range goes to %s", s->c[holder].name);
		else
			cg_info("IP pass: nobody holds the whole range");
		s->holder = holder;
	}
	/* What each client asked for, once per change: whether it got it. */
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		struct client *C = &s->c[i];

		if (!C->used || C->pass == C->pass_seen)
			continue;
		C->pass_seen = C->pass;
		if (C->pass != 1 || holder == i)
			cg_info("%sasks for IP pass %s", C->tag, C->pass ? "on" : "off");
		else if (!C->passthrough || holder < 0)
			cg_info("%sasks for IP pass on, which it may not have (passthrough = no)", C->tag);
		else
			cg_info("%sasks for IP pass on, which %s holds: its turn comes when %s lets it go", C->tag,
				s->c[holder].name, s->c[holder].name);
	}
	if (!s->fw.running)
		return;
	/* The rules name their clients by their index in the configuration. */
	for (int i = 0; i < cfg->nclients; i++) {
		names[i] = cfg->clients[i].name;
		writes[i] = (uint8_t)cfg->clients[i].enabled;
		if (holder >= 0 && !strcmp(names[i], s->c[holder].name))
			hc = i;
	}
	len = cg_fwd_table(s->fw_next, sizeof(s->fw_next), hc, names, cfg->forward, cfg->nforward, writes);
	if (len < 0) {
		if (cg_ratelimit_ok(&s->rl_fw, now_ms, 60000))
			cg_warn("%s: the forward table would be over %d bytes, not written", cfg->forward_file,
				FWD_TABLE_MAX - 1);
		return;
	}
	if (s->fw_written && (size_t)len == s->fw_len && !memcmp(s->fw_next, s->fw_text, (size_t)len))
		return;
	cg_json_init(&j);
	cg_json_raw(&j, "%s", s->fw_next);
	if (cg_status_writer_submit(&s->fw, &j) == 0) {
		memcpy(s->fw_text, s->fw_next, (size_t)len + 1);
		s->fw_len = (size_t)len;
		s->fw_written = 1;
		s->fw_holder = holder;
		for (int i = 0; i < cfg->nforward; i++)
			rules += writes[cfg->forward[i].client];
		cg_info("forward table: IP pass %s%s, %d rule%s: %s", holder >= 0 ? "to " : "to nobody",
			holder >= 0 ? s->c[holder].name : "", rules, rules == 1 ? "" : "s", cfg->forward_file);
	}
	cg_json_free(&j);
}

/* The forward table a previous run left, once at start: whoever held IP
 * pass keeps it until it lets it go (fwdtable.h), and a table that says
 * what this run would write is not written again. */
static void fwd_load(struct server *s)
{
	char name[CG_CLIENT_NAME];
	FILE *f = fopen(s->cfg->forward_file, "r");
	size_t n;

	if (!f)
		return;
	n = fread(s->fw_text, 1, sizeof(s->fw_text) - 1, f);
	fclose(f);
	s->fw_text[n] = '\0';
	if (n == sizeof(s->fw_text) - 1 || strlen(s->fw_text) != n)
		return;
	s->fw_len = n;
	s->fw_written = 1;
	if (!cg_fwd_table_holder(s->fw_text, name, sizeof(name)))
		return;
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		if (s->c[i].used && !strcmp(s->c[i].name, name)) {
			s->holder = s->fw_holder = i;
			cg_info("IP pass: %s keeps the whole range, as %s says", name, s->cfg->forward_file);
		}
}

/* WireGuard ignores the handshakes of C's newest session S, most likely
 * because the client's clock went back: a datagram to the client's address
 * in the tunnel makes it start one of its own, which the client takes
 * whatever its clock says (wgwatch.h). From a socket opened for the moment:
 * at most one every CG_WGW_POKE_MS, and only while the client knocks. */
static void wg_poke(struct server *s, struct client *C, struct session *S, uint64_t now_ms)
{
	const struct sockaddr_storage *to = &C->poke;
	char a[64];
	int fd;

	if (to->ss_family == AF_UNSPEC || !cg_wgw_poke(&S->wgw, now_ms))
		return;
	fd = cg_udp_socket(to->ss_family);
	if (fd < 0 || sendto(fd, "", 1, 0, (const struct sockaddr *)to, cg_addr_len(to)) < 0) {
		if (cg_ratelimit_ok(&C->rl_poke, now_ms, 60000))
			cg_warn("%ssession %08x: cannot poke WireGuard through %s: %s", C->tag, S->id,
				cg_addr_str(to, a, sizeof(a)), strerror(errno));
	} else {
		s->wg_pokes++;
		if (cg_ratelimit_ok(&C->rl_poke, now_ms, 60000))
			cg_info("%ssession %08x: WireGuard ignores the client's handshakes (%u so far; its clock may have "
				"gone back): poking it through %s to start one",
				C->tag, S->id, S->wgw.unanswered, cg_addr_str(to, a, sizeof(a)));
	}
	if (fd >= 0)
		close(fd);
}

/* Once a second: the CPU clock of each thread, for "ctl threads". */
static void threads_sample(struct server *s, uint64_t now_ms)
{
	cg_cpuwin_add(&s->cpu[0], now_ms, cg_thread_cpu_ns(pthread_self(), 1));
	if (s->sw.running)
		cg_cpuwin_add(&s->cpu[1], now_ms, cg_thread_cpu_ns(s->sw.thread, 0));
	if (s->pw.running)
		cg_cpuwin_add(&s->cpu[2], now_ms, cg_thread_cpu_ns(s->pw.thread, 0));
	if (s->fw.running)
		cg_cpuwin_add(&s->cpu[3], now_ms, cg_thread_cpu_ns(s->fw.thread, 0));
	s->next_cpu_ms = now_ms + 1000;
}

static void threads_text(struct server *s, struct cg_json *j)
{
	cg_threads_head(j);
	cg_threads_row(j, "cg-main", cg_gettid(), cg_thread_cpu_ns(pthread_self(), 1), cg_cpuwin_permille(&s->cpu[0]));
	if (s->sw.running)
		cg_threads_row(j, s->sw.name, s->sw.tid, cg_thread_cpu_ns(s->sw.thread, 0), cg_cpuwin_permille(&s->cpu[1]));
	if (s->pw.running)
		cg_threads_row(j, s->pw.name, s->pw.tid, cg_thread_cpu_ns(s->pw.thread, 0), cg_cpuwin_permille(&s->cpu[2]));
	if (s->fw.running)
		cg_threads_row(j, s->fw.name, s->fw.tid, cg_thread_cpu_ns(s->fw.thread, 0), cg_cpuwin_permille(&s->cpu[3]));
}

static void tick(struct server *s)
{
	uint64_t now_ms = cg_now_ms();

	if (now_ms >= s->next_cpu_ms)
		threads_sample(s, now_ms);
	health_tick(s, now_ms);
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		if (s->c[i].used && s->c[i].newest >= 0)
			wg_poke(s, &s->c[i], &s->s[s->c[i].newest], now_ms);
	if (now_ms >= s->next_sweep_ms) {
		sweep(s, now_ms);
		s->next_sweep_ms = now_ms + 1000;
	}
	if (s->multi)
		fwd_sync(s, now_ms);
	else
		pass_sync(s);
	if (s->sw.running && now_ms >= s->next_status_ms) {
		write_status(s, now_ms);
		s->next_status_ms = now_ms + s->cfg->status_interval_ms;
	}
	cg_ctl_expire(&s->ctl, s->ep, now_ms);
}

/* ---- listen sockets ---- */

/* The receive budget (rcvbudget.h), from net.ipv4.udp_mem as it is now:
 * at start and on every reload. The lanes and every session's WireGuard
 * socket get the new value when it changed, or always with force (the
 * configured rcvbuf changed: the send buffers follow it, as before). The
 * junk socket keeps its fixed buffer. */
static void rcvbuf_apply(struct server *s, int force)
{
	uint64_t mem[3];
	int from = cg_udp_mem_read(mem);
	long page = sysconf(_SC_PAGESIZE);
	uint32_t routers = 0;
	struct cg_rcvbudget rb;

	/* One socket to WireGuard per router: its newest session's. */
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		routers += s->c[i].used && s->c[i].enabled;
	rb = cg_rcvbudget(from >= 0 ? mem : NULL, page > 0 ? (uint64_t)page : 4096,
			  cg_rcvbudget_sockets(s->nlanes, 1, routers ? routers : 1, s->jfd >= 0), s->cfg->rcvbuf);

	s->udp_mem_from = from;

	if (rb.capped && (!s->rb.capped || rb.per_socket != s->rb.per_socket))
		cg_warn("rcvbuf: %d bytes per socket instead of %d: %u receiving sockets share %" PRIu64
			" MiB, half of net.ipv4.udp_mem's pressure threshold, so that other UDP sockets keep room",
			rb.per_socket, s->cfg->rcvbuf, rb.sockets, rb.budget >> 20);
	if (!force && rb.per_socket == s->rb.per_socket) {
		s->rb = rb;
		return;
	}
	s->rb = rb;
	for (uint32_t i = 0; i < s->nlanes; i++) {
		int rcv = cg_sock_buffers(s->lane[i].fd, rb.per_socket, s->cfg->rcvbuf);

		if (!i)
			s->rcv_eff = rcv;
	}
	for (uint32_t i = 0; i < s->cap; i++)
		if (s->s[i].used)
			cg_sock_buffers(s->s[i].wg_fd, rb.per_socket, s->cfg->rcvbuf);
}

static void lanes_close(struct server *s)
{
	for (uint32_t i = 0; i < CG_MAX_LANES; i++)
		if (s->lane[i].fd >= 0) {
			close(s->lane[i].fd);
			s->lane[i].fd = -1;
		}
	if (s->jfd >= 0)
		close(s->jfd);
	s->jfd = -1;
	s->nlanes = 0;
}

/* One listen socket, as before lanes. */
static int lane_single(struct server *s, char *err, size_t errlen)
{
	s->lane[0].fd = cg_udp_bind_opts(&s->cfg->listen, 0, 0, &s->pktinfo, &s->lfamily, err, errlen);
	if (s->lane[0].fd < 0)
		return -1;
	s->nlanes = 1;
	return 0;
}

/* Opens the listen sockets: with lanes > 1, the SO_REUSEPORT group of the
 * lanes and the junk socket, steered by link id (steer.h). Returns 0, or -1
 * with err set: the start fails. */
static int lanes_open(struct server *s, char *err, size_t errlen)
{
	const struct cg_config *cfg = s->cfg;
	struct sock_filter prog[CG_STEER_MAX];
	int n, fd, off = 0, pktinfo = s->pktinfo;

	if (cfg->lanes <= 1)
		return lane_single(s, err, errlen);
	n = cg_steer_prog(prog, CG_STEER_MAX, cfg->lanes);
	if (n < 0) {
		snprintf(err, errlen, "lanes = %u: no steering program for it", cfg->lanes);
		return -1;
	}
	/* Bound once without SO_REUSEPORT: it fails if another process holds
	 * the port. With SO_REUSEPORT alone, a second cengarde of the same
	 * user would join the group in silence and take part of the traffic. */
	fd = cg_udp_bind_opts(&cfg->listen, 0, 0, &off, NULL, err, errlen);
	if (fd < 0)
		return -1;
	close(fd);
	/* In the order of steer.h: the lanes, then junk. A member is never
	 * closed alone (the last one would move into its index), so a failure
	 * in the middle closes them all and the start fails. */
	for (uint32_t i = 0; i <= cfg->lanes; i++) {
		int pk = i < cfg->lanes ? pktinfo : 0;

		fd = cg_udp_bind_opts(&cfg->listen, 0, 1, &pk, i ? NULL : &s->lfamily, err, errlen);
		if (fd < 0) {
			lanes_close(s);
			return -1;
		}
		if (i < cfg->lanes) {
			s->lane[i].fd = fd;
			s->nlanes = i + 1;
			if (!pk)
				s->pktinfo = 0;
		} else {
			s->jfd = fd;
		}
	}
	if (cg_sock_steer(s->lane[0].fd, prog, (unsigned short)n) == 0)
		return 0;
	/* A kernel before 4.5: one socket, as before lanes. */
	snprintf(s->steer_error, sizeof(s->steer_error), "SO_ATTACH_REUSEPORT_CBPF: %s", strerror(errno));
	cg_warn("%s: the kernel cannot steer the %u lanes; one listen socket instead", s->steer_error, cfg->lanes);
	lanes_close(s);
	s->pktinfo = pktinfo;
	return lane_single(s, err, errlen);
}

/* ---- reload ---- */

static void reload_start(struct server *s)
{
	if (cg_loader_start(&s->loader, s->run->path) < 0)
		s->reload_again = 1; /* once the load under way is done */
}

/* Client C's settings from k (its key and WireGuard address included: the
 * caller closes its sessions first when they change). */
static void client_set(struct server *s, struct client *C, const struct cg_client_cfg *k)
{
	snprintf(C->name, sizeof(C->name), "%s", k->name);
	if (s->multi)
		snprintf(C->tag, sizeof(C->tag), "%s: ", k->name);
	else
		C->tag[0] = '\0';
	snprintf(C->label, sizeof(C->label), "%s", k->label);
	memcpy(C->key, k->key, CG_KEY_LEN);
	C->hint = cg_client_hint(k->key);
	C->enabled = k->enabled;
	C->passthrough = k->passthrough;
	C->wireguard = k->wireguard;
	C->poke = k->wireguard_poke;
}

/* Puts the clients of cfg in place. A client keeps its slot, its sessions
 * and its IP pass while its name stays; one that is gone or disabled, or
 * whose key or WireGuard address changed, loses its sessions (its routers
 * start new ones at once, or are refused). New names take free slots. With
 * no [client] section, the global key is the one client, named "". */
static void clients_apply(struct server *s, const struct cg_config *cfg, uint64_t now_ms)
{
	struct cg_client_cfg one;
	const struct cg_client_cfg *list = cfg->clients;
	uint8_t hint[CG_MAX_CLIENTS], used[CG_MAX_CLIENTS];
	int n = cfg->nclients;

	s->multi = cfg->nclients > 0;
	if (!s->multi) {
		memset(&one, 0, sizeof(one));
		memcpy(one.key, cfg->key, CG_KEY_LEN);
		one.enabled = one.passthrough = 1;
		one.wireguard = cfg->wireguard;
		one.wireguard_poke = cfg->wireguard_poke;
		list = &one;
		n = 1;
	}
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		struct client *C = &s->c[i];
		const struct cg_client_cfg *k = NULL;
		const char *why = NULL;

		if (!C->used)
			continue;
		for (int j = 0; j < n && !k; j++)
			if (!strcmp(list[j].name, C->name))
				k = &list[j];
		if (!k)
			why = "the client was removed";
		else if (memcmp(k->key, C->key, CG_KEY_LEN))
			why = "the client's key changed";
		else if (!cg_addr_equal(&k->wireguard, &C->wireguard))
			why = "the client's WireGuard address changed";
		else if (!k->enabled && C->enabled)
			why = "the client was disabled";
		if (why)
			client_close(s, C, why, now_ms);
		if (!k) {
			cg_info("%sremoved", C->tag);
			C->used = 0;
			continue;
		}
		if (!k->enabled && C->enabled)
			cg_info("%sdisabled: its probes are refused", C->tag);
		client_set(s, C, k);
	}
	for (int j = 0; j < n; j++) {
		struct client *C = NULL;
		int taken = 0;

		for (int i = 0; i < CG_MAX_CLIENTS && !taken; i++)
			taken = s->c[i].used && !strcmp(s->c[i].name, list[j].name);
		for (int i = 0; i < CG_MAX_CLIENTS && !taken && !C; i++)
			if (!s->c[i].used)
				C = &s->c[i];
		if (taken || !C)
			continue;
		memset(C, 0, sizeof(*C));
		C->used = 1;
		C->newest = -1;
		C->pass = C->pass_seen = -1;
		if (cg_random(&C->mix, sizeof(C->mix)) < 0)
			C->mix = (uint32_t)cg_now_us() * 2654435761u;
		client_set(s, C, &list[j]);
		if (s->multi)
			cg_info("%sclient hint %02x, WireGuard at %s%s", C->tag, C->hint, cg_addr_str(&C->wireguard,
				(char[64]){ 0 }, 64), C->enabled ? "" : ", disabled");
	}
	if (s->holder >= 0 && !s->c[s->holder].used)
		s->holder = -1;
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		hint[i] = s->c[i].hint;
		used[i] = (uint8_t)s->c[i].used;
	}
	cg_hintidx_build(&s->hx, hint, used, CG_MAX_CLIENTS);
}

/* Room for max sessions: the session array and the map grow (never
 * shrink) between two batches, where nothing points into them. A smaller
 * max only refuses new sessions. Returns 0, or -1 when out of memory (the
 * old room stays). */
static int sessions_room(struct server *s, uint32_t max)
{
	struct session *ns;
	struct cg_idmap nm;

	s->max = max;
	if (max <= s->cap)
		return 0;
	ns = realloc(s->s, sizeof(*ns) * max);
	if (!ns)
		return -1;
	s->s = ns;
	memset(s->s + s->cap, 0, sizeof(*ns) * (max - s->cap));
	if (cg_idmap_init(&nm, max) < 0) {
		s->max = s->cap;
		return -1;
	}
	for (uint32_t i = 0; i < s->cap; i++)
		if (s->s[i].used)
			cg_idmap_put(&nm, cg_sesskey(s->c[s->s[i].client].mix, s->s[i].id), (int32_t)i);
	cg_idmap_free(&s->ids);
	s->ids = nm;
	s->cap = max;
	return 0;
}

static void writer_restart(struct cg_status_writer *w, const char *path, const char *name)
{
	cg_status_writer_stop(w);
	if (path[0] && cg_status_writer_start(w, path, name) < 0)
		cg_warn("%s: cannot start the writer thread", path);
}

/* Puts next in place of the running configuration, keeping the sessions. */
static void apply_config(struct server *s, struct cg_config *next)
{
	struct cg_config *old = s->cfg;
	uint64_t now_ms = cg_now_ms();

	s->cfg = next;
	s->hcfg = cg_hcfg_of(next);
	cg_log_level_set(s->run->verbose ? CG_LOG_DEBUG : next->log_level);
	clients_apply(s, next, now_ms);
	if (sessions_room(s, next->max_sessions) < 0)
		cg_warn("max_sessions = %u: out of memory, room for %u only", next->max_sessions, s->cap);
	if (strcmp(old->status_file, next->status_file))
		writer_restart(&s->sw, next->status_file, "cg-status");
	if (strcmp(old->passthrough_file, next->passthrough_file)) {
		writer_restart(&s->pw, next->passthrough_file, "cg-pass");
		s->pass_written = -1; /* write it again, there */
	}
	if (strcmp(old->forward_file, next->forward_file)) {
		writer_restart(&s->fw, next->forward_file, "cg-forward");
		s->fw_written = 0; /* write it again, there */
	}
	rcvbuf_apply(s, old->rcvbuf != next->rcvbuf);
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
	cg_status_writer_stop(&s->fw);
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
		case CG_CTL_THREADS:
			threads_text(s, &j);
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
	struct sockaddr_storage bound;
	socklen_t blen = sizeof(bound);
	char err[256], wg[64];
	uint64_t last_traffic_us = 0;
	uint32_t busy;
	int rc = 1;

	if (!s) {
		cg_err("out of memory");
		cg_config_free(cfg);
		free(cfg);
		return 1;
	}
	s->cfg = cfg;
	s->run = run;
	s->ep = s->tfd = s->jfd = -1;
	for (uint32_t i = 0; i < CG_MAX_LANES; i++)
		s->lane[i].fd = -1;
	s->pass_written = s->holder = s->fw_holder = -1;
	s->hcfg = cg_hcfg_of(cfg);
	cg_ctl_init(&s->ctl);
	if (cg_loader_init(&s->loader) < 0) {
		cg_err("eventfd: %s", strerror(errno));
		goto out;
	}
	s->start_ms = cg_now_ms();
	clients_apply(s, cfg, s->start_ms);
	if (cg_idmap_init(&s->ids, 1) < 0 || sessions_room(s, cfg->max_sessions) < 0) {
		cg_err("out of memory for %u sessions", cfg->max_sessions);
		goto out;
	}

	s->pktinfo = 1; /* on a wildcard only */
	if (lanes_open(s, err, sizeof(err)) < 0) {
		cg_err("%s", err);
		goto out;
	}
	/* What was bound: "*" is IPv4 on a kernel without IPv6. */
	if (getsockname(s->lane[0].fd, (struct sockaddr *)&bound, &blen) < 0)
		bound = cfg->listen;
	cg_addr_str(&bound, s->laddr, sizeof(s->laddr));
	s->lport = bound.ss_family == AF_INET ? ((const struct sockaddr_in *)&bound)->sin_port :
						((const struct sockaddr_in6 *)&bound)->sin6_port;
	rcvbuf_apply(s, 1);
	if (s->jfd >= 0)
		cg_sock_buffers(s->jfd, JUNK_RCVBUF, 0);
	if (!s->rb.capped && s->rcv_eff < (1 << 20))
		cg_warn("receive buffer is only %d bytes; raise net.core.rmem_max or run with CAP_NET_ADMIN",
			s->rcv_eff);
	s->ep = epoll_create1(EPOLL_CLOEXEC);
	s->tfd = cg_timerfd(CG_TICK_MS);
	if (s->ep < 0 || s->tfd < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	for (uint32_t i = 0; i < s->nlanes; i++)
		if (cg_epoll_add(s->ep, s->lane[i].fd, CG_EV(CG_EV_LISTEN, i)) < 0) {
			cg_err("epoll setup: %s", strerror(errno));
			goto out;
		}
	if ((s->jfd >= 0 && cg_epoll_add(s->ep, s->jfd, CG_EV(CG_EV_JUNK, 0)) < 0) ||
	    cg_epoll_add(s->ep, s->tfd, CG_EV(CG_EV_TIMER, 0)) < 0 ||
	    cg_epoll_add(s->ep, run->sigfd, CG_EV(CG_EV_SIG, 0)) < 0 ||
	    cg_epoll_add(s->ep, s->loader.efd, CG_EV(CG_EV_LOAD, 0)) < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	/* The control socket is a convenience: the tunnel runs without it. */
	if (cfg->control_socket[0] && cg_ctl_open(&s->ctl, cfg->control_socket, s->ep, err, sizeof(err)) < 0)
		cg_warn("control socket: %s", err);
	if (cfg->status_file[0] && cg_status_writer_start(&s->sw, cfg->status_file, "cg-status") < 0)
		cg_warn("status file %s: cannot start the writer thread", cfg->status_file);
	if (cfg->passthrough_file[0] && cg_status_writer_start(&s->pw, cfg->passthrough_file, "cg-pass") < 0)
		cg_warn("%s: cannot start the writer thread", cfg->passthrough_file);
	if (cfg->forward_file[0]) {
		fwd_load(s);
		if (cg_status_writer_start(&s->fw, cfg->forward_file, "cg-forward") < 0)
			cg_warn("%s: cannot start the writer thread", cfg->forward_file);
	}
	if (s->multi)
		cg_info("server %s: listening on %s%s, %d clients, up to %u sessions", CG_VERSION, s->laddr,
			s->pktinfo ? ", replying from each packet's arrival address" : "", cfg->nclients, s->max);
	else
		cg_info("server %s: listening on %s%s, WireGuard at %s, up to %u sessions", CG_VERSION, s->laddr,
			s->pktinfo ? ", replying from each packet's arrival address" : "",
			cg_addr_str(&cfg->wireguard, wg, sizeof(wg)), s->max);
	if (s->jfd >= 0)
		cg_info("%u listen lanes by link id, and a junk socket; receive buffers %d bytes each (%u sockets)",
			s->nlanes, s->rb.per_socket, s->rb.sockets);
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
				if (idx < s->nlanes)
					listen_read(s, &s->lane[idx]);
				traffic = 1;
				break;
			case CG_EV_JUNK:
				junk_read(s);
				break;
			case CG_EV_WG:
				if (idx < s->cap && s->s[idx].used)
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
	cg_status_writer_stop(&s->fw);
	cg_ctl_close(&s->ctl, s->ep);
	cg_loader_free(&s->loader);
	for (uint32_t i = 0; s->s && i < s->cap; i++)
		if (s->s[i].used)
			close(s->s[i].wg_fd);
	lanes_close(s);
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
