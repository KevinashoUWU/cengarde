/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "test.h"
#include "thrplan.h"

void test_thrplan(void)
{
	uint8_t nl[8] = { 0 };
	int pins[4] = { -1, 2, -1, 2 };
	struct cg_cpuwin w;

	/* Explicit settings stand. */
	CHECK_EQ(cg_lt_resolve(CG_LT_ON, 0, 1, 1, 0), CG_LT_ON);
	CHECK_EQ(cg_lt_resolve(CG_LT_OFF, 1, 8, 0, 1), CG_LT_OFF);
	CHECK_EQ(cg_lt_resolve(CG_LT_LEGACY, 1, 8, 0, 1), CG_LT_LEGACY);
	/* auto in this release: legacy everywhere, the Pi's class included. */
	CHECK_EQ(CG_LT_AUTO_ON, 0);
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 4, 0, CG_LT_AUTO_ON), CG_LT_LEGACY);
	/* auto once a release allows it: only the measured class. */
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 4, 0, 1), CG_LT_ON);     /* aarch64 or x86-64, 4 CPUs */
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 8, 0, 1), CG_LT_ON);
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 2, 0, 1), CG_LT_LEGACY); /* 2 CPUs */
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 1, 0, 1), CG_LT_LEGACY);
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 0, 4, 0, 1), CG_LT_LEGACY); /* MIPS, 32-bit ARM */
	CHECK_EQ(cg_lt_resolve(CG_LT_AUTO, 1, 4, 1, 1), CG_LT_LEGACY); /* the hub pinned with cpu */
#if defined(__aarch64__) || defined(__x86_64__)
	CHECK_EQ(CG_LT_ARCH_MEASURED, 1);
#else
	CHECK_EQ(CG_LT_ARCH_MEASURED, 0);
#endif

	/* Pumps: one per link up to the cap, then the fewest links, lowest
	 * index on ties. */
	CHECK_EQ(cg_pump_pick(nl, 0, 8), 0);
	CHECK_EQ(cg_pump_pick(nl, 5, 8), 5);
	for (int i = 0; i < 8; i++)
		nl[i] = 1;
	CHECK_EQ(cg_pump_pick(nl, 8, 8), 0);
	nl[0] = 2;
	CHECK_EQ(cg_pump_pick(nl, 8, 8), 1);
	nl[1] = 2;
	nl[5] = 0;
	CHECK_EQ(cg_pump_pick(nl, 8, 8), 5);
	CHECK_EQ(cg_pump_pick(nl, 2, 2), 0); /* a cap of 2 */

	/* Guards. */
	CHECK_EQ(cg_thr_guards(4, 4, 10, 0, NULL, 0), CG_TG_RT_ALL);
	CHECK_EQ(cg_thr_guards(3, 4, 10, 0, NULL, 0), 0);
	CHECK_EQ(cg_thr_guards(6, 4, 0, 50, NULL, 0), CG_TG_BUSY_HUB);
	CHECK_EQ(cg_thr_guards(4, 4, 0, 50, NULL, 0), CG_TG_BUSY_HUB); /* no CPU to spare */
	CHECK_EQ(cg_thr_guards(3, 4, 0, 50, NULL, 0), 0);
	CHECK_EQ(cg_thr_guards(5, 8, 0, 0, pins, 4), CG_TG_PIN_SHARED);
	CHECK_EQ(cg_thr_guards(5, 8, 0, 0, pins, 3), 0);
	CHECK_EQ(cg_thr_guards(6, 4, 5, 50, pins, 4), CG_TG_RT_ALL | CG_TG_BUSY_HUB | CG_TG_PIN_SHARED);

	/* CPU over the last 5 s from a thread's clock: none with one sample;
	 * half a CPU; then the window slides past an idle start. */
	memset(&w, 0, sizeof(w));
	CHECK_EQ(cg_cpuwin_permille(&w), -1);
	cg_cpuwin_add(&w, 1000, 0);
	CHECK_EQ(cg_cpuwin_permille(&w), -1);
	cg_cpuwin_add(&w, 2000, 500000000ull);
	CHECK_EQ(cg_cpuwin_permille(&w), 500);
	for (uint64_t t = 3; t <= 6; t++)
		cg_cpuwin_add(&w, t * 1000, 500000000ull);
	CHECK_EQ(cg_cpuwin_permille(&w), 100); /* 0.5 s over 5 s */
	cg_cpuwin_add(&w, 7000, 500000000ull + 250000000ull);
	CHECK_EQ(cg_cpuwin_permille(&w), 50); /* the first second left the window */
	for (uint64_t t = 8; t <= 13; t++)
		cg_cpuwin_add(&w, t * 1000, 750000000ull + (t - 7) * 1000000000ull);
	CHECK_EQ(cg_cpuwin_permille(&w), 1000); /* a whole CPU */
}
