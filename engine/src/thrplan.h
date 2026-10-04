/* Thread plan: which link_threads mode a router runs, which pump each link
 * gets, the warnings for knobs that would starve the machine, and the CPU
 * use of a thread over the last seconds. Pure functions, no clock and no
 * I/O (tests/test_thrplan.c).
 *
 * - link_threads = auto turns the per-link threads on only for the class of
 *   machine they were measured on (aarch64 or x86-64, at least 4 CPUs in the
 *   affinity mask, the hub not pinned) and only in a release that allows it
 *   (CG_LT_AUTO_ON): until gate P passes on the Pi, auto is legacy, today's
 *   loop, everywhere. A 2-core MIPS router pays far more per wake-up and was
 *   never measured: it stays on legacy unless set by hand.
 * - A link gets a pump of its own while fewer than CG_MAX_PUMPS exist; later
 *   links join the pump with the fewest links (ties: the lowest index). The
 *   choice is sticky: link slots are never freed.
 * - Guards: real-time priority on as many data threads as there are CPUs
 *   (the kernel's softirqs and WireGuard's workers would starve), busy
 *   polling on every data thread when they do not fit the CPUs with one to
 *   spare (then only the hub polls), two pumps pinned to one CPU.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_THRPLAN_H
#define CG_THRPLAN_H

#include <stdint.h>

#include "config.h" /* enum cg_link_threads */

/* Whether link_threads = auto may turn the threads on in this release: not
 * until gate P (the Pi, docs/historias/011) passes. */
#define CG_LT_AUTO_ON 0

#if defined(__aarch64__) || defined(__x86_64__)
#define CG_LT_ARCH_MEASURED 1
#else
#define CG_LT_ARCH_MEASURED 0
#endif

/* The mode to run: CG_LT_ON, CG_LT_OFF or CG_LT_LEGACY. arch_measured: the
 * build is for a measured architecture; ncpus: CPUs in the affinity mask;
 * hub_pinned: the cpu knob is set; auto_on: CG_LT_AUTO_ON. */
static inline int cg_lt_resolve(int setting, int arch_measured, int ncpus, int hub_pinned, int auto_on)
{
	if (setting != CG_LT_AUTO)
		return setting;
	return auto_on && arch_measured && ncpus >= 4 && !hub_pinned ? CG_LT_ON : CG_LT_LEGACY;
}

/* The pump for a link that has none yet: a new one (index npumps) while
 * fewer than cap exist, else the one with the fewest links. nlinks: links
 * per existing pump. */
static inline int cg_pump_pick(const uint8_t *nlinks, int npumps, int cap)
{
	int best = 0;

	if (npumps < cap)
		return npumps;
	for (int i = 1; i < npumps; i++)
		if (nlinks[i] < nlinks[best])
			best = i;
	return best;
}

/* What the guards found (bits). */
#define CG_TG_RT_ALL 1     /* rt_priority on as many data threads as CPUs */
#define CG_TG_BUSY_HUB 2   /* busy polling only on the hub: the threads do not fit */
#define CG_TG_PIN_SHARED 4 /* two pumps pinned to one CPU */

/* data_threads: the hub and the pumps; pins: the CPU each pump is pinned
 * to, -1 for none. */
static inline unsigned cg_thr_guards(int data_threads, int ncpus, uint32_t rt_priority, uint32_t busy_poll_us,
				     const int *pins, int npins)
{
	unsigned g = 0;

	if (rt_priority && data_threads >= ncpus)
		g |= CG_TG_RT_ALL;
	if (busy_poll_us && data_threads + 1 > ncpus)
		g |= CG_TG_BUSY_HUB;
	for (int i = 0; i < npins; i++)
		for (int k = i + 1; k < npins; k++)
			if (pins[i] >= 0 && pins[i] == pins[k])
				g |= CG_TG_PIN_SHARED;
	return g;
}

/* CPU use of a thread: one sample of its CPU clock a second, the share of
 * one CPU over the last CG_CPUWIN_S seconds. */
#define CG_CPUWIN_S 5

struct cg_cpuwin {
	uint64_t t_ms[CG_CPUWIN_S + 1];
	uint64_t cpu_ns[CG_CPUWIN_S + 1];
	uint8_t n, pos; /* samples held, and where the next one goes */
};

static inline void cg_cpuwin_add(struct cg_cpuwin *w, uint64_t now_ms, uint64_t cpu_ns)
{
	w->t_ms[w->pos] = now_ms;
	w->cpu_ns[w->pos] = cpu_ns;
	w->pos = (uint8_t)((w->pos + 1) % (CG_CPUWIN_S + 1));
	if (w->n < CG_CPUWIN_S + 1)
		w->n++;
}

/* Tenths of a percent of one CPU between the oldest and the newest sample;
 * -1 with fewer than two. */
static inline int cg_cpuwin_permille(const struct cg_cpuwin *w)
{
	unsigned newest = (w->pos + CG_CPUWIN_S) % (CG_CPUWIN_S + 1);
	unsigned oldest = w->n < CG_CPUWIN_S + 1 ? 0 : w->pos;
	uint64_t dt, dc;

	if (w->n < 2)
		return -1;
	dt = w->t_ms[newest] - w->t_ms[oldest];
	dc = w->cpu_ns[newest] - w->cpu_ns[oldest];
	if (!dt || w->cpu_ns[newest] < w->cpu_ns[oldest])
		return -1;
	return (int)(dc / dt / 1000); /* ns per ms of wall time: 10^6 is one CPU, 1000 per permille */
}

#endif
