/* macbench: the CPU a server with several clients (routers) adds to each
 * packet, in ns per operation, on the machine it runs on: no root, no network
 * namespaces, so it runs as is on the Pi, a VPS or a laptop.
 *
 *   macbench [-n SECONDS] [-r RUNS] [-s SEED]
 *
 * (bench/lab.sh build puts it in bench/bin.) Each operation is a timed loop
 * of about SECONDS (default 0.2) repeated RUNS times (default 5); the table
 * shows the median ns per operation and the spread, (max - min) / median.
 * SEED (default 1) drives the pseudo-random keys, hints and session ids, so
 * two runs build the same clients and the same hint collisions.
 *
 * What it measures follows listen_read() in engine/src/server.c, with the
 * same functions and the same headers (proto.h, clients.h, idmap.h,
 * cookie.h):
 *
 *   1. the header: cg_hdr_parse, cg_hdr_verify and cg_hdr_write (SipHash-2-4
 *      over the header and the payload) for DATA with 1400 payload bytes and
 *      for a probe with 40;
 *   2. the lookup of an existing session: the clients of the packet's hint
 *      (cg_hintidx), the idmap get under cg_sesskey, and the check that the
 *      session found is that client's and has that id; for 1 client with 1
 *      session, 32 with 2 and 64 with 4 (random keys and hints, so hints
 *      collide as in a real fleet; the table is sized as the server sizes
 *      it, for max(64, clients x CG_CLIENT_SESSIONS) sessions);
 *   3. the same lookup when it fails: a hint no client has (rejected before
 *      any MAC), and a hint that matches with a session id nobody holds;
 *   4. new-session admission: a probe with no session yet tries the MAC of
 *      each client in its hint chain; chains of 1, 2 and 4 clients with the
 *      right key last (the worst case), so 0, 1 and 3 failures and a success;
 *   5. cg_cookie_make and cg_cookie_ok, over IPv4 and IPv6 (the cookie of the
 *      current epoch, and of the previous one, which costs a second hash);
 *   6. what the one-client server did before: cg_idmap_get plus
 *      cg_hdr_verify, next to the lookup plus cg_hdr_verify of this one.
 *
 * Read the figures knowing that: the loops run back to back on hot caches (a
 * packet in the middle of a recvmmsg batch, not the first after a syscall);
 * lookups take their keys from a precomputed array of 2048 picks, so the loop
 * measures the lookup and not a random number generator, and "harness" shows
 * what that loop costs by itself; the MAC is the same work whatever the key,
 * so the lookup + verify rows verify one packet buffer under the key of the
 * client found; the program is built as lab.sh builds it (-O2, no LTO, no
 * hardening flags), and only the loop is timed, never a syscall or a
 * socket. Nothing here includes receiving, the replay window or the send.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <getopt.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "clients.h"
#include "cookie.h"
#include "idmap.h"
#include "proto.h"

#define PICKS 2048 /* a power of two */
#define NAME_W 60

#if defined(__clang__)
#define CC_NAME "clang " __clang_version__
#else
#define CC_NAME "gcc " __VERSION__
#endif

#ifdef __OPTIMIZE__
#define OPT_NAME "optimized"
#else
#define OPT_NAME "UNOPTIMIZED (build with -O2)"
#endif

/* Every loop leaves its result here, so that none of it can be dropped. */
static volatile uint64_t sink;

typedef uint64_t (*op_fn)(const void *ctx, uint64_t iters);

static uint64_t rng_state = 1;

static uint64_t rnd(void) /* splitmix64 */
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static void rnd_bytes(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)rnd();
}

static void die(const char *what)
{
	fprintf(stderr, "macbench: %s\n", what);
	exit(1);
}

/* ---- the clients of a server ---- */

struct sess {
	uint32_t id;
	int client;
};

struct pick {
	uint32_t id;
	uint8_t hint;
};

struct world {
	int nclients, nper, nsess, longest;
	uint8_t key[CG_MAX_CLIENTS][CG_SIPHASH_KEY_LEN];
	uint8_t hint[CG_MAX_CLIENTS];
	uint8_t used[CG_MAX_CLIENTS];
	uint32_t mix[CG_MAX_CLIENTS];
	struct cg_hintidx hx;
	struct cg_idmap map;
	struct sess sess[CG_MAX_CLIENTS * CG_CLIENT_SESSIONS];
	struct pick hit[PICKS];    /* sessions that exist */
	struct pick nohint[PICKS]; /* a hint no client has */
	struct pick nosess[PICKS]; /* a hint some client has, a session nobody holds */
};

/* As in listen_read(): the clients of the hint, the session under each one's
 * mix, and its owner and id checked. Returns the session's slot or -1, and
 * the client that owns it in *client. */
static inline int32_t lookup(const struct world *w, uint8_t hint, uint32_t id, int *client)
{
	for (int c = w->hx.first[hint]; c >= 0; c = w->hx.next[c]) {
		int32_t i = cg_idmap_get(&w->map, cg_sesskey(w->mix[c], id));

		if (i >= 0 && w->sess[i].client == c && w->sess[i].id == id) {
			*client = c;
			return i;
		}
	}
	return -1;
}

static void world_init(struct world *w, int nclients, int nper)
{
	uint32_t cap = (uint32_t)nclients * CG_CLIENT_SESSIONS;
	int dummy;

	memset(w, 0, sizeof(*w));
	w->nclients = nclients;
	w->nper = nper;
	for (int c = 0; c < nclients; c++) {
		rnd_bytes(w->key[c], CG_SIPHASH_KEY_LEN);
		w->hint[c] = (uint8_t)rnd(); /* the first byte of a hash: uniform */
		w->used[c] = 1;
		w->mix[c] = (uint32_t)rnd();
	}
	cg_hintidx_build(&w->hx, w->hint, w->used, nclients);
	if (cg_idmap_init(&w->map, cap < 64 ? 64 : cap) < 0)
		die("out of memory");
	for (int c = 0; c < nclients; c++) {
		for (int k = 0; k < nper; k++) {
			uint32_t id;

			do
				id = (uint32_t)rnd();
			while (cg_idmap_get(&w->map, cg_sesskey(w->mix[c], id)) >= 0);
			w->sess[w->nsess] = (struct sess){ .id = id, .client = c };
			cg_idmap_put(&w->map, cg_sesskey(w->mix[c], id), w->nsess);
			w->nsess++;
		}
	}
	for (int h = 0; h < 256; h++) {
		int n = 0;

		for (int c = w->hx.first[h]; c >= 0; c = w->hx.next[c])
			n++;
		if (n > w->longest)
			w->longest = n;
	}
	for (int j = 0; j < PICKS; j++) {
		const struct sess *s = &w->sess[rnd() % (uint64_t)w->nsess];
		uint8_t h;
		uint32_t id;
		int c;

		w->hit[j] = (struct pick){ .id = s->id, .hint = w->hint[s->client] };
		do
			h = (uint8_t)rnd();
		while (w->hx.first[h] >= 0);
		w->nohint[j] = (struct pick){ .id = (uint32_t)rnd(), .hint = h };
		do {
			c = (int)(rnd() % (uint64_t)nclients);
			id = (uint32_t)rnd();
		} while (lookup(w, w->hint[c], id, &dummy) >= 0);
		w->nosess[j] = (struct pick){ .id = id, .hint = w->hint[c] };
	}
}

/* ---- the operations: each loop returns what it accumulated ---- */

/* A header and payload in buf; verify and write vary byte 3 (the link id),
 * which the MAC leaves out, so that the input changes at each turn and the
 * answer stays the same. */
struct vctx {
	uint8_t *buf;
	size_t len;
	const uint8_t *key;
};

static uint64_t op_harness(const void *p, uint64_t iters)
{
	const struct pick *picks = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		const struct pick *k = &picks[i & (PICKS - 1)];

		acc += k->id + k->hint;
	}
	return acc;
}

static uint64_t op_parse(const void *p, uint64_t iters)
{
	const struct vctx *x = p;
	struct cg_hdr h;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		x->buf[CG_LINK_OFF] = (uint8_t)i;
		acc += (uint64_t)(cg_hdr_parse(&h, x->buf, x->len) + 1) + h.session + h.link;
	}
	return acc;
}

static uint64_t op_verify(const void *p, uint64_t iters)
{
	const struct vctx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		x->buf[CG_LINK_OFF] = (uint8_t)i;
		acc += (uint64_t)cg_hdr_verify(x->buf, x->len, x->key);
	}
	return acc;
}

static uint64_t op_write(const void *p, uint64_t iters)
{
	const struct vctx *x = p;
	struct cg_hdr h = { .type = CG_T_DATA, .hint = 0x5a, .session = 0x12345678u, .ts = 1000 };
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		h.seq = (uint32_t)i;
		cg_hdr_write(x->buf, &h, x->key, x->buf + CG_HDR_LEN, x->len - CG_HDR_LEN);
		acc += x->buf[CG_MAC_OFF];
	}
	return acc;
}

struct lctx {
	const struct world *w;
	const struct pick *picks;
	uint8_t *buf; /* lookup + verify only */
	size_t len;
};

static uint64_t op_lookup(const void *p, uint64_t iters)
{
	const struct lctx *x = p;
	uint64_t acc = 0;
	int c = 0;

	for (uint64_t i = 0; i < iters; i++) {
		const struct pick *k = &x->picks[i & (PICKS - 1)];

		acc += (uint32_t)lookup(x->w, k->hint, k->id, &c);
	}
	return acc + (uint64_t)c;
}

/* The lookup, then the MAC under the key of the client found. */
static uint64_t op_lookup_verify(const void *p, uint64_t iters)
{
	const struct lctx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		const struct pick *k = &x->picks[i & (PICKS - 1)];
		int c = 0;

		if (lookup(x->w, k->hint, k->id, &c) < 0)
			continue;
		x->buf[CG_LINK_OFF] = (uint8_t)i;
		acc += (uint64_t)cg_hdr_verify(x->buf, x->len, x->w->key[c]);
	}
	return acc;
}

/* The one-client server before this change: a plain idmap get on the id,
 * then the MAC. */
struct octx {
	struct cg_idmap map;
	uint32_t id;
	uint8_t *buf;
	size_t len;
	const uint8_t *key;
};

static uint64_t op_old_verify(const void *p, uint64_t iters)
{
	const struct octx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		if (cg_idmap_get(&x->map, x->id) < 0)
			continue;
		x->buf[CG_LINK_OFF] = (uint8_t)i;
		acc += (uint64_t)cg_hdr_verify(x->buf, x->len, x->key);
	}
	return acc;
}

/* Admission: the chain of hint 0x42 holds n clients and the packet is under
 * the key of the last one. */
struct actx {
	struct cg_hintidx hx;
	uint8_t key[4][CG_SIPHASH_KEY_LEN];
	uint8_t *buf;
	size_t len;
};

static uint64_t op_admit(const void *p, uint64_t iters)
{
	const struct actx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		x->buf[CG_LINK_OFF] = (uint8_t)i;
		for (int c = x->hx.first[x->buf[CG_HINT_OFF]]; c >= 0; c = x->hx.next[c]) {
			if (cg_hdr_verify(x->buf, x->len, x->key[c])) {
				acc++;
				break;
			}
		}
	}
	return acc;
}

#define COOKS 1024 /* a power of two */

struct kctx {
	struct cg_cookie_keys k;
	struct sockaddr_storage from;
	uint32_t client;
	uint8_t link;
	uint32_t sess[COOKS];
	uint32_t cur[COOKS];  /* the cookie of each session under the current epoch's key */
	uint32_t prev[COOKS]; /* ... and under the previous epoch's */
	const uint32_t *cook; /* the one cg_cookie_ok is given: cur or prev */
};

static uint64_t op_cookie_make(const void *p, uint64_t iters)
{
	const struct kctx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++)
		acc += cg_cookie_make(&x->k, x->client, x->sess[i & (COOKS - 1)], x->link, &x->from);
	return acc;
}

static uint64_t op_cookie_ok(const void *p, uint64_t iters)
{
	const struct kctx *x = p;
	uint64_t acc = 0;

	for (uint64_t i = 0; i < iters; i++) {
		size_t j = i & (COOKS - 1);

		acc += (uint64_t)cg_cookie_ok(&x->k, x->cook[j], x->client, x->sess[j], x->link, &x->from);
	}
	return acc;
}

/* ---- the harness ---- */

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double secs = 0.2;
static int runs = 5;

static int cmp_dbl(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* The loop length is chosen so that a measurement takes about `secs`; the
 * calibration runs double as a warm-up. */
static void bench(const char *name, op_fn fn, const void *ctx)
{
	double ns[100], dt, per_op;
	uint64_t iters = 1024;

	for (;;) {
		double t0 = now_s();

		sink = fn(ctx, iters);
		dt = now_s() - t0;
		if (dt >= 0.01 || iters >= (1ull << 40))
			break;
		iters *= 4;
	}
	per_op = dt > 0 ? dt * 1e9 / (double)iters : 1.0;
	iters = (uint64_t)(secs * 1e9 / per_op);
	if (iters < 16)
		iters = 16;
	for (int r = 0; r < runs; r++) {
		double t0 = now_s();

		sink = fn(ctx, iters);
		ns[r] = (now_s() - t0) * 1e9 / (double)iters;
	}
	qsort(ns, (size_t)runs, sizeof(ns[0]), cmp_dbl);
	printf("%-*s %9.1f %9.1f %6.1f%%\n", NAME_W, name, ns[runs / 2], ns[0],
	       100.0 * (ns[runs - 1] - ns[0]) / ns[runs / 2]);
	fflush(stdout);
}

static const char *pl(int n)
{
	return n == 1 ? "" : "s";
}

static void section(const char *title)
{
	printf("\n%s\n", title);
}

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "macbench: self-check failed: %s\n", what);
		exit(1);
	}
}

static void cpu_model(char *out, size_t n)
{
	static const char *const keys[] = { "model name", "Model", "Hardware", "cpu model", "system type" };
	char line[256];
	FILE *f = fopen("/proc/cpuinfo", "r");

	snprintf(out, n, "unknown CPU");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			size_t kl = strlen(keys[i]);
			char *v;

			if (strncmp(line, keys[i], kl) || !(v = strchr(line, ':')))
				continue;
			for (v++; *v == ' ' || *v == '\t'; v++)
				;
			v[strcspn(v, "\n")] = 0;
			snprintf(out, n, "%s", v);
			fclose(f);
			return;
		}
	}
	fclose(f);
}

static void fill_packet(uint8_t *buf, size_t plen, uint8_t type, uint8_t hint, const uint8_t *key)
{
	struct cg_hdr h = { .type = type, .hint = hint, .link = 1, .session = (uint32_t)rnd(), .seq = 7, .ts = 99 };

	rnd_bytes(buf + CG_HDR_LEN, plen);
	cg_hdr_write(buf, &h, key, buf + CG_HDR_LEN, plen);
}

#define DATA_LEN 1400
#define PROBE_LEN CG_PROBE_INFO_LEN

int main(int argc, char **argv)
{
	static const int shape[3][2] = { { 1, 1 }, { 32, 2 }, { 64, 4 } };
	static struct world W[3];
	static uint8_t data[CG_HDR_LEN + DATA_LEN], probe[CG_HDR_LEN + PROBE_LEN];
	static struct kctx kc[2];
	static struct actx ac[3];
	static struct octx old;
	uint8_t key[CG_SIPHASH_KEY_LEN];
	char name[128], cpu[160];
	unsigned long long seed = 1;
	int opt;

	while ((opt = getopt(argc, argv, "n:r:s:h")) != -1) {
		switch (opt) {
		case 'n':
			secs = atof(optarg);
			break;
		case 'r':
			runs = atoi(optarg);
			break;
		case 's':
			seed = strtoull(optarg, NULL, 0);
			rng_state = seed;
			break;
		default:
			fprintf(stderr, "usage: macbench [-n SECONDS] [-r RUNS] [-s SEED]\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	if (secs <= 0 || runs < 1 || runs > 100) {
		fprintf(stderr, "macbench: -n must be positive and -r between 1 and 100\n");
		return 2;
	}

	rnd_bytes(key, sizeof(key));
	fill_packet(data, DATA_LEN, CG_T_DATA, 0x5a, key);
	fill_packet(probe, PROBE_LEN, CG_T_PROBE, 0x5a, key);
	for (int i = 0; i < 3; i++)
		world_init(&W[i], shape[i][0], shape[i][1]);

	struct vctx vdata = { data, sizeof(data), key }, vprobe = { probe, sizeof(probe), key };

	/* Admission: n clients on hint 0x42; the packet is under the last key. */
	for (int i = 0, n = 1; i < 3; i++, n = n == 1 ? 2 : 4) {
		uint8_t hint[4] = { 0x42, 0x42, 0x42, 0x42 }, used[4] = { 1, 1, 1, 1 };

		for (int c = 0; c < 4; c++)
			rnd_bytes(ac[i].key[c], CG_SIPHASH_KEY_LEN);
		cg_hintidx_build(&ac[i].hx, hint, used, n);
		ac[i].len = sizeof(probe);
		ac[i].buf = malloc(sizeof(probe));
		if (!ac[i].buf)
			die("out of memory");
		fill_packet(ac[i].buf, PROBE_LEN, CG_T_PROBE, 0x42, ac[i].key[n - 1]);
	}

	/* Cookies over IPv4 and IPv6; keys[1] is the previous epoch's. */
	for (int i = 0; i < 2; i++) {
		struct kctx *k = &kc[i];

		rnd_bytes(k->k.key[0], CG_SIPHASH_KEY_LEN);
		rnd_bytes(k->k.key[1], CG_SIPHASH_KEY_LEN);
		k->k.have = 2;
		k->k.epoch = 1;
		k->client = (uint32_t)rnd();
		k->link = 2;
		if (i == 0) {
			struct sockaddr_in *a = (struct sockaddr_in *)&k->from;

			a->sin_family = AF_INET;
			a->sin_port = htons(51820);
			a->sin_addr.s_addr = htonl(0xc0000201u);
		} else {
			struct sockaddr_in6 *a = (struct sockaddr_in6 *)&k->from;

			a->sin6_family = AF_INET6;
			a->sin6_port = htons(51820);
			rnd_bytes((uint8_t *)&a->sin6_addr, 16);
		}
		for (int j = 0; j < COOKS; j++)
			k->sess[j] = (uint32_t)rnd();
	}

	/* The one-client server of before: a 64-entry table, one session. */
	if (cg_idmap_init(&old.map, 64) < 0)
		die("out of memory");
	old.id = (uint32_t)rnd();
	cg_idmap_put(&old.map, old.id, 0);
	old.key = key;

	/* The loops do what the table says they do. */
	{
		static uint8_t own[CG_HDR_LEN + DATA_LEN];
		struct lctx lc = { .w = &W[0], .picks = W[0].hit, .buf = own, .len = sizeof(own) };
		int c;

		for (int i = 0; i < 3; i++) {
			const struct world *w = &W[i];

			for (int j = 0; j < PICKS; j++) {
				check(lookup(w, w->hit[j].hint, w->hit[j].id, &c) >= 0, "existing session found");
				check(lookup(w, w->nohint[j].hint, w->nohint[j].id, &c) < 0 && w->hx.first[w->nohint[j].hint] < 0,
				      "no client has the hint");
				check(lookup(w, w->nosess[j].hint, w->nosess[j].id, &c) < 0 && w->hx.first[w->nosess[j].hint] >= 0,
				      "unknown session under a known hint");
			}
		}
		check(cg_hdr_verify(data, sizeof(data), key) && cg_hdr_verify(probe, sizeof(probe), key), "own MAC verifies");
		check(op_verify(&vdata, 100) == 100 && op_verify(&vprobe, 100) == 100, "verify keeps succeeding");
		fill_packet(own, DATA_LEN, CG_T_DATA, W[0].hint[0], W[0].key[0]);
		check(op_lookup_verify(&lc, 100) == 100, "lookup + verify succeeds under the client's key");
		old.buf = data;
		old.len = sizeof(data);
		check(op_old_verify(&old, 100) == 100, "the old path finds its session");
		for (int i = 0; i < 3; i++)
			check(op_admit(&ac[i], 100) == 100, "admission finds the last key");
		for (int i = 0; i < 2; i++) {
			struct kctx *k = &kc[i];

			for (int j = 0; j < COOKS; j++) {
				k->cur[j] = cg_cookie_make(&k->k, k->client, k->sess[j], k->link, &k->from);
				k->prev[j] = cg_cookie(k->k.key[1], k->client, k->sess[j], k->link, &k->from);
			}
			k->cook = k->cur;
			check(op_cookie_ok(k, COOKS) == COOKS, "current-epoch cookies accepted");
			k->cook = k->prev;
			check(op_cookie_ok(k, COOKS) == COOKS, "previous-epoch cookies accepted");
		}
	}

	cpu_model(cpu, sizeof(cpu));
	printf("macbench: %s\n", cpu);
	printf("          %s, %s, %.2f s x %d runs, seed %llu\n", CC_NAME, OPT_NAME, secs, runs,
	       (unsigned long long)seed);
	printf("worlds:   ");
	for (int i = 0; i < 3; i++)
		printf("%s%d client%s x %d session%s (longest hint chain %d)", i ? "; " : "", W[i].nclients,
		       pl(W[i].nclients), W[i].nper, pl(W[i].nper), W[i].longest);
	printf("\n\n%-*s %9s %9s %7s\n", NAME_W, "operation", "ns/op", "min", "spread");

	section("1. header (proto.h)");
	bench("cg_hdr_parse, DATA", op_parse, &vdata);
	bench("cg_hdr_verify, DATA 1400 B", op_verify, &vdata);
	bench("cg_hdr_verify, probe 40 B", op_verify, &vprobe);
	bench("cg_hdr_write, DATA 1400 B", op_write, &vdata);
	bench("cg_hdr_write, probe 40 B", op_write, &vprobe);

	section("2. lookup of an existing session (hint chain + idmap + owner)");
	bench("harness: pick load + add (floor of the loops below)", op_harness, W[0].hit);
	for (int i = 0; i < 3; i++) {
		struct lctx lc = { .w = &W[i], .picks = W[i].hit };

		snprintf(name, sizeof(name), "%d client%s x %d session%s", W[i].nclients, pl(W[i].nclients), W[i].nper,
			 pl(W[i].nper));
		bench(name, op_lookup, &lc);
	}

	section("3. lookup that fails");
	for (int i = 0; i < 3; i++) {
		struct lctx lc = { .w = &W[i], .picks = W[i].nohint };

		snprintf(name, sizeof(name), "%d client%s: hint no client has", W[i].nclients, pl(W[i].nclients));
		bench(name, op_lookup, &lc);
		lc.picks = W[i].nosess;
		snprintf(name, sizeof(name), "%d client%s: hint matches, session unknown", W[i].nclients,
			 pl(W[i].nclients));
		bench(name, op_lookup, &lc);
	}

	section("4. new-session admission: MAC tries on a probe, right key last");
	for (int i = 0, n = 1; i < 3; i++, n = n == 1 ? 2 : 4) {
		snprintf(name, sizeof(name), "chain of %d: %d failing + 1 succeeding verify", n, n - 1);
		bench(name, op_admit, &ac[i]);
	}

	section("5. cookies (cookie.h)");
	for (int i = 0; i < 2; i++) {
		const char *fam = i ? "IPv6" : "IPv4";

		snprintf(name, sizeof(name), "cg_cookie_make, %s", fam);
		bench(name, op_cookie_make, &kc[i]);
		kc[i].cook = kc[i].cur;
		snprintf(name, sizeof(name), "cg_cookie_ok, %s, current epoch (1 hash)", fam);
		bench(name, op_cookie_ok, &kc[i]);
		kc[i].cook = kc[i].prev;
		snprintf(name, sizeof(name), "cg_cookie_ok, %s, previous epoch (2 hashes)", fam);
		bench(name, op_cookie_ok, &kc[i]);
	}

	section("6. one client before this change, and lookup + verify now");
	for (int t = 0; t < 2; t++) {
		struct vctx *v = t ? &vprobe : &vdata;

		old.buf = v->buf;
		old.len = v->len;
		snprintf(name, sizeof(name), "before: idmap_get + verify, %s", t ? "probe 40 B" : "DATA 1400 B");
		bench(name, op_old_verify, &old);
		for (int i = 0; i < 3; i++) {
			struct lctx lc = { .w = &W[i], .picks = W[i].hit, .buf = v->buf, .len = v->len };

			snprintf(name, sizeof(name), "now: lookup + verify, %s, %d client%s x %d session%s",
				 t ? "probe 40 B" : "DATA 1400 B", W[i].nclients, pl(W[i].nclients), W[i].nper, pl(W[i].nper));
			bench(name, op_lookup_verify, &lc);
		}
	}

	printf("\nns/op: median of %d runs of about %.2f s; min: the fastest run; spread: (max - min) / median.\n", runs,
	       secs);
	return 0;
}
