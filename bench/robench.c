// robench: the cost of engine/src/reorder.h alone, in ns per packet (story 012,
// Fase 5 step 1). No sockets, no engine: made-up streams of verified first copies,
// fed to cg_ro_in() and drained with cg_ro_next() as the engine's callers do (one
// drain after every packet), timed over the loop.
//
//   in order      one link, 1400 bytes, nothing to reorder: the fast path
//   swaps         redundancy with reorder_dup_wait_ms 0: 1 packet in 100 comes after
//                 the next one (a returning link), so the gap is given up and the
//                 packet goes at once, without a copy
//   spread 50     split over two links 50 ms apart at 20 kpps, 1400 bytes: half of
//                 the packets wait for the other half
//   imix bypass   the same with IMIX sizes (7 x 64, 4 x 576, 1 x 1400): the 64-byte
//                 ones (duplicated in real life, so first copy on the fast link)
//                 bypass; the big ones are split
//   overflow      IMIX at 30 kpps with 150 ms between the links: 4096 sequences
//                 are 136 ms, so the window overflows and gaps are forced out
//
// Each runs twice: the logic alone, and with the copy of the payload into the pool
// buffer that the caller does for a held packet (a memcpy of its length). Every
// packet must come out exactly once; it says FAIL and exits 1 when not.
//
// usage: robench [packets-per-scenario (1000000)]
// Build: gcc -O2 -Wall -Wextra -I engine/src -o bench/bin/robench bench/robench.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reorder.h"

struct ev {
	uint32_t seq, ts, t_arr;
	uint16_t len;
	uint8_t link, flags;
};

struct scenario {
	const char *name;
	int kind;           /* 0 one link, 1 swaps, 2 split, 3 split with IMIX */
	uint32_t pps;       /* packets sent per second */
	uint32_t delay_us[2]; /* of each link */
	uint32_t dup_wait_us;
};

static const struct scenario scenarios[] = {
	{ "in order", 0, 20000, { 20000, 20000 }, 0 },
	{ "swaps 1%, dup_wait 0", 1, 20000, { 20000, 20000 }, 0 },
	{ "spread 50 ms, 50/50", 2, 20000, { 20000, 70000 }, 0 },
	{ "imix bypass, spread 50 ms", 3, 20000, { 20000, 70000 }, 0 },
	{ "overflow (imix 30k, 150 ms)", 3, 30000, { 20000, 170000 }, 0 },
};

static uint32_t rng = 0x2545f491u;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint16_t imix_len(uint32_t i)
{
	uint32_t k = i % 12;

	return k < 7 ? 64 : k < 11 ? 576 : 1400;
}

/* The arrivals of a scenario, in arrival order. The sender's clock and the local
 * one differ by an offset the reorderer must not care about. */
static struct ev *make(const struct scenario *sc, size_t n)
{
	struct ev *a = calloc(n, sizeof(*a)), *b = calloc(n, sizeof(*b)), *out = calloc(n, sizeof(*out));
	size_t na = 0, nb = 0, i, j, k;
	const uint32_t seq0 = 0xffffffffu - 5000; /* wraps within the first 5000 packets */
	const uint32_t ts0 = 0xfffff000u, off = 0x30000000u;
	uint32_t big = 0;

	if (!a || !b || !out)
		exit(2);
	for (i = 0; i < n; i++) {
		struct ev e;
		uint64_t sent_us = (uint64_t)i * 1000000u / sc->pps;
		int link = 0;

		e.seq = seq0 + (uint32_t)i;
		e.ts = ts0 + (uint32_t)sent_us;
		e.len = sc->kind == 3 ? imix_len((uint32_t)i) : 1400;
		e.flags = 0;
		if (sc->kind >= 2 && e.len >= 256) { /* the big ones are split, one link then the other */
			link = (int)(big++ & 1);
			e.flags = CG_F_SPLIT;
		}
		e.link = (uint8_t)link;
		e.t_arr = ts0 + off + (uint32_t)sent_us + sc->delay_us[link];
		if (link == 0)
			a[na++] = e;
		else
			b[nb++] = e;
	}
	for (i = j = k = 0; i < na || j < nb;) { /* merge the two links by arrival time */
		if (j >= nb || (i < na && (int32_t)(a[i].t_arr - b[j].t_arr) <= 0))
			out[k++] = a[i++];
		else
			out[k++] = b[j++];
	}
	if (sc->kind == 1) { /* swap the packets of neighbours, not their times */
		for (i = 0; i + 1 < n; i++) {
			if (rnd() % 100 == 0) {
				struct ev x = out[i], y = out[i + 1];
				uint32_t t0 = x.t_arr, t1 = y.t_arr;

				out[i] = y;
				out[i + 1] = x;
				out[i].t_arr = t0;
				out[i + 1].t_arr = t1;
				i++;
			}
		}
	}
	free(a);
	free(b);
	return out;
}

struct result {
	double ns;
	uint64_t now, released, flushed;
	struct cg_ro_stats st;
};

static uint8_t payload[CG_RO_BUF];

/* One pass over the arrivals: the elapsed seconds, and what came out. */
static void run(const struct ev *ev, size_t n, const struct scenario *sc, int copy, struct result *res)
{
	static struct cg_reorder r;
	struct cg_ro_pool pool;
	struct cg_ro_cfg cf = { .max_wait_us = 400000, .dup_wait_us = sc->dup_wait_us, .margin_us = 10000,
				.bypass_len = 128 };
	const uint16_t rset = 0x3, npool = 4096;
	static uint8_t *mem;
	static uint16_t *stack;
	struct cg_ro_out o;
	uint64_t sink = 0;
	double t0, t1;
	size_t i;

	if (!mem) {
		mem = calloc(npool, CG_RO_BUF);
		stack = calloc(npool, sizeof(*stack));
	}
	cg_ro_init(&r);
	cg_ro_pool_init(&pool, mem, stack, npool);
	memset(res, 0, sizeof(*res));
	t0 = now_s();
	for (i = 0; i < n; i++) {
		struct cg_ro_pkt k = { .seq = ev[i].seq, .ts = ev[i].ts, .t_arr = ev[i].t_arr, .len = ev[i].len,
				       .link = ev[i].link, .flags = ev[i].flags };
		uint32_t now = k.t_arr;
		uint16_t buf;
		enum cg_ro_v v = cg_ro_in(&r, &pool, &cf, &k, rset, &buf);

		if (v == CG_RO_AGAIN) {
			while (cg_ro_next(&r, &cf, rset, now, &o)) {
				sink += o.len;
				res->released++;
				cg_ro_pool_put(&pool, o.buf);
			}
			v = cg_ro_in(&r, &pool, &cf, &k, rset, &buf);
		}
		if (v == CG_RO_HOLD) {
			if (copy)
				memcpy(cg_ro_pool_buf(&pool, buf), payload, k.len);
		} else {
			res->now++;
		}
		while (cg_ro_next(&r, &cf, rset, now, &o)) {
			sink += o.len;
			res->released++;
			cg_ro_pool_put(&pool, o.buf);
		}
	}
	t1 = now_s();
	res->ns = (t1 - t0) * 1e9 / (double)n;
	res->st = r.st;
	while (cg_ro_flush(&r, &o)) {
		res->flushed++;
		cg_ro_pool_put(&pool, o.buf);
	}
	if (sink == 1) /* keep the loop */
		putchar(' ');
}

int main(int argc, char **argv)
{
	size_t n = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 1000000;
	int fail = 0;

	if (n < 10000) {
		fprintf(stderr, "usage: robench [packets (at least 10000)]\n");
		return 2;
	}
	memset(payload, 0xa5, sizeof(payload));
	printf("%-28s %7s %6s %6s  %-20s %-25s %-9s %9s %9s\n", "scenario", "packets", "held%", "byp%",
	       "lost rack/cap/ovf", "late rack/cap/ovf/old", "far/full", "ns logic", "ns +copy");
	for (size_t s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); s++) {
		const struct scenario *sc = &scenarios[s];
		struct ev *ev = make(sc, n);
		struct result best[2], cur;

		for (int copy = 0; copy < 2; copy++) {
			for (int rep = 0; rep < 3; rep++) { /* the least of three: the noise is only ever up */
				run(ev, n, sc, copy, &cur);
				if (rep == 0 || cur.ns < best[copy].ns)
					best[copy] = cur;
				if (cur.now + cur.released + cur.flushed != n) {
					printf("FAIL: %s: %lu packets in, %lu out\n", sc->name, (unsigned long)n,
					       (unsigned long)(cur.now + cur.released + cur.flushed));
					fail = 1;
				}
			}
		}
		const struct cg_ro_stats *st = &best[1].st;

		char lost[48], late[64], other[32];

		snprintf(lost, sizeof(lost), "%lu/%lu/%lu", (unsigned long)st->lost_rack, (unsigned long)st->lost_cap,
			 (unsigned long)st->lost_overflow);
		snprintf(late, sizeof(late), "%lu/%lu/%lu/%lu", (unsigned long)st->late_rack, (unsigned long)st->late_cap,
			 (unsigned long)st->late_overflow, (unsigned long)st->late_old);
		snprintf(other, sizeof(other), "%lu/%lu", (unsigned long)st->far, (unsigned long)st->pool_full);
		printf("%-28s %7zu %5.1f%% %5.1f%%  %-20s %-25s %-9s %9.1f %9.1f\n", sc->name, n,
		       100.0 * (double)st->held_total / (double)n, 100.0 * (double)st->bypassed / (double)n, lost, late,
		       other, best[0].ns, best[1].ns);
		free(ev);
	}
	printf("robench: %s\n", fail ? "FAILED" : "ok (every packet out exactly once)");
	return fail;
}
