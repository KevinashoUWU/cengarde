/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <string.h>

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

void test_config(void)
{
	struct cg_config c;
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
}
