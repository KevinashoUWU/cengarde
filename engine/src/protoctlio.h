/* The protocontroller's sockets (protoctl.c, story 025): the router's listening sockets
 * and the TCP connections it takes from its LAN, the VPS's connections to
 * the destinations, and the streams between them (stream.h, protoctl.h,
 * streamcc.h). The client and the server each own one struct cg_pctl and hand
 * it what concerns it: epoll events tagged CG_EV_PCTL, the stream messages
 * that come out of the tunnel (cg_pctl_rx), a service after every loop pass
 * (cg_pctl_service), the tick (cg_pctl_tick); it hands back the messages to
 * send through the ops (the engine's own send path: copies, FEC, rescue).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PROTOCTLIO_H
#define CG_PROTOCTLIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define CG_PCTL_LISTEN_IDX 0xFFFF0000u /* CG_EV_PCTL's index for the listening sockets (+ 0 v4, + 1 v6, + 2 the VPS's A) */
#define CG_PCTL_MID 0x01000000u        /* CG_EV_PCTL's index of a flow's middle socket (variant A): slot | this */
enum { CG_PCTL_NO = 0, CG_PCTL_A, CG_PCTL_B };

struct cg_json;

struct cg_pctl_cfg {
	int server;          /* the VPS: connects; else the router: listens */
	int mode;            /* the router: CG_PCTL_A (plain TCP in the middle) or CG_PCTL_B (a stream); the VPS takes both */
	struct sockaddr_storage a_peer; /* the router, variant A: the VPS's end of the tunnel and port */
	struct sockaddr_storage a_bind; /* the VPS: where variant A's connections come (its tunnel address, port) */
	unsigned a_len;      /* the VPS: the tunnel's prefix length; only its addresses may connect there */
	char mid_cc[16];     /* variant A: the middle leg's congestion control for what this end sends ("" the kernel's) */
	uint16_t port;       /* the router's listening port (the firewall's REDIRECT) */
	uint32_t mss;        /* stream bytes per DATA (the tunnel's MTU less the header) */
	uint32_t buf_kb;     /* each stream's buffer per direction, at most */
	uint32_t mem_mb;     /* every stream's buffers together */
	uint32_t max_flows;  /* streams at once */
	uint32_t queue_ms;   /* the queue the middle leg may build (streamcc.h) */
	uint32_t backlog_ms; /* what a stream holds unsent or unwritten, in time at its rate */
	uint32_t dead_s;     /* a stream without progress this long, with something pending: reset */
	uint32_t reo_ms;     /* RACK's window beyond min_rtt / 4: the tunnel's repairs come first */
	int offer;           /* with subflow (cg_pctl_rack off): the engine's offer (streamcc.h's cg_scc_offer) */
	uint32_t code_ms;    /* the tunnel's code horizon for what this end sends (rescue_code_ms with fec; 0: no code):
			      * how far duplicates may widen that window (streamcc.h's cg_scc_reo_cap) */
	uint32_t peers;      /* the server: sessions it may have (their pacers) */
	char cc[16];         /* the VPS's congestion control toward the destination ("" the kernel's) */
};

#define CG_PCTL_SPREAD_MAX_US 1000000u

/* The spread of the links a peer's data takes, us: the largest delay behind
 * the fastest link (health.h's behind_us: path and queue) among those of set
 * whose delay is known; 0 with none. What the engine's split may reorder a
 * stream by: RACK waiting less takes for lost what is only queued on a
 * slower link (cell4: queues of 300-590 ms on the small links, bursts of
 * up to 1435 retransmissions, nearly all of them duplicates, and the
 * estimator inflated by the "lost" bytes counted twice). */
static inline uint32_t cg_pctl_spread(const int32_t *behind_us, uint16_t known, uint16_t set, unsigned n)
{
	uint32_t s = 0;

	for (unsigned i = 0; i < n && i < 16; i++)
		if ((set & known) >> i & 1 && behind_us[i] > 0 && (uint32_t)behind_us[i] > s)
			s = (uint32_t)behind_us[i];
	return s < CG_PCTL_SPREAD_MAX_US ? s : CG_PCTL_SPREAD_MAX_US;
}

struct cg_pctl_ops {
	void *ctx;
	/* Sends n messages to peer (the server: session index peer whose id is
	 * sid; the router: 0, 0) through the tunnel; copy[i]: on every link
	 * that carries. */
	void (*send)(void *ctx, uint32_t peer, uint32_t sid, uint8_t *const *msg, const uint16_t *len,
		     const uint8_t *copy, int n);
	/* The engine's capacity for the links that carry the data to peer,
	 * bytes/s (0: none), and whether it sees them queued. */
	void (*hint)(void *ctx, uint32_t peer, uint32_t sid, uint64_t *bps, int *full);
	/* The tunnel's repair horizon for what this end sends to peer now, us
	 * (task #77): how long a hole may wait at the receiver for the code's
	 * repairs (while it sends them) and for the rescue's NACK (while the
	 * rescue is on); 0: neither (RACK alone). NULL: 0. */
	uint32_t (*horizon)(void *ctx, uint32_t peer, uint32_t sid);
};

struct cg_pctl;

/* NULL with err set on failure (the router: a listening socket). */
struct cg_pctl *cg_pctl_new(const struct cg_pctl_cfg *cfg, int ep, const struct cg_pctl_ops *ops, char *err,
			  size_t errlen);
void cg_pctl_free(struct cg_pctl *p);
/* The server's sessions may grow on a reload: room for n pacers. 0 or -1. */
int cg_pctl_peers(struct cg_pctl *p, uint32_t n);
/* An epoll event tagged CG_EV(CG_EV_PCTL, idx). */
void cg_pctl_event(struct cg_pctl *p, uint32_t idx, uint32_t events);
/* A stream message out of the tunnel, from peer (see ops->send), that came
 * by link (the server's path index); CG_PCTL_NOLINK: rebuilt by the tunnel's
 * codes, or not known (no delay sample: linkowd.h). */
void cg_pctl_rx(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, unsigned link);
/* The rescue's NACK named msg (len bytes), a stream message this end sent
 * to peer and kept (rescue.h), on link (the one link it went on, the
 * server's path index; CG_PCTL_NOLINK: copied, or not known): its loss goes
 * to its stream (a DATA: marked lost, sent again at the next service; an
 * ACK: a fresh one). The link is only counted for now (the losses each
 * link had, for a view the engine and the streams share later). Returns
 * CG_PCTL_NACK_* (protoctl.h): NOT or RESEND, the rescue sends the packet
 * again as before; DATA or ACK, it does not, and answers CG_F_GONE. sure
 * (rescue.h's cg_resc_sure, story 029): how sure the engine is that a DATA
 * was lost; CG_PCTL_UNSURE, it is only a hint, and RACK decides (in the
 * field what the tunnel gave up came later); CG_PCTL_SURE_ASKED, the
 * tunnel's NACK alone (protocontroller_nack yes); CG_PCTL_SURE_LINK, its
 * one link lost it as well (subflow.h); CG_PCTL_SURE_LATER, its link lost
 * it but a code may rebuild it: a suspect, lost at the engine's repair
 * horizon (and a round trip) unless delivered first. */
enum { CG_PCTL_UNSURE, CG_PCTL_SURE_ASKED, CG_PCTL_SURE_LINK, CG_PCTL_SURE_LATER };
#define CG_PCTL_LINKS 16      /* links counted (CG_MAX_LINKS) */
#define CG_PCTL_NOLINK 0xffu  /* as rescue.h's CG_RESC_NOLINK */
int cg_pctl_nack(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, uint8_t link,
		 int sure);
/* RACK over the mix of the links: on (1, the default), or off (0) when the
 * engine's subflows find each link's losses (subflow.h, story 029): a
 * segment sent before the newest one delivered is not lost because it went
 * on a slower link, and the timer stays as the backstop. */
void cg_pctl_rack(struct cg_pctl *p, int on);
/* After every loop pass: the sockets, the timers, the sends. */
void cg_pctl_service(struct cg_pctl *p);
/* The epoll timeout it needs, ms: -1 none. */
int cg_pctl_timeout(const struct cg_pctl *p);
/* Every CG_TICK_MS: the engine's hints. */
void cg_pctl_tick(struct cg_pctl *p);
/* The server: a session closed, its streams are reset. */
void cg_pctl_peer_gone(struct cg_pctl *p, uint32_t peer, uint32_t sid);
/* The status: an object "protocontroller". */
void cg_pctl_json(const struct cg_pctl *p, struct cg_json *j);

#endif
