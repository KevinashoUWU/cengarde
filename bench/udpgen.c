// udpgen: fake WireGuard endpoint for benchmarking engarde.
// Sends sequence-numbered, timestamped UDP packets at a fixed rate and counts
// what it receives: unique packets, duplicates, reordering and one-way latency
// (CLOCK_MONOTONIC is shared across network namespaces on the same host).
//
// usage: udpgen -b ip:port [-p ip:port | -l] [-r pps] [-s size] [-d secs] [-g grace]
//   -l  learn the peer from the first received packet (like WG does)
//   -r 0 (default) only receives. Latency percentiles saturate at 200000 us.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x45474e44u /* "EGND" */
#define HIST_US 200000    /* 1us buckets up to 200ms */

struct hdr {
	uint32_t magic;
	uint32_t pad;
	uint64_t seq;
	uint64_t ts_ns;
};

static int sock;
static struct sockaddr_in peer;
static atomic_int have_peer;
static atomic_int stop_rx;
static long rate = 0, size = 1400, duration = 10, grace = 2;
static uint64_t max_seq_bits;
static uint8_t *seen;
static uint64_t rx_total, rx_unique, rx_dup, rx_reord, rx_late_bits, max_seen;
static uint64_t *hist;
static uint64_t hist_over, lat_sum_us;

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
	if (!c) {
		fprintf(stderr, "bad addr %s\n", s);
		exit(1);
	}
	*c = 0;
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port = htons(atoi(c + 1));
	if (inet_pton(AF_INET, buf, &a->sin_addr) != 1) {
		fprintf(stderr, "bad ip %s\n", buf);
		exit(1);
	}
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
		int n = recvmmsg(sock, msgs, B, MSG_WAITFORONE, NULL); /* SO_RCVTIMEO bounds the wait */
		if (n <= 0)
			continue;
		uint64_t t = now_ns();
		for (int i = 0; i < n; i++) {
			if (msgs[i].msg_len < sizeof(struct hdr))
				continue;
			struct hdr *h = (struct hdr *)bufs[i];
			if (h->magic != MAGIC)
				continue;
			if (!atomic_load(&have_peer)) {
				peer = from[i];
				atomic_store(&have_peer, 1);
			}
			rx_total++;
			uint64_t s = h->seq;
			if (s >= max_seq_bits) {
				rx_late_bits++;
				continue;
			}
			if (seen[s >> 3] & (1u << (s & 7))) {
				rx_dup++;
				continue;
			}
			seen[s >> 3] |= 1u << (s & 7);
			rx_unique++;
			if (s < max_seen)
				rx_reord++;
			else
				max_seen = s;
			uint64_t us = (t - h->ts_ns) / 1000;
			lat_sum_us += us;
			if (us < HIST_US)
				hist[us]++;
			else
				hist_over++;
		}
	}
	return NULL;
}

static uint64_t pct(double p)
{
	uint64_t target = (uint64_t)(p * rx_unique), acc = 0;
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
	int learn = 0, have_bind = 0, opt;
	while ((opt = getopt(argc, argv, "b:p:lr:s:d:g:")) != -1) {
		switch (opt) {
		case 'b': parse_addr(optarg, &bind_a); have_bind = 1; break;
		case 'p': parse_addr(optarg, &peer); atomic_store(&have_peer, 1); break;
		case 'l': learn = 1; break;
		case 'r': rate = atol(optarg); break;
		case 's': size = atol(optarg); break;
		case 'd': duration = atol(optarg); break;
		case 'g': grace = atol(optarg); break;
		default: fprintf(stderr, "bad args\n"); return 1;
		}
	}
	if (!have_bind || size < (long)sizeof(struct hdr) || size > 2000) {
		fprintf(stderr, "usage: udpgen -b ip:port [-p ip:port|-l] [-r pps] [-s size] [-d secs]\n");
		return 1;
	}
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
	max_seq_bits = (uint64_t)(rate > 0 ? rate : 1000000) * (duration + 5) + 1024;
	if (max_seq_bits < (1u << 24))
		max_seq_bits = 1u << 24;
	seen = calloc(max_seq_bits / 8 + 1, 1);
	hist = calloc(HIST_US, sizeof(uint64_t));
	pthread_t rx;
	pthread_create(&rx, NULL, rx_thread, NULL);

	uint64_t sent = 0, send_err = 0;
	if (rate > 0) {
		if (learn)
			while (!atomic_load(&have_peer))
				usleep(1000);
		char *pkt = calloc(1, size);
		struct hdr *h = (struct hdr *)pkt;
		h->magic = MAGIC;
		uint64_t start = now_ns(), end = start + duration * 1000000000ull;
		for (;;) {
			uint64_t t = now_ns();
			if (t >= end)
				break;
			uint64_t due = (t - start) * rate / 1000000000ull;
			while (sent < due) {
				h->seq = sent;
				h->ts_ns = now_ns();
				if (sendto(sock, pkt, size, 0, (struct sockaddr *)&peer, sizeof(peer)) < 0)
					send_err++;
				sent++;
			}
			struct timespec ts = { 0, 200 * 1000 }; /* 200us pacing granularity */
			nanosleep(&ts, NULL);
		}
		sleep(grace);
	} else {
		sleep(duration + grace);
	}
	atomic_store(&stop_rx, 1);
	pthread_join(rx, NULL);
	printf("sent=%lu send_err=%lu rx=%lu uniq=%lu dup=%lu reord=%lu lat_avg_us=%lu p50_us=%lu p99_us=%lu "
	       "p999_us=%lu over200ms=%lu\n",
	       sent, send_err, rx_total, rx_unique, rx_dup, rx_reord,
	       rx_unique ? lat_sum_us / rx_unique : 0, rx_unique ? pct(0.50) : 0,
	       rx_unique ? pct(0.99) : 0, rx_unique ? pct(0.999) : 0, hist_over);
	return 0;
}
