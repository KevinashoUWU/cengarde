/* docs/desvan/rescue-by-owd.c: rescue_by_owd: the client's rescue goes on the link of least upload delay, not RTT (clockwork audit C10)
 *
 * Out of the engine since claude/desvan-2 (story 029, «El desván, segunda
 * tanda»): no preset, no field configuration and no lab case turned it on.
 * It lived in engine/src/client.c until commit 4569dec (`git show 4569dec:<file>`).
 * Not compiled: a block to pick up again with a chisel, for some scenario.
 * How to put it back: README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* In client.c's on_nack, before the links by RTT (which then run only while
 * no link was picked, by_rtt = !best[0]): best[0] and best[1], the two
 * links a rescued packet may go on. */
#if 0
	/* rescue_by_owd (audit C10): the link of least upload delay, as the
	 * server picks; by RTT while no link has one */
	for (int i = 0; c->cfg->rescue_by_owd && i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !c->uh[i].have_owd)
			continue;
		if (!best[0] || cg_owd_diff(c->uh[i].owd, c->uh[best[0] - c->link].owd) < 0) {
			best[1] = best[0];
			best[0] = l;
		} else if (!best[1] || cg_owd_diff(c->uh[i].owd, c->uh[best[1] - c->link].owd) < 0) {
			best[1] = l;
		}
	}
#endif

/* config: rescue_by_owd = no. Story 015 lists it among the audit's fixes per
 * link type (C10, global, client only); never on in a preset or the field:
 * the server already picks by the one-way delay, the client by RTT. */
