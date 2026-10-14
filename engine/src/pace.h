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
 * The rate is twice the link's capacity (capacity.h), and never under
 * CG_PACE_FLOOR: pacing is for breaking the bursts at the local network's
 * speed into something near the link's, not for holding the link. A first
 * rule at 5/4 of the capacity sank on the testnet: right after a restart
 * the capacity is what an idle link delivers, near nothing; the probes,
 * paced on the same socket, took the pacing for the link's queue; and the
 * capacity, measured behind the pacing, stayed at it (the upload went to
 * 0-2 Mbit/s). Twice the capacity is never under what the link delivers.
 * No capacity yet, no pacing. The engine keeps no queue of its own: the
 * kernel does, per socket, in fq.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PACE_H
#define CG_PACE_H

#include <stdint.h>

#define CG_PACE_NONE UINT64_MAX /* SO_MAX_PACING_RATE's "no limit" */
#define CG_PACE_FLOOR 2500000ull /* bytes/s: 20 Mbit/s */

/* The pacing rate in bytes/s for a link of capacity cap_bps (bits/s, 0:
 * unknown). */
static inline uint64_t cg_pace_rate(uint64_t cap_bps)
{
	uint64_t r;

	if (!cap_bps)
		return CG_PACE_NONE;
	r = cap_bps / 4; /* twice, in bytes */
	return r > CG_PACE_FLOOR ? r : CG_PACE_FLOOR;
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
