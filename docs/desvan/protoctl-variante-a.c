/* docs/desvan/protoctl-variante-a.c: the protocontroller's variant A: TCP split at the router and the VPS with a plain TCP of the engines' through the tunnel in the middle (protocontroller = a)
 *
 * Out of the engine in 0.17 (story 029, step 4: the proxy by default):
 * GRIT (variant B) beat it in the field (2026-10-29: the download +9 % with
 * a lower ping and less data, the upload +12 %, in redundancy) and in the
 * lab (stories 024, 025), and is the protocontroller's only middle leg
 * now. It lived in engine/src/protoctl.c (the relays: struct flow's relay,
 * mfd, hdr and d[2]; a_open, a_accept, a_move, a_service, a_event,
 * listen_a; CG_PCTL_MID in the epoll index), protoctlio.h (cfg.mode,
 * a_peer, a_bind, a_len, mid_cc), client.c (pctl_vps_addr, f3_ins_port),
 * server.c (pc.a_bind from tun_addr), fec3.h (cg_f3_tcp_port, for
 * cg_f3_mix) and config.c (protocontroller_mid_cc, protocontroller_vps)
 * until commit 8eb670f, v0.16.0 (`git show 8eb670f:<file>`). The lab's
 * BOND_PEP=cga and BOND_PEP_CGMIDCC ran it.
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* From protoctl.c, its header's constants:
 *	#define CG_PCTL_AHDR_MAX 20    // variant A's header: magic, family, port, address
 *	#define PCTL_AMAGIC 0xA5
 * and struct flow's fields:
 *	// variant A (relay): fd is the LAN's (router) or the destination's (VPS)
 *	// socket, mfd the middle TCP through the tunnel; sb carries fd -> mfd,
 *	// rb mfd -> fd
 *	uint8_t relay, mreg, mhup, mconn, hdr_n, hdr_want, m_rd, m_wb;
 *	uint32_t mev;
 *	int mfd;
 *	uint8_t hdr[CG_PCTL_AHDR_MAX];
 *	struct rdir { uint64_t in, out; uint8_t eof, shut, nomem; } d[2];
 * The router took it in accept_some (p->cfg.mode == CG_PCTL_A: a_open), the
 * VPS in cg_pctl_event (idx CG_PCTL_LISTEN_IDX + 2: a_accept); a relay's
 * events went to a_event, its service to a_service. */

/* ---- protoctl.c ---- */

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
	set_rcvbuf(fd, (int)p->buf_cap);
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
		if ((why = cg_pctl_dest_ok(f->family, f->addr)) || (f->fd = dest_connect(p, f->family, f->addr, f->port, 0, &why)) < 0) {
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


/* ---- client.c ---- */

/* Variant A: where the middle TCP goes, the VPS's tunnel address
 * (protocontroller_vps; empty: the first address of tun_addr's prefix, the
 * VPS's by convention, story 010) and protocontroller_port. 0 or -1. */
static int pctl_vps_addr(const struct cg_config *cfg, struct sockaddr_storage *a, char *err, size_t errlen)
{
	uint8_t ad[16];
	int fam = 0;

	memset(a, 0, sizeof(*a));
	if (cfg->protocontroller_vps[0]) {
		if (inet_pton(AF_INET, cfg->protocontroller_vps, ad) == 1)
			fam = AF_INET;
		else if (inet_pton(AF_INET6, cfg->protocontroller_vps, ad) == 1)
			fam = AF_INET6;
		else {
			snprintf(err, errlen, "protocontroller_vps: '%s' is not an IP address", cfg->protocontroller_vps);
			return -1;
		}
	} else {
		for (int i = 0; i < cfg->ntun_addr && !fam; i++) {
			const struct cg_pfx *t = &cfg->tun_addr[i];
			unsigned n = t->family == AF_INET ? 4 : 16, len = t->len;

			if (t->family == AF_INET6 && i + 1 < cfg->ntun_addr)
				continue; /* IPv4 first */
			if (len + 1 >= n * 8) { /* OpenWrt's /32: no prefix to take it from */
				snprintf(err, errlen, "protocontroller = a: tun_addr is a single address; set protocontroller_vps");
				return -1;
			}
			memcpy(ad, t->a, n);
			for (unsigned b = len; b < n * 8; b++)
				ad[b / 8] &= (uint8_t)~(0x80u >> (b % 8));
			ad[n - 1] |= 1;
			if (!memcmp(ad, t->a, n)) {
				snprintf(err, errlen, "protocontroller = a: tun_addr is the VPS's own; set protocontroller_vps");
				return -1;
			}
			fam = t->family;
		}
		if (!fam) {
			snprintf(err, errlen, "protocontroller = a: no tun_addr to take the VPS's from; set protocontroller_vps");
			return -1;
		}
	}
	if (fam == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)a;

		s->sin_family = AF_INET;
		memcpy(&s->sin_addr, ad, 4);
		s->sin_port = htons((uint16_t)cfg->protocontroller_port);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)a;

		s->sin6_family = AF_INET6;
		memcpy(&s->sin6_addr, ad, 16);
		s->sin6_port = htons((uint16_t)cfg->protocontroller_port);
	}
	return 0;
}
