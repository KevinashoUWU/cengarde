/* the protocontroller's sockets and streams (story 025): see protoctlio.h. The decisions
 * live in stream.h (the reliable byte stream), streamcc.h (the rate) and
 * protoctl.h (the messages, the table, the destinations); this file moves bytes
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
#include "linkowd.h"
#include "log.h"
#include "protoctl.h"
#include "streamcc.h"
#include "protoctlio.h"
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

#define PCTL_LOWAT_MIN 32768u   /* bytes a socket may hold unsent (TCP_NOTSENT_LOWAT): its rate for */
#define PCTL_LOWAT_MS 100u      /* this long, between these two (a slow destination's kernel */
#define PCTL_LOWAT_MAX 524288u  /* hid 0.7 s at 128 KB) */
#define PCTL_LAN_RCVBUF 131072  /* the router's LAN sockets' receive buffer (the kernel doubles it) */
#define PCTL_INIT_EDGE 262144u  /* the peer's window before its first ACK */
#define PCTL_MIN_BACKLOG 65536u   /* what a stream may hold unsent or unwritten, at least */
#define PCTL_START_BACKLOG 262144u /* and in its first PCTL_START_US: under it a stream that started slow stopped
				   * reading, the LAN socket's small buffer dropped a segment (its window
				   * before the kernel learned the segments' overhead) and the sender waited
				   * out its 200 ms timer (live, protect) */
#define PCTL_START_US 2000000u
#define PCTL_ACK_US 2000u       /* an ACK at most this late */
#define PCTL_ACK_FRAMES 4       /* or after this many DATA */
#define PCTL_CONNECT_S 30       /* the VPS gives a connect this long */
#define PCTL_READ_PASS 262144u  /* bytes read from one socket per pass, at most */
#define PCTL_RATE_US 100000u    /* the window of a stream's rates */
#define PCTL_FRAME (2048)
#define PCTL_HOLE_BUCKETS 8
#define PCTL_CC_LOSSES 1        /* protocontroller_cc auto: segments the destination's leg sent again before BBR (3 left
				* a stream at cubic's 1 % at 100 ms for its first 2-3 s: beyond) */
#define PCTL_AGES 16
#define CG_PCTL_AHDR_MAX 20    /* variant A's header: magic, family, port, address */
#define PCTL_AMAGIC 0xA5
#define PCTL_REO_STEP_US 4000u  /* a duplicate widens RACK's window by this */
#define PCTL_REO_DECAY_US 200u  /* and it narrows by this a tick (2 ms a second), down to protocontroller_reo_ms;
				 * up to reo_cap (streamcc.h's cg_scc_reo_cap: the code's horizon) */

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
	uint8_t cc_bbr;   /* protocontroller_cc auto: switched to BBR (the destination's leg lost) */
	uint8_t have_owd; /* a DATA came since the last ACK: owd is its least one-way delay */
	int32_t owd;
	struct cg_lowd_ack la; /* and by link (linkowd.h): the ACK says the worst of the links that carried */
	uint64_t hole_t;  /* since when the receiver misses something (holes in rx), 0: none */
	/* where a live stream's delay is: when the bytes up to each offset were
	 * read from the socket (sending side) and came in order (receiving side) */
	struct age { uint64_t off, t; } rd[PCTL_AGES], io[PCTL_AGES];
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
	/* variant A (relay): fd is the LAN's (router) or the destination's (VPS)
	 * socket, mfd the middle TCP through the tunnel; sb carries fd -> mfd,
	 * rb mfd -> fd */
	uint8_t relay, mreg, mhup, mconn, hdr_n, hdr_want, m_rd, m_wb;
	uint32_t mev;
	int mfd;
	uint8_t hdr[CG_PCTL_AHDR_MAX];
	struct rdir { uint64_t in, out; uint8_t eof, shut, nomem; } d[2];
};

struct peer {
	uint32_t sid;
	uint8_t used, app;
	uint32_t reo_us; /* RACK's window beyond min_rtt / 4: protocontroller_reo_ms, widened by duplicates */
	uint64_t dups;   /* ACKs that said a duplicate came */
	struct cg_scc cc;
	struct cg_lowd lo; /* what each link adds to this peer's messages' delay, as they come (linkowd.h) */
};

struct cg_pctl {
	struct cg_pctl_cfg cfg;
	struct cg_pctl_ops ops;
	struct cg_scc_cfg ccf;
	int ep;
	int lfd[3]; /* the router's v4 and v6; the VPS's variant A */
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
	struct cg_pctl_tab tab; /* the server */
	uint64_t *tkey;
	uint32_t *tval;
	struct peer *pr;
	uint32_t npr;
	uint64_t buf_cap, due_us;
	uint32_t reo_cap; /* how far duplicates widen RACK's window (cg_scc_reo_cap) */
	/* the batch of messages to send, all to one peer */
	uint8_t msg[CG_BATCH][PCTL_FRAME];
	uint8_t *mptr[CG_BATCH];
	uint16_t mlen[CG_BATCH];
	uint8_t mcopy[CG_BATCH];
	int nmsg;
	uint32_t mpeer, msid;
	uint8_t in_rx; /* in cg_pctl_rx: the engine is mid-batch, nothing may be sent now */
	/* counters */
	uint64_t opened, closed, refused[CG_PCTL_R_N], rst_tx, rst_lost, rst_rx[CG_PCTL_R_N], bad, corrupt, unknown;
	uint64_t up_bytes, down_bytes, data_tx, data_rx, acks_tx, acks_rx, opens_tx, retx, copies, rto_n;
	uint64_t dup_frames, ooo_frames, drop_frames, nomem;
	uint64_t cc_switches; /* protocontroller_cc auto: connections switched to BBR */
	uint64_t retx_by[CG_ST_BY_N]; /* segments sent again, by why they were lost (RACK, the timer, a NACK) */
	uint64_t nack[CG_ST_NACK_N];  /* the rescue's NACKs of DATA, by what they did (cg_st_tx_nack) */
	uint64_t nack_noflow, nack_ack; /* NACKs of a stream gone; of an ACK (a fresh one sent instead) */
	uint64_t nack_link[CG_PCTL_LINKS + 1]; /* segments a NACK found lost, by the link they went on (the last: copied or not known) */
	uint64_t a_opened, a_bytes_in, a_bytes_out; /* variant A: relays opened, bytes read and written */
	uint8_t cc_auto;      /* protocontroller_cc auto */
	uint64_t holes[PCTL_HOLE_BUCKETS]; /* how long the receivers missed something: < 5, 10, 20, 40, 80, 160, 320 ms, more */
	uint64_t sendq[PCTL_HOLE_BUCKETS]; /* how long read bytes waited to be sent: < 1, 2, 4, 8, 16, 32, 64 ms, more */
	uint64_t writeq[PCTL_HOLE_BUCKETS]; /* how long bytes in order waited to be written: the same */
	struct cg_ratelimit rl_rst, rl_cc, rl_open;
};

static uint64_t ewma_rate(uint64_t *bytes, uint64_t *t, uint64_t rate, uint64_t now)
{
	uint64_t dt = now - *t;

	if (dt < PCTL_RATE_US)
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

	for (uint64_t lim = 1000; b < PCTL_HOLE_BUCKETS - 1 && us >= lim; lim *= 2)
		b++;
	return b;
}

static void age_push(struct age *a, uint8_t *h, uint8_t *n, uint64_t off, uint64_t t)
{
	if (*n && a[(*h + *n - 1) % PCTL_AGES].off >= off)
		return;
	if (*n == PCTL_AGES) { /* full: the oldest goes (its wait is not counted) */
		*h = (uint8_t)((*h + 1) % PCTL_AGES);
		(*n)--;
	}
	a[(*h + *n) % PCTL_AGES].off = off;
	a[(*h + *n) % PCTL_AGES].t = t;
	(*n)++;
}

/* Bytes up to off went on at now: the entries they cover, counted. */
static void age_pop(struct age *a, uint8_t *h, uint8_t *n, uint64_t off, uint64_t now, uint64_t *hist)
{
	while (*n && a[*h].off <= off) {
		hist[age_bucket(now > a[*h].t ? now - a[*h].t : 0)]++;
		*h = (uint8_t)((*h + 1) % PCTL_AGES);
		(*n)--;
	}
}

/* ---- the batch ---- */

static void flush(struct cg_pctl *p)
{
	if (p->nmsg)
		p->ops.send(p->ops.ctx, p->mpeer, p->msid, p->mptr, p->mlen, p->mcopy, p->nmsg);
	p->nmsg = 0;
}

/* Room for a message to peer: its buffer, PCTL_FRAME bytes. NULL in
 * cg_pctl_rx when the batch would have to go first: the engine is in the
 * middle of its own receive batch (it is a RST, best effort). */
static uint8_t *slot_for(struct cg_pctl *p, uint32_t peer, uint32_t sid)
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

static void push(struct cg_pctl *p, size_t len, int copy)
{
	len = cg_pctl_seal(p->msg[p->nmsg], len);
	p->mptr[p->nmsg] = p->msg[p->nmsg];
	p->mlen[p->nmsg] = (uint16_t)len;
	p->mcopy[p->nmsg] = (uint8_t)copy;
	p->nmsg++;
}

static void send_rst(struct cg_pctl *p, uint32_t peer, uint32_t sid, uint32_t id, uint8_t reason)
{
	uint8_t *b = slot_for(p, peer, sid);

	if (!b) {
		p->rst_lost++;
		return;
	}
	push(p, cg_pctl_put_rst(b, id, reason), 1);
	p->rst_tx++;
}

/* ---- flows ---- */

static struct peer *peer_of(struct cg_pctl *p, const struct flow *f)
{
	struct peer *r = &p->pr[p->cfg.server ? f->peer : 0];

	if (!r->used || r->sid != f->sid) {
		memset(r, 0, sizeof(*r));
		r->used = 1;
		r->app = 1; /* nothing sent yet: what comes first says nothing of the path */
		r->sid = f->sid;
		r->reo_us = p->cfg.reo_ms * 1000;
		cg_scc_init(&r->cc, cg_now_us());
	}
	return r;
}

/* The events f's socket is watched for. A socket that hung up reports it
 * whatever it is watched for: while f does not read it, it leaves the set
 * (or a full stream would spin the loop). */
static void ev_set(struct cg_pctl *p, struct flow *f, uint32_t want)
{
	struct epoll_event e = { .events = want, .data.u64 = CG_EV(CG_EV_PCTL, f - p->f) };

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

static struct flow *flow_alloc(struct cg_pctl *p)
{
	uint32_t s;
	struct flow *f;

	if (!p->nfree || p->pool.nfree < 4)
		return NULL;
	s = p->freeslot[--p->nfree];
	f = &p->f[s];
	memset(f, 0, sizeof(*f));
	f->fd = f->mfd = -1;
	cg_st_buf_init(&f->sb);
	cg_st_buf_init(&f->rb);
	cg_st_rx_init(&f->rx);
	cg_st_tx_init(&f->tx, p->segs + (size_t)s * p->nseg, p->nseg, PCTL_INIT_EDGE);
	f->created_t = f->progress_t = f->s_t = f->w_t = cg_now_us();
	p->used[p->nused++] = s;
	return f;
}

static void flow_free(struct cg_pctl *p, struct flow *f, int abort_sock)
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
	if (f->mfd >= 0) {
		if (abort_sock) {
			struct linger lg = { 1, 0 };

			setsockopt(f->mfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		}
		if (f->mreg)
			epoll_ctl(p->ep, EPOLL_CTL_DEL, f->mfd, NULL);
		close(f->mfd);
		f->mfd = -1;
	}
	if (f->state != F_FREE && !f->relay) {
		struct peer *r = &p->pr[p->cfg.server ? f->peer : 0];

		if (r->used && r->sid == f->sid)
			r->cc.inflight -= r->cc.inflight >= f->tx.inflight ? f->tx.inflight : r->cc.inflight;
	}
	cg_st_buf_free(&f->sb, &p->pool);
	cg_st_buf_free(&f->rb, &p->pool);
	if (p->cfg.server && !f->relay)
		cg_pctl_tab_del(&p->tab, cg_pctl_key(f->sid, f->id));
	else if (!p->cfg.server)
		p->gen[s] = cg_pctl_next_gen(p->gen[s]);
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
static void flow_reset(struct cg_pctl *p, struct flow *f, uint8_t reason)
{
	if (!f->relay)
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
		return CG_PCTL_R_REFUSED;
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ENETDOWN:
		return CG_PCTL_R_UNREACHABLE;
	case ETIMEDOUT:
		return CG_PCTL_R_TIMEOUT;
	case ECONNRESET:
	case EPIPE:
		return CG_PCTL_R_PEER;
	default:
		return CG_PCTL_R_ERROR;
	}
}

/* ---- the router: taking connections from the LAN ---- */

static int listen_one(struct cg_pctl *p, int family, char *err, size_t errlen)
{
	int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_storage a;
	socklen_t al;
	struct epoll_event e = { .events = EPOLLIN, .data.u64 = CG_EV(CG_EV_PCTL, CG_PCTL_LISTEN_IDX + (family == AF_INET6)) };

	if (fd < 0) {
		snprintf(err, errlen, "protocontroller: socket: %s", strerror(errno));
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
	set_int(fd, SOL_SOCKET, SO_RCVBUF, PCTL_LAN_RCVBUF);
	if (bind(fd, (struct sockaddr *)&a, al) < 0 || listen(fd, 128) < 0 ||
	    epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
		snprintf(err, errlen, "protocontroller: listen on port %u (%s): %s", p->cfg.port, family == AF_INET ? "IPv4" : "IPv6",
			 strerror(errno));
		close(fd);
		return -1;
	}
	p->lfd[family == AF_INET6] = fd;
	return 0;
}

/* The VPS: variant A's listening socket, on its tunnel address. The
 * address may not be up yet (IP_FREEBIND). */
static int listen_a(struct cg_pctl *p, char *err, size_t errlen)
{
	const struct sockaddr_storage *a = &p->cfg.a_bind;
	socklen_t al = a->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	int fd = socket(a->ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct epoll_event e = { .events = EPOLLIN, .data.u64 = CG_EV(CG_EV_PCTL, CG_PCTL_LISTEN_IDX + 2) };

	if (fd < 0) {
		snprintf(err, errlen, "protocontroller: socket: %s", strerror(errno));
		return -1;
	}
	set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
	if (a->ss_family == AF_INET6) {
		set_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1);
		set_int(fd, IPPROTO_IPV6, IPV6_FREEBIND, 1);
	} else {
		set_int(fd, IPPROTO_IP, IP_FREEBIND, 1);
	}
	set_int(fd, SOL_SOCKET, SO_RCVBUF, (int)p->buf_cap);
	if (bind(fd, (const struct sockaddr *)a, al) < 0 || listen(fd, 128) < 0 ||
	    epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
		snprintf(err, errlen, "protocontroller: listen on the tunnel's address, port %u: %s", p->cfg.port,
			 strerror(errno));
		close(fd);
		return -1;
	}
	p->lfd[2] = fd;
	return 0;
}

/* The connection's original destination: conntrack's (REDIRECT), else
 * the local address (TPROXY). 0, or -1 for one that is not redirected
 * (a connection to the listening port itself). */
static int orig_dst(struct cg_pctl *p, int fd, int v6, struct flow *f)
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

static void refuse_fd(struct cg_pctl *p, int fd, unsigned reason)
{
	struct linger lg = { 1, 0 };

	setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	close(fd);
	p->refused[reason < CG_PCTL_R_N ? reason : CG_PCTL_R_ERROR]++;
}

static void a_open(struct cg_pctl *p, int fd, const struct flow *tmp);

static void accept_some(struct cg_pctl *p, int v6)
{
	for (int k = 0; k < 32; k++) {
		int fd = accept4(p->lfd[v6], NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		struct flow *f, tmp;
		struct epoll_event e = { .events = EPOLLIN };

		if (fd < 0)
			return;
		memset(&tmp, 0, sizeof(tmp));
		if (orig_dst(p, fd, v6, &tmp) < 0 || cg_pctl_dest_ok(tmp.family, tmp.addr)) {
			refuse_fd(p, fd, CG_PCTL_R_DENIED);
			continue;
		}
		if (p->cfg.mode == CG_PCTL_A) {
			a_open(p, fd, &tmp);
			continue;
		}
		if (!(f = flow_alloc(p))) {
			refuse_fd(p, fd, CG_PCTL_R_FULL);
			if (cg_ratelimit_ok(&p->rl_open, cg_now_ms(), 10000))
				cg_warn("protocontroller: a connection refused: %s", p->nfree ? "protocontroller_mem_mb is full" : "protocontroller_max_flows streams open");
			continue;
		}
		f->family = tmp.family;
		f->port = tmp.port;
		memcpy(f->addr, tmp.addr, 16);
		f->fd = fd;
		f->state = F_OPEN;
		f->id = cg_pctl_id(p->gen[f - p->f], (uint16_t)(f - p->f));
		f->rd_ready = 1;
		set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
		set_int(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PCTL_LOWAT_MIN);
		e.data.u64 = CG_EV(CG_EV_PCTL, f - p->f);
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

static socklen_t sa_of(struct sockaddr_storage *a, int family, const uint8_t *addr, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	if (family == 4) {
		struct sockaddr_in *s = (struct sockaddr_in *)a;

		s->sin_family = AF_INET;
		memcpy(&s->sin_addr, addr, 4);
		s->sin_port = htons(port);
		return sizeof(*s);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)a;

		s->sin6_family = AF_INET6;
		memcpy(&s->sin6_addr, addr, 16);
		s->sin6_port = htons(port);
		return sizeof(*s);
	}
}

/* A non-blocking connect to the destination, with protocontroller_cc,
 * TCP_NODELAY and the bounded buffers. The socket, or -1 with *why. */
static int dest_connect(struct cg_pctl *p, int family, const uint8_t *addr, uint16_t port, int *why)
{
	struct sockaddr_storage a;
	socklen_t al = sa_of(&a, family, addr, port);
	int fd = socket(a.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0) {
		*why = CG_PCTL_R_ERROR;
		return -1;
	}
	if (p->cfg.cc[0] && !p->cc_auto &&
	    setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, p->cfg.cc, (socklen_t)strlen(p->cfg.cc)) < 0 &&
	    cg_ratelimit_ok(&p->rl_cc, cg_now_ms(), 600000))
		cg_warn("protocontroller: protocontroller_cc %s: %s (the kernel's own instead; modprobe tcp_%s?)", p->cfg.cc,
			strerror(errno), p->cfg.cc);
	set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
	set_int(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PCTL_LOWAT_MIN);
	/* what the destination sends while the stream is full waits in this
	 * socket: as much as the stream's own buffer (the kernel doubles it),
	 * not the autotuned 6 MB (a download, 7 MB a connection) */
	set_int(fd, SOL_SOCKET, SO_RCVBUF, (int)p->buf_cap);
	if (connect(fd, (struct sockaddr *)&a, al) < 0 && errno != EINPROGRESS) {
		*why = reason_of(errno);
		close(fd);
		return -1;
	}
	return fd;
}

static void open_in(struct cg_pctl *p, uint32_t peer, uint32_t sid, const struct cg_pctl_msg *m)
{
	struct flow *f;
	struct epoll_event e = { .events = EPOLLOUT };
	int fd, r;
	uint8_t why = (uint8_t)cg_pctl_dest_ok(m->family, m->addr);

	if (why) {
		send_rst(p, peer, sid, m->id, why);
		p->refused[why]++;
		return;
	}
	if (!(f = flow_alloc(p))) {
		send_rst(p, peer, sid, m->id, CG_PCTL_R_FULL);
		p->refused[CG_PCTL_R_FULL]++;
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
	if (cg_pctl_tab_put(&p->tab, cg_pctl_key(sid, m->id), (uint32_t)(f - p->f)) < 0) {
		flow_free(p, f, 0);
		send_rst(p, peer, sid, m->id, CG_PCTL_R_FULL);
		return;
	}
	if ((fd = dest_connect(p, m->family, m->addr, m->port, &r)) < 0) {
		flow_reset(p, f, (uint8_t)r);
		return;
	}
	f->fd = fd;
	e.data.u64 = CG_EV(CG_EV_PCTL, f - p->f);
	if (epoll_ctl(p->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
		flow_reset(p, f, CG_PCTL_R_ERROR);
		return;
	}
	f->ev = EPOLLOUT;
	f->reg = 1;
	peer_of(p, f);
	p->opened++;
}


/* ---- variant A: the middle leg is a plain TCP through the tunnel ---- */

static void due(struct cg_pctl *p, uint64_t t);

/* The events f's middle socket is watched for. */
static void mid_ev_set(struct cg_pctl *p, struct flow *f, uint32_t want)
{
	struct epoll_event e = { .events = want, .data.u64 = CG_EV(CG_EV_PCTL, (uint32_t)(f - p->f) | CG_PCTL_MID) };

	if (f->mfd < 0)
		return;
	if (f->mhup && !want) { /* as ev_set */
		if (f->mreg && epoll_ctl(p->ep, EPOLL_CTL_DEL, f->mfd, NULL) == 0)
			f->mreg = 0;
		f->mev = 0;
		return;
	}
	if (!f->mreg) {
		if (epoll_ctl(p->ep, EPOLL_CTL_ADD, f->mfd, &e) == 0) {
			f->mreg = 1;
			f->mev = want;
		}
		return;
	}
	if (want != f->mev && epoll_ctl(p->ep, EPOLL_CTL_MOD, f->mfd, &e) == 0)
		f->mev = want;
}

/* The header the router writes first on the middle connection: magic,
 * family, port, address. Returns its length. */
static size_t a_hdr(const struct flow *f, uint8_t *b)
{
	size_t al = f->family == 6 ? 16 : 4;

	b[0] = PCTL_AMAGIC;
	b[1] = f->family;
	b[2] = (uint8_t)(f->port >> 8);
	b[3] = (uint8_t)f->port;
	memcpy(b + 4, f->addr, al);
	return 4 + al;
}

/* The router: a LAN connection (fd, its destination in tmp) for variant A:
 * a TCP to the VPS's end of the tunnel. */
static void a_open(struct cg_pctl *p, int fd, const struct flow *tmp)
{
	struct flow *f = flow_alloc(p);
	int m;
	socklen_t al;

	if (!f) {
		refuse_fd(p, fd, CG_PCTL_R_FULL);
		return;
	}
	f->relay = 1;
	f->family = tmp->family;
	f->port = tmp->port;
	memcpy(f->addr, tmp->addr, 16);
	f->fd = fd;
	f->state = F_EST;
	f->rd_ready = 1;
	set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
	set_int(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PCTL_LOWAT_MIN);
	m = socket(p->cfg.a_peer.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (m < 0) {
		flow_free(p, f, 1);
		return;
	}
	f->mfd = m;
	if (p->cfg.mid_cc[0])
		setsockopt(m, IPPROTO_TCP, TCP_CONGESTION, p->cfg.mid_cc, (socklen_t)strlen(p->cfg.mid_cc));
	set_int(m, IPPROTO_TCP, TCP_NODELAY, 1);
	set_int(m, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PCTL_LOWAT_MIN);
	al = p->cfg.a_peer.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	if (connect(m, (const struct sockaddr *)&p->cfg.a_peer, al) < 0 && errno != EINPROGRESS) {
		flow_free(p, f, 1);
		return;
	}
	f->mconn = 1;
	ev_set(p, f, 0);
	mid_ev_set(p, f, EPOLLOUT);
	p->opened++;
	p->a_opened++;
}

/* Whether a middle connection comes from the tunnel's prefix. */
static int from_tunnel(const struct cg_pctl *p, const struct sockaddr_storage *a)
{
	const struct sockaddr_storage *b = &p->cfg.a_bind;

	if (a->ss_family != b->ss_family)
		return 0;
	if (a->ss_family == AF_INET)
		return cg_pctl_in_pfx(4, (const uint8_t *)&((const struct sockaddr_in *)a)->sin_addr,
				      (const uint8_t *)&((const struct sockaddr_in *)b)->sin_addr, p->cfg.a_len);
	return cg_pctl_in_pfx(6, (const uint8_t *)&((const struct sockaddr_in6 *)a)->sin6_addr,
			      (const uint8_t *)&((const struct sockaddr_in6 *)b)->sin6_addr, p->cfg.a_len);
}

/* The VPS: variant A's connections from the routers, through the tunnel. */
static void a_accept(struct cg_pctl *p)
{
	for (int k = 0; k < 32; k++) {
		struct sockaddr_storage from;
		socklen_t fl = sizeof(from);
		int m = accept4(p->lfd[2], (struct sockaddr *)&from, &fl, SOCK_NONBLOCK | SOCK_CLOEXEC);
		struct flow *f;

		if (m < 0)
			return;
		if (!from_tunnel(p, &from)) {
			refuse_fd(p, m, CG_PCTL_R_DENIED);
			continue;
		}
		if (!(f = flow_alloc(p))) {
			refuse_fd(p, m, CG_PCTL_R_FULL);
			continue;
		}
		f->relay = 1;
		f->mfd = m;
		f->state = F_OPEN; /* until the header came */
		if (p->cfg.mid_cc[0])
			setsockopt(m, IPPROTO_TCP, TCP_CONGESTION, p->cfg.mid_cc, (socklen_t)strlen(p->cfg.mid_cc));
		f->hdr_want = 4;
		f->m_rd = 1;
		set_int(m, IPPROTO_TCP, TCP_NODELAY, 1);
		set_int(m, IPPROTO_TCP, TCP_NOTSENT_LOWAT, PCTL_LOWAT_MIN);
		mid_ev_set(p, f, EPOLLIN);
		p->opened++;
		p->a_opened++;
	}
}

/* One direction of a relay: what from has, into the buffer (cap), and on
 * to to; the FIN once all went. Returns -1 on an error (reset). */
static int a_move(struct cg_pctl *p, int from, int to, int *rd, int *wb, struct cg_st_buf *b, struct rdir *d,
		  int can_read)
{
	uint32_t got = 0;

	while (can_read && *rd && !d->eof && d->in - d->out < p->buf_cap && got < PCTL_READ_PASS) {
		uint32_t room;
		uint64_t k = p->buf_cap - (d->in - d->out);
		uint8_t *w = cg_st_buf_at(b, &p->pool, d->in, 1, &room);
		ssize_t n;

		if (!w) { /* out of chunks: again when some came back */
			d->nomem = 1;
			p->nomem++;
			break;
		}
		if (k > room)
			k = room;
		n = recv(from, w, k, 0);
		if (n > 0) {
			d->in += (uint64_t)n;
			got += (uint32_t)n;
			p->a_bytes_in += (uint64_t)n;
			continue;
		}
		if (n == 0) {
			d->eof = 1;
			break;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			*rd = 0;
			break;
		}
		if (errno == EINTR)
			continue;
		return -1;
	}
	while (d->out < d->in) {
		uint32_t room;
		const uint8_t *r = cg_st_buf_at(b, &p->pool, d->out, 0, &room);
		uint64_t k = d->in - d->out;
		ssize_t n;

		if (!r)
			return -1;
		if (k > room)
			k = room;
		n = send(to, r, k, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			d->out += (uint64_t)n;
			p->a_bytes_out += (uint64_t)n;
			cg_st_buf_release(b, &p->pool, d->out);
			*wb = 0;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			*wb = 1;
			break;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	if (d->eof && d->out == d->in && !d->shut) {
		shutdown(to, SHUT_WR);
		d->shut = 1;
	}
	return 0;
}

/* A relay's pass. Returns -1 when the flow went. */
static int a_service(struct cg_pctl *p, struct flow *f, uint64_t now)
{
	int rd, wb, mrd, mwb, why;

	if (f->state == F_OPEN && p->cfg.server) { /* the VPS: the header */
		while (f->m_rd && f->hdr_n < f->hdr_want) {
			ssize_t n = recv(f->mfd, f->hdr + f->hdr_n, f->hdr_want - f->hdr_n, 0);

			if (n > 0) {
				f->hdr_n = (uint8_t)(f->hdr_n + n);
				if (f->hdr_n == 4) {
					if (f->hdr[0] != PCTL_AMAGIC || (f->hdr[1] != 4 && f->hdr[1] != 6)) {
						p->bad++;
						flow_free(p, f, 1);
						return -1;
					}
					f->hdr_want = (uint8_t)(4 + (f->hdr[1] == 6 ? 16 : 4));
				}
				continue;
			}
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
				flow_free(p, f, 1);
				return -1;
			}
			f->m_rd = 0;
		}
		if (f->hdr_n < f->hdr_want || f->hdr_want == 4) {
			if (now - f->created_t > (uint64_t)PCTL_CONNECT_S * 1000000) {
				flow_free(p, f, 1);
				return -1;
			}
			mid_ev_set(p, f, EPOLLIN);
			return 0;
		}
		f->family = f->hdr[1];
		f->port = (uint16_t)(f->hdr[2] << 8 | f->hdr[3]);
		memcpy(f->addr, f->hdr + 4, f->family == 6 ? 16 : 4);
		if ((why = cg_pctl_dest_ok(f->family, f->addr)) || (f->fd = dest_connect(p, f->family, f->addr, f->port, &why)) < 0) {
			p->refused[why < CG_PCTL_R_N ? why : CG_PCTL_R_ERROR]++;
			flow_free(p, f, 1);
			return -1;
		}
		f->state = F_EST;
		f->connecting = 1;
		mid_ev_set(p, f, 0);
		ev_set(p, f, EPOLLOUT);
		return 0;
	}
	if (f->connecting || f->mconn) {
		if ((f->connecting || f->mconn) && now - f->created_t > (uint64_t)PCTL_CONNECT_S * 1000000) {
			p->refused[CG_PCTL_R_TIMEOUT]++;
			flow_free(p, f, 1);
			return -1;
		}
		return 0;
	}
	f->d[0].nomem = f->d[1].nomem = 0;
	rd = f->rd_ready;
	wb = f->wr_blocked;
	mrd = f->m_rd;
	mwb = f->m_wb;
	if (a_move(p, f->fd, f->mfd, &rd, &mwb, &f->sb, &f->d[0], 1) < 0 ||
	    a_move(p, f->mfd, f->fd, &mrd, &wb, &f->rb, &f->d[1], 1) < 0) {
		flow_free(p, f, 1);
		return -1;
	}
	f->rd_ready = (uint8_t)rd;
	f->wr_blocked = (uint8_t)wb;
	f->m_rd = (uint8_t)mrd;
	f->m_wb = (uint8_t)mwb;
	if (f->d[0].in - f->d[0].out + f->d[1].in - f->d[1].out)
		f->progress_t = now;
	if (f->d[0].shut && f->d[1].shut) {
		flow_free(p, f, 0);
		return -1;
	}
	{
		int can0 = !f->d[0].eof && !f->d[0].nomem && f->d[0].in - f->d[0].out < p->buf_cap;
		int can1 = !f->d[1].eof && !f->d[1].nomem && f->d[1].in - f->d[1].out < p->buf_cap;

		if (f->d[0].nomem || f->d[1].nomem)
			due(p, now + 10000);
		if (can0 && !(f->ev & EPOLLIN))
			f->rd_ready = 1;
		if (can1 && !(f->mev & EPOLLIN))
			f->m_rd = 1;
		ev_set(p, f, (can0 ? EPOLLIN : 0) | (f->wr_blocked ? EPOLLOUT : 0));
		mid_ev_set(p, f, (can1 ? EPOLLIN : 0) | (f->m_wb ? EPOLLOUT : 0));
	}
	return 0;
}

/* A relay's socket became ready (mid: the middle one). */
static void a_event(struct cg_pctl *p, struct flow *f, int mid, uint32_t events)
{
	int err = 0;
	socklen_t el = sizeof(err);

	if (mid && f->mconn) { /* the router's connect to the VPS */
		if (getsockopt(f->mfd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
			err = errno;
		if (err) {
			p->refused[reason_of(err)]++;
			flow_free(p, f, 1);
			return;
		}
		{
			uint8_t h[CG_PCTL_AHDR_MAX];
			size_t hl = a_hdr(f, h);

			if (send(f->mfd, h, hl, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)hl) {
				flow_free(p, f, 1);
				return;
			}
		}
		f->mconn = 0;
		f->m_rd = f->rd_ready = 1;
		return;
	}
	if (!mid && f->connecting) { /* the VPS's connect to the destination */
		if (getsockopt(f->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
			err = errno;
		if (err) {
			p->refused[reason_of(err)]++;
			flow_free(p, f, 1);
			return;
		}
		f->connecting = 0;
		f->rd_ready = f->m_rd = 1;
		return;
	}
	if (events & EPOLLERR) { /* a reset or an error: both go */
		flow_free(p, f, 1);
		return;
	}
	if (mid) {
		if (events & (EPOLLIN | EPOLLHUP))
			f->m_rd = 1;
		if (events & EPOLLHUP)
			f->mhup = 1;
		if (events & EPOLLOUT)
			f->m_wb = 0;
	} else {
		if (events & (EPOLLIN | EPOLLHUP))
			f->rd_ready = 1;
		if (events & EPOLLHUP)
			f->hup = 1;
		if (events & EPOLLOUT)
			f->wr_blocked = 0;
	}
}

/* ---- messages in ---- */

static struct flow *find(struct cg_pctl *p, uint32_t sid, uint32_t id)
{
	if (p->cfg.server) {
		uint32_t s = cg_pctl_tab_find(&p->tab, cg_pctl_key(sid, id));

		return s == CG_PCTL_TAB_EMPTY ? NULL : &p->f[s];
	} else {
		uint32_t s = id & 0xffff;

		if (s >= p->cfg.max_flows || p->f[s].state == F_FREE || p->f[s].id != id)
			return NULL;
		return &p->f[s];
	}
}

static void data_in(struct cg_pctl *p, struct flow *f, const struct cg_pctl_msg *m, uint64_t now, unsigned link)
{
	uint64_t off = cg_st_unwrap(f->rx.nxt, m->off);
	int fin = !!(m->flags & CG_PCTLF_FIN), v;
	unsigned nr;
	uint64_t was = f->rx.nxt;

	p->data_rx++;
	/* the least one-way delay since the last ACK (duplicates too: the
	 * slower copies never win the minimum), less what the link it came by
	 * adds over the shortest one (linkowd.h, task #77: the engine says
	 * the link); a message the tunnel's codes rebuilt gives none */
	if (link != CG_PCTL_NOLINK) {
		int32_t d = cg_lowd_take(&peer_of(p, f)->lo, link, (int32_t)((uint32_t)now - m->ts), now);

		cg_lowd_ack_add(&f->la, link, d);

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

		for (uint64_t lim = 5; b < PCTL_HOLE_BUCKETS - 1 && ms >= lim; lim *= 2)
			b++;
		p->holes[b]++;
		f->hole_t = 0;
	}
	/* at once: a new hole (the sender's RACK wants to know), a hole filled
	 * (its retransmission went: the sender frees and sends), the FIN; the
	 * rest every PCTL_ACK_FRAMES frames or PCTL_ACK_US. An ACK for every
	 * frame out of order (60 % lost, the microcut) copied on every link
	 * filled the cellular link's download to 2 s */
	if (f->rx.nr > nr || (was < f->rx.nxt && nr) || fin)
		f->ack_now = 1;
	if (!f->ack_pend++)
		f->ack_t = now;
}

/* RACK's window beyond min_rtt / 4, and the timer's: reo_us, widened by
 * duplicates (the tunnel's repairs come first), unless the loss is heavy
 * (streamcc.h's cg_scc_lossw): then protocontroller_reo_ms. */
static uint32_t peer_reo(const struct cg_pctl *p, const struct peer *r)
{
	return r->cc.heavy ? p->cfg.reo_ms * 1000 : r->reo_us;
}

/* The timer: rto after the oldest SENT segment's last transmission (a
 * segment sent again restarts its own clock); none with nothing SENT. */
static void rto_arm(const struct cg_pctl *p, struct flow *f, struct peer *r)
{
	uint64_t o = f->tx.inflight ? cg_st_tx_oldest(&f->tx) : 0;

	f->rto_at = o ? o + cg_scc_rto(&r->cc, f->backoff, peer_reo(p, r)) : 0;
}

static void ack_in(struct cg_pctl *p, struct flow *f, const struct cg_pctl_msg *m, uint64_t now)
{
	struct peer *r = peer_of(p, f);
	struct cg_st_ack a;
	struct cg_st_ackres res;
	uint64_t lost, lq = 0;

	p->acks_rx++;
	/* something was sent again for nothing: wait longer before the next */
	if ((m->flags & CG_PCTLF_DUP) && r->reo_us < p->reo_cap && !r->cc.heavy) {
		r->reo_us += PCTL_REO_STEP_US;
		r->dups++;
	}
	cg_pctl_ack_of(m, f->tx.una, f->tx.edge, &a);
	cg_st_tx_ack(&f->tx, &a, &res);
	if (m->owd != CG_PCTL_NOOWD)
		cg_scc_owd(&r->cc, &p->ccf, now, m->owd);
	r->cc.inflight -= r->cc.inflight >= res.was_inflight ? res.was_inflight : r->cc.inflight;
	cg_scc_ack(&r->cc, &p->ccf, now, res.delivered, res.rs.have, res.rs.tx, res.rs.app, res.rs.sent_us, res.rs.dl,
		   res.rs.dla, res.rs.dl_us, res.rs.ss);
	if (res.progress) {
		cg_st_buf_release(&f->sb, &p->pool, f->tx.una < f->tx.tail ? f->tx.una : f->tx.tail);
		f->backoff = 0;
		f->s_bytes += res.freed;
	}
	lost = cg_st_tx_rack(&f->tx, cg_scc_reo(&r->cc, peer_reo(p, r)), &lq);
	if (lost) {
		r->cc.inflight -= r->cc.inflight >= lost ? lost : r->cc.inflight;
		cg_scc_lost(&r->cc, lost - lq);
	}
	rto_arm(p, f, r);
}

int cg_pctl_nack(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, uint8_t link)
{
	uint32_t id = 0, off = 0, ts = 0, bytes;
	int k = cg_pctl_nack_kind(msg, len, &id, &off, &ts), v;
	struct flow *f;
	struct peer *r;
	uint64_t lq = 0;

	if (k == CG_PCTL_NACK_NOT || k == CG_PCTL_NACK_RESEND)
		return k;
	if (!p->cfg.server)
		sid = 0;
	f = find(p, sid, id);
	if (!f || f->relay) {
		p->nack_noflow++; /* gone: nothing to send again */
		return k;
	}
	if (k == CG_PCTL_NACK_ACK) {
		f->ack_now = 1; /* a fresh one at the next service */
		p->nack_ack++;
		due(p, cg_now_us());
		return k;
	}
	(void)peer;
	v = cg_st_tx_nack(&f->tx, off, ts, &bytes, &lq);
	p->nack[v]++;
	if (v != CG_ST_NACK_LOST)
		return k;
	p->nack_link[link < CG_PCTL_LINKS ? link : CG_PCTL_LINKS]++;
	/* as a loss RACK found: out of flight, into the rate's losses; it goes
	 * again first, at the next service, within the pacer's tokens */
	r = peer_of(p, f);
	r->cc.inflight -= r->cc.inflight >= bytes ? bytes : r->cc.inflight;
	cg_scc_lost(&r->cc, bytes - lq);
	rto_arm(p, f, r);
	due(p, cg_now_us());
	return k;
}

static void rx_one(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, unsigned link);

void cg_pctl_rx(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, unsigned link)
{
	p->in_rx = 1;
	rx_one(p, peer, sid, msg, len, link);
	p->in_rx = 0;
}

static void rx_one(struct cg_pctl *p, uint32_t peer, uint32_t sid, const uint8_t *msg, size_t len, unsigned link)
{
	struct cg_pctl_msg m;
	struct flow *f;
	uint64_t now = cg_now_us();

	switch (cg_pctl_parse(msg, len, &m)) {
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
	if (m.type == CG_PCTL_OPEN) {
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
		if (m.type != CG_PCTL_RST && (m.flags & CG_PCTLF_EST) && cg_ratelimit_ok(&p->rl_rst, now / 1000, 1))
			send_rst(p, peer, sid, m.id, CG_PCTL_R_UNKNOWN);
		return;
	}
	f->progress_t = now;
	switch (m.type) {
	case CG_PCTL_DATA:
		if (!p->cfg.server)
			f->est = 1;
		data_in(p, f, &m, now, link);
		break;
	case CG_PCTL_ACK:
		if (!p->cfg.server && f->state == F_OPEN) {
			f->state = F_EST;
			f->est = 1;
		}
		ack_in(p, f, &m, now);
		break;
	case CG_PCTL_RST:
		p->rst_rx[m.reason < CG_PCTL_R_N ? m.reason : CG_PCTL_R_ERROR]++;
		/* all said both ways but the FIN's ACK: a clean close */
		flow_free(p, f, !(f->tx.eof && f->tx.una >= f->tx.tail && cg_st_rx_ended(&f->rx) &&
				  f->consumed >= f->rx.fin));
		break;
	default:
		p->bad++;
	}
}

/* ---- the sockets ---- */

static uint64_t backlog_max(const struct cg_pctl *p, const struct flow *f, uint64_t rate, uint64_t now)
{
	return cg_pctl_backlog(rate, p->cfg.backlog_ms,
			      now - f->created_t < PCTL_START_US ? PCTL_START_BACKLOG : PCTL_MIN_BACKLOG);
}

/* The unsent bytes f's socket may hold: its write rate for PCTL_LOWAT_MS
 * (TCP_NOTSENT_LOWAT follows, so that EPOLLOUT comes at it). */
static uint32_t lowat_of(struct flow *f)
{
	uint64_t l = f->w_rate * PCTL_LOWAT_MS / 1000;

	if (l < PCTL_LOWAT_MIN)
		l = PCTL_LOWAT_MIN;
	if (l > PCTL_LOWAT_MAX)
		l = PCTL_LOWAT_MAX;
	if (!f->lowat || l > f->lowat + f->lowat / 4 || l + l / 4 < f->lowat) {
		f->lowat = (uint32_t)l;
		setsockopt(f->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &f->lowat, sizeof(f->lowat));
	}
	return f->lowat;
}

/* Reads what the socket has into the stream, as far as it may hold.
 * Returns -1 when the flow was reset. */
static int sock_read(struct cg_pctl *p, struct flow *f)
{
	uint32_t got = 0;

	while (f->rd_ready && !f->tx.eof && got < PCTL_READ_PASS) {
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
static int sock_write(struct cg_pctl *p, struct flow *f, uint64_t now)
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
		uint64_t e = cg_pctl_edge(f->consumed, f->rx.nxt, p->buf_cap, backlog_max(p, f, f->w_rate, now));

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
static void connected(struct cg_pctl *p, struct flow *f)
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

void cg_pctl_event(struct cg_pctl *p, uint32_t idx, uint32_t events)
{
	struct flow *f;

	if (idx >= CG_PCTL_LISTEN_IDX) {
		if (!p->cfg.server && idx - CG_PCTL_LISTEN_IDX < 2 && p->lfd[idx - CG_PCTL_LISTEN_IDX] >= 0)
			accept_some(p, (int)(idx - CG_PCTL_LISTEN_IDX));
		else if (p->cfg.server && idx == CG_PCTL_LISTEN_IDX + 2 && p->lfd[2] >= 0)
			a_accept(p);
		return;
	}
	if ((idx & ~CG_PCTL_MID) >= p->cfg.max_flows || (f = &p->f[idx & ~CG_PCTL_MID])->state == F_FREE)
		return;
	if (f->relay) {
		a_event(p, f, !!(idx & CG_PCTL_MID), events);
		return;
	}
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

static void send_ack(struct cg_pctl *p, struct flow *f)
{
	struct cg_st_ack a;
	uint64_t edge = cg_pctl_edge(f->consumed, f->rx.nxt, p->buf_cap, backlog_max(p, f, f->w_rate, cg_now_us()));
	uint8_t *b = slot_for(p, f->peer, f->sid);

	int32_t d;

	cg_st_rx_ack(&f->rx, edge, ++f->aseq, &a);
	f->adv_edge = edge;
	/* the delay: the worst of the links that carried a quarter or more
	 * (linkowd.h), else the least of all */
	if (f->have_owd && cg_lowd_ack_get(&f->la, &d))
		f->owd = d;
	push(p, cg_pctl_put_ack(b, f->id, (uint8_t)((f->est ? CG_PCTLF_EST : 0) | (f->dup_seen ? CG_PCTLF_DUP : 0)), &a,
			       f->have_owd ? f->owd : CG_PCTL_NOOWD),
	     0);
	f->have_owd = 0;
	f->dup_seen = 0;
	f->ack_now = 0;
	f->ack_pend = 0;
	p->acks_tx++;
}

static void send_open(struct cg_pctl *p, struct flow *f, uint64_t now)
{
	struct peer *r = peer_of(p, f);
	uint8_t *b = slot_for(p, f->peer, f->sid);

	push(p, cg_pctl_put_open(b, f->id, f->family, f->addr, f->port), 1);
	f->open_sent = 1;
	f->open_due = now + cg_scc_rto(&r->cc, f->open_tries < 6 ? f->open_tries : 6, peer_reo(p, r));
	if (f->open_tries < 255)
		f->open_tries++;
	p->opens_tx++;
}

/* One segment of f, if it has one and its pacer lets it. Returns 1 if it
 * sent. */
static int send_one(struct cg_pctl *p, struct flow *f, struct peer *r, uint64_t now)
{
	uint32_t idx, len, sent;
	uint64_t off;
	int fin, copy;
	uint8_t *b;

	if (!cg_scc_may_repair(&r->cc) || !cg_st_tx_next(&f->tx, p->cfg.mss, &idx, &off, &len, &fin))
		return 0;
	/* new bytes need room in flight; a repair, tokens only */
	if (idx == f->tx.n && !cg_scc_may(&r->cc, &p->ccf, cg_scc_reo(&r->cc, peer_reo(p, r))))
		return 0;
	cg_scc_restart(&r->cc, now);
	copy = cg_st_tx_copy(&f->tx, idx);
	b = slot_for(p, f->peer, f->sid);
	cg_pctl_put_data(b, f->id, (uint8_t)((fin ? CG_PCTLF_FIN : 0) | (f->est ? CG_PCTLF_EST : 0)), off, (uint32_t)now);
	if (len && cg_st_buf_read(&f->sb, &p->pool, off, b + CG_PCTL_DATA_HDR, len) < 0)
		return 0; /* not reached: unacknowledged bytes are held */
	push(p, CG_PCTL_DATA_HDR + len, copy);
	if (idx < f->tx.n) {
		uint8_t by = cg_st_tx_seg(&f->tx, idx)->by;

		p->retx++;
		p->retx_by[by < CG_ST_BY_N ? by : CG_ST_BY_RACK]++;
	} else {
		age_pop(f->rd, &f->rd_h, &f->rd_n, off + len, now, p->sendq);
	}
	p->copies += (uint64_t)copy;
	/* app-limited: the peer's senders ran out of bytes lately, or this one
	 * has none left after it (a stream's last bytes, a FIN alone, a live
	 * stream between frames): its sample says what they had, not the path */
	sent = cg_st_tx_sent(&f->tx, idx, off, len, fin, now, r->cc.dl, r->cc.dla, r->cc.dl_us, r->cc.sent,
			     r->app || (!cg_st_tx_unsent(&f->tx) && !f->tx.nlost), r->cc.qnow || r->cc.cong);
	r->cc.inflight += sent;
	cg_scc_spend(&r->cc, sent ? sent : 1);
	if (!f->rto_at)
		f->rto_at = now + cg_scc_rto(&r->cc, f->backoff, peer_reo(p, r));
	p->data_tx++;
	(void)now;
	return 1;
}

static void due(struct cg_pctl *p, uint64_t t)
{
	if (t && (!p->due_us || t < p->due_us))
		p->due_us = t;
}

/* The timers of f at now. Returns -1 when the flow went. */
static int timers(struct cg_pctl *p, struct flow *f, uint64_t now)
{
	struct peer *r = peer_of(p, f);

	if (f->connecting && now - f->created_t > (uint64_t)PCTL_CONNECT_S * 1000000) {
		flow_reset(p, f, CG_PCTL_R_TIMEOUT);
		return -1;
	}
	if ((cg_st_tx_busy(&f->tx) || f->state == F_OPEN) && p->cfg.dead_s &&
	    now - f->progress_t > (uint64_t)p->cfg.dead_s * 1000000) {
		flow_reset(p, f, CG_PCTL_R_TIMEOUT);
		return -1;
	}
	if (f->rto_at && now >= f->rto_at) {
		uint64_t lq = 0, lost = cg_st_tx_rto(&f->tx, now, cg_scc_rto(&r->cc, f->backoff, peer_reo(p, r)), &lq);

		r->cc.inflight -= r->cc.inflight >= lost ? lost : r->cc.inflight;
		cg_scc_lost(&r->cc, lost - lq);
		if (lost) {
			if (f->backoff < 6)
				f->backoff++;
			p->rto_n++;
		}
		rto_arm(p, f, r);
	}
	if (!p->cfg.server && f->state == F_OPEN && (!f->open_sent || now >= f->open_due))
		send_open(p, f, now);
	return 0;
}

void cg_pctl_service(struct cg_pctl *p)
{
	uint64_t now = cg_now_us();

	p->due_us = 0;
	for (uint32_t i = 0; i < p->nused;) {
		struct flow *f = &p->f[p->used[i]];
		uint32_t want;

		if (f->relay) {
			if (a_service(p, f, now) == 0)
				i++;
			continue;
		}
		if (timers(p, f, now) < 0 || sock_read(p, f) < 0 || sock_write(p, f, now) < 0)
			continue; /* freed: used[i] is another flow now */
		if (f->ack_pend || f->ack_now) {
			if (f->ack_now || f->ack_pend >= PCTL_ACK_FRAMES || now - f->ack_t >= PCTL_ACK_US)
				send_ack(p, f);
			else
				due(p, f->ack_t + PCTL_ACK_US);
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
			cg_scc_refill(&p->pr[i].cc, &p->ccf, now);
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
		    r->cc.inflight < cg_scc_cwnd(&r->cc, &p->ccf, cg_scc_reo(&r->cc, peer_reo(p, r))))
			due(p, now + cg_scc_wait_us(&r->cc));
	}
	/* tokens left and room in flight with nothing to send: the samples
	 * that follow say what the senders had, not the path */
	for (uint32_t i = 0; i < p->npr; i++) {
		struct peer *r = &p->pr[i];

		if (r->used)
			r->app = r->cc.tokens > 0 && r->cc.inflight < cg_scc_cwnd(&r->cc, &p->ccf, cg_scc_reo(&r->cc, peer_reo(p, r)));
	}
}

int cg_pctl_timeout(const struct cg_pctl *p)
{
	uint64_t now;

	if (!p->due_us)
		return -1;
	now = cg_now_us();
	if (p->due_us <= now)
		return 0;
	return (int)((p->due_us - now + 999) / 1000);
}

/* protocontroller_cc auto: a connection to a destination starts with the kernel's own
 * congestion control (cubic: no BBR ProbeRTT, which every 10 s held a live
 * stream to 4 packets a round trip for 200 ms, 50-140 ms of delay) and goes
 * to BBR once its leg loses (a segment sent again: cubic took
 * 1 % of loss at 100 ms for congestion, story 024). */
static void cc_auto(struct cg_pctl *p, struct flow *f)
{
	struct tcp_info ti;
	socklen_t tl = sizeof(ti);

	if (f->cc_bbr || f->connecting || f->fd < 0)
		return;
	memset(&ti, 0, sizeof(ti));
	if (getsockopt(f->fd, IPPROTO_TCP, TCP_INFO, &ti, &tl) < 0 || ti.tcpi_total_retrans < PCTL_CC_LOSSES)
		return;
	f->cc_bbr = 1;
	if (setsockopt(f->fd, IPPROTO_TCP, TCP_CONGESTION, "bbr", 3) == 0)
		p->cc_switches++;
	else if (cg_ratelimit_ok(&p->rl_cc, cg_now_ms(), 600000))
		cg_warn("protocontroller: protocontroller_cc auto: bbr: %s (modprobe tcp_bbr?)", strerror(errno));
}

void cg_pctl_tick(struct cg_pctl *p)
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
		cg_scc_hint(&r->cc, bps, full);
		if (r->reo_us > p->cfg.reo_ms * 1000 + PCTL_REO_DECAY_US)
			r->reo_us -= PCTL_REO_DECAY_US;
		else
			r->reo_us = p->cfg.reo_ms * 1000;
	}
}

void cg_pctl_peer_gone(struct cg_pctl *p, uint32_t peer, uint32_t sid)
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

int cg_pctl_peers(struct cg_pctl *p, uint32_t n)
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

struct cg_pctl *cg_pctl_new(const struct cg_pctl_cfg *cfg, int ep, const struct cg_pctl_ops *ops, char *err,
			  size_t errlen)
{
	struct cg_pctl *p = calloc(1, sizeof(*p));
	uint32_t nch, tsz;

	if (!p) {
		snprintf(err, errlen, "protocontroller: out of memory");
		return NULL;
	}
	p->cfg = *cfg;
	p->ops = *ops;
	p->ep = ep;
	p->lfd[0] = p->lfd[1] = p->lfd[2] = -1;
	p->cc_auto = cfg->server && !strcmp(cfg->cc, "auto");
	p->buf_cap = (uint64_t)cfg->buf_kb * 1024;
	if (p->buf_cap > CG_ST_MAXWIN)
		p->buf_cap = CG_ST_MAXWIN;
	p->ccf.queue_us = cfg->queue_ms * 1000;
	p->ccf.mss = cfg->mss;
	p->reo_cap = cg_scc_reo_cap(cfg->reo_ms * 1000, cfg->code_ms * 1000);
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
	    !p->tval || cg_pctl_peers(p, cfg->server ? (cfg->peers ? cfg->peers : 1) : 1) < 0) {
		snprintf(err, errlen, "protocontroller: out of memory (protocontroller_mem_mb %u, protocontroller_max_flows %u)", cfg->mem_mb, cfg->max_flows);
		cg_pctl_free(p);
		return NULL;
	}
	cg_st_pool_init(&p->pool, p->mem, p->stack, nch);
	cg_pctl_tab_init(&p->tab, p->tkey, p->tval, tsz);
	for (uint32_t i = 0; i < cfg->max_flows; i++) {
		p->freeslot[i] = cfg->max_flows - 1 - i;
		p->gen[i] = 1;
		p->f[i].fd = -1;
	}
	p->nfree = cfg->max_flows;
	if (cfg->server && cfg->a_bind.ss_family && listen_a(p, err, errlen) < 0) {
		cg_pctl_free(p);
		return NULL;
	}
	if (!cfg->server) {
		if (listen_one(p, AF_INET, err, errlen) < 0) {
			cg_pctl_free(p);
			return NULL;
		}
		if (listen_one(p, AF_INET6, err, errlen) < 0) {
			cg_warn("%s: IPv4 only", err);
			err[0] = '\0';
		}
	}
	return p;
}

void cg_pctl_free(struct cg_pctl *p)
{
	if (!p)
		return;
	while (p->nused)
		flow_free(p, &p->f[p->used[0]], 1);
	for (int i = 0; i < 3; i++)
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

void cg_pctl_json(const struct cg_pctl *p, struct cg_json *j)
{
	uint64_t txb = 0, rxb = 0, ab = 0;

	cg_json_obj(j, "protocontroller");
	if (!p->cfg.server) {
		cg_json_str(j, "mode", p->cfg.mode == CG_PCTL_A ? "a" : "b");
		/* the middle leg's protocol: a plain TCP (A) or GRIT (B) */
		cg_json_str(j, "protocol", p->cfg.mode == CG_PCTL_A ? "TCP" : "GRIT");
	}
	cg_json_u64(j, "flows", p->nused);
	cg_json_u64(j, "opened", p->opened);
	cg_json_u64(j, "closed", p->closed);
	cg_json_obj(j, "refused");
	for (unsigned r = 1; r < CG_PCTL_R_N; r++)
		if (p->refused[r])
			cg_json_u64(j, cg_pctl_reason(r), p->refused[r]);
	cg_json_end(j, '}');
	cg_json_obj(j, "rst_rx");
	for (unsigned r = 0; r < CG_PCTL_R_N; r++)
		if (p->rst_rx[r])
			cg_json_u64(j, cg_pctl_reason(r), p->rst_rx[r]);
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
	/* the segments sent again by why they were lost: RACK, the timer, the
	 * tunnel's NACK (the rescue hands the loss over, story 025) */
	cg_json_obj(j, "retx_by");
	cg_json_u64(j, "rack", p->retx_by[CG_ST_BY_RACK]);
	cg_json_u64(j, "rto", p->retx_by[CG_ST_BY_RTO]);
	cg_json_u64(j, "nack", p->retx_by[CG_ST_BY_NACK]);
	cg_json_end(j, '}');
	/* the rescue's NACKs of this end's messages: a DATA marked lost, or
	 * already delivered (a DSACK), sent again since (an older copy asked
	 * for), lost already, no such segment; of a stream gone; of an ACK */
	cg_json_obj(j, "nacks");
	cg_json_u64(j, "lost", p->nack[CG_ST_NACK_LOST]);
	cg_json_u64(j, "delivered", p->nack[CG_ST_NACK_DELIVERED]);
	cg_json_u64(j, "stale", p->nack[CG_ST_NACK_STALE]);
	cg_json_u64(j, "pending", p->nack[CG_ST_NACK_PENDING]);
	cg_json_u64(j, "unknown", p->nack[CG_ST_NACK_UNKNOWN]);
	cg_json_u64(j, "no_stream", p->nack_noflow);
	cg_json_u64(j, "acks", p->nack_ack);
	/* the segments found lost, by the link they went on (the server: the
	 * path index, every session together); then copied or not known */
	cg_json_arr(j, "lost_by_link");
	for (unsigned l = 0; l < CG_PCTL_LINKS; l++)
		cg_json_u64(j, NULL, p->nack_link[l]);
	cg_json_end(j, ']');
	cg_json_u64(j, "lost_no_link", p->nack_link[CG_PCTL_LINKS]);
	cg_json_end(j, '}');
	cg_json_ms(j, "reo_cap_ms", p->reo_cap);
	{
		/* queues the cuts did not drain: the delay's base moved up (streamcc.h's cg_scc_stuck) */
		uint64_t nr = 0;

		for (uint32_t i = 0; i < p->npr; i++)
			if (p->pr[i].used)
				nr += p->pr[i].cc.n_rebase;
		cg_json_u64(j, "rebases", nr);
	}
	cg_json_u64(j, "copies", p->copies);
	cg_json_u64(j, "rto", p->rto_n);
	cg_json_u64(j, "dup_frames", p->dup_frames);
	cg_json_u64(j, "ooo_frames", p->ooo_frames);
	cg_json_u64(j, "drop_frames", p->drop_frames);
	cg_json_u64(j, "nomem", p->nomem);
	if (p->cc_auto)
		cg_json_u64(j, "cc_to_bbr", p->cc_switches);
	cg_json_arr(j, "holes_ms"); /* < 5, 10, 20, 40, 80, 160, 320, more */
	for (unsigned b = 0; b < PCTL_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->holes[b]);
	cg_json_end(j, ']');
	cg_json_arr(j, "sendq_ms"); /* < 1, 2, 4, 8, 16, 32, 64, more */
	for (unsigned b = 0; b < PCTL_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->sendq[b]);
	cg_json_end(j, ']');
	cg_json_arr(j, "writeq_ms"); /* < 1, 2, 4, 8, 16, 32, 64, more */
	for (unsigned b = 0; b < PCTL_HOLE_BUCKETS; b++)
		cg_json_u64(j, NULL, p->writeq[b]);
	cg_json_end(j, ']');
	for (uint32_t i = 0; i < p->nused; i++) {
		const struct flow *f = &p->f[p->used[i]];

		if (f->relay) {
			ab += f->d[0].in - f->d[0].out + f->d[1].in - f->d[1].out;
			continue;
		}
		txb += f->tx.tail > f->tx.una ? f->tx.tail - f->tx.una : 0;
		rxb += f->rx.nxt > f->consumed ? f->rx.nxt - f->consumed : 0;
	}
	cg_json_u64(j, "a_opened", p->a_opened); /* variant A's connections (a plain TCP in the middle) */
	cg_json_u64(j, "a_bytes_in", p->a_bytes_in);
	cg_json_u64(j, "a_bytes_out", p->a_bytes_out);
	cg_json_u64(j, "a_held_bytes", ab);
	cg_json_u64(j, "held_tx_bytes", txb);
	cg_json_u64(j, "held_rx_bytes", rxb);
	cg_json_u64(j, "mem_bytes", (uint64_t)cg_st_pool_used(&p->pool) * CG_ST_CHUNK);
	cg_json_u64(j, "mem_peak_bytes", (uint64_t)p->pool.peak * CG_ST_CHUNK);
	cg_json_arr(j, "pacers");
	for (uint32_t i = 0; i < p->npr; i++) {
		const struct cg_scc *c = &p->pr[i].cc;

		if (!p->pr[i].used)
			continue;
		cg_json_obj(j, NULL);
		cg_json_u64(j, "peer", i);
		cg_json_u64(j, "bw_bps", cg_scc_bw(c) * 8);
		cg_json_u64(j, "rate_bps", cg_scc_rate(c) * 8);
		cg_json_u64(j, "hint_bps", c->hint * 8);
		cg_json_bool(j, "hint_full", c->hint_full);
		cg_json_ms(j, "min_rtt_ms", c->min_rtt);
		cg_json_ms(j, "srtt_ms", c->srtt);
		cg_json_u64(j, "inflight", c->inflight);
		cg_json_u64(j, "cwnd", cg_scc_cwnd(c, &p->ccf, cg_scc_reo(c, peer_reo(p, &p->pr[i]))));
		cg_json_str(j, "mode", c->mode == CG_SCC_STARTUP_M ? "startup" : c->mode == CG_SCC_DRAIN_M ? "drain" : "probe");
		cg_json_ms(j, "reo_ms", peer_reo(p, &p->pr[i]));
		cg_json_bool(j, "heavy_loss", c->heavy);
		cg_json_u64(j, "heavy_windows", c->n_heavy);
		if (c->have_owd)
			cg_json_ms_signed(j, "owd_queue_ms", (int32_t)((uint32_t)c->owd_last - (uint32_t)c->owd_base));
		cg_json_u64(j, "dup_acks", p->pr[i].dups);
		cg_json_u64(j, "congested", c->n_cong);
		cg_json_u64(j, "cuts", c->n_cut);
		cg_json_u64(j, "rebases", c->n_rebase);
		/* what each link adds to the delay of what comes from this peer
		 * (linkowd.h): taken off before the ACK's least */
		{
			const struct cg_lowd *o = &p->pr[i].lo;

			cg_json_u64(j, "owd_samples", o->samples);
			cg_json_u64(j, "owd_corrected", o->corrected);
			cg_json_arr(j, "link_extra_ms");
			for (unsigned k = 0; k < CG_LOWD_LINKS; k++) {
				int ok;

				cg_lowd_base(o, k, &ok);
				if (ok)
					cg_json_ms(j, NULL, cg_lowd_extra(o, k));
				else
					cg_json_null(j, NULL);
			}
			cg_json_end(j, ']');
		}
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_end(j, '}');
}
