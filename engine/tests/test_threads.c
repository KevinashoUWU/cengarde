/* The concurrency primitives under real threads (also run under TSAN, on an
 * AArch64 runner and under qemu):
 * - ring stress: a producer and a consumer pass CG_TEST_RING_N entries (10^7
 *   by default) in random batches, each checked against the sequence it
 *   should carry; once with a consumer that sleeps on its bell (an eventfd)
 *   and once with one that spins without any eventfd, so that the atomics
 *   alone must order the payload (TSAN treats an eventfd as a
 *   synchronisation and would hide a missing barrier);
 * - lost wake-up: the sleeping consumer waits with a 1 s timeout, and a
 *   timeout that finds entries waiting fails the test;
 * - a router pump thread (pump.h) with real sockets: commands in order,
 *   slots stamped with (link, gen), the pump closing what it was handed;
 * - the space handshake's race (ring.h), forced with the pump's test hook:
 *   the pump stores rx_blocked and finds its ring still full, then the hub
 *   frees room and clears rx_blocked before the pump sets sleeping. The
 *   pump must read again within 1 s with both safeguards, and with either
 *   one alone; with both taken out it must not (the test sees the bug);
 * - open_failed: a socket the pump's epoll refuses comes back to the hub,
 *   stays open until the hub's CLOSE, and is closed by it;
 * - a stalled pump: at most CG_PUMP_CMDS commands in flight, and the
 *   pending state of a link meanwhile (cg_pump_send, as the client's
 *   link_cmd uses it): only the newest state waits, a socket the pump never
 *   saw goes back to the hub to close, the one the pump holds never does,
 *   and the newest state reaches the pump once it runs again.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pump.h"
#include "ring.h"
#include "test.h"

#define RING_SIZE 256

struct ent {
	uint64_t seq;
	uint64_t check; /* ~seq * odd: a torn or stale entry does not match */
};

struct stress {
	struct cg_ring r;
	struct cg_bell bell;
	int sleeping_consumer;
	uint64_t n;
	/* the consumer's results, read after the join */
	uint64_t got, bad, lost_wakeups, sleeps;
	_Atomic uint32_t abort;
};

static uint64_t check_of(uint64_t seq)
{
	return ~seq * 0x9e3779b97f4a7c15ull;
}

static uint32_t rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return (uint32_t)*s;
}

static void *producer(void *arg)
{
	struct stress *t = arg;
	uint64_t seq = 0, seed = 0x1234567;

	while (seq < t->n && !atomic_load_explicit(&t->abort, memory_order_relaxed)) {
		uint32_t want = 1 + rnd(&seed) % 64, room = cg_ring_room(&t->r, want), p;

		if (!room) {
			sched_yield();
			continue;
		}
		if (want > room)
			want = room;
		if (want > t->n - seq)
			want = (uint32_t)(t->n - seq);
		p = cg_ring_prod(&t->r);
		for (uint32_t i = 0; i < want; i++) {
			struct ent *e = cg_ring_at(&t->r, p + i);

			e->seq = seq + i;
			e->check = check_of(seq + i);
		}
		cg_ring_publish(&t->r, want);
		seq += want;
		if (t->sleeping_consumer)
			cg_bell_ring(&t->bell);
	}
	return NULL;
}

static void *consumer(void *arg)
{
	struct stress *t = arg;
	uint64_t spins = 0;

	while (t->got < t->n) {
		uint32_t n = cg_ring_avail(&t->r), c = cg_ring_cons(&t->r);

		if (!n) {
			struct pollfd pfd = { .fd = t->bell.efd, .events = POLLIN };

			if (!t->sleeping_consumer) {
				if (++spins % 64 == 0)
					sched_yield();
				continue;
			}
			cg_bell_arm(&t->bell);
			if (cg_ring_has_work_sc(&t->r)) {
				cg_bell_disarm(&t->bell);
				continue;
			}
			t->sleeps++;
			if (poll(&pfd, 1, 1000) == 0 && cg_ring_has_work_sc(&t->r)) {
				t->lost_wakeups++;
				break;
			}
			cg_bell_drain(&t->bell);
			continue;
		}
		for (uint32_t i = 0; i < n; i++) {
			const struct ent *e = cg_ring_at(&t->r, c + i);

			if (e->seq != t->got + i || e->check != check_of(t->got + i))
				t->bad++;
		}
		cg_ring_release(&t->r, n);
		t->got += n;
		if (t->bad)
			break;
	}
	atomic_store(&t->abort, 1);
	return NULL;
}

static uint64_t ring_n(void)
{
	const char *v = getenv("CG_TEST_RING_N");

	return v && *v ? strtoull(v, NULL, 10) : 10000000ull;
}

static void stress(int sleeping)
{
	static struct stress t;
	pthread_t pt, ct;

	memset(&t, 0, sizeof(t));
	CHECK_EQ(cg_ring_init(&t.r, RING_SIZE, sizeof(struct ent)), 0);
	CHECK_EQ(cg_bell_init(&t.bell), 0);
	t.sleeping_consumer = sleeping;
	t.n = ring_n();
	atomic_init(&t.abort, 0);
	CHECK_EQ(pthread_create(&ct, NULL, consumer, &t), 0);
	CHECK_EQ(pthread_create(&pt, NULL, producer, &t), 0);
	pthread_join(pt, NULL);
	pthread_join(ct, NULL);
	CHECK_EQ(t.got, t.n);
	CHECK_EQ(t.bad, 0);
	CHECK_EQ(t.lost_wakeups, 0);
	cg_bell_free(&t.bell);
	cg_ring_free(&t.r);
}

/* ---- a pump thread ---- */

/* A UDP pair on loopback: *a stands for a link socket (connected, as
 * cg_udp_link makes them), *b for the server sending to it. */
static int udp_pair(int *a, int *b)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }, sb = sa;
	socklen_t len = sizeof(sa);

	*a = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	*b = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (*a < 0 || *b < 0 || bind(*a, (struct sockaddr *)&sa, sizeof(sa)) || bind(*b, (struct sockaddr *)&sb, sizeof(sb)) ||
	    getsockname(*a, (struct sockaddr *)&sa, &len) || (len = sizeof(sb), getsockname(*b, (struct sockaddr *)&sb, &len)) ||
	    connect(*a, (struct sockaddr *)&sb, sizeof(sb)) || connect(*b, (struct sockaddr *)&sa, sizeof(sa)))
		return -1;
	return 0;
}

static void send_n(int b, uint32_t first, uint32_t n)
{
	for (uint32_t i = first; i < first + n; i++)
		CHECK_EQ(send(b, &i, sizeof(i), 0), (long long)sizeof(i));
}

static int fd_open(int fd)
{
	return fcntl(fd, F_GETFD) >= 0;
}

static struct cg_pump *pump_alloc(uint32_t rxq)
{
	struct cg_pump *p = aligned_alloc(CG_CACHELINE, (sizeof(*p) + CG_CACHELINE - 1) / CG_CACHELINE * CG_CACHELINE);

	if (p && cg_pump_init(p, 1, -1, rxq, 64) < 0) {
		free(p);
		p = NULL;
	}
	return p;
}

static struct cg_bell hub;

/* The hub's side: sleeps on its bell, at most ms in all, until the pump
 * published something, the way the client's loop does; a wake-up left over
 * from an earlier ring finds nothing and sleeps again. Returns the slots
 * ready, 0 when ms ran out. */
static uint32_t hub_wait(struct cg_pump *p, int ms)
{
	uint64_t end = cg_now_ms() + (uint64_t)ms;

	for (;;) {
		struct pollfd pfd = { .fd = hub.efd, .events = POLLIN };
		uint32_t n = cg_pump_rx_avail(p);
		uint64_t now;

		if (n)
			return n;
		cg_bell_arm(&hub);
		if (cg_ring_has_work_sc(&p->rxq)) {
			cg_bell_disarm(&hub);
			continue;
		}
		now = cg_now_ms();
		if (now >= end) {
			cg_bell_disarm(&hub);
			return 0;
		}
		poll(&pfd, 1, (int)(end - now));
		cg_bell_drain(&hub);
	}
}

/* Takes every slot until want arrived or 1 s of nothing; checks each one's
 * link, generation and payload (the next number). Returns how many. */
static uint32_t hub_take(struct cg_pump *p, uint8_t link, uint8_t gen, uint32_t *next, uint32_t want)
{
	uint32_t got = 0, n;

	while (got < want && (n = hub_wait(p, 1000)) > 0) {
		for (uint32_t i = 0; i < n; i++) {
			const struct cg_rxslot *sl = cg_pump_rx_slot(p, i);
			uint32_t v;

			memcpy(&v, sl->buf, sizeof(v));
			CHECK_EQ(sl->link, link);
			CHECK_EQ(sl->gen, gen);
			CHECK_EQ(sl->len, sizeof(v));
			CHECK(sl->t_us > 0);
			CHECK_EQ(v, *next);
			(*next)++;
		}
		cg_pump_rx_release(p, n);
		got += n;
	}
	return got;
}

/* Waits until the pump took every command posted. */
static int cmds_done(struct cg_pump *p)
{
	for (int i = 0; i < 1000; i++) {
		if (!cg_pump_cmds_waiting(p))
			return 1;
		usleep(1000);
	}
	return 0;
}

static void pump_basics(void)
{
	struct cg_pump *p = pump_alloc(16);
	struct cg_pump_cmd c = { .op = CG_PUMP_OPEN, .link = 2, .gen = 5 };
	int a, b, a2, b2;
	uint32_t next = 0;

	CHECK(p != NULL);
	if (!p)
		return;
	CHECK_EQ(udp_pair(&a, &b), 0);
	CHECK_EQ(udp_pair(&a2, &b2), 0);
	snprintf(p->name, sizeof(p->name), "unused");
	CHECK_EQ(cg_pump_start(p, "cg-test0123456789", &hub), 0);
	CHECK(!strcmp(p->name, "cg-test01234567")); /* 15 characters kept */
	c.fd = a;
	CHECK_EQ(cg_pump_post(p, &c), 0);
	send_n(b, 0, 100); /* more than its ring: it waits for the hub */
	CHECK_EQ(hub_take(p, 2, 5, &next, 100), 100);
	for (int i = 0; i < 1000 && atomic_load(&p->rx_pkts) != 100; i++)
		usleep(1000); /* stored after the publish */
	CHECK_EQ(atomic_load(&p->rx_pkts), 100);
	CHECK(atomic_load(&p->tid) > 0);
	CHECK(cg_pump_cpu_ns(p) > 0);

	/* A new socket for the link: the old one closes, and its slots carry
	 * the new generation. */
	c.fd = a2;
	c.gen = 7;
	CHECK_EQ(cg_pump_post(p, &c), 0);
	CHECK(cmds_done(p));
	CHECK(!fd_open(a));
	send_n(b2, next, 20);
	CHECK_EQ(hub_take(p, 2, 7, &next, 20), 20);
	c.op = CG_PUMP_CLOSE;
	CHECK_EQ(cg_pump_post(p, &c), 0);
	CHECK(cmds_done(p));
	CHECK(!fd_open(a2));
	CHECK_EQ(cg_pump_stop(p, 1000), 0);
	cg_pump_free(p);
	free(p);
	close(b);
	close(b2);
}

/* A stalled pump takes no commands: at most CG_PUMP_CMDS wait. */
static void pump_stalled(void)
{
	struct cg_pump *p = pump_alloc(16);
	struct cg_pump_cmd c = { .op = CG_PUMP_CLOSE, .link = 1 };

	CHECK(p != NULL);
	if (!p)
		return;
	for (int i = 0; i < CG_PUMP_CMDS; i++)
		CHECK_EQ(cg_pump_post(p, &c), 0);
	CHECK_EQ(cg_pump_post(p, &c), -1);
	CHECK_EQ(cg_pump_cmds_waiting(p), CG_PUMP_CMDS);
	CHECK_EQ(cg_pump_start(p, "cg-stalled", &hub), 0); /* it was not running: now it takes them */
	CHECK(cmds_done(p));
	CHECK_EQ(cg_pump_post(p, &c), 0);
	CHECK_EQ(cg_pump_stop(p, 1000), 0);
	cg_pump_free(p);
	free(p);
}

/* Open file descriptors of the process: a socket the pump or the hub
 * forgot to close shows here. */
static int nfds(void)
{
	DIR *d = opendir("/proc/self/fd");
	int n = 0;

	while (d && readdir(d))
		n++;
	if (d)
		closedir(d);
	return n;
}

/* What the pending state's truth table says (cg_pend_keep). */
static void pend_keep(void)
{
	struct cg_pump_cmd pend = { 0 }, open1 = { .op = CG_PUMP_OPEN, .gen = 1, .fd = 41 },
			   open2 = { .op = CG_PUMP_OPEN, .gen = 2, .fd = 42 }, close3 = { .op = CG_PUMP_CLOSE, .gen = 3, .fd = -1 };

	CHECK_EQ(cg_pend_keep(&pend, &close3), -1); /* nothing pending */
	CHECK(pend.op == CG_PUMP_CLOSE && pend.gen == 3);
	CHECK_EQ(cg_pend_keep(&pend, &open1), -1); /* a CLOSE holds no socket */
	CHECK(pend.op == CG_PUMP_OPEN && pend.fd == 41 && pend.gen == 1);
	CHECK_EQ(cg_pend_keep(&pend, &open2), 41); /* an OPEN over an OPEN */
	CHECK(pend.op == CG_PUMP_OPEN && pend.fd == 42 && pend.gen == 2);
	CHECK_EQ(cg_pend_keep(&pend, &close3), 42);
	CHECK(pend.op == CG_PUMP_CLOSE && pend.gen == 3);
}

/* A stalled pump and the pending state of link 0, with real sockets: the
 * pump holds x, and the hub closes, reopens and closes the link while the
 * pump's thread has not started and CG_PUMP_CMDS commands wait. */
static void pump_pending(void)
{
	struct cg_pump *p = pump_alloc(16);
	struct cg_pump_cmd hold = { .op = CG_PUMP_OPEN, .link = 0, .gen = 1 }, filler = { .op = CG_PUMP_CLOSE, .link = 5 };
	struct cg_pump_cmd pend = { 0 }, c = { .link = 0 };
	int x, xpeer, a, a2, b, bpeer, base;
	uint32_t next = 0;
	uint8_t gen = 1;

	CHECK(p != NULL);
	if (!p)
		return;
	CHECK_EQ(udp_pair(&x, &xpeer), 0);
	hold.fd = x;
	cg_pump_cmd(p, &hold); /* an OPEN it took before it stalled */
	for (int i = 0; i < CG_PUMP_CMDS; i++)
		CHECK_EQ(cg_pump_post(p, &filler), 0);

	/* The link closes: pending; x is the pump's, never the hub's. */
	c.op = CG_PUMP_CLOSE;
	c.gen = ++gen;
	c.fd = -1;
	CHECK_EQ(cg_pump_send(p, &pend, &c), -1);
	CHECK(pend.op == CG_PUMP_CLOSE && pend.gen == gen);
	CHECK(fd_open(x));

	/* Reopened and closed again ten times: the OPEN replaces the CLOSE,
	 * and the next CLOSE gives its socket back to the hub. */
	base = nfds();
	for (int i = 0; i < 10; i++) {
		int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		CHECK(s >= 0);
		c.op = CG_PUMP_OPEN;
		c.gen = ++gen;
		c.fd = s;
		CHECK_EQ(cg_pump_send(p, &pend, &c), -1);
		CHECK(pend.op == CG_PUMP_OPEN && pend.fd == s && pend.gen == gen);
		c.op = CG_PUMP_CLOSE;
		c.gen = ++gen;
		c.fd = -1;
		CHECK_EQ(cg_pump_send(p, &pend, &c), s);
		close(s);
		CHECK(pend.op == CG_PUMP_CLOSE && pend.gen == gen);
		CHECK_EQ(nfds(), base);
		CHECK(fd_open(x));
	}

	/* An OPEN over an OPEN: the older socket goes back. */
	a = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	a2 = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(a >= 0 && a2 >= 0);
	c.op = CG_PUMP_OPEN;
	c.gen = ++gen;
	c.fd = a;
	CHECK_EQ(cg_pump_send(p, &pend, &c), -1);
	c.gen = ++gen;
	c.fd = a2;
	CHECK_EQ(cg_pump_send(p, &pend, &c), a);
	close(a);
	CHECK(pend.op == CG_PUMP_OPEN && pend.fd == a2 && pend.gen == gen);

	/* Still stalled: tick's retry posts nothing. */
	cg_pump_send_pending(p, &pend);
	CHECK(pend.op == CG_PUMP_OPEN && pend.fd == a2);

	/* It runs again and takes what was in flight. A newer state that
	 * comes before tick's retry does not jump ahead of the pending one,
	 * though the ring has room: it replaces it. */
	CHECK_EQ(cg_pump_start(p, "cg-pending", &hub), 0);
	CHECK(cmds_done(p));
	CHECK_EQ(udp_pair(&b, &bpeer), 0);
	c.gen = ++gen;
	c.fd = b;
	CHECK_EQ(cg_pump_send(p, &pend, &c), a2);
	close(a2);
	CHECK(pend.op == CG_PUMP_OPEN && pend.fd == b && pend.gen == gen);
	CHECK_EQ(cg_pump_cmds_waiting(p), 0);

	/* The retry posts the newest state, and the pump lets go of x for b,
	 * whose datagrams carry the newest generation. */
	cg_pump_send_pending(p, &pend);
	CHECK_EQ(pend.op, 0);
	CHECK(cmds_done(p));
	CHECK(!fd_open(x));
	CHECK(fd_open(b));
	send_n(bpeer, 0, 5);
	CHECK_EQ(hub_take(p, 0, gen, &next, 5), 5);

	/* Nothing pending: straight to the pump, which closes b. */
	c.op = CG_PUMP_CLOSE;
	c.gen = ++gen;
	c.fd = -1;
	CHECK_EQ(cg_pump_send(p, &pend, &c), -1);
	CHECK_EQ(pend.op, 0);
	CHECK(cmds_done(p));
	CHECK(!fd_open(b));
	CHECK_EQ(cg_pump_stop(p, 1000), 0);
	cg_pump_free(p);
	free(p);
	close(xpeer);
	close(bpeer);
}

/* open_failed: /dev/null cannot be polled. */
static void pump_open_failed(void)
{
	struct cg_pump *p = pump_alloc(16);
	struct cg_pump_cmd c = { .op = CG_PUMP_OPEN, .link = 3, .gen = 1 };
	uint32_t m = 0;

	CHECK(p != NULL);
	if (!p)
		return;
	c.fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	CHECK(c.fd >= 0);
	CHECK_EQ(cg_pump_start(p, "cg-failed", &hub), 0);
	CHECK_EQ(cg_pump_post(p, &c), 0);
	for (int i = 0; i < 1000 && !m; i++) {
		m = atomic_load_explicit(&p->open_failed, memory_order_relaxed) ? atomic_exchange(&p->open_failed, 0) : 0;
		if (!m)
			usleep(1000);
	}
	CHECK_EQ(m, 1u << 3);
	CHECK(fd_open(c.fd)); /* kept until the hub's CLOSE: the hub may still use the number */
	c.op = CG_PUMP_CLOSE;
	CHECK_EQ(cg_pump_post(p, &c), 0);
	CHECK(cmds_done(p));
	CHECK(!fd_open(c.fd));
	CHECK_EQ(cg_pump_stop(p, 1000), 0);
	cg_pump_free(p);
	free(p);
}

/* ---- the space handshake's race ---- */

enum { RACE_BOTH, RACE_EVENTFD_ONLY, RACE_PRESLEEP_ONLY, RACE_NEITHER, RACE_SPINNING };

static struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int at_sleep, released, armed;
} race = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 };

/* In the pump thread, right before it arms its bell with its ring full:
 * the hub gets to free room now, and the pump goes on only after. */
static void race_hook(struct cg_pump *p, int where)
{
	if (where != CG_PUMP_HOOK_SLEEP || !p->paused)
		return;
	pthread_mutex_lock(&race.mu);
	if (race.armed) {
		uint64_t v;

		/* No wake-up left over from earlier commands: only what the hub
		 * does from here on may wake the pump. */
		if (read(p->bell.efd, &v, sizeof(v)) < 0) {
			/* EAGAIN: nothing left over */
		}
		race.armed = 0;
		race.at_sleep = 1;
		pthread_cond_broadcast(&race.cv);
		while (!race.released)
			pthread_cond_wait(&race.cv, &race.mu);
	}
	pthread_mutex_unlock(&race.mu);
}

static void race_run(int variant)
{
	struct cg_pump *p = pump_alloc(8);
	struct cg_pump_cmd c = { .op = CG_PUMP_OPEN, .link = 0, .gen = 1 };
	uint32_t next = 0, n;
	int a, b;

	CHECK(p != NULL);
	if (!p)
		return;
	CHECK_EQ(udp_pair(&a, &b), 0);
	race.at_sleep = race.released = 0;
	race.armed = variant != RACE_SPINNING;
	p->hook = race_hook;
	if (variant == RACE_EVENTFD_ONLY || variant == RACE_NEITHER)
		p->test_flags = CG_PUMP_TEST_NO_PRESLEEP;
	if (variant == RACE_SPINNING)
		atomic_store_explicit(&p->busy_poll_us, 2000000, memory_order_relaxed);
	send_n(b, 0, 40); /* five times its ring */
	CHECK_EQ(cg_pump_start(p, "cg-race", &hub), 0);
	c.fd = a;
	CHECK_EQ(cg_pump_post(p, &c), 0);

	/* The ring fills: 8 slots, and the pump stops reading. */
	for (int i = 0; i < 1000 && cg_pump_rx_avail(p) < 8; i++)
		usleep(1000);
	CHECK_EQ(cg_pump_rx_avail(p), 8);
	if (variant != RACE_SPINNING) {
		pthread_mutex_lock(&race.mu);
		while (!race.at_sleep)
			pthread_cond_wait(&race.cv, &race.mu);
		pthread_mutex_unlock(&race.mu);
	} else {
		for (int i = 0; i < 1000 && !atomic_load(&p->rx_blocked); i++)
			usleep(1000);
		CHECK_EQ(atomic_load(&p->rx_blocked), 1);
	}
	/* The hub takes the 8 and makes room, before the pump sets sleeping. */
	n = cg_pump_rx_avail(p);
	CHECK_EQ(n, 8);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v;

		memcpy(&v, cg_pump_rx_slot(p, i)->buf, sizeof(v));
		CHECK_EQ(v, next);
		next++;
	}
	cg_ring_release(&p->rxq, n);
	if (variant == RACE_PRESLEEP_ONLY || variant == RACE_NEITHER) {
		/* Without the unconditional write: through the bell's sleeping
		 * flag, which the pump has not set yet, so no eventfd. */
		CHECK_EQ(atomic_exchange(&p->rx_blocked, 0), 1);
		CHECK_EQ(cg_bell_ring(&p->bell), 0);
	} else {
		CHECK_EQ(cg_ring_unblock(&p->rx_blocked, p->bell.efd), 1);
	}
	pthread_mutex_lock(&race.mu);
	race.released = 1;
	pthread_cond_broadcast(&race.cv);
	pthread_mutex_unlock(&race.mu);

	if (variant == RACE_NEITHER) {
		/* The bug: the pump sleeps with its sockets out of the poll. */
		CHECK_EQ(hub_wait(p, 300), 0);
		cg_efd_write(p->bell.efd); /* rescued by hand */
	}
	/* It reads again: the other 32, in order. */
	CHECK_EQ(hub_take(p, 0, 1, &next, 32), 32);
	CHECK_EQ(next, 40);
	CHECK(atomic_load(&p->rx_paused) >= 1);
	CHECK_EQ(cg_pump_stop(p, 1000), 0);
	cg_pump_free(p);
	free(p);
	close(b);
}

void test_threads(void)
{
	stress(1);
	stress(0);
	CHECK_EQ(cg_bell_init(&hub), 0);
	pump_basics();
	pump_stalled();
	pend_keep();
	pump_pending();
	pump_open_failed();
	race_run(RACE_BOTH);
	race_run(RACE_EVENTFD_ONLY);
	race_run(RACE_PRESLEEP_ONLY);
	race_run(RACE_NEITHER);
	race_run(RACE_SPINNING);
	cg_bell_free(&hub);
}
