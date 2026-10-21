/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "linkmodel.h"
#include "test.h"
#include "util.h"

#define KEY "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=" /* bytes 0..31 */

static const char client_conf[] = "# cengarde client\n"
				  "mode = client\n"
				  "key = " KEY "\n"
				  "description = \"Pi #1 (rooftop)\"\n"
				  "server = 203.0.113.10:59402 [2001:db8::1]:59402\n"
				  "interfaces = eth1.* wwan0   # VLANs and the modem\n"
				  "exclude = eth1.99\n"
				  "probe_interval_ms = 500\n"
				  "\n"
				  "[link   eth1.10]\n"
				  "label = WOM\n"
				  "[link eth1.20]\n"
				  "label = Entel\n"
				  "server = 198.51.100.7:59402\n"
				  "enabled = no\n"
				  "colour = blue\n";

#define SERVER "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\n"
#define CLIENT "mode = client\nkey = " KEY "\nserver = 192.0.2.1:59402\n"

/* Control socket, IP pass, and what a reload applies in place. */
static void test_config_reload(void)
{
	static struct cg_config a, b;
	char err[256], warn[512], path[] = "/tmp/cengarde-test-XXXXXX", out[108];
	FILE *f;
	int fd;

	CHECK_EQ(cg_config_parse(&a, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(a.passthrough, -1); /* asks nothing of the server */
	CHECK(a.control_socket[0] == '\0');
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT "passthrough = yes\ncontrol_socket = /var/run/cengarde/cengarde.sock\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(a.passthrough, 1);
	CHECK(!strcmp(a.control_socket, "/var/run/cengarde/cengarde.sock"));
	CHECK(warn[0] == '\0');
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT "passthrough = no\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(a.passthrough, 0);
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT "passthrough = maybe\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(strstr(err, "passthrough") != NULL);
	CHECK_EQ(cg_config_parse(&a, CLIENT "control_socket = cengarde.sock\n", err, sizeof(err), warn, sizeof(warn)),
		 -1); /* relative: refused */
	CHECK(strstr(err, "absolute") != NULL);
	CHECK_EQ(cg_config_parse(&a, SERVER "passthrough_file = /run/cengarde/passthrough\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(!strcmp(a.passthrough_file, "/run/cengarde/passthrough"));
	CHECK(!strcmp(a.wireguard_port_file, "")); /* none by default */
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, SERVER "wireguard_port_file = /var/lib/cengarde/wg/ports\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(!strcmp(a.wireguard_port_file, "/var/lib/cengarde/wg/ports"));
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT "wireguard_port_file = /run/w\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'wireguard_port_file'") != NULL); /* a server setting */
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, SERVER "passthrough = yes\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'passthrough'") != NULL); /* a client setting */
	cg_config_free(&a);

	/* In place: links, servers, health, probes, IP pass, status. */
	CHECK_EQ(cg_config_parse(&a, CLIENT "interfaces = eth1.*\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_parse(&b,
				 "mode = client\nkey = " KEY "\nserver = 198.51.100.7:1\ninterfaces = wwan*\n"
				 "passthrough = yes\nmute_behind_ms = 300\nprobe_interval_ms = 200\n"
				 "status_file = /tmp/x.json\nlog_level = debug\nsndbuf = 65536\n[link eth9]\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&a, &b) == NULL);
	cg_config_free(&b);
	/* A restart for what the event loop set up once. */
	CHECK_EQ(cg_config_parse(&b, CLIENT "interfaces = eth1.*\nlisten = 127.0.0.1:1\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&a, &b) && !strcmp(cg_config_restart_needed(&a, &b), "listen"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b,
				 "mode = client\nkey = AQECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\n"
				 "server = 192.0.2.1:59402\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "key"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "control_socket = /tmp/s\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "control_socket"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "cpu = 1\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "cpu"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "busy_poll_us = 10\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "busy_poll_us"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "rt_priority = 5\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "rt_priority"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "mode"));
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, SERVER "passthrough_file = /run/p\nsession_timeout_ms = 5000\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&a, &b) == NULL);
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, SERVER "max_sessions = 8\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(cg_config_restart_needed(&a, &b) == NULL); /* a limit: the table grows in place */
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51821\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "wireguard"));
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, SERVER "lanes = 4\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "lanes")); /* the socket group is built once */
	cg_config_free(&a);
	cg_config_free(&b);

	/* Peek: one key, without the rest of the file having to make sense. */
	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	fputs("mode = client\nserver = name.that.never.resolves.invalid:1\ncontrol_socket = /run/x.sock\n", f);
	fclose(f);
	CHECK_EQ(cg_config_peek(path, "control_socket", out, sizeof(out), err, sizeof(err)), 0);
	CHECK(!strcmp(out, "/run/x.sock"));
	CHECK_EQ(cg_config_peek(path, "status_file", out, sizeof(out), err, sizeof(err)), 1);
	CHECK_EQ(cg_config_peek(path, "control_socket", out, 4, err, sizeof(err)), -1);
	unlink(path);
	CHECK_EQ(cg_config_peek(path, "control_socket", out, sizeof(out), err, sizeof(err)), -1);
	CHECK(strstr(err, path) != NULL);
}

/* How the client handles its link sockets (pump.h): legacy by default. */
#define KEY2 "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA=" /* bytes 1..32 */
#define KEY3 "AgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICE=" /* bytes 2..33 */
#define MULTI "mode = server\nforward_file = /var/lib/cengarde/forward\nwireguard_port_file = /var/lib/cengarde/wg/ports\n"
#define ALPHA "[client alpha]\nkey = " KEY "\nwireguard = 127.0.0.1:65501\n"
#define BRAVO "[client bravo]\nkey = " KEY2 "\nwireguard = 127.0.0.1:65502\n"

#define NAME_ERR(sec) sec ": a client's name is 1 to 12 lowercase letters, digits or '-', not starting with " \
		     "'-' and not \"default\""

/* A server with [client NAME] sections. */
static void test_config_clients(void)
{
	static struct cg_config a, b;
	char err[256], warn[512], buf[64];
	const struct cg_client_cfg *k;

	CHECK_EQ(cg_config_parse(&a,
				 MULTI ALPHA "label = Roof\nwireguard_poke = 10.79.12.34:9\nforward = tcp:9000=22 both:6000\n"
				       BRAVO "enabled = no\npassthrough = no\nforward = udp:5000-5010=15000\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(warn[0] == '\0');
	CHECK_EQ(a.nclients, 2);
	k = cg_config_client(&a, "alpha");
	CHECK(k == &a.clients[0]);
	CHECK(!strcmp(k->label, "Roof"));
	CHECK_EQ(k->enabled, 1);
	CHECK_EQ(k->passthrough, 1);
	CHECK_EQ(k->key[31], 31);
	CHECK(!strcmp(cg_addr_str(&k->wireguard, buf, sizeof(buf)), "127.0.0.1:65501"));
	CHECK(!strcmp(cg_addr_str(&k->wireguard_poke, buf, sizeof(buf)), "10.79.12.34:9"));
	k = cg_config_client(&a, "bravo");
	CHECK(k == &a.clients[1]);
	CHECK_EQ(k->enabled, 0);
	CHECK_EQ(k->passthrough, 0);
	CHECK_EQ(k->wireguard_poke.ss_family, AF_UNSPEC); /* no default with several clients */
	CHECK(cg_config_client(&a, "charlie") == NULL);
	CHECK_EQ(a.nforward, 4); /* both: a tcp and a udp rule */
	CHECK_EQ(a.forward[0].client, 0);
	CHECK_EQ(a.forward[0].to, 22);
	CHECK_EQ(a.forward[3].client, 1);
	CHECK_EQ(a.forward[3].last, 5010);
	CHECK(!strcmp(a.forward_file, "/var/lib/cengarde/forward"));
	CHECK(!strcmp(a.wireguard_port_file, "/var/lib/cengarde/wg/ports"));
	CHECK_EQ(a.max_sessions, 64); /* at least CG_CLIENT_SESSIONS per client */

	/* Changes to the clients apply in place; going from one client to
	 * several (or back) is another server. */
	CHECK_EQ(cg_config_parse(&b, MULTI ALPHA "[client charlie]\nkey = " KEY3 "\nwireguard = 127.0.0.1:65503\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&a, &b) == NULL);
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "[client] sections"));
	CHECK(!strcmp(cg_config_restart_needed(&b, &a), "[client] sections"));
	cg_config_free(&b);
	cg_config_free(&a);
	CHECK(a.clients == NULL && a.forward == NULL && a.nclients == 0);

	/* A global WireGuard address serves the clients without their own. */
	CHECK_EQ(cg_config_parse(&a, "mode = server\nwireguard = 127.0.0.1:51820\n[client alpha]\nkey = " KEY "\n" BRAVO,
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_addr_str(&a.clients[0].wireguard, buf, sizeof(buf)), "127.0.0.1:51820"));
	CHECK(!strcmp(cg_addr_str(&a.clients[1].wireguard, buf, sizeof(buf)), "127.0.0.1:65502"));
	cg_config_free(&a);

	/* max_sessions: room for every client unless set. */
	{
		char text[16384];
		size_t n = (size_t)snprintf(text, sizeof(text), "mode = server\nwireguard = 127.0.0.1:51820\n");
		uint8_t key[CG_KEY_LEN];
		char b64[64];

		for (int i = 0; i < 20; i++) {
			memset(key, i, sizeof(key));
			cg_base64_encode(b64, key, sizeof(key));
			n += (size_t)snprintf(text + n, sizeof(text) - n, "[client r%d]\nkey = %s\n", i, b64);
		}
		CHECK_EQ(cg_config_parse(&a, text, err, sizeof(err), warn, sizeof(warn)), 0);
		CHECK_EQ(a.max_sessions, 20 * CG_CLIENT_SESSIONS);
		cg_config_free(&a);
		snprintf(text + n, sizeof(text) - n, "[client r0]\nlabel = x\n"); /* a section again: the same client */
		CHECK_EQ(cg_config_parse(&a, text, err, sizeof(err), warn, sizeof(warn)), 0);
		CHECK_EQ(a.nclients, 20);
		CHECK(!strcmp(a.clients[0].label, "x"));
		cg_config_free(&a);
		/* A max_sessions of its own is the limit. */
		{
			static char text2[sizeof(text) + 32];

			snprintf(text2, sizeof(text2), "max_sessions = 9\n%s", text);
			CHECK_EQ(cg_config_parse(&a, text2, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(a.max_sessions, 9);
			cg_config_free(&a);
		}
		/* At most CG_MAX_CLIENTS. */
		n = (size_t)snprintf(text, sizeof(text), "mode = server\nwireguard = 127.0.0.1:51820\n");
		for (int i = 0; i <= CG_MAX_CLIENTS; i++) {
			memset(key, i, sizeof(key));
			cg_base64_encode(b64, key, sizeof(key));
			n += (size_t)snprintf(text + n, sizeof(text) - n, "[client r%d]\nkey = %s\n", i, b64);
		}
		CHECK_EQ(cg_config_parse(&a, text, err, sizeof(err), warn, sizeof(warn)), -1);
		CHECK(!strcmp(err, "too many [client] sections (max 64)"));
	}

	/* Mistakes. */
	{
		static const struct {
			const char *text, *err;
		} bad[] = {
			{ MULTI "key = " KEY "\n" ALPHA, "key: not with [client] sections, each has its own" },
			{ MULTI "[client]\nkey = " KEY "\n", "[client]: expected [client NAME]" },
			{ MULTI "[client -x]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client -x]") },
			{ MULTI "[client a/b]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client a/b]") },
			{ MULTI "[client Alpha]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client Alpha]") },
			{ MULTI "[client r_1]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client r_1]") },
			{ MULTI "[client abcdefghijklm]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client abcdefghijklm]") },
			{ MULTI "[client default]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n",
			  NAME_ERR("[client default]") },
			{ MULTI "[client alpha]\nwireguard = 127.0.0.1:1\n",
			  "[client alpha] key: expected the base64 of 32 bytes" },
			{ MULTI "[client alpha]\nkey = " KEY "\n", "[client alpha] wireguard: required (no global one)" },
			{ MULTI ALPHA "[client bravo]\nkey = " KEY "\nwireguard = 127.0.0.1:2\n",
			  "[client bravo] key: the same as [client alpha]'s" },
			{ MULTI ALPHA "enabled = maybe\n", "[client alpha] enabled: expected yes or no, got 'maybe'" },
			{ MULTI ALPHA "wireguard_poke = 0.0.0.0:9\n", NULL },
			{ MULTI ALPHA "forward = tcp:70000\n", NULL },
			{ MULTI ALPHA "forward = tcp:9000-9010\n" BRAVO "forward = tcp:9010=22\n",
			  "forward: tcp 9000-9010 of [client alpha] overlaps tcp 9010-9010 of [client bravo]" },
			{ MULTI ALPHA "forward = both:53 udp:53\n",
			  "forward: udp 53-53 of [client alpha] overlaps udp 53-53 of [client alpha]" },
			{ "mode = server\nwireguard = 127.0.0.1:1\npassthrough_file = /run/p\n" ALPHA,
			  "passthrough_file: not with [client] sections, see forward_file" },
			{ SERVER "forward_file = /run/f\n",
			  "forward_file: only with [client] sections (one client: passthrough_file)" },
			{ MULTI "wireguard_poke = 10.79.0.2:9\n" ALPHA,
			  "wireguard_poke: with [client] sections, in each of them (that client's address in the tunnel)" },
		};

		for (size_t i = 0; i < CG_ARRAY_SIZE(bad); i++) {
			int rc = cg_config_parse(&a, bad[i].text, err, sizeof(err), warn, sizeof(warn));

			CHECK_EQ(rc, -1);
			if (rc == 0)
				cg_config_free(&a);
			else if (bad[i].err && strcmp(err, bad[i].err))
				fprintf(stderr, "case %zu: got \"%s\"\n", i, err);
			CHECK(!bad[i].err || !strcmp(err, bad[i].err));
		}
	}

	/* The longest name: cg-NAME fits in an interface name. */
	CHECK_EQ(cg_config_parse(&a, MULTI "[client abcdefghijk-]\nkey = " KEY "\nwireguard = 127.0.0.1:1\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	cg_config_free(&a);

	/* passthrough_file is global; in a [client] section, an unknown key. */
	CHECK_EQ(cg_config_parse(&a, MULTI ALPHA "passthrough_file = /run/p\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'passthrough_file' in [client alpha]") != NULL);
	cg_config_free(&a);

	/* A [client] section in a client's configuration is not a client. */
	CHECK_EQ(cg_config_parse(&a, CLIENT ALPHA, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'key' in [client alpha]") != NULL);
	CHECK_EQ(a.nclients, 0);
	cg_config_free(&a);
}

static void test_config_threads(void)
{
	static struct cg_config a, b;
	char err[256], warn[512];

	CHECK_EQ(cg_config_parse(&a, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(a.link_threads, CG_LT_LEGACY);
	CHECK_EQ(a.io_queue, 256);
	CHECK(!strcmp(cg_lt_name(a.link_threads), "legacy"));
	CHECK_EQ(cg_config_parse(&b, CLIENT "link_threads = off\nio_queue = 64\n", err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(b.link_threads, CG_LT_OFF);
	CHECK_EQ(b.io_queue, 64);
	CHECK(warn[0] == '\0');
	/* Set up once: a restart. */
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "link_threads"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "link_threads = legacy\nio_queue = 1024\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "io_queue"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "link_threads = maybe\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(strstr(err, "link_threads") != NULL);
	CHECK_EQ(cg_config_parse(&b, CLIENT "link_threads = on\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(b.link_threads, CG_LT_ON);
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "link_threads = auto\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(b.link_threads, CG_LT_AUTO);
	CHECK(!strcmp(cg_lt_name(b.link_threads), "auto"));
	cg_config_free(&b);
	/* [link] cpu pins that link's thread: a restart, as every pin. */
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT "[link eth1]\nlabel = A\n[link eth2]\ncpu = 3\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(warn[0] == '\0');
	CHECK_EQ(cg_config_link(&a, "eth1")->cpu, -1);
	CHECK_EQ(cg_config_link(&a, "eth2")->cpu, 3);
	CHECK_EQ(cg_config_parse(&b, CLIENT "[link eth1]\nlabel = B\n[link eth2]\ncpu = 3\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&a, &b) == NULL);
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "[link eth1]\ncpu = 0\n[link eth2]\ncpu = 3\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "cpu of a [link]"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "[link eth1]\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "cpu of a [link]")); /* eth2's pin went away */
	CHECK(!strcmp(cg_config_restart_needed(&b, &a), "cpu of a [link]"));
	cg_config_free(&b);
	CHECK_EQ(cg_config_parse(&b, CLIENT "[link eth1]\ncpu = 1024\n", err, sizeof(err), warn, sizeof(warn)), -1);
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_parse(&b, CLIENT "io_queue = 100\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(strstr(err, "power of two") != NULL);
	CHECK_EQ(cg_config_parse(&b, CLIENT "io_queue = 2048\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK_EQ(cg_config_parse(&b, CLIENT "io_queue = 32\n", err, sizeof(err), warn, sizeof(warn)), -1);
	/* Client settings: unknown to a server. */
	CHECK_EQ(cg_config_parse(&b, SERVER "link_threads = off\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'link_threads'") != NULL);
	cg_config_free(&b);
	cg_config_free(&a);
}

/* Parses text expecting an error that names key (and, if given, has more in
 * it). */
static void bad(const char *text, const char *key, const char *more)
{
	static struct cg_config c;
	char err[256], warn[512];

	CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(!strncmp(err, key, strlen(key)));
	CHECK(!more || strstr(err, more) != NULL);
}

/* Reordering and splitting (story 012): the defaults, the ranges, the mode
 * words of each end, the weight of a link, and what a reload does with them. */
static void test_config_bonding(void)
{
	static struct cg_config c, o;
	char err[256], warn[512];
	const struct cg_link_cfg *l;

	/* The defaults, in both modes; the split of a server follows its client. */
	CHECK_EQ(cg_config_parse(&c, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.reorder, 1);
	CHECK_EQ(c.reorder_max_wait_ms, 200);
	CHECK_EQ(c.reorder_dup_wait_ms, 0);
	CHECK_EQ(c.reorder_margin_ms, 10);
	CHECK_EQ(c.reorder_bypass, 128);
	CHECK_EQ(c.reorder_pool_kb, 8192);
	CHECK_EQ(c.split, CG_SPLIT_OFF);
	CHECK_EQ(c.split_small, 256);
	CHECK_EQ(c.split_spread_ms, 30);
	CHECK_EQ(c.split_spread_copy, 1);
	CHECK(c.governor == 0 && c.governor_target_ms == 20 && c.governor_window_ms == 10000);
	CHECK(c.capacity_weights == 0 && c.split_eta == 0 && c.pacing == 0);
	CHECK(c.capacity_sat_ms == c.governor_target_ms && c.rescue_target_ms == c.governor_target_ms);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.reorder, 1);
	CHECK_EQ(c.reorder_max_wait_ms, 200);
	CHECK_EQ(c.reorder_dup_wait_ms, 0);
	CHECK_EQ(c.reorder_margin_ms, 10);
	CHECK_EQ(c.reorder_bypass, 128);
	CHECK_EQ(c.reorder_pool_kb, 8192);
	CHECK_EQ(c.split, CG_SPLIT_CLIENT);
	CHECK_EQ(c.split_small, 256);
	CHECK_EQ(c.split_spread_ms, 30);
	CHECK_EQ(c.split_spread_copy, 1);
	CHECK(c.governor == 0 && c.governor_target_ms == 20 && c.governor_window_ms == 10000);
	CHECK(c.capacity_weights == 0 && c.split_eta == 0 && c.pacing == 0);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
	CHECK(!strcmp(cg_split_name(CG_SPLIT_OFF), "off") && !strcmp(cg_split_name(CG_SPLIT_ON), "on") &&
	      !strcmp(cg_split_name(CG_SPLIT_CLIENT), "client") && !strcmp(cg_split_name(CG_SPLIT_FORCE), "force"));

	/* All of them, in each mode (the same keys, no warning). */
#define KNOBS                                                                                      \
	"reorder = no\nreorder_max_wait_ms = 400\nreorder_dup_wait_ms = 20\nreorder_margin_ms = 25\n" \
	"reorder_bypass = 256\nreorder_pool_kb = 1024\nsplit_small = 512\nsplit_spread_ms = 0\n" \
	"split_spread_copy = no\ngovernor = yes\ngovernor_target_ms = 35\ngovernor_window_ms = 30000\n" \
	"capacity_weights = yes\nsplit_eta = yes\npacing = yes\ncapacity_sat_ms = 50\n"
	CHECK_EQ(cg_config_parse(&c, CLIENT KNOBS "split = force\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.capacity_sat_ms, 50);  /* its own */
	CHECK_EQ(c.rescue_target_ms, 35); /* governor_target_ms's */
	CHECK_EQ(c.reorder, 0);
	CHECK_EQ(c.reorder_max_wait_ms, 400);
	CHECK_EQ(c.reorder_dup_wait_ms, 20);
	CHECK_EQ(c.reorder_margin_ms, 25);
	CHECK_EQ(c.reorder_bypass, 256);
	CHECK_EQ(c.reorder_pool_kb, 1024);
	CHECK_EQ(c.split_small, 512);
	CHECK_EQ(c.split, CG_SPLIT_FORCE);
	CHECK_EQ(c.split_spread_ms, 0);
	CHECK_EQ(c.split_spread_copy, 0);
	CHECK(c.governor == 1 && c.governor_target_ms == 35 && c.governor_window_ms == 30000);
	CHECK(c.capacity_weights == 1 && c.split_eta == 1 && c.pacing == 1);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER KNOBS "split = off\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.reorder, 0);
	CHECK_EQ(c.reorder_max_wait_ms, 400);
	CHECK_EQ(c.reorder_dup_wait_ms, 20);
	CHECK_EQ(c.reorder_margin_ms, 25);
	CHECK_EQ(c.reorder_bypass, 256);
	CHECK_EQ(c.reorder_pool_kb, 1024);
	CHECK_EQ(c.split_small, 512);
	CHECK_EQ(c.split, CG_SPLIT_OFF);
	CHECK_EQ(c.split_spread_ms, 0);
	CHECK_EQ(c.split_spread_copy, 0);
	CHECK(c.governor == 1 && c.governor_target_ms == 35 && c.governor_window_ms == 30000);
	CHECK(c.capacity_weights == 1 && c.split_eta == 1 && c.pacing == 1);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
#undef KNOBS

	/* split_spread_copy: yes by default, no by default with governor, as
	 * set either way. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "governor = yes\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.governor == 1 && c.split_spread_copy == 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "governor = yes\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.governor == 1 && c.split_spread_copy == 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "governor = yes\nsplit_spread_copy = yes\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(c.governor == 1 && c.split_spread_copy == 1);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "governor = no\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.governor == 0 && c.split_spread_copy == 1);
	cg_config_free(&c);
	/* capacity_weights too: the copies would keep filling its links */
	CHECK_EQ(cg_config_parse(&c, SERVER "capacity_weights = yes\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.capacity_weights == 1 && c.split_spread_copy == 0);
	cg_config_free(&c);

	/* reorder and split_spread_copy are a yes or no. */
	{
		static const char *const yes[] = { "yes", "true", "on", "1" }, *const no[] = { "no", "false", "off", "0" };
		char text[160];

		for (size_t i = 0; i < CG_ARRAY_SIZE(yes); i++) {
			snprintf(text, sizeof(text), SERVER "reorder = %s\n", yes[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.reorder, 1);
			cg_config_free(&c);
			snprintf(text, sizeof(text), CLIENT "reorder = %s\n", no[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.reorder, 0);
			cg_config_free(&c);
			snprintf(text, sizeof(text), CLIENT "split_spread_copy = %s\n", no[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.split_spread_copy, 0);
			cg_config_free(&c);
			snprintf(text, sizeof(text), SERVER "split_spread_ms = 0\nsplit_spread_copy = %s\n", yes[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.split_spread_copy, 1);
			cg_config_free(&c);
		}
		bad(CLIENT "reorder = maybe\n", "reorder", "yes or no");
		bad(SERVER "reorder = \n", "reorder", "yes or no");
		bad(SERVER "split_spread_copy = copy\n", "split_spread_copy", "yes or no");
		bad(CLIENT "governor = maybe\n", "governor", "yes or no");
		bad(SERVER "capacity_weights = maybe\n", "capacity_weights", "yes or no");
		bad(CLIENT "split_eta = maybe\n", "split_eta", "yes or no");
		bad(CLIENT "pacing = maybe\n", "pacing", "yes or no");
	}

	/* The ranges, edge by edge, in a client and a server alike. */
	{
		static const struct {
			const char *key;
			uint32_t min, max;
			size_t off;
		} r[] = {
			{ "reorder_max_wait_ms", 0, 2000, offsetof(struct cg_config, reorder_max_wait_ms) },
			{ "reorder_margin_ms", 0, 1000, offsetof(struct cg_config, reorder_margin_ms) },
			{ "reorder_bypass", 0, 2048, offsetof(struct cg_config, reorder_bypass) },
			{ "reorder_pool_kb", 256, 131070, offsetof(struct cg_config, reorder_pool_kb) },
			{ "split_small", 0, 2048, offsetof(struct cg_config, split_small) },
			{ "split_spread_ms", 0, 2000, offsetof(struct cg_config, split_spread_ms) },
			{ "governor_target_ms", 1, 1000, offsetof(struct cg_config, governor_target_ms) },
			{ "governor_window_ms", 1000, 120000, offsetof(struct cg_config, governor_window_ms) },
		};
		char text[160], want[64];

		for (size_t i = 0; i < CG_ARRAY_SIZE(r); i++) {
			const char *base[2] = { CLIENT, SERVER };

			for (int m = 0; m < 2; m++) {
				for (int e = 0; e < 2; e++) {
					uint32_t v = e ? r[i].max : r[i].min;

					snprintf(text, sizeof(text), "%s%s = %u\n", base[m], r[i].key, v);
					CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
					CHECK_EQ(*(uint32_t *)((char *)&c + r[i].off), v);
					CHECK(warn[0] == '\0');
					cg_config_free(&c);
				}
				snprintf(want, sizeof(want), "between %u and %u, got", r[i].min, r[i].max);
				snprintf(text, sizeof(text), "%s%s = %u\n", base[m], r[i].key, r[i].max + 1);
				bad(text, r[i].key, want);
				if (r[i].min) {
					snprintf(text, sizeof(text), "%s%s = %u\n", base[m], r[i].key, r[i].min - 1);
					bad(text, r[i].key, want);
				}
				snprintf(text, sizeof(text), "%s%s = -1\n", base[m], r[i].key);
				bad(text, r[i].key, "got '-1'");
				snprintf(text, sizeof(text), "%s%s = fast\n", base[m], r[i].key);
				bad(text, r[i].key, "got 'fast'");
				snprintf(text, sizeof(text), "%s%s = 5ms\n", base[m], r[i].key);
				bad(text, r[i].key, "got '5ms'");
			}
		}
	}

	/* reorder_dup_wait_ms goes up to reorder_max_wait_ms, wherever that is
	 * set in the file, and no further. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "reorder_dup_wait_ms = 200\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.reorder_dup_wait_ms, 200); /* the default maximum, 200 */
	cg_config_free(&c);
	bad(CLIENT "reorder_dup_wait_ms = 201\n", "reorder_dup_wait_ms", "between 0 and 200, got '201'");
	CHECK_EQ(cg_config_parse(&c, SERVER "reorder_dup_wait_ms = 50\nreorder_max_wait_ms = 100\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.reorder_dup_wait_ms, 50);
	CHECK_EQ(c.reorder_max_wait_ms, 100);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "reorder_max_wait_ms = 100\nreorder_dup_wait_ms = 100\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.reorder_dup_wait_ms, 100);
	cg_config_free(&c);
	bad(SERVER "reorder_max_wait_ms = 100\nreorder_dup_wait_ms = 101\n", "reorder_dup_wait_ms",
	    "between 0 and 100, got '101'");
	bad(CLIENT "reorder_dup_wait_ms = 101\nreorder_max_wait_ms = 100\n", "reorder_dup_wait_ms", "got '101'");
	/* No wait at all: the packets behind a gap never wait. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "reorder_max_wait_ms = 0\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.reorder_max_wait_ms == 0 && c.reorder_dup_wait_ms == 0);
	cg_config_free(&c);
	bad(CLIENT "reorder_max_wait_ms = 0\nreorder_dup_wait_ms = 1\n", "reorder_dup_wait_ms", "between 0 and 0");

	/* The mode words depend on the end: a client has on, a server client. */
	{
		static const char *const cw[] = { "off", "on", "force" }, *const sw[] = { "off", "client", "force" };
		static const int cv[] = { CG_SPLIT_OFF, CG_SPLIT_ON, CG_SPLIT_FORCE };
		static const int sv[] = { CG_SPLIT_OFF, CG_SPLIT_CLIENT, CG_SPLIT_FORCE };
		char text[160];

		for (size_t i = 0; i < 3; i++) {
			snprintf(text, sizeof(text), CLIENT "split = %s\n", cw[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.split, cv[i]);
			CHECK(warn[0] == '\0');
			cg_config_free(&c);
			snprintf(text, sizeof(text), SERVER "split = %s\n", sw[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.split, sv[i]);
			CHECK(warn[0] == '\0');
			cg_config_free(&c);
		}
		bad(CLIENT "split = client\n", "split", "expected off, on or force, got 'client'");
		bad(SERVER "split = on\n", "split", "expected off, client or force, got 'on'");
		/* Not a yes or no, not a case away, not empty. */
		bad(CLIENT "split = yes\n", "split", "expected off, on or force, got 'yes'");
		bad(SERVER "split = no\n", "split", "expected off, client or force, got 'no'");
		bad(CLIENT "split = ON\n", "split", "got 'ON'");
		bad(SERVER "split = Force\n", "split", "got 'Force'");
		bad(CLIENT "split = \n", "split", "got ''");
		bad(SERVER "split = 1\n", "split", "got '1'");
	}

	/* weight, per link: 1 by default, 0 to 1000; an interface with no
	 * section has none to read. */
	CHECK_EQ(cg_config_parse(&c,
				 CLIENT "[link eth1]\nlabel = A\n[link eth2]\nweight = 3\n[link eth3]\nweight = 0\n"
					"[link eth4]\nweight = 1000\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(warn[0] == '\0');
	CHECK_EQ(c.nlinks, 4);
	l = cg_config_link(&c, "eth1");
	CHECK(l && l->weight == 1);
	l = cg_config_link(&c, "eth2");
	CHECK(l && l->weight == 3);
	l = cg_config_link(&c, "eth3");
	CHECK(l && l->weight == 0);
	l = cg_config_link(&c, "eth4");
	CHECK(l && l->weight == 1000);
	CHECK(cg_config_link(&c, "eth5") == NULL);
	cg_config_free(&c);
	bad(CLIENT "[link eth1]\nweight = 1001\n", "weight", "between 0 and 1000, got '1001'");
	bad(CLIENT "[link eth1]\nweight = -1\n", "weight", "got '-1'");
	bad(CLIENT "[link eth1]\nweight = heavy\n", "weight", "got 'heavy'");
	/* type, per link (linkmodel.h, story 015): auto by default, one of the
	 * names, anything else refused with the list; a server has none (its
	 * client's probes say them). */
	CHECK_EQ(cg_config_parse(&c,
				 CLIENT "[link eth1]\nlabel = A\n[link eth2]\ntype = starlink\n[link eth3]\ntype = cellular\n"
					"[link eth4]\ntype = auto\n[link eth5]\ntype = radio\n[link eth6]\ntype = fiber\n"
					"[link eth7]\ntype = ethernet\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(warn[0] == '\0');
	CHECK(cg_config_link(&c, "eth1")->type == CG_LTYPE_AUTO);
	CHECK(cg_config_link(&c, "eth2")->type == CG_LTYPE_STARLINK);
	CHECK(cg_config_link(&c, "eth3")->type == CG_LTYPE_CELLULAR);
	CHECK(cg_config_link(&c, "eth4")->type == CG_LTYPE_AUTO);
	CHECK(cg_config_link(&c, "eth5")->type == CG_LTYPE_RADIO);
	CHECK(cg_config_link(&c, "eth6")->type == CG_LTYPE_FIBER);
	CHECK(cg_config_link(&c, "eth7")->type == CG_LTYPE_ETHERNET);
	cg_config_free(&c);
	bad(CLIENT "[link eth1]\ntype = satellite\n", "type",
	    "expected auto, cellular, starlink, fiber, ethernet or radio, got 'satellite'");
	bad(CLIENT "[link eth1]\ntype = Starlink\n", "type", "got 'Starlink'");
	bad(CLIENT "[link eth1]\ntype = \n", "type", "got ''");
	CHECK_EQ(cg_config_parse(&c, CLIENT "type = starlink\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'type'") != NULL);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "[link eth1]\ntype = starlink\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'type' in [link eth1]") != NULL);
	cg_config_free(&c);
	/* exp_link_types: the types the global experiment knobs reach, on
	 * either end; none by default (every link) */
	CHECK_EQ(cg_config_parse(&c, SERVER "split_qpred_ms = 300\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(c.exp_link_types == 0 && c.split_qpred_ms == 300 && c.redund_pace_ms == 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "exp_link_types = starlink, cellular\nredund_pace_ms = 30\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(warn[0] == '\0');
	CHECK_EQ(c.exp_link_types, (1u << CG_LTYPE_STARLINK) | (1u << CG_LTYPE_CELLULAR));
	CHECK_EQ(c.redund_pace_ms, 30);
	CHECK_EQ(cg_config_link_knobs(&c).types, c.exp_link_types);
	CHECK_EQ(cg_config_link_knobs(&c).pace_ms, 30);
	cg_config_free(&c);
	bad(CLIENT "exp_link_types = starlink satellite\n", "exp_link_types", "got 'starlink satellite'");
	/* split_dominant_pct: 0 (the preset's) by default, any end */
	CHECK_EQ(cg_config_parse(&c, SERVER "\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_link_knobs(&c).dominant_pct, 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "split_dominant_pct = 400\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(warn[0] == '\0');
	CHECK_EQ(cg_config_link_knobs(&c).dominant_pct, 400);
	cg_config_free(&c);
	bad(SERVER "split_dominant_pct = 70000\n", "split_dominant_pct", "70000");
	/* the Starlink gears (task #63-#65): seq lag on, the slot clock at
	 * :12 +- 100/50 ms, hold and edge copies off, by default */
	CHECK_EQ(cg_config_parse(&c, SERVER "\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.capacity_seq_lag, 1);
	CHECK_EQ(c.starlink_edge_ms, 12000);
	CHECK_EQ(c.starlink_edge_pre_ms, 100);
	CHECK_EQ(c.starlink_edge_post_ms, 50);
	CHECK_EQ(c.starlink_edge_copies, 0);
	CHECK_EQ(cg_config_link_knobs(&c).dominant_hold, 0);
	CHECK_EQ(cg_config_link_knobs(&c).slot_clock, 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "capacity_seq_lag = no\nstarlink_slot_clock = 2\nsplit_dominant_hold = 1\n"
				      "starlink_edge_ms = 11950\nstarlink_edge_copies = yes\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.capacity_seq_lag, 0);
	CHECK_EQ(c.starlink_edge_ms, 11950);
	CHECK_EQ(c.starlink_edge_copies, 1);
	CHECK_EQ(cg_config_link_knobs(&c).slot_clock, 2);
	CHECK_EQ(cg_config_link_knobs(&c).dominant_hold, 1);
	cg_config_free(&c);
	bad(SERVER "starlink_edge_ms = 15000\n", "starlink_edge_ms", "15000");
	bad(CLIENT "split_dominant_hold = 3\n", "split_dominant_hold", "3");
	/* The weight is a link's, not global; a server has no [link] weights (its
	 * client's probes say them), and says so. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "weight = 2\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'weight'") != NULL);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "[link eth1]\nweight = 2\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'weight' in [link eth1]") != NULL);
	cg_config_free(&c);

	/* A reload: only the pool is set up once (a restart); everything else
	 * applies in place, in either mode. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "[link eth1]\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_parse(&o,
				 CLIENT "reorder = no\nreorder_max_wait_ms = 300\nreorder_dup_wait_ms = 10\n"
					"reorder_margin_ms = 40\nreorder_bypass = 0\nsplit = force\nsplit_small = 64\n"
					"split_spread_ms = 100\nsplit_spread_copy = no\n[link eth1]\nweight = 7\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&c, &o) == NULL);
	CHECK(cg_config_restart_needed(&o, &c) == NULL);
	cg_config_free(&o);
	CHECK_EQ(cg_config_parse(&o, CLIENT "reorder_pool_kb = 4096\n[link eth1]\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&c, &o) && !strcmp(cg_config_restart_needed(&c, &o), "reorder_pool_kb"));
	CHECK(!strcmp(cg_config_restart_needed(&o, &c), "reorder_pool_kb"));
	cg_config_free(&o);
	CHECK_EQ(cg_config_parse(&o, CLIENT "reorder_pool_kb = 8192\n[link eth1]\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&c, &o) == NULL); /* the same as the default */
	cg_config_free(&o);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_parse(&o, SERVER "reorder = no\nsplit = force\nreorder_max_wait_ms = 50\nsplit_small = 0\n"
					   "split_spread_ms = 0\nsplit_spread_copy = no\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&c, &o) == NULL);
	cg_config_free(&o);
	CHECK_EQ(cg_config_parse(&o, SERVER "reorder_pool_kb = 256\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_config_restart_needed(&c, &o), "reorder_pool_kb"));
	cg_config_free(&o);
	cg_config_free(&c);

	/* A configuration from before these keys loads as it did, and says
	 * nothing about them. */
	CHECK_EQ(cg_config_parse(&c, client_conf, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "reorder") == NULL && strstr(warn, "split") == NULL && strstr(warn, "weight") == NULL);
	CHECK_EQ(c.reorder, 1);
	CHECK_EQ(c.split, CG_SPLIT_OFF);
	CHECK_EQ(cg_config_link(&c, "eth1.10")->weight, 1);
	CHECK_EQ(cg_config_link(&c, "eth1.20")->weight, 1);
	cg_config_free(&c);
}

/* Address lists: a single listen and wireguard address, servers to send to,
 * IPv4-mapped addresses as IPv4. */
static void test_config_addrs(void)
{
	static const char *const bad[] = {
		"*:59402", "0.0.0.0:59402", "[::]:59402", "[::ffff:0.0.0.0]:59402", "224.0.0.1:59402",
		"[ff02::1]:59402", "[::ffff:239.1.2.3]:59402",
	};
	static struct cg_config c, d;
	char err[256], warn[512], text[256];

	/* Extra addresses were dropped in silence. */
	CHECK_EQ(cg_config_parse(&c, SERVER "listen = 192.0.2.1:59402 198.51.100.1:59402\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK(strstr(err, "listen: expected a single address") != NULL);
	CHECK_EQ(cg_config_parse(&c, CLIENT "listen = 127.0.0.1:1, 127.0.0.1:2\n", err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "listen") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820 127.0.0.1:51821\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "wireguard: expected a single address") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 192.0.2.1:1 192.0.2.2:1 192.0.2.3:1 "
				 "192.0.2.4:1 192.0.2.5:1 192.0.2.6:1 192.0.2.7:1 192.0.2.8:1 192.0.2.9:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "server: at most 8 addresses") != NULL);
	CHECK_EQ(cg_config_parse(&c, CLIENT "[link eth1]\nserver = 192.0.2.1:1 192.0.2.2:1 192.0.2.3:1 192.0.2.4:1 "
				 "[2001:db8::5]:1 192.0.2.6:1 192.0.2.7:1 192.0.2.8:1 192.0.2.9:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "server: at most 8 addresses") != NULL);
	/* An ordered list of up to CG_MAX_SERVERS, global or per link, IPv4
	 * and IPv6 mixed. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "[link eth1]\nserver = 192.0.2.1:1 192.0.2.2:1 192.0.2.3:1 192.0.2.4:1 "
				 "[2001:db8::5]:1 192.0.2.6:1 192.0.2.7:1 192.0.2.8:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.links[0].nserver, CG_MAX_SERVERS);
	CHECK(c.links[0].nentry == CG_MAX_SERVERS && c.links[0].entry_n[0] == 1 && c.links[0].entry_n[7] == 1);
	CHECK(!strcmp(cg_addr_str(&c.links[0].server[4], text, sizeof(text)), "[2001:db8::5]:1"));
	CHECK(!strcmp(cg_addr_str(&c.links[0].server[7], text, sizeof(text)), "192.0.2.8:1"));
	CHECK_EQ(c.nserver, 1);
	cg_config_free(&c);
	/* A name never pushes a later entry out unchecked or unused: localhost
	 * has two addresses wherever /etc/hosts also lists ::1. */
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 192.0.2.1:1 192.0.2.2:1 192.0.2.3:1 "
				 "224.0.0.1:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '224.0.0.1:1' is a multicast address"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = localhost:1 192.0.2.2:1 192.0.2.3:1 "
				 "224.0.0.1:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '224.0.0.1:1' is a multicast address"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = localhost:1 192.0.2.2:1 192.0.2.3:1 "
				 "0.0.0.0:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '0.0.0.0:1' is the wildcard address"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = localhost:1 192.0.2.2:1 192.0.2.3:1 "
				 "192.0.2.4:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(c.nserver >= 4 && c.nserver <= 5); /* localhost: 127.0.0.1, maybe ::1 */
	CHECK(!strcmp(cg_addr_str(&c.server[c.nserver - 1], text, sizeof(text)), "192.0.2.4:1"));
	/* Which addresses each entry gave, for a reload (srvpick.h). */
	CHECK_EQ(c.nentry, 4);
	CHECK(c.entry_n[0] == c.nserver - 3 && c.entry_n[1] == 1 && c.entry_n[3] == 1);
	cg_config_free(&c);

	/* A server is somewhere to send to: no wildcard, no multicast. */
	for (size_t i = 0; i < CG_ARRAY_SIZE(bad); i++) {
		snprintf(text, sizeof(text), "mode = client\nkey = " KEY "\nserver = %s\n", bad[i]);
		CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), -1);
		CHECK(!strncmp(err, "server: ", 8) && strstr(err, bad[i]) != NULL);
		snprintf(text, sizeof(text), CLIENT "[link eth1]\nserver = 198.51.100.7:1 %s\n", bad[i]);
		CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), -1);
		CHECK(strstr(err, bad[i]) != NULL);
	}
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 0.0.0.0:59402\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '0.0.0.0:59402' is the wildcard address"));
	/* The wildcard is still fine where it means "any local address". */
	CHECK_EQ(cg_config_parse(&c, SERVER "listen = 0.0.0.0:59402\n", err, sizeof(err), warn, sizeof(warn)), 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "listen = [::]:59401\n", err, sizeof(err), warn, sizeof(warn)), 0);
	cg_config_free(&c);

	/* IPv4-mapped addresses become IPv4, so they leave through IPv4. */
	CHECK_EQ(cg_config_parse(&c,
				 "mode = client\nkey = " KEY "\nserver = [::ffff:203.0.113.10]:59402 [2001:db8::1]:59402\n"
				 "listen = [::ffff:127.0.0.1]:59401\n[link eth1]\nserver = [::ffff:198.51.100.7]:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.nserver, 2);
	CHECK_EQ(c.server[0].ss_family, AF_INET);
	CHECK_EQ(c.server[1].ss_family, AF_INET6);
	CHECK_EQ(c.listen.ss_family, AF_INET);
	CHECK_EQ(c.links[0].server[0].ss_family, AF_INET);
	CHECK(!strcmp(cg_addr_str(&c.server[0], text, sizeof(text)), "203.0.113.10:59402"));
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\nwireguard = [::ffff:127.0.0.1]:51820\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.wireguard.ss_family, AF_INET);
	CHECK_EQ(cg_config_parse(&d, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(cg_config_restart_needed(&c, &d) == NULL); /* the same address */
	cg_config_free(&c);
	cg_config_free(&d);

	/* server_failover_ms: 10 s, or 3 idle probes when those take longer;
	 * 0 never moves; less than 3 x probe_idle_ms is refused. */
	CHECK_EQ(cg_config_parse(&c, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.server_failover_ms, 10000);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "probe_idle_ms = 5000\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.server_failover_ms, 15000);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "server_failover_ms = 0\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.server_failover_ms, 0);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "server_failover_ms = 3000\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.server_failover_ms, 3000);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "server_failover_ms = 2999\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(strstr(err, "server_failover_ms") != NULL && strstr(err, "3000") != NULL);
	CHECK_EQ(cg_config_parse(&c, CLIENT "probe_idle_ms = 2000\nserver_failover_ms = 5000\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK_EQ(cg_config_parse(&c, CLIENT "server_failover_ms = 3600001\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK_EQ(cg_config_parse(&c, SERVER "server_failover_ms = 5000\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'server_failover_ms'") != NULL); /* a client setting */
	cg_config_free(&c);
	/* Applied in place, like the lists themselves. */
	CHECK_EQ(cg_config_parse(&c, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(cg_config_parse(&d, "mode = client\nkey = " KEY "\nserver = 192.0.2.9:1 [2001:db8::9]:1\n"
				 "server_failover_ms = 0\n[link eth1]\nserver = 192.0.2.8:1 192.0.2.7:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(cg_config_restart_needed(&c, &d) == NULL);
	cg_config_free(&c);
	cg_config_free(&d);
}

/* Names in the server lists: kept for the client to look up, never
 * resolved here, unless the hosts file lists them; and the resolvers. */
static void test_config_names(void)
{
	static struct cg_config c;
	char err[256], warn[512], buf[64], path[] = "/tmp/cengarde-hosts-XXXXXX";
	const char *saved = cg_hosts_path;
	struct cg_nameaddrs got;
	const struct cg_nameaddrs *names[CG_MAX_NAMES] = { NULL };
	FILE *f;
	int fd;

	/* A name gives no address until it is looked up; the list keeps its
	 * place and its port. */
	cg_hosts_path = "/nonexistent/hosts";
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = VPS.Example.com.:59402 192.0.2.2:7\n"
				 "[link eth1]\nserver = vps.example.com:1 home.example.net:2\n[link eth2]\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.nnames, 2);
	CHECK(!strcmp(c.names[0].name, "vps.example.com") && !c.names[0].hosts);
	CHECK(!strcmp(c.names[1].name, "home.example.net"));
	CHECK(c.nsrv == 2 && c.srv[0].name == 0 && c.srv[0].port == 59402 && c.srv[1].name == -1);
	CHECK(c.nserver == 1 && c.nentry == 2 && c.entry_n[0] == 0 && c.entry_n[1] == 1);
	CHECK(!strcmp(cg_addr_str(&c.server[0], buf, sizeof(buf)), "192.0.2.2:7"));
	CHECK(c.links[0].nsrv == 2 && c.links[0].nserver == 0 && c.links[0].srv[1].name == 1);
	CHECK_EQ(c.links[1].nsrv, 0); /* the global list */
	/* Built again with what the names have now. */
	memset(&got, 0, sizeof(got));
	got.a[got.n++] = (struct sockaddr_storage){ .ss_family = AF_INET };
	((struct sockaddr_in *)&got.a[0])->sin_addr.s_addr = htonl(0xcb007105); /* 203.0.113.5 */
	names[0] = &got;
	cg_config_build(&c, names);
	CHECK(c.nserver == 2 && c.entry_n[0] == 1 && c.entry_n[1] == 1);
	CHECK(!strcmp(cg_addr_str(&c.server[0], buf, sizeof(buf)), "203.0.113.5:59402"));
	CHECK(c.links[0].nserver == 1 && c.links[0].entry_n[0] == 1 && c.links[0].entry_n[1] == 0);
	CHECK(!strcmp(cg_addr_str(&c.links[0].server[0], buf, sizeof(buf)), "203.0.113.5:1"));
	/* Resolvers: the default ones, and a link's own. */
	CHECK_EQ(c.nresolver, 4);
	CHECK(!strcmp(cg_addr_str(&c.resolver[0], buf, sizeof(buf)), "1.1.1.1:53"));
	CHECK(!strcmp(cg_addr_str(&c.resolver[2], buf, sizeof(buf)), "[2606:4700:4700::1111]:53"));
	CHECK_EQ(c.links[0].nresolver, 0);
	cg_config_free(&c);

	/* Only names is fine: they come later. */
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = vps.example.com:1\nresolver =\n"
				 "[link eth1]\nresolver = 192.168.8.1 [2001:db8::53]:5353 2001:db8::1 [2001:db8::2]\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(c.nserver == 0 && c.nsrv == 1 && c.nresolver == 0);
	CHECK_EQ(c.links[0].nresolver, 4);
	CHECK(!strcmp(cg_addr_str(&c.links[0].resolver[0], buf, sizeof(buf)), "192.168.8.1:53"));
	CHECK(!strcmp(cg_addr_str(&c.links[0].resolver[1], buf, sizeof(buf)), "[2001:db8::53]:5353"));
	CHECK(!strcmp(cg_addr_str(&c.links[0].resolver[2], buf, sizeof(buf)), "[2001:db8::1]:53"));
	CHECK(!strcmp(cg_addr_str(&c.links[0].resolver[3], buf, sizeof(buf)), "[2001:db8::2]:53"));
	CHECK(!*warn);
	cg_config_free(&c);

	/* Mistakes. */
	CHECK_EQ(cg_config_parse(&c, CLIENT "resolver = 224.0.0.1\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(!strcmp(err, "resolver: '224.0.0.1' is a multicast address"));
	CHECK_EQ(cg_config_parse(&c, CLIENT "resolver = dns.google\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(!strcmp(err, "resolver: 'dns.google' is not an address"));
	CHECK_EQ(cg_config_parse(&c, CLIENT "resolver = 1.1.1.1 1.1.1.2 1.1.1.3 1.1.1.4 1.1.1.5 1.1.1.6 1.1.1.7 "
				 "1.1.1.8 1.1.1.9\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "resolver: at most 8 addresses"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 10.0.0.256:1\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '10.0.0.256:1' is neither an address nor a name"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = *:59402\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: '*:59402' is the wildcard address"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = [vps.example.com]:1\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "neither an address nor a name") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = bad..name:1\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = a.x:1 b.x:1 c.x:1 d.x:1 e.x:1\n"
				 "[link eth1]\nserver = f.x:1 g.x:1 h.x:1 a.x:1 i.x:1\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: at most 8 names in all the server lists"));
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver =\n", err, sizeof(err), warn,
				 sizeof(warn)),
		 -1);
	CHECK(!strcmp(err, "server: no address given"));
	/* The server takes no resolver: an unknown key there. */
	CHECK_EQ(cg_config_parse(&c, SERVER "resolver = 1.1.1.1\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'resolver'") != NULL);
	cg_config_free(&c);

	/* The hosts file goes first: its addresses, never looked up. */
	fd = mkstemp(path);
	CHECK(fd >= 0);
	f = fdopen(fd, "w");
	fputs("192.0.2.77 vps.example.com vps\n2001:db8::77 vps\n", f);
	fclose(f);
	cg_hosts_path = path;
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = vps:9 other.example.com:9\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK(c.nnames == 2 && c.names[0].hosts && c.names[0].addrs.n == 2 && !c.names[1].hosts);
	CHECK(c.nserver == 2 && c.entry_n[0] == 2 && c.entry_n[1] == 0);
	CHECK(!strcmp(cg_addr_str(&c.server[0], buf, sizeof(buf)), "[2001:db8::77]:9"));
	CHECK(!strcmp(cg_addr_str(&c.server[1], buf, sizeof(buf)), "192.0.2.77:9"));
	cg_config_free(&c);
	unlink(path);
	cg_hosts_path = saved;
}

/* tunnel = native: the TUN device, the server's routes, and what a reload
 * cannot change in place. */
static void test_config_native(void)
{
	static struct cg_config c, o;
	char err[256], warn[512], buf[64];

	/* The default: WireGuard, nothing about a TUN needed. */
	CHECK_EQ(cg_config_parse(&c, CLIENT, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.tunnel, CG_TUNNEL_WIREGUARD);
	CHECK(!strcmp(c.tun, "cg0"));
	CHECK_EQ(c.tun_mtu, CG_TUN_MTU_DEFAULT);
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, CLIENT "tun_txqueuelen = 5000\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.tun_txqueuelen, 5000);
	cg_config_free(&c);
	CHECK(cg_config_parse(&c, CLIENT "tun_txqueuelen = 200000\n", err, sizeof(err), warn, sizeof(warn)) < 0);

#define NATIVE CLIENT "tunnel = native\ntun = cgt\ntun_addr = 10.79.0.2/16 fd00:79::2/64\ntun_mtu = 1400\n"
	CHECK_EQ(cg_config_parse(&c, NATIVE, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.tunnel, CG_TUNNEL_NATIVE);
	CHECK(!strcmp(c.tun, "cgt"));
	CHECK_EQ(c.ntun_addr, 2);
	CHECK(!strcmp(cg_pfx_str(&c.tun_addr[0], buf, sizeof(buf)), "10.79.0.2/16"));
	CHECK(!strcmp(cg_pfx_str(&c.tun_addr[1], buf, sizeof(buf)), "fd00:79::2/64"));
	CHECK_EQ(c.tun_mtu, 1400);
	CHECK(!warn[0]);
	/* every TUN setting restarts */
	CHECK_EQ(cg_config_parse(&o, NATIVE, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(cg_config_restart_needed(&c, &o) == NULL);
	o.tun_mtu = 1300;
	CHECK(!strcmp(cg_config_restart_needed(&c, &o), "tun_mtu"));
	o.tun_mtu = c.tun_mtu;
	CHECK_EQ(c.tun_txqueuelen, 0); /* the kernel's */
	o.tun_txqueuelen = 5000;
	CHECK(!strcmp(cg_config_restart_needed(&c, &o), "tun_txqueuelen"));
	o.tun_txqueuelen = 0;
	o.tun_mtu = 1400;
	o.ntun_addr = 1;
	CHECK(!strcmp(cg_config_restart_needed(&c, &o), "tun_addr"));
	o.tunnel = CG_TUNNEL_WIREGUARD;
	CHECK(!strcmp(cg_config_restart_needed(&c, &o), "tunnel"));
	cg_config_free(&c);
	cg_config_free(&o);
#undef NATIVE

	bad(CLIENT "tunnel = tun\n", "tunnel", "wireguard or native");
	bad(CLIENT "tunnel = native\ntun_mtu = 3000\n", "tun_mtu", NULL);
	bad(CLIENT "tunnel = native\ntun_addr = 10.79.0.2/40\n", "tun_addr", NULL);
	bad(CLIENT "tunnel = native\ntun = a/b\n", "tun", NULL);

	/* A server with one client needs no WireGuard, and takes everything. */
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\ntunnel = native\ntun_addr = 10.79.0.1/16\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.ntun_route, 2);
	CHECK(!strcmp(cg_pfx_str(&c.tun_route[0], buf, sizeof(buf)), "0.0.0.0/0"));
	CHECK(!strcmp(cg_pfx_str(&c.tun_route[1], buf, sizeof(buf)), "::/0"));
	CHECK_EQ(c.wireguard_poke.ss_family, AF_UNSPEC); /* no WireGuard to poke */
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\ntunnel = native\ntun_route = 10.79.0.2\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.ntun_route, 1);
	CHECK_EQ(c.tun_route[0].len, 32);
	cg_config_free(&c);
	bad("mode = server\nkey = " KEY "\ntunnel = native\ntun_route = 10.79.0.1/16\n", "tun_route", "a prefix");
	/* WireGuard still required without native */
	bad("mode = server\nkey = " KEY "\n", "wireguard", NULL);

	/* Several clients: each its own routes, never the same prefix. */
#define TWO(ra, rb)                                                                                     \
	"mode = server\ntunnel = native\n[client alpha]\nkey = " KEY "\ntun_route = " ra "\n[client bravo]\n" \
	"key = AQECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\ntun_route = " rb "\n"
	CHECK_EQ(cg_config_parse(&c, TWO("10.79.0.2/32 192.168.1.0/24", "10.79.0.3/32 fd12::/64"), err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.nclients, 2);
	CHECK_EQ(c.clients[0].ntun_route, 2);
	CHECK_EQ(c.clients[1].ntun_route, 2);
	CHECK_EQ(c.clients[1].tun_route[1].family, AF_INET6);
	cg_config_free(&c);
	bad(TWO("10.79.0.2/32 192.168.1.0/24", "10.79.0.3/32 192.168.1.0/24"), "tun_route",
	    "192.168.1.0/24 is in [client");
	bad("mode = server\ntunnel = native\n[client alpha]\nkey = " KEY "\n", "[client alpha] tun_route", "required");
	bad("mode = server\ntunnel = native\ntun_route = 0.0.0.0/0\n[client alpha]\nkey = " KEY "\ntun_route = 10.79.0.2\n",
	    "tun_route", "in each");
#undef TWO
}

void test_config(void)
{
	static struct cg_config c;
	char err[256], warn[512], buf[64];
	const struct cg_link_cfg *l;

	CHECK_EQ(cg_config_parse(&c, client_conf, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.mode, CG_MODE_CLIENT);
	CHECK_EQ(c.key[0], 0);
	CHECK_EQ(c.key[31], 31);
	CHECK(!strcmp(c.description, "Pi #1 (rooftop)"));
	CHECK_EQ(c.nserver, 2);
	CHECK(!strcmp(cg_addr_str(&c.server[0], buf, sizeof(buf)), "203.0.113.10:59402"));
	CHECK(!strcmp(cg_addr_str(&c.server[1], buf, sizeof(buf)), "[2001:db8::1]:59402"));
	CHECK(!strcmp(cg_addr_str(&c.listen, buf, sizeof(buf)), "127.0.0.1:59401"));
	CHECK_EQ(c.probe_interval_ms, 500);
	CHECK_EQ(c.probe_idle_ms, 1000);
	CHECK_EQ(c.mute_behind_ms, 150); /* link health defaults */
	CHECK_EQ(c.unmute_behind_ms, 120);
	CHECK_EQ(c.mute_settle_ms, 2000);
	CHECK_EQ(c.mute_trickle, 0);
	CHECK_EQ(c.min_active_links, 2);
	CHECK_EQ(c.busy_poll_us, 0);
	CHECK_EQ(c.cpu, -1);
	CHECK_EQ(c.rt_priority, 0);
	CHECK_EQ(c.ninclude, 2);
	CHECK(cg_match_any("eth1.10", c.include, c.ninclude));
	CHECK(!cg_match_any("eth0", c.include, c.ninclude));
	CHECK(cg_match_any("eth1.99", c.exclude, c.nexclude));
	CHECK(cg_match_any("wg0", c.exclude, c.nexclude)); /* built-in exclusions */
	CHECK(cg_match_any("lo", c.exclude, c.nexclude));
	CHECK_EQ(c.nlinks, 2);
	l = cg_config_link(&c, "eth1.10");
	CHECK(l && !strcmp(l->label, "WOM") && l->enabled && l->nserver == 0);
	l = cg_config_link(&c, "eth1.20");
	CHECK(l && !strcmp(l->label, "Entel") && !l->enabled && l->nserver == 1);
	CHECK(strstr(warn, "unknown key 'colour' in [link eth1.20]") != NULL);
	cg_config_free(&c);

	/* Server mode, defaults. */
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.mode, CG_MODE_SERVER);
	CHECK(!strcmp(cg_addr_str(&c.listen, buf, sizeof(buf)), "[::]:59402"));
	CHECK(!strcmp(cg_addr_str(&c.wireguard, buf, sizeof(buf)), "127.0.0.1:51820"));
	CHECK_EQ(c.max_sessions, 64);
	CHECK_EQ(c.lanes, 8);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);

	/* lanes: auto (8) or a power of two up to 16. */
	{
		static const char *const ok[] = { "auto", "1", "2", "4", "8", "16" };
		static const unsigned want[] = { 8, 1, 2, 4, 8, 16 };
		static const char *const bad[] = { "0", "3", "6", "32", "-8", "eight", "4x", "4294967300" };
		char text[160];

		for (size_t i = 0; i < CG_ARRAY_SIZE(ok); i++) {
			snprintf(text, sizeof(text), SERVER "lanes = %s\n", ok[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), 0);
			CHECK_EQ(c.lanes, want[i]);
			cg_config_free(&c);
		}
		for (size_t i = 0; i < CG_ARRAY_SIZE(bad); i++) {
			snprintf(text, sizeof(text), SERVER "lanes = %s\n", bad[i]);
			CHECK_EQ(cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)), -1);
			CHECK(!strncmp(err, "lanes: expected auto, 1, 2, 4, 8 or 16", 38));
		}
		CHECK_EQ(cg_config_parse(&c, CLIENT "lanes = 4\n", err, sizeof(err), warn, sizeof(warn)), 0);
		CHECK(strstr(warn, "unknown key 'lanes'") != NULL); /* a server setting */
		cg_config_free(&c);
	}

	/* wireguard_poke: the router's end of the tunnel by default, an address,
	 * or none; never the wildcard. A reload applies it. */
	CHECK_EQ(cg_config_parse(&c, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_addr_str(&c.wireguard_poke, buf, sizeof(buf)), "10.79.0.2:9"));
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "wireguard_poke = 10.80.0.6:7\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(!strcmp(cg_addr_str(&c.wireguard_poke, buf, sizeof(buf)), "10.80.0.6:7"));
	CHECK(warn[0] == '\0');
	{
		static struct cg_config o;

		CHECK_EQ(cg_config_parse(&o, SERVER, err, sizeof(err), warn, sizeof(warn)), 0);
		CHECK(cg_config_restart_needed(&o, &c) == NULL);
		cg_config_free(&o);
	}
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "wireguard_poke = none\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK_EQ(c.wireguard_poke.ss_family, AF_UNSPEC);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c, SERVER "wireguard_poke = *:9\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(!strncmp(err, "wireguard_poke: ", 16));
	CHECK_EQ(cg_config_parse(&c, CLIENT "wireguard_poke = 10.79.0.2:9\n", err, sizeof(err), warn, sizeof(warn)), 0);
	CHECK(strstr(warn, "unknown key 'wireguard_poke'") != NULL);
	cg_config_free(&c);

	/* Link health and latency knobs, the same keys in both modes. */
	CHECK_EQ(cg_config_parse(&c,
				 "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\nmute_behind_ms = 300\n"
				 "mute_trickle = 50\nmin_active_links = 1\nbusy_poll_us = 50\ncpu = 3\nrt_priority = 10\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.mute_behind_ms, 300);
	CHECK_EQ(c.unmute_behind_ms, 240); /* 80 % of mute_behind_ms */
	CHECK_EQ(c.mute_trickle, 50);
	CHECK_EQ(c.min_active_links, 1);
	CHECK_EQ(c.busy_poll_us, 50);
	CHECK_EQ(c.cpu, 3);
	CHECK_EQ(c.rt_priority, 10);
	CHECK(warn[0] == '\0');
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c,
				 "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\nmute_behind_ms = 0\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.unmute_behind_ms, 0); /* delay muting off */
	cg_config_free(&c);
	CHECK_EQ(cg_config_parse(&c,
				 "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\nmute_behind_ms = 100\n"
				 "unmute_behind_ms = 150\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "unmute_behind_ms") != NULL);
	CHECK_EQ(cg_config_parse(&c,
				 "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\nmin_active_links = 0\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 192.0.2.1:1\nprobe_interval_ms = 200\n"
				 "probe_idle_ms = 100\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1); /* idle probing cannot be faster than active probing */
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\nserver = 192.0.2.1:1\nprobe_interval_ms = 2000\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 0);
	CHECK_EQ(c.probe_idle_ms, 2000); /* raised to the active interval */
	cg_config_free(&c);

	/* Errors carry the key or the line. */
	CHECK_EQ(cg_config_parse(&c, "mode = client\nkey = " KEY "\n", err, sizeof(err), warn, sizeof(warn)), -1);
	CHECK(strstr(err, "server") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = c2hvcnQ=\nwireguard = 127.0.0.1:1\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "key") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = client\n\nthis line is wrong\n", err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "line 3") != NULL);
	CHECK_EQ(cg_config_parse(&c,
				 "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51820\nmax_sessions = 0\n",
				 err, sizeof(err), warn, sizeof(warn)),
		 -1);
	CHECK(strstr(err, "max_sessions") != NULL);
	CHECK_EQ(cg_config_parse(&c, "mode = server\nkey = " KEY "\nwireguard = localhost:51820\n", err,
				 sizeof(err), warn, sizeof(warn)),
		 -1); /* wireguard must be numeric */

	test_config_reload();
	test_config_addrs();
	test_config_names();
	test_config_native();
	test_config_threads();
	test_config_clients();
	test_config_bonding();
}
