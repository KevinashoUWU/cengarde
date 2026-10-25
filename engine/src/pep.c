/* PEP mode B's sockets and streams (story 025): see pepio.h. The decisions
 * live in stream.h (the reliable byte stream), pepcc.h (the rate) and
 * pep.h (the messages, the table, the destinations); this file moves bytes
 * between them and the sockets.
 *
 * Data plane: no allocation per packet (the streams' memory is one pool
 * taken at the start, the segments' rings one per stream at its open), no
 * blocking (every socket is non-blocking, the connects too), no log per
 * packet (counters, and logs rate-limited per event).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/sockios.h>

#include "engine.h"
#include "log.h"
#include "pep.h"
#include "pepcc.h"
#include "pepio.h"
#include "status.h"
#include "stream.h"
#include "util.h"

#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif
#ifndef IP6T_SO_ORIGINAL_DST
#define IP6T_SO_ORIGINAL_DST 80
#endif
#ifndef TCP_NOTSENT_LOWAT
#define TCP_NOTSENT_LOWAT 25
#endif

#define PEP_LOWAT_MIN 32768u   /* bytes a socket may hold unsent (TCP_NOTSENT_LOWAT): its rate for */
#define PEP_LOWAT_MS 100u      /* this long, between these two (a slow destination's kernel */
#define PEP_LOWAT_MAX 524288u  /* hid 0.7 s at 128 KB) */
#define PEP_LAN_RCVBUF 131072  /* the router's LAN sockets' receive buffer (the kernel doubles it) */
#define PEP_INIT_EDGE 262144u  /* the peer's window before its first ACK */
#define PEP_MIN_BACKLOG 65536u   /* what a stream may hold unsent or unwritten, at least */
#define PEP_START_BACKLOG 262144u /* and in its first PEP_START_US: under it a stream that started slow stopped
				   * reading, the LAN socket's small buffer dropped a segment (its window
				   * before the kernel learned the segments' overhead) and the sender waited
				   * out its 200 ms timer (live, protect) */
#define PEP_START_US 2000000u
#define PEP_ACK_US 2000u       /* an ACK at most this late */
#define PEP_ACK_FRAMES 4       /* or after this many DATA */
#define PEP_CONNECT_S 30       /* the VPS gives a connect this long */
#define PEP_READ_PASS 262144u  /* bytes read from one socket per pass, at most */
#define PEP_RATE_US 100000u    /* the window of a stream's rates */
#define PEP_FRAME (2048)
#define PEP_HOLE_BUCKETS 8
#define PEP_CC_LOSSES 1        /* pep_cc auto: segments the destination's leg sent again before BBR (3 left
				* a stream at cubic's 1 % at 100 ms for its first 2-3 s: beyond) */
#define PEP_AGES 16
#define PEP_REO_STEP_US 4000u  /* a duplicate widens RACK's window by this */
#define PEP_REO_MAX_US 100000u /* up to this */
#define PEP_REO_DECAY_US 200u  /* and it narrows by this a tick (2 ms a second), down to pep_reo_ms */

enum { F_FREE = 0, F_OPEN, F_EST };

struct flow {
	uint32_t id;
	uint32_t peer, sid; /* the server: its session */
	int fd;
	uint8_t state;
	uint8_t connecting; /* the server: the connect is in progress */
	uint8_t rd_ready, wr_blocked, wr_shut, est;
	uint8_t open_sent, open_tries;
	uint8_t ack_now, backoff;
	uint8_t reg, hup; /* in the epoll set; a hangup seen while not reading */
	uint8_t dup_seen; /* a duplicate came since the last ACK */
	uint8_t cc_bbr;   /* pep_cc auto: switched to BBR (the destination's leg lost) */
	uint8_t have_owd; /* a DATA came since the last ACK: owd is its least one-way delay */
	int32_t owd;
	uint64_t hole_t;  /* since when the receiver misses something (holes in rx), 0: none */
	/* where a live stream's delay is: when the bytes up to each offset were
	 * read from the socket (sending side) and came in order (receiving side) */
	struct age { uint64_t off, t; } rd[PEP_AGES], io[PEP_AGES];
	uint8_t rd_h, rd_n, io_h, io_n;
	uint8_t family;
	uint16_t port;
	uint8_t addr[16];
	uint32_t ev; /* the epoll events registered */
	uint16_t aseq, ack_pend;
	uint64_t ack_t;
	uint64_t consumed; /* received bytes written to the socket */
	uint64_t adv_edge; /* the edge of the last ACK sent */
	uint64_t open_due, rto_at, progress_t, created_t;
	uint64_t s_bytes, s_t, s_rate; /* bytes delivered to the peer, a window's start, the rate */
	uint64_t w_bytes, w_t, w_rate; /* bytes written to the socket, idem */
	uint64_t up, down;             /* bytes read from and written to the socket */
	uint32_t lowat;                /* the socket's TCP_NOTSENT_LOWAT now */
	struct cg_st_tx tx;
	struct cg_st_rx rx;
	struct cg_st_buf sb, rb;
};

struct peer {
	uint32_t sid;
	uint8_t used, app;
	uint32_t reo_us; /* RACK's window beyond min_rtt / 4: pep_reo_ms, widened by duplicates */
	uint64_t dups;   /* ACKs that said a duplicate came */
	struct cg_pcc cc;
};

struct cg_pep {
	struct cg_pep_cfg cfg;
	struct cg_pep_ops ops;
	struct cg_pcc_cfg ccf;
	int ep;
	int lfd[2];
	struct cg_st_pool pool;
	uint8_t *mem;
	uint32_t *stack;
	struct flow *f;     /* max_flows */
	struct cg_st_seg *segs; /* max_flows x nseg */
	uint32_t nseg;
	uint16_t *gen;      /* the router: each slot's generation */
	uint32_t *used;     /* slots in use, densely */
	uint32_t nused;
	uint32_t *freeslot;
	uint32_t nfree;
	uint32_t rr;
	struct cg_pep_tab tab; /* the server */
	uint64_t *tkey;
	uint32_t *tval;
	struct peer *pr;
	uint32_t npr;
	uint64_t buf_cap, due_us;
	/* the batch of messages to send, all to one peer */
	uint8_t msg[CG_BATCH][PEP_FRAME];
	uint8_t *mptr[CG_BATCH];
	uint16_t mlen[CG_BATCH];
	uint8_t mcopy[CG_BATCH];
	int nmsg;
	uint32_t mpeer, msid;
	uint8_t in_rx; /* in cg_pep_rx: the engine is mid-batch, nothing may be sent now */
	/* counters */
	uint64_t opened, closed, refused[CG_PEP_R_N], rst_tx, rst_lost, rst_rx[CG_PEP_R_N], bad, corrupt, unknown;
	uint64_t up_bytes, down_bytes, data_tx, data_rx, acks_tx, acks_rx, opens_tx, retx, copies, rto_n;
	uint64_t dup_frames, ooo_frames, drop_frames, nomem;
	uint64_t cc_switches; /* pep_cc auto: connections switched to BBR */
	uint8_t cc_auto;      /* pep_cc auto */
	uint64_t holes[PEP_HOLE_BUCKETS]; /* how long the receivers missed something: < 5, 10, 20, 40, 80, 160, 320 ms, more */
	uint64_t sendq[PEP_HOLE_BUCKETS]; /* how long read bytes waited to be sent: < 1, 2, 4, 8, 16, 32, 64 ms, more */
	uint64_t writeq[PEP_HOLE_BUCKETS]; /* how long bytes in order waited to be written: the same */
	struct cg_ratelimit rl_rst, rl_cc, rl_open;
};

static uint64_t ewma_rate(uint64_t *bytes, uint64_t *t, uint64_t rate, uint64_t now)
{
	uint64_t dt = now - *t;

	if (dt < PEP_RATE_US)
		return rate;
	{
		uint64_t r = *bytes * 1000000 / dt;

		rate = rate ? (rate * 3 + r) / 4 : r;
	}
	*bytes = 0;
	*t = now;
	return rate;
}

/* The bucket of a wait of us: < 1, 2, 4, ... 64 ms, more. */
static unsigned age_bucket(uint64_t us)
{
	unsigned b = 0;

	for (uint64_t lim = 1000; b < PEP_HOLE_BUCKETS - 1 && us >= lim; lim *= 2)
		b++;
	return b;
}

static void age_push(struct age *a, uint8_t *h, uint8_t *n, uint64_t off, uint64_t t)
{
	if (*n && a[(*h + *n - 1) % PEP_AGES].off >= off)
		return;
	if (*n == PEP_AGES) { /* full: the oldest goes (its wait is not counted) */
		*h = (uint8_t)((*h + 1) % PEP_AGES);
		(*n)--;
	}
	a[(*h + *n) % PEP_AGES].off = off;
	a[(*h + *n) % PEP_AGES].t = t;
	(*n)++;
}

/* Bytes up to off went on at now: the entries they cover, counted. */
static void age_pop(struct age *a, uint8_t *h, uint8_t *n, uint64_t off, uint64_t now, uint64_t *hist)
{
	while (*n && a[*h].off <= off) {
		hist[age_bucket(now > a[*h].t ? now - a[*h].t : 0)]++;
		*h = (uint8_t)((*h + 1) % PEP_AGES);
		(*n)--;
	}
}

/* ---- the batch ---- */

static void flush(struct cg_pep *p)
{
	if (p->nmsg)
		p->ops.send(p->ops.ctx, p->mpeer, p->msid, p->mptr, p->mlen, p->mcopy, p->nmsg);
	p->nmsg = 0;
}

/* Room for a message to peer: its buffer, PEP_FRAME bytes. NULL in
 * cg_pep_rx when the batch would have to go first: the engine is in the
 * middle of its own receive batch (it is a RST, best effort). */
static uint8_t *slot_for(struct cg_pep *p, uint32_t peer, uint32_t sid)
{
	if ((p->nmsg && (p->mpeer != peer || p->msid != sid)) || p->nmsg == CG_BATCH) {
		if (p->in_rx)
			return NULL;
		flush(p);
	}
	p->mpeer = peer;
	p->msid = sid;
	return p->msg[p->nmsg];
}

static void push(struct cg_pep *p, size_t len, int copy)
{
	len = cg_pep_seal(p->msg[p->nmsg], len);
	p->mptr[p->nmsg] = p->msg[p->nmsg];
	p->mlen[p->nmsg] = (uint16_t)len;
	p->mcopy[p->nmsg] = (uint8_t)copy;
	p->nmsg++;
}

static void send_rst(struct cg_pep *p, uint32_t peer, uint32_t sid, uint32_t id, uint8_t reason)
{
	uint8_t *b = slot_for(p, peer, sid);

	if (!b) {
		p->rst_lost++;
		return;
	}
	push(p, cg_pep_put_rst(b, id, reason), 1);
	p->rst_tx++;
}

/* ---- flows ---- */

static struct peer *peer_of(struct cg_pep *p, const struct flow *f)
{
	struct peer *r = &p->pr[p->cfg.server ? f->peer : 0];

	if (!r->used || r->sid != f->sid) {
		memset(r, 0, sizeof(*r));
		r->used = 1;
		r->app = 1; /* nothing sent yet: what comes first says nothing of the path */
		r->sid = f->sid;
		r->reo_us = p->cfg.reo_ms * 1000;
		cg_pcc_init(&r->cc, cg_now_us());
	}
	return r;
}

/* The events f's socket is watched for. A socket that hung up reports it
 * whatever it is watched for: while f does not read it, it leaves the set
 * (or a full stream would spin the loop). */
static void ev_set(struct cg_pep *p, struct flow *f, uint32_t want)
{
	struct epoll_event e = { .events = want, .data.u64 = CG_EV(CG_EV_PEP, f - p->f) };

	if (f->hup && !want) {
		if (f->reg && epoll_ctl(p->ep, EPOLL_CTL_DEL, f->fd, NULL) == 0)
			f->reg = 0;
		f->ev = 0;
		return;
	}
	if (!f->reg) {
		if (epoll_ctl(p->ep, EPOLL_CTL_ADD, f->fd, &e) == 0) {
			f->reg = 1;
			f->ev = want;
		}
		return;
	}
	if (want == f->ev)
		return;
	if (epoll_ctl(p->ep, EPOLL_CTL_MOD, f->fd, &e) == 0)
		f->ev = want;
}

static struct flow *flow_alloc(struct cg_pep *p)
{
	uint32_t s;
	struct flow *f;

	if (!p->nfree || p->pool.nfree < 4)
		return NULL;
	s = p->freeslot[--p->nfree];
	f = &p->f[s];
	memset(f, 0, sizeof(*f));
	f->fd = -1;
	cg_st_buf_init(&f->sb);
	cg_st_buf_init(&f->rb);
	cg_st_rx_init(&f->rx);
	cg_st_tx_init(&f->tx, p->segs + (size_t)s * p->nseg, p->nseg, PEP_INIT_EDGE);
	f->created_t = f->progress_t = f->s_t = f->w_t = cg_now_us();
	p->used[p->nused++] = s;
	return f;
}

static void flow_free(struct cg_pep *p, struct flow *f, int abort_sock)
{
	uint32_t s = (uint32_t)(f - p->f);

	if (f->fd >= 0) {
		if (abort_sock) {
			struct linger lg = { 1, 0 };

			setsockopt(f->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		}
		if (f->reg)
			epoll_ctl(p->ep, EPOLL_CTL_DEL, f->fd, NULL);
		close(f->fd);
	}
	if (f->state != F_FREE) {
		struct peer *r = &p->pr[p->cfg.server ? f->peer : 0];

		if (r->used && r->sid == f->sid)
			r->cc.inflight -= r->cc.inflight >= f->tx.inflight ? f->tx.inflight : r->cc.inflight;
	}
	cg_st_buf_free(&f->sb, &p->pool);
	cg_st_buf_free(&f->rb, &p->pool);
	if (p->cfg.server)
		cg_pep_tab_del(&p->tab, cg_pep_key(f->sid, f->id));
	else
		p->gen[s] = cg_pep_next_gen(p->gen[s]);
	f->state = F_FREE;
	f->fd = -1;
	for (uint32_t i = 0; i < p->nused; i++)
		if (p->used[i] == s) {
			p->used[i] = p->used[--p->nused];
			break;
		}
	p->freeslot[p->nfree++] = s;
	p->closed++;
}

/* Resets f: a RST to the peer (reason), the socket closed with a RST. */
static void flow_reset(struct cg_pep *p, struct flow *f, uint8_t reason)
{
	send_rst(p, f->peer, f->sid, f->id, reason);
	flow_free(p, f, 1);
}

static int set_int(int fd, int level, int opt, int v)
{
	return setsockopt(fd, level, opt, &v, sizeof(v));
}

static uint8_t reason_of(int err)
{
	switch (err) {
	case ECONNREFUSED:
		return CG_PEP_R_REFUSED;
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ENETDOWN:
		return CG_PEP_R_UNREACHABLE;
	case ETIMEDOUT:
		return CG_PEP_R_TIMEOUT;
	case ECONNRESET:
	case EPIPE:
		return CG_PEP_R_PEER;
	default:
		return CG_PEP_R_ERROR;
	}
}

/* ---- the router: taking connections from the LAN ---- */

static int listen_one(struct cg_pep *p, int family, char *err, size_t errlen)
{
	int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_storage a;
	socklen_t al;
	struct epoll_event e = { .events = EPOLLIN, .data.u64 = CG_EV(CG_EV_PEP, CG_PEP_LISTEN_IDX + (family == AF_INET6)) };

	if (fd < 0) {
		snprintf(err, errlen, "pep: socket: %s", strerror(errno));
		return -1;
	}
	memset(&a, 0, sizeof(a));
	if (family == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)&a;

		s->sin_family = AF_INET;
		s->sin_port = htons(p->cfg.port);
		al = sizeof(*s);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a;

		s->sin6_family = AF_INET6;
		s->sin6_port = htons(p->cfg.port);
		al = sizeof(*s);
		set_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1);
	}
	set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
	/* small, so that a full stream's window reaches the sender soon; set
	 * before listen, the connections inherit it (and the window scale) */
	set_int(fd, SOL_SOCKET, SO_RCVBUF, PEP_LAN_RCVBUF);
	if (bind(fd, (struct sockaddr *)&a, al) < 0 || listen(fd, 128) < 0 ||
	    epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
		snprintf(err, errlen, "pep: listen on port %u (%s): %s", p->cfg.port, family == AF_INET ? "IPv4" : "IPv6",
			 strerror(errno));
		close(fd);
		return -1;
	}
	p->lfd[family == AF_INET6] = fd;
	return 0;
}

/* The connection's original destination: conntrack's (REDIRECT), else
 * the local address (TPROXY). 0, or -1 for one that is not redirected
 * (a connection to the listening port itself). */
static int orig_dst(struct cg_pep *p, int fd, int v6, struct flow *f)
{
	struct sockaddr_storage a;
	socklen_t al = sizeof(a);
	int nat;

	memset(&a, 0, sizeof(a));
	nat = getsockopt(fd, v6 ? SOL_IPV6 : SOL_IP, v6 ? IP6T_SO_ORIGINAL_DST : SO_ORIGINAL_DST, &a, &al) == 0;
	if (!nat) {
		al = sizeof(a);
		if (getsockname(fd, (struct sockaddr *)&a, &al) < 0)
			return -1;
	}
	if (a.ss_family == AF_INET) {
		const struct sockaddr_in *s = (const struct sockaddr_in *)&a;

		f->family = 4;
		memcpy(f->addr, &s->sin_addr, 4);
		f->port = ntohs(s->sin_port);
	} else if (a.ss_family == AF_INET6) {
		const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)&a;

		f->family = 6;
		memcpy(f->addr, &s->sin6_addr, 16);
		f->port = ntohs(s->sin6_port);
	} else {
		return -1;
	}
	if (!nat && f->port == p->cfg.port)
		return -1;
	return 0;
}

static void refuse_fd(struct cg_pep *p, int fd, unsigned reason)
{
	struct linger lg = { 1, 0 };

	setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	close(fd);
	p->refused[reason < CG_PEP_R_N ? reason : CG_PEP_R_ERROR]++;
}

static void accept_some(struct cg_pep *p, int v6)
{
	for (int k = 0; k < 32; k++) {
		int fd = accept4(p->lfd[v6], NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		struct flow *f, tmp;
		struct epoll_event e = { .events = EPOLLIN };

		if (fd < 0)
			return;
		memset(&tmp, 0, sizeof(tmp));
		if (orig_dst(p, fd, v6, &tmp) < 0 || cg_pep_dest_ok(tmp.family, tmp.addr)) {
			refuse_fd(p, fd, CG_PEP_R_DENIED);
			continue;
		}
		if (!(f = flow_alloc(p))) {
			refuse_fd(p, fd, CG_PEP_R_FULL);
			if (cg_ratelimit_ok(&p->rl_open, cg_now_ms(), 10000))
				cg_warn("pep: a connection refused: %s", p->nfree ? "pep_mem_mb is full" : "pep_max_flows streams open");
			continue;
		}
		f->family = tmp.family;
		f->port = tmp.port;
		memcpy(f->addr, tmp.addr, 16);
		f->fd = fd;
		f->state = F_OPEN;
		f->id = cg_pep_id(p->gen[f - p->f], (uint16_t)(f - p->f));
		f->rd_ready = 1;
		set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
		set_int(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PEP_LOWAT_MIN);
		e.data.u64 = CG_EV(CG_EV_PEP, f - p->f);
		if (epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
			flow_free(p, f, 1);
			continue;
		}
		f->ev = EPOLLIN;
		f->reg = 1;
		peer_of(p, f);
		p->opened++;
	}
}

/* ---- the VPS: connecting to the destination ---- */

static void open_in(struct cg_pep *p, uint32_t peer, uint32_t sid, const struct cg_pep_msg *m)
{
	struct sockaddr_storage a;
	socklen_t al;
	struct flow *f;
	struct epoll_event e = { .events = EPOLLOUT };
	int fd, r;
	uint8_t why = (uint8_t)cg_pep_dest_ok(m->family, m->addr);

	if (why) {
		send_rst(p, peer, sid, m->id, why);
		p->refused[why]++;
		return;
	}
	if (!(f = flow_alloc(p))) {
		send_rst(p, peer, sid, m->id, CG_PEP_R_FULL);
		p->refused[CG_PEP_R_FULL]++;
		return;
	}
	f->id = m->id;
	f->peer = peer;
	f->sid = sid;
	f->family = m->family;
	f->port = m->port;
	memcpy(f->addr, m->addr, 16);
	f->state = F_EST;
	f->est = 1;
	f->connecting = 1;
	f->ack_now = 1;
	if (cg_pep_tab_put(&p->tab, cg_pep_key(sid, m->id), (uint32_t)(f - p->f)) < 0) {
		flow_free(p, f, 0);
		send_rst(p, peer, sid, m->id, CG_PEP_R_FULL);
		return;
	}
	memset(&a, 0, sizeof(a));
	if (m->family == 4) {
		struct sockaddr_in *s = (struct sockaddr_in *)&a;

		s->sin_family = AF_INET;
		memcpy(&s->sin_addr, m->addr, 4);
		s->sin_port = htons(m->port);
		al = sizeof(*s);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a;

		s->sin6_family = AF_INET6;
		memcpy(&s->sin6_addr, m->addr, 16);
		s->sin6_port = htons(m->port);
		al = sizeof(*s);
	}
	fd = socket(a.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		flow_reset(p, f, CG_PEP_R_ERROR);
		return;
	}
	f->fd = fd;
	if (p->cfg.cc[0] && !p->cc_auto &&
	    setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, p->cfg.cc, (socklen_t)strlen(p->cfg.cc)) < 0 &&
	    cg_ratelimit_ok(&p->rl_cc, cg_now_ms(), 600000))
		cg_warn("pep: pep_cc %s: %s (the kernel's own instead; modprobe tcp_%s?)", p->cfg.cc, strerror(errno),
			p->cfg.cc);
	set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
	set_int(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PEP_LOWAT_MIN);
	/* what the destination sends while the stream is full waits in this
	 * socket: as much as the stream's own buffer (the kernel doubles it),
	 * not the autotuned 6 MB (a download, 7 MB a connection) */
	set_int(fd, SOL_SOCKET, SO_RCVBUF, (int)p->buf_cap);
	r = connect(fd, (struct sockaddr *)&a, al);
	if (r < 0 && errno != EINPROGRESS) {
		flow_reset(p, f, reason_of(errno));
		return;
	}
	e.data.u64 = CG_EV(CG_EV_PEP, f - p->f);
	if (epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
		flow_reset(p, f, CG_PEP_R_ERROR);
		return;
	}
	f->ev = EPOLLOUT;
	f->reg = 1;
	peer_of(p, f);
	p->opened++;
}

/* ---- messages in ---- */

static struct flow *find(struct cg_pep *p, uint32_t sid, uint32_t id)
{
	if (p->cfg.server) {
		uint32_t s = cg_pep_tab_find(&p->tab, cg_pep_key(sid, id));

		return s == CG_PEP_TAB_EMPTY ? NULL : &p->f[s];
	} else {
		uint32_t s = id & 0xffff;

		if (s >= p->cfg.max_flows || p->f[s].state == F_FREE || p->f[s].id != id)
			return NULL;
		return &p->f[s];
	}
}

static void data_in(struct cg_pep *p, struct flow *f, const struct cg_pep_msg *m, uint64_t now)
{
	uint64_t off = cg_st_unwrap(f->rx.nxt, m->off);
	int fin = !!(m->flags & CG_PEPF_FIN), v;
	unsigned nr;
	uint64_t was = f->rx.nxt;

	p->data_rx++;
	/* the least one-way delay since the last ACK (duplicates too: the
	 * slower copies never win the minimum) */
	{
		int32_t d = (int32_t)((uint32_t)now - m->ts);

		if (!f->have_owd || (int32_t)((uint32_t)d - (uint32_t)f->owd) < 0) {
			f->owd = d;
			f->have_owd = 1;
		}
	}
	v = cg_st_rx_judge(&f->rx, off, m->len, fin, f->consumed + p->buf_cap);
	if (v == CG_ST_RX_DUP) {
		p->dup_frames++;
		f->dup_seen = 1;
		if (!f->ack_pend++)
			f->ack_t = now;
		return;
	}
	if (v == CG_ST_RX_DROP) {
		p->drop_frames++;
		f->ack_now = 1;
		return;
	}
	if (m->len && cg_st_buf_write(&f->rb, &p->pool, off, m->data, m->len) < 0) {
		p->nomem++;
		f->ack_now = 1;
		return;
	}
	nr = f->rx.nr;
	cg_st_rx_add(&f->rx, off, m->len, fin);
	if (f->rx.nxt > was)
		age_push(f->io, &f->io_h, &f->io_n, f->rx.nxt, now);
	if (off > was)
		p->ooo_frames++;
	/* a hole opens, or the last one closes: how long it lasted */
	if (f->rx.nr && !f->hole_t) {
		f->hole_t = now;
	} else if (!f->rx.nr && f->hole_t) {
		uint64_t ms = (now - f->hole_t) / 1000;
		unsigned b = 0;

		for (uint64_t lim = 5; b < PEP_HOLE_BUCKETS - 1 && ms >= lim; lim *= 2)
			b++;
		p->holes[b]++;
		f->hole_t = 0;
	}
	/* at once: a new hole (the sender's RACK wants to know), a hole filled
	 * (its retransmission went: the sender frees and sends), the FIN; the
	 * rest every PEP_ACK_FRAMES frames or PEP_ACK_US. An ACK for every
	 * frame out of order (60 % lost, the microcut) copied on every link
	 * filled the cellular link's download to 2 s */
	if (f->rx.nr > nr || (was < f->rx.nxt && nr) || fin)
		f->ack_now = 1;
	if (!f->ack_pend++)
		f->ack_t = now;
}

/* The timer: rto after the oldest SENT segment's last transmission (a
 * segment sent again restarts its own clock); none with nothing SENT. */
static void rto_arm(struct flow *f, struct peer *r)
{
	uint64_t o = f->tx.inflight ? cg_st_tx_oldest(&f->tx) : 0;

	f->rto_at = o ? o + cg_pcc_rto(&r->cc, f->backoff, r->reo_us) : 0;
}

static void ack_in(struct cg_pep *p, struct flow *f, const struct cg_pep_msg *m, uint64_t now)
{
	struct peer *r = peer_of(p, f);
	struct cg_st_ack a;
	struct cg_st_ackres res;
	uint64_t lost, lq = 0;

	p->acks_rx++;
	/* something was sent again for nothing: wait longer before the next */
	if ((m->flags & CG_PEPF_DUP) && r->reo_us < PEP_REO_MAX_US) {
		r->reo_us += PEP_REO_STEP_US;
		r->dups++;
	}
	cg_pep_ack_of(m, f->tx.una, f->tx.edge, &a);
	cg_st_tx_ack(&f->tx, &a, &res);
	if (m->owd != CG_PEP_NOOWD)
		cg_pcc_owd(&r->cc, &p->ccf, now, m->owd);
	r->cc.inflight -= r->cc.inflight >= res.was_inflight ? res.was_inflight : r->cc.inflight;
	cg_pcc_ack(&r->cc, &p->ccf, now, res.delivered, res.rs.have, res.rs.tx, res.rs.app, res.rs.sent_us, res.rs.dl,
		   res.rs.dla, res.rs.dl_us, res.rs.ss);
	if (res.progress) {
		cg_st_buf_release(&f->sb, &p->pool, f->tx.una < f->tx.tail ? f->tx.una : f->tx.tail);
		f->backoff = 0;
		f->s_bytes += res.freed;
	}
	lost = cg_st_tx_rack(&f->tx, cg_pcc_reo(&r->cc, r->reo_us), &lq);
	if (lost) {
		r->cc.inflight -= r->cc.inflight >= lost ? lost : r->cc.inflight;
		cg_pcc_lost(&r->cc, lost - lq);
	}
	rto_arm(f, r);
}

static void rx_one(struct cg_pep *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len);

void cg_pep_rx(struct cg_pep *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len)
{
	p->in_rx = 1;
	rx_one(p, peer, sid, msg, len);
	p->in_rx = 0;
}

static void rx_one(struct cg_pep *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len)
{
	struct cg_pep_msg m;
	struct flow *f;
	uint64_t now = cg_now_us();

	switch (cg_pep_parse(msg, len, &m)) {
	case 0:
		break;
	case -2:
		p->corrupt++;
		return;
	default:
		p->bad++;
		return;
	}
	if (!p->cfg.server) {
		peer = 0;
		sid = 0;
	}
	if (m.type == CG_PEP_OPEN) {
		if (!p->cfg.server) {
			p->bad++;
			return;
		}
		if ((f = find(p, sid, m.id)))
			f->ack_now = 1;
		else
			open_in(p, peer, sid, &m);
		return;
	}
	f = find(p, sid, m.id);
	if (!f) {
		p->unknown++;
		/* a peer that knows the stream and an end that does not: it restarted */
		if (m.type != CG_PEP_RST && (m.flags & CG_PEPF_EST) && cg_ratelimit_ok(&p->rl_rst, now / 1000, 1))
			send_rst(p, peer, sid, m.id, CG_PEP_R_UNKNOWN);
		return;
	}
	f->progress_t = now;
	switch (m.type) {
	case CG_PEP_DATA:
		if (!p->cfg.server)
			f->est = 1;
		data_in(p, f, &m, now);
		break;
	case CG_PEP_ACK:
		if (!p->cfg.server && f->state == F_OPEN) {
			f->state = F_EST;
			f->est = 1;
		}
		ack_in(p, f, &m, now);
		break;
	case CG_PEP_RST:
		p->rst_rx[m.reason < CG_PEP_R_N ? m.reason : CG_PEP_R_ERROR]++;
		/* all said both ways but the FIN's ACK: a clean close */
		flow_free(p, f, !(f->tx.eof && f->tx.una >= f->tx.tail && cg_st_rx_ended(&f->rx) &&
				  f->consumed >= f->rx.fin));
		break;
	default:
		p->bad++;
	}
}

/* ---- the sockets ---- */

static uint64_t backlog_max(const struct cg_pep *p, const struct flow *f, uint64_t rate, uint64_t now)
{
	return cg_pep_backlog(rate, p->cfg.backlog_ms,
			      now - f->created_t < PEP_START_US ? PEP_START_BACKLOG : PEP_MIN_BACKLOG);
}

/* The unsent bytes f's socket may hold: its write rate for PEP_LOWAT_MS
 * (TCP_NOTSENT_LOWAT follows, so that EPOLLOUT comes at it). */
static uint32_t lowat_of(struct flow *f)
{
	uint64_t l = f->w_rate * PEP_LOWAT_MS / 1000;

	if (l < PEP_LOWAT_MIN)
		l = PEP_LOWAT_MIN;
	if (l > PEP_LOWAT_MAX)
		l = PEP_LOWAT_MAX;
	if (!f->lowat || l > f->lowat + f->lowat / 4 || l + l / 4 < f->lowat) {
		f->lowat = (uint32_t)l;
		setsockopt(f->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &f->lowat, sizeof(f->lowat));
	}
	return f->lowat;
}

/* Reads what the socket has into the stream, as far as it may hold.
 * Returns -1 when the flow was reset. */
static int sock_read(struct cg_pep *p, struct flow *f)
{
	uint32_t got = 0;

	while (f->rd_ready && !f->tx.eof && got < PEP_READ_PASS) {
		uint64_t held = f->tx.tail - f->tx.una, unsent = cg_st_tx_unsent(&f->tx);
		uint64_t room = p->buf_cap > held ? p->buf_cap - held : 0, bl = backlog_max(p, f, f->s_rate, cg_now_us());
		uint32_t crun;
		uint8_t *d;
		ssize_t n;

		if (unsent >= bl)
			break;
		if (room > bl - unsent)
			room = bl - unsent;
		if (!room || !(d = cg_st_buf_at(&f->sb, &p->pool, f->tx.tail, 1, &crun)))
			break;
		if (room > crun)
			room = crun;
		n = recv(f->fd, d, room, 0);
		if (n > 0) {
			f->tx.tail += (uint64_t)n;
			age_push(f->rd, &f->rd_h, &f->rd_n, f->tx.tail, cg_now_us());
			f->up += (uint64_t)n;
			p->up_bytes += (uint64_t)n;
			got += (uint32_t)n;
			continue;
		}
		if (n == 0) {
			f->tx.eof = 1;
			break;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			f->rd_ready = 0;
			break;
		}
		if (errno == EINTR)
			continue;
		flow_reset(p, f, reason_of(errno));
		return -1;
	}
	return 0;
}

/* Writes what came in order to the socket, as far as it takes without
 * holding more than lowat_of unsent. Returns -1 when the flow was reset. */
static int sock_write(struct cg_pep *p, struct flow *f, uint64_t now)
{
	uint64_t was = f->consumed;
	uint32_t lowat;

	if (f->connecting)
		return 0;
	lowat = lowat_of(f);
	for (;;) {
		uint64_t ready = cg_st_rx_ready(&f->rx, f->consumed);
		uint32_t crun;
		int unsent = 0;
		const uint8_t *s;
		ssize_t n;
		uint64_t k;

		if (!ready)
			break;
		if (ioctl(f->fd, SIOCOUTQNSD, &unsent) == 0 && unsent >= (int)lowat) {
			f->wr_blocked = 1;
			break;
		}
		if (!(s = cg_st_buf_at(&f->rb, &p->pool, f->consumed, 0, &crun)))
			break; /* not reached: what is in order is held */
		k = ready < crun ? ready : crun;
		if (k > lowat - (uint32_t)(unsent > 0 ? unsent : 0))
			k = lowat - (uint32_t)(unsent > 0 ? unsent : 0);
		n = send(f->fd, s, k, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			f->consumed += (uint64_t)n;
			f->down += (uint64_t)n;
			f->w_bytes += (uint64_t)n;
			p->down_bytes += (uint64_t)n;
			cg_st_buf_release(&f->rb, &p->pool, f->consumed);
			f->wr_blocked = 0;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			f->wr_blocked = 1;
			break;
		}
		if (n < 0 && errno == EINTR)
			continue;
		flow_reset(p, f, reason_of(n < 0 ? errno : EPIPE));
		return -1;
	}
	if (f->consumed != was) {
		uint64_t e = cg_pep_edge(f->consumed, f->rx.nxt, p->buf_cap, backlog_max(p, f, f->w_rate, now));

		age_pop(f->io, &f->io_h, &f->io_n, f->consumed, now, p->writeq);
		f->progress_t = now;
		/* a window that opened by a quarter of the buffer: tell the sender */
		if (e >= f->adv_edge + p->buf_cap / 4)
			f->ack_now = 1;
	}
	if (cg_st_rx_ended(&f->rx) && f->consumed >= f->rx.fin && !f->wr_shut) {
		shutdown(f->fd, SHUT_WR);
		f->wr_shut = 1;
	}
	return 0;
}

/* The connect finished (the server). */
static void connected(struct cg_pep *p, struct flow *f)
{
	int err = 0;
	socklen_t el = sizeof(err);

	if (getsockopt(f->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
		err = errno;
	if (err) {
		flow_reset(p, f, reason_of(err));
		return;
	}
	f->connecting = 0;
	f->rd_ready = 1;
}

void cg_pep_event(struct cg_pep *p, uint32_t idx, uint32_t events)
{
	struct flow *f;

	if (idx >= CG_PEP_LISTEN_IDX) {
		if (!p->cfg.server && idx - CG_PEP_LISTEN_IDX < 2 && p->lfd[idx - CG_PEP_LISTEN_IDX] >= 0)
			accept_some(p, (int)(idx - CG_PEP_LISTEN_IDX));
		return;
	}
	if (idx >= p->cfg.max_flows || (f = &p->f[idx])->state == F_FREE)
		return;
	if (f->connecting) {
		if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
			connected(p, f);
		return;
	}
	if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
		f->rd_ready = 1;
	if ((events & (EPOLLHUP | EPOLLERR)) && !(f->ev & EPOLLIN))
		f->hup = 1;
	if (events & (EPOLLOUT | EPOLLERR))
		f->wr_blocked = 0;
}

/* ---- the service ---- */

static void send_ack(struct cg_pep *p, struct flow *f)
{
	struct cg_st_ack a;
	uint64_t edge = cg_pep_edge(f->consumed, f->rx.nxt, p->buf_cap, backlog_max(p, f, f->w_rate, cg_now_us()));
	uint8_t *b = slot_for(p, f->peer, f->sid);

	cg_st_rx_ack(&f->rx, edge, ++f->aseq, &a);
	f->adv_edge = edge;
	push(p, cg_pep_put_ack(b, f->id, (uint8_t)((f->est ? CG_PEPF_EST : 0) | (f->dup_seen ? CG_PEPF_DUP : 0)), &a,
			       f->have_owd ? f->owd : CG_PEP_NOOWD),
	     0);
	f->have_owd = 0;
	f->dup_seen = 0;
	f->ack_now = 0;
	f->ack_pend = 0;
	p->acks_tx++;
}

static void send_open(struct cg_pep *p, struct flow *f, uint64_t now)
{
	struct peer *r = peer_of(p, f);
	uint8_t *b = slot_for(p, f->peer, f->sid);

	push(p, cg_pep_put_open(b, f->id, f->family, f->addr, f->port), 1);
	f->open_sent = 1;
	f->open_due = now + cg_pcc_rto(&r->cc, f->open_tries < 6 ? f->open_tries : 6, r->reo_us);
	if (f->open_tries < 255)
		f->open_tries++;
	p->opens_tx++;
}

/* One segment of f, if it has one and its pacer lets it. Returns 1 if it
 * sent. */
static int send_one(struct cg_pep *p, struct flow *f, struct peer *r, uint64_t now)
{
	uint32_t idx, len, sent;
	uint64_t off;
	int fin, copy;
	uint8_t *b;

	if (!cg_pcc_may_repair(&r->cc) || !cg_st_tx_next(&f->tx, p->cfg.mss, &idx, &off, &len, &fin))
		return 0;
	/* new bytes need room in flight; a repair, tokens only */
	if (idx == f->tx.n && !cg_pcc_may(&r->cc, &p->ccf, cg_pcc_reo(&r->cc, r->reo_us)))
		return 0;
	cg_pcc_restart(&r->cc, now);
	copy = idx < f->tx.n && cg_st_tx_seg(&f->tx, idx)->tx >= 2;
	b = slot_for(p, f->peer, f->sid);
	cg_pep_put_data(b, f->id, (uint8_t)((fin ? CG_PEPF_FIN : 0) | (f->est ? CG_PEPF_EST : 0)), off, (uint32_t)now);
	if (len && cg_st_buf_read(&f->sb, &p->pool, off, b + CG_PEP_DATA_HDR, len) < 0)
		return 0; /* not reached: unacknowledged bytes are held */
	push(p, CG_PEP_DATA_HDR + len, copy);
	if (idx < f->tx.n)
		p->retx++;
	else {
		age_pop(f->rd, &f->rd_h, &f->rd_n, off + len, now, p->sendq);
	}
	p->copies += (uint64_t)copy;
	/* app-limited: the peer's senders ran out of bytes lately, or this one
	 * has none left after it (a stream's last bytes, a FIN alone, a live
	 * stream between frames): its sample says what they had, not the path */
	sent = cg_st_tx_sent(&f->tx, idx, off, len, fin, now, r->cc.dl, r->cc.dla, r->cc.dl_us, r->cc.sent,
			     r->app || (!cg_st_tx_unsent(&f->tx) && !f->tx.nlost), r->cc.qnow || r->cc.cong);
	r->cc.inflight += sent;
	cg_pcc_spend(&r->cc, sent ? sent : 1);
	if (!f->rto_at)
		f->rto_at = now + cg_pcc_rto(&r->cc, f->backoff, r->reo_us);
	p->data_tx++;
	(void)now;
	return 1;
}

static void due(struct cg_pep *p, uint64_t t)
{
	if (t && (!p->due_us || t < p->due_us))
		p->due_us = t;
}

/* The timers of f at now. Returns -1 when the flow went. */
static int timers(struct cg_pep *p, struct flow *f, uint64_t now)
{
	struct peer *r = peer_of(p, f);

	if (f->connecting && now - f->created_t > (uint64_t)PEP_CONNECT_S * 1000000) {
		flow_reset(p, f, CG_PEP_R_TIMEOUT);
		return -1;
	}
	if ((cg_st_tx_busy(&f->tx) || f->state == F_OPEN) && p->cfg.dead_s &&
	    now - f->progress_t > (uint64_t)p->cfg.dead_s * 1000000) {
		flow_reset(p, f, CG_PEP_R_TIMEOUT);
		return -1;
	}
	if (f->rto_at && now >= f->rto_at) {
		uint64_t lq = 0, lost = cg_st_tx_rto(&f->tx, now, cg_pcc_rto(&r->cc, f->backoff, r->reo_us), &lq);

		r->cc.inflight -= r->cc.inflight >= lost ? lost : r->cc.inflight;
		cg_pcc_lost(&r->cc, lost - lq);
		if (lost) {
			if (f->backoff < 6)
				f->backoff++;
			p->rto_n++;
		}
		rto_arm(f, r);
	}
	if (!p->cfg.server && f->state == F_OPEN && (!f->open_sent || now >= f->open_due))
		send_open(p, f, now);
	return 0;
}

void cg_pep_service(struct cg_pep *p)
{
	uint64_t now = cg_now_us();

	p->due_us = 0;
	for (uint32_t i = 0; i < p->nused;) {
		struct flow *f = &p->f[p->used[i]];
		uint32_t want;

		if (timers(p, f, now) < 0 || sock_read(p, f) < 0 || sock_write(p, f, now) < 0)
			continue; /* freed: used[i] is another flow now */
		if (f->ack_pend || f->ack_now) {
			if (f->ack_now || f->ack_pend >= PEP_ACK_FRAMES || now - f->ack_t >= PEP_ACK_US)
				send_ack(p, f);
			else
				due(p, f->ack_t + PEP_ACK_US);
		}
		/* done both ways, the FIN's ACK sent */
		if (cg_st_tx_done(&f->tx) && f->wr_shut && !f->ack_pend) {
			flow_free(p, f, 0);
			continue;
		}
		if (f->connecting) {
			want = EPOLLOUT;
		} else {
			uint64_t held = f->tx.tail - f->tx.una;
			int can = !f->tx.eof && held < p->buf_cap &&
				  cg_st_tx_unsent(&f->tx) < backlog_max(p, f, f->s_rate, now);

			if (can && !(f->ev & EPOLLIN)) {
				f->rd_ready = 1; /* it may have data already: read it next pass */
				f->hup = 0;
			}
			want = (can ? EPOLLIN : 0) | (f->wr_blocked ? EPOLLOUT : 0);
		}
		ev_set(p, f, want);
		f->s_rate = ewma_rate(&f->s_bytes, &f->s_t, f->s_rate, now);
		f->w_rate = ewma_rate(&f->w_bytes, &f->w_t, f->w_rate, now);
		due(p, f->rto_at);
		if (!p->cfg.server && f->state == F_OPEN)
			due(p, f->open_due);
		i++;
	}
	/* the sends: a segment per flow per round, while the pacers let them */
	for (uint32_t i = 0; i < p->npr; i++)
		if (p->pr[i].used)
			cg_pcc_refill(&p->pr[i].cc, &p->ccf, now);
	for (int any = 1; any && p->nused;) {
		any = 0;
		for (uint32_t k = 0; k < p->nused; k++) {
			struct flow *f = &p->f[p->used[(p->rr + k) % p->nused]];

			if (f->state != F_FREE && send_one(p, f, peer_of(p, f), now))
				any = 1;
		}
	}
	p->rr++;
	flush(p);
	/* what is left to send waits for the pacer (or for ACKs: no timer) */
	for (uint32_t i = 0; i < p->nused; i++) {
		struct flow *f = &p->f[p->used[i]];
		struct peer *r = peer_of(p, f);
		uint32_t idx, len;
		uint64_t off;
		int fin;
		struct cg_st_tx t = f->tx; /* cg_st_tx_next may move lost_from: a copy */

		if (cg_st_tx_next(&t, p->cfg.mss, &idx, &off, &len, &fin) &&
		    r->cc.inflight < cg_pcc_cwnd(&r->cc, &p->ccf, cg_pcc_reo(&r->cc, r->reo_us)))
			due(p, now + cg_pcc_wait_us(&r->cc));
	}
	/* tokens left and room in flight with nothing to send: the samples
	 * that follow say what the senders had, not the path */
	for (uint32_t i = 0; i < p->npr; i++) {
		struct peer *r = &p->pr[i];

		if (r->used)
			r->app = r->cc.tokens > 0 && r->cc.inflight < cg_pcc_cwnd(&r->cc, &p->ccf, cg_pcc_reo(&r->cc, r->reo_us));
	}
}

int cg_pep_timeout(const struct cg_pep *p)
{
	uint64_t now;

	if (!p->due_us)
		return -1;
	now = cg_now_us();
	if (p->due_us <= now)
		return 0;
	return (int)((p->due_us - now + 999) / 1000);
}

/* pep_cc auto: a connection to a destination starts with the kernel's own
 * congestion control (cubic: no BBR ProbeRTT, which every 10 s held a live
 * stream to 4 packets a round trip for 200 ms, 50-140 ms of delay) and goes
 * to BBR once its leg loses (a segment sent again: cubic took
 * 1 % of loss at 100 ms for congestion, story 024). */
static void cc_auto(struct cg_pep *p, struct flow *f)
{
	struct tcp_info ti;
	socklen_t tl = sizeof(ti);

	if (f->cc_bbr || f->connecting || f->fd < 0)
		return;
	memset(&ti, 0, sizeof(ti));
	if (getsockopt(f->fd, IPPROTO_TCP, TCP_INFO, &ti, &tl) < 0 || ti.tcpi_total_retrans < PEP_CC_LOSSES)
		return;
	f->cc_bbr = 1;
	if (setsockopt(f->fd, IPPROTO_TCP, TCP_CONGESTION, "bbr", 3) == 0)
		p->cc_switches++;
	else if (cg_ratelimit_ok(&p->rl_cc, cg_now_ms(), 600000))
		cg_warn("pep: pep_cc auto: bbr: %s (modprobe tcp_bbr?)", strerror(errno));
}

void cg_pep_tick(struct cg_pep *p)
{
	if (p->cc_auto)
		for (uint32_t i = 0; i < p->nused; i++)
			cc_auto(p, &p->f[p->used[i]]);
	for (uint32_t i = 0; i < p->npr; i++) {
		struct peer *r = &p->pr[i];
		uint64_t bps = 0;
		int full = 0;

		if (!r->used)
			continue;
		p->ops.hint(p->ops.ctx, i, r->sid, &bps, &full);
		cg_pcc_hint(&r->cc, bps, full);
		if (r->reo_us > p->cfg.reo_ms * 1000 + PEP_REO_DECAY_US)
			r->reo_us -= PEP_REO_DECAY_US;
		else
			r->reo_us = p->cfg.reo_ms * 1000;
	}
}

void cg_pep_peer_gone(struct cg_pep *p, uint32_t peer, uint32_t sid)
{
	for (uint32_t i = 0; i < p->nused;) {
		struct flow *f = &p->f[p->used[i]];

		if (f->peer == peer && f->sid == sid) {
			flow_free(p, f, 1);
			continue;
		}
		i++;
	}
	if (peer < p->npr && p->pr[peer].sid == sid)
		p->pr[peer].used = 0;
}

int cg_pep_peers(struct cg_pep *p, uint32_t n)
{
	struct peer *q;

	if (n <= p->npr)
		return 0;
	q = realloc(p->pr, (size_t)n * sizeof(*q));
	if (!q)
		return -1;
	memset(q + p->npr, 0, (size_t)(n - p->npr) * sizeof(*q));
	p->pr = q;
	p->npr = n;
	return 0;
}

struct cg_pep *cg_pep_new(const struct cg_pep_cfg *cfg, int ep, const struct cg_pep_ops *ops, char *err,
			  size_t errlen)
{
	struct cg_pep *p = calloc(1, sizeof(*p));
	uint32_t nch, tsz;

	if (!p) {
		snprintf(err, errlen, "pep: out of memory");
		return NULL;
	}
	p->cfg = *cfg;
	p->ops = *ops;
	p->ep = ep;
	p->lfd[0] = p->lfd[1] = -1;
	p->cc_auto = cfg->server && !strcmp(cfg->cc, "auto");
	p->buf_cap = (uint64_t)cfg->buf_kb * 1024;
	if (p->buf_cap > CG_ST_MAXWIN)
		p->buf_cap = CG_ST_MAXWIN;
	p->ccf.queue_us = cfg->queue_ms * 1000;
	p->ccf.mss = cfg->mss;
	/* a segment per mss of the buffer, and some for small writes */
	p->nseg = (uint32_t)(p->buf_cap / (cfg->mss > 256 ? cfg->mss / 2 : 128)) + 64;
	nch = (uint32_t)((uint64_t)cfg->mem_mb * 1024 * 1024 / CG_ST_CHUNK);
	p->mem = malloc((size_t)nch * CG_ST_CHUNK); /* untouched pages stay free */
	p->stack = malloc((size_t)nch * sizeof(*p->stack));
	p->f = calloc(cfg->max_flows, sizeof(*p->f));
	p->segs = malloc((size_t)cfg->max_flows * p->nseg * sizeof(*p->segs));
	p->gen = calloc(cfg->max_flows, sizeof(*p->gen));
	p->used = calloc(cfg->max_flows, sizeof(*p->used));
	p->freeslot = calloc(cfg->max_flows, sizeof(*p->freeslot));
	for (tsz = 16; tsz < 2 * cfg->max_flows; tsz *= 2)
		;
	p->tkey = calloc(tsz, sizeof(*p->tkey));
	p->tval = calloc(tsz, sizeof(*p->tval));
	if (!nch || !p->mem || !p->stack || !p->f || !p->segs || !p->gen || !p->used || !p->freeslot || !p->tkey ||
	    !p->tval || cg_pep_peers(p, cfg->server ? (cfg->peers ? cfg->peers : 1) : 1) < 0) {
		snprintf(err, errlen, "pep: out of memory (pep_mem_mb %u, pep_max_flows %u)", cfg->mem_mb, cfg->max_flows);
		cg_pep_free(p);
		return NULL;
	}
	cg_st_pool_init(&p->pool, p->mem, p->stack, nch);
	cg_pep_tab_init(&p->tab, p->tkey, p->tval, tsz);
	for (uint32_t i = 0; i < cfg->max_flows; i++) {
		p->freeslot[i] = cfg->max_flows - 1 - i;
		p->gen[i] = 1;
		p->f[i].fd = -1;
	}
	p->nfree = cfg->max_flows;
	if (!cfg->server) {
		if (listen_one(p, AF_INET, err, errlen) < 0) {
			cg_pep_free(p);
			return NULL;
		}
		if (listen_one(p, AF_INET6, err, errlen) < 0) {
			cg_warn("%s: IPv4 only", err);
			err[0] = '\0';
		}
	}
	return p;
}

void cg_pep_free(struct cg_pep *p)
{
	if (!p)
		return;
	while (p->nused)
		flow_free(p, &p->f[p->used[0]], 1);
	for (int i = 0; i < 2; i++)
		if (p->lfd[i] >= 0) {
			epoll_ctl(p->ep, EPOLL_CTL_DEL, p->lfd[i], NULL);
			close(p->lfd[i]);
		}
	free(p->mem);
	free(p->stack);
	free(p->f);
	free(p->segs);
	free(p->gen);
	free(p->used);
	free(p->freeslot);
	free(p->tkey);
	free(p->tval);
	free(p->pr);
	free(p);
}

void cg_pep_json(const struct cg_pep *p, struct cg_json *j)
{
	uint64_t txb = 0, rxb = 0;

	cg_json_obj(j, "pep");
	cg_json_u64(j, "flows", p->nused);
	cg_json_u64(j, "opened", p->opened);
	cg_json_u64(j, "closed", p->closed);
	cg_json_obj(j, "refused");
	for (unsigned r = 1; r < CG_PEP_R_N; r++)
		if (p->refused[r])
			cg_json_u64(j, cg_pep_reason(r), p->refused[r]);
	cg_json_end(j, '}');
	cg_json_obj(j, "rst_rx");
	for (unsigned r = 0; r < CG_PEP_R_N; r++)
		if (p->rst_rx[r])
			cg_json_u64(j, cg_pep_reason(r), p->rst_rx[r]);
	cg_json_end(j, '}');
	cg_json_u64(j, "rst_tx", p->rst_tx);
	cg_json_u64(j, "rst_lost", p->rst_lost);
	cg_json_u64(j, "bad", p->bad);
	cg_json_u64(j, "corrupt", p->corrupt); /* tags that did not match: wrong rebuilds of the tunnel's repairs */
	cg_json_u64(j, "unknown", p->unknown);
	cg_json_u64(j, "sock_rx_bytes", p->up_bytes);
	cg_json_u64(j, "sock_tx_bytes", p->down_bytes);
	cg_json_u64(j, "data_tx", p->data_tx);
	cg_json_u64(j, "data_rx", p->data_rx);
	cg_json_u64(j, "acks_tx", p->acks_tx);
	cg_json_u64(j, "acks_rx", p->acks_rx);
	cg_json_u64(j, "opens_tx", p->opens_tx);
	cg_json_u64(j, "retx", p->retx);
	cg_json_u64(j, "copies", p->copies);
	cg_json_u64(j, "rto", p->rto_n);
	cg_json_u64(j, "dup_frames", p->dup_frames);
	cg_json_u64(j, "ooo_frames", p->ooo_frames);
	cg_json_u64(j, "drop_frames", p->drop_frames);
	cg_json_u64(j, "nomem", p->nomem);
	if (p->cc_auto)
		cg_json_u64(j, "cc_to_bbr", p->cc_switches);
	cg_json_arr(j, "holes_ms"); /* < 5, 10, 20, 40, 80, 160, 320, more */
	for (unsigned b = 0; b < PEP_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->holes[b]);
	cg_json_end(j, ']');
	cg_json_arr(j, "sendq_ms"); /* < 1, 2, 4, 8, 16, 32, 64, more */
	for (unsigned b = 0; b < PEP_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->sendq[b]);
	cg_json_end(j, ']');
	cg_json_arr(j, "writeq_ms"); /* < 1, 2, 4, 8, 16, 32, 64, more */
	for (unsigned b = 0; b < PEP_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->writeq[b]);
	cg_json_end(j, ']');
	for (uint32_t i = 0; i < p->nused; i++) {
		const struct flow *f = &p->f[p->used[i]];

		txb += f->tx.tail > f->tx.una ? f->tx.tail - f->tx.una : 0;
		rxb += f->rx.nxt > f->consumed ? f->rx.nxt - f->consumed : 0;
	}
	cg_json_u64(j, "held_tx_bytes", txb);
	cg_json_u64(j, "held_rx_bytes", rxb);
	cg_json_u64(j, "mem_bytes", (uint64_t)cg_st_pool_used(&p->pool) * CG_ST_CHUNK);
	cg_json_u64(j, "mem_peak_bytes", (uint64_t)p->pool.peak * CG_ST_CHUNK);
	cg_json_arr(j, "pacers");
	for (uint32_t i = 0; i < p->npr; i++) {
		const struct cg_pcc *c = &p->pr[i].cc;

		if (!p->pr[i].used)
			continue;
		cg_json_obj(j, NULL);
		cg_json_u64(j, "peer", i);
		cg_json_u64(j, "bw_bps", cg_pcc_bw(c) * 8);
		cg_json_u64(j, "rate_bps", cg_pcc_rate(c) * 8);
		cg_json_u64(j, "hint_bps", c->hint * 8);
		cg_json_bool(j, "hint_full", c->hint_full);
		cg_json_ms(j, "min_rtt_ms", c->min_rtt);
		cg_json_ms(j, "srtt_ms", c->srtt);
		cg_json_u64(j, "inflight", c->inflight);
		cg_json_u64(j, "cwnd", cg_pcc_cwnd(c, &p->ccf, cg_pcc_reo(c, p->pr[i].reo_us)));
		cg_json_str(j, "mode", c->mode == CG_PCC_STARTUP_M ? "startup" : c->mode == CG_PCC_DRAIN_M ? "drain" : "probe");
		cg_json_ms(j, "reo_ms", p->pr[i].reo_us);
		if (c->have_owd)
			cg_json_ms_signed(j, "owd_queue_ms", (int32_t)((uint32_t)c->owd_last - (uint32_t)c->owd_base));
		cg_json_u64(j, "dup_acks", p->pr[i].dups);
		cg_json_u64(j, "congested", c->n_cong);
		cg_json_u64(j, "cuts", c->n_cut);
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_end(j, '}');
}
