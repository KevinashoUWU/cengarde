/* Router link pumps (pump.h).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "pump.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

int cg_pump_init(struct cg_pump *p, int threaded, int ep, uint32_t rxq, uint32_t cmdq)
{
	struct epoll_event ev = { .events = EPOLLIN };

	memset(p, 0, sizeof(*p));
	p->ep = ep;
	p->bell.efd = -1;
	p->threaded = threaded;
	p->cpu = -1;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		p->fd[i] = -1;
	if (cg_ring_init(&p->rxq, rxq, sizeof(struct cg_rxslot)) < 0)
		return -1;
	if (!threaded)
		return 0;
	/* A thread of its own: its command ring, its epoll, and its bell in it. */
	p->ep = epoll_create1(EPOLL_CLOEXEC);
	ev.data.u64 = CG_EV(CG_EV_BELL, 0);
	if (cg_ring_init(&p->cmdq, cmdq, sizeof(uint32_t)) < 0 || p->ep < 0 || cg_bell_init(&p->bell) < 0 ||
	    epoll_ctl(p->ep, EPOLL_CTL_ADD, p->bell.efd, &ev) < 0) {
		cg_pump_free(p);
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
	if (p->threaded) {
		cg_bell_free(&p->bell);
		if (p->ep >= 0)
			close(p->ep);
		p->ep = -1;
	}
}

/* With its receive ring full a pump thread takes its sockets out of the
 * poll (the kernel keeps the backlog), and puts them back once the hub made
 * room. */
static void pump_poll(struct cg_pump *p, int on)
{
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct epoll_event ev = { .events = on ? EPOLLIN : 0, .data.u64 = CG_EV(CG_EV_LINK, i) };

		if ((p->held >> i & 1) && !(p->failed >> i & 1))
			epoll_ctl(p->ep, EPOLL_CTL_MOD, p->fd[i], &ev);
	}
	p->paused = !on;
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

		if (!room) {
			if (!p->threaded)
				break; /* inline: the hub drains it right after */
			if (!cg_ring_block(&p->rxq, &p->rx_blocked))
				continue; /* the hub made room meanwhile */
			pump_poll(p, 0);
			atomic_store_explicit(&p->rx_paused, ++p->n_paused, memory_order_relaxed);
			if (p->hook)
				p->hook(p, CG_PUMP_HOOK_BLOCKED);
			break;
		}
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
		if (p->threaded)
			cg_bell_ring(p->hub);
		p->n_pkts += (uint32_t)n;
		atomic_store_explicit(&p->rx_pkts, p->n_pkts, memory_order_relaxed);
		total += n;
		if ((uint32_t)n < want)
			break;
	}
	return total;
}

/* Forgets the commands the pump took: the hub's view of cmdq. */
static void pump_cmds_taken(struct cg_pump *p)
{
	uint32_t cons = atomic_load_explicit(&p->cmdq.cons, memory_order_acquire);

	while (p->cmd_tail != p->cmd_head && (int32_t)(cons - p->cmd_at[p->cmd_tail % CG_PUMP_CMDS]) > 0)
		p->cmd_tail++;
}

uint32_t cg_pump_cmds_waiting(struct cg_pump *p)
{
	if (!p->threaded)
		return 0;
	pump_cmds_taken(p);
	return p->cmd_head - p->cmd_tail;
}

int cg_pump_post(struct cg_pump *p, const struct cg_pump_cmd *c)
{
	uint32_t idx, prod;

	if (!p->threaded) {
		cg_pump_cmd(p, c);
		return 0;
	}
	/* A command slot is free once the pump took the entry that named it:
	 * it read the slot before it released the entry. */
	if (cg_pump_cmds_waiting(p) >= CG_PUMP_CMDS || !cg_ring_room(&p->cmdq, 1))
		return -1;
	idx = p->cmd_head % CG_PUMP_CMDS;
	p->cmd[idx] = *c;
	prod = cg_ring_prod(&p->cmdq);
	*(uint32_t *)cg_ring_at(&p->cmdq, prod) = CG_PUMP_ENT_CMD | idx;
	p->cmd_at[idx] = prod;
	p->cmd_head++;
	cg_ring_publish(&p->cmdq, 1);
	cg_bell_ring(&p->bell);
	return 0;
}

/* ---- the thread ---- */

/* Runs the commands the hub posted. Returns 1 on STOP. */
static int pump_take(struct cg_pump *p)
{
	uint32_t n = cg_ring_avail(&p->cmdq), c = cg_ring_cons(&p->cmdq);

	for (uint32_t i = 0; i < n; i++) {
		uint32_t e = *(const uint32_t *)cg_ring_at(&p->cmdq, c + i);

		if (e & CG_PUMP_ENT_CMD)
			cg_pump_cmd(p, &p->cmd[e % CG_PUMP_CMDS]);
		/* else a packet to send: PR 3c */
	}
	if (n)
		cg_ring_release(&p->cmdq, n);
	return (int)atomic_load_explicit(&p->stop, memory_order_relaxed);
}

/* Scheduling of the thread: it starts with whatever the hub has (the cpu
 * and rt_priority knobs are the hub's), so back to the process's CPUs, then
 * its own [link] cpu and the rt_priority of every data thread. */
static void pump_tune(struct cg_pump *p)
{
	struct sched_param sp = { .sched_priority = (int)p->rt_priority };
	cpu_set_t set;

	if (p->cpu >= 0) {
		CPU_ZERO(&set);
		CPU_SET(p->cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) < 0)
			cg_warn("%s: cpu %d: %s", p->name, p->cpu, strerror(errno));
	} else if (p->have_cpus) {
		sched_setaffinity(0, sizeof(p->cpus), &p->cpus);
	}
	if (!p->rt_priority)
		sp.sched_priority = 0;
	if (pthread_setschedparam(pthread_self(), p->rt_priority ? SCHED_FIFO : SCHED_OTHER, &sp) && p->rt_priority)
		cg_warn("%s: rt_priority %u refused", p->name, p->rt_priority);
}

static void *pump_main(void *arg)
{
	struct cg_pump *p = arg;
	struct epoll_event ev[CG_MAX_LINKS + 1];
	uint64_t last_traffic_us = 0;

	atomic_store_explicit(&p->tid, (int)syscall(SYS_gettid), memory_order_relaxed);
	pthread_setname_np(pthread_self(), p->name);
	pump_tune(p);
	for (;;) {
		uint64_t now_us = cg_now_us();
		int n, timeout = -1, armed = 0, traffic = 0;

		atomic_store_explicit(&p->loop_ms, (uint32_t)(now_us / 1000), memory_order_relaxed);
		if (pump_take(p))
			break;
		/* Awake, and the hub made room: read again. */
		if (p->paused && cg_ring_unblocked(&p->rx_blocked))
			pump_poll(p, 1);
		if (p->busy_poll_us && now_us - last_traffic_us < p->busy_poll_us) {
			timeout = 0;
		} else {
			if (p->hook)
				p->hook(p, CG_PUMP_HOOK_SLEEP);
			/* Pre-sleep check: commands, or room again in its ring
			 * (ring.h); either way, it does not sleep. */
			cg_bell_arm(&p->bell);
			armed = 1;
			if (cg_ring_has_work_sc(&p->cmdq) ||
			    (p->paused && !(p->test_flags & CG_PUMP_TEST_NO_PRESLEEP) && cg_ring_unblocked(&p->rx_blocked)))
				timeout = 0;
		}
		n = epoll_wait(p->ep, ev, (int)(sizeof(ev) / sizeof(ev[0])), timeout);
		if (armed)
			cg_bell_disarm(&p->bell);
		for (int i = 0; i < n; i++) {
			uint32_t kind = (uint32_t)(ev[i].data.u64 >> 32), idx = (uint32_t)ev[i].data.u64;

			if (kind == CG_EV_BELL) {
				cg_bell_drain(&p->bell);
			} else if (kind == CG_EV_LINK && idx < CG_MAX_LINKS && p->fd[idx] >= 0) {
				if (!p->paused && cg_pump_rx(p, idx, CG_MAX_ROUNDS) > 0) {
					traffic = 1;
				} else if (p->paused && (ev[i].events & EPOLLERR)) {
					/* An ICMP error is reported even out of the poll:
					 * take it, or this loop would spin until it reads. */
					int e;
					socklen_t len = sizeof(e);

					getsockopt(p->fd[idx], SOL_SOCKET, SO_ERROR, &e, &len);
				}
			}
		}
		if (traffic && p->busy_poll_us)
			last_traffic_us = cg_now_us();
	}
	return NULL;
}

int cg_pump_start(struct cg_pump *p, const char *name, struct cg_bell *hub)
{
	pthread_attr_t attr;
	int rc;

	snprintf(p->name, sizeof(p->name), "%s", name);
	p->hub = hub;
	if (pthread_attr_init(&attr))
		return -1;
	pthread_attr_setstacksize(&attr, CG_PUMP_STACK);
	rc = pthread_create(&p->thread, &attr, pump_main, p);
	pthread_attr_destroy(&attr);
	if (rc)
		return -1;
	p->started = 1;
	return 0;
}

int cg_pump_stop(struct cg_pump *p, int timeout_ms)
{
	struct cg_pump_cmd stop = { .op = CG_PUMP_STOP };
	struct timespec ts;

	if (!p->started)
		return 0;
	/* Through the ring when there is room, so that what was posted before
	 * runs first; the flag and the eventfd in any case. */
	cg_pump_post(p, &stop);
	atomic_store(&p->stop, 1);
	cg_efd_write(p->bell.efd);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	if (pthread_timedjoin_np(p->thread, NULL, &ts))
		return -1;
	p->started = 0;
	return 0;
}

uint64_t cg_pump_cpu_ns(struct cg_pump *p)
{
	struct timespec ts;
	clockid_t clk;

	if (!p->started || pthread_getcpuclockid(p->thread, &clk) || clock_gettime(clk, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
