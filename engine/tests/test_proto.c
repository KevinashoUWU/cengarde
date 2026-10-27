/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "fec.h"
#include "repair.h"
#include "proto.h"
#include "test.h"

void test_proto(void)
{
	uint8_t key_a[16], key_b[16], pkt[CG_HDR_LEN + 64], payload[64];
	struct cg_hdr h = { .type = CG_T_DATA, .flags = 0, .hint = 0x5a, .link = 5, .session = 0xdeadbeef,
			    .seq = 0x01020304, .ts = 0xa0b0c0d0 },
		      p;
	struct cg_probe_info pi = { 1, 0x80000001, 100, 2, 3, 0xfffffffe, 0xc00c1e, 0x11223344, 0x55667788, 0x99aabbcc },
			     po;
	struct cg_hello hl = { 0x01020304, 0xcafef00d }, ho;
	uint8_t pib[CG_PROBE_INFO_LEN + 8], hb[CG_HELLO_LEN];

	for (int i = 0; i < 16; i++) {
		key_a[i] = (uint8_t)i;
		key_b[i] = (uint8_t)(i + 100);
	}
	for (int i = 0; i < 64; i++)
		payload[i] = (uint8_t)(i * 3);

	/* The previous link in DATA's flags (version 5): every link id, beside
	 * the split and ECN bits, and none when not said. */
	for (int l = 0; l < 16; l++) {
		uint8_t f = (uint8_t)(cg_prev_flags(l) | CG_F_SPLIT | 0x03);

		CHECK_EQ(cg_prev_link(f), l);
		CHECK((f & CG_F_SPLIT) && (f & 0x03) == 0x03);
	}
	CHECK_EQ(cg_prev_flags(-1), 0);
	CHECK_EQ(cg_prev_flags(16), 0);
	CHECK_EQ(cg_prev_link(CG_F_SPLIT | 0x03), -1);

	cg_hdr_write(pkt, &h, key_a, payload, sizeof(payload));
	memcpy(pkt + CG_HDR_LEN, payload, sizeof(payload));

	/* Layout: version/type, the client hint at byte 2, link at byte 3,
	 * session, seq and ts in network order. */
	CHECK_EQ(pkt[0], 0xb1);
	CHECK_EQ(pkt[2], 0x5a);
	CHECK_EQ(pkt[3], 5);
	CHECK(!memcmp(pkt + 4, "\xde\xad\xbe\xef\x01\x02\x03\x04\xa0\xb0\xc0\xd0", 12));

	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), 0);
	CHECK_EQ(p.type, CG_T_DATA);
	CHECK_EQ(p.hint, 0x5a);
	CHECK_EQ(p.link, 5);
	CHECK_EQ(p.session, 0xdeadbeef);
	CHECK_EQ(p.seq, 0x01020304);
	CHECK_EQ(p.ts, 0xa0b0c0d0);
	CHECK(cg_hdr_verify(pkt, sizeof(pkt), key_a));

	/* The other direction's key does not verify. */
	CHECK(!cg_hdr_verify(pkt, sizeof(pkt), key_b));

	/* The link id is not authenticated: patching it keeps the MAC valid. */
	cg_hdr_set_link(pkt, 9);
	CHECK(cg_hdr_verify(pkt, sizeof(pkt), key_a));

	/* Every other header byte and every payload byte is authenticated. */
	for (size_t i = 0; i < sizeof(pkt); i++) {
		if (i == CG_LINK_OFF)
			continue;
		pkt[i] ^= 0x01;
		CHECK(!cg_hdr_verify(pkt, sizeof(pkt), key_a));
		pkt[i] ^= 0x01;
	}
	/* Truncation is detected. */
	CHECK(!cg_hdr_verify(pkt, sizeof(pkt) - 1, key_a));

	/* Malformed headers. */
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN), -1); /* DATA without payload */
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN - 1), -1);
	pkt[0] = 0x11; /* version 1 (no delay reports) is refused */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x21; /* version 2 (no IP pass flags) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x31; /* version 3 (no hint, cookies or control sequence) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x41; /* version 4 (no previous link in DATA's flags) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x51; /* version 5 (no CG_F_FREE) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x61; /* version 6 (no FEC parity) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x71; /* version 7 (no repairs) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x81; /* version 8 (no parity asked for again) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x91; /* version 9 (no protocontroller streams) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0xa1; /* version 10 (no wide windows, FEC v3) too */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0xbf; /* unknown type */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0xb6; /* a parity (fec.h): its header and a byte at least */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), sizeof(pkt) > CG_HDR_LEN + CG_FEC_HDR ? 0 : -1);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_FEC_HDR), -1);
	pkt[0] = 0xb7; /* a repair (repair.h): its header and a byte at least */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), sizeof(pkt) > CG_HDR_LEN + CG_REP_HDR ? 0 : -1);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_REP_HDR), -1);
	CHECK(cg_type_ctl(CG_T_REPAIR));
	CHECK(cg_type_ctl(CG_T_PARQ));
	{
		/* a PARQ (harq.h): a count, and exactly that many requests */
		uint8_t q[CG_HDR_LEN + CG_PARQ_LEN];

		memcpy(q, pkt, CG_HDR_LEN);
		q[0] = 0xb8;
		memset(q + CG_HDR_LEN, 0, sizeof(q) - CG_HDR_LEN);
		q[CG_HDR_LEN] = 2;
		CHECK_EQ(cg_hdr_parse(&p, q, CG_HDR_LEN + 1 + 2 * CG_PARQ_REQ), 0);
		CHECK_EQ(cg_hdr_parse(&p, q, CG_HDR_LEN + 1 + 2 * CG_PARQ_REQ + 1), -1);
		CHECK_EQ(cg_hdr_parse(&p, q, CG_HDR_LEN + 1 + CG_PARQ_REQ), -1);
		q[CG_HDR_LEN] = 0;
		CHECK_EQ(cg_hdr_parse(&p, q, CG_HDR_LEN + 1 + CG_PARQ_REQ), -1);
		q[CG_HDR_LEN] = CG_PARQ_MAX + 1;
		CHECK_EQ(cg_hdr_parse(&p, q, sizeof(q)), -1);
		q[CG_HDR_LEN] = CG_PARQ_MAX;
		CHECK_EQ(cg_hdr_parse(&p, q, sizeof(q)), 0);
	}
	pkt[0] = 0xb1;
	pkt[2] = 0xff; /* any hint parses: the server judges it */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), 0);
	CHECK_EQ(p.hint, 0xff);
	pkt[2] = 0x5a;

	/* The hint is authenticated, unlike the link id. */
	CHECK(cg_hdr_verify(pkt, sizeof(pkt), key_a));
	pkt[CG_HINT_OFF] ^= 1;
	CHECK(!cg_hdr_verify(pkt, sizeof(pkt), key_a));
	pkt[CG_HINT_OFF] ^= 1;

	/* Probes carry at least one cg_probe_info, and a longer payload parses
	 * (its tail is for later fields); flags are authenticated. */
	h.type = CG_T_PROBE;
	h.flags = CG_F_OWD | CG_F_MUTED;
	memset(pib, 0xee, sizeof(pib));
	cg_probe_info_write(pib, &pi);
	cg_hdr_write(pkt, &h, key_a, pib, CG_PROBE_INFO_LEN);
	memcpy(pkt + CG_HDR_LEN, pib, sizeof(pib));
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN), 0);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN + 8), 0);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN - 1), -1);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + 24), -1); /* a version 3 probe */
	CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN + 8, key_a)); /* the tail is under the MAC */
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN), 0);
	CHECK_EQ(p.flags, CG_F_OWD | CG_F_MUTED);
	pkt[1] ^= CG_F_MUTED;
	CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	pkt[1] ^= CG_F_MUTED;
	cg_probe_info_read(&po, pkt + CG_HDR_LEN);
	CHECK(po.echo_ts == 1 && po.owd == 0x80000001 && po.interval_ms == 100 && po.rx == 2 && po.wins == 3 &&
	      po.lag_us == 0xfffffffe && po.cookie == 0xc00c1e && po.rx_top == 0x11223344 &&
	      po.rx_top_ctl == 0x55667788 && po.tx_next == 0x99aabbcc);
	CHECK(!memcmp(pkt + CG_HDR_LEN, "\x00\x00\x00\x01\x80\x00\x00\x01\x00\x00\x00\x64", 12));
	CHECK(!memcmp(pkt + CG_HDR_LEN + 24, "\x00\xc0\x0c\x1e\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc", 16));

	/* HELLO: echo and cookie, at least 8 bytes. */
	h.type = CG_T_HELLO;
	h.flags = CG_F_REFUSED;
	h.seq = 0;
	cg_hello_write(hb, &hl);
	cg_hdr_write(pkt, &h, key_a, hb, sizeof(hb));
	memcpy(pkt + CG_HDR_LEN, hb, sizeof(hb));
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_HELLO_LEN), 0);
	CHECK_EQ(p.type, CG_T_HELLO);
	CHECK_EQ(p.flags, CG_F_REFUSED);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_HELLO_LEN - 1), -1);
	CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_HELLO_LEN, key_a));
	cg_hello_read(&ho, pkt + CG_HDR_LEN);
	CHECK(ho.echo_ts == 0x01020304 && ho.cookie == 0xcafef00d);

	/* Sequence classes: probes and replies count apart from DATA. */
	CHECK(cg_type_ctl(CG_T_PROBE) && cg_type_ctl(CG_T_PROBE_REPLY));
	CHECK(!cg_type_ctl(CG_T_DATA) && !cg_type_ctl(CG_T_HELLO));
	h.type = CG_T_PROBE;
	h.seq = 0x01020304;

	/* IP pass in the flags: three states, the rest of the flags untouched. */
	CHECK_EQ(cg_pass_flags(-1), 0);
	CHECK_EQ(cg_pass_flags(0), CG_F_PASS_SET);
	CHECK_EQ(cg_pass_flags(1), CG_F_PASS_SET | CG_F_PASS);
	CHECK_EQ(cg_pass_get(0), -1);
	CHECK_EQ(cg_pass_get(CG_F_PASS), -1); /* without PASS_SET it says nothing */
	CHECK_EQ(cg_pass_get(CG_F_OWD | CG_F_MUTED | cg_pass_flags(0)), 0);
	CHECK_EQ(cg_pass_get(CG_F_OWD | cg_pass_flags(1)), 1);
	CHECK_EQ(cg_pass_flags(1) & (CG_F_OWD | CG_F_MUTED), 0);
	h.flags = (uint8_t)(CG_F_OWD | cg_pass_flags(1));
	cg_hdr_write(pkt, &h, key_a, pib, CG_PROBE_INFO_LEN);
	memcpy(pkt + CG_HDR_LEN, pib, CG_PROBE_INFO_LEN);
	CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	pkt[1] ^= CG_F_PASS; /* nobody on the way can turn it off */
	CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));

	/* The bonding tail (proto.h): 4 bytes after cg_probe_info, the same
	 * version; a 40-byte peer has none, and what a reader does not know is
	 * ignored. */
	{
		struct cg_probe_bond b = { .weight = 1000, .reorders = 1, .split_me = 1 }, r;
		uint8_t tp[CG_PROBE_BOND_LEN + 8];

		CHECK_EQ(CG_PROBE_BOND_LEN, 44);
		memset(tp, 0xee, sizeof(tp));
		cg_probe_info_write(tp, &pi);
		cg_probe_bond_write(tp, &b);
		CHECK(!memcmp(tp + CG_PROBE_INFO_LEN, "\x00\x03\x03\xe8", 4));
		CHECK_EQ(tp[CG_PROBE_BOND_LEN], 0xee); /* nothing written after it */
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(r.weight == 1000 && r.reorders == 1 && r.split_me == 1);
		cg_probe_info_read(&po, tp); /* the info before it is untouched */
		CHECK(po.echo_ts == 1 && po.tx_next == 0x99aabbcc);
		/* Each bit on its own, and weights at the ends of the range. */
		for (unsigned k = 0; k < 4; k++) {
			b = (struct cg_probe_bond){ .weight = k == 3 ? 0 : (uint16_t)(k + 1), .reorders = k == 1, .split_me = k == 2 };
			cg_probe_bond_write(tp, &b);
			CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
			CHECK(r.weight == b.weight && r.reorders == b.reorders && r.split_me == b.split_me);
		}
		b = (struct cg_probe_bond){ .weight = 1023 }; /* above the range: written as the maximum */
		cg_probe_bond_write(tp, &b);
		CHECK(!memcmp(tp + CG_PROBE_INFO_LEN, "\x00\x00\x03\xe8", 4));
		/* A newer peer: reserved bits set, a weight past 1000, a longer
		 * payload. The known bits are read, the rest ignored. */
		memcpy(tp + CG_PROBE_INFO_LEN, "\xff\xfc\xfc\x05", 4);
		CHECK_EQ(cg_probe_bond_read(&r, tp, sizeof(tp)), 1);
		CHECK(r.weight == 5 && !r.reorders && !r.split_me);
		memcpy(tp + CG_PROBE_INFO_LEN, "\xff\xff\xff\xff", 4);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(r.weight == CG_BOND_WEIGHT_MAX && r.reorders && r.split_me);
		/* An older peer: 40 bytes (or anything short of the tail) has none,
		 * and reads as nothing asked. */
		r.weight = 7;
		r.reorders = r.split_me = 1;
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_INFO_LEN), 0);
		CHECK(r.weight == 0 && !r.reorders && !r.split_me);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN - 1), 0);
		/* A probe and a reply with it parse, and the MAC covers it. */
		b = (struct cg_probe_bond){ .weight = 3, .reorders = 1 };
		cg_probe_bond_write(tp, &b);
		for (uint8_t t = CG_T_PROBE; t <= CG_T_PROBE_REPLY; t++) {
			h.type = t;
			cg_hdr_write(pkt, &h, key_a, tp, CG_PROBE_BOND_LEN);
			memcpy(pkt + CG_HDR_LEN, tp, CG_PROBE_BOND_LEN);
			CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_BOND_LEN), 0);
			CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_BOND_LEN, key_a));
			CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a)); /* cut to 40: no */
			pkt[CG_HDR_LEN + CG_PROBE_BOND_LEN - 3] ^= 0x01; /* CG_BOND_REORDERS on the way */
			CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_BOND_LEN, key_a));
		}
		h.type = CG_T_PROBE;
	}

	/* Outer packet sizes: the tun_mtu that fills a 1500-byte path is 1448
	 * over IPv4 and 1428 over IPv6; the default 1420 fits both. */
	CHECK_EQ(cg_outer_len(AF_INET, 1448), 1500);
	CHECK_EQ(cg_outer_len(AF_INET6, 1428), 1500);
	CHECK_EQ(cg_outer_len(AF_INET6, 1420), 1492); /* CG_TUN_MTU_DEFAULT */
	CHECK_EQ(1500 - cg_outer_len(AF_INET6, 0), 1428);
	CHECK_EQ(cg_outer_len(AF_INET, 0), CG_HDR_LEN + 28);

	/* The rescue bit of a reply's bonding tail (version 5). */
	{
		struct cg_probe_bond b = { .rescue = 1 }, r;
		uint8_t tp[CG_PROBE_BOND_LEN];

		cg_probe_bond_write(tp, &b);
		CHECK(!memcmp(tp + CG_PROBE_INFO_LEN, "\x00\x04\x00\x00", 4));
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(r.rescue && !r.reorders && !r.split_me && !r.weight);
	}

	/* The link type in the bonding tail (bits 20-22, story 015): written
	 * and read back, next to every other field and untouched by them; a
	 * writer that predates it writes the bits 0 (auto); a reader that
	 * predates it masks them off as reserved, its fields the same. */
	{
		struct cg_probe_bond b = { .weight = 1000, .reorders = 1, .split_me = 1, .rescue = 1, .full = 1 }, r;
		uint8_t tp[CG_PROBE_BOND_LEN];
		uint32_t v;

		CHECK_EQ(CG_BOND_TYPE, 0x00700000u);
		for (uint8_t t = 0; t <= 7; t++) {
			b.type = t;
			cg_probe_bond_write(tp, &b);
			v = (uint32_t)tp[CG_PROBE_INFO_LEN] << 24 | (uint32_t)tp[CG_PROBE_INFO_LEN + 1] << 16 |
			    (uint32_t)tp[CG_PROBE_INFO_LEN + 2] << 8 | tp[CG_PROBE_INFO_LEN + 3];
			CHECK_EQ(v >> 24, 0); /* bits 24+ stay reserved */
			CHECK_EQ(v & CG_BOND_TCPMARK, 0);
			CHECK_EQ((v >> 20) & 7, t);
			/* What an older reader knows of the word: bits 0-19 only, the same for every type */
			CHECK_EQ(v & 0xfffffu, 1000u | CG_BOND_REORDERS | CG_BOND_SPLIT_ME | CG_BOND_RESCUE | CG_BOND_FULL);
			CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
			CHECK(r.type == t && r.weight == 1000 && r.reorders && r.split_me && r.rescue && r.full);
		}
		b.type = 9; /* past 3 bits: only the low 3 go, never the bits above */
		cg_probe_bond_write(tp, &b);
		CHECK_EQ(tp[CG_PROBE_INFO_LEN] & 0xff, 0x00);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK_EQ(r.type, 1);
		/* An older writer: the word without bits 20-22, read as auto */
		memcpy(tp + CG_PROBE_INFO_LEN, "\x00\x0f\x03\xe8", 4);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(r.type == 0 && r.weight == 1000 && r.full);
		/* No tail at all (a 40-byte peer): auto */
		r.type = 5;
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_INFO_LEN), 0);
		CHECK_EQ(r.type, 0);
	}

	/* CG_BOND_TCPMARK (bit 23, story 023): written and read back, alone
	 * and next to the type; an older writer leaves it 0 (its ACKs' marks
	 * mean nothing), an older reader ignores it. */
	{
		struct cg_probe_bond b = { .weight = 7, .type = 7, .tcpmark = 1 }, r;
		uint8_t tp[CG_PROBE_BOND_LEN];

		CHECK_EQ(CG_BOND_TCPMARK, 0x00800000u);
		cg_probe_bond_write(tp, &b);
		CHECK(!memcmp(tp + CG_PROBE_INFO_LEN, "\x00\xf0\x00\x07", 4));
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(r.tcpmark && r.type == 7 && r.weight == 7 && !r.rescue);
		b.tcpmark = 0;
		cg_probe_bond_write(tp, &b);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_BOND_LEN), 1);
		CHECK(!r.tcpmark && r.type == 7);
		CHECK_EQ(cg_probe_bond_read(&r, tp, CG_PROBE_INFO_LEN), 0);
		CHECK(!r.tcpmark);
	}

	/* NACK: ranges of a base and a 64-bit mask, under the MAC; a count of 0,
	 * past CG_NACK_RANGES or longer than the payload does not parse. */
	{
		uint32_t seq[200], got[CG_NACK_SEQ_MAX];
		uint8_t nb[CG_HDR_LEN + CG_NACK_LEN];
		struct cg_hdr nh = { .type = CG_T_NACK, .flags = CG_F_RETRY, .hint = 0x5a, .session = 7, .seq = 9, .ts = 1 },
			      np;
		unsigned used, n = 0;
		size_t nl;

		/* 70 sequences in a row across the wrap, then 3 far apart */
		for (unsigned i = 0; i < 70; i++)
			seq[n++] = 0xffffffe0u + i;
		seq[n++] = 5000;
		seq[n++] = 5064; /* 64 after 5000: in its mask */
		seq[n++] = 5065; /* 65 after: a range of its own */
		CHECK(cg_type_ctl(CG_T_NACK));
		CHECK(cg_type_ctl(CG_T_FEC));
		nl = cg_nack_write(nb + CG_HDR_LEN, seq, n, &used);
		CHECK_EQ(used, n);
		CHECK_EQ(nb[CG_HDR_LEN], 4); /* 0xffffffe0 + 64 more, 6 more, 5000 (+5064), 5065 */
		CHECK_EQ(nl, 1 + 12 * 4);
		cg_hdr_write(nb, &nh, key_a, nb + CG_HDR_LEN, nl);
		CHECK_EQ(cg_hdr_parse(&np, nb, CG_HDR_LEN + nl), 0);
		CHECK(np.type == CG_T_NACK && np.flags == CG_F_RETRY);
		CHECK(cg_hdr_verify(nb, CG_HDR_LEN + nl, key_a));
		CHECK_EQ(cg_nack_read(got, CG_NACK_SEQ_MAX, nb + CG_HDR_LEN, nl), n);
		CHECK(!memcmp(got, seq, n * sizeof(seq[0])));
		CHECK_EQ(cg_nack_read(got, 10, nb + CG_HDR_LEN, nl), 10); /* at most max */
		CHECK_EQ(cg_hdr_parse(&np, nb, CG_HDR_LEN + nl - 1), -1); /* short by a byte */
		/* 20 sequences 100 apart: 16 ranges, the rest for the next NACK */
		for (unsigned i = 0; i < 20; i++)
			seq[i] = 100 * i;
		nl = cg_nack_write(nb + CG_HDR_LEN, seq, 20, &used);
		CHECK_EQ(used, CG_NACK_RANGES);
		CHECK_EQ(nl, CG_NACK_LEN);
		CHECK_EQ(cg_nack_read(got, CG_NACK_SEQ_MAX, nb + CG_HDR_LEN, nl), CG_NACK_RANGES);
		CHECK_EQ(got[CG_NACK_RANGES - 1], 100 * (CG_NACK_RANGES - 1));
		nl = cg_nack_write(nb + CG_HDR_LEN, seq, 1, &used);
		CHECK(used == 1 && nl == 13);
		CHECK_EQ(cg_hdr_parse(&np, nb, CG_HDR_LEN + nl), 0);
		nb[CG_HDR_LEN] = 0;
		CHECK_EQ(cg_hdr_parse(&np, nb, CG_HDR_LEN + nl), -1);
		nb[CG_HDR_LEN] = CG_NACK_RANGES + 1;
		CHECK_EQ(cg_hdr_parse(&np, nb, sizeof(nb)), -1);
	}
}
