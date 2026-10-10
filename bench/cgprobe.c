/* cgprobe: protocol 4 datagrams by hand, for the replay scenario
 * (bench/lab.d/replay.sh): a probe with or without a cookie, a DATA packet,
 * or a datagram saved earlier, sent from a chosen address and port; it
 * prints what the server answered within the timeout.
 *
 *   cgprobe -k KEY -s SERVER -b BIND [options] probe SESSION LINK [COOKIE]
 *   cgprobe -k KEY -s SERVER -b BIND [options] data SESSION LINK
 *   cgprobe -s SERVER -b BIND [options] raw FILE
 *   cgprobe -k KEY hint
 *
 * KEY is the base64 "key" of the configuration; SERVER and BIND are
 * ADDR:PORT. Options: -q SEQ (sequence, default random), -T TX_NEXT and -R
 * RX_TOP (the probe's view of our windows), -P (ask for IP pass), -H HINT
 * (a client hint other than the key's), -t MS (wait, 1000), -w FILE (save
 * the datagram sent). Output, one line: "hello COOKIE", "refused COOKIE",
 * "reply", "data", "none", "bad-mac TYPE" or "bad-echo". Exit 0 unless it
 * could not send.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "pair.h"
#include "proto.h"
#include "util.h"

static uint32_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

static int usage(void)
{
	fprintf(stderr, "usage: cgprobe -k KEY -s SERVER -b BIND [-q SEQ] [-T TX_NEXT] [-R RX_TOP] [-P] [-H HINT]\n"
			"               [-t MS] [-w FILE] probe SESSION LINK [COOKIE] | data SESSION LINK | raw FILE\n"
			"       cgprobe -k KEY hint\n");
	return 2;
}

static int addr(const char *s, struct sockaddr_storage *a)
{
	char err[160];

	if (cg_addr_parse(s, 0, a, 1, err, sizeof(err)) != 1) {
		fprintf(stderr, "cgprobe: %s: %s\n", s, err);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t key[CG_KEY_LEN], pkt[2048], in[2048];
	struct sockaddr_storage srv, bind_to;
	const char *keyb64 = NULL, *srvs = NULL, *binds = NULL, *save = NULL;
	uint32_t seq, tx_next = 0, rx_top = 0, ts = now_us();
	int have_key = 0, hint = -1, pass = 0, timeout = 1000, o, fd;
	size_t len = 0;
	ssize_t n;
	struct pollfd p;

	if (getrandom(&seq, sizeof(seq), 0) != sizeof(seq))
		seq = ts;
	while ((o = getopt(argc, argv, "k:s:b:q:T:R:PH:t:w:")) != -1) {
		switch (o) {
		case 'k': keyb64 = optarg; break;
		case 's': srvs = optarg; break;
		case 'b': binds = optarg; break;
		case 'q': seq = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'T': tx_next = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'R': rx_top = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'P': pass = 1; break;
		case 'H': hint = (int)strtol(optarg, NULL, 0); break;
		case 't': timeout = atoi(optarg); break;
		case 'w': save = optarg; break;
		default: return usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (keyb64) {
		if (cg_base64_decode(key, sizeof(key), keyb64) != CG_KEY_LEN) {
			fprintf(stderr, "cgprobe: KEY: expected the base64 of %d bytes\n", CG_KEY_LEN);
			return 2;
		}
		have_key = 1;
	}
	if (argc == 1 && !strcmp(argv[0], "hint") && have_key) {
		printf("%u\n", cg_client_hint(key));
		return 0;
	}
	if (argc < 2 || !srvs || !binds || addr(srvs, &srv) < 0 || addr(binds, &bind_to) < 0)
		return usage();
	if (!strcmp(argv[0], "raw")) {
		FILE *f = fopen(argv[1], "rb");

		if (!f || (len = fread(pkt, 1, sizeof(pkt), f)) < CG_HDR_LEN) {
			fprintf(stderr, "cgprobe: %s: cannot read a datagram\n", argv[1]);
			return 2;
		}
		fclose(f);
	} else if ((!strcmp(argv[0], "probe") || !strcmp(argv[0], "data")) && argc >= 3 && have_key) {
		struct cg_hdr h = { .type = strcmp(argv[0], "probe") ? CG_T_DATA : CG_T_PROBE,
				    .hint = (uint8_t)(hint >= 0 ? hint : cg_client_hint(key)),
				    .link = (uint8_t)atoi(argv[2]),
				    .session = (uint32_t)strtoul(argv[1], NULL, 16),
				    .seq = seq,
				    .ts = ts };

		if (h.type == CG_T_PROBE) {
			struct cg_probe_info pi = { .interval_ms = 100,
						    .cookie = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0,
						    .rx_top = rx_top,
						    .tx_next = tx_next };

			h.flags = (uint8_t)cg_pass_flags(pass ? 1 : -1);
			cg_probe_info_write(pkt + CG_HDR_LEN, &pi);
			len = CG_HDR_LEN + CG_PROBE_INFO_LEN;
		} else {
			memset(pkt + CG_HDR_LEN, 0, 32);
			pkt[CG_HDR_LEN] = 4; /* a WireGuard data message, as far as anyone can tell */
			len = CG_HDR_LEN + 64;
		}
		cg_hdr_write(pkt, &h, key, pkt + CG_HDR_LEN, len - CG_HDR_LEN);
	} else {
		return usage();
	}
	if (save) {
		FILE *f = fopen(save, "wb");

		if (!f || fwrite(pkt, 1, len, f) != len || fclose(f)) {
			fprintf(stderr, "cgprobe: %s: %s\n", save, strerror(errno));
			return 2;
		}
	}
	fd = socket(srv.ss_family, SOCK_DGRAM, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&bind_to, cg_addr_len(&bind_to)) < 0 ||
	    sendto(fd, pkt, len, 0, (struct sockaddr *)&srv, cg_addr_len(&srv)) != (ssize_t)len) {
		fprintf(stderr, "cgprobe: send from %s to %s: %s\n", binds, srvs, strerror(errno));
		return 1;
	}
	p.fd = fd;
	p.events = POLLIN;
	if (poll(&p, 1, timeout) <= 0 || (n = recv(fd, in, sizeof(in), 0)) < CG_HDR_LEN) {
		puts("none");
		return 0;
	}
	{
		struct cg_hdr h, sent;

		if (cg_hdr_parse(&h, in, (size_t)n) < 0) {
			puts("malformed");
			return 0;
		}
		if (have_key && !cg_hdr_verify(in, (size_t)n, key + CG_SIPHASH_KEY_LEN)) {
			printf("bad-mac %u\n", h.type);
			return 0;
		}
		if (h.type == CG_T_HELLO) {
			struct cg_hello hl;

			cg_hello_read(&hl, in + CG_HDR_LEN);
			if (cg_hdr_parse(&sent, pkt, len) < 0 || hl.echo_ts != sent.ts)
				puts("bad-echo");
			else
				printf("%s %08x\n", h.flags & CG_F_REFUSED ? "refused" : "hello", hl.cookie);
		} else {
			puts(h.type == CG_T_PROBE_REPLY ? "reply" : h.type == CG_T_DATA ? "data" : "other");
		}
	}
	return 0;
}
