/* DiffServ in the protocontroller (story 031, protocol 21): the DSCP a LAN
 * device marked its flow with (its TCP's SYN, its datagram) travels in the
 * OPEN, both ends mark their own sockets with it (what GRIT ends, the TUN
 * kept), and it chooses the flow's tier in GRIT's sends:
 *
 *   CG_DS_PRIO    voice, video calls and signalling (EF, VOICE-ADMIT, CS5,
 *                 AF4x, CS4, CS3, CS6, CS7; RFC 4594): their turns before
 *                 the rest, while the flow sends under a quarter of the
 *                 pacer's rate (an app that marks all it sends EF is
 *                 served as the rest, not before it)
 *   CG_DS_NORMAL  everything else, CS0 included
 *   CG_DS_LOW     lower effort (LE, RFC 8622; CS1, its older mark): only
 *                 what the rest leaves, and only while a quarter of the
 *                 peer's window in flight is free (cg_ds_low_room: what it
 *                 put in flight held the rest back once they had more to
 *                 send), but a segment after CG_DS_LOW_STARVE_US without
 *                 one, so a starved flow keeps going (with 250 ms, an
 *                 iperf3 run marked CS1 failed once in the field to get its
 *                 control messages; protocontroller_dead_s would reset a
 *                 silent one)
 *
 * Pure: no clock, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DSCP_H
#define CG_DSCP_H

#include <stdint.h>

enum { CG_DS_PRIO = 0, CG_DS_NORMAL, CG_DS_LOW, CG_DS_TIERS };

#define CG_DS_LOW_STARVE_US 50000u /* a lower-effort flow's segment at least this often */
#define CG_DS_PRIO_SHARE 4u         /* a priority flow under 1 / this of the pacer's rate */

/* The tier a DSCP asks for (its 6 bits, the TOS byte >> 2). */
static inline unsigned cg_ds_tier(uint8_t dscp)
{
	switch (dscp & 0x3f) {
	case 46: /* EF */
	case 44: /* VOICE-ADMIT */
	case 40: /* CS5 */
	case 32: /* CS4 */
	case 34: /* AF41 */
	case 36: /* AF42 */
	case 38: /* AF43 */
	case 24: /* CS3 */
	case 48: /* CS6 */
	case 56: /* CS7 */
		return CG_DS_PRIO;
	case 1: /* LE */
	case 8: /* CS1 */
		return CG_DS_LOW;
	default:
		return CG_DS_NORMAL;
	}
}

/* A flow's tier now: its DSCP's, but a priority flow that sends a quarter
 * of the pacer's rate (pace, bytes/s) or more (rate, bytes/s) goes as the
 * rest. */
static inline unsigned cg_ds_flow_tier(uint8_t dscp, uint64_t rate, uint64_t pace)
{
	unsigned t = cg_ds_tier(dscp);

	if (t == CG_DS_PRIO && rate * CG_DS_PRIO_SHARE >= pace)
		return CG_DS_NORMAL;
	return t;
}

/* Whether a lower-effort flow that last sent at last_us is owed a segment
 * at now_us among the rest (0: it never sent; it waits a turn from its
 * start, created_us). */
static inline int cg_ds_low_owed(uint64_t last_us, uint64_t created_us, uint64_t now_us)
{
	uint64_t from = last_us ? last_us : created_us;

	return now_us >= from + CG_DS_LOW_STARVE_US;
}

/* Whether a lower-effort flow may put more in flight: a quarter of the
 * window (cwnd, bytes) left free for the rest. */
static inline int cg_ds_low_room(uint64_t inflight, uint64_t cwnd)
{
	return inflight < cwnd - cwnd / 4;
}

#endif
