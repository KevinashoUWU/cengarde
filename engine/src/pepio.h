/* PEP mode B's sockets (pep.c, story 025): the router's listening sockets
 * and the TCP connections it takes from its LAN, the VPS's connections to
 * the destinations, and the streams between them (stream.h, pep.h,
 * pepcc.h). The client and the server each own one struct cg_pep and hand
 * it what concerns it: epoll events tagged CG_EV_PEP, the stream messages
 * that come out of the tunnel (cg_pep_rx), a service after every loop pass
 * (cg_pep_service), the tick (cg_pep_tick); it hands back the messages to
 * send through the ops (the engine's own send path: copies, FEC, rescue).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_PEPIO_H
#define CG_PEPIO_H

#include <stddef.h>
#include <stdint.h>

#define CG_PEP_LISTEN_IDX 0xFFFF0000u /* CG_EV_PEP's index for the listening sockets (+ 0 v4, + 1 v6) */

struct cg_json;

struct cg_pep_cfg {
	int server;          /* the VPS: connects; else the router: listens */
	uint16_t port;       /* the router's listening port (the firewall's REDIRECT) */
	uint32_t mss;        /* stream bytes per DATA (the tunnel's MTU less the header) */
	uint32_t buf_kb;     /* each stream's buffer per direction, at most */
	uint32_t mem_mb;     /* every stream's buffers together */
	uint32_t max_flows;  /* streams at once */
	uint32_t queue_ms;   /* the queue the middle leg may build (pepcc.h) */
	uint32_t backlog_ms; /* what a stream holds unsent or unwritten, in time at its rate */
	uint32_t dead_s;     /* a stream without progress this long, with something pending: reset */
	uint32_t reo_ms;     /* RACK's window beyond min_rtt / 4: the tunnel's repairs come first */
	uint32_t peers;      /* the server: sessions it may have (their pacers) */
	char cc[16];         /* the VPS's congestion control toward the destination ("" the kernel's) */
};

struct cg_pep_ops {
	void *ctx;
	/* Sends n messages to peer (the server: session index peer whose id is
	 * sid; the router: 0, 0) through the tunnel; copy[i]: on every link
	 * that carries. */
	void (*send)(void *ctx, uint32_t peer, uint32_t sid, uint8_t *const *msg, const uint16_t *len,
		     const uint8_t *copy, int n);
	/* The engine's capacity for the links that carry the data to peer,
	 * bytes/s (0: none), and whether it sees them queued. */
	void (*hint)(void *ctx, uint32_t peer, uint32_t sid, uint64_t *bps, int *full);
};

struct cg_pep;

/* NULL with err set on failure (the router: a listening socket). */
struct cg_pep *cg_pep_new(const struct cg_pep_cfg *cfg, int ep, const struct cg_pep_ops *ops, char *err,
			  size_t errlen);
void cg_pep_free(struct cg_pep *p);
/* The server's sessions may grow on a reload: room for n pacers. 0 or -1. */
int cg_pep_peers(struct cg_pep *p, uint32_t n);
/* An epoll event tagged CG_EV(CG_EV_PEP, idx). */
void cg_pep_event(struct cg_pep *p, uint32_t idx, uint32_t events);
/* A stream message out of the tunnel, from peer (see ops->send). */
void cg_pep_rx(struct cg_pep *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len);
/* After every loop pass: the sockets, the timers, the sends. */
void cg_pep_service(struct cg_pep *p);
/* The epoll timeout it needs, ms: -1 none. */
int cg_pep_timeout(const struct cg_pep *p);
/* Every CG_TICK_MS: the engine's hints. */
void cg_pep_tick(struct cg_pep *p);
/* The server: a session closed, its streams are reset. */
void cg_pep_peer_gone(struct cg_pep *p, uint32_t peer, uint32_t sid);
/* The status: an object "pep". */
void cg_pep_json(const struct cg_pep *p, struct cg_json *j);

#endif
