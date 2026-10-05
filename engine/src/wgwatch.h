/* Server side: getting past a client whose clock went back.
 *
 * WireGuard drops a handshake initiation whose timestamp is not newer than
 * the last one it took from that peer (its replay protection). A router
 * without a battery-backed clock (a Raspberry Pi) starts with the date of
 * its image, or of the newest file in /etc, which is often behind the last
 * handshake the VPS took: WireGuard on the VPS then ignores every
 * initiation of it, and with all traffic through the tunnel the router
 * cannot reach NTP to fix its clock. The tunnel stays down until the
 * router's clock catches up, hours or months later (docs/historias/002).
 *
 * WireGuard on the VPS still starts handshakes of its own, and the router
 * takes them whatever its clock says. It starts one when it has something
 * to send and no session, or when what it sent got no answer for 15 s. So
 * the server watches the initiations it hands to WireGuard. After
 * CG_WGW_POKE_AFTER of them without an answer, the last one at most
 * CG_WGW_FRESH_MS ago, it pokes WireGuard: a datagram to the client's
 * address in the tunnel (wireguard_poke), at most every CG_WGW_POKE_MS.
 * WireGuard sends its handshake to the endpoint it knows, which after a
 * router restart is an older session: cg_wgw_redirect says when it goes
 * down the newest one instead.
 *
 * Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_WGWATCH_H
#define CG_WGWATCH_H

#include <stddef.h>
#include <stdint.h>

enum {
	CG_WG_INITIATION = 1,
	CG_WG_RESPONSE = 2,
	CG_WG_COOKIE = 3,
};

#define CG_WGW_POKE_AFTER 3   /* unanswered initiations: about 10 s of retries */
#define CG_WGW_POKE_MS 15000  /* between pokes */
#define CG_WGW_FRESH_MS 30000 /* older initiations: the client went away */

/* All zero: nothing seen. */
struct cg_wgw {
	uint32_t unanswered; /* initiations from the client WireGuard has not answered */
	uint64_t first_ms;   /* when the first of them came */
	uint64_t last_ms;    /* and the last */
	uint64_t poked_ms;   /* the last poke, 0: none yet */
};

/* The handshake message type of a WireGuard datagram (CG_WG_*), 0 for
 * anything else: data, or not WireGuard. Cheap enough for every packet. */
static inline int cg_wg_handshake(const uint8_t *b, size_t len)
{
	if (len != 148 && len != 92 && len != 64)
		return 0;
	if (b[1] || b[2] || b[3])
		return 0;
	if ((len == 148 && b[0] == CG_WG_INITIATION) || (len == 92 && b[0] == CG_WG_RESPONSE) ||
	    (len == 64 && b[0] == CG_WG_COOKIE))
		return b[0];
	return 0;
}

/* A datagram of handshake type `type` (0: none) from the client to
 * WireGuard. An initiation more than CG_WGW_FRESH_MS after the last one
 * starts a new run. A response answers a handshake WireGuard started. */
static inline void cg_wgw_from_client(struct cg_wgw *w, int type, uint64_t now_ms)
{
	if (type == CG_WG_INITIATION) {
		if (!w->unanswered || now_ms - w->last_ms > CG_WGW_FRESH_MS) {
			w->unanswered = 0;
			w->first_ms = now_ms;
		}
		w->unanswered++;
		w->last_ms = now_ms;
	} else if (type == CG_WG_RESPONSE) {
		w->unanswered = 0;
	}
}

/* A datagram of handshake type `type` from WireGuard to the client: a
 * response, or a cookie when it is under load, answers the client. */
static inline void cg_wgw_from_wireguard(struct cg_wgw *w, int type)
{
	if (type == CG_WG_RESPONSE || type == CG_WG_COOKIE)
		w->unanswered = 0;
}

/* Is the client knocking: initiations WireGuard has not answered, the last
 * one recent? */
static inline int cg_wgw_knocking(const struct cg_wgw *w, uint64_t now_ms)
{
	return w->unanswered && now_ms - w->last_ms <= CG_WGW_FRESH_MS;
}

/* Time to poke WireGuard? A yes counts as the poke. */
static inline int cg_wgw_poke(struct cg_wgw *w, uint64_t now_ms)
{
	if (w->unanswered < CG_WGW_POKE_AFTER || !cg_wgw_knocking(w, now_ms) ||
	    (w->poked_ms && now_ms - w->poked_ms < CG_WGW_POKE_MS))
		return 0;
	w->poked_ms = now_ms;
	return 1;
}

/* An initiation WireGuard sends to an older session of the client, last
 * heard at old_rx_ms: does it go down the newest session (w) instead? Only
 * while the newest one knocks, and the older one fell silent before that
 * started: the router restarted. An older session still heard keeps what
 * is sent to it. */
static inline int cg_wgw_redirect(const struct cg_wgw *w, uint64_t old_rx_ms, uint64_t now_ms)
{
	return cg_wgw_knocking(w, now_ms) && old_rx_ms < w->first_ms;
}

#endif
