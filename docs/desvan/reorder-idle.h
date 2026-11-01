/* docs/desvan/reorder-idle.h: reorder_idle_ms: the reorderer stops waiting for a link that brought no data for this long (clockwork audit C2)
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/arrival.h, client.c and server.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* Whether link i brought no copy for idle_us (0: never idle): the reorderer
 * need not wait for it (reorder_idle_ms; clockwork audit C2). */
static inline int cg_arr_idle(const struct cg_link_rx *l, uint32_t now_us, uint32_t idle_us)
{
	return idle_us && (!l->last_us || now_us - l->last_us > idle_us);
}

/* The callers, on the links the reorderer waits for (client.c's ro_rset,
 * server.c's ro_rset_of), before reorder_active narrows them:
 *
 *	// reorder_idle_ms: not a link that brought no data for that long (the
 *	// server's ETA may send it nothing; clockwork audit C2)
 *	for (int i = 0; c->cfg->reorder_idle_ms && i < CG_MAX_LINKS; i++)
 *		if ((m >> i & 1) && cg_arr_idle(&c->rxs.rx[i], (uint32_t)(now_ms * 1000), c->cfg->reorder_idle_ms * 1000))
 *			m &= (uint16_t)~(1u << i);
 *
 * config: reorder_idle_ms, 0-60000, 0 (off). bench/labq.py's example plan
 * used it (idle50).
 *
 * Measured (story 013): reorder_idle_ms = 500 did not help; what the audit's
 * C2 asked for came with the active-link mask (reorder_active, active.h),
 * which narrows the set by what each link carried lately. */
