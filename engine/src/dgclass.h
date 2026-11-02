/* What kind of datagram a TUN packet carries, for the receiver's reorderer
 * (story 030): one rule for every UDP flow cannot fit them all.
 *
 *   CG_DG_TS     MPEG-TS straight over UDP: 188-byte packets, each starting
 *                with the sync byte 0x47 (7 to a datagram, 1316 bytes, is
 *                the usual). Its receiver has no reorder buffer of its own:
 *                a packet that comes late is as bad as a lost one, so a gap
 *                before it is worth waiting for, rescue included.
 *   CG_DG_RTP    RTP or RTCP (RFC 7983's demultiplexing: first byte 128 to
 *                191, version 2), with a payload type RTP uses (static 0-34,
 *                dynamic 96-127) or RTCP's (200-207), and the CSRC list
 *                inside. Its receiver has a jitter buffer that orders by the
 *                RTP sequence: held behind a gap, it only loses time; a
 *                rescued packet that comes late still takes its place there.
 *                RIST is RTP.
 *   CG_DG_OTHER  anything else, TCP included.
 *
 * MPEG-TS's first byte (0x47) is never RTP's, so the two do not overlap; MP2T
 * inside RTP (RFC 2250) is RTP. A guess on payload bytes: a datagram of
 * another protocol whose first byte and second look like RTP's is taken for
 * RTP and only skips the wait (its own protocol orders or not, as without
 * the tunnel).
 *
 * Pure: a few comparisons on the packet, no state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DGCLASS_H
#define CG_DGCLASS_H

#include <stddef.h>
#include <stdint.h>

#include "ippkt.h"

enum { CG_DG_OTHER = 0, CG_DG_TS, CG_DG_RTP };

#define CG_DG_TS_PKT 188
#define CG_DG_TS_SYNC 0x47
#define CG_DG_RTP_HDR 12

/* The UDP payload of IP packet b (len bytes): *off and *plen, 1; 0 when b is
 * not an unfragmented UDP datagram (IPv6 with no extension header). */
static inline int cg_dg_udp(const uint8_t *b, size_t len, size_t *off, size_t *plen)
{
	int ver = cg_ip_check(b, len);
	size_t o;

	if (ver == 4) {
		if (b[9] != 17 || (b[6] & 0x3f) || b[7])
			return 0;
		o = (size_t)(b[0] & 15) * 4;
	} else if (ver == 6) {
		if (b[6] != 17)
			return 0;
		o = CG_IP6_HDR;
	} else {
		return 0;
	}
	if (o + 8 > len)
		return 0;
	*off = o + 8;
	*plen = len - o - 8;
	return 1;
}

static inline int cg_dg_is_ts(const uint8_t *p, size_t n)
{
	if (n < CG_DG_TS_PKT || n % CG_DG_TS_PKT)
		return 0;
	for (size_t i = 0; i < n; i += CG_DG_TS_PKT)
		if (p[i] != CG_DG_TS_SYNC)
			return 0;
	return 1;
}

static inline int cg_dg_is_rtp(const uint8_t *p, size_t n)
{
	uint8_t pt;

	if (n < CG_DG_RTP_HDR || (p[0] & 0xc0) != 0x80)
		return 0;
	if (p[1] >= 200 && p[1] <= 207) /* RTCP: SR, RR, SDES, BYE, APP, feedback, XR */
		return 1;
	pt = p[1] & 0x7f;
	if (pt > 34 && pt < 96)
		return 0;
	return CG_DG_RTP_HDR + (size_t)(p[0] & 0x0f) * 4 <= n;
}

/* The class of IP packet b (len bytes). */
static inline int cg_dg_class(const uint8_t *b, size_t len)
{
	size_t off, n;

	if (!cg_dg_udp(b, len, &off, &n))
		return CG_DG_OTHER;
	if (cg_dg_is_ts(b + off, n))
		return CG_DG_TS;
	if (cg_dg_is_rtp(b + off, n))
		return CG_DG_RTP;
	return CG_DG_OTHER;
}

#endif
