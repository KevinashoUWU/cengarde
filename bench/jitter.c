// jitter: how late a 1 ms sleep wakes, on every CPU (like cyclictest, at
// normal priority). A VM that pauses as a whole shows up as late wakeups at
// the same instant on several CPUs; lab scenarios run it alongside traffic
// to tell those pauses from the engine's own stalls.
//
// usage: jitter [-d SECS] [-e MS]
//   -d  how long to run (default 30)
//   -e  print an event line for each wakeup later than this (default 5 ms)
//
// Output: "event cpu=N t=S mono=S late_ms=L" per late wakeup (t: seconds
// since the start; mono: CLOCK_MONOTONIC, to match other tools' clocks),
// then one summary line per CPU.
//
// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MAX_CPUS 256

struct res {
	uint64_t n, max_ns, over1, over5, over20;
};

static long secs = 30;
static uint64_t event_ns = 5000000;
static uint64_t start_ns;
static struct res res[MAX_CPUS];
static pthread_mutex_t out = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void *run(void *arg)
{
	long cpu = (long)arg;
	struct res *r = &res[cpu];
	cpu_set_t set;
	uint64_t end;

	CPU_ZERO(&set);
	CPU_SET((int)cpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	end = now_ns() + (uint64_t)secs * 1000000000u;
	while (now_ns() < end) {
		struct timespec d = { 0, 1000000 };
		uint64_t t0 = now_ns(), late;

		nanosleep(&d, NULL);
		late = now_ns() - t0 - 1000000;
		r->n++;
		if (late > r->max_ns)
			r->max_ns = late;
		r->over1 += late > 1000000;
		r->over5 += late > 5000000;
		r->over20 += late > 20000000;
		if (late > event_ns) {
			pthread_mutex_lock(&out);
			printf("event cpu=%ld t=%.3f mono=%.6f late_ms=%.1f\n", cpu, (double)(t0 - start_ns) / 1e9,
			       (double)t0 / 1e9, (double)late / 1e6);
			fflush(stdout);
			pthread_mutex_unlock(&out);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t[MAX_CPUS];
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	int opt;

	while ((opt = getopt(argc, argv, "d:e:")) != -1) {
		switch (opt) {
		case 'd':
			secs = atol(optarg);
			break;
		case 'e':
			event_ns = (uint64_t)(atof(optarg) * 1e6);
			break;
		default:
			fprintf(stderr, "usage: jitter [-d SECS] [-e MS]\n");
			return 2;
		}
	}
	if (n < 1)
		n = 1;
	if (n > MAX_CPUS)
		n = MAX_CPUS;
	start_ns = now_ns();
	for (long i = 0; i < n; i++)
		pthread_create(&t[i], NULL, run, (void *)i);
	for (long i = 0; i < n; i++)
		pthread_join(t[i], NULL);
	for (long i = 0; i < n; i++)
		printf("cpu=%ld wakeups=%llu max_late_us=%llu late>1ms=%llu >5ms=%llu >20ms=%llu\n", i,
		       (unsigned long long)res[i].n, (unsigned long long)(res[i].max_ns / 1000),
		       (unsigned long long)res[i].over1, (unsigned long long)res[i].over5,
		       (unsigned long long)res[i].over20);
	return 0;
}
