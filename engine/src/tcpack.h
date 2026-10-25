/* A TCP retransmission's links from evidence (story 023, 2026-10-24).
 *
 * With tcp_aware in redundancy, the end that sends a flow's data into the
 * tunnel (the router for an upload, the server for a download) decides on
 * how many links each of TCP's retransmissions goes. It used to guess
 * (tcp_retx_one): the first retransmission of some bytes on one link, the
 * fastest, as "the original most likely arrived"; a repeat copied. This
 * header decides from what is known of the original instead:
 *
 *   the receiver already ACKed or SACKed it: a spurious     one link
 *     retransmission (an ACK covering it came back out
 *     of the tunnel before it went in)
 *   the tunnel lost it on every link that may bring it    copied on every
 *     (tunnel)                                              link it would go on
 *   the far host's SACK shows a hole there, but the       as tcp_retx_one:
 *     tunnel delivered it (lost beyond the tunnel), it      one link first,
 *     is still on its way on some link, or the tunnel       copied on a repeat
 *     does not know (delivered); or nothing known yet
 *     (unknown)
 *
 * A delivered one is not kept to one link on a repeat: one bit cannot tell
 * "delivered" from "on its way", and a repeat of what was on its way on a
 * slow link went again on the lossy fast one (the lab's protectcut: TCP
 * 12.3 -> 10.8 Mbit/s, story 023); repeats of losses beyond the tunnel are
 * few.
 *
 * The same end reads the flow's ACKs as they come out of the tunnel, with
 * the exit end's verdict on their holes (tcpfree.h's cg_tcpe_in, written as
 * CG_F_TUNHOLE on the ACK's DATA, proto.h): per flow the highest cumulative
 * ACK, a few recent SACK blocks, and the scoreboard range of the newest ACK
 * with SACK blocks, [its cumulative ACK, its highest SACK edge), with that
 * verdict. "No ACK yet" is not "lost": an ACK comes back a round trip after
 * the data, so a retransmission that nothing speaks of stays in the last
 * row. Without the peer's word on its marks (CG_BOND_TCPMARK, an older peer
 * or one without tcp_aware) a hole's verdict is not known either.
 *
 * Why a spurious retransmission is never dropped although the receiver has
 * those bytes: its arrival makes the receiver send a D-SACK (RFC 2883), and
 * that is how the sender learns it was spurious, undoes its window cut and,
 * with RACK (RFC 8985, 7.2), widens its reordering window so that the next
 * reordering does not fire it again. Dropping it would save one packet on
 * one link and keep TCP firing.
 *
 * Per flow a direct-mapped entry by the hash of the data direction (the
 * ACKs find it by their reversed tuple, flow.h's cg_flow_of_dir with rev);
 * a flow sharing it takes it over. Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TCPACK_H
#define CG_TCPACK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "flow.h"
#include "tcpfree.h"

#define CG_TCPA_SACK 4 /* SACK blocks kept per flow */

/* What is known of a retransmission's original (the rule's rows): nothing,
 * ACKed or SACKed already, not lost by the tunnel as far as it knows
 * (delivered, or still on its way), lost by the tunnel on every link. */
enum { CG_TCPA_UNKNOWN = 0, CG_TCPA_SPURIOUS, CG_TCPA_DELIVERED, CG_TCPA_TUNNEL, CG_TCPA_ROWS };

/* The exit end's verdict on the holes of [ev_lo, ev_hi). */
enum { CG_TCPA_EV_NONE = 0, CG_TCPA_EV_THROUGH, CG_TCPA_EV_TUNNEL };

struct cg_tcpa_flow {
	uint32_t key;
	uint32_t ack;                                /* the highest cumulative ACK (with ack_ok) */
	uint32_t sl[CG_TCPA_SACK], sr[CG_TCPA_SACK]; /* SACK blocks above ack; sl == sr: a free slot */
	uint32_t ev_lo, ev_hi;                       /* the newest scoreboard with SACK blocks */
	uint8_t used, ack_ok, ns, ev;                /* ns: the next slot to reuse; ev: CG_TCPA_EV_* */
};

struct cg_tcpa {
	struct cg_tcpa_flow f[CG_TCPF_N];
	uint64_t acks, sacked, marked; /* ACKs read; with SACK blocks; of them marked by the exit end */
};

/* Keeps the SACK block [l, r) in x: merged with those it overlaps or
 * touches, else in a free slot or the next one to reuse. */
static inline void cg_tcpa_block(struct cg_tcpa_flow *x, uint32_t l, uint32_t r)
{
	int free = -1;

	for (int i = 0; i < CG_TCPA_SACK; i++) {
		if (x->sl[i] == x->sr[i]) {
			if (free < 0)
				free = i;
			continue;
		}
		if (!cg_tseq_lt(r, x->sl[i]) && !cg_tseq_lt(x->sr[i], l)) { /* merged into it, freed */
			if (cg_tseq_lt(x->sl[i], l))
				l = x->sl[i];
			if (cg_tseq_lt(r, x->sr[i]))
				r = x->sr[i];
			x->sl[i] = x->sr[i] = 0;
			if (free < 0)
				free = i;
		}
	}
	if (free < 0)
		free = x->ns++ % CG_TCPA_SACK;
	x->sl[free] = l;
	x->sr[free] = r;
}

/* A TCP segment that came out of the tunnel at the end that sends its
 * flow's data (b, len bytes; r: its flow with rev, the data direction's
 * hash): its ACK and SACK blocks, and the exit end's mark (tunhole, the
 * header's CG_F_TUNHOLE) when the peer says it writes one (marks). */
static inline void cg_tcpa_ack(struct cg_tcpa *a, const struct cg_flow *r, const uint8_t *b, size_t len, int tunhole,
			       int marks)
{
	struct cg_tcpa_flow *x = &a->f[r->hash & (CG_TCPF_N - 1)];
	uint32_t seq, ack, dl, sl[CG_TCP_SACK_MAX], sr[CG_TCP_SACK_MAX];
	size_t doff;
	uint8_t fl;
	int n;

	if (cg_tcp_seg(r, b, len, &seq, &ack, &fl, &dl, &doff) < 0 || !(fl & 0x10) || (fl & CG_TCP_RST))
		return;
	a->acks++;
	if ((fl & CG_TCP_SYN) || !x->used || x->key != r->hash) { /* a new flow, or one taking the entry over */
		memset(x, 0, sizeof(*x));
		x->key = r->hash;
		x->used = 1;
	}
	if (!x->ack_ok || cg_tseq_lt(x->ack, ack)) {
		x->ack = ack;
		x->ack_ok = 1;
		for (int i = 0; i < CG_TCPA_SACK; i++) /* blocks the cumulative ACK passed */
			if (x->sl[i] != x->sr[i] && !cg_tseq_lt(ack, x->sr[i]))
				x->sl[i] = x->sr[i] = 0;
	}
	n = cg_tcp_sacks(r, b, doff, ack, sl, sr);
	if (!n) {
		if (x->ev && !cg_tseq_lt(x->ack, x->ev_hi)) /* every hole of it filled */
			x->ev = CG_TCPA_EV_NONE;
		return;
	}
	a->sacked++;
	for (int i = 0; i < n; i++)
		cg_tcpa_block(x, sl[i], sr[i]);
	x->ev_lo = ack;
	x->ev_hi = sr[0];
	for (int i = 1; i < n; i++)
		if (cg_tseq_lt(x->ev_hi, sr[i]))
			x->ev_hi = sr[i];
	x->ev = !marks ? CG_TCPA_EV_NONE : tunhole ? CG_TCPA_EV_TUNNEL : CG_TCPA_EV_THROUGH;
	a->marked += marks && tunhole;
}

/* What is known of the original of a retransmission [seq, end) of flow f
 * (the data direction's hash): CG_TCPA_*. */
static inline int cg_tcpa_row(const struct cg_tcpa *a, const struct cg_flow *f, uint32_t seq, uint32_t end)
{
	const struct cg_tcpa_flow *x = &a->f[f->hash & (CG_TCPF_N - 1)];

	if (!x->used || x->key != f->hash || !x->ack_ok)
		return CG_TCPA_UNKNOWN;
	if (!cg_tseq_lt(x->ack, end))
		return CG_TCPA_SPURIOUS;
	for (int i = 0; i < CG_TCPA_SACK; i++)
		if (x->sl[i] != x->sr[i] && !cg_tseq_lt(seq, x->sl[i]) && !cg_tseq_lt(x->sr[i], end))
			return CG_TCPA_SPURIOUS;
	if (cg_tseq_lt(seq, x->ack)) /* partly ACKed: what is left of it */
		seq = x->ack;
	if (x->ev != CG_TCPA_EV_NONE && !cg_tseq_lt(seq, x->ev_lo) && cg_tseq_lt(seq, x->ev_hi))
		return x->ev == CG_TCPA_EV_TUNNEL ? CG_TCPA_TUNNEL : CG_TCPA_DELIVERED;
	return CG_TCPA_UNKNOWN;
}

/* The row of the retransmission b (len bytes, flow f). */
static inline int cg_tcpa_retx(const struct cg_tcpa *a, const struct cg_flow *f, const uint8_t *b, size_t len)
{
	uint32_t seq, ack, dl;
	size_t doff;
	uint8_t fl;

	if (cg_tcp_seg(f, b, len, &seq, &ack, &fl, &dl, &doff) < 0)
		return CG_TCPA_UNKNOWN;
	return cg_tcpa_row(a, f, seq, seq + dl + (fl & CG_TCP_FIN ? 1 : 0));
}

/* Whether a retransmission of row (CG_TCPA_*) and class (tcpfree.h's
 * CG_TCPF_RETX1 or CG_TCPF_RETXN) goes copied on every link it would go
 * on (1) or on one link (0). */
static inline int cg_tcpa_copy(int row, int cls)
{
	switch (row) {
	case CG_TCPA_SPURIOUS:
		return 0;
	case CG_TCPA_TUNNEL:
		return 1;
	default: /* delivered (or on its way), unknown */
		return cls == CG_TCPF_RETXN;
	}
}

#endif
