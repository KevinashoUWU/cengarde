/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "proto.h"
#include "test.h"

void test_proto(void)
{
	uint8_t key_a[16], key_b[16], pkt[CG_HDR_LEN + 64], payload[64];
	struct cg_hdr h = { .type = CG_T_DATA, .flags = 0, .link = 5, .session = 0xdeadbeef, .seq = 0x01020304,
			    .ts = 0xa0b0c0d0 },
		      p;
	struct cg_probe_info pi = { 1, 0x80000001, 100, 2, 3, 0xfffffffe }, po;
	uint8_t pib[CG_PROBE_INFO_LEN];

	for (int i = 0; i < 16; i++) {
		key_a[i] = (uint8_t)i;
		key_b[i] = (uint8_t)(i + 100);
	}
	for (int i = 0; i < 64; i++)
		payload[i] = (uint8_t)(i * 3);

	cg_hdr_write(pkt, &h, key_a, payload, sizeof(payload));
	memcpy(pkt + CG_HDR_LEN, payload, sizeof(payload));

	/* Layout: version/type, session, seq and ts in network order, link at byte 3. */
	CHECK_EQ(pkt[0], 0x31);
	CHECK_EQ(pkt[3], 5);
	CHECK(!memcmp(pkt + 4, "\xde\xad\xbe\xef\x01\x02\x03\x04\xa0\xb0\xc0\xd0", 12));

	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), 0);
	CHECK_EQ(p.type, CG_T_DATA);
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
	pkt[0] = 0x41; /* version 4 */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x3f; /* unknown type */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[0] = 0x31;
	pkt[2] = 1; /* reserved must be zero */
	CHECK_EQ(cg_hdr_parse(&p, pkt, sizeof(pkt)), -1);
	pkt[2] = 0;

	/* Probes carry exactly one cg_probe_info; flags are authenticated. */
	h.type = CG_T_PROBE;
	h.flags = CG_F_OWD | CG_F_MUTED;
	cg_probe_info_write(pib, &pi);
	cg_hdr_write(pkt, &h, key_a, pib, sizeof(pib));
	memcpy(pkt + CG_HDR_LEN, pib, sizeof(pib));
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN), 0);
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN + 1), -1);
	CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	CHECK_EQ(cg_hdr_parse(&p, pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN), 0);
	CHECK_EQ(p.flags, CG_F_OWD | CG_F_MUTED);
	pkt[1] ^= CG_F_MUTED;
	CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	pkt[1] ^= CG_F_MUTED;
	cg_probe_info_read(&po, pkt + CG_HDR_LEN);
	CHECK(po.echo_ts == 1 && po.owd == 0x80000001 && po.interval_ms == 100 && po.rx == 2 && po.wins == 3 &&
	      po.lag_us == 0xfffffffe);
	CHECK(!memcmp(pkt + CG_HDR_LEN, "\x00\x00\x00\x01\x80\x00\x00\x01\x00\x00\x00\x64", 12));

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
	cg_hdr_write(pkt, &h, key_a, pib, sizeof(pib));
	memcpy(pkt + CG_HDR_LEN, pib, sizeof(pib));
	CHECK(cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));
	pkt[1] ^= CG_F_PASS; /* nobody on the way can turn it off */
	CHECK(!cg_hdr_verify(pkt, CG_HDR_LEN + CG_PROBE_INFO_LEN, key_a));

	/* WireGuard message shapes (docs/historias/002). */
	{
		uint8_t wg[200] = { 0 };

		wg[0] = 1;
		CHECK(cg_looks_like_wg(wg, 148));
		CHECK(!cg_looks_like_wg(wg, 149));
		wg[0] = 2;
		CHECK(cg_looks_like_wg(wg, 92));
		wg[0] = 3;
		CHECK(cg_looks_like_wg(wg, 64));
		wg[0] = 4;
		CHECK(cg_looks_like_wg(wg, 32));
		CHECK(cg_looks_like_wg(wg, 177));
		CHECK(!cg_looks_like_wg(wg, 31));
		wg[2] = 1; /* reserved bytes must be zero */
		CHECK(!cg_looks_like_wg(wg, 64));
		wg[2] = 0;
		wg[0] = 5;
		CHECK(!cg_looks_like_wg(wg, 64));
	}
}
