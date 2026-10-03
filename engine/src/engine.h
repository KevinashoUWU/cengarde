/* Shared pieces of the client and server event loops.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_ENGINE_H
#define CG_ENGINE_H

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "config.h"
#include "proto.h"

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
enum { CG_EV_SIG = 1, CG_EV_TIMER, CG_EV_NL, CG_EV_WG, CG_EV_LINK, CG_EV_LISTEN };
#define CG_EV(kind, idx) (((uint64_t)(kind) << 32) | (uint32_t)(idx))

struct cg_rxbatch {
	struct mmsghdr msg[CG_BATCH];
	struct iovec iov[CG_BATCH];
	struct sockaddr_storage from[CG_BATCH];
	uint8_t buf[CG_BATCH][CG_BUF];
};

/* One non-blocking recvmmsg. Returns the datagram count, 0 or -1. */
static inline int cg_rx(int fd, struct cg_rxbatch *b)
{
	for (int i = 0; i < CG_BATCH; i++) {
		b->iov[i].iov_base = b->buf[i];
		b->iov[i].iov_len = CG_BUF;
		memset(&b->msg[i].msg_hdr, 0, sizeof(b->msg[i].msg_hdr));
		b->msg[i].msg_hdr.msg_name = &b->from[i];
		b->msg[i].msg_hdr.msg_namelen = sizeof(b->from[i]);
		b->msg[i].msg_hdr.msg_iov = &b->iov[i];
		b->msg[i].msg_hdr.msg_iovlen = 1;
	}
	return recvmmsg(fd, b->msg, CG_BATCH, MSG_DONTWAIT, NULL);
}

int cg_timerfd(unsigned interval_ms);
int cg_random(void *buf, size_t len);
int cg_epoll_add(int ep, int fd, uint64_t tag);

int cg_client_run(const struct cg_config *cfg, int sigfd);
int cg_server_run(const struct cg_config *cfg, int sigfd);

#endif
