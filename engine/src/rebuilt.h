/* What a rebuilt packet must show before it goes on (story 025's wrong
 * rebuilds, fixed in rlc.h; this is the guard behind any code: the XOR
 * parity of fec.h, the RS of rs.h, the RLC of rlc.h).
 *
 * A DATA the receiver gets came under the tunnel's MAC; one a code rebuilt
 * did not: the MAC covered the members and the repairs, not the sum the
 * decoder made of them. A decoder bug, or a member that was not what the
 * sender coded, gives bytes nobody sent. So, before a rebuilt payload is
 * written to the TUN, it is checked with what it carries:
 *
 *  - an IP packet: the shape of ippkt.h (version, header length,
 *    total length == what was rebuilt), then a checksum: TCP, UDP (a
 *    nonzero one), ICMP and ICMPv6 over the whole packet (with the pseudo
 *    header); IPv4 without one of those (a fragment, another protocol,
 *    UDP with checksum 0) by its header checksum. IPv6 without one (an
 *    extension header, a fragment, another protocol) cannot be confirmed.
 *  - a protocontroller message (protoctl.h): never good (since protocol
 *    13 one travels only as a STREAM, which no code covers; 14 dropped
 *    its tag).
 *
 * A random 16-bit sum passes by chance once in 65 536, after the length
 * prefix (rlc.h, rs.h: about one in 40 of 1400-byte garbage) and the IP
 * length field have passed. Costs one pass over a rebuilt packet (not over
 * each DATA): ~0.2 us for 1400 bytes.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_REBUILT_H
#define CG_REBUILT_H

#include <stddef.h>
#include <stdint.h>

#include "ippkt.h"
#include "protoctl.h"

enum {
	CG_RB_OK = 0,     /* confirmed: goes on */
	CG_RB_BAD = 1,    /* a check failed: garbage */
	CG_RB_UNSURE = 2, /* nothing to check it with (IPv6 without a known checksum) */
};

/* The ones' complement sum of n bytes at p onto s (not folded). */
static inline uint32_t cg_rb_sum(uint32_t s, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i + 1 < n; i += 2)
		s += (uint32_t)p[i] << 8 | p[i + 1];
	if (i < n)
		s += (uint32_t)p[i] << 8;
	return s;
}

static inline uint16_t cg_rb_fold(uint32_t s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (uint16_t)s;
}

/* Whether the transport part of an IP packet (l4, n bytes, protocol proto)
 * sums right, with the pseudo header of ver's addresses when it has one. */
static inline int cg_rb_l4_ok(int ver, const uint8_t *ip, uint8_t proto, const uint8_t *l4, size_t n)
{
	uint32_t s = 0;

	if (proto != 1) { /* ICMP sums alone; TCP, UDP and ICMPv6 with the pseudo header */
		size_t al = ver == 4 ? 4 : 16;

		s = cg_rb_sum(s, cg_ip_src(ip, ver), al);
		s = cg_rb_sum(s, cg_ip_dst(ip, ver), al);
		s += proto;
		s += (uint32_t)(n & 0xffff) + (uint32_t)(n >> 16);
	}
	return cg_rb_fold(cg_rb_sum(s, l4, n)) == 0xffff;
}

/* A rebuilt payload (p, len bytes): an IP packet or a stream message. */
static inline int cg_rb_native(const uint8_t *p, size_t len)
{
	int ver;

	/* a stream message travels only as a STREAM (13), which no code
	 * covers: one rebuilt is garbage */
	if (cg_pctl_is(p, len))
		return CG_RB_BAD;
	if (!(ver = cg_ip_check(p, len)))
		return CG_RB_BAD;
	if (ver == 4) {
		size_t ihl = (size_t)(p[0] & 15) * 4;
		uint8_t proto = p[9];
		int frag = ((p[6] & 0x3f) | p[7]) != 0; /* MF or an offset */
		const uint8_t *l4 = p + ihl;
		size_t n = len - ihl;

		if (cg_rb_fold(cg_rb_sum(0, p, ihl)) != 0xffff)
			return CG_RB_BAD;
		if (frag)
			return CG_RB_OK; /* the header's sum; the datagram's is the host's */
		if ((proto == 6 && n >= 20) || (proto == 1 && n >= 4))
			return cg_rb_l4_ok(4, p, proto, l4, n) ? CG_RB_OK : CG_RB_BAD;
		if (proto == 17 && n >= 8) {
			if (!(l4[6] | l4[7]))
				return CG_RB_OK; /* no UDP checksum: the header's */
			return cg_rb_l4_ok(4, p, proto, l4, n) ? CG_RB_OK : CG_RB_BAD;
		}
		if (proto == 6 || proto == 17 || proto == 1)
			return CG_RB_BAD; /* shorter than its header */
		return CG_RB_OK;
	} else {
		uint8_t nh = p[6];
		const uint8_t *l4 = p + CG_IP6_HDR;
		size_t n = len - CG_IP6_HDR;

		if ((nh == 6 && n >= 20) || (nh == 17 && n >= 8) || (nh == 58 && n >= 4))
			return cg_rb_l4_ok(6, p, nh, l4, n) ? CG_RB_OK : CG_RB_BAD;
		if (nh == 6 || nh == 17 || nh == 58)
			return CG_RB_BAD;
		return CG_RB_UNSURE;
	}
}

/* What a rebuilt payload (p, len bytes) shows: CG_RB_OK, CG_RB_BAD or
 * CG_RB_UNSURE. */
static inline int cg_rebuilt_check(const uint8_t *p, size_t len)
{
	return cg_rb_native(p, len);
}

#endif
