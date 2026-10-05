/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "test.h"
#include "wgwatch.h"

/* The start of a WireGuard datagram of type t, reserved bytes zero. */
static const uint8_t *wg(uint8_t t)
{
	static uint8_t b[256];

	memset(b, 0xab, sizeof(b));
	b[0] = t;
	b[1] = b[2] = b[3] = 0;
	return b;
}

void test_wgwatch(void)
{
	struct cg_wgw w;
	const uint64_t t = 1000000; /* ms */
	uint8_t odd[148];

	/* Handshake messages by type and their fixed sizes; data and anything
	 * else are 0. */
	CHECK_EQ(cg_wg_handshake(wg(1), 148), CG_WG_INITIATION);
	CHECK_EQ(cg_wg_handshake(wg(2), 92), CG_WG_RESPONSE);
	CHECK_EQ(cg_wg_handshake(wg(3), 64), CG_WG_COOKIE);
	CHECK_EQ(cg_wg_handshake(wg(4), 148), 0);
	CHECK_EQ(cg_wg_handshake(wg(4), 92), 0);
	CHECK_EQ(cg_wg_handshake(wg(1), 149), 0);
	CHECK_EQ(cg_wg_handshake(wg(1), 92), 0);
	CHECK_EQ(cg_wg_handshake(wg(2), 148), 0);
	CHECK_EQ(cg_wg_handshake(wg(3), 92), 0);
	memcpy(odd, wg(1), sizeof(odd));
	odd[2] = 1;
	CHECK_EQ(cg_wg_handshake(odd, 148), 0);

	/* A handshake that works: initiation, response. Never knocking. */
	memset(&w, 0, sizeof(w));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t);
	CHECK(cg_wgw_knocking(&w, t));
	cg_wgw_from_wireguard(&w, CG_WG_RESPONSE);
	CHECK(!cg_wgw_knocking(&w, t + 1));
	CHECK(!cg_wgw_poke(&w, t + 1));

	/* Data either way changes nothing. */
	cg_wgw_from_client(&w, 0, t + 2);
	cg_wgw_from_wireguard(&w, 0);
	CHECK_EQ(w.unanswered, 0);

	/* WireGuard ignores the client (its clock went back): it retries every
	 * 5 s, and the third unanswered one brings a poke, then one every 15 s
	 * at most while it knocks. */
	memset(&w, 0, sizeof(w));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t);
	CHECK(!cg_wgw_poke(&w, t));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t + 5000);
	CHECK(!cg_wgw_poke(&w, t + 5000));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t + 10000);
	CHECK_EQ(w.first_ms, t);
	CHECK(cg_wgw_poke(&w, t + 10000));
	CHECK(!cg_wgw_poke(&w, t + 10001));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t + 15000);
	CHECK(!cg_wgw_poke(&w, t + 24999));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t + 20000);
	CHECK(cg_wgw_poke(&w, t + 25000));

	/* The client gives up (or goes away): 30 s after its last initiation it
	 * no longer knocks, and nothing is poked. */
	CHECK(cg_wgw_knocking(&w, t + 50000));
	CHECK(!cg_wgw_knocking(&w, t + 50001));
	CHECK(!cg_wgw_poke(&w, t + 60000));

	/* It knocks again later: a new run, counted from one. */
	cg_wgw_from_client(&w, CG_WG_INITIATION, t + 90000);
	CHECK_EQ(w.unanswered, 1);
	CHECK_EQ(w.first_ms, t + 90000);
	CHECK(!cg_wgw_poke(&w, t + 90000));

	/* WireGuard's answer ends the run: a response, or a cookie under load. */
	memset(&w, 0, sizeof(w));
	for (int i = 0; i < 3; i++)
		cg_wgw_from_client(&w, CG_WG_INITIATION, t + 5000 * (uint64_t)i);
	cg_wgw_from_wireguard(&w, CG_WG_COOKIE);
	CHECK(!cg_wgw_knocking(&w, t + 10000));
	CHECK(!cg_wgw_poke(&w, t + 10000));

	/* The poke works: WireGuard starts a handshake of its own and the
	 * client's response ends the run, its initiation never answered. */
	memset(&w, 0, sizeof(w));
	for (int i = 0; i < 3; i++)
		cg_wgw_from_client(&w, CG_WG_INITIATION, t + 5000 * (uint64_t)i);
	CHECK(cg_wgw_poke(&w, t + 10000));
	cg_wgw_from_wireguard(&w, CG_WG_INITIATION);
	CHECK(cg_wgw_knocking(&w, t + 12000));
	cg_wgw_from_client(&w, CG_WG_RESPONSE, t + 12000);
	CHECK(!cg_wgw_knocking(&w, t + 12000));

	/* Redirect: the router restarted. Its old session fell silent before the
	 * new one knocked: WireGuard's initiation to the old one goes down the
	 * new one. An old session heard since, or none knocking: no. */
	memset(&w, 0, sizeof(w));
	CHECK(!cg_wgw_redirect(&w, t - 60000, t));
	cg_wgw_from_client(&w, CG_WG_INITIATION, t);
	CHECK(cg_wgw_redirect(&w, t - 60000, t + 1000));
	CHECK(!cg_wgw_redirect(&w, t, t + 1000));
	CHECK(!cg_wgw_redirect(&w, t + 500, t + 1000));
	CHECK(!cg_wgw_redirect(&w, t - 60000, t + CG_WGW_FRESH_MS + 1));
	cg_wgw_from_wireguard(&w, CG_WG_RESPONSE);
	CHECK(!cg_wgw_redirect(&w, t - 60000, t + 1000));
}
