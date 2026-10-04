/* The concurrency primitives under real threads (also run under TSAN, on an
 * AArch64 runner and under qemu):
 * - ring stress: a producer and a consumer pass CG_TEST_RING_N entries (10^7
 *   by default) in random batches, each checked against the sequence it
 *   should carry; once with a consumer that sleeps on its bell (an eventfd)
 *   and once with one that spins without any eventfd, so that the atomics
 *   alone must order the payload (TSAN treats an eventfd as a
 *   synchronisation and would hide a missing barrier);
 * - lost wake-up: the sleeping consumer waits with a 1 s timeout, and a
 *   timeout that finds entries waiting fails the test.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

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

void test_threads(void)
{
	stress(1);
	stress(0);
}
