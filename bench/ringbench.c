// ringbench: what a hand-off between two threads costs through the engine's
// ring (engine/src/ring.h), on the machine it runs on: no root, no network
// namespaces, so it runs as is on the Pi, a VPS or a laptop.
//
// usage: ringbench [-d SECS] [-q]     (bench/lab.sh build puts it in bench/bin)
//
// - spin: a consumer that never sleeps; throughput in ns per entry for
//   batches of 1, 8 and 64 entries (the cost when both threads are busy).
// - wake: a producer that publishes one stamped entry at a fixed rate and a
//   consumer that sleeps on its doorbell (an eventfd) when the ring is empty,
//   as the router's hub and pumps do at low rates: hand-off latency (p50 and
//   p99, from the entry's stamp to the consumer's clock) and CPU per entry of
//   each thread, from their own CPU clocks (the producer's includes the
//   nanosleep that paces it); with -q also with the consumer polling
//   (busy_poll) instead of sleeping.
//
// The figures are this machine's: docs/historias/011 explains how they feed
// the router's gates.
#define _GNU_SOURCE
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hist.h"
#include "ring.h"
#include "util.h"

struct ent {
	uint64_t t_us;
	uint64_t seq;
};

struct bench {
	struct cg_ring r;
	struct cg_bell bell;
	uint64_t n;       /* entries to pass */
	uint32_t batch;   /* spin: entries per publish */
	uint32_t rate;    /* wake: entries per second; 0: as fast as possible */
	int sleeper;      /* the consumer sleeps on the bell when idle */
	struct cg_hist lat;
	uint64_t prod_ns, cons_ns, wakes, bad;
	_Atomic uint32_t done;
};

static uint64_t thread_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *producer(void *arg)
{
	struct bench *b = arg;
	uint64_t seq = 0, c0 = thread_ns(), start = cg_now_us();

	while (seq < b->n) {
		uint32_t want = b->batch, p;

		if (b->rate) {
			uint64_t due = start + seq * 1000000ull / b->rate, now = cg_now_us();

			if (now < due) {
				struct timespec ts = { 0, (long)(due - now) * 1000 };

				nanosleep(&ts, NULL);
			}
		}
		if (want > b->n - seq)
			want = (uint32_t)(b->n - seq);
		while (cg_ring_room(&b->r, want) < want)
			sched_yield();
		p = cg_ring_prod(&b->r);
		for (uint32_t i = 0; i < want; i++) {
			struct ent *e = cg_ring_at(&b->r, p + i);

			e->seq = seq + i;
			e->t_us = b->rate ? cg_now_us() : 0;
		}
		cg_ring_publish(&b->r, want);
		if (b->sleeper)
			cg_bell_ring(&b->bell);
		seq += want;
	}
	b->prod_ns = thread_ns() - c0;
	return NULL;
}

static void *consumer(void *arg)
{
	struct bench *b = arg;
	uint64_t got = 0, c0 = thread_ns();

	while (got < b->n) {
		uint32_t n = cg_ring_avail(&b->r), c = cg_ring_cons(&b->r);
		uint64_t now;

		if (!n) {
			struct pollfd pfd = { .fd = b->bell.efd, .events = POLLIN };

			if (!b->sleeper)
				continue;
			cg_bell_arm(&b->bell);
			if (cg_ring_has_work_sc(&b->r)) {
				cg_bell_disarm(&b->bell);
				continue;
			}
			poll(&pfd, 1, -1);
			cg_bell_drain(&b->bell);
			b->wakes++;
			continue;
		}
		now = b->rate ? cg_now_us() : 0;
		for (uint32_t i = 0; i < n; i++) {
			const struct ent *e = cg_ring_at(&b->r, c + i);

			if (e->seq != got + i)
				b->bad++;
			if (b->rate)
				cg_hist_add(&b->lat, (uint32_t)(now - e->t_us));
		}
		cg_ring_release(&b->r, n);
		got += n;
	}
	b->cons_ns = thread_ns() - c0;
	return NULL;
}

static int run(struct bench *b)
{
	pthread_t pt, ct;

	memset(&b->lat, 0, sizeof(b->lat));
	b->prod_ns = b->cons_ns = b->wakes = b->bad = 0;
	if (cg_ring_init(&b->r, 256, sizeof(struct ent)) < 0 || cg_bell_init(&b->bell) < 0) {
		perror("ringbench: ring");
		return -1;
	}
	if (pthread_create(&ct, NULL, consumer, b) || pthread_create(&pt, NULL, producer, b)) {
		perror("ringbench: pthread_create");
		return -1;
	}
	pthread_join(pt, NULL);
	pthread_join(ct, NULL);
	cg_bell_free(&b->bell);
	cg_ring_free(&b->r);
	if (b->bad) {
		fprintf(stderr, "ringbench: %llu entries out of order\n", (unsigned long long)b->bad);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const uint32_t batches[] = { 1, 8, 64 }, rates[] = { 2000, 20000, 80000 };
	static struct bench b;
	double secs = 2;
	int opt, poll_too = 0;

	while ((opt = getopt(argc, argv, "d:qh")) != -1) {
		switch (opt) {
		case 'd':
			secs = atof(optarg);
			break;
		case 'q':
			poll_too = 1;
			break;
		default:
			fprintf(stderr, "usage: ringbench [-d SECS] [-q]\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	if (secs <= 0)
		secs = 2;
	for (size_t i = 0; i < sizeof(batches) / sizeof(batches[0]); i++) {
		uint64_t t0;

		b.n = 2000000;
		b.batch = batches[i];
		b.rate = 0;
		b.sleeper = 0;
		t0 = cg_now_us();
		if (run(&b) < 0)
			return 1;
		printf("spin  batch=%-3u ns/entry=%.1f\n", b.batch, 1000.0 * (double)(cg_now_us() - t0) / (double)b.n);
	}
	for (int sleeper = 1; sleeper >= (poll_too ? 0 : 1); sleeper--) {
		for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
			uint32_t p50 = 0, p99 = 0;

			b.rate = rates[i];
			b.n = (uint64_t)(rates[i] * secs);
			b.batch = 1;
			b.sleeper = sleeper;
			if (run(&b) < 0)
				return 1;
			cg_hist_pct(b.lat.b, 50, &p50);
			cg_hist_pct(b.lat.b, 99, &p99);
			printf("%s rate=%-6u p50_us=%-5u p99_us=%-6u wakes/entry=%.2f us/entry producer=%.2f consumer=%.2f\n",
			       sleeper ? "wake " : "poll ", b.rate, p50, p99, (double)b.wakes / (double)b.n,
			       (double)b.prod_ns / 1000.0 / (double)b.n, (double)b.cons_ns / 1000.0 / (double)b.n);
		}
	}
	return 0;
}
