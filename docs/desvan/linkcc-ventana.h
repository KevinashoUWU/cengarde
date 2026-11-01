/* docs/desvan/linkcc-ventana.h: the split by each link's window, as OMR's MPTCP scheduler (link_cc_bits 64, WND)
 *
 * Out of the engine since claude/lc-clean (story 029, «La limpieza del
 * ritmo por enlace»); it lived in engine/src/linkcc.h and engine/src/eta.h until commit 3e35f6f
 * (`git show 3e35f6f:engine/src/linkcc.h`). Not compiled: a block to pick
 * up again with a chisel, for some scenario. How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#define CG_LC_WND 64u /* the split gives a link data only while its window (cg_lc_cwnd) has room (eta.h's cg_eta_wnd) */

/* The bytes that still fit in flight (negative: over the window). */
static inline int64_t cg_lc_wnd_left(const struct cg_lc *c)
{
	return (int64_t)cg_lc_cwnd(c, c->target_us) - (int64_t)c->inflight;
}

/* In eta.h, struct cg_eta_link had
 *
 *	// link_cc's window (cg_eta_wnd): with has_wnd, the bytes it may still
 *	// take before its window is full; past it, as past limit_us
 *	uint8_t has_wnd;
 *	int64_t wnd;
 *
 * cleared (l->has_wnd = 0) in cg_eta_set and cg_eta_own; cg_eta_pick took
 * a link past its window as one past its limit_us, and counted what it took:
 *
 *	if ((l->limit_us && eta - l->base_us > l->limit_us) || (l->has_wnd && l->wnd < (int64_t)len))
 *		eta += l->over_us ? l->over_us : CG_ETA_OVER_US;
 *	...
 *	e->l[best].wnd -= len;
 */

/* Link i's window (link_cc, linkcc.h's CG_LC_WND): it may take wnd bytes
 * more before what is in flight fills it; each packet it takes counts, and
 * a packet past it goes there only as a last resort, as one past limit_us
 * (OMR's MPTCP scheduler gives a subflow data only while its window has
 * room: a link that slows down stops taking it by itself). */
static inline void cg_eta_wnd(struct cg_eta *e, unsigned i, int64_t wnd)
{
	e->l[i].has_wnd = 1;
	e->l[i].wnd = wnd;
}

/* The callers, in lc_eta (client.c and server.c), after cg_eta_own:
 *
 *	if (lc->bits & CG_LC_WND)
 *		cg_eta_wnd(e, i, cg_lc_wnd_left(lc));
 *
 * Neutral in the lab (n = 4, 6 cases: down -6 to 0 %, up 0 to +4 %). */

/* Its test (engine/tests/test_eta.c) */
/* link_cc's window: a link whose window is full takes a packet only as a
 * last resort, whatever its arrival; each packet it takes counts. */
static void test_eta_wnd(void)
{
	struct cg_eta e;

	cg_eta_reset(&e);
	cg_eta_own(&e, 0, 100000000, 0, 0, 0, 1000); /* fast: arrives first */
	cg_eta_own(&e, 1, 10000000, 5000, 0, 0, 1000);
	cg_eta_wnd(&e, 0, 3000);
	CHECK_EQ(cg_eta_pick(&e, 3, 1400, 1000), 0);
	CHECK_EQ(cg_eta_pick(&e, 3, 1400, 1000), 0);
	CHECK_EQ(e.l[0].wnd, 200);
	CHECK_EQ(cg_eta_pick(&e, 3, 1400, 1000), 1); /* its window is full */
	/* a new window (the next SFACK) opens it again */
	cg_eta_wnd(&e, 0, 100000);
	CHECK_EQ(cg_eta_pick(&e, 3, 1400, 1000), 0);
	/* every window full: the earliest arrival, as without them */
	cg_eta_wnd(&e, 0, 0);
	cg_eta_wnd(&e, 1, 0);
	CHECK_EQ(cg_eta_pick(&e, 3, 1400, 1000), 0);
	/* cg_eta_own clears it */
	cg_eta_own(&e, 0, 100000000, 0, 0, 0, 2000);
	CHECK_EQ(e.l[0].has_wnd, 0);
}
