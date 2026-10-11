/* What the tunnel carries, in one direction: WireGuard datagrams by message
 * type and, for data, by size. Keepalives, empty TCP ACKs and full packets
 * are worth a different number of copies, so story 012 (k-of-N, how many
 * copies ACKs and handshakes take) and the field recorder (bench/fieldrec.py)
 * need to know how much of each there is. Measurement only: nothing
 * decides on it.
 *
 * The type is the first byte, WireGuard's message type: 1 handshake
 * initiation, 2 response, 3 cookie reply, 4 data; anything else, or an
 * empty datagram, is other. Data goes in CG_WGS_N buckets of the datagram's
 * size (WireGuard's: its 16-byte header and 16-byte tag included), packets
 * and bytes, with inclusive upper edges
 *   32    a keepalive: header and tag only
 *   96    an empty TCP ACK over IPv4 (40 or 52 bytes, padded to 16)
 *   128, 192, 256, 384, 512, 768, 1024, 1280, 1440
 * and a last bucket above 1440. Every edge is a multiple of 32, so the
 * bucket is a table lookup by the size in 32-byte units: one increment of
 * each counter per datagram, no loop over the edges.
 *
 * Pure: no clock, no I/O, no log. tests/test_wgsize.c checks the table
 * against the edges.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_WGSIZE_H
#define CG_WGSIZE_H

#include <stddef.h>
#include <stdint.h>

#define CG_WGS_N 12      /* data size buckets: CG_WGS_N - 1 edges, then above the last */
#define CG_WGS_MAX 1440  /* the last edge */
#define CG_WGS_DATA 4    /* WireGuard's data message */

/* Indexes of msg[]: WireGuard's handshake types, 0 for anything else. */
enum { CG_WGS_OTHER = 0, CG_WGS_INIT = 1, CG_WGS_RESPONSE = 2, CG_WGS_COOKIE = 3 };

struct cg_wgs {
	uint64_t data[CG_WGS_N];       /* data datagrams by size bucket */
	uint64_t data_bytes[CG_WGS_N]; /* their bytes */
	uint64_t msg[4];               /* the rest, by type (above) */
};

/* The upper edge of bucket i, i < CG_WGS_N - 1. */
static inline uint32_t cg_wgs_edge(unsigned i)
{
	static const uint16_t edge[CG_WGS_N - 1] = { 32, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1440 };

	return edge[i];
}

/* The bucket of a datagram of len bytes: the first whose edge is len or
 * more, CG_WGS_N - 1 above the last edge. */
static inline unsigned cg_wgs_bucket(size_t len)
{
	/* By ceil(len / 32): the first edge of 32 x that or more. */
	static const uint8_t by32[CG_WGS_MAX / 32 + 1] = {
		0, 0,                   /* 0, 32 */
		1, 1,                   /* 96 */
		2,                      /* 128 */
		3, 3,                   /* 192 */
		4, 4,                   /* 256 */
		5, 5, 5, 5,             /* 384 */
		6, 6, 6, 6,             /* 512 */
		7, 7, 7, 7, 7, 7, 7, 7, /* 768 */
		8, 8, 8, 8, 8, 8, 8, 8, /* 1024 */
		9, 9, 9, 9, 9, 9, 9, 9, /* 1280 */
		10, 10, 10, 10, 10,     /* 1440 */
	};

	return len > CG_WGS_MAX ? CG_WGS_N - 1 : by32[(len + 31) / 32];
}

/* One datagram of len bytes at b. */
static inline void cg_wgs_add(struct cg_wgs *w, const uint8_t *b, size_t len)
{
	unsigned t = len ? b[0] : CG_WGS_OTHER;

	if (t == CG_WGS_DATA) {
		unsigned i = cg_wgs_bucket(len);

		w->data[i]++;
		w->data_bytes[i] += len;
	} else {
		w->msg[t < CG_WGS_DATA ? t : CG_WGS_OTHER]++;
	}
}

#endif
