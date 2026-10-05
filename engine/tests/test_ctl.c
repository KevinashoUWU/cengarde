/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "ctl.h"
#include "test.h"

static int parses(const char *line, struct cg_ctl_cmd *cmd)
{
	char err[128];

	return cg_ctl_parse(line, cmd, err, sizeof(err)) == 0;
}

static void test_parse(void)
{
	struct cg_ctl_cmd c;
	char err[128], longline[400];

	CHECK(parses("status", &c) && c.op == CG_CTL_STATUS);
	CHECK(parses("  links\n", &c) && c.op == CG_CTL_LINKS);
	CHECK(parses("reset\r\n", &c) && c.op == CG_CTL_RESET);
	CHECK(parses("reload", &c) && c.op == CG_CTL_RELOAD);
	CHECK(parses("link eth1.10 off", &c) && c.op == CG_CTL_LINK && !strcmp(c.ifname, "eth1.10") &&
	      c.ovr == CG_OVR_OFF);
	CHECK(parses("link\twwan0  on\n", &c) && c.op == CG_CTL_LINK && !strcmp(c.ifname, "wwan0") && c.ovr == CG_OVR_ON);
	CHECK(parses("link eth0 auto", &c) && c.ovr == CG_OVR_AUTO);
	CHECK(parses("link abcdefghijklmno off", &c)); /* 15 characters: the longest name */

	CHECK_EQ(cg_ctl_parse("", &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "empty") != NULL);
	CHECK_EQ(cg_ctl_parse(" \n", &c, err, sizeof(err)), -1);
	CHECK_EQ(cg_ctl_parse("status now", &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "takes no arguments") != NULL);
	CHECK_EQ(cg_ctl_parse("shutdown", &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "unknown command 'shutdown'") != NULL);
	CHECK_EQ(cg_ctl_parse("link eth0", &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "usage") != NULL);
	CHECK_EQ(cg_ctl_parse("link eth0 down", &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "off, on or auto") != NULL);
	CHECK_EQ(cg_ctl_parse("link abcdefghijklmnop off", &c, err, sizeof(err)), -1); /* 16: too long */
	CHECK_EQ(cg_ctl_parse("link ../x off", &c, err, sizeof(err)), -1);
	CHECK_EQ(cg_ctl_parse("link a:1 off", &c, err, sizeof(err)), -1);
	CHECK_EQ(cg_ctl_parse("link .. off", &c, err, sizeof(err)), -1);
	CHECK_EQ(cg_ctl_parse("link a b c d", &c, err, sizeof(err)), -1);
	memset(longline, 'x', sizeof(longline) - 1);
	longline[sizeof(longline) - 1] = '\0';
	CHECK_EQ(cg_ctl_parse(longline, &c, err, sizeof(err)), -1);
	CHECK(strstr(err, "too long") != NULL);

	CHECK(cg_ifname_valid("eth1.10"));
	CHECK(cg_ifname_valid("wwan0"));
	CHECK(!cg_ifname_valid(""));
	CHECK(!cg_ifname_valid("."));
	CHECK(!cg_ifname_valid("a/b"));
	CHECK(!cg_ifname_valid("a b"));
}

static void test_overrides(void)
{
	struct cg_ovr_table t = { 0 };
	char name[IFNAMSIZ];

	CHECK_EQ(cg_ovr_get(&t, "eth0"), CG_OVR_AUTO);
	CHECK_EQ(cg_ovr_set(&t, "eth0", CG_OVR_OFF), 0);
	CHECK_EQ(cg_ovr_set(&t, "wwan0", CG_OVR_ON), 0);
	CHECK_EQ(t.n, 2);
	CHECK_EQ(cg_ovr_get(&t, "eth0"), CG_OVR_OFF);
	CHECK_EQ(cg_ovr_get(&t, "wwan0"), CG_OVR_ON);
	CHECK_EQ(cg_ovr_get(&t, "eth1"), CG_OVR_AUTO);

	/* Setting again replaces; auto removes. */
	CHECK_EQ(cg_ovr_set(&t, "eth0", CG_OVR_ON), 0);
	CHECK_EQ(t.n, 2);
	CHECK_EQ(cg_ovr_get(&t, "eth0"), CG_OVR_ON);
	CHECK_EQ(cg_ovr_set(&t, "eth0", CG_OVR_AUTO), 0);
	CHECK_EQ(t.n, 1);
	CHECK_EQ(cg_ovr_get(&t, "eth0"), CG_OVR_AUTO);
	CHECK_EQ(cg_ovr_get(&t, "wwan0"), CG_OVR_ON);
	CHECK_EQ(cg_ovr_set(&t, "never-set", CG_OVR_AUTO), 0);
	CHECK_EQ(t.n, 1);

	/* A name fits with its terminator: IFNAMSIZ - 1 characters at most. */
	CHECK_EQ(cg_ovr_set(&t, "abcdefghijklmno", CG_OVR_OFF), 0);
	CHECK_EQ(t.n, 2);
	CHECK_EQ(cg_ovr_get(&t, "abcdefghijklmno"), CG_OVR_OFF);
	CHECK_EQ(cg_ovr_set(&t, "abcdefghijklmnop", CG_OVR_OFF), -1);
	CHECK_EQ(t.n, 2);
	CHECK_EQ(cg_ovr_get(&t, "abcdefghijklmnop"), CG_OVR_AUTO);
	CHECK_EQ(cg_ovr_set(&t, "abcdefghijklmno", CG_OVR_AUTO), 0);
	CHECK_EQ(t.n, 1);

	/* Full table: new names are refused, existing ones still change. */
	for (int i = 0; t.n < CG_CTL_OVERRIDES; i++) {
		snprintf(name, sizeof(name), "eth%d", i);
		CHECK_EQ(cg_ovr_set(&t, name, CG_OVR_OFF), 0);
	}
	CHECK_EQ(cg_ovr_set(&t, "one-more", CG_OVR_OFF), -1);
	CHECK_EQ(cg_ovr_set(&t, "wwan0", CG_OVR_OFF), 0);
	CHECK_EQ(cg_ovr_get(&t, "wwan0"), CG_OVR_OFF);
	cg_ovr_reset(&t);
	CHECK_EQ(t.n, 0);
	CHECK_EQ(cg_ovr_get(&t, "wwan0"), CG_OVR_AUTO);

	/* An override goes before the configuration, auto leaves it the say. */
	CHECK(cg_ovr_wanted(CG_OVR_AUTO, 1));
	CHECK(!cg_ovr_wanted(CG_OVR_AUTO, 0));
	CHECK(!cg_ovr_wanted(CG_OVR_OFF, 1));
	CHECK(cg_ovr_wanted(CG_OVR_ON, 0));
	CHECK(!strcmp(cg_ovr_name(CG_OVR_OFF), "off") && !strcmp(cg_ovr_name(CG_OVR_ON), "on") &&
	      !strcmp(cg_ovr_name(CG_OVR_AUTO), "auto"));
}

void test_ctl(void)
{
	test_parse();
	test_overrides();
}
