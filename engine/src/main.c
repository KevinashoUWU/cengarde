/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>

#include "config.h"
#include "engine.h"
#include "log.h"
#include "util.h"

static void usage(FILE *f)
{
	fprintf(f, "usage: cengarde [-v] -c FILE     run as client or server (mode in FILE)\n"
		   "       cengarde -t -c FILE        check FILE and exit\n"
		   "       cengarde genkey            print a new shared key\n"
		   "       cengarde version\n");
}

static int genkey(void)
{
	uint8_t key[CG_KEY_LEN];
	char out[64];

	if (cg_random(key, sizeof(key)) < 0) {
		fprintf(stderr, "getrandom: %s\n", strerror(errno));
		return 1;
	}
	cg_base64_encode(out, key, sizeof(key));
	puts(out);
	return 0;
}

int main(int argc, char **argv)
{
	static struct cg_config cfg;
	char err[512], warn[2048], *line, *save = NULL;
	const char *path = NULL;
	int opt, check = 0, verbose = 0, rc, sigfd;
	sigset_t set;

	if (argc == 2 && !strcmp(argv[1], "genkey"))
		return genkey();
	if (argc == 2 && !strcmp(argv[1], "version")) {
		puts("cengarde " CG_VERSION);
		return 0;
	}
	while ((opt = getopt(argc, argv, "c:tvh")) != -1) {
		switch (opt) {
		case 'c':
			path = optarg;
			break;
		case 't':
			check = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (!path || optind != argc) {
		usage(stderr);
		return 2;
	}
	if (cg_config_load(&cfg, path, err, sizeof(err), warn, sizeof(warn)) < 0) {
		cg_err("%s: %s", path, err);
		return 1;
	}
	cg_log_level = verbose ? CG_LOG_DEBUG : cfg.log_level;
	for (line = strtok_r(warn, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		cg_warn("%s: %s", path, line);
	if (check) {
		printf("%s: ok (%s)\n", path, cfg.mode == CG_MODE_CLIENT ? "client" : "server");
		cg_config_free(&cfg);
		return 0;
	}

	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &set, NULL) < 0 || (sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC)) < 0) {
		cg_err("signalfd: %s", strerror(errno));
		return 1;
	}
	rc = cfg.mode == CG_MODE_CLIENT ? cg_client_run(&cfg, sigfd) : cg_server_run(&cfg, sigfd);
	cg_config_free(&cfg);
	return rc;
}
