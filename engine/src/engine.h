/* Shared pieces of the client and server event loops.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_ENGINE_H
#define CG_ENGINE_H

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "config.h"
#include "health.h"
#include "pktinfo.h"
#include "proto.h"
#include "util.h"

#ifndef CG_VERSION
#define CG_VERSION "unknown"
#endif

#define CG_BATCH 64
#define CG_BUF 2048
#define CG_MAX_PAYLOAD (CG_BUF - CG_HDR_LEN) /* largest WireGuard datagram carried */
#define CG_TICK_MS 100
/* Batches read from one socket before going back to epoll, so a busy
 * direction cannot starve the other one or the probes (level-triggered). */
#define CG_MAX_ROUNDS 8

/* epoll tags: kind in the high half, index in the low half. */
enum { CG_EV_SIG = 1, CG_EV_TIMER, CG_EV_NL, CG_EV_WG, CG_EV_LINK, CG_EV_LISTEN, CG_EV_CTL, CG_EV_LOAD, CG_EV_JUNK };
#define CG_EV(kind, idx) (((uint64_t)(kind) << 32) | (uint32_t)(idx))

/* What a run needs besides its configuration: where that came from, to
 * reload it, and the command line, to restart in place. */
struct cg_run {
	const char *path;
	char **argv;
	int sigfd; /* SIGHUP reloads; SIGINT and SIGTERM stop */
	int verbose; /* -v: debug logging, whatever log_level says */
};

/* Loads the configuration in a thread of its own, so that a reload never
 * stalls the event loop: a server given by name waits on DNS. */
struct cg_loader {
	pthread_t thread;
	int efd; /* eventfd, readable once a load is done (CG_EV_LOAD) */
	int busy, joinable;
	const char *path;
	struct cg_config *cfg; /* the result */
	char err[512], warn[2048];
};

int cg_loader_init(struct cg_loader *l); /* 0 or -1 */
/* Starts loading path. Returns -1 while a load is still running. */
int cg_loader_start(struct cg_loader *l, const char *path);
/* On CG_EV_LOAD: returns 1 once the load is done, with *cfg the new
 * configuration (the caller's from now on) or NULL and l->err set; else 0. */
int cg_loader_done(struct cg_loader *l, struct cg_config **cfg);
void cg_loader_free(struct cg_loader *l);

/* Logs the warnings of a configuration load (one per line in warn). */
void cg_log_warnings(const char *path, char *warn);
/* Runs the same command line again in this process, which keeps its PID for
 * procd or systemd; signals stay blocked, so none is lost. Returns only on
 * failure. */
void cg_reexec(char **argv);
/* Undoes the cpu and rt_priority knobs for the calling thread: helper
 * threads must not compete with the event loop. */
void cg_thread_normal(void);

struct cg_rxbatch {
	struct mmsghdr msg[CG_BATCH];
	struct iovec iov[CG_BATCH];
	struct sockaddr_storage from[CG_BATCH];
	uint8_t buf[CG_BATCH][CG_BUF];
};

/* One non-blocking recvmmsg, with room in ctl[CG_BATCH] (when not NULL) for
 * the control messages of each datagram: its arrival address (pktinfo.h).
 * msg_controllen is set again on every call, because the kernel writes back
 * how much it used. Returns the datagram count, 0 or -1. */
static inline int cg_rx_ctl(int fd, struct cg_rxbatch *b, union cg_ctl_rx *ctl)
{
	for (int i = 0; i < CG_BATCH; i++) {
		b->iov[i].iov_base = b->buf[i];
		b->iov[i].iov_len = CG_BUF;
		memset(&b->msg[i].msg_hdr, 0, sizeof(b->msg[i].msg_hdr));
		b->msg[i].msg_hdr.msg_name = &b->from[i];
		b->msg[i].msg_hdr.msg_namelen = sizeof(b->from[i]);
		b->msg[i].msg_hdr.msg_iov = &b->iov[i];
		b->msg[i].msg_hdr.msg_iovlen = 1;
		if (ctl) {
			b->msg[i].msg_hdr.msg_control = ctl[i].b;
			b->msg[i].msg_hdr.msg_controllen = sizeof(ctl[i].b);
		}
	}
	return recvmmsg(fd, b->msg, CG_BATCH, MSG_DONTWAIT, NULL);
}

static inline int cg_rx(int fd, struct cg_rxbatch *b)
{
	return cg_rx_ctl(fd, b, NULL);
}

/* epoll_wait that, with busy_poll_us, keeps polling without sleeping for
 * that long after the last traffic: the next packet is picked up without a
 * wake-up, at the cost of a busy CPU while traffic flows. */
static inline int cg_wait(int ep, struct epoll_event *ev, int max, uint32_t busy_poll_us, uint64_t last_traffic_us)
{
	return epoll_wait(ep, ev, max, busy_poll_us && cg_now_us() - last_traffic_us < busy_poll_us ? 0 : -1);
}

static inline struct cg_hcfg cg_hcfg_of(const struct cg_config *cfg)
{
	return (struct cg_hcfg){ .mute_behind_us = cfg->mute_behind_ms * 1000,
				 .unmute_behind_us = cfg->unmute_behind_ms * 1000,
				 .settle_ms = cfg->mute_settle_ms,
				 .min_active = cfg->min_active_links };
}

int cg_timerfd(unsigned interval_ms);
int cg_random(void *buf, size_t len);
int cg_epoll_add(int ep, int fd, uint64_t tag);
/* Applies the cpu and rt_priority knobs to the calling thread and returns
 * the busy_poll_us to use (0 when polling would starve a single CPU).
 * Failures are warnings: the tunnel runs anyway. */
uint32_t cg_tune(const struct cg_config *cfg);

/* Both take over cfg (calloc'ed) and free it, or the one a reload put in
 * its place, when they return. */
int cg_client_run(struct cg_config *cfg, const struct cg_run *run);
int cg_server_run(struct cg_config *cfg, const struct cg_run *run);

#endif
