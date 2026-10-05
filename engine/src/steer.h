/* Steering the server's listen sockets: a classic BPF program attached to
 * the SO_REUSEPORT group of the listen address (SO_ATTACH_REUSEPORT_CBPF).
 * The kernel runs it on every datagram for the port and delivers the
 * datagram to the socket whose index in the group it returns.
 *
 * The group is built once, in a fixed order: the lanes 0 to L - 1, then the
 * junk socket, index L. Lane l gets every datagram whose link id (byte 3)
 * is l modulo L, so the copies of one packet, one per client link, wait in
 * different receive queues: a stall that overflows one queue rarely takes
 * every copy of a packet, and each path has its own send buffer for the
 * download, since the server answers path p on lane p & (L - 1). The junk
 * socket gets what is not cengarde protocol 3: datagrams shorter than a
 * header, and a wrong version nibble (a client of another protocol
 * version). Nothing else ever reaches it, so a flood of garbage fills only
 * that socket's small buffer.
 *
 * The program sees the UDP payload at offset 0, and a load past its end
 * ends the program with 0 (lane 0), so the length is checked first. An
 * index past the group would make the kernel fall back to its hash, which
 * is why junk has a socket of its own.
 *
 * Protocol 4 adds the client hint (byte 2), which picks a group of lanes
 * per router; the program then grows one comparison per hint (design
 * PR 3d2). Here, protocol 3: one group.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_STEER_H
#define CG_STEER_H

#include <linux/filter.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#define CG_MAX_LANES 16
/* lanes = auto: a router's links 0 to 7 each on a queue of their own (one
 * has 5: four 5G modems and Starlink). An unused lane costs a socket. */
#define CG_LANES_AUTO 8
#define CG_STEER_MAX 16 /* instructions of the protocol 3 program */

/* lanes: 1, 2, 4, 8 or 16. */
static inline int cg_lanes_valid(unsigned lanes)
{
	return lanes >= 1 && lanes <= CG_MAX_LANES && !(lanes & (lanes - 1));
}

/* What the program computes, in C: the socket of the group for a datagram
 * of len bytes at p. The tests run the program against it. */
static inline unsigned cg_steer_ref(const uint8_t *p, size_t len, unsigned lanes)
{
	if (len < CG_HDR_LEN || p[0] >> 4 != CG_PROTO_VERSION)
		return lanes; /* junk */
	return p[CG_LINK_OFF] & (lanes - 1);
}

/* Writes the protocol 3 program into prog, which has room for max
 * instructions. Returns its length, or -1 when lanes is not valid or prog
 * too short. */
static inline int cg_steer_v3(struct sock_filter *prog, int max, unsigned lanes)
{
	/* Instruction indices, so that the jumps are computed, not counted. */
	enum { I_LEN, I_JLEN, I_VER, I_VMASK, I_JVER, I_LINK, I_LMASK, I_LANE, I_JUNK, I_N };
#define CG_STEER_TO(from, to) ((uint8_t)((to) - (from) - 1))
	const struct sock_filter p[I_N] = {
		[I_LEN] = BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
		[I_JLEN] = BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, CG_HDR_LEN, 0, CG_STEER_TO(I_JLEN, I_JUNK)),
		[I_VER] = BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
		[I_VMASK] = BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xf0),
		[I_JVER] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CG_PROTO_VERSION << 4, 0, CG_STEER_TO(I_JVER, I_JUNK)),
		[I_LINK] = BPF_STMT(BPF_LD | BPF_B | BPF_ABS, CG_LINK_OFF),
		[I_LMASK] = BPF_STMT(BPF_ALU | BPF_AND | BPF_K, lanes - 1),
		[I_LANE] = BPF_STMT(BPF_RET | BPF_A, 0),
		[I_JUNK] = BPF_STMT(BPF_RET | BPF_K, lanes),
	};
#undef CG_STEER_TO

	if (!cg_lanes_valid(lanes) || max < I_N)
		return -1;
	for (int i = 0; i < I_N; i++)
		prog[i] = p[i];
	return I_N;
}

#endif
