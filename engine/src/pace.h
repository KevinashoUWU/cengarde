/* Each link's sending rate, for the kernel to pace its socket at
 * (SO_MAX_PACING_RATE, with the fq qdisc on the way out; story 013, the
 * step after split_eta). The engine sends a batch on a link with one
 * sendmmsg, at the speed of the local network: a burst that waits in the
 * modem's queue and makes that link's delay jump by tens of ms while it
 * drains, and the packets of the other links then arrive out of order (on
 * the testnet the links' delays without a queue differed by 2-6 ms only,
 * and the reorderer still held 80 % of the download). Paced, a link takes
 * its packets at about the rate it carries them and its queue stays short.
 *
 * The rate is the link's capacity (capacity.h) times 5/4 while its queue
 * is under half the target, and the capacity itself above it: pacing
 * smooths the bursts, it does not hold the link below what it can show (a
 * pace below the capacity would bring what the link delivers, and with it
 * the capacity, down: the shaper of PR 4 did that). No capacity yet, no
 * pacing. The engine keeps no queue of its own: the kernel does, per
 * socket, in fq.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PACE_H
#define CG_PACE_H

#include <stdint.h>

#define CG_PACE_NONE UINT64_MAX /* SO_MAX_PACING_RATE's "no limit" */

/* The pacing rate in bytes/s for a link of capacity cap_bps (bits/s, 0:
 * unknown) whose queue is queue_us (-1: unknown), against target_us. */
static inline uint64_t cg_pace_rate(uint64_t cap_bps, int32_t queue_us, uint32_t target_us)
{
	uint64_t r;

	if (!cap_bps)
		return CG_PACE_NONE;
	r = cap_bps / 8;
	if (queue_us >= 0 && (uint32_t)queue_us < target_us / 2)
		r += r / 4;
	return r ? r : 1;
}

/* Whether a rate set before, old, should be set again to now: a change of
 * more than 1/16 either way, or to or from no limit. */
static inline int cg_pace_changed(uint64_t old, uint64_t now)
{
	if (old == now)
		return 0;
	if (old == CG_PACE_NONE || now == CG_PACE_NONE)
		return 1;
	return now > old + old / 16 || now + now / 16 < old;
}

#endif
