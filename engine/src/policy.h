/* Link and path send policy, kept pure so it can be unit-tested.
 *
 * Routing and lifetime are separate (libRIST, docs/historias/003): a link
 * that has been silent for stall_ms stops carrying payload but keeps its
 * socket or path, and resumes as soon as a verified packet arrives on it.
 * If every link is silent, payload goes to all of them rather than to none.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_POLICY_H
#define CG_POLICY_H

#include <stdint.h>

/* A link is live when a verified packet arrived on it within stall_ms. */
static inline int cg_link_live(uint64_t last_rx_ms, uint64_t now_ms, uint64_t stall_ms)
{
	return last_rx_ms != 0 && now_ms >= last_rx_ms && now_ms - last_rx_ms <= stall_ms;
}

/* Should payload go out on this link? any_live: at least one link is live. */
static inline int cg_link_sends(uint64_t last_rx_ms, uint64_t now_ms, uint64_t stall_ms, int any_live)
{
	return cg_link_live(last_rx_ms, now_ms, stall_ms) || !any_live;
}

#endif
