/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>

#include "config.h"
#include "ctl.h"
#include "engine.h"
#include "log.h"
#include "pair.h"
#include "proto.h"
#include "thrplan.h"
#include "util.h"

static void usage(FILE *f)
{
	fprintf(f, "usage: cengarde [-v] -c FILE     run as client or server (mode in FILE)\n"
		   "       cengarde -t -c FILE        check FILE and exit\n"
		   "       cengarde ctl [-s SOCKET | -c FILE] COMMAND\n"
		   "                                  talk to a running cengarde through its\n"
		   "                                  control_socket (default " CG_CTL_DEFAULT_SOCKET "):\n"
		   "                                  status, links, link NAME off|on|auto, reset, reload,\n"
		   "                                  threads\n"
		   "       cengarde genkey            print a new shared key or pairing secret\n"
		   "       cengarde keys < SECRET     print the keys, tunnel addresses and client hint\n"
		   "                                  derived from a pairing secret\n"
		   "       cengarde version           print the version and the protocol version\n"
		   "SIGHUP reloads the configuration without dropping the tunnel.\n");
}

/* "cengarde ctl": one command to the control socket (ctl.h). */
static int ctl(int argc, char **argv)
{
	char sock[sizeof(((struct cg_config *)0)->control_socket)] = CG_CTL_DEFAULT_SOCKET;
	char line[CG_CTL_LINE] = "", err[512], *reply;
	const char *conf = NULL;
	struct cg_ctl_cmd cmd;
	size_t len, n = 0;
	int opt, rc;

	while ((opt = getopt(argc, argv, "s:c:h")) != -1) {
		switch (opt) {
		case 's':
			if (strlen(optarg) >= sizeof(sock)) {
				fprintf(stderr, "cengarde ctl: socket path too long\n");
				return 2;
			}
			strcpy(sock, optarg);
			break;
		case 'c':
			conf = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind == argc) {
		usage(stderr);
		return 2;
	}
	for (int i = optind; i < argc; i++) {
		size_t w = strlen(argv[i]);

		if (n + w + 2 > sizeof(line)) {
			fprintf(stderr, "cengarde ctl: command too long\n");
			return 2;
		}
		if (n)
			line[n++] = ' ';
		memcpy(line + n, argv[i], w + 1);
		n += w;
	}
	if (cg_ctl_parse(line, &cmd, err, sizeof(err)) < 0) {
		fprintf(stderr, "cengarde ctl: %s\n", err);
		return 2;
	}
	if (conf) {
		rc = cg_config_peek(conf, "control_socket", sock, sizeof(sock), err, sizeof(err));
		if (rc) {
			fprintf(stderr, "cengarde ctl: %s\n", rc < 0 ? err : "no control_socket in that file");
			return 1;
		}
	}
	if (cg_ctl_request(sock, line, cmd.op == CG_CTL_RELOAD ? CG_CTL_RELOAD_TIMEOUT_MS + 5000 : CG_CTL_TIMEOUT_MS,
			   &reply, &len, err, sizeof(err)) < 0) {
		fprintf(stderr, "cengarde ctl: %s\n", err);
		return 1;
	}
	if (len >= 7 && !memcmp(reply, "error: ", 7)) {
		fprintf(stderr, "cengarde ctl: %s", reply + 7);
		rc = 1;
	} else {
		fwrite(reply, 1, len, stdout);
		rc = 0;
	}
	free(reply);
	return rc;
}

/* Reads the secret from stdin so that it never shows in the process list,
 * and prints shell assignments (base64 needs no quoting inside '...').
 * cengarde-setup and cengarde-vps-setup eval them: new lines go last and
 * the old ones never change. */
static int keys(void)
{
	uint8_t secret[CG_PAIR_LEN], k[CG_PAIR_LEN], a4[4], ula[CG_PAIR_ULA_LEN];
	char line[128], out[64];
	size_t n;
	int rc = 0;

	if (!fgets(line, sizeof(line), stdin)) {
		fprintf(stderr, "keys: no secret on standard input\n");
		return 1;
	}
	n = strcspn(line, " \t\r\n");
	line[n] = 0;
	if (cg_base64_decode(secret, sizeof(secret), line) != CG_PAIR_LEN) {
		fprintf(stderr, "keys: expected the base64 of %d bytes (see 'cengarde genkey')\n", CG_PAIR_LEN);
		rc = 1;
	} else {
		for (int i = 0; i < CG_PAIR_NKEYS; i++) {
			cg_pair_derive(k, secret, (enum cg_pair_key)i);
			cg_base64_encode(out, k, sizeof(k));
			printf("CG_%s='%s'\n", cg_pair_name((enum cg_pair_key)i), out);
		}
		cg_pair_tunnel4(a4, secret);
		cg_pair_tunnel_ula(ula, secret);
		cg_pair_derive(k, secret, CG_PAIR_LINK);
		/* The ULA as a /48 prefix, four digits per group, so that
		 * "${CG_TUNNEL_ULA}::2" is an address. */
		printf("CG_TUNNEL_ADDR='%d.%d.%d.%d'\n"
		       "CG_TUNNEL_ULA='%02x%02x:%02x%02x:%02x%02x'\n"
		       "CG_CLIENT_HINT='%d'\n",
		       a4[0], a4[1], a4[2], a4[3], ula[0], ula[1], ula[2], ula[3], ula[4], ula[5], cg_client_hint(k));
	}
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(k, sizeof(k));
	explicit_bzero(line, sizeof(line));
	explicit_bzero(out, sizeof(out));
	return rc;
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
	struct cg_config *cfg;
	struct cg_run run;
	char err[512], warn[2048];
	const char *path = NULL;
	int opt, check = 0, verbose = 0, sigfd;
	sigset_t set;

	if (argc == 2 && !strcmp(argv[1], "genkey"))
		return genkey();
	if (argc == 2 && !strcmp(argv[1], "keys"))
		return keys();
	if (argc >= 2 && !strcmp(argv[1], "ctl"))
		return ctl(argc - 1, argv + 1);
	if (argc == 2 && !strcmp(argv[1], "version")) {
		printf("cengarde %s (protocol %d)\n", CG_VERSION, CG_PROTO_VERSION);
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
	cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		cg_err("out of memory");
		return 1;
	}
	if (cg_config_load(cfg, path, err, sizeof(err), warn, sizeof(warn)) < 0) {
		cg_err("%s", err);
		free(cfg);
		return 1;
	}
	cg_log_level = verbose ? CG_LOG_DEBUG : cfg->log_level;
	cg_log_warnings(path, warn);
	if (check) {
		if (cfg->mode == CG_MODE_CLIENT) {
			/* What link_threads comes to on this machine (thrplan.h). */
			static const char *const what[] = { [CG_LT_ON] = "a thread per link, up to 8",
							    [CG_LT_OFF] = "the per-link structure in one thread",
							    [CG_LT_LEGACY] = "the loop of 0.4" };
			cpu_set_t cpus;
			long n = sched_getaffinity(0, sizeof(cpus), &cpus) == 0 ? CPU_COUNT(&cpus) : 1;
			int lt = cg_lt_resolve(cfg->link_threads, CG_LT_ARCH_MEASURED, (int)n, cfg->cpu >= 0, CG_LT_AUTO_ON);

			printf("%s: ok (client, link_threads %s: %s%s%s)\n", path, cg_lt_name(cfg->link_threads),
			       cfg->link_threads == CG_LT_AUTO ? cg_lt_name(lt) : "", cfg->link_threads == CG_LT_AUTO ? ", " : "",
			       what[lt]);
		} else {
			printf("%s: ok (server)\n", path);
		}
		cg_config_free(cfg);
		free(cfg);
		return 0;
	}

	/* Blocked before anything else: a restart in place (cg_reexec) keeps
	 * them blocked, so a signal sent meanwhile waits in the signalfd. */
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &set, NULL) < 0 || (sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC)) < 0) {
		cg_err("signalfd: %s", strerror(errno));
		return 1;
	}
	run = (struct cg_run){ .path = path, .argv = argv, .sigfd = sigfd, .verbose = verbose };
	return cfg->mode == CG_MODE_CLIENT ? cg_client_run(cfg, &run) : cg_server_run(cfg, &run);
}
