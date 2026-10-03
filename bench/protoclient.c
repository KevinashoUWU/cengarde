// protoclient: minimal C engarde-client data path, for benchmarking only.
// Wire-compatible with engarde-server (raw packets, no extra header).
// One epoll thread, recvmmsg/sendmmsg batches, non-blocking sends that drop
// on EAGAIN instead of stalling the other links, optional exact-duplicate
// suppression before handing packets to WireGuard.
//
// usage: protoclient -l 127.0.0.1:59401 [-D] -L ifname,srcip,dstip:port [-L ...]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BATCH 64
#define MAXL 16
#define PKT 2048
#define DEDUP_SLOTS (1 << 16)

struct link {
	char ifname[32];
	int fd;
	struct sockaddr_in dst;
	uint64_t tx, tx_drop, rx;
};

static struct link links[MAXL];
static int nlinks, wg_fd, dedup;
static struct sockaddr_in wg_addr;
static int have_wg;
static volatile sig_atomic_t stop;
static uint64_t fwd_up, fwd_down, dup_dropped, wg_tx_drop;
static uint64_t dd_key[DEDUP_SLOTS];

static void parse_addr(const char *s, struct sockaddr_in *a)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", s);
	char *c = strrchr(buf, ':');
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	if (c) {
		*c = 0;
		a->sin_port = htons(atoi(c + 1));
	}
	if (inet_pton(AF_INET, buf, &a->sin_addr) != 1) {
		fprintf(stderr, "bad address %s\n", s);
		exit(1);
	}
}

static int mksock(const struct sockaddr_in *bind_a, const char *ifname)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	int buf = 4 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &buf, sizeof(buf));
	if (ifname && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0)
		perror("SO_BINDTODEVICE");
	if (bind(fd, (const struct sockaddr *)bind_a, sizeof(*bind_a)) < 0) {
		perror("bind");
		exit(1);
	}
	return fd;
}

// FNV-1a over the first 64 bytes + length: identifies exact copies.
// (A real implementation keys on WG receiver index + counter + auth tag.)
static uint64_t pkt_key(const uint8_t *p, unsigned len)
{
	uint64_t h = 1469598103934665603ull ^ len;
	unsigned n = len < 64 ? len : 64;
	for (unsigned i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h | 1;
}

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

int main(int argc, char **argv)
{
	struct sockaddr_in listen_a;
	int opt, have_listen = 0;
	while ((opt = getopt(argc, argv, "l:L:D")) != -1) {
		if (opt == 'l') {
			parse_addr(optarg, &listen_a);
			have_listen = 1;
		} else if (opt == 'D') {
			dedup = 1;
		} else if (opt == 'L' && nlinks < MAXL) {
			char *a = strdup(optarg), *src = strchr(a, ','), *dst;
			if (!src || !(dst = strchr(src + 1, ','))) {
				fprintf(stderr, "bad link %s\n", optarg);
				return 1;
			}
			*src++ = 0;
			*dst++ = 0;
			struct link *l = &links[nlinks++];
			snprintf(l->ifname, sizeof(l->ifname), "%s", a);
			struct sockaddr_in sa;
			parse_addr(src, &sa);
			l->fd = mksock(&sa, l->ifname);
			parse_addr(dst, &l->dst);
		}
	}
	if (!have_listen || !nlinks) {
		fprintf(stderr, "usage: protoclient -l ip:port [-D] -L ifname,srcip,dstip:port ...\n");
		return 1;
	}
	wg_fd = mksock(&listen_a, NULL);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	int ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLIN, .data.u32 = MAXL };
	epoll_ctl(ep, EPOLL_CTL_ADD, wg_fd, &ev);
	for (int i = 0; i < nlinks; i++) {
		ev.data.u32 = i;
		epoll_ctl(ep, EPOLL_CTL_ADD, links[i].fd, &ev);
	}

	static uint8_t bufs[BATCH][PKT];
	struct mmsghdr in[BATCH], out[BATCH];
	struct iovec iov[BATCH];
	struct sockaddr_in from[BATCH];
	struct epoll_event evs[MAXL + 1];

	while (!stop) {
		int ne = epoll_wait(ep, evs, MAXL + 1, 200);
		for (int e = 0; e < ne; e++) {
			uint32_t id = evs[e].data.u32;
			int fd = id == MAXL ? wg_fd : links[id].fd;
			for (;;) { /* drain the socket */
				for (int i = 0; i < BATCH; i++) {
					iov[i] = (struct iovec){ bufs[i], PKT };
					in[i].msg_hdr = (struct msghdr){ .msg_name = &from[i], .msg_namelen = sizeof(from[i]),
									 .msg_iov = &iov[i], .msg_iovlen = 1 };
				}
				int n = recvmmsg(fd, in, BATCH, MSG_DONTWAIT, NULL);
				if (n <= 0)
					break;
				if (id == MAXL) {
					/* WireGuard -> every link. Never block: a full link drops, others go on. */
					wg_addr = from[n - 1];
					have_wg = 1;
					fwd_up += n;
					for (int l = 0; l < nlinks; l++) {
						for (int i = 0; i < n; i++) {
							iov[i].iov_len = in[i].msg_len;
							out[i].msg_hdr = (struct msghdr){ .msg_name = &links[l].dst,
											  .msg_namelen = sizeof(links[l].dst),
											  .msg_iov = &iov[i], .msg_iovlen = 1 };
						}
						int s = sendmmsg(links[l].fd, out, n, MSG_DONTWAIT);
						if (s < 0)
							s = 0;
						links[l].tx += s;
						links[l].tx_drop += n - s;
					}
				} else {
					/* link -> WireGuard, first copy wins */
					links[id].rx += n;
					int m = 0;
					for (int i = 0; i < n; i++) {
						if (dedup) {
							uint64_t k = pkt_key(bufs[i], in[i].msg_len);
							uint64_t *slot = &dd_key[k & (DEDUP_SLOTS - 1)];
							if (*slot == k) {
								dup_dropped++;
								continue;
							}
							*slot = k;
						}
						iov[m] = (struct iovec){ bufs[i], in[i].msg_len };
						m++;
					}
					if (!have_wg || !m)
						continue;
					for (int i = 0; i < m; i++)
						out[i].msg_hdr = (struct msghdr){ .msg_name = &wg_addr, .msg_namelen = sizeof(wg_addr),
										  .msg_iov = &iov[i], .msg_iovlen = 1 };
					int s = sendmmsg(wg_fd, out, m, MSG_DONTWAIT);
					if (s < 0)
						s = 0;
					fwd_down += s;
					wg_tx_drop += m - s;
				}
				if (n < BATCH)
					break;
			}
		}
	}
	fprintf(stderr, "up=%lu down=%lu dup_dropped=%lu wg_tx_drop=%lu\n", fwd_up, fwd_down, dup_dropped, wg_tx_drop);
	for (int l = 0; l < nlinks; l++)
		fprintf(stderr, "  %s tx=%lu tx_drop=%lu rx=%lu\n", links[l].ifname, links[l].tx, links[l].tx_drop,
			links[l].rx);
	return 0;
}
