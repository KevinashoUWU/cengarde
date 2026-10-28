// techo: the ceiling capture test's traffic (story 029). Plain-text records of
// a fixed size, each with its link, its number and the time it was written,
// over TCP (one record per segment: both ends fix the MSS to the record) or
// UDP (one record per datagram, at a fixed rate), so that a capture shows the
// number inside every packet and the TCP sequence agrees with it
// (seq - ISN = (n + 1) x size, the hello taking the first slot). Each end
// logs every record: when it wrote or sent it, and when TCP delivered it or
// the UDP datagram came (the kernel's receive time).
//
// usage:
//   techo serve [-t TPORT] [-u UPORT] [-r SIZE] [-c CC] -o DIR
//       the VPS end: TCP and UDP, both directions, any number of links
//   techo tcp-up|tcp-down|udp-up|udp-down -s HOST [-t TPORT|-u UPORT] -l LINK
//       [-i DEV] [-r SIZE] [-c CC] [-R MBITS] -T SECS -o FILE
//       the PC end of one link: -i binds to the link's device (the PC's VLAN
//       of that modem), -R is the UDP rate (Mbit/s of records)
//
// The record (SIZE bytes, 1300 by default, which fits any path of the
// testnet with IPv4 and TCP timestamps):
//   "CGT L2 Q000000000123 T1730148392123456789 " then readable filler, '\n'
// A hello in the same format ("CGH ...") opens each TCP connection and asks
// the server what to do; a UDP hello does so for the download, and a "CGK"
// every 5 s keeps the carrier's NAT open.
//
// The log is binary, 32 bytes per record, little-endian:
//   u64 number, i64 time inside the record (written, ns since the epoch),
//   i64 this end's time (sent for a sender; delivered or received for a
//   receiver), u8 link, u8 kind (1 tcp sent, 2 tcp delivered, 3 udp sent,
//   4 udp received), u16 0, u32 bytes
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/net_tstamp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REC_MIN 128
#define REC_MAX 1472
#define TCP_TS_OPT 12 /* the timestamps option in every segment */
#define BATCH 32      /* records per TCP write */

enum { K_TCP_SENT = 1, K_TCP_DELIVERED, K_UDP_SENT, K_UDP_RECEIVED };

struct logrec {
	uint64_t num;
	int64_t t_rec;
	int64_t t_here;
	uint8_t link;
	uint8_t kind;
	uint16_t pad;
	uint32_t bytes;
};

static int rec_size = 1300;
static const char *cc_name = "bbr";
static volatile sig_atomic_t stopping;

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

// One record: header at fixed offsets, filler, '\n'.
static void fill(char *p, char tag, int link, uint64_t num, int64_t t)
{
	static const char filler[] = "ceiling capture of cengarde, one record per packet; ";
	int n = snprintf(p, rec_size, "CG%c L%d Q%012llu T%019lld ", tag, link % 10, (unsigned long long)num, (long long)t);

	for (int i = n, j = 0; i < rec_size - 1; i++, j = (j + 1) % (int)(sizeof(filler) - 1))
		p[i] = filler[j];
	p[rec_size - 1] = '\n';
}

// A record's header, back: 0 when it is not one.
static int parse(const char *p, int *link, uint64_t *num, int64_t *t)
{
	if (p[0] != 'C' || p[1] != 'G' || p[4] != 'L' || p[7] != 'Q' || p[21] != 'T')
		return 0;
	*link = p[5] - '0';
	*num = strtoull(p + 8, NULL, 10);
	*t = strtoll(p + 22, NULL, 10);
	return p[2];
}

struct log {
	FILE *f;
	pthread_mutex_t mu;
};

static void log_open(struct log *l, const char *path)
{
	l->f = fopen(path, "wb");
	if (!l->f)
		die(path);
	setvbuf(l->f, NULL, _IOFBF, 1 << 20);
	pthread_mutex_init(&l->mu, NULL);
}

static void log_put(struct log *l, uint64_t num, int64_t t_rec, int64_t t_here, int link, int kind)
{
	struct logrec r = { num, t_rec, t_here, (uint8_t)link, (uint8_t)kind, 0, (uint32_t)rec_size };

	pthread_mutex_lock(&l->mu);
	fwrite(&r, sizeof(r), 1, l->f);
	pthread_mutex_unlock(&l->mu);
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

// The MSS that makes one segment carry exactly one record (the timestamps
// option rides in every segment), and the congestion control.
static void tcp_tune(int fd)
{
	int mss = rec_size + TCP_TS_OPT, one = 1, lowat = 128 * 1024;

	if (setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) < 0)
		perror("TCP_MAXSEG");
	if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, cc_name, strlen(cc_name)) < 0)
		perror("TCP_CONGESTION");
	// written close to when it leaves: the write time inside means something
	setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

static void tcp_check_mss(int fd, const char *who)
{
	int mss = 0;
	socklen_t len = sizeof(mss);

	if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, &len) == 0 && mss != rec_size)
		fprintf(stderr, "techo: %s: segments of %d bytes, records of %d: not one record per segment\n", who, mss, rec_size);
}

// Records over TCP for secs: BATCH at a time, each stamped when written.
static uint64_t tcp_send(int fd, int link, int secs, struct log *l)
{
	char *buf = malloc((size_t)rec_size * BATCH);
	int64_t end = now_ns() + (int64_t)secs * 1000000000;
	uint64_t num = 0;

	if (!buf)
		die("malloc");
	while (!stopping && now_ns() < end) {
		int64_t t = now_ns();

		for (int i = 0; i < BATCH; i++)
			fill(buf + (size_t)i * rec_size, 'T', link, num + i, t);
		if (write_all(fd, buf, (size_t)rec_size * BATCH) < 0)
			break;
		for (int i = 0; i < BATCH; i++)
			log_put(l, num + i, t, t, link, K_TCP_SENT);
		num += BATCH;
	}
	free(buf);
	return num;
}

// Records from TCP until the peer closes: each logged when its last byte came.
static uint64_t tcp_recv(int fd, struct log *l)
{
	char *buf = malloc((size_t)rec_size * 64);
	size_t have = 0;
	uint64_t n = 0;

	if (!buf)
		die("malloc");
	for (;;) {
		ssize_t r = read(fd, buf + have, (size_t)rec_size * 64 - have);
		int64_t t = now_ns();
		size_t off = 0;

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		have += (size_t)r;
		for (; have - off >= (size_t)rec_size; off += (size_t)rec_size) {
			int link;
			uint64_t num;
			int64_t tr;

			if (parse(buf + off, &link, &num, &tr) == 'T') {
				log_put(l, num, tr, t, link, K_TCP_DELIVERED);
				n++;
			}
		}
		memmove(buf, buf + off, have - off);
		have -= off;
	}
	free(buf);
	return n;
}

// ---- the server

struct server {
	int tport, uport;
	const char *dir;
	struct log tlog, ulog;
	int ufd;
};

struct conn {
	struct server *s;
	int fd;
	char peer[64];
};

static void *tcp_conn(void *arg)
{
	struct conn *c = arg;
	char hello[REC_MAX + 1];
	char dir[8] = "";
	int link = 0, secs = 0;
	size_t got = 0;

	while (got < (size_t)rec_size) {
		ssize_t r = read(c->fd, hello + got, (size_t)rec_size - got);

		if (r <= 0)
			goto out;
		got += (size_t)r;
	}
	hello[got] = 0;
	// "CGH L2 Q000000000000 T... up 30 "
	if (hello[2] != 'H' || sscanf(hello + 42, "%7s %d", dir, &secs) != 2)
		goto out;
	link = hello[5] - '0';
	fprintf(stderr, "techo: tcp %s link %d from %s\n", dir, link, c->peer);
	tcp_check_mss(c->fd, c->peer);
	if (!strcmp(dir, "up"))
		tcp_recv(c->fd, &c->s->tlog);
	else if (!strcmp(dir, "down"))
		tcp_send(c->fd, link, secs, &c->s->tlog);
out:
	close(c->fd);
	free(c);
	return NULL;
}

static void *tcp_server(void *arg)
{
	struct server *s = arg;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(s->tport) };
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	if (fd < 0)
		die("socket");
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	tcp_tune(fd); // inherited: the MSS the SYN-ACK says
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0)
		die("tcp bind");
	for (;;) {
		struct sockaddr_in p;
		socklen_t pl = sizeof(p);
		int cfd = accept(fd, (struct sockaddr *)&p, &pl);
		struct conn *c;
		pthread_t th;

		if (cfd < 0)
			continue;
		c = calloc(1, sizeof(*c));
		c->s = s;
		c->fd = cfd;
		snprintf(c->peer, sizeof(c->peer), "%s:%d", inet_ntoa(p.sin_addr), ntohs(p.sin_port));
		tcp_tune(cfd);
		pthread_create(&th, NULL, tcp_conn, c);
		pthread_detach(th);
	}
	return NULL;
}

// Paced UDP records to dst for secs at mbits.
static uint64_t udp_send(int fd, const struct sockaddr *dst, socklen_t dl, int link, double mbits, int secs,
			 struct log *l)
{
	char buf[REC_MAX];
	int64_t gap = (int64_t)((double)rec_size * 8 * 1000 / mbits); // ns between records
	int64_t start = now_ns(), end = start + (int64_t)secs * 1000000000, next = start;
	uint64_t num = 0;

	while (!stopping) {
		int64_t t = now_ns();

		if (t >= end)
			break;
		if (t < next) {
			struct timespec d = { 0, next - t > 60000 ? next - t - 50000 : 0 };

			if (d.tv_nsec)
				nanosleep(&d, NULL);
			continue;
		}
		fill(buf, 'U', link, num, t);
		if (sendto(fd, buf, (size_t)rec_size, 0, dst, dl) < 0 && errno != ENOBUFS && errno != EAGAIN)
			break;
		log_put(l, num, t, t, link, K_UDP_SENT);
		num++;
		next += gap;
		if (t - next > 100000000) // over 100 ms behind: do not burst it all
			next = t;
	}
	return num;
}

struct udp_job {
	struct server *s;
	struct sockaddr_storage to;
	socklen_t tl;
	int link, secs;
	double mbits;
};

static void *udp_down(void *arg)
{
	struct udp_job *j = arg;
	uint64_t n = udp_send(j->s->ufd, (struct sockaddr *)&j->to, j->tl, j->link, j->mbits, j->secs, &j->s->ulog);

	fprintf(stderr, "techo: udp down link %d: %llu records\n", j->link, (unsigned long long)n);
	free(j);
	return NULL;
}

static void udp_enable_ts(int fd)
{
	int on = 1;

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0)
		perror("SO_TIMESTAMPNS");
}

// One datagram with its kernel receive time.
static ssize_t udp_recv(int fd, char *buf, size_t len, struct sockaddr_storage *from, socklen_t *fl, int64_t *t)
{
	char ctl[256];
	struct iovec iov = { buf, len };
	struct msghdr m = { .msg_name = from, .msg_namelen = *fl, .msg_iov = &iov, .msg_iovlen = 1,
			    .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	ssize_t r = recvmsg(fd, &m, 0);

	*fl = m.msg_namelen;
	*t = now_ns();
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); r > 0 && c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
			struct timespec ts;

			memcpy(&ts, CMSG_DATA(c), sizeof(ts));
			*t = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
		}
	return r;
}

static void *udp_server(void *arg)
{
	struct server *s = arg;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(s->uport) };
	int rcv = 32 << 20;
	char buf[REC_MAX + 1];

	s->ufd = socket(AF_INET, SOCK_DGRAM, 0);
	if (s->ufd < 0)
		die("socket");
	setsockopt(s->ufd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof(rcv));
	setsockopt(s->ufd, SOL_SOCKET, SO_SNDBUFFORCE, &rcv, sizeof(rcv));
	udp_enable_ts(s->ufd);
	if (bind(s->ufd, (struct sockaddr *)&a, sizeof(a)) < 0)
		die("udp bind");
	for (;;) {
		struct sockaddr_storage from;
		socklen_t fl = sizeof(from);
		int64_t t;
		ssize_t r = udp_recv(s->ufd, buf, sizeof(buf) - 1, &from, &fl, &t);
		int link, tag, secs;
		uint64_t num;
		int64_t tr;
		double mbits;
		char dir[8];

		if (r < 42)
			continue;
		buf[r] = 0;
		tag = parse(buf, &link, &num, &tr);
		if (tag == 'U')
			log_put(&s->ulog, num, tr, t, link, K_UDP_RECEIVED);
		else if (tag == 'H' && sscanf(buf + 42, "%7s %d %lf", dir, &secs, &mbits) == 3 && !strcmp(dir, "down")) {
			struct udp_job *j = calloc(1, sizeof(*j));
			pthread_t th;

			j->s = s;
			memcpy(&j->to, &from, fl);
			j->tl = fl;
			j->link = link;
			j->secs = secs;
			j->mbits = mbits;
			fprintf(stderr, "techo: udp down link %d at %.1f Mbit/s for %d s\n", link, mbits, secs);
			pthread_create(&th, NULL, udp_down, j);
			pthread_detach(th);
		}
	}
	return NULL;
}

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static int serve(int tport, int uport, const char *dir)
{
	static struct server s;
	char path[512];
	pthread_t tt, ut;

	s.tport = tport;
	s.uport = uport;
	s.dir = dir;
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/tcp.log", dir);
	log_open(&s.tlog, path);
	snprintf(path, sizeof(path), "%s/udp.log", dir);
	log_open(&s.ulog, path);
	pthread_create(&tt, NULL, tcp_server, &s);
	pthread_create(&ut, NULL, udp_server, &s);
	while (!stopping) {
		sleep(1);
		pthread_mutex_lock(&s.tlog.mu);
		fflush(s.tlog.f);
		pthread_mutex_unlock(&s.tlog.mu);
		pthread_mutex_lock(&s.ulog.mu);
		fflush(s.ulog.f);
		pthread_mutex_unlock(&s.ulog.mu);
	}
	fclose(s.tlog.f);
	fclose(s.ulog.f);
	return 0;
}

// ---- the PC end of one link

static int resolve(const char *host, int port, struct sockaddr_in *a)
{
	struct addrinfo h = { .ai_family = AF_INET }, *r;

	if (getaddrinfo(host, NULL, &h, &r))
		return -1;
	memcpy(a, r->ai_addr, sizeof(*a));
	a->sin_port = htons(port);
	freeaddrinfo(r);
	return 0;
}

static void bind_dev(int fd, const char *dev)
{
	if (dev && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, dev, strlen(dev)) < 0)
		die("SO_BINDTODEVICE");
}

static void hello(char *buf, int link, const char *what)
{
	int n;

	fill(buf, 'H', link, 0, now_ns());
	n = snprintf(buf + 42, rec_size - 43, "%s ", what);
	buf[42 + n] = ' ';
}

static int client(const char *mode, const char *host, int tport, int uport, int link, const char *dev, double mbits,
		  int secs, const char *out)
{
	struct sockaddr_in a;
	struct log l;
	char buf[REC_MAX + 1], what[64];
	uint64_t n = 0;
	int fd;

	log_open(&l, out);
	if (!strncmp(mode, "tcp-", 4)) {
		if (resolve(host, tport, &a) < 0)
			die("resolve");
		fd = socket(AF_INET, SOCK_STREAM, 0);
		bind_dev(fd, dev);
		tcp_tune(fd);
		if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
			die("connect");
		tcp_check_mss(fd, mode);
		snprintf(what, sizeof(what), "%s %d", mode + 4, secs);
		hello(buf, link, what);
		if (write_all(fd, buf, (size_t)rec_size) < 0)
			die("hello");
		if (!strcmp(mode, "tcp-up")) {
			n = tcp_send(fd, link, secs, &l);
			shutdown(fd, SHUT_WR);
			while (read(fd, buf, sizeof(buf)) > 0) // until the server has it all
				;
		} else {
			n = tcp_recv(fd, &l);
		}
	} else {
		if (resolve(host, uport, &a) < 0)
			die("resolve");
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		bind_dev(fd, dev);
		if (!strcmp(mode, "udp-up")) {
			n = udp_send(fd, (struct sockaddr *)&a, sizeof(a), link, mbits, secs, &l);
		} else {
			int64_t end = now_ns() + ((int64_t)secs + 3) * 1000000000, keep = 0;
			struct timeval tv = { 0, 200000 };
			int rcv = 32 << 20;

			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof(rcv));
			udp_enable_ts(fd);
			snprintf(what, sizeof(what), "down %d %.3f", secs, mbits);
			while (!stopping && now_ns() < end) {
				struct sockaddr_storage from;
				socklen_t fl = sizeof(from);
				int64_t t, tr;
				int lk;
				uint64_t num;
				ssize_t r;

				if (!keep) { // once: the server starts on it, and the NAT opens
					hello(buf, link, what);
					sendto(fd, buf, (size_t)rec_size, 0, (struct sockaddr *)&a, sizeof(a));
					keep = now_ns();
				} else if (now_ns() - keep > 5000000000) { // the NAT stays open
					fill(buf, 'K', link, 0, now_ns());
					sendto(fd, buf, (size_t)rec_size, 0, (struct sockaddr *)&a, sizeof(a));
					keep = now_ns();
				}
				r = udp_recv(fd, buf, sizeof(buf) - 1, &from, &fl, &t);
				if (r >= 42 && parse(buf, &lk, &num, &tr) == 'U') {
					log_put(&l, num, tr, t, lk, K_UDP_RECEIVED);
					n++;
				}
			}
		}
	}
	fclose(l.f);
	fprintf(stderr, "techo: %s link %d: %llu records\n", mode, link, (unsigned long long)n);
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = NULL, *dev = NULL, *out = NULL, *mode;
	int tport = 5310, uport = 5320, link = 0, secs = 30, c;
	double mbits = 10;

	if (argc < 2) {
		fprintf(stderr, "usage: techo serve|tcp-up|tcp-down|udp-up|udp-down [options] (see techo.c)\n");
		return 2;
	}
	mode = argv[1];
	optind = 2;
	while ((c = getopt(argc, argv, "s:t:u:l:i:r:c:R:T:o:")) != -1) {
		switch (c) {
		case 's': host = optarg; break;
		case 't': tport = atoi(optarg); break;
		case 'u': uport = atoi(optarg); break;
		case 'l': link = atoi(optarg); break;
		case 'i': dev = optarg; break;
		case 'r': rec_size = atoi(optarg); break;
		case 'c': cc_name = optarg; break;
		case 'R': mbits = atof(optarg); break;
		case 'T': secs = atoi(optarg); break;
		case 'o': out = optarg; break;
		default: return 2;
		}
	}
	if (rec_size < REC_MIN || rec_size > REC_MAX || link < 0 || link > 9 || mbits <= 0 || !out) {
		fprintf(stderr, "techo: -r %d..%d, -l 0..9, -R above 0 and -o are needed\n", REC_MIN, REC_MAX);
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (!strcmp(mode, "serve"))
		return serve(tport, uport, out);
	if (!host) {
		fprintf(stderr, "techo: -s HOST is needed\n");
		return 2;
	}
	return client(mode, host, tport, uport, link, dev, mbits, secs, out);
}
