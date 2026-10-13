/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "wgport.h"

void test_wgport(void)
{
	const char *names[] = { "alpha", "b-2", "gamma" };
	const uint16_t ports[] = { 34604, 0, 1 };
	const char *one[] = { "" };
	const uint16_t one_port[] = { 65535 };
	char buf[256], small[sizeof(CG_WGPORT_HEADER) + 4];
	int len;

	/* Written: the header, then one line per client with a port. */
	len = cg_wgport_table(buf, sizeof(buf), names, ports, 3);
	CHECK(len > 0);
	CHECK_EQ((size_t)len, strlen(buf));
	CHECK(!strcmp(buf, CG_WGPORT_HEADER "34604 alpha\n1 gamma\n"));
	CHECK_EQ(cg_wgport_find(buf, "alpha"), 34604);
	CHECK_EQ(cg_wgport_find(buf, "gamma"), 1);
	CHECK_EQ(cg_wgport_find(buf, "b-2"), 0);
	CHECK_EQ(cg_wgport_find(buf, ""), 0);
	CHECK_EQ(cg_wgport_find(buf, "alph"), 0);
	CHECK_EQ(cg_wgport_find(buf, "alphaa"), 0);

	/* The one client of a server without [client] sections has no name. */
	len = cg_wgport_table(buf, sizeof(buf), one, one_port, 1);
	CHECK(!strcmp(buf, CG_WGPORT_HEADER "65535\n"));
	CHECK_EQ(cg_wgport_find(buf, ""), 65535);
	CHECK_EQ(cg_wgport_find(buf, "alpha"), 0);

	/* Nothing to write: the header alone. Too small a buffer: -1. */
	CHECK_EQ(cg_wgport_table(buf, sizeof(buf), names, (const uint16_t[]){ 0, 0, 0 }, 3),
		 (int)strlen(CG_WGPORT_HEADER));
	CHECK_EQ(cg_wgport_table(small, sizeof(small), names, ports, 3), -1);
	CHECK_EQ(cg_wgport_table(buf, sizeof(CG_WGPORT_HEADER) - 1, names, ports, 0), -1);

	/* Read: another version, no header, or no text is nothing. */
	CHECK_EQ(cg_wgport_find(NULL, "alpha"), 0);
	snprintf(buf, sizeof(buf), "%s", "34604 alpha\n"); /* a literal would draw -Warray-bounds */
	CHECK_EQ(cg_wgport_find(buf, "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_TITLE "10, written by the engine: data only\n34604 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_TITLE "2, written by the engine: data only\n34604 alpha\n", "alpha"), 0);

	/* Lines that do not parse are skipped; the last good line for a name wins. */
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "0 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "65536 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "034604 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "100000 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "-5 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER " 5 alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "5  alpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "5\talpha\n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "5 alpha \n", "alpha"), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "5x\n", ""), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "# 5 alpha\n7 alpha\nbad\n\n9 gamma\n", "alpha"), 7);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "7 alpha\n8 alpha\n", "alpha"), 8);
	/* A last line cut short (no "\n") is not taken. */
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "7 alpha\n8 alph", "alpha"), 7);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER "7", ""), 0);
	CHECK_EQ(cg_wgport_find(CG_WGPORT_HEADER, "alpha"), 0);
}
