// mgen: udpgen for several peers (the fake WireGuard of bench/mt.py multi).
// Same packet format as bench/udpgen.c (WireGuard-shaped, seq + timestamp).
// Receiving: counts per source address (flow), each with its own sequence
// space, so several sessions can share one fake WireGuard endpoint.
// Sending (-l): learns peers from received packets (a peer is a distinct
// source address:port, in the order first heard) and sends to each at its
// own rate, with a sequence per peer.
//
// usage: mgen -b ip:port [-l -n NPEERS -R r0,r1,...] [-s size] [-d secs] [-g grace]
//   -R rates per peer in learning order; the last one repeats for later peers
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x45474e44u
#define HIST_US 200000
#define MAXF 64

struct hdr {
	uint32_t wg_type;
	uint32_t magic;
	uint64_t seq;
	uint64_t ts_ns;
};

struct flow {
	uint32_t addr;
	uint16_t port;
	uint8_t *seen;
	uint64_t total, uniq, dup, late, *hist, over;
};

static int sock;
static struct flow flows[MAXF];
static atomic_int nflows;
static int learn;
static atomic_int stop_rx;
static long size = 1400, duration = 10, grace = 2, npeers = 1;
static long rates[MAXF];
static int nrates;
static uint64_t max_seq_bits = 1u << 24;
static uint64_t ghist[HIST_US], gover;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void parse_addr(const char *s, struct sockaddr_in *a)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", s);
	char *c = strrchr(buf, ':');
	if (!c)
		exit(1);
	*c = 0;
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port = htons(atoi(c + 1));
	if (inet_pton(AF_INET, buf, &a->sin_addr) != 1)
		exit(1);
}

static struct flow *flow_of(const struct sockaddr_in *f)
{
	int n = atomic_load(&nflows);
	for (int i = 0; i < n; i++)
		if (flows[i].addr == f->sin_addr.s_addr && flows[i].port == f->sin_port)
			return &flows[i];
	if (n >= MAXF)
		return NULL;
	struct flow *x = &flows[n];
	x->addr = f->sin_addr.s_addr;
	x->port = f->sin_port;
	x->seen = calloc(max_seq_bits / 8 + 1, 1);
	x->hist = calloc(HIST_US, sizeof(uint64_t));
	atomic_store(&nflows, n + 1);
	return x;
}

static void *rx_thread(void *arg)
{
	(void)arg;
	enum { B = 64 };
	static char bufs[B][2048];
	struct mmsghdr msgs[B];
	struct iovec iov[B];
	struct sockaddr_in from[B];
	while (!atomic_load(&stop_rx)) {
		for (int i = 0; i < B; i++) {
			iov[i].iov_base = bufs[i];
			iov[i].iov_len = sizeof(bufs[i]);
			memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
			msgs[i].msg_hdr.msg_iov = &iov[i];
			msgs[i].msg_hdr.msg_iovlen = 1;
			msgs[i].msg_hdr.msg_name = &from[i];
			msgs[i].msg_hdr.msg_namelen = sizeof(from[i]);
		}
		int n = recvmmsg(sock, msgs, B, MSG_WAITFORONE, NULL);
		if (n <= 0)
			continue;
		uint64_t t = now_ns();
		for (int i = 0; i < n; i++) {
			if (msgs[i].msg_len < sizeof(struct hdr))
				continue;
			struct hdr *h = (struct hdr *)bufs[i];
			if (h->magic != MAGIC)
				continue;
			struct flow *f = flow_of(&from[i]);
			if (!f)
				continue;
			f->total++;
			uint64_t s = h->seq;
			if (s >= max_seq_bits) {
				f->late++;
				continue;
			}
			if (f->seen[s >> 3] & (1u << (s & 7))) {
				f->dup++;
				continue;
			}
			f->seen[s >> 3] |= 1u << (s & 7);
			f->uniq++;
			uint64_t us = (t - h->ts_ns) / 1000;
			if (us < HIST_US) {
				f->hist[us]++;
				ghist[us]++;
			} else {
				f->over++;
				gover++;
			}
		}
	}
	return NULL;
}

static uint64_t pct(const uint64_t *hist, uint64_t n, double p)
{
	uint64_t target = (uint64_t)(p * n), acc = 0;
	for (uint64_t i = 0; i < HIST_US; i++) {
		acc += hist[i];
		if (acc > target)
			return i;
	}
	return HIST_US;
}

int main(int argc, char **argv)
{
	struct sockaddr_in bind_a;
	int have_bind = 0, opt;
	while ((opt = getopt(argc, argv, "b:ln:R:s:d:g:")) != -1) {
		switch (opt) {
		case 'b': parse_addr(optarg, &bind_a); have_bind = 1; break;
		case 'l': learn = 1; break;
		case 'n': npeers = atol(optarg); break;
		case 'R': {
			char *s = strdup(optarg), *t, *save;
			for (t = strtok_r(s, ",", &save); t && nrates < MAXF; t = strtok_r(NULL, ",", &save))
				rates[nrates++] = atol(t);
			break;
		}
		case 's': size = atol(optarg); break;
		case 'd': duration = atol(optarg); break;
		case 'g': grace = atol(optarg); break;
		default: return 1;
		}
	}
	if (!have_bind)
		return 1;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	int big = 8 << 20;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof(big));
	setsockopt(sock, SOL_SOCKET, SO_SNDBUFFORCE, &big, sizeof(big));
	struct timeval rto = { 0, 100 * 1000 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
	if (bind(sock, (struct sockaddr *)&bind_a, sizeof(bind_a)) < 0) {
		perror("bind");
		return 1;
	}
	{
		long tot = 0;
		for (int i = 0; i < nrates; i++)
			tot += rates[i];
		if ((uint64_t)tot * (duration + 5) + 1024 > max_seq_bits)
			max_seq_bits = (uint64_t)tot * (duration + 5) + 1024;
	}
	pthread_t rx;
	pthread_create(&rx, NULL, rx_thread, NULL);

	uint64_t sent[MAXF] = { 0 }, send_err = 0;
	if (learn && nrates) {
		while (atomic_load(&nflows) < npeers)
			usleep(1000);
		struct sockaddr_in peer[MAXF];
		long r[MAXF];
		int np = (int)npeers;
		for (int i = 0; i < np; i++) {
			memset(&peer[i], 0, sizeof(peer[i]));
			peer[i].sin_family = AF_INET;
			peer[i].sin_addr.s_addr = flows[i].addr;
			peer[i].sin_port = flows[i].port;
			r[i] = rates[i < nrates ? i : nrates - 1];
		}
		char *pkt = calloc(1, size);
		struct hdr *h = (struct hdr *)pkt;
		h->wg_type = htole32(4);
		h->magic = MAGIC;
		uint64_t start = now_ns(), end = start + duration * 1000000000ull;
		for (;;) {
			uint64_t t = now_ns();
			if (t >= end)
				break;
			for (int i = 0; i < np; i++) {
				uint64_t due = (t - start) * r[i] / 1000000000ull;
				while (sent[i] < due) {
					h->seq = sent[i];
					h->ts_ns = now_ns();
					if (sendto(sock, pkt, size, 0, (struct sockaddr *)&peer[i], sizeof(peer[i])) < 0)
						send_err++;
					sent[i]++;
				}
			}
			struct timespec ts = { 0, 200 * 1000 };
			nanosleep(&ts, NULL);
		}
		sleep(grace);
		uint64_t tot = 0;
		for (int i = 0; i < np; i++)
			tot += sent[i];
		printf("sent=%lu send_err=%lu", tot, send_err);
		for (int i = 0; i < np; i++)
			printf(" sent%d=%lu", i, sent[i]);
		printf("\n");
	} else {
		sleep(duration + grace);
	}
	atomic_store(&stop_rx, 1);
	pthread_join(rx, NULL);
	uint64_t tu = 0, tt = 0, td = 0;
	int n = atomic_load(&nflows);
	for (int i = 0; i < n; i++) {
		tu += flows[i].uniq;
		tt += flows[i].total;
		td += flows[i].dup;
	}
	printf("flows=%d rx=%lu uniq=%lu dup=%lu p50_us=%lu p99_us=%lu p999_us=%lu over200ms=%lu\n", n, tt, tu, td,
	       tu ? pct(ghist, tu, 0.5) : 0, tu ? pct(ghist, tu, 0.99) : 0, tu ? pct(ghist, tu, 0.999) : 0, gover);
	for (int i = 0; i < n; i++) {
		struct flow *f = &flows[i];
		struct in_addr a = { f->addr };
		printf("flow%d=%s:%u rx=%lu uniq=%lu dup=%lu p50_us=%lu p99_us=%lu p999_us=%lu over200ms=%lu\n", i,
		       inet_ntoa(a), ntohs(f->port), f->total, f->uniq, f->dup, f->uniq ? pct(f->hist, f->uniq, 0.5) : 0,
		       f->uniq ? pct(f->hist, f->uniq, 0.99) : 0, f->uniq ? pct(f->hist, f->uniq, 0.999) : 0, f->over);
	}
	return 0;
}
