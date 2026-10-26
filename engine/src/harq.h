/* Loss repair, made coherent (story 023): which of the tunnel's three
 * repairs (the erasure code, the parity asked for again, the packet asked
 * for again) and inner TCP's own retransmission answers a gap, so that none
 * does another's work.
 *
 * The order, at the receiver of a gap:
 *   1. the code (redund_spare = rs | rlc, story 021) repairs it by itself:
 *      nothing is asked while repairs may still come for it (until it is
 *      sealed: a repair whose base is past it came, or the code horizon,
 *      rescue_code_ms, passed);
 *   2. sealed and still missing, with equations (RLC) or a block (RS)
 *      that miss it: the parity is asked for again (PARQ, CG_T_PARQ), as
 *      many fresh repairs as the rank deficit (RLC: combinations of exactly
 *      the missing members; RS: more indices of the same block, within its
 *      geometry, cg_rs_mhdr), never what is already decodable;
 *   3. not covered by any code (no code on, or none of its repairs came):
 *      the packet itself (CG_T_NACK, rescue.h);
 *   4. never when inner TCP's fast retransmit would come first: TCP takes
 *      a hole for lost about its RTT plus RACK's reordering window (RFC
 *      8985, a quarter of the minimum RTT) after the packets behind it
 *      arrive, so a repair that cannot land within that budget only
 *      duplicates TCP's (and the gap is given up, not held for it).
 *
 * Waiting for the code holds everything behind the gap only without TCP
 * (cg_harq_hold): with TCP flows the gap is given up at once and followed
 * out of order (the code's repair, or a late PARQ within the budget, comes
 * as a late packet that RACK's reordering window takes), because holding
 * for the code's seal (tens of ms) stalled TCP's ACK clock: in the lab,
 * with the far host 100 ms away, 23.6 against 16.9 Mbit/s (story 023).
 * Holding for a NACK, one round trip of the tunnel, did pay (7.6 to 14.4
 * Mbit/s with copies).
 *
 * The receiver learns which gaps went on one link alone (CG_F_PREV) from
 * the packet after it; cg_harq_only says what that packet must say once
 * the copies are decided (eta.h's windows and redund_spare drop copies
 * after the split chose every carrier: a hint written before them said
 * "several" for a packet that went on the first link alone, so the
 * receiver never knew it was lost on every link, story 023).
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_HARQ_H
#define CG_HARQ_H

#include <stdint.h>
#include <string.h>

#include "repair.h" /* CG_REP_*, CG_RS_MMAX */
#include "proto.h"  /* CG_PARQ_* */
#include "rs.h"     /* cg_rs_mhdr */
#include "split.h"  /* cg_split_dst */

/* The one link a packet goes on, once its copies are decided: cg_split_dst
 * of pick, carry and dup, within cmask (the copies kept, 0xffff: all); -1
 * when on several or none. trickle: mute_trickle copies a duplicated
 * packet on other live links too, so it never goes alone. */
static inline int cg_harq_only(int pick, uint16_t carry, uint16_t dup, uint16_t cmask, int trickle)
{
	uint16_t d = (uint16_t)(cg_split_dst(pick, carry, dup) & cmask);

	if (pick < 0 && trickle)
		return -1;
	return d && !(d & (d - 1)) ? __builtin_ctz(d) : -1;
}

/* ---- the parity asked for again (CG_T_PARQ, protocol version 9) ----
 *
 * Payload: a count n (1 to CG_PARQ_MAX), then n requests of CG_PARQ_REQ
 * bytes:
 *   0       kind: CG_REP_RS or CG_REP_RLC (repair.h)
 *   1       RS: the block's m as its repairs said (its geometry); RLC: 0
 *   2       d: fresh repairs wanted (1 to CG_REP_SPAN)
 *   3       0
 *   4..7    base (big-endian)
 *   8..15   map (big-endian, bit 0 set): RS, the block's members; RLC, the
 *           members missing, the only ones a fresh combination covers
 *   16..19  RS: the indices the receiver has (bit j); RLC: 0
 * The sender rebuilds them from what it keeps for the rescue (rescue.h):
 * RS, the block encoded again and d indices the receiver does not have;
 * RLC, d combinations with fresh seeds. */
struct cg_parq_req {
	uint8_t kind, m, d;
	uint32_t base;
	uint64_t map;
	uint32_t have;
};

/* The payload for req[0..n) (n 1 to CG_PARQ_MAX) into out; its length. */
static inline unsigned cg_parq_write(uint8_t *out, const struct cg_parq_req *req, unsigned n)
{
	uint8_t *o = out + 1;

	out[0] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++, o += CG_PARQ_REQ) {
		const struct cg_parq_req *q = &req[i];

		o[0] = q->kind;
		o[1] = q->m;
		o[2] = q->d;
		o[3] = 0;
		for (int b = 0; b < 4; b++)
			o[4 + b] = (uint8_t)(q->base >> (24 - 8 * b));
		for (int b = 0; b < 8; b++)
			o[8 + b] = (uint8_t)(q->map >> (56 - 8 * b));
		for (int b = 0; b < 4; b++)
			o[16 + b] = (uint8_t)(q->have >> (24 - 8 * b));
	}
	return 1 + n * CG_PARQ_REQ;
}

/* Parses a payload of plen bytes into req (CG_PARQ_MAX); the count, or -1
 * when malformed (a wrong length, kind, d or map). */
static inline int cg_parq_read(struct cg_parq_req *req, const uint8_t *p, unsigned plen)
{
	unsigned n;

	if (plen < 1 + CG_PARQ_REQ)
		return -1;
	n = p[0];
	if (!n || n > CG_PARQ_MAX || plen != 1 + n * CG_PARQ_REQ)
		return -1;
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *o = p + 1 + i * CG_PARQ_REQ;
		struct cg_parq_req *q = &req[i];

		q->kind = o[0];
		q->m = o[1];
		q->d = o[2];
		q->base = (uint32_t)o[4] << 24 | (uint32_t)o[5] << 16 | (uint32_t)o[6] << 8 | o[7];
		q->map = 0;
		for (int b = 0; b < 8; b++)
			q->map = q->map << 8 | o[8 + b];
		q->have = (uint32_t)o[16] << 24 | (uint32_t)o[17] << 16 | (uint32_t)o[18] << 8 | o[19];
		if (!(q->map & 1) || !q->d || q->d > CG_REP_SPAN)
			return -1;
		if (q->kind == CG_REP_RS) {
			unsigned k = (unsigned)__builtin_popcountll(q->map);

			if (!q->m || q->m > CG_RS_MMAX || q->m > k)
				return -1;
		} else if (q->kind != CG_REP_RLC || q->m || q->have) {
			return -1;
		}
	}
	return (int)n;
}

/* ---- the route of a gap ---- */

enum cg_harq_do {
	CG_HARQ_WAIT = 0, /* repairs of the code may still come: ask later */
	CG_HARQ_NACK,     /* the packet again (rescue.h) */
	CG_HARQ_PARQ,     /* the code's deficit again */
	CG_HARQ_NONE,     /* nothing to ask: decodable, or here already */
	CG_HARQ_TCP,      /* too late to beat TCP's own retransmission: leave it to TCP */
};

struct cg_harq_gap {
	uint8_t code;     /* a code is on (its repairs came lately) */
	uint8_t here;     /* the packet is here (came, or rebuilt) */
	uint8_t sealed;   /* a repair whose base is past it came: no more will cover it */
	uint8_t deficit;  /* the rank deficit of the equations or block missing it; 0: none miss it */
	uint32_t age_us;  /* since it was found missing */
	uint32_t horizon_us; /* how long the code may take (rescue_code_ms) */
	uint32_t eta_us;  /* how long an answer takes (the round trip of the fastest path) */
	uint32_t budget_us; /* until TCP retransmits it from when it was found missing; 0: no TCP */
	uint8_t wide;       /* FEC v3's wide windows protect it (story 026) */
	uint16_t future;    /* wide: the repairs still to come over it, about (cg_harq_future) */
};

/* What to do with gap g now. */
static inline enum cg_harq_do cg_harq_route(const struct cg_harq_gap *g)
{
	if (g->here)
		return CG_HARQ_NONE;
	if (g->code && !g->sealed && g->age_us < g->horizon_us) {
		/* the code may still repair it; but if waiting more leaves no
		 * time to ask before TCP retransmits, TCP has it already */
		if (g->budget_us && g->age_us + g->eta_us >= g->budget_us)
			return CG_HARQ_TCP;
		/* a wide window's coming repairs cannot close its deficit (a
		 * stall longer than the code's share covers): the rest asked
		 * for now, a round trip, not after the window has passed it */
		if (g->wide && g->deficit > g->future)
			return CG_HARQ_PARQ;
		return CG_HARQ_WAIT;
	}
	if (g->budget_us && g->age_us + g->eta_us > g->budget_us)
		return CG_HARQ_TCP;
	if (g->code && g->deficit)
		return CG_HARQ_PARQ;
	return CG_HARQ_NACK;
}

/* FEC v3 (story 026): the repairs still to come over gap seq, about: the
 * newest long repair covered up to top with a window of span sequences, so
 * repairs go on covering seq until the newest sequence passes seq + span -
 * 1; one comes every gap16 / 16 sequences (smoothed, as they arrive: those
 * a stalled link took are not counted), and every other one covers only
 * the window's last quarter (fec3.h's cg_f3_span_us): a gap older than that
 * gets half of them. At most 65535; 0 when gap16 is 0 (not known yet). */
static inline uint16_t cg_harq_future(uint32_t seq, uint32_t top, uint32_t span, uint32_t gap16)
{
	int32_t left = (int32_t)(seq + span - 1 - top);
	uint64_t f;

	if (left <= 0 || !gap16)
		return 0;
	f = (uint64_t)left * 16 / gap16;
	if ((int32_t)(top - seq) > (int32_t)(span / 4))
		f /= 2;
	return f > 65535 ? 65535 : (uint16_t)f;
}

/* Whether a gap waiting for the code holds what follows it: only without
 * TCP flows (budget_us 0). */
static inline int cg_harq_hold(uint32_t budget_us)
{
	return !budget_us;
}

/* RACK's budget (RFC 8985): TCP retransmits a hole about its RTT plus a
 * quarter of its minimum RTT after the packets behind it arrive; 0 when no
 * TCP flow is known. */
static inline uint32_t cg_harq_budget_us(uint32_t flow_rtt_us)
{
	return flow_rtt_us ? flow_rtt_us + flow_rtt_us / 4 : 0;
}

/* FEC v3 without the rescue (story 026, rescue_wait_ms 0, the client keeps
 * no ring): an asked gap waits for the code alone, code_us (its horizon,
 * rescue_code_ms), never past TCP's budget; 0 when no code is on. */
static inline uint32_t cg_harq_code_wait_us(uint32_t code_us, uint32_t budget_us)
{
	if (!code_us)
		return 0;
	if (budget_us && code_us > budget_us)
		code_us = budget_us;
	return code_us ? code_us : 1;
}

/* The members of a PARQ request (qbase, qmap) as a map from base (bit d:
 * base + d, within CG_REP_SPAN): what another request of the same batch
 * already asks for, so that two gaps of one system ask for its free
 * unknowns once (FEC v3's wide windows tie many gaps together). */
static inline uint64_t cg_parq_overlap(uint32_t qbase, uint64_t qmap, uint32_t base)
{
	int32_t d = (int32_t)(qbase - base);

	if (d >= 0)
		return d < CG_REP_SPAN ? qmap << d : 0;
	return -d < CG_REP_SPAN ? qmap >> -d : 0;
}

/* How long an asked gap waits at the reorderer: rescue_us (rescue_wait_ms),
 * the code's horizon before it when a code is on, and never past TCP's
 * budget when TCP flows are seen (held longer, TCP's probes and timers
 * fire on a tunnel that only delays). Never 0 when rescue_us is not. */
static inline uint32_t cg_harq_wait_us(uint32_t rescue_us, uint32_t code_us, uint32_t budget_us)
{
	uint32_t w = rescue_us + code_us;

	if (!rescue_us)
		return 0;
	if (budget_us && w > budget_us)
		w = budget_us;
	return w ? w : 1;
}

#endif
