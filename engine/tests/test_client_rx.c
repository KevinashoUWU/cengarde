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

/* A HELLO answering a probe sent with ts echo. */
static size_t hello(uint32_t echo, uint32_t cookie, uint8_t flags)
{
	struct cg_hello hl = { .echo_ts = echo, .cookie = cookie };
	struct cg_hdr h = { .type = CG_T_HELLO, .flags = flags, .session = SESSION, .seq = 0, .ts = 1 };

	cg_hello_write(pkt + CG_HDR_LEN, &hl);
	cg_hdr_write(pkt, &h, key + CG_SIPHASH_KEY_LEN, pkt + CG_HDR_LEN, CG_HELLO_LEN);
	return CG_HDR_LEN + CG_HELLO_LEN;
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

	/* 7. A probe reply: marked in the control window, back to the caller,
	 * no arrival; DATA's window does not move. */
	n = reply(101, 77);
	CHECK_EQ(in(&l2, 1, 7, n, 1002), CG_RXV_REPLY);
	CHECK_EQ(l2.last_rx_ms, 1002);
	CHECK_EQ(r.rx[1].wins, 0);
	CHECK_EQ(cg_replay_check(&r.ctl, 101), CG_RP_DUP);
	CHECK_EQ(cg_replay_check(&r.replay, 101), CG_RP_NEW);
	CHECK_EQ(r.replay.top, 100);
	/* A DUP reply is no arrival either. */
	CHECK_EQ(in(&l1, 0, 3, n, 1003), CG_RXV_DROP);
	CHECK_EQ(r.dups, 2);
	CHECK_EQ(r.rx[0].dups, 0);

	/* 5. OLD: far behind the window. */
	n = mk(CG_T_DATA, SESSION, 100 - CG_REPLAY_WINDOW - 5, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 9000), CG_RXV_DROP);
	CHECK_EQ(r.old, 1);
}

/* HELLO: no sequence, so it counts only for an unanswered probe of the
 * link it arrives on, once, and only with a MAC that verifies. */
static void test_hello(void)
{
	size_t n;

	reset_state();
	cg_echo_push(&l1.probes, 555, 10000);
	cg_echo_push(&l1.probes, 556, 10100);
	/* Forged: counted, no cookie, the probe still unanswered. */
	n = hello(555, 0xc00c1e, 0);
	pkt[CG_MAC_OFF] ^= 1;
	CHECK_EQ(in(&l1, 0, 3, n, 10200), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 1);
	CHECK_EQ(l1.cookie, 0);
	pkt[CG_MAC_OFF] ^= 1;
	/* On the wrong link: its ring has no such probe. */
	CHECK_EQ(in(&l2, 1, 7, n, 10200), CG_RXV_DROP);
	CHECK_EQ(r.hellos_stale, 1);
	CHECK_EQ(l2.cookie, 0);
	/* Genuine: the link keeps the cookie. */
	CHECK_EQ(in(&l1, 0, 3, n, 10200), CG_RXV_HELLO);
	CHECK_EQ(l1.cookie, 0xc00c1e);
	CHECK_EQ(l1.refused, 0);
	CHECK_EQ(l1.hello_ms, 10200);
	CHECK_EQ(r.hellos, 1);
	/* Sent again: its probe is answered already. */
	CHECK_EQ(in(&l1, 0, 3, n, 10300), CG_RXV_DROP);
	CHECK_EQ(r.hellos_stale, 2);
	/* A refusal is kept too; a HELLO marks no window. */
	n = hello(556, 0xbeef, CG_F_REFUSED);
	CHECK_EQ(in(&l1, 0, 3, n, 10300), CG_RXV_HELLO);
	CHECK_EQ(l1.cookie, 0xbeef);
	CHECK_EQ(l1.refused, 1);
	CHECK(!r.replay.init && !r.ctl.init);
	/* A reply to a probe a HELLO answered still passes (its sequence is
	 * what counts), and takes nothing. */
	n = reply(9, 556);
	CHECK_EQ(in(&l1, 0, 3, n, 10400), CG_RXV_REPLY);
}

/* DATA and control messages count apart: a link whose DATA lags beyond the
 * window still delivers its replies, and a server that lost the session
 * and goes on CG_SEQ_LEAP past the newest sequence needs no reset. */
static void test_classes(void)
{
	uint32_t top = 50000;
	size_t n;

	reset_state();
	n = mk(CG_T_DATA, SESSION, top, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 10000), CG_RXV_DATA);
	n = reply(7, 1);
	CHECK_EQ(in(&l1, 0, 3, n, 10001), CG_RXV_REPLY);
	/* The same sequence in the other class is new. */
	n = mk(CG_T_DATA, SESSION, 7, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 10002), CG_RXV_DROP); /* OLD for DATA */
	n = reply(top, 1);
	CHECK_EQ(in(&l1, 0, 3, n, 10003), CG_RXV_REPLY);
	CHECK_EQ(cg_rx_top(&r.replay), top);
	CHECK_EQ(cg_rx_top(&r.ctl), top);
	/* The server comes back past what the probes reported. */
	n = mk(CG_T_DATA, SESSION, top + CG_SEQ_LEAP, 1, 40);
	CHECK_EQ(in(&l2, 1, 7, n, 20000), CG_RXV_DATA);
	n = reply(top + CG_SEQ_LEAP, 1);
	CHECK_EQ(in(&l2, 1, 7, n, 20001), CG_RXV_REPLY);
	/* What the old server still had in flight is OLD now, never DATA again. */
	n = mk(CG_T_DATA, SESSION, top + 1, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 20002), CG_RXV_DROP);
	/* Nothing received: the probes say 0. */
	reset_state();
	CHECK_EQ(cg_rx_top(&r.replay), 0);
}

static void test_up(void)
{
	struct cg_txs t = { .session = SESSION, .seq = 0xfffffffeu, .seq_ctl = 40, .hint = 0x5a, .k_tx = key };
	uint8_t hdr[CG_HDR_LEN + CG_PROBE_INFO_LEN], payload[CG_PROBE_INFO_LEN] = "a probe's info";
	struct cg_hdr h;

	/* Each class has its own sequence; every header carries the hint. */
	CHECK_EQ(cg_up_header(&t, hdr, CG_T_DATA, 0, 0, 1234, payload, sizeof(payload)), 0xfffffffeu);
	CHECK_EQ(cg_up_header(&t, hdr, CG_T_DATA, 0, 0, 1234, payload, sizeof(payload)), 0xffffffffu);
	CHECK_EQ(cg_up_header(&t, hdr, CG_T_PROBE, CG_F_OWD, 3, 99, payload, sizeof(payload)), 40);
	CHECK_EQ(t.seq, 0);
	CHECK_EQ(t.seq_ctl, 41);
	memcpy(hdr + CG_HDR_LEN, payload, sizeof(payload));
	CHECK_EQ(cg_hdr_parse(&h, hdr, sizeof(hdr)), 0);
	CHECK(h.type == CG_T_PROBE && h.flags == CG_F_OWD && h.hint == 0x5a && h.link == 3 && h.session == SESSION &&
	      h.seq == 40 && h.ts == 99);
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
	test_hello();
	test_classes();
	test_up();
}
