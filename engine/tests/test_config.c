/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
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
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "max_sessions"));
	cg_config_free(&a);
	CHECK_EQ(cg_config_parse(&a, "mode = server\nkey = " KEY "\nwireguard = 127.0.0.1:51821\n", err, sizeof(err),
				 warn, sizeof(warn)),
		 0);
	CHECK(!strcmp(cg_config_restart_needed(&a, &b), "wireguard"));
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
	CHECK(warn[0] == '\0');
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
}
