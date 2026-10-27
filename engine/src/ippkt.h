/* IP packets in the native tunnel (tunnel = native): what cengarde checks
 * of a packet before it reaches a TUN device, and where its addresses are.
 *
 * Each DATA payload is a raw IP packet, read from a TUN
 * device at one end and written to the other end's. The MAC already says
 * who sent it; this only makes sure the kernel gets something shaped like
 * an IP packet: version 4 or 6, a header that fits, and a length field that
 * says exactly what was carried (a TUN read gives one whole packet, so
 * anything else is a bug or a forgery, never padding).
 *
 * Pure functions, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_IPPKT_H
#define CG_IPPKT_H

#include <stddef.h>
#include <stdint.h>

#define CG_IP4_HDR 20
#define CG_IP6_HDR 40

/* 4 or 6 when b (len bytes) is a well-formed IPv4 or IPv6 packet as far as
 * its fixed header goes: version, header length, total length == len. 0
 * otherwise. */
static inline int cg_ip_check(const uint8_t *b, size_t len)
{
	if (len < 1)
		return 0;
	switch (b[0] >> 4) {
	case 4: {
		size_t ihl = (size_t)(b[0] & 15) * 4;

		if (len < CG_IP4_HDR || ihl < CG_IP4_HDR || ihl > len)
			return 0;
		return ((size_t)b[2] << 8 | b[3]) == len ? 4 : 0;
	}
	case 6:
		if (len < CG_IP6_HDR)
			return 0;
		return CG_IP6_HDR + ((size_t)b[4] << 8 | b[5]) == len ? 6 : 0;
	}
	return 0;
}

/* The source and destination addresses of a packet cg_ip_check found to be
 * version ver (4 bytes each for 4, 16 for 6), inside b. */
static inline const uint8_t *cg_ip_src(const uint8_t *b, int ver)
{
	return b + (ver == 4 ? 12 : 8);
}

static inline const uint8_t *cg_ip_dst(const uint8_t *b, int ver)
{
	return b + (ver == 4 ? 16 : 24);
}

#endif
