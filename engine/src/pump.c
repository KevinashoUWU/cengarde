/* Router link pumps (pump.h).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "pump.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

int cg_pump_init(struct cg_pump *p, int threaded, int ep, uint32_t rxq, uint32_t cmdq)
{
	memset(p, 0, sizeof(*p));
	p->ep = ep;
	p->bell.efd = -1;
	p->threaded = threaded;
	p->cpu = -1;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		p->fd[i] = -1;
	if (cg_ring_init(&p->rxq, rxq, sizeof(struct cg_rxslot)) < 0)
		return -1;
	if (threaded && cg_ring_init(&p->cmdq, cmdq, sizeof(uint32_t)) < 0) {
		cg_ring_free(&p->rxq);
		return -1;
	}
	return 0;
}

/* Lets go of the socket of link, if it holds one: the only place a link
 * socket handed to a pump is closed. */
static void pump_drop(struct cg_pump *p, unsigned link)
{
	uint16_t bit = (uint16_t)(1u << link);

	if (!(p->held & bit))
		return;
	if (!(p->failed & bit))
		epoll_ctl(p->ep, EPOLL_CTL_DEL, p->fd[link], NULL);
	close(p->fd[link]);
	p->fd[link] = -1;
	p->held &= (uint16_t)~bit;
	p->failed &= (uint16_t)~bit;
}

void cg_pump_free(struct cg_pump *p)
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		pump_drop(p, i);
	cg_ring_free(&p->rxq);
	cg_ring_free(&p->cmdq);
}

void cg_pump_cmd(struct cg_pump *p, const struct cg_pump_cmd *c)
{
	unsigned l = c->link;
	uint16_t bit = (uint16_t)(1u << l);
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = CG_EV(CG_EV_LINK, l) };

	if (l >= CG_MAX_LINKS)
		return;
	switch (c->op) {
	case CG_PUMP_OPEN:
		pump_drop(p, l); /* a new socket replaces the one it held */
		p->fd[l] = c->fd;
		p->gen[l] = c->gen;
		p->held |= bit;
		if (p->paused)
			ev.events = 0;
		if (epoll_ctl(p->ep, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
			/* Kept, unpolled, until the hub's CLOSE: until it has seen
			 * the failure the hub may still use the number. */
			p->failed |= bit;
			atomic_fetch_or_explicit(&p->open_failed, bit, memory_order_relaxed);
		}
		break;
	case CG_PUMP_CLOSE:
		pump_drop(p, l);
		break;
	case CG_PUMP_MODE: /* reserved: every link drops on a full socket until bonding */
		break;
	case CG_PUMP_STOP:
		atomic_store_explicit(&p->stop, 1, memory_order_relaxed);
		break;
	}
}

int cg_pump_rx(struct cg_pump *p, unsigned link, int rounds)
{
	int fd = p->fd[link], total = 0;

	for (int round = 0; round < rounds; round++) {
		uint32_t room = cg_ring_room(&p->rxq, CG_BATCH), want, prod;
		uint64_t now_us;
		int n;

		if (!room)
			break; /* inline: the hub drains it right after */
		want = room < CG_BATCH ? room : CG_BATCH;
		prod = cg_ring_prod(&p->rxq);
		for (uint32_t i = 0; i < want; i++) {
			struct cg_rxslot *s = cg_ring_at(&p->rxq, prod + i);

			p->iov[i].iov_base = s->buf;
			p->iov[i].iov_len = CG_BUF;
			memset(&p->msg[i].msg_hdr, 0, sizeof(p->msg[i].msg_hdr));
			p->msg[i].msg_hdr.msg_iov = &p->iov[i];
			p->msg[i].msg_hdr.msg_iovlen = 1;
		}
		n = recvmmsg(fd, p->msg, want, MSG_DONTWAIT, NULL);
		if (n < 0 && (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH))
			continue; /* an ICMP error from an earlier send; the queue may hold more */
		if (n < 0 && errno != EAGAIN && errno != EINTR) {
			atomic_store_explicit(&p->rx_errno, errno, memory_order_relaxed);
			atomic_store_explicit(&p->rx_errors, ++p->n_errors, memory_order_relaxed);
		}
		if (n <= 0)
			break;
		now_us = cg_now_us();
		for (int i = 0; i < n; i++) {
			struct cg_rxslot *s = cg_ring_at(&p->rxq, prod + (uint32_t)i);

			s->t_us = now_us;
			s->len = (uint16_t)p->msg[i].msg_len;
			s->link = (uint8_t)link;
			s->gen = p->gen[link];
			s->trunc = !!(p->msg[i].msg_hdr.msg_flags & MSG_TRUNC);
		}
		cg_ring_publish(&p->rxq, (uint32_t)n);
		p->n_pkts += (uint32_t)n;
		atomic_store_explicit(&p->rx_pkts, p->n_pkts, memory_order_relaxed);
		total += n;
		if ((uint32_t)n < want)
			break;
	}
	return total;
}

int cg_pump_post(struct cg_pump *p, const struct cg_pump_cmd *c)
{
	cg_pump_cmd(p, c);
	return 0;
}
