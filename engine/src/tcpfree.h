/* TCP-aware release (story 014, phase C): with tunnel = native the sender
 * sees inner TCP, and marks CG_F_FREE its segments without data (pure ACKs,
 * SYN, FIN, RST), which the receiver's reorderer hands on at once, and its
 * retransmissions. A retransmission was handed on at once too, at first:
 * held behind the gap of the packet it repairs, it waited for the tunnel
 * to give that gap up (on the testnet TCP repaired in ~30 ms and its
 * retransmission waited 500 ms, reorder_max_wait_ms). But ahead of what
 * was held, sent before it, it made TCP take all of that for lost and send
 * it again (story 014, 2026-10-16): the receiver now tells it apart
 * (cg_tcp_data) and keeps it in sequence (reorder.h).
 *
 * Per flow (flow.h's hash, a direct-mapped table of CG_TCPF_N entries, no
 * allocation per packet) the highest sequence sent, its end: a segment that
 * ends at or below it is sent again. A flow sharing an entry takes it over
 * (it is new to the table, nothing is marked); SYN starts a flow again.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TCPFREE_H
#define CG_TCPFREE_H

#include <stddef.h>
#include <stdint.h>

#include "flow.h"

#define CG_TCPF_N 4096 /* flows tracked: a power of two */

_Static_assert(!(CG_TCPF_N & (CG_TCPF_N - 1)), "CG_TCPF_N is a power of two");

#define CG_TCP_FIN 0x01
#define CG_TCP_SYN 0x02
#define CG_TCP_RST 0x04

struct cg_tcpf {
	uint32_t key[CG_TCPF_N]; /* the flow's hash */
	uint32_t hi[CG_TCPF_N];  /* the end (seq + length) of its highest segment */
	uint8_t used[CG_TCPF_N];
	uint64_t ctl, retx; /* marked: without data (ACK, SYN, FIN, RST); sent again */
};

/* Whether the IP packet b (len bytes, flow f from cg_flow_of) is a TCP
 * segment to mark CG_F_FREE; it updates the flow's highest end. */
static inline int cg_tcpf_free(struct cg_tcpf *t, const struct cg_flow *f, const uint8_t *b, size_t len)
{
	size_t off = f->l4off, doff;
	uint32_t seq, end, i;
	uint8_t fl;

	if (!f->tcp || f->frag || off + 20 > len)
		return 0;
	doff = (size_t)(b[off + 12] >> 4) * 4;
	if (doff < 20 || off + doff > len)
		return 0;
	seq = (uint32_t)b[off + 4] << 24 | (uint32_t)b[off + 5] << 16 | (uint32_t)b[off + 6] << 8 | b[off + 7];
	fl = b[off + 13];
	end = seq + (uint32_t)(len - off - doff) + (fl & CG_TCP_SYN ? 1 : 0) + (fl & CG_TCP_FIN ? 1 : 0);
	i = f->hash & (CG_TCPF_N - 1);
	if (fl & (CG_TCP_SYN | CG_TCP_RST)) {
		t->key[i] = f->hash;
		t->hi[i] = end;
		t->used[i] = !(fl & CG_TCP_RST);
		t->ctl++;
		return 1;
	}
	if (len == off + doff && !(fl & CG_TCP_FIN)) { /* a pure ACK (or a window update) */
		t->ctl++;
		return 1;
	}
	if (!t->used[i] || t->key[i] != f->hash) {
		t->key[i] = f->hash;
		t->hi[i] = end;
		t->used[i] = 1;
		return 0;
	}
	if ((int32_t)(end - t->hi[i]) <= 0) {
		t->retx++;
		return 1;
	}
	t->hi[i] = end;
	return 0;
}

/* At the receiver, whether a packet marked CG_F_FREE is one of TCP's
 * retransmissions rather than a segment without data: IP packet b (len
 * bytes) is a TCP segment, not a fragment, without SYN or RST, that
 * carries data. No state, and no change to the protocol: what was marked
 * for which reason shows in the segment itself. A fragment, a short or
 * malformed header, or anything not TCP reads as not one (as a free ACK:
 * delivered at once and nothing more). */
static inline int cg_tcp_data(const uint8_t *b, size_t len)
{
	struct cg_flow f;
	size_t off, doff;

	if (!cg_flow_of(b, len, 0, &f) || !f.tcp || f.frag)
		return 0;
	off = f.l4off;
	if (off + 20 > len)
		return 0;
	doff = (size_t)(b[off + 12] >> 4) * 4;
	if (doff < 20 || off + doff >= len)
		return 0;
	return !(b[off + 13] & (CG_TCP_SYN | CG_TCP_RST));
}

#endif
