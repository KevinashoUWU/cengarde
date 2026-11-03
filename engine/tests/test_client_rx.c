/* cg_rx_entry and cg_up_header (clientpath.h): the router's per-packet
 * decisions, the same in every link_threads mode, with a fake link and
 * window, and the arrival ring's expected links for a split packet.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "clientpath.h"
#include "repair.h"
#include "test.h"

#define SESSION 0x0badcafeu

static uint8_t key[CG_KEY_LEN];
static const uint8_t epoch[CG_EPOCH_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static struct cg_rxs r;
static struct cg_rxl l1, l2;
static struct cg_seal srv;             /* the server's download, sealing */
static uint8_t mem[CG_HDR_LEN + 256];  /* a packet in memory */
static uint8_t pkt[CG_HDR_LEN + 256 + CG_WIRE_EXTRA_MAX]; /* and on the wire */
static uint8_t rxb[sizeof(pkt)];       /* what a link reads: a copy of pkt */

/* A packet from the server: type, sequence, timestamp, payload length and
 * header flags, sealed (kind 1 with epoch1) with seal into pkt. */
static size_t mks(uint8_t type, uint32_t session, uint32_t seq, uint32_t ts, size_t plen, uint8_t flags,
		  struct cg_seal *seal, int epoch1)
{
	struct cg_hdr h = { .type = type, .flags = flags, .session = session, .seq = seq, .ts = ts, .link = 0 };

	cg_hdr_put(mem, &h);
	return cg_wire_seal(pkt, mem, mem + CG_HDR_LEN, plen, seal, epoch1);
}

static size_t mkf(uint8_t type, uint32_t session, uint32_t seq, uint32_t ts, size_t plen, uint8_t flags)
{
	for (size_t i = 0; i < plen; i++)
		mem[CG_HDR_LEN + i] = (uint8_t)(seq + i);
	return mks(type, session, seq, ts, plen, flags, &srv, 0);
}

static size_t mk(uint8_t type, uint32_t session, uint32_t seq, uint32_t ts, size_t plen)
{
	return mkf(type, session, seq, ts, plen, 0);
}

/* A HELLO answering a probe sent with ts echo: under an epoch of its own,
 * as the server seals it. */
static size_t hello(uint32_t echo, uint32_t cookie, uint8_t flags)
{
	static const uint8_t own[CG_EPOCH_LEN] = { 7, 7, 7, 7, 7, 7, 7, 7 };
	struct cg_hello hl = { .echo_ts = echo, .cookie = cookie };
	struct cg_seal hs;

	cg_seal_init(&hs, key, CG_DIR_S2C, SESSION, own);
	cg_hello_write(mem + CG_HDR_LEN, &hl);
	return mks(CG_T_HELLO, SESSION, 0, 1, CG_HELLO_LEN, flags, &hs, 1);
}

/* A probe reply answering a probe sent with ts echo (kind 1, seal's
 * epoch). */
static size_t reply_by(uint32_t seq, uint32_t echo, struct cg_seal *seal)
{
	struct cg_probe_info pi = { .echo_ts = echo };

	cg_probe_info_write(mem + CG_HDR_LEN, &pi);
	return mks(CG_T_PROBE_REPLY, SESSION, seq, 1, CG_PROBE_INFO_LEN, 0, seal, 1);
}

static size_t reply(uint32_t seq, uint32_t echo)
{
	return reply_by(seq, echo, &srv);
}

/* pkt (len bytes) read by link l: a copy of it, opened in place. */
static enum cg_rxv in(struct cg_rxl *l, unsigned link, uint8_t gen, size_t len, uint64_t now_ms)
{
	struct cg_hdr h;
	size_t n = len;

	memcpy(rxb, pkt, len);
	return cg_rx_entry(&r, l, link, gen, rxb, &n, 0, now_ms * 1000, 0x3, &h);
}

static void reset_state(void)
{
	memset(&r, 0, sizeof(r));
	memset(&l1, 0, sizeof(l1));
	memset(&l2, 0, sizeof(l2));
	r.session = SESSION;
	r.secret = key;
	cg_unseal_init(&r.un, key, CG_DIR_S2C, SESSION, epoch);
	cg_seal_init(&srv, key, CG_DIR_S2C, SESSION, epoch);
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
	memcpy(rxb, pkt, n);
	CHECK_EQ(cg_rx_entry(&r, &l1, 0, 3, rxb, &n, 1, 1000000, 0x3, &h), CG_RXV_DROP);
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

	/* 3. A forged packet fails its tag and marks nothing, so the genuine
	 * copy still gets through. */
	n = mk(CG_T_DATA, SESSION, 100, 1, 40);
	pkt[CG_HDR_LEN + 5] ^= 1;
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 1);
	CHECK(!r.replay.init);
	CHECK_EQ(l1.last_rx_ms, 0);
	pkt[CG_HDR_LEN + 5] ^= 1;
	/* 7. The first copy: marked, noted in the arrival ring, for the TUN,
	 * opened in place. */
	{
		size_t m = n;

		memcpy(rxb, pkt, n);
		CHECK_EQ(cg_rx_entry(&r, &l1, 0, 3, rxb, &m, 0, 1000000, 0x3, &h), CG_RXV_DATA);
		CHECK_EQ(m, CG_HDR_LEN + 40);
		CHECK(!memcmp(rxb + CG_HDR_LEN, mem + CG_HDR_LEN, 40));
		CHECK_EQ(r.un.top, srv.ctr - 1);
	}
	CHECK_EQ(h.seq, 100);
	CHECK_EQ(cg_replay_check(&r.replay, 100), CG_RP_DUP);
	CHECK_EQ(l1.last_rx_ms, 1000);
	CHECK_EQ(r.newest_ms, 1000);
	CHECK_EQ(r.rx[0].wins, 1);
	/* 5. The copy on the other link: its tag first (a broken one is
	 * forged, not a duplicate), then a duplicate, noted as such, its
	 * payload never decrypted. */
	pkt[CG_HDR_LEN + 5] ^= 1;
	CHECK_EQ(in(&l2, 1, 7, n, 1001), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 2);
	CHECK_EQ(r.dups, 0);
	pkt[CG_HDR_LEN + 5] ^= 1;
	CHECK_EQ(in(&l2, 1, 7, n, 1001), CG_RXV_DROP);
	CHECK_EQ(r.dups, 1);
	CHECK_EQ(r.rx[1].dups, 1);
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
 * link it arrives on, once, and only with a tag that opens; its epoch is
 * its own, never the download's key. */
static void test_hello(void)
{
	size_t n;

	reset_state();
	cg_echo_push(&l1.probes, 555, 10000);
	cg_echo_push(&l1.probes, 556, 10100);
	/* Forged: counted, no cookie, the probe still unanswered. */
	n = hello(555, 0xc00c1e, 0);
	pkt[n - 1] ^= 1;
	CHECK_EQ(in(&l1, 0, 3, n, 10200), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 1);
	CHECK_EQ(l1.cookie, 0);
	pkt[n - 1] ^= 1;
	/* On the wrong link: its ring has no such probe. */
	CHECK_EQ(in(&l2, 1, 7, n, 10200), CG_RXV_DROP);
	CHECK_EQ(r.hellos_stale, 1);
	CHECK_EQ(l2.cookie, 0);
	/* Genuine: the link keeps the cookie. */
	CHECK_EQ(in(&l1, 0, 3, n, 10200), CG_RXV_HELLO);
	CHECK_EQ(l1.cookie, 0xc00c1e);
	CHECK_EQ(l1.refused, 0);
	CHECK(!memcmp(r.un.epoch, epoch, CG_EPOCH_LEN)); /* the download's key stays */
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
	/* A repair of the download (story 026, protocol 11): a control
	 * message, verified, for the decoder; a PARQ too, as before. */
	n = mk(CG_T_REPAIR, SESSION, top + CG_SEQ_LEAP + 1, 1, CG_REP_HDR + 40);
	CHECK_EQ(in(&l2, 1, 7, n, 20003), CG_RXV_REPAIR);
	CHECK_EQ(in(&l2, 1, 7, n, 20004), CG_RXV_DROP); /* a copy: DUP in the control window */
	/* A STREAM (protocol 13): its own window, so the same number as a DATA
	 * is new; to the protocontroller, never to the arrival ring or the
	 * reorderer; a copy is a DUP of its own class, counted for the probe. */
	n = mk(CG_T_STREAM, SESSION, top + 1, 1, 40);
	CHECK_EQ(in(&l1, 0, 3, n, 20005), CG_RXV_STREAM);
	CHECK_EQ(cg_rx_top(&r.stream), top + 1);
	CHECK_EQ(cg_rx_top(&r.replay), top + CG_SEQ_LEAP); /* DATA's window untouched */
	CHECK_EQ(in(&l2, 1, 7, n, 20006), CG_RXV_DROP);
	CHECK_EQ(r.rx[0].stream, 1);
	CHECK_EQ(r.rx[1].stream, 1);
	/* Nothing received: the probes say 0. */
	reset_state();
	CHECK_EQ(cg_rx_top(&r.replay), 0);
}

/* A split packet (CG_F_SPLIT, split.h) is expected on the link it came on
 * only: the other links neither miss it nor are late for it. The flag is
 * under the tag. */
static void test_split_expect(void)
{
	const struct cg_arr_slot *a = &r.arr.slot[1000 % CG_ARR_SLOTS], *b = &r.arr.slot[1001 % CG_ARR_SLOTS];
	size_t n;

	reset_state();
	n = mkf(CG_T_DATA, SESSION, 1000, 1, 40, CG_F_SPLIT);
	CHECK_EQ(in(&l2, 1, 7, n, 1000), CG_RXV_DATA);
	CHECK_EQ(a->expect, 1u << 1);
	n = mk(CG_T_DATA, SESSION, 1001, 1, 40);
	CHECK_EQ(in(&l2, 1, 7, n, 1000), CG_RXV_DATA);
	CHECK_EQ(b->expect, 0x3); /* duplicated: every link of expect */
	n = mkf(CG_T_DATA, SESSION, 1002, 1, 40, CG_F_SPLIT);
	CHECK_EQ(in(&l1, 0, 3, n, 1000), CG_RXV_DATA);
	CHECK_EQ(r.arr.slot[1002 % CG_ARR_SLOTS].expect, 1u << 0);
	/* When the slots are reused, link 0 missed the duplicated 1001 only,
	 * and link 1 the split 1002 not at all. */
	for (uint32_t q = 1000; q < 1003; q++) {
		n = mk(CG_T_DATA, SESSION, q + CG_ARR_SLOTS, 1, 40);
		CHECK_EQ(in(&l1, 0, 3, n, 2000), CG_RXV_DATA);
	}
	CHECK_EQ(r.rx[0].missed, 1);
	CHECK_EQ(r.rx[1].missed, 0);
	/* Set on the way, the flag breaks the tag: never a forged split. */
	n = mk(CG_T_DATA, SESSION, 1003, 1, 40);
	pkt[CG_WIRE_HDR + 1] ^= CG_F_SPLIT;
	CHECK_EQ(in(&l1, 0, 3, n, 3000), CG_RXV_AUTH);
	CHECK_EQ(r.auth_fail, 1);
}

/* The server's epoch moves (it restarted, or took the session up again):
 * its first kind 1 that passes the window says the new one, and from it
 * the download opens under the new key only. An old reply sent again
 * opens under its old epoch but is OLD: it never takes the key back. */
static void rx_epoch(void)
{
	static const uint8_t e2[CG_EPOCH_LEN] = { 8, 8, 8, 8, 8, 8, 8, 8 };
	uint8_t old[sizeof(pkt)];
	struct cg_seal srv2;
	size_t n, nold;

	reset_state();
	nold = reply(5, 1);
	memcpy(old, pkt, nold);
	CHECK_EQ(in(&l1, 0, 3, nold, 1000), CG_RXV_REPLY);
	cg_seal_init(&srv2, key, CG_DIR_S2C, SESSION, e2);
	/* kind 0 under the new key before its reply: not opened */
	n = mks(CG_T_DATA, SESSION, 5 + CG_SEQ_LEAP, 1, 40, 0, &srv2, 0);
	CHECK_EQ(in(&l1, 0, 3, n, 1001), CG_RXV_AUTH);
	n = reply_by(5 + CG_SEQ_LEAP, 1, &srv2);
	CHECK_EQ(in(&l1, 0, 3, n, 1002), CG_RXV_REPLY);
	CHECK(!memcmp(r.un.epoch, e2, CG_EPOCH_LEN));
	n = mks(CG_T_DATA, SESSION, 6 + CG_SEQ_LEAP, 1, 40, 0, &srv2, 0);
	CHECK_EQ(in(&l1, 0, 3, n, 1003), CG_RXV_DATA);
	n = mks(CG_T_DATA, SESSION, 7 + CG_SEQ_LEAP, 1, 40, 0, &srv, 0);
	CHECK_EQ(in(&l1, 0, 3, n, 1004), CG_RXV_AUTH);
	memcpy(pkt, old, nold);
	CHECK_EQ(in(&l1, 0, 3, nold, 1005), CG_RXV_DROP);
	CHECK(!memcmp(r.un.epoch, e2, CG_EPOCH_LEN));
}

static void test_up(void)
{
	uint8_t ep[CG_EPOCH_LEN] = { 3 };
	struct cg_txs t = { .session = SESSION, .seq = 0xfffffffeu, .seq_ctl = 40, .hint = 0x5a };
	uint8_t hdr[CG_HDR_LEN + CG_PROBE_INFO_LEN], payload[CG_PROBE_INFO_LEN] = "a probe's info";
	uint8_t wire[sizeof(hdr) + CG_WIRE_EXTRA_MAX];
	struct cg_unseal u;
	struct cg_wire w;
	struct cg_hdr h;
	size_t n;

	cg_seal_init(&t.seal, key, CG_DIR_C2S, SESSION, ep);
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
	/* sealed for the link it holds when it goes: the server opens that one */
	cg_hdr_set_link(hdr, 9);
	n = cg_up_wire(&t, wire, hdr, hdr + CG_HDR_LEN, sizeof(payload), 1);
	CHECK_EQ(t.seal.ctr, 1);
	CHECK_EQ(cg_wire_peek(&w, wire, n), 0);
	CHECK(w.kind == CG_WIRE_KIND_EPOCH && w.link == 9 && w.hint == 0x5a && w.session == SESSION);
	cg_unseal_init(&u, key, CG_DIR_C2S, SESSION, w.epoch);
	CHECK_EQ(cg_wire_open_head(&h, wire, n, &w, u.key, CG_DIR_C2S, cg_wire_ctr(&w, 0)), 0);
	CHECK(h.link == 9 && h.seq == 40 && h.type == CG_T_PROBE);
	CHECK_EQ(cg_wire_open_body(wire, n, &h, &w, u.key, CG_DIR_C2S, 0), sizeof(hdr));
	CHECK(!memcmp(wire, hdr, sizeof(hdr)));
	/* the download's key does not open the upload */
	{
		uint8_t k2[CG_AEAD_KEY_LEN];

		n = cg_up_wire(&t, wire, hdr, hdr + CG_HDR_LEN, sizeof(payload), 1);
		CHECK_EQ(cg_wire_peek(&w, wire, n), 0);
		cg_wire_key(k2, key, CG_DIR_S2C, SESSION, w.epoch);
		CHECK(cg_wire_open_head(&h, wire, n, &w, k2, CG_DIR_C2S, 1) < 0);
	}
}

void test_client_rx(void)
{
	for (int i = 0; i < CG_KEY_LEN; i++)
		key[i] = (uint8_t)(i * 7 + 1);
	test_order();
	test_hello();
	test_classes();
	test_split_expect();
	rx_epoch();
	test_up();
}
