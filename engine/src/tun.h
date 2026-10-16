/* The TUN device of the native tunnel (tunnel = native).
 *
 * Each end owns one TUN device (IFF_TUN | IFF_NO_PI: raw IP packets, no
 * header in front), opened non-blocking when the engine starts and gone
 * when it stops: the kernel removes a TUN nobody holds. cengarde sets its
 * MTU, the addresses tun_addr lists and brings it up; routes through it
 * are the system's (ip route ... dev cg0), as for any other interface.
 *
 * A read gives one packet, a write takes one: a batch is a loop of them,
 * never waiting (EAGAIN ends it), as the UDP socket to WireGuard is read
 * with recvmmsg and written with sendmmsg.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_TUN_H
#define CG_TUN_H

#include <stddef.h>

#include "config.h"
#include "engine.h"

/* Opens cfg->tun with cfg->tun_mtu and cfg->tun_addr, up. Returns the
 * descriptor (non-blocking, close-on-exec) or -1 with err set. Needs
 * CAP_NET_ADMIN. */
int cg_tun_open(const struct cg_config *cfg, char *err, size_t errlen);

/* Reads up to CG_BATCH packets into b, as cg_rx does: msg_len is each
 * one's length and msg_flags 0 (MSG_TRUNC for one that filled CG_BUF, a
 * packet larger than any MTU cengarde sets). Returns the count, 0 when
 * there was nothing to read, or -1 on an error (errno set). */
int cg_tun_rx(int fd, struct cg_rxbatch *b);

/* Writes one packet. Returns 0, or -1 when it could not go (errno set;
 * EAGAIN: the device's queue is full, a drop like any other). */
int cg_tun_tx(int fd, const void *pkt, size_t len);

#endif
