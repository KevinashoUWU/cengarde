/* The 1.0 wire format (wire.c, story 032): seal and open, the counter, the
 * epochs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "test.h"
#include "wire.h"

static void mem_packet(uint8_t *buf, struct cg_hdr *h, uint8_t type, size_t plen, uint32_t salt)
{
	memset(h, 0, sizeof(*h));
	h->type = type;
	h->flags = (uint8_t)(0x0c ^ salt);
	h->hint = 0x5a;
	h->link = 3;
	h->session = 0xdeadbeef;
	h->seq = 1000 + salt;
	h->ts = 0x01020304u * (salt + 1);
	h->sseq = 77 + salt;
	cg_hdr_put(buf, h);
	for (size_t i = 0; i < plen; i++)
		buf[CG_HDR_LEN + i] = (uint8_t)(i * 7 + salt);
	if (type == CG_T_NACK && plen >= 13)
		buf[CG_HDR_LEN] = 1;
}

void test_wire(void)
{
	uint8_t secret[CG_KEY_LEN], epoch[CG_EPOCH_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	for (int i = 0; i < CG_KEY_LEN; i++)
		secret[i] = (uint8_t)(i * 13 + 1);

	/* a round trip at the block edges, and the same bytes as the AEAD of
	 * the inner header and the payload under the clear header */
	{
		static const size_t lens[] = { 1, 49, 50, 51, 113, 114, 115, 200, 1400 };

		for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
			uint8_t mem[1500], wire[1600], ref[1600], inner[1500], tag[16];
			struct cg_seal s;
			struct cg_unseal u;
			struct cg_wire w;
			struct cg_hdr h, g;
			size_t plen = lens[k], n, m;
			uint8_t nonce[12] = { CG_DIR_C2S, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5 };

			mem_packet(mem, &h, CG_T_DATA, plen, (uint32_t)k);
			cg_seal_init(&s, secret, CG_DIR_C2S, h.session, epoch);
			s.ctr = 5;
			n = cg_wire_seal(wire, mem, mem + CG_HDR_LEN, plen, &s, 0);
			CHECK(n == plen + CG_HDR_LEN + CG_WIRE_EXTRA);
			CHECK(s.ctr == 6);
			inner[0] = h.type;
			inner[1] = h.flags;
			memcpy(inner + 2, mem + 8, 8);
			memcpy(inner + 10, mem + CG_SSEQ_OFF, 4);
			memcpy(inner + 14, mem + CG_HDR_LEN, plen);
			cg_aead_seal(ref, tag, inner, CG_WIRE_INNER + plen, wire, CG_WIRE_HDR, s.key, nonce);
			CHECK(!memcmp(ref, wire + CG_WIRE_HDR, CG_WIRE_INNER + plen));
			CHECK(!memcmp(tag, wire + n - CG_WIRE_TAG, 16));

			CHECK(cg_wire_peek(&w, wire, n) == 0);
			CHECK(w.kind == CG_WIRE_KIND_PLAIN && w.hint == 0x5a && w.link == 3 && w.session == 0xdeadbeef);
			cg_unseal_init(&u, secret, CG_DIR_C2S, w.session, epoch);
			CHECK(cg_wire_ctr(&w, 4) == 5);
			CHECK(cg_wire_open_head(&g, wire, n, &w, u.key, CG_DIR_C2S, 5) == 0);
			CHECK(g.type == h.type && g.flags == h.flags && g.seq == h.seq && g.ts == h.ts && g.sseq == h.sseq &&
			      g.link == h.link && g.hint == h.hint && g.session == h.session);
			m = cg_wire_open_body(wire, n, &g, &w, u.key, CG_DIR_C2S, 5);
			CHECK(m == CG_HDR_LEN + plen);
			CHECK(!memcmp(wire, mem, m));
		}
	}

	/* any byte changed, the wrong counter or the wrong direction: refused,
	 * the packet untouched */
	{
		uint8_t mem[200], wire[300], keep[300];
		struct cg_seal s;
		struct cg_wire w;
		struct cg_hdr g;
		size_t n;
		int bad = 0;

		mem_packet(mem, &g, CG_T_DATA, 60, 9);
		cg_seal_init(&s, secret, CG_DIR_S2C, g.session, epoch);
		n = cg_wire_seal(wire, mem, mem + CG_HDR_LEN, 60, &s, 0);
		memcpy(keep, wire, n);
		for (size_t i = 0; i < n; i++)
			for (int b = 0; b < 8; b += 3) {
				wire[i] ^= (uint8_t)(1 << b);
				if (cg_wire_peek(&w, wire, n) == 0 &&
				    cg_wire_open_head(&g, wire, n, &w, s.key, CG_DIR_S2C, cg_wire_ctr(&w, 0)) == 0)
					bad++;
				wire[i] ^= (uint8_t)(1 << b);
			}
		CHECK(bad == 0);
		CHECK(!memcmp(wire, keep, n));
		CHECK(cg_wire_peek(&w, wire, n) == 0);
		CHECK(cg_wire_open_head(&g, wire, n, &w, s.key, CG_DIR_S2C, 1) < 0);
		CHECK(cg_wire_open_head(&g, wire, n, &w, s.key, CG_DIR_C2S, 0) < 0);
		CHECK(cg_wire_open_head(&g, wire, n, &w, s.key, CG_DIR_S2C, 0) == 0);
		CHECK(!memcmp(wire, keep, n));
		CHECK(cg_wire_peek(&w, wire, CG_WIRE_HDR + CG_WIRE_INNER + CG_WIRE_TAG - 1) < 0);
		wire[0] ^= 0x10;
		CHECK(cg_wire_peek(&w, wire, n) < 0);
	}

	/* kind 1: the whole counter and the epoch; a receiver that knew none
	 * (or another) opens it under that epoch's key and takes it on */
	{
		uint8_t mem[200], wire[300], tmp[32];
		uint8_t e2[CG_EPOCH_LEN] = { 9, 9, 9, 9, 9, 9, 9, 9 };
		struct cg_seal s;
		struct cg_unseal u;
		struct cg_wire w;
		struct cg_hdr h, g;
		const uint8_t *key;
		uint64_t ctr;
		size_t n;

		mem_packet(mem, &h, CG_T_PROBE_REPLY, CG_PROBE_INFO_LEN, 2);
		cg_seal_init(&s, secret, CG_DIR_S2C, h.session, e2);
		s.ctr = 0x123456789ull;
		n = cg_wire_seal(wire, mem, mem + CG_HDR_LEN, CG_PROBE_INFO_LEN, &s, 1);
		CHECK(n == CG_PROBE_INFO_LEN + CG_HDR_LEN + CG_WIRE_EXTRA_MAX);
		CHECK(cg_wire_peek(&w, wire, n) == 0 && w.kind == CG_WIRE_KIND_EPOCH && w.hlen == 23);
		ctr = cg_wire_ctr(&w, 0);
		CHECK(ctr == 0x123456789ull);
		cg_unseal_init(&u, secret, CG_DIR_S2C, h.session, NULL);
		key = cg_unseal_key(&u, &w, secret, w.session, tmp);
		CHECK(key == tmp);
		CHECK(cg_wire_open_head(&g, wire, n, &w, key, CG_DIR_S2C, ctr) == 0);
		CHECK(cg_wire_open_body(wire, n, &g, &w, key, CG_DIR_S2C, ctr) == CG_HDR_LEN + CG_PROBE_INFO_LEN);
		CHECK(!memcmp(wire, mem, CG_HDR_LEN + CG_PROBE_INFO_LEN));
		cg_unseal_accept(&u, &w, key, ctr);
		CHECK(u.keyed && u.seen && u.top == ctr && !memcmp(u.epoch, e2, 8) && !memcmp(u.key, s.key, 32));
		/* a kind 0 after it, under that key */
		mem_packet(mem, &h, CG_T_DATA, 30, 3);
		n = cg_wire_seal(wire, mem, mem + CG_HDR_LEN, 30, &s, 0);
		CHECK(cg_wire_peek(&w, wire, n) == 0);
		CHECK(cg_unseal_key(&u, &w, secret, w.session, tmp) == u.key);
		CHECK(cg_wire_ctr(&w, u.top) == ctr + 1);
		CHECK(cg_wire_open_head(&g, wire, n, &w, u.key, CG_DIR_S2C, ctr + 1) == 0);
		/* the old epoch's key does not open it */
		{
			uint8_t old[32];

			cg_wire_key(old, secret, CG_DIR_S2C, h.session, epoch);
			CHECK(cg_wire_open_head(&g, wire, n, &w, old, CG_DIR_S2C, ctr + 1) < 0);
		}
	}

	/* the counter widened against the highest opened, either side of a
	 * 2^32 boundary */
	{
		struct cg_wire w = { .kind = CG_WIRE_KIND_PLAIN };

		w.nseq = 10;
		CHECK(cg_wire_ctr(&w, 0) == 10);
		w.nseq = 0xfffffff0u;
		CHECK(cg_wire_ctr(&w, 0) == 0xfffffff0u);
		CHECK(cg_wire_ctr(&w, 0x100000005ull) == 0xfffffff0u);
		w.nseq = 3;
		CHECK(cg_wire_ctr(&w, 0xfffffff0u) == 0x100000003ull);
		CHECK(cg_wire_ctr(&w, 0x500000002ull) == 0x500000003ull);
		w.nseq = 0x7fffffffu;
		CHECK(cg_wire_ctr(&w, 0x300000000ull) == 0x37fffffffull);
		w.nseq = 0x80000002u;
		CHECK(cg_wire_ctr(&w, 0x300000000ull) == 0x280000002ull);
	}

	/* a payload that does not fit its type is refused after the tag */
	{
		uint8_t mem[100], wire[200];
		struct cg_seal s;
		struct cg_wire w;
		struct cg_hdr h, g;
		size_t n;

		mem_packet(mem, &h, CG_T_PROBE, 10, 0); /* shorter than cg_probe_info */
		cg_seal_init(&s, secret, CG_DIR_C2S, h.session, epoch);
		n = cg_wire_seal(wire, mem, mem + CG_HDR_LEN, 10, &s, 1);
		CHECK(cg_wire_peek(&w, wire, n) == 0);
		CHECK(cg_wire_open_head(&g, wire, n, &w, s.key, CG_DIR_C2S, 0) == 0);
		CHECK(cg_wire_open_body(wire, n, &g, &w, s.key, CG_DIR_C2S, 0) == 0);
	}

	/* the keys: one per direction, session and epoch */
	{
		uint8_t a[32], b[32], c[32], d[32];
		uint8_t e2[CG_EPOCH_LEN] = { 1, 2, 3, 4, 5, 6, 7, 9 };

		cg_wire_key(a, secret, CG_DIR_C2S, 1, epoch);
		cg_wire_key(b, secret, CG_DIR_S2C, 1, epoch);
		cg_wire_key(c, secret, CG_DIR_C2S, 2, epoch);
		cg_wire_key(d, secret, CG_DIR_C2S, 1, e2);
		CHECK(memcmp(a, b, 32) && memcmp(a, c, 32) && memcmp(a, d, 32));
	}
}
