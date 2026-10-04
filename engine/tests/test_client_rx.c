/* cg_rx_entry and cg_up_header (clientpath.h): the router's per-packet
 * decisions, the same in every link_threads mode, with a fake link and
 * window.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "clientpath.h"
#include "test.h"

#define SESSION 0x0badcafeu

static uint8_t key[CG_KEY_LEN];
static struct cg_rxs r;
static struct cg_rxl l1, l2;
static uint8_t pkt[CG_HDR_LEN + 256];

/* A packet from the server: type, sequence, timestamp, payload length. */
static size_t mk(uint8_t type, uint32_t session, uint32_t seq, uint32_t ts, size_t plen)
{
	struct cg_hdr h = { .type = type, .session = session, .seq = seq, .ts = ts, .link = 0 };

	for (size_t i = 0; i < plen; i++)
		pkt[CG_HDR_LEN + i] = (uint8_t)(seq + i);
	cg_hdr_write(pkt, &h, key + CG_SIPHASH_KEY_LEN, pkt + CG_HDR_LEN, plen);
	return CG_HDR_LEN + plen;
}

/* A probe reply answering a probe sent with ts echo. */
static size_t reply(uint32_t seq, uint32_t echo)
{
	struct cg_probe_info pi = { .echo_ts = echo };
	struct cg_hdr h = { .type = CG_T_PROBE_REPLY, .session = SESSION, .seq = seq, .ts = 1 };

	cg_probe_info_write(pkt + CG_HDR_LEN, &pi);
	cg_hdr_write(pkt, &h, key + CG_SIPHASH_KEY_LEN, pkt + CG_HDR_LEN, CG_PROBE_INFO_LEN);
	return CG_HDR_LEN + CG_PROBE_INFO_LEN;
}

static enum cg_rxv in(struct cg_rxl *l, unsigned link, uint8_t gen, size_t len, uint64_t now_ms)
{
	struct cg_hdr h;

	return cg_rx_entry(&r, l, link, gen, pkt, len, 0, now_ms * 1000, 0x3, &h);
}

static void reset_state(void)
{
	memset(&r, 0, sizeof(r));
	memset(&l1, 0, sizeof(l1));
	memset(&l2, 0, sizeof(l2));
	r.session = SESSION;
	r.k_rx = key + CG_SIPHASH_KEY_LEN;
	r.restart_ms = 2000;
	l1.open = l2.open = 1;
	l1.gen = 3;
	l2.gen = 7;
}

static void test_order(void)
{
	struct cg_hdr h;
	size_t n;

	reset_state();
	/* 1. Generation and closed links, before anything else: even garbage
	 * counts as stale there, not as malformed. */
	n = mk(CG_T_DATA, SESSION, 100, 1, 40);
	CHECK_EQ(in(&l1, 0, 2, n, 1000), CG_RXV_DROP);
	CHECK_EQ(r.stale_gen, 1);
	memset(pkt, 0xff, 8);
	CHECK_EQ(in(&l1, 0, 2, 5, 1000), CG_RXV_DROP);
	CHECK_EQ(r.stale_gen, 2);
	CHECK_EQ(r.malformed, 0);
	n = mk(CG_T_DATA, SESSION, 100, 1, 40);
	l1.open = 0;
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_DROP);
	CHECK_EQ(r.stale_gen, 3);
	l1.open = 1;
	CHECK(!r.replay.init); /* nothing marked */

	/* 2. Truncated, malformed, a type the client does not take. */
	CHECK_EQ(cg_rx_entry(&r, &l1, 0, 3, pkt, n, 1, 1000000, 0x3, &h), CG_RXV_DROP);
	CHECK_EQ(r.trunc, 1);
	CHECK_EQ(in(&l1, 0, 3, CG_HDR_LEN - 1, 1000), CG_RXV_DROP);
	CHECK_EQ(r.malformed, 1);
	n = mk(CG_T_PROBE, SESSION, 100, 1, CG_PROBE_INFO_LEN);
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_DROP);
	CHECK_EQ(r.malformed, 2);

	/* 3. Another session. */
	n = mk(CG_T_DATA, SESSION + 1, 100, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_DROP);
	CHECK_EQ(r.foreign, 1);
	CHECK(!r.replay.init);

	/* 5-6. A forged NEW packet pays for the MAC, fails it and marks
	 * nothing, so the genuine copy still gets through. */
	n = mk(CG_T_DATA, SESSION, 100, 1, 40);
	pkt[CG_HDR_LEN + 5] ^= 1;
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 1);
	CHECK(!r.replay.init);
	CHECK_EQ(l1.last_rx_ms, 0);
	pkt[CG_HDR_LEN + 5] ^= 1;
	/* 7. The first copy: marked, noted in the arrival ring, for WireGuard. */
	CHECK_EQ(cg_rx_entry(&r, &l1, 0, 3, pkt, n, 0, 1000000, 0x3, &h), CG_RXV_DATA);
	CHECK_EQ(h.seq, 100);
	CHECK_EQ(cg_replay_check(&r.replay, 100), CG_RP_DUP);
	CHECK_EQ(l1.last_rx_ms, 1000);
	CHECK_EQ(r.newest_ms, 1000);
	CHECK_EQ(r.rx[0].wins, 1);
	/* 4. The copy on the other link: a duplicate, before any MAC (a broken
	 * MAC changes nothing), noted as such. */
	pkt[CG_HDR_LEN + 5] ^= 1;
	CHECK_EQ(in(&l2, 1, 7, n, 1001), CG_RXV_DROP);
	CHECK_EQ(r.dups, 1);
	CHECK_EQ(r.rx[1].dups, 1);
	CHECK_EQ(r.auth_fail, 1);
	CHECK_EQ(l2.last_rx_ms, 0);

	/* 7. A probe reply: marked, back to the caller, no arrival. */
	n = reply(101, 77);
	CHECK_EQ(in(&l2, 1, 7, n, 1002), CG_RXV_REPLY);
	CHECK_EQ(l2.last_rx_ms, 1002);
	CHECK_EQ(r.rx[1].wins, 0);
	CHECK_EQ(cg_replay_check(&r.replay, 101), CG_RP_DUP);
	/* A DUP reply is no arrival either. */
	CHECK_EQ(in(&l1, 0, 3, n, 1003), CG_RXV_DROP);
	CHECK_EQ(r.dups, 2);
	CHECK_EQ(r.rx[0].dups, 0);

	/* 4. OLD: far behind the window, and not a reply that shows a restart. */
	n = mk(CG_T_DATA, SESSION, 101 - CG_REPLAY_WINDOW - 5, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 9000), CG_RXV_DROP);
	CHECK_EQ(r.old, 1);
	CHECK_EQ(r.window_resets, 0);
}

static void test_restart(void)
{
	uint32_t top = 50000, old = top - CG_REPLAY_WINDOW - 100;
	size_t n;

	reset_state();
	n = mk(CG_T_DATA, SESSION, top, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 10000), CG_RXV_DATA);
	cg_echo_push(&l1.probes, 555, 10500);

	/* Not yet 2 s of silence since the newest packet: OLD. */
	n = reply(old, 555);
	CHECK_EQ(in(&l1, 0, 3, n, 11999), CG_RXV_DROP);
	CHECK_EQ(r.old, 1);
	/* The echo was taken by that verified reply: a later one with the same
	 * echo proves nothing. */
	CHECK_EQ(in(&l1, 0, 3, n, 12500), CG_RXV_DROP);
	CHECK_EQ(r.old, 2);
	CHECK_EQ(r.window_resets, 0);

	/* A forged reply never consumes the echo. */
	cg_echo_push(&l1.probes, 556, 12000);
	n = reply(old + 1, 556);
	pkt[CG_MAC_OFF] ^= 1;
	CHECK_EQ(in(&l1, 0, 3, n, 12600), CG_RXV_DROP);
	CHECK_EQ(r.old, 3);
	pkt[CG_MAC_OFF] ^= 1;
	/* Genuine, with silence: the window starts again from it. */
	CHECK_EQ(in(&l1, 0, 3, n, 12600), CG_RXV_RESET);
	CHECK_EQ(r.window_resets, 1);
	CHECK_EQ(r.replay.top, old + 1);
	CHECK_EQ(cg_replay_check(&r.replay, old + 2), CG_RP_NEW);
	CHECK_EQ(r.newest_ms, 12600);
	/* A DATA packet never resets it. */
	n = mk(CG_T_DATA, SESSION, old + 1 - CG_REPLAY_WINDOW - 10, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 20000), CG_RXV_DROP);
	CHECK_EQ(r.window_resets, 1);

	/* Out of order between pumps: a packet stamped before the newest one
	 * already taken shows no silence at all. */
	reset_state();
	n = mk(CG_T_DATA, SESSION, top, 1, 40);
	CHECK_EQ(in(&l2, 1, 7, n, 30000), CG_RXV_DATA);
	cg_echo_push(&l1.probes, 700, 9000);
	n = reply(old, 700);
	CHECK_EQ(in(&l1, 0, 3, n, 9500), CG_RXV_DROP);
	CHECK_EQ(r.window_resets, 0);
	CHECK_EQ(r.newest_ms, 30000);
}

static void test_up(void)
{
	struct cg_txs t = { .session = SESSION, .seq = 0xfffffffeu, .k_tx = key };
	uint8_t hdr[CG_HDR_LEN + CG_PROBE_INFO_LEN], payload[CG_PROBE_INFO_LEN] = "a probe's info";
	struct cg_hdr h;

	CHECK_EQ(cg_up_header(&t, hdr, CG_T_DATA, 0, 0, 1234, payload, sizeof(payload)), 0xfffffffeu);
	CHECK_EQ(cg_up_header(&t, hdr, CG_T_PROBE, CG_F_OWD, 3, 99, payload, sizeof(payload)), 0xffffffffu);
	CHECK_EQ(t.seq, 0);
	memcpy(hdr + CG_HDR_LEN, payload, sizeof(payload));
	CHECK_EQ(cg_hdr_parse(&h, hdr, sizeof(hdr)), 0);
	CHECK(h.type == CG_T_PROBE && h.flags == CG_F_OWD && h.link == 3 && h.session == SESSION &&
	      h.seq == 0xffffffffu && h.ts == 99);
	CHECK(cg_hdr_verify(hdr, sizeof(hdr), key));
	cg_hdr_set_link(hdr, 9); /* patched per link, outside the MAC */
	CHECK(cg_hdr_verify(hdr, sizeof(hdr), key));
	CHECK(!cg_hdr_verify(hdr, sizeof(hdr), key + CG_SIPHASH_KEY_LEN));
}

void test_client_rx(void)
{
	for (int i = 0; i < CG_KEY_LEN; i++)
		key[i] = (uint8_t)(i * 7 + 1);
	test_order();
	test_restart();
	test_up();
}
