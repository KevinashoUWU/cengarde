/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "repair.h"
#include "rs.h"
#include "test.h"

static uint32_t r_seq[CG_REP_RING];
static uint16_t r_len[CG_REP_RING];
static uint8_t r_have[CG_REP_RING];
static uint8_t r_mem[CG_REP_RING * CG_SYM_PLEN];

static struct cg_rep_ring ring_new(void)
{
	struct cg_rep_ring r = { r_seq, r_len, r_have, r_mem };

	memset(r_have, 0, sizeof(r_have));
	return r;
}

static uint16_t pkt(uint32_t seq, uint8_t *p)
{
	uint16_t len = (uint16_t)(60 + seq * 37 % 1300);

	for (unsigned i = 0; i < len; i++)
		p[i] = (uint8_t)(seq * 13 + i * 5 + (i >> 4));
	return len;
}

static void ring_put(struct cg_rep_ring *r, uint32_t seq)
{
	uint32_t i = seq & (CG_REP_RING - 1);

	r->len[i] = pkt(seq, r->mem + (size_t)i * CG_SYM_PLEN);
	r->seq[i] = seq;
	r->have[i] = 1;
}

static int ring_ok(const struct cg_rep_ring *r, uint32_t seq)
{
	uint8_t p[CG_SYM_PLEN];
	uint16_t len = pkt(seq, p), l2;
	const uint8_t *q;

	if (!cg_rep_has(r, seq))
		return 0;
	q = cg_rep_data(r, seq, &l2);
	return l2 == len && !memcmp(p, q, len);
}

static struct cg_rs_tx T;
static struct cg_rs_rx R;
static struct cg_rs_scratch X;
static uint8_t O[14][256] __attribute__((aligned(64)));

/* Leopard is MDS: k = 10, m = 4, every pattern of up to 4 lost among the
 * 14 gives the originals back; 5 lost does not. */
static void test_rs_mds(void)
{
	const uint8_t *orig[10], *rec[4];
	uint8_t *work[CG_RS_WORK_ENC], *dw[CG_RS_WORK_DEC];
	unsigned ok = 0, tried = 0;

	for (unsigned i = 0; i < 10; i++) {
		for (unsigned b = 0; b < 256; b++)
			O[i][b] = (uint8_t)(i * 29 + b * 3 + (b >> 5));
		orig[i] = O[i];
	}
	for (unsigned j = 0; j < CG_RS_WORK_ENC; j++)
		work[j] = T.work[j];
	CHECK_EQ(cg_rs_encode(10, 4, 256, orig, work), 0);
	for (unsigned j = 0; j < 4; j++)
		memcpy(O[10 + j], work[j], 256);
	CHECK_EQ(cg_rs_encode(4, 5, 256, orig, work), -1); /* m over k */
	CHECK_EQ(cg_rs_encode(10, 4, 100, orig, work), -1); /* not a multiple of 64 */
	for (unsigned j = 0; j < CG_RS_WORK_DEC; j++)
		dw[j] = X.work[j];
	for (unsigned lost = 0; lost < 1u << 14; lost++) {
		unsigned n = (unsigned)__builtin_popcount(lost), good = 1;

		if (n > 5 || !(lost & 0x3ff))
			continue;
		for (unsigned i = 0; i < 10; i++)
			orig[i] = lost >> i & 1 ? NULL : O[i];
		for (unsigned j = 0; j < 4; j++)
			rec[j] = lost >> (10 + j) & 1 ? NULL : O[10 + j];
		if (n == 5) {
			if (!(lost & 1) || (lost >> 10) != 0xf)
				continue; /* one of them suffices */
			CHECK_EQ(cg_rs_decode(10, 4, 256, orig, rec, dw), -1);
			continue;
		}
		tried++;
		if (cg_rs_decode(10, 4, 256, orig, rec, dw) < 0)
			continue;
		for (unsigned i = 0; i < 10; i++)
			if (!orig[i] && memcmp(dw[i], O[i], 256))
				good = 0;
		ok += good;
	}
	CHECK_EQ(ok, tried);
	CHECK(tried > 1000);
}

/* A block through the sender and the receiver: members lost and rebuilt
 * byte for byte; the block's symbol size is its longest, rounded to 64. */
static void test_rs_block(void)
{
	struct cg_rep_ring ring = ring_new();
	struct cg_rep_hdr h, g;
	uint8_t *sym[CG_RS_MMAX], p[CG_SYM_PLEN], wire[CG_REP_HDR + CG_RS_BYTES_MAX];
	uint32_t out[CG_RS_KMAX];
	unsigned bytes, m, got = 0, slen = 0;

	memset(&R, 0, sizeof(R));
	for (uint32_t s = 4294967290u, i = 0; i < 20; s += 1 + (i % 3 == 0), i++) {
		CHECK(cg_rs_tx_fits(&T, s));
		cg_rs_tx_add(&T, s, p, pkt(s, p), 3, 1000);
		if (i != 2 && i != 7 && i != 19)
			ring_put(&ring, s);
	}
	CHECK_EQ(T.k, 20);
	CHECK(!cg_rs_tx_fits(&T, T.base + CG_REP_SPAN));
	m = cg_rs_tx_close(&T, &h, sym, &bytes);
	CHECK_EQ(m, 3);
	CHECK_EQ(T.k, 0);
	CHECK_EQ(h.kind, CG_REP_RS);
	CHECK_EQ(h.base, 4294967290u);
	CHECK_EQ(__builtin_popcountll(h.map), 20);
	CHECK_EQ(bytes % 64, 0);
	for (unsigned j = 0; j < m; j++) {
		h.idx = (uint16_t)j;
		cg_rep_put(wire, &h);
		memcpy(wire + CG_REP_HDR, sym[j], bytes);
		CHECK_EQ(cg_rep_get(wire, CG_REP_HDR + bytes, &g, &slen), 0);
		got += cg_rs_rx_repair(&R, &X, &ring, &g, wire + CG_REP_HDR, slen, out + got, CG_RS_KMAX - got);
		if (j < 2)
			CHECK_EQ(got, 0); /* 3 missing: waits for the third repair */
	}
	CHECK_EQ(got, 3);
	for (unsigned i = 0; i < got; i++)
		CHECK(ring_ok(&ring, out[i]));
	CHECK_EQ(R.recovered, 3);
	CHECK_EQ(R.decodes, 1);
	CHECK_EQ(R.pool_used, 0);
	for (unsigned i = 0; i < CG_REP_RING; i++)
		CHECK(!R.want[i]);
	/* again: everything there now, the repairs are useless */
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &g, wire + CG_REP_HDR, slen, out, CG_RS_KMAX), 0);
	CHECK_EQ(R.useless, 1);
}

/* A short block (its deadline): m no more than k; and one lost member that
 * arrives late after a repair: the block is let go, nothing rebuilt. Then
 * two lost, a repair kept, one arrives late: the second repair is not
 * needed, the first rebuilds the other. */
static void test_rs_short_late(void)
{
	struct cg_rep_ring ring = ring_new();
	struct cg_rep_hdr h;
	uint8_t *sym[CG_RS_MMAX], p[CG_SYM_PLEN], keep[2][CG_RS_BYTES_MAX];
	uint32_t out[CG_RS_KMAX];
	unsigned bytes, m;

	memset(&R, 0, sizeof(R));
	cg_rs_tx_add(&T, 50, p, pkt(50, p), 6, 0);
	cg_rs_tx_add(&T, 51, p, pkt(51, p), 6, 0);
	m = cg_rs_tx_close(&T, &h, sym, &bytes);
	CHECK_EQ(m, 2);
	ring_put(&ring, 50);
	h.idx = 0;
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, sym[0], bytes, out, CG_RS_KMAX), 1);
	CHECK_EQ(out[0], 51);
	CHECK(ring_ok(&ring, 51));

	ring = ring_new();
	for (uint32_t s = 200; s < 208; s++)
		cg_rs_tx_add(&T, s, p, pkt(s, p), 2, 0);
	m = cg_rs_tx_close(&T, &h, sym, &bytes);
	CHECK_EQ(m, 2);
	memcpy(keep[0], sym[0], bytes);
	memcpy(keep[1], sym[1], bytes);
	for (uint32_t s = 200; s < 208; s++)
		if (s != 203 && s != 206)
			ring_put(&ring, s);
	h.idx = 1;
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, keep[1], bytes, out, CG_RS_KMAX), 0);
	CHECK_EQ(R.want[203], 1);
	ring_put(&ring, 206);
	CHECK_EQ(cg_rs_rx_data(&R, &X, &ring, 206, out, CG_RS_KMAX), 1);
	CHECK_EQ(out[0], 203);
	CHECK(ring_ok(&ring, 203));
	CHECK_EQ(R.want[203], 0);
	/* the late repair finds the block whole */
	h.idx = 0;
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, keep[0], bytes, out, CG_RS_KMAX), 0);
	/* a wrong size for the block: bad */
	ring.have[201] = 0;
	h.idx = 1;
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, keep[1], bytes, out, CG_RS_KMAX), 1);
	ring.have[201] = 0;
	ring.have[202] = 0;
	memset(&R, 0, sizeof(R));
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, keep[1], bytes, out, CG_RS_KMAX), 0);
	h.idx = 0;
	CHECK_EQ(cg_rs_rx_repair(&R, &X, &ring, &h, keep[0], bytes - 64, out, CG_RS_KMAX), 0);
	CHECK_EQ(R.bad, 1);
}

void test_rs(void)
{
	if (cg_rs_init() < 0) {
#ifndef CG_NO_LEOPARD
		CHECK(!"leo_init failed");
#endif
		return;
	}
	test_rs_mds();
	test_rs_block();
	test_rs_short_late();
}
