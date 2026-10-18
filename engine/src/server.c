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
 * a distinct endpoint and many clients can share one port, and puts its
 * client's verified DATA back in sequence first (reorder.h, story 012): a
 * packet that comes in ahead of a missing one waits in a pool buffer shared
 * by every session, until the missing one comes, every path brought a later
 * one, or its deadline; the sessions holding packets are on a list, and the
 * nearest deadline among them caps the loop's wait. Link health
 * (health.h) mutes the download on paths that lag far behind the fastest
 * one, from the delays the client reports in its probes. With split
 * (split.h) a big data packet goes down one path instead, by smooth
 * round-robin among the live, active paths the client still answers on:
 * always with force (the lab, evenly), and with client (the default) when
 * the client's probes ask for it and say it reorders, by the weights they
 * carry per link. Replies to such probes say whether this end reorders
 * (proto.h's tail), which is what the client's upload split follows.
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
 * closes, or when it starts alone. That port outlives the engine: the server
 * writes each client's to wireguard_port_file (wgport.h), from a thread, and
 * the first session after a restart binds it again.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "decrec.h"
#include "engine.h"
#include "health.h"
#include "idmap.h"
#include "log.h"
#include "pair.h"
#include "pktinfo.h"
#include "rcvbudget.h"
#include "replay.h"
#include "sock.h"
#include "capacity.h"
#include "pace.h"
#include "tcpfree.h"
#include "rescue.h"
#include "govern.h"
#include "split.h"
#include "status.h"
#include "steer.h"
#include "thrplan.h" /* cg_cpuwin */
#include "ippkt.h"
#include "tun.h"
#include "tunroute.h"
#include "util.h"
#include "wgport.h"
#include "wgsize.h"
#include "wgwatch.h"

/* split = force shares the packets out evenly (split.h): every path weighs
 * 1. With split = client, the weights the client sends per link. */
static const uint16_t split_equal[CG_MAX_LINKS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
_Static_assert(CG_MAX_LINKS == 16, "split_equal has a weight per path");

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
	uint16_t bond_w;      /* the weight its newest probe's tail gives it (split = client) */
	uint8_t bond_ok;      /* that probe had a tail */
	uint64_t tx_pkts, tx_bytes, tx_drops;
	uint64_t tx_split;     /* of tx_pkts, split packets (split.h): its share, or all of them with weight 0 */
	uint64_t moves;        /* changes of address, at either end */
	uint64_t local_errors; /* datagrams that could not leave: our address gone, no route */
	struct cg_probe_info peer_view; /* client's view of this path (download) */
	uint64_t peer_view_ms;
};

struct session {
	int used;
	uint32_t id;
	int client; /* its client's slot in server.c */
	int wg_fd;        /* -1 with tunnel = native: the server's TUN instead */
	uint16_t wg_port; /* wg_fd's own port (network order): WireGuard knows the client by it */
	struct cg_wgw wgw; /* the client's handshakes WireGuard has not answered */
	uint32_t tx_seq; /* next DATA sequence down */
	uint8_t prev_only; /* 1 + the path the previous one went on alone, 0: several (proto.h) */
	uint32_t *resc_idx; /* its index into the rescue ring (rescue.h), NULL: none */
	struct cg_resc_budget resc_b;
	uint64_t nacks, resc_sent, resc_miss, resc_budget; /* NACKs taken; sequences sent again, not kept, over budget */
	uint64_t resc_congested; /* not sent again: lost on a full path (TCP has to see it) */
	int peer_rescue;          /* the client keeps what it sends up: its reorder asks (rescue.h) */
	uint64_t nacks_sent;      /* NACKs sent to the client for the upload */
	uint64_t resc_gone_tx, resc_gone_rx; /* CG_F_GONE said to the client; asked gaps it said were gone */
	struct cg_ro_cfg rocf;    /* the server's reorder settings, with its rescue */
	uint32_t tx_ctl; /* next probe reply sequence */
	int pass; /* IP pass its probes ask for: -1 nothing */
	uint64_t created_ms, last_rx_ms;
	struct path path[CG_MAX_LINKS];
	struct cg_hlink dh[CG_MAX_LINKS]; /* download health, by link id */
	struct cg_cap_link cap[CG_MAX_LINKS]; /* each path's download capacity (capacity.h), by link id */
	uint16_t cw[CG_MAX_LINKS];            /* the weights from them, with capacity_weights */
	struct cg_cap_total ctot;             /* their total, for a shaper on the router's interface */
	struct cg_eta eta;                    /* the split by expected arrival, with split_eta */
	struct cg_replay replay; /* DATA up; its lost: lost on every link (lost_all) */
	struct cg_replay ctl;    /* probes */
	struct cg_arrivals arr;  /* by link, and joint lateness (joint) */
	struct cg_link_rx rx[CG_MAX_LINKS];
	uint64_t up_pkts, up_bytes, down_pkts, down_bytes, wg_drops, toobig;
	uint64_t not_ip, spoofed; /* native: upload not written to the TUN (not IP; a source not the client's) */
	uint32_t rot; /* download batches sent: the first path of each rotates */
	uint64_t down_split; /* of down_pkts, packets split (split.h) instead of copied to every path */
	struct cg_split sp;  /* the split's round-robin credits */
	struct cg_spread spread; /* guard G4: the paths of similar delay that share the split (split.h) */
	struct cg_sdup sdup;     /* copies under G4's stress, on a budget */
	/* The download's last decisions (decision.h), and the 0/1 states they
	 * were recorded at: G3 per path, G4's stress and recovery, the copies'
	 * budget, the rescue's. */
	struct cg_decring dec;
	uint8_t dec_stale[CG_MAX_LINKS];
	uint8_t dec_stress, dec_recover, dec_dry, dec_rfull;
	struct cg_gov gov;       /* the download's weights from each path's queue (govern.h), with governor */
	uint16_t gw[CG_MAX_LINKS];
	uint64_t last_down_ms;   /* the newest download packet: the governor moves while traffic goes */
	uint32_t ro_pos;       /* its place in the server's ro_active, plus 1; 0: holds nothing */
	/* The newest probe's bonding tail (proto.h): whether it had one, the
	 * client reorders, and asks for a split download (split = client); and
	 * which probe that was (split.h). */
	uint8_t peer_bond, peer_reorders, peer_asks;
	struct cg_split_tail tail;
	struct cg_reorder ro;  /* the upload, back in sequence (reorder.h); about 322 KiB */
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
	int ntun_route; /* tunnel = native: the prefixes it owns (tunroute.h) */
	struct cg_pfx tun_route[CG_TUN_ROUTES];
	uint32_t mix;     /* its sessions' keys in the map (cg_sesskey) */
	uint32_t nsess;   /* its sessions */
	uint64_t alone_ms; /* since when it has none (at first, since it was configured) */
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
	uint64_t paced; /* the pacing rate set on it, bytes/s (pace.h); CG_PACE_NONE: none */
};

/* Forward tables are small: cengarde-nat refuses one over 64 KiB. */
#define FWD_TABLE_MAX 65536
/* A line per client in wireguard_port_file: "65535 NAME\n" (wgport.h). */
#define WGPORT_MAX (sizeof(CG_WGPORT_HEADER) + CG_MAX_CLIENTS * (7 + CG_CLIENT_NAME))

struct server {
	struct cg_config *cfg; /* replaced by a reload */
	const struct cg_run *run;
	/* tunnel = native: one TUN for every client, and which client owns
	 * each address in it (tunroute.h), rebuilt with the clients. */
	int native;
	/* native, tcp_aware: the download's inner TCP retransmissions and
	 * segments without data go marked CG_F_FREE (tcpfree.h); one table for
	 * every session (a flow's hash includes its addresses) */
	struct cg_tcpf tcpf;
	uint32_t flow_seed;
	int tun_fd;
	struct cg_rt rt;
	uint64_t tun_rx, tun_bad, tun_no_route, tun_no_session;
	struct client c[CG_MAX_CLIENTS];
	struct cg_hintidx hx; /* hint -> clients (clients.h) */
	int multi;            /* [client] sections: their names in the logs, the forward table */
	struct cg_cookie_keys ck;
	int ep, tfd;
	uint64_t ticks_missed, tick_max_us; /* timer expirations the tick ran late for, and its longest run */
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
	struct cg_status_writer gw; /* wireguard_port_file */
	struct cg_cpuwin cpu[5];    /* this loop, sw, pw, fw and gw, for "ctl threads" */
	uint64_t next_cpu_ms;
	int pass_written; /* one client: last IP pass handed to pw, -1 none */
	/* Several clients: the forward table (fwdtable.h). */
	int holder;      /* slot holding IP pass, -1: none */
	int fw_holder;   /* the holder in the table last handed on (or found on disk), -1: none */
	int fw_written;  /* a table was handed on, or the one on disk is current */
	size_t fw_len;   /* length of fw_text */
	char fw_text[FWD_TABLE_MAX]; /* that table */
	char fw_next[FWD_TABLE_MAX];
	/* The port WireGuard knows each client by (wgport.h). */
	int gw_dirty;   /* a client's wg_port, or the clients, changed */
	int gw_written; /* gw_text was handed on, or is what the file says */
	size_t gw_len;
	char gw_text[WGPORT_MAX];
	char gw_next[WGPORT_MAX];
	struct cg_ctl ctl;
	struct cg_loader loader;
	int reload_again;
	char config_error[600];
	uint64_t start_ms, next_status_ms, next_sweep_ms;
	struct cg_wgs wgs_up, wgs_down; /* first copies to WireGuard, and its datagrams read: every client (wgsize.h) */

	uint64_t rx_malformed, rx_auth_fail, rx_old, rx_dups, rx_trunc, rx_ctrunc, sessions_full;
	uint64_t rx_hint, rx_no_session; /* no client's hint; DATA for no session */
	uint64_t hellos, hellos_refused, hellos_over; /* sent, of those refusing, not sent (budget) */
	uint64_t rx_junk, rx_short, rx_bad_version; /* read from the junk socket */
	uint64_t wg_pokes, wg_redirects, id_clashes;
	struct cg_ratelimit rl_auth, rl_full, rl_send, rl_local, rl_junk, rl_version, rl_redirect, rl_hint, rl_fw;

	/* Reordering of every session's upload (reorder.h): one pool of held
	 * packets for all of them, allocated once; the sessions holding
	 * packets (indexes into s, cap entries), the only ones whose deadlines
	 * are looked at; and the releases left in this loop pass. */
	struct cg_ro_pool pool;
	struct cg_resc_ring resc; /* DATA sent lately, for NACKs (rescue_kb; n 0: none) */
	struct cg_ro_cfg rocf;
	uint32_t *ro_active;
	uint32_t nactive;
	uint32_t ro_budget;

	struct cg_rxbatch in;
	union cg_ctl_rx rxctl[CG_BATCH]; /* arrival addresses of a listen batch */
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
	/* per listen batch: payloads for WireGuard (the session of each, and
	 * the pool buffer it is in, given back once flush_wg sent it, even when
	 * its session closed meanwhile), and probe replies */
	uint32_t q_sess[CG_BATCH];
	uint16_t q_pool[CG_BATCH];
	int8_t pick[CG_BATCH]; /* per packet of a download batch: its one path (split), or -1 (every carrier) */
	uint8_t reply[CG_BATCH][CG_HDR_LEN + CG_PROBE_BOND_LEN];
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

/* The paths the upload's reorderer waits for: the expected ones, less those
 * that brought no data for reorder_idle_ms (clockwork audit C2). */
static uint16_t ro_rset(const struct server *s, const struct session *S, uint64_t now_ms)
{
	uint16_t m = expect_mask(S, now_ms);

	for (int i = 0; s->cfg->reorder_idle_ms && i < CG_MAX_LINKS; i++)
		if ((m >> i & 1) && cg_arr_idle(&S->rx[i], (uint32_t)(now_ms * 1000), s->cfg->reorder_idle_ms * 1000))
			m &= (uint16_t)~(1u << i);
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

/* The session of a client of h's hint (a few at worst, clients.h) with h's
 * id, for c >= 0 in the chain from c on; -1 when there is none, else its
 * index, and *c its client. */
static int32_t lookup_from(struct server *s, const struct cg_hdr *h, int *c)
{
	for (; *c >= 0; *c = s->hx.next[*c]) {
		int32_t i = cg_idmap_get(&s->ids, cg_sesskey(s->c[*c].mix, h->session));

		if (i >= 0 && s->s[i].client == *c && s->s[i].id == h->session)
			return i;
	}
	return -1;
}

/* The session of packet b (header h), NULL when there is none. One client
 * of h's hint with a session of h's id is the usual case: its MAC is
 * checked later, after the replay window. Two clients sharing the hint
 * may have sessions with the same id (ids travel in the clear: one can
 * pick the other's), and one's probe may carry an id only the other has a
 * session with: then the keys decide, here, and *verified says the MAC
 * was checked. A probe no key of those sessions verifies goes on as one
 * without a session (its client is found by its key). */
static struct session *lookup(struct server *s, const struct cg_hdr *h, const uint8_t *b, size_t len,
			      int *verified)
{
	int c = s->hx.first[h->hint], n = 0, k;
	int32_t one = -1, i;

	if (c < 0)
		return NULL;
	for (k = c; (i = lookup_from(s, h, &k)) >= 0; k = s->hx.next[k]) {
		one = one < 0 ? i : one;
		n++;
	}
	if (!n || (n == 1 && (s->hx.next[c] < 0 || h->type != CG_T_PROBE)))
		return one < 0 ? NULL : &s->s[one];
	for (k = c; (i = lookup_from(s, h, &k)) >= 0; k = s->hx.next[k])
		if (cg_hdr_verify(b, len, s->c[k].key)) {
			*verified = 1;
			return &s->s[i];
		}
	return NULL;
}

/* The port of an address (network order), 0 for another family. */
static uint16_t addr_port(const struct sockaddr_storage *a)
{
	if (a->ss_family == AF_INET)
		return ((const struct sockaddr_in *)a)->sin_port;
	return a->ss_family == AF_INET6 ? ((const struct sockaddr_in6 *)a)->sin6_port : 0;
}

/* The port of a bound socket (network order), 0 when unknown. */
static uint16_t local_port(int fd)
{
	struct sockaddr_storage a;
	socklen_t len = sizeof(a);

	if (getsockname(fd, (struct sockaddr *)&a, &len) < 0)
		return 0;
	return addr_port(&a);
}

/* A socket connected to C's WireGuard with the session buffers, bound to
 * port when it is not 0 and free (else to any other); -1 with errno set. */
static int wg_socket_once(struct server *s, const struct client *C, uint16_t port)
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

/* wg_socket_once, never on WireGuard's own port. The kernel picks a free
 * port from the ephemeral range, and WireGuard's is free while WireGuard is
 * not up: if it listens in that range (the lab's fake one does, on 59301),
 * the socket would hold the port WireGuard needs and talk to itself,
 * sending what the client sends up back down to it. Such a socket stays
 * open while the next one is picked, so that the kernel picks another
 * port, and is closed then. */
static int wg_socket(struct server *s, const struct client *C, uint16_t port)
{
	uint16_t wg = addr_port(&C->wireguard);
	int fd = -1, held = -1, e;

	for (int i = 0; i < 4; i++) {
		fd = wg_socket_once(s, C, i ? 0 : port);
		if (fd < 0 || local_port(fd) != wg)
			break;
		if (held >= 0)
			close(held);
		held = fd;
		fd = -1;
		errno = EADDRINUSE;
	}
	e = errno;
	if (held >= 0) {
		close(held);
		cg_info("%sthe socket to WireGuard got WireGuard's own port (%u); %s", C->tag, ntohs(wg),
			fd >= 0 ? "took another" : "found no other");
	}
	errno = e;
	return fd;
}

/* N, knocking in vain, takes over port, which an older session of its
 * client just freed: WireGuard may still know the client by it, and send
 * there the handshake it starts (wgwatch.h). When the port is no longer
 * free, N keeps its socket. */
static void session_rehome(struct server *s, struct session *N, uint16_t port)
{
	struct client *C = client_of(s, N);
	int fd;

	if (s->native)
		return;
	fd = wg_socket(s, C, port);
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
	s->gw_dirty = 1;
	cg_info("%ssession %08x: takes over the port WireGuard knew its client by", C->tag, N->id);
}

/* ---- the upload's reorderer, per session ---- */

/* Keeps S on the list of sessions holding packets while it holds any. The
 * list is walked backwards, so taking S out (the last one takes its place)
 * never skips one not visited yet. */
static void ro_track(struct server *s, struct session *S)
{
	uint32_t last;

	if (S->ro.held && !S->ro_pos) {
		s->ro_active[s->nactive++] = (uint32_t)(S - s->s);
		S->ro_pos = s->nactive;
	} else if (!S->ro.held && S->ro_pos) {
		last = s->ro_active[--s->nactive];
		s->ro_active[S->ro_pos - 1] = last;
		s->s[last].ro_pos = S->ro_pos;
		S->ro_pos = 0;
	}
}

/* native: a packet of S's upload into the TUN, if it is an IP packet from
 * an address of S's client (tunroute.h: as WireGuard's AllowedIPs). */
static void tun_write(struct server *s, struct session *S, const uint8_t *p, size_t len)
{
	int ver = cg_ip_check(p, len);

	if (!ver) {
		S->not_ip++;
		return;
	}
	if (cg_rt_lookup(&s->rt, ver == 4 ? AF_INET : AF_INET6, cg_ip_src(p, ver)) != S->client) {
		S->spoofed++;
		return;
	}
	if (cg_tun_tx(s->tun_fd, p, len) < 0)
		S->wg_drops++;
}

/* Everything S holds, in order, straight to its WireGuard (its session
 * closes, or reorder was turned off), and its window starts again. Through
 * s->out, which only flush_wg and down_send use, never across this. */
static void ro_flush_session(struct server *s, struct session *S)
{
	struct iovec iov[CG_BATCH];
	uint16_t bufs[CG_BATCH];
	struct cg_ro_out o;
	int m = 0, more = 1, sent;

	while (more) {
		more = cg_ro_flush(&S->ro, &o);
		if (more) {
			iov[m].iov_base = cg_ro_pool_buf(&s->pool, o.buf);
			iov[m].iov_len = o.len;
			memset(&s->out[m].msg_hdr, 0, sizeof(s->out[m].msg_hdr));
			s->out[m].msg_hdr.msg_iov = &iov[m];
			s->out[m].msg_hdr.msg_iovlen = 1;
			bufs[m++] = o.buf;
		}
		if (m == CG_BATCH || (!more && m)) {
			if (s->native) {
				for (int x = 0; x < m; x++)
					tun_write(s, S, iov[x].iov_base, iov[x].iov_len);
				sent = m;
			} else {
				sent = sendmmsg(S->wg_fd, s->out, (unsigned)m, MSG_DONTWAIT);
			}
			if (sent < m)
				S->wg_drops += (uint64_t)(m - (sent < 0 ? 0 : sent));
			while (m)
				cg_ro_pool_put(&s->pool, bufs[--m]);
		}
	}
	ro_track(s, S);
}

static void session_destroy(struct server *s, struct session *S, const char *why, uint64_t now_ms)
{
	struct client *C = client_of(s, S);
	int32_t idx = (int32_t)(S - s->s);
	struct session *N;

	cg_info("%ssession %08x closed: %s", C->tag, S->id, why);
	/* What it holds goes first, in order: its WireGuard socket closes now.
	 * What a listen batch queued for it is dropped below, as before. */
	ro_flush_session(s, S);
	if (S->wg_fd >= 0) {
		epoll_ctl(s->ep, EPOLL_CTL_DEL, S->wg_fd, NULL);
		close(S->wg_fd);
	}
	cg_idmap_del(&s->ids, cg_sesskey(C->mix, S->id));
	free(S->resc_idx);
	S->resc_idx = NULL;
	S->used = 0;
	if (!--C->nsess)
		C->alone_ms = now_ms;
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
	if (C->newest >= 0) {
		C->wg_port = s->s[C->newest].wg_port;
		s->gw_dirty = 1;
	}
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
static void rescue_apply(const struct server *s, struct session *S);

static struct session *session_create(struct server *s, struct client *C, const struct cg_hdr *h,
				      const struct cg_probe_info *pi, const struct sockaddr_storage *from,
				      uint64_t now_ms)
{
	uint32_t id = h->session, key = cg_sesskey(C->mix, id);
	struct session *S = NULL, *old = NULL;
	char a[64];
	int fd, alone;

	/* Another client's session already has this key: a 2^-32 chance. */
	if (cg_idmap_get(&s->ids, key) >= 0) {
		s->id_clashes++;
		if (cg_ratelimit_ok(&s->rl_full, now_ms, 10000))
			cg_warn("%ssession %08x: its key clashes with another client's session, refused", C->tag, id);
		return NULL;
	}
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
	/* WireGuard knows the client by the port of its session's socket. One
	 * that starts while no other of its client is left takes the newest
	 * one's port again: what WireGuard sends still arrives, its own
	 * handshakes included, which a router that came back with its clock
	 * behind needs (wgwatch.h). */
	alone = !C->nsess;
	fd = s->native ? -1 : wg_socket(s, C, alone ? C->wg_port : 0);
	if (fd < 0 && !s->native) {
		cg_err("%ssession %08x: socket to WireGuard: %s", C->tag, id, strerror(errno));
		return NULL;
	}
	memset(S, 0, sizeof(*S));
	cg_gov_reset(&S->gov);
	cg_gov_weights(&S->gov, 0, S->gw);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		cg_cap_reset(&S->cap[i]);
		S->cw[i] = CG_GOV_MAX;
	}
	S->used = 1;
	S->id = id;
	S->client = (int)(C - s->c);
	S->wg_fd = fd;
	S->wg_port = fd >= 0 ? local_port(fd) : 0;
	S->tx_seq = pi->rx_top + CG_SEQ_LEAP;
	S->tx_ctl = pi->rx_top_ctl + CG_SEQ_LEAP;
	S->created_ms = S->last_rx_ms = now_ms;
	S->pass = -1;
	cg_replay_init_marked(&S->replay, pi->tx_next - 1);
	cg_replay_init_marked(&S->ctl, h->seq);
	if (fd >= 0 && cg_epoll_add(s->ep, fd, CG_EV(CG_EV_WG, S - s->s)) < 0) {
		close(fd);
		S->used = 0;
		return NULL;
	}
	cg_idmap_put(&s->ids, key, (int32_t)(S - s->s));
	rescue_apply(s, S);
	/* Its index into the rescue ring; without memory, no rescue for it. */
	if (s->resc.n)
		S->resc_idx = calloc(CG_RESC_IDX, sizeof(*S->resc_idx));
	C->nsess++;
	s->nsess++;
	C->newest = (int32_t)(S - s->s);
	C->wg_port = S->wg_port;
	s->gw_dirty = 1;
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
		cg_split_spread_reset(&S->spread, link);
		cg_gov_link_reset(&S->gov, link);
		cg_cap_reset(&S->cap[link]);
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
	if (ch & CG_PATH_NEW_FAMILY) {
		cg_health_reset(&S->dh[link], now_ms);
		cg_split_spread_reset(&S->spread, link);
		cg_gov_link_reset(&S->gov, link);
		cg_cap_reset(&S->cap[link]);
	}
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

static int path_full(const struct server *s, const struct session *S, unsigned p);

/* up_owd: the probe's trip up this path, timed here, for the client's link
 * health. bond: the probe had a bonding tail, so the reply has one too,
 * saying whether this end reorders (never larger than the probe). */
static void queue_reply(struct server *s, int *nr, struct session *S, unsigned link, const struct cg_hdr *probe,
			uint32_t up_owd, int bond, uint64_t now_us)
{
	size_t plen = bond ? CG_PROBE_BOND_LEN : CG_PROBE_INFO_LEN;
	struct cg_probe_bond pb = cg_split_reply_tail(s->cfg->reorder);
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
	pb.rescue = S->resc_idx != NULL; /* it sends DATA again on a NACK */
	/* but not what is lost on this path now: congestion, or no budget */
	pb.full = pb.rescue && (cg_resc_low(&S->resc_b) || path_full(s, S, link));
	if (bond)
		cg_probe_bond_write(r + CG_HDR_LEN, &pb);
	cg_hdr_write(r, &h, C->key + CG_SIPHASH_KEY_LEN, r + CG_HDR_LEN, plen);
	s->reply_to[*nr] = P->addr;
	s->riov[*nr].iov_base = r;
	s->riov[*nr].iov_len = CG_HDR_LEN + plen;
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

/* A verified probe on P (link of S), with its info and bonding tail (pb,
 * NULL: none, an older client). */
static void on_probe(struct server *s, struct session *S, struct path *P, unsigned link, const struct cg_hdr *h,
		     const struct cg_probe_info *pi, const struct cg_probe_bond *pb, uint64_t now_ms)
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
	/* What split = client follows from the next batch on: whether the
	 * client reorders and asks for a split download, from the newest probe
	 * it sent on any path (not the last to arrive: one sent before a reload
	 * may come later on a slow path), and this path's weight, from this
	 * path's newest (each path is FIFO). A new weight starts the
	 * round-robin even. */
	if (cg_split_tail_newer(&S->tail, h->seq)) {
		S->peer_bond = pb != NULL;
		S->peer_reorders = pb && pb->reorders;
		S->peer_asks = pb && pb->split_me;
		/* Whether it keeps what it sends up, for the upload's rescue. */
		if (S->peer_rescue != (pb && pb->rescue)) {
			S->peer_rescue = pb && pb->rescue;
			rescue_apply(s, S);
		}
	}
	/* This path's losses it will not send again now (CG_BOND_FULL): the
	 * reorderer does not ask for them. */
	if (pb && pb->full)
		S->ro.full |= (uint16_t)(1u << link);
	else
		S->ro.full &= (uint16_t)~(1u << link);
	if (pb && (!P->bond_ok || P->bond_w != pb->weight))
		cg_split_reset(&S->sp);
	P->bond_ok = pb != NULL;
	P->bond_w = pb ? pb->weight : 0;
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
		if (s->native) {
			for (int x = 0; x < m; x++)
				tun_write(s, S, s->out[x].msg_hdr.msg_iov->iov_base, s->out[x].msg_hdr.msg_iov->iov_len);
			continue;
		}
		sent = sendmmsg(S->wg_fd, s->out, (unsigned)m, MSG_DONTWAIT);
		if (sent < m)
			S->wg_drops += (uint64_t)(m - (sent < 0 ? 0 : sent));
	}
}

/* Sends the *nq queued payloads (flush_wg), then gives back the pool
 * buffers they were in: only then, or a packet held meanwhile could take
 * one still queued. */
static void q_flush(struct server *s, int *nq)
{
	if (*nq)
		flush_wg(s, *nq);
	for (int j = 0; j < *nq; j++)
		if (s->q_pool[j] != CG_RO_NOBUF)
			cg_ro_pool_put(&s->pool, s->q_pool[j]);
	*nq = 0;
}

/* Queues a payload for S's WireGuard, after what S has queued already;
 * buf: the pool buffer it is in (CG_RO_NOBUF: in the listen batch, kept
 * until the queue is sent). A full queue goes first: each session's
 * entries still leave in queue order. */
static void q_add(struct server *s, int *nq, struct session *S, uint8_t *payload, size_t len, uint16_t buf)
{
	if (*nq == CG_BATCH)
		q_flush(s, nq);
	s->q_sess[*nq] = (uint32_t)(S - s->s);
	s->q_pool[*nq] = buf;
	s->oiov[*nq][0].iov_base = payload;
	s->oiov[*nq][0].iov_len = len;
	(*nq)++;
}

/* S's two live paths of least delay down (best[0] first), -1: none. */
static void best_paths(const struct session *S, uint64_t now_ms, int best[2])
{
	uint16_t present, live;

	best[0] = best[1] = -1;
	path_masks(S, now_ms, &present, &live);
	for (int p = 0; p < CG_MAX_LINKS; p++) {
		if (!(live >> p & 1) || !S->path[p].used || !S->dh[p].have_owd)
			continue;
		if (best[0] < 0 || cg_owd_diff(S->dh[p].owd, S->dh[best[0]].owd) < 0) {
			best[1] = best[0];
			best[0] = p;
		} else if (best[1] < 0 || cg_owd_diff(S->dh[p].owd, S->dh[best[1]].owd) < 0) {
			best[1] = p;
		}
	}
}

/* One datagram (len bytes at b) down path p of S. */
static int send_path(struct server *s, struct session *S, int p, const uint8_t *b, size_t len)
{
	struct path *P = &S->path[p];
	struct msghdr mh = { 0 };
	struct iovec iov = { .iov_base = (void *)b, .iov_len = len };

	mh.msg_name = &P->addr;
	mh.msg_namelen = cg_addr_len(&P->addr);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	if (P->ctl_len) {
		mh.msg_control = P->ctl.b;
		mh.msg_controllen = P->ctl_len;
	}
	return sendmsg(s->lane[(unsigned)p & (s->nlanes - 1)].fd, &mh, MSG_DONTWAIT) < 0 ? -1 : 0;
}

/* S's reorder settings: the server's, and with rescue_wait_ms the rescue of
 * the upload when the client said it keeps what it sends (rescue.h). */
static void rescue_apply(const struct server *s, struct session *S)
{
	S->rocf = s->rocf;
	S->rocf.rescue_us = s->cfg->reorder && S->peer_rescue ? s->cfg->rescue_wait_ms * 1000 : 0;
}

/* The upload gaps S's reorderer asked for, as NACKs down its live path of
 * least delay. now_ms: the wall clock of the live mask (not the
 * reorderer's 32-bit one). */
static void send_nacks(struct server *s, struct session *S, uint64_t now_ms)
{
	const struct client *C = client_of(s, S);
	struct cg_reorder *r = &S->ro;
	int best[2];

	best_paths(S, now_ms, best);
	/* First asks, then retries (CG_F_RETRY), each on the two fastest paths. */
	for (int list = 0; best[0] >= 0 && list < 2; list++) {
		const uint32_t *seq = list ? r->ask2 : r->ask;
		uint32_t n = list ? r->nask2 : r->nask;

		for (uint32_t i = 0; i < n;) {
			uint8_t pkt[CG_HDR_LEN + CG_NACK_LEN];
			unsigned used;
			size_t plen = cg_nack_write(pkt + CG_HDR_LEN, seq + i, n - i, &used);

			i += used;
			for (int b = 0; b < 2 && best[b] >= 0; b++) {
				struct cg_hdr h = { .type = CG_T_NACK,
						    .flags = list ? CG_F_RETRY : 0,
						    .hint = C->hint,
						    .link = (uint8_t)best[b],
						    .session = S->id,
						    .seq = S->tx_ctl++,
						    .ts = (uint32_t)cg_now_us() };

				cg_hdr_write(pkt, &h, C->key + CG_SIPHASH_KEY_LEN, pkt + CG_HDR_LEN, plen);
				if (!send_path(s, S, best[b], pkt, CG_HDR_LEN + plen))
					S->nacks_sent++;
			}
		}
	}
	r->nask = r->nask2 = 0;
}

/* What S's reorderer may hand on at now (the loop's clock, as its arrival
 * times), queued after what is there: everything with all (a window
 * overflow), else up to the pass's budget; the next pass comes at once for
 * the rest (cg_ro_due_us says 0). rset: the paths the client sends DATA on
 * (expect_mask). A drain the budget cuts ends here for max_burst, which says
 * what one drain gave WireGuard at once. */
static void up_drain(struct server *s, int *nq, struct session *S, uint16_t rset, uint32_t now, int all)
{
	struct cg_ro_out o;

	while ((all || s->ro_budget) && cg_ro_next(&S->ro, &S->rocf, rset, now, &o)) {
		q_add(s, nq, S, cg_ro_pool_buf(&s->pool, o.buf), o.len, o.buf);
		if (s->ro_budget)
			s->ro_budget--;
	}
	cg_ro_burst_end(&S->ro);
	if (S->ro.nask || S->ro.nask2)
		send_nacks(s, S, cg_now_ms());
}

/* A verified first copy of S's DATA h (payload, len bytes) from path link,
 * read at now: queued for WireGuard now, or once what comes before it did
 * (copied into a pool buffer meanwhile), with what it releases after it. */
static void up_deliver(struct server *s, int *nq, struct session *S, const struct cg_hdr *h, uint8_t *payload,
		       size_t len, uint16_t rset, uint32_t now)
{
	struct cg_ro_pkt k = { .seq = h->seq,
			       .ts = h->ts,
			       .t_arr = now,
			       .len = (uint16_t)len,
			       .link = h->link,
			       .flags = h->flags,
			       .wgtype = s->native ? CG_RO_WG_DATA : len ? payload[0] : 0,
			       .prev = (uint8_t)(cg_prev_link(h->flags) + 1),
			       /* free with TCP data: a retransmission (tcp_aware) */
			       .retx = (uint8_t)(s->native && (h->flags & CG_F_FREE) && cg_tcp_data(payload, len)) };
	enum cg_ro_v v;
	uint16_t buf;

	if (!s->cfg->reorder) {
		q_add(s, nq, S, payload, len, CG_RO_NOBUF);
		return;
	}
	v = cg_ro_in(&S->ro, &s->pool, &S->rocf, &k, rset, &buf);
	if (v == CG_RO_AGAIN) {
		/* The window overflowed: the gaps it forces out go, with what was
		 * held behind them, all of it; then the packet is taken. */
		up_drain(s, nq, S, rset, now, 1);
		v = cg_ro_in(&S->ro, &s->pool, &S->rocf, &k, rset, &buf);
	}
	if (v == CG_RO_HOLD)
		memcpy(cg_ro_pool_buf(&s->pool, buf), payload, len);
	else
		q_add(s, nq, S, payload, len, CG_RO_NOBUF); /* NOW (a second AGAIN cannot be: never dropped) */
	up_drain(s, nq, S, rset, now, 0);
	ro_track(s, S);
}

/* Whether path p of S is full, so that what it loses is congestion, for TCP
 * to see, not a loss to send again (rescue.h): its rescue weight
 * (capacity.h's cg_cap_rescue_w, from where its queue will be in a second)
 * is 0. (Delivering 9/10 of its capacity, as a sign, took the rescue off
 * links lossy at random as well; story 013.) */
static int path_full(const struct server *s, const struct session *S, unsigned p)
{
	return cg_cap_rescue_w(&S->cap[p], s->cfg->governor_target_ms * 1000) == 0;
}

/* A verified NACK of S (rescue.h): each sequence it names that the ring
 * still keeps goes again, within S's budget, on its live path of least
 * delay (the delays of one session's paths share the client's clock). */
static void on_nack(struct server *s, struct session *S, uint8_t flags, const uint8_t *payload, size_t plen,
		    uint64_t now_ms)
{
	uint32_t seq[CG_NACK_SEQ_MAX];
	unsigned n = cg_nack_read(seq, CG_NACK_SEQ_MAX, payload, plen);
	int best[2];

	uint32_t gone[CG_NACK_SEQ_MAX];
	unsigned ngone = 0;

	S->nacks++;
	/* The client will not send these again: the reorderer gives them up
	 * now instead of waiting for them. */
	if (flags & CG_F_GONE) {
		for (unsigned i = 0; i < n; i++)
			S->resc_gone_rx += cg_ro_refused(&S->ro, &S->rocf, seq[i], (uint32_t)cg_now_us());
		return;
	}
	if (!S->resc_idx)
		return;
	best_paths(S, now_ms, best);
	for (unsigned i = 0; best[0] >= 0 && i < n; i++) {
		uint16_t len;
		uint8_t went;
		uint8_t *e = (uint8_t *)cg_resc_get(&s->resc, S->resc_idx, S->id, seq[i], &len, &went);

		if (!e) {
			S->resc_miss++;
			gone[ngone++] = seq[i];
			continue;
		}
		if (cg_resc_dup(&s->resc, S->resc_idx, seq[i], (uint32_t)now_ms))
			continue;
		/* Lost where the path is full: congestion, for TCP to see. */
		if (went < CG_MAX_LINKS && path_full(s, S, went)) {
			S->resc_congested++;
			gone[ngone++] = seq[i];
			continue;
		}
		/* A retry goes on the two fastest paths. */
		for (int b = 0; b < ((flags & CG_F_RETRY) ? 2 : 1) && best[b] >= 0; b++) {
			struct path *P = &S->path[best[b]];

			if (!cg_resc_spend(&S->resc_b, len - CG_HDR_LEN)) {
				S->resc_budget++;
				if (!b)
					gone[ngone++] = seq[i];
				break;
			}
			cg_hdr_set_link(e, (uint8_t)best[b]);
			if (send_path(s, S, best[b], e, len) < 0) {
				P->tx_drops++;
				send_failed(s, S, (unsigned)best[b], errno, 1, now_ms);
				continue;
			}
			P->tx_pkts++;
			P->tx_bytes += len - CG_HDR_LEN;
			S->resc_sent++;
		}
	}
	/* What will not come again, said back so the client does not wait. */
	for (unsigned i = 0; best[0] >= 0 && i < ngone;) {
		const struct client *C = client_of(s, S);
		uint8_t pkt[CG_HDR_LEN + CG_NACK_LEN];
		unsigned used;
		size_t glen = cg_nack_write(pkt + CG_HDR_LEN, gone + i, ngone - i, &used);
		struct cg_hdr h = { .type = CG_T_NACK,
				    .flags = CG_F_GONE,
				    .hint = C->hint,
				    .link = (uint8_t)best[0],
				    .session = S->id,
				    .seq = S->tx_ctl++,
				    .ts = (uint32_t)cg_now_us() };

		i += used;
		cg_hdr_write(pkt, &h, C->key + CG_SIPHASH_KEY_LEN, pkt + CG_HDR_LEN, glen);
		if (!send_path(s, S, best[0], pkt, CG_HDR_LEN + glen))
			S->resc_gone_tx++;
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
			struct cg_probe_bond pb;
			int bond = 0;
			struct cg_replay *w;
			struct session *S;
			struct client *C;
			struct path *P;
			struct cg_hdr h;
			int verified = 0, fresh = 0, c;
			uint16_t expect;
			char a[64];

			if (s->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) {
				s->rx_trunc++;
				continue;
			}
			if (cg_hdr_parse(&h, b, len) < 0 ||
			    (h.type != CG_T_DATA && h.type != CG_T_PROBE && h.type != CG_T_NACK) ||
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
			if (h.type == CG_T_PROBE) {
				cg_probe_info_read(&pi, b + CG_HDR_LEN);
				bond = cg_probe_bond_read(&pb, b + CG_HDR_LEN, len - CG_HDR_LEN);
			}
			S = lookup(s, &h, b, len, &verified);
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
			if (h.type == CG_T_NACK) {
				on_nack(s, S, h.flags, b + CG_HDR_LEN, len - CG_HDR_LEN, now_ms);
				continue;
			}
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
				on_probe(s, S, P, h.link, &h, &pi, bond ? &pb : NULL, now_ms);
				queue_reply(s, &nr, S, h.link, &h, now32 - h.ts, bond, now_us);
				/* Its trip up this path, for the reorderer's deadlines;
				 * the path may have changed its rset too. */
				cg_ro_owd(&S->ro, h.link, now32 - h.ts);
				if (s->cfg->reorder && S->ro.held) {
					up_drain(s, &nq, S, ro_rset(s, S, now_ms), now32, 0);
					ro_track(s, S);
				}
				continue;
			}
			/* DATA: to WireGuard wherever it came from; it tells the path
			 * only which address of ours it went to. */
			if (P->used && path_may_move(s, C, P, S->id, h.link, from, 0, now_ms)) {
				P = path_update(s, S, h.link, from, &s->in.msg[i].msg_hdr, now_ms);
				P->last_rx_ms = now_ms;
			}
			/* A split packet comes up its one path: the others do
			 * not miss it (arrival.h). The reorderer waits for
			 * every path of expect all the same. */
			expect = expect_mask(S, now_ms);
			cg_arr_first(&S->arr, S->rx, h.seq, now32, h.link,
				     h.flags & CG_F_SPLIT ? (uint16_t)(1u << h.link) : expect);
			if (!s->native)
				cg_wgw_from_client(&S->wgw, cg_wg_handshake(b + CG_HDR_LEN, len - CG_HDR_LEN), now_ms);
			S->up_pkts++;
			S->up_bytes += len - CG_HDR_LEN;
			cg_wgs_add(&s->wgs_up, b + CG_HDR_LEN, len - CG_HDR_LEN);
			up_deliver(s, &nq, S, &h, b + CG_HDR_LEN, len - CG_HDR_LEN,
				   s->cfg->reorder_idle_ms ? ro_rset(s, S, now_ms) : expect, now32);
		}
		q_flush(s, &nq);
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

/* Where a download batch of S goes: its paths present, live, and those
 * that carry every duplicated packet (health.h); and with the split
 * (split.h), the paths that may take a share, their weights, the weight-0
 * ones, which carry every packet, and what the picks take: guard G4
 * applied as split_spread_copy says. */
struct dpaths {
	uint16_t present, live, carry;
	int split;               /* the download is split (cg_split_active) */
	uint16_t elig;           /* see split_eligible */
	uint16_t dup0;           /* weight 0 */
	struct cg_split_masks k; /* for cg_split_pick and cg_split_dst */
	uint16_t w[CG_MAX_LINKS];
};

/* The paths that may take a share of a split download (split.h): live and
 * carrying, not muted for delay (guard G1), and with a verified packet from
 * the client within CG_SPLIT_STALE_PROBES of the interval it announced on
 * that path (guard G3). One with a weight takes every split packet, none
 * makes the split duplicate everything (guard G2). */
static uint16_t split_eligible_g3(const struct session *S, const struct dpaths *d, uint64_t now_ms, uint16_t *stale)
{
	uint16_t e = 0, st = 0;

	for (int p = 0; p < CG_MAX_LINKS; p++) {
		if (!((d->live & d->carry) >> p & 1) || S->dh[p].state != CG_H_ACTIVE)
			continue;
		if (cg_split_stale(S->path[p].last_rx_ms, now_ms, S->path[p].interval_ms))
			st |= (uint16_t)(1u << p);
		else
			e |= (uint16_t)(1u << p);
	}
	if (stale)
		*stale = st;
	return e;
}

static uint16_t split_eligible(const struct session *S, const struct dpaths *d, uint64_t now_ms)
{
	return split_eligible_g3(S, d, now_ms, NULL);
}

static struct dpaths down_paths(const struct server *s, const struct session *S, uint64_t now_ms)
{
	struct dpaths d;

	memset(&d, 0, sizeof(d));
	path_masks(S, now_ms, &d.present, &d.live);
	d.carry = cg_health_carriers(S->dh, CG_MAX_LINKS, d.present, d.live);
	d.split = cg_split_active(s->cfg->split, S->peer_reorders, S->peer_asks);
	if (!d.split)
		return d;
	d.elig = split_eligible(S, &d, now_ms);
	for (int p = 0; p < CG_MAX_LINKS; p++) {
		/* force: even; client: as the client says, 1 until it does */
		d.w[p] = s->cfg->split == CG_SPLIT_FORCE ? split_equal[p] : S->path[p].bond_ok ? S->path[p].bond_w : 1;
		if (s->cfg->split != CG_SPLIT_FORCE && S->path[p].bond_ok && !d.w[p])
			d.dup0 |= (uint16_t)(1u << p);
	}
	/* With governor, its weights (govern.h); the client's weight-0 paths kept. */
	if (s->cfg->governor)
		for (int p = 0; p < CG_MAX_LINKS; p++)
			d.w[p] = (d.dup0 >> p & 1) ? 0 : S->gw[p];
	/* With capacity_weights, from each path's capacity (capacity.h). */
	if (s->cfg->capacity_weights)
		for (int p = 0; p < CG_MAX_LINKS; p++)
			d.w[p] = (d.dup0 >> p & 1) ? 0 : S->cw[p];
	d.k = cg_split_spread_apply(s->cfg->split_spread_copy,
				    cg_split_spread_out(&S->spread, d.elig, d.dup0, s->cfg->split_spread_ms * 1000), d.carry,
				    d.elig, d.dup0);
	return d;
}

/* What the server does with S's download, for the status: off, force,
 * client (split as the client asks) or duplicating (split = client, but the
 * client does not ask for it, does not reorder, or sends no tail). */
static const char *split_mode(const struct server *s, const struct session *S)
{
	if (s->cfg->split == CG_SPLIT_FORCE)
		return "force";
	if (s->cfg->split != CG_SPLIT_CLIENT)
		return "off";
	return cg_split_active(CG_SPLIT_CLIENT, S->peer_reorders, S->peer_asks) ? "client" : "duplicating";
}

/* Datagram buf from WireGuard as entry m of a batch down S's paths: split
 * among the paths d allows when the download is split, else copied to
 * every carrier. */
static void down_prepare(struct server *s, struct session *S, int m, uint8_t *buf, size_t len, uint64_t now_us,
			 const struct dpaths *d)
{
	const struct client *C = client_of(s, S);
	struct cg_hdr h = { .type = CG_T_DATA, .hint = C->hint, .session = S->id, .ts = (uint32_t)now_us };

	/* Under G4's stress, on its budget: a copy on every carrier. */
	int stress = d->split && cg_sdup(&S->sdup, S->spread.stress, (uint32_t)len, (unsigned)__builtin_popcount(d->carry),
					 s->cfg->split_stress_dup);

	/* Before the header: CG_F_SPLIT is under the MAC. */
	s->pick[m] = (int8_t)(d->split ? cg_split_pick_eta(&S->sp, s->cfg->split_eta ? &S->eta : NULL, now_us,
							   d->k.carriers, stress ? 0 : cg_split_stress_elig(&S->spread, d->k.eligible, s->cfg->split_stress_single), d->k.dup, d->w,
							   s->native ? CG_WGS_DATA : buf[0],
							   (uint32_t)len, s->cfg->split_small, &h.flags)
				       : -1);
	S->down_split += s->pick[m] >= 0;
	S->last_down_ms = now_us / 1000;
	/* The path the previous sequence went on alone, for the client's
	 * reorderer (CG_F_PREV); then this one's, for the next (down_send:
	 * cg_split_dst of the same masks, and with mute_trickle a copied
	 * packet goes on the other live paths too). */
	h.flags |= cg_prev_flags((int)S->prev_only - 1);
	if (s->native && s->cfg->tcp_aware) {
		struct cg_flow f;

		if (cg_flow_of(buf, len, s->flow_seed, &f) && cg_tcpf_free(&s->tcpf, &f, buf, len))
			h.flags |= CG_F_FREE;
	}
	S->prev_only = (uint8_t)(1 + (s->pick[m] < 0 && s->cfg->mute_trickle
					      ? -1
					      : cg_split_only(s->pick[m], d->carry, d->k.dup)));
	h.seq = S->tx_seq++;
	cg_hdr_write(s->hdr[m], &h, C->key + CG_SIPHASH_KEY_LEN, buf, len);
	/* Kept for a NACK (rescue.h): the header, its link patched when sent
	 * again, and the datagram; and its share of the rescue budget. */
	if (S->resc_idx && CG_HDR_LEN + len <= CG_RESC_ENTRY) {
		int only = cg_split_only(s->pick[m], d->carry, d->k.dup);
		uint8_t *e = cg_resc_put(&s->resc, S->resc_idx, S->id, h.seq, (uint16_t)(CG_HDR_LEN + len),
					 only < 0 ? CG_RESC_NOLINK : (uint8_t)only);

		memcpy(e, s->hdr[m], CG_HDR_LEN);
		memcpy(e + CG_HDR_LEN, buf, len);
		/* the budget grows with the path's weight (capacity.h): less as it
		 * nears congestion */
		cg_resc_earn(&S->resc_b,
			     only < 0 ? (uint32_t)len
				      : (uint32_t)((uint64_t)len *
						   cg_cap_rescue_w(&S->cap[only], s->cfg->governor_target_ms * 1000) /
						   CG_CAP_W_ONE),
			     s->cfg->rescue_pct);
	}
	s->oiov[m][0].iov_base = s->hdr[m];
	s->oiov[m][0].iov_len = CG_HDR_LEN;
	s->oiov[m][1].iov_base = buf;
	s->oiov[m][1].iov_len = len;
	S->down_pkts++;
	S->down_bytes += len;
	cg_wgs_add(&s->wgs_down, buf, len);
}

/* Sends the batch of m entries down the paths of S: each on the paths
 * pick[] says (every carrier, or its one path and the weight-0 ones when
 * split, and with split_spread_copy the ones G4 keeps out), and one
 * duplicated entry in mute_trickle on the other live paths. d: as when the
 * entries were picked. */
static void down_send(struct server *s, struct session *S, int m, uint64_t now_ms, const struct dpaths *d)
{
	int order[CG_MAX_LINKS], no = 0;
	uint16_t live = d->live, carry = d->carry;

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
				if (cg_split_dst(s->pick[j], carry, d->k.dup) >> p & 1)
					sel[k++] = j;
		} else if ((live >> p & 1) && s->cfg->mute_trickle) {
			for (int j = 0; j < m; j++)
				if (s->pick[j] < 0 && cg_trickle(&S->dh[p], s->cfg->mute_trickle))
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
		for (int x = 0; x < sent; x++) {
			P->tx_bytes += s->oiov[sel[x]][1].iov_len;
			P->tx_split += s->pick[sel[x]] >= 0;
		}
	}
	S->rot++;
}

static void wg_read(struct server *s, struct session *S)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(S->wg_fd, &s->in), m = 0, redir[CG_BATCH], nr = 0;
		const struct client *C = client_of(s, S);
		struct session *N = NULL;
		struct dpaths d, dn;
		uint64_t now_us, now_ms;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		/* Where the batch goes, once: the split is decided per packet
		 * before its header. */
		d = down_paths(s, S, now_ms);
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
				down_prepare(s, S, m++, s->in.buf[i], len, now_us, &d);
		}
		if (m)
			down_send(s, S, m, now_ms, &d);
		if (nr) {
			/* Handshakes, which the split never splits. */
			dn = down_paths(s, N, now_ms);
			for (int r = 0; r < nr; r++)
				down_prepare(s, N, r, s->in.buf[redir[r]], s->in.msg[redir[r]].msg_len, now_us, &dn);
			down_send(s, N, nr, now_ms, &dn);
			s->wg_redirects += (uint64_t)nr;
			if (cg_ratelimit_ok(&s->rl_redirect, now_ms, 60000))
				cg_info("%ssession %08x: a handshake WireGuard started goes down session %08x, the "
					"client's newest", client_of(s, S)->tag, S->id, N->id);
		}
		if (n < CG_BATCH)
			return;
	}
}

/* native: packets the kernel routed into the TUN, each down the newest
 * session of the client that owns its destination (tunroute.h), a batch
 * per session in the order they were read. */
static void tun_read(struct server *s)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_tun_rx(s->tun_fd, &s->in);
		int32_t who[CG_BATCH];
		uint64_t now_us, now_ms;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		s->tun_rx += (uint64_t)n;
		for (int i = 0; i < n; i++) {
			size_t len = s->in.msg[i].msg_len;
			const uint8_t *b = s->in.buf[i];
			int ver = 0, k;

			who[i] = -1;
			if (len <= CG_MAX_PAYLOAD && !(s->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC))
				ver = cg_ip_check(b, len);
			if (!ver) {
				s->tun_bad++;
				continue;
			}
			k = cg_rt_lookup(&s->rt, ver == 4 ? AF_INET : AF_INET6, cg_ip_dst(b, ver));
			if (k < 0 || !s->c[k].used || !s->c[k].enabled)
				s->tun_no_route++;
			else if (s->c[k].newest < 0)
				s->tun_no_session++;
			else
				who[i] = s->c[k].newest;
		}
		for (int i = 0; i < n; i++) {
			int32_t idx = who[i];
			struct session *S;
			struct dpaths d;
			int m = 0;

			if (idx < 0)
				continue;
			S = &s->s[idx];
			d = down_paths(s, S, now_ms);
			for (int j = i; j < n; j++)
				if (who[j] == idx) {
					down_prepare(s, S, m++, s->in.buf[j], s->in.msg[j].msg_len, now_us, &d);
					who[j] = -1;
				}
			down_send(s, S, m, now_ms, &d);
		}
		if (n < CG_BATCH)
			return;
	}
}

/* ---- housekeeping ---- */

/* Guard G4 (split.h), after health: which candidates of S's download split
 * are within split_spread_ms of the fastest. Logs a path that leaves or
 * comes back. */
static void spread_tick(struct server *s, struct session *S, const char *tag, uint64_t now_ms)
{
	struct dpaths d = down_paths(s, S, now_ms);
	uint16_t changed, stale = 0;
	uint8_t before[CG_MAX_LINKS];

	/* G3, the copies' budget and the rescue's, as decisions (decision.h) */
	if (d.split)
		split_eligible_g3(S, &d, now_ms, &stale);
	for (int p = 0; p < CG_MAX_LINKS; p++)
		cg_dec_edge(&S->dec, &S->dec_stale[p], stale >> p & 1, now_ms, (unsigned)p, CG_DEC_STALE, CG_DEC_FRESH,
			    S->path[p].last_rx_ms ? (int64_t)(now_ms - S->path[p].last_rx_ms) : -1);
	cg_dec_edge(&S->dec, &S->dec_dry, S->sdup.dry, now_ms, CG_DEC_NOLINK, CG_DEC_COPIES_DRY, CG_DEC_COPIES_OK,
		    (int64_t)S->sdup.tokens);
	if (S->resc_idx)
		cg_dec_edge(&S->dec, &S->dec_rfull, cg_resc_low(&S->resc_b), now_ms, CG_DEC_NOLINK,
			    CG_DEC_RESCUE_FULL_ON, CG_DEC_RESCUE_FULL_OFF, (int64_t)S->resc_b.credit);
	cg_decrec_g4_before(&S->spread, before);
	changed = cg_split_spread_eval_r(&S->spread, S->dh, (uint16_t)(d.elig & ~d.dup0), s->cfg->split_spread_ms * 1000,
					 s->cfg->mute_settle_ms, now_ms, s->cfg->split_stress_rejoin);
	cg_decrec_g4(&S->dec, &S->spread, changed, before, &S->dec_stress, &S->dec_recover, now_ms);

	for (int p = 0; changed; p++, changed >>= 1) {
		int32_t ms = S->spread.behind_us[p] / 1000;

		if (!(changed & 1))
			continue;
		if (S->spread.left >> p & 1)
			cg_info("%ssession %08x link %d: leaves the download split, %d ms behind the fastest link "
				"(split_spread_ms %u)",
				tag, S->id, p, ms, s->cfg->split_spread_ms);
		else if (!s->cfg->split_spread_ms)
			cg_info("%ssession %08x link %d: back in the download split, split_spread_ms is 0", tag, S->id,
				p);
		else
			cg_info("%ssession %08x link %d: back in the download split, within %d ms of the fastest link",
				tag, S->id, p, ms > 0 ? ms : 0);
	}
}

/* The governor (govern.h), after health: the download's weights from each
 * path's queue, while the download is split and carries traffic. */
static void gov_tick(struct server *s, struct session *S, uint64_t now_ms)
{
	struct cg_gov_cfg gc = { .target_us = s->cfg->governor_target_ms * 1000, .window_ms = s->cfg->governor_window_ms };
	struct dpaths d;
	uint16_t w0[CG_MAX_LINKS], crossed;

	if (!s->cfg->governor)
		return;
	d = down_paths(s, S, now_ms);
	cg_decrec_gov_before(&S->gov, w0);
	crossed = cg_gov_tick(&S->gov, S->dh, (uint16_t)(d.elig & ~d.dup0),
			      d.split && S->last_down_ms && now_ms - S->last_down_ms < 1000, now_ms, &gc);
	cg_decrec_gov(&S->dec, &S->gov, crossed, w0, now_ms);
	cg_gov_weights(&S->gov, d.dup0, S->gw);
}

/* Each path's download capacity (capacity.h), from what the client says it
 * received on it, and with capacity_weights the split's weights from them
 * (scaled by the governor's, with governor), after the governor's tick. */
static void cap_tick(struct server *s, struct session *S, uint16_t live, uint64_t now_ms)
{
	struct cg_cap_cfg cc = { .window_ms = s->cfg->governor_window_ms, .sat_us = s->cfg->governor_target_ms * 1000 };

	for (int p = 0; p < CG_MAX_LINKS; p++) {
		const struct path *P = &S->path[p];

		if (P->used) {
			uint64_t was = cg_cap_bps(&S->cap[p]);
			unsigned ev = cg_cap_tick(&S->cap[p], now_ms, P->peer_view_ms, P->peer_view.rx, P->tx_pkts,
						  P->tx_bytes, &S->dh[p], &cc);

			cg_decrec_cap(&S->dec, &S->cap[p], (unsigned)p, ev, was, now_ms);
		}
	}
	if (s->cfg->capacity_weights) {
		struct dpaths d = down_paths(s, S, now_ms);

		cg_cap_weights(S->cap, d.dup0, s->cfg->governor ? S->gw : NULL, cc.sat_us / 2, S->cw);
	}
	cg_cap_total_tick(&S->ctot, S->cap, live, cc.sat_us);
	if (s->cfg->split_eta)
		cg_cap_eta(&S->eta, S->cap, live, cc.sat_us / 2, now_ms * 1000);
}

/* SO_MAX_PACING_RATE on a lane's socket (pace.h): fq on the way out paces
 * it. Kernels that take only 32 bits get it capped. */
static void lane_pace_set(struct lane *ln, uint64_t rate)
{
	uint32_t r32 = rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;

	if (setsockopt(ln->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate)) < 0 &&
	    setsockopt(ln->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &r32, sizeof(r32)) < 0)
		return;
	ln->paced = rate;
}

/* With pacing, each lane at the sum of the rates of the paths whose
 * downloads leave from it (down_send: path p from lane p & (lanes - 1)),
 * over every session; a lane none leaves from, or one lane for all, is not
 * paced. Without, as it was set, back to none (a reload turned it off). */
static void pace_lanes(struct server *s)
{
	uint64_t r[CG_MAX_LANES];
	int on = s->cfg->pacing && s->nlanes > 1, any[CG_MAX_LANES];

	for (uint32_t l = 0; l < s->nlanes; l++) {
		r[l] = 0;
		any[l] = 0;
	}
	for (uint32_t i = 0; on && i < s->cap; i++) {
		const struct session *S = &s->s[i];

		if (!S->used)
			continue;
		for (unsigned p = 0; p < CG_MAX_LINKS; p++) {
			unsigned l = p & (s->nlanes - 1);

			if (!S->path[p].used)
				continue;
			r[l] = cg_pace_add(r[l], cg_pace_rate(S->cap[p].have_rate ? cg_cap_bps(&S->cap[p]) : 0));
			any[l] = 1;
		}
	}
	for (uint32_t l = 0; l < s->nlanes; l++) {
		uint64_t rate = on && any[l] ? r[l] : CG_PACE_NONE;

		if (s->lane[l].fd >= 0 && cg_pace_changed(s->lane[l].paced, rate))
			lane_pace_set(&s->lane[l], rate);
	}
}

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
		cg_decrec_health(&S->dec, S->dh, changed, now_ms);
		spread_tick(s, S, tag, now_ms);
		gov_tick(s, S, now_ms);
		cap_tick(s, S, live, now_ms);
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
		if (ln->paced != CG_PACE_NONE)
			cg_json_ms(j, "pace_mbit", ln->paced * 8 / 1000);
		else
			cg_json_null(j, "pace_mbit");
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
	cg_json_u64(j, "ticks_missed", s->ticks_missed); /* the tick ran late (clockwork audit) */
	cg_json_u64(j, "tick_max_us", s->tick_max_us);
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
	cg_json_str(j, "tunnel", s->native ? "native" : "wireguard");
	cg_json_obj(j, "tcp_free");
	cg_json_bool(j, "on", s->native && s->cfg->tcp_aware);
	cg_json_u64(j, "control", s->tcpf.ctl);
	cg_json_u64(j, "retransmissions", s->tcpf.retx);
	cg_json_end(j, '}');
	if (s->native) {
		cg_json_obj(j, "tun");
		cg_json_str(j, "name", s->cfg->tun);
		cg_json_u64(j, "rx", s->tun_rx);
		cg_json_u64(j, "not_ip", s->tun_bad);
		cg_json_u64(j, "no_route", s->tun_no_route);
		cg_json_u64(j, "no_session", s->tun_no_session);
		cg_json_end(j, '}');
	}
	cg_json_obj(j, "wireguard");
	cg_json_u64(j, "pokes", s->wg_pokes);
	cg_json_u64(j, "redirected_handshakes", s->wg_redirects);
	cg_json_end(j, '}');
	cg_json_sizes(j, "sizes", &s->wgs_up, &s->wgs_down);
	/* The pool every session's reorderer holds packets in (each session's
	 * own counters are in its "reorder"). */
	cg_json_obj(j, "reorder_pool");
	cg_json_u64(j, "buffers", s->pool.n);
	cg_json_u64(j, "free", s->pool.nfree);
	cg_json_u64(j, "sessions_holding", s->nactive);
	cg_json_u64(j, "full", s->pool.full);
	cg_json_end(j, '}');
	clients_json(s, j);
	cg_json_arr(j, "sessions");
	for (uint32_t i = 0; i < s->cap; i++) {
		const struct session *S = &s->s[i];
		struct dpaths d;
		uint16_t rset;

		if (!S->used)
			continue;
		rset = ro_rset(s, S, now_ms);
		d = down_paths(s, S, now_ms); /* the download split, as a batch now would take it */
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
		cg_json_u64(j, "download_split", S->down_split);
		/* What the server does with this download, and what the client's
		 * newest probe asked (null: no tail, an older client). */
		cg_json_str(j, "split_mode", split_mode(s, S));
		cg_json_u64(j, "split_spread_ms", s->cfg->split_spread_ms);
		cg_json_obj(j, "stress_dup");
		cg_json_u64(j, "pct", s->cfg->split_stress_dup);
		cg_json_bool(j, "stress", S->spread.stress);
		cg_json_bool(j, "recover", S->spread.recover);
		cg_json_u64(j, "queue_leaves", S->spread.n_qleave);
		cg_json_u64(j, "recoveries", S->spread.n_recover);
		cg_json_u64(j, "stalls", S->spread.n_stall);
		cg_json_u64(j, "packets", S->sdup.pkts);
		cg_json_u64(j, "bytes", S->sdup.bytes);
		cg_json_end(j, '}');
		cg_json_bool(j, "split_spread_copy", s->cfg->split_spread_copy);
		cg_json_bool(j, "governor", s->cfg->governor);
		cg_json_bool(j, "capacity_weights", s->cfg->capacity_weights);
		cg_json_bool(j, "split_eta", s->cfg->split_eta);
		cg_json_obj(j, "split_by"); /* split.h, enum cg_split_by */
		cg_json_u64(j, "eta", S->sp.n_by[CG_SPLIT_BY_ETA]);
		cg_json_u64(j, "wrr", S->sp.n_by[CG_SPLIT_BY_WRR]);
		cg_json_u64(j, "g2", S->sp.n_by[CG_SPLIT_BY_G2]);
		cg_json_u64(j, "all", S->sp.n_by[CG_SPLIT_BY_ALL]);
		cg_json_end(j, '}');
		/* What picks the path of a split packet now (decrec.h): with "eta"
		 * the gov_weight and cap_weight of the paths are not used. */
		if (d.split)
			cg_json_str(j, "weights_used",
				    cg_weights_used(s->cfg->split_eta, &S->eta, (uint16_t)(S->spread.in & d.elig & ~d.dup0),
						    s->cfg->capacity_weights, s->cfg->governor));
		else
			cg_json_null(j, "weights_used");
		/* G4's fastest path in the split (null: none) and the governor's
		 * shortest queue. */
		if (S->spread.lead >= 0)
			cg_json_u64(j, "lead", (uint64_t)S->spread.lead);
		else
			cg_json_null(j, "lead");
		if (s->cfg->governor && S->gov.qmin_us >= 0)
			cg_json_ms(j, "gov_qmin_ms", (uint64_t)S->gov.qmin_us);
		else
			cg_json_null(j, "gov_qmin_ms");
		cg_json_bool(j, "copies_dry", S->sdup.dry);
		cg_json_decisions(j, &S->dec, now_ms);
		/* For a shaper on the router's WireGuard interface (capacity.h). */
		if (cg_cap_shape_bps(&S->ctot))
			cg_json_ms(j, "shape_mbit", cg_cap_shape_bps(&S->ctot) / 1000);
		else
			cg_json_null(j, "shape_mbit");
		if (S->peer_bond) {
			cg_json_bool(j, "client_reorders", S->peer_reorders);
			cg_json_bool(j, "client_asks_split", S->peer_asks);
		} else {
			cg_json_null(j, "client_reorders");
			cg_json_null(j, "client_asks_split");
		}
		cg_json_u64(j, "wireguard_drops", S->wg_drops);
		cg_json_u64(j, "not_ip", S->not_ip);
		cg_json_u64(j, "spoofed", S->spoofed);
		cg_json_u64(j, "wireguard_unanswered", cg_wgw_knocking(&S->wgw, now_ms) ? S->wgw.unanswered : 0);
		cg_json_u64(j, "too_big", S->toobig);
		/* Upload: lost on every link, joint lateness, and the reorderer
		 * (story 012). */
		cg_json_u64(j, "lost_all", S->replay.lost);
		/* The rescue (rescue.h): NACKs taken, sequences sent again, not
		 * kept any more, over budget. */
		cg_json_obj(j, "rescue");
		cg_json_bool(j, "on", S->resc_idx != NULL);
		cg_json_u64(j, "nacks", S->nacks);
		cg_json_u64(j, "sent", S->resc_sent);
		cg_json_u64(j, "missing", S->resc_miss);
		cg_json_u64(j, "over_budget", S->resc_budget);
		cg_json_u64(j, "congested", S->resc_congested);
		/* the upload's: the client keeps what it sends, NACKs sent to it */
		cg_json_bool(j, "client_keeps", S->peer_rescue);
		cg_json_bool(j, "asks", S->rocf.rescue_us != 0);
		cg_json_u64(j, "nacks_sent", S->nacks_sent);
		cg_json_u64(j, "gone_sent", S->resc_gone_tx);
		cg_json_u64(j, "gone_received", S->resc_gone_rx);
		cg_json_end(j, '}');
		cg_json_joint(j, "joint", &S->arr);
		cg_json_reorder(j, "reorder", &S->ro, s->cfg->reorder, NULL);
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
			cg_json_u64(j, "mute_backoff", h->backoff); /* unmute wait = 2 x settle << it */
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
			cg_json_u64(j, "tx_split", P->tx_split);
			if (P->bond_ok)
				cg_json_u64(j, "client_weight", P->bond_w);
			else
				cg_json_null(j, "client_weight");
			/* The governor's weight and the queue it measures (null: off,
			 * or no delay yet). */
			if (s->cfg->governor)
				cg_json_u64(j, "gov_weight", S->gw[p]);
			else
				cg_json_null(j, "gov_weight");
			if (s->cfg->governor && S->gov.l[p].have_queue)
				cg_json_ms(j, "gov_queue_ms", (uint64_t)S->gov.l[p].queue_us);
			else
				cg_json_null(j, "gov_queue_ms");
			if (s->cfg->governor) {
				cg_json_obj(j, "gov_ticks");
				cg_json_u64(j, "down", S->gov.l[p].ticks_down);
				cg_json_u64(j, "up", S->gov.l[p].ticks_up);
				cg_json_u64(j, "hold", S->gov.l[p].ticks_hold);
				cg_json_end(j, '}');
			} else {
				cg_json_null(j, "gov_ticks");
			}
			if (s->cfg->capacity_weights)
				cg_json_u64(j, "cap_weight", S->cw[p]);
			else
				cg_json_null(j, "cap_weight");
			/* What it does in the download split (null: not split now),
			 * and its delay behind the fastest candidate as guard G4 last
			 * saw it. */
			if (d.split)
				cg_json_str(j, "split_share",
					    cg_split_share_name(cg_split_share(&S->spread, (unsigned)p, d.elig, d.dup0,
										 s->cfg->split_spread_ms * 1000)));
			else
				cg_json_null(j, "split_share");
			if (S->spread.behind >> p & 1)
				cg_json_ms_signed(j, "split_behind_ms", S->spread.behind_us[p]);
			else
				cg_json_null(j, "split_behind_ms");
			cg_json_g4(j, &S->spread, (unsigned)p, now_ms);
			cg_json_owd_fresh(j, h, now_ms);
			cg_json_u64(j, "moves", P->moves);
			cg_json_u64(j, "local_errors", P->local_errors);
			cg_json_u64(j, "rx_first", S->rx[p].wins);
			cg_json_u64(j, "rx_duplicate", S->rx[p].dups);
			cg_json_u64(j, "rx_late", S->rx[p].late);
			cg_json_u64(j, "rx_missed", S->rx[p].missed);
			cg_json_ms(j, "rx_lag_ms", cg_lag_us(&S->rx[p]));
			/* Its upload delay behind the fastest path the reorderer
			 * waits for, from the probes and the first copies. */
			cg_json_owd_spread(j, "rx_owd_spread_ms", &S->ro, (unsigned)p, rset);
			cg_json_owd_swing(j, "rx_owd_swing_ms", &S->ro, (unsigned)p);
			cg_json_u64(j, "rx_reorder_late", p < CG_MAX_LINKS ? S->ro.link[p].late : 0);
			cg_json_capacity(j, &S->cap[p], s->cfg->governor_target_ms * 1000, now_ms);
			/* What the split by expected arrival sees of it (null: off, or no rate). */
			if (s->cfg->split_eta && S->eta.l[p].bps) {
				cg_json_ms(j, "eta_base_ms", S->eta.l[p].base_us);
				cg_json_ms(j, "eta_mbit", S->eta.l[p].bps / 1000);
			} else {
				cg_json_null(j, "eta_base_ms");
				cg_json_null(j, "eta_mbit");
			}
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
	char id[16], link[12], ago[24], a[64], l[64];

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

	/* A router gone for longer than a session lasts (powered off, or moved
	 * away) lets go of IP pass: it would block the others for good. */
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		const struct client *C = &s->c[i];

		cand[i].eligible = C->used && C->enabled && C->passthrough &&
				   (C->nsess || now_ms - C->alone_ms < cfg->session_timeout_ms);
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

/* Hands every client's port WireGuard knows to gw when one changed and the
 * file would say something else. */
static void wgport_sync(struct server *s)
{
	const char *names[CG_MAX_CLIENTS];
	uint16_t ports[CG_MAX_CLIENTS];
	struct cg_json j;
	int len, n = 0;

	if (!s->gw_dirty || !s->gw.running)
		return;
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		if (s->c[i].used) {
			names[n] = s->c[i].name;
			ports[n++] = ntohs(s->c[i].wg_port);
		}
	len = cg_wgport_table(s->gw_next, sizeof(s->gw_next), names, ports, n);
	if (len < 0) {
		s->gw_dirty = 0; /* cannot happen: WGPORT_MAX has room for every client */
		return;
	}
	if (s->gw_written && (size_t)len == s->gw_len && !memcmp(s->gw_next, s->gw_text, (size_t)len)) {
		s->gw_dirty = 0;
		return;
	}
	cg_json_init(&j);
	cg_json_raw(&j, "%s", s->gw_next);
	if (cg_status_writer_submit(&s->gw, &j) == 0) {
		memcpy(s->gw_text, s->gw_next, (size_t)len + 1);
		s->gw_len = (size_t)len;
		s->gw_written = 1;
		s->gw_dirty = 0;
	}
	cg_json_free(&j);
}

/* The ports a previous run left, once at start: each client's first session
 * binds its port again, where WireGuard still sends (wgport.h). */
static void wgport_load(struct server *s)
{
	FILE *f = fopen(s->cfg->wireguard_port_file, "r");
	size_t n;

	s->gw_dirty = 1;
	if (!f)
		return;
	n = fread(s->gw_text, 1, sizeof(s->gw_text) - 1, f);
	fclose(f);
	s->gw_text[n] = '\0';
	if (n == sizeof(s->gw_text) - 1 || strlen(s->gw_text) != n)
		return;
	s->gw_len = n;
	s->gw_written = 1;
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		struct client *C = &s->c[i];
		uint16_t port = C->used ? cg_wgport_find(s->gw_text, C->name) : 0;

		if (!port)
			continue;
		C->wg_port = htons(port);
		cg_info("%sWireGuard knew %s by port %u before the restart, as %s says", C->tag,
			s->multi ? "it" : "the client", port, s->cfg->wireguard_port_file);
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
	if (s->gw.running)
		cg_cpuwin_add(&s->cpu[4], now_ms, cg_thread_cpu_ns(s->gw.thread, 0));
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
	if (s->gw.running)
		cg_threads_row(j, s->gw.name, s->gw.tid, cg_thread_cpu_ns(s->gw.thread, 0), cg_cpuwin_permille(&s->cpu[4]));
}

/* What became due in the sessions holding packets while nothing arrived (a
 * deadline, or the rest of a drain the budget cut): after every loop pass,
 * and every tick. */
static void ro_service(struct server *s)
{
	uint64_t now_us, now_ms;
	int nq = 0;

	if (!s->nactive)
		return;
	now_us = cg_now_us();
	now_ms = now_us / 1000;
	for (uint32_t i = s->nactive; i-- > 0;) {
		struct session *S = &s->s[s->ro_active[i]];

		up_drain(s, &nq, S, ro_rset(s, S, now_ms), (uint32_t)now_us, 0);
		ro_track(s, S);
	}
	q_flush(s, &nq);
}

/* The longest the loop may sleep for the reorderer: -1 when no session
 * holds anything, 0 when something may go now, else the ms until the
 * nearest deadline, ceiled. Asked again on every pass: a probe can bring a
 * deadline forward. */
static int ro_timeout(struct server *s)
{
	int32_t due = INT32_MAX;
	uint64_t now_us;

	if (!s->nactive)
		return -1;
	now_us = cg_now_us();
	for (uint32_t i = 0; i < s->nactive && due > 0; i++) {
		const struct session *S = &s->s[s->ro_active[i]];
		int32_t d = cg_ro_due_us(&S->ro, &S->rocf, ro_rset(s, S, now_us / 1000), (uint32_t)now_us);

		if (d < due)
			due = d;
	}
	return cg_due_ms(due);
}

static void tick(struct server *s)
{
	uint64_t now_ms = cg_now_ms();

	ro_service(s);
	if (now_ms >= s->next_cpu_ms)
		threads_sample(s, now_ms);
	health_tick(s, now_ms);
	pace_lanes(s);
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
	wgport_sync(s);
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
		if (s->s[i].used && s->s[i].wg_fd >= 0)
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
	s->lane[0].paced = CG_PACE_NONE;
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
			s->lane[i].paced = CG_PACE_NONE;
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
	C->ntun_route = k->ntun_route;
	memcpy(C->tun_route, k->tun_route, sizeof(C->tun_route));
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
		one.ntun_route = cfg->ntun_route;
		memcpy(one.tun_route, cfg->tun_route, sizeof(one.tun_route));
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
			/* Its slot may go to a new client below: never its IP pass. */
			if (s->holder == i)
				s->holder = -1;
			if (s->fw_holder == i)
				s->fw_holder = -1;
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
		C->alone_ms = now_ms;
		C->pass = C->pass_seen = -1;
		if (cg_random(&C->mix, sizeof(C->mix)) < 0)
			C->mix = (uint32_t)cg_now_us() * 2654435761u;
		client_set(s, C, &list[j]);
		if (s->multi && cfg->tunnel == CG_TUNNEL_NATIVE)
			cg_info("%sclient hint %02x, %d prefixes in the TUN%s", C->tag, C->hint, C->ntun_route,
				C->enabled ? "" : ", disabled");
		else if (s->multi)
			cg_info("%sclient hint %02x, WireGuard at %s%s", C->tag, C->hint, cg_addr_str(&C->wireguard,
				(char[64]){ 0 }, 64), C->enabled ? "" : ", disabled");
	}
	for (int i = 0; i < CG_MAX_CLIENTS; i++) {
		hint[i] = s->c[i].hint;
		used[i] = (uint8_t)s->c[i].used;
	}
	cg_hintidx_build(&s->hx, hint, used, CG_MAX_CLIENTS);
	/* The TUN's routes, from the clients in place (the configuration
	 * refused two with one prefix). */
	memset(&s->rt, 0, sizeof(s->rt));
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		for (int r = 0; s->c[i].used && r < s->c[i].ntun_route; r++)
			cg_rt_add(&s->rt, &s->c[i].tun_route[r], (uint8_t)i);
}

/* Room for max sessions: the session array and the map grow (never
 * shrink) between two batches, where nothing points into them. A smaller
 * max only refuses new sessions. Returns 0, or -1 when out of memory (the
 * old room stays). */
static int sessions_room(struct server *s, uint32_t max)
{
	struct session *ns;
	struct cg_idmap nm;
	uint32_t *na;

	s->max = max;
	if (max <= s->cap)
		return 0;
	na = realloc(s->ro_active, sizeof(*na) * max);
	if (!na) {
		s->max = s->cap;
		return -1;
	}
	s->ro_active = na;
	ns = realloc(s->s, sizeof(*ns) * max);
	if (!ns) {
		s->max = s->cap;
		return -1;
	}
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
	/* The reorderer's knobs apply from the next packet; turned off, what
	 * each session holds goes on at once, in order, and every window
	 * starts again when it is turned back on. */
	s->rocf = cg_ro_cfg_of(next);
	for (uint32_t i = 0; i < s->cap; i++)
		if (s->s[i].used)
			rescue_apply(s, &s->s[i]);
	if (old->split != next->split)
		for (uint32_t i = 0; i < s->cap; i++)
			cg_split_reset(&s->s[i].sp);
	if (old->reorder && !next->reorder) {
		uint32_t held = 0;

		for (uint32_t i = 0; i < s->cap; i++)
			if (s->s[i].used) {
				held += s->s[i].ro.held;
				ro_flush_session(s, &s->s[i]);
			}
		if (held)
			cg_info("reorder off: %u held packets handed on to WireGuard", held);
	}
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
	if (strcmp(old->wireguard_port_file, next->wireguard_port_file)) {
		writer_restart(&s->gw, next->wireguard_port_file, "cg-wgport");
		s->gw_written = 0; /* write it again, there */
	}
	s->gw_dirty = 1; /* clients may have come or gone */
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
	wgport_sync(s); /* the last change, written before the thread ends */
	cg_status_writer_stop(&s->gw);
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
	s->ep = s->tfd = s->jfd = s->tun_fd = -1;
	s->native = cfg->tunnel == CG_TUNNEL_NATIVE;
	if (cg_random(&s->flow_seed, sizeof(s->flow_seed)) < 0)
		s->flow_seed = (uint32_t)cg_now_us();
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
	if (cg_ro_pool_alloc(&s->pool, cfg->reorder_pool_kb) < 0) {
		cg_err("reorder_pool_kb = %u: out of memory", cfg->reorder_pool_kb);
		goto out;
	}
	if (cfg->rescue_kb) {
		uint32_t n = (uint32_t)((uint64_t)cfg->rescue_kb * 1024 / CG_RESC_ENTRY);

		s->resc.mem = malloc((size_t)n * CG_RESC_ENTRY);
		s->resc.sess = calloc(n, sizeof(*s->resc.sess));
		s->resc.seq = calloc(n, sizeof(*s->resc.seq));
		s->resc.len = calloc(n, sizeof(*s->resc.len));
		s->resc.link = calloc(n, sizeof(*s->resc.link));
		s->resc.sent = calloc(n, sizeof(*s->resc.sent));
		if (!s->resc.mem || !s->resc.sess || !s->resc.seq || !s->resc.len || !s->resc.link || !s->resc.sent) {
			cg_err("rescue_kb = %u: out of memory", cfg->rescue_kb);
			goto out;
		}
		s->resc.n = n;
	}
	s->rocf = cg_ro_cfg_of(cfg);

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
	if (s->native) {
		s->tun_fd = cg_tun_open(cfg, err, sizeof(err));
		if (s->tun_fd < 0) {
			cg_err("%s", err);
			goto out;
		}
	}
	if ((s->tun_fd >= 0 && cg_epoll_add(s->ep, s->tun_fd, CG_EV(CG_EV_TUN, 0)) < 0) ||
	    (s->jfd >= 0 && cg_epoll_add(s->ep, s->jfd, CG_EV(CG_EV_JUNK, 0)) < 0) ||
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
	if (cfg->wireguard_port_file[0]) {
		wgport_load(s);
		if (cg_status_writer_start(&s->gw, cfg->wireguard_port_file, "cg-wgport") < 0)
			cg_warn("%s: cannot start the writer thread", cfg->wireguard_port_file);
	}
	if (s->native)
		cg_info("server %s: listening on %s%s, native tunnel on %s (MTU %u; authenticated, not encrypted), "
			"%d client%s, up to %u sessions",
			CG_VERSION, s->laddr, s->pktinfo ? ", replying from each packet's arrival address" : "", cfg->tun,
			cfg->tun_mtu, s->multi ? cfg->nclients : 1, s->multi && cfg->nclients > 1 ? "s" : "", s->max);
	else if (s->multi)
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
		int n, traffic = 0;

		/* Until the reorderer's nearest deadline at most (CG_TICK_MS is
		 * too coarse for it). */
		n = cg_wait(s->ep, ev, 32, busy, last_traffic_us, ro_timeout(s));
		s->ro_budget = CG_RO_PASS_MAX;
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
			case CG_EV_TUN:
				tun_read(s);
				traffic = 1;
				break;
			case CG_EV_WG:
				if (idx < s->cap && s->s[idx].used)
					wg_read(s, &s->s[idx]);
				traffic = 1;
				break;
			case CG_EV_TIMER: {
				uint64_t expirations = 0;

				uint64_t t0;

				if (read(s->tfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
					cg_warn("timerfd: %s", strerror(errno));
				else if (expirations > 1)
					s->ticks_missed += expirations - 1;
				t0 = cg_now_us();
				tick(s);
				t0 = cg_now_us() - t0;
				if (t0 > s->tick_max_us)
					s->tick_max_us = t0;
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
		ro_service(s);
		if (traffic && busy)
			last_traffic_us = cg_now_us();
	}
out:
	cg_status_writer_stop(&s->sw);
	cg_status_writer_stop(&s->pw);
	cg_status_writer_stop(&s->fw);
	wgport_sync(s); /* the last change, written before the thread ends */
	cg_status_writer_stop(&s->gw);
	cg_ctl_close(&s->ctl, s->ep);
	cg_loader_free(&s->loader);
	for (uint32_t i = 0; s->s && i < s->cap; i++)
		if (s->s[i].used && s->s[i].wg_fd >= 0)
			close(s->s[i].wg_fd);
	if (s->tun_fd >= 0)
		close(s->tun_fd);
	lanes_close(s);
	if (s->tfd >= 0)
		close(s->tfd);
	if (s->ep >= 0)
		close(s->ep);
	for (uint32_t i = 0; s->s && i < s->cap; i++)
		free(s->s[i].resc_idx);
	free(s->s);
	free(s->ro_active);
	cg_ro_pool_free(&s->pool);
	free(s->resc.mem);
	free(s->resc.sess);
	free(s->resc.seq);
	free(s->resc.len);
	free(s->resc.link);
	free(s->resc.sent);
	cg_idmap_free(&s->ids);
	cg_config_free(s->cfg);
	free(s->cfg);
	free(s);
	return rc;
}
