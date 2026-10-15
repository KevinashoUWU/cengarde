/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "fec.h"
#include "test.h"

static uint32_t rseq[CG_FEC_RING];
static uint16_t rlen[CG_FEC_RING];
static uint8_t rhave[CG_FEC_RING], rpidx[CG_FEC_RING];
static uint8_t rmem[CG_FEC_RING * CG_FEC_PLEN_MAX];
static struct cg_fec_rx rx;
static struct cg_fec_tx tx;

static void rx_reset(void)
{
	memset(&rx, 0, sizeof(rx));
	memset(rhave, 0, sizeof(rhave));
	memset(rpidx, 0, sizeof(rpidx));
	rx.seq = rseq;
	rx.len = rlen;
	rx.have = rhave;
	rx.mem = rmem;
	rx.pidx = rpidx;
}

/* Packet seq: len bytes, from the sequence. */
static uint16_t pkt(uint32_t seq, uint8_t *p)
{
	uint16_t len = (uint16_t)(60 + seq * 37 % 1300);

	for (uint16_t i = 0; i < len; i++)
		p[i] = (uint8_t)(seq * 7 + i * 13);
	return len;
}

/* A group of n from base, one per link 0..n-1; its parity into par. */
static unsigned group(uint32_t base, unsigned n, uint8_t *par)
{
	uint8_t p[CG_FEC_PLEN_MAX];

	for (unsigned j = 0; j < n; j++) {
		uint16_t len = pkt(base + j, p);

		CHECK(!cg_fec_tx_breaks(&tx, base + j, (int)j));
		cg_fec_tx_add(&tx, base + j, (int)j, p, len, 1000 + j);
	}
	return cg_fec_tx_close(&tx, par);
}

static void test_fec_rebuild(void)
{
	uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX], p[CG_FEC_PLEN_MAX], out[CG_FEC_PLEN_MAX];
	uint32_t s;
	uint16_t l;

	/* Each one of a group of 3 missing, the parity first or last. */
	for (unsigned miss = 0; miss < 3; miss++) {
		for (int parity_first = 0; parity_first < 2; parity_first++) {
			unsigned plen = group(100, 3, par);
			int k = -1, got = 0;

			rx_reset();
			CHECK(plen == CG_FEC_HDR + 60 + 102 * 37 % 1300 || plen > CG_FEC_HDR);
			if (parity_first)
				k = cg_fec_rx_parity(&rx, par, plen);
			for (unsigned j = 0; j < 3; j++) {
				uint16_t len;
				int slot;

				if (j == miss)
					continue;
				len = pkt(100 + j, p);
				slot = cg_fec_rx_put(&rx, 100 + j, p, len);
				if (parity_first && slot >= 0 && cg_fec_rx_try(&rx, slot, out, &s, &l) == 1)
					got = 1;
			}
			if (!parity_first) {
				k = cg_fec_rx_parity(&rx, par, plen);
				got = cg_fec_rx_try(&rx, k, out, &s, &l) == 1;
			}
			CHECK(got);
			CHECK_EQ(s, 100 + miss);
			CHECK_EQ(l, pkt(100 + miss, p));
			CHECK(!memcmp(out, p, l));
			CHECK_EQ(rx.recovered, 1);
		}
	}
	/* Two missing: nothing; the third arriving later still rebuilds the
	 * other. */
	{
		unsigned plen = group(200, 3, par);
		int k;
		uint16_t len;

		rx_reset();
		len = pkt(200, p);
		cg_fec_rx_put(&rx, 200, p, len);
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 0);
		len = pkt(202, p);
		CHECK_EQ(cg_fec_rx_put(&rx, 202, p, len), k);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 1);
		CHECK_EQ(s, 201);
		CHECK(l == pkt(201, p) && !memcmp(out, p, l));
	}
	/* All arrived: the parity is let go, nothing rebuilt. */
	{
		unsigned plen = group(300, 2, par);
		int k;

		rx_reset();
		for (unsigned j = 0; j < 2; j++)
			cg_fec_rx_put(&rx, 300 + j, p, pkt(300 + j, p));
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), -1);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), -1);
		CHECK_EQ(rx.recovered, 0);
	}
	/* A group of one: its parity is the packet. */
	{
		unsigned plen = group(400, 1, par);
		int k;

		rx_reset();
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 1);
		CHECK(s == 400 && l == pkt(400, p) && !memcmp(out, p, l));
	}
}

static void test_fec_groups(void)
{
	uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX], p[64] = { 1 };

	memset(&tx, 0, sizeof(tx));
	CHECK(!cg_fec_tx_breaks(&tx, 5, 0)); /* none open */
	CHECK_EQ(cg_fec_tx_close(&tx, par), 0);
	cg_fec_tx_add(&tx, 5, 0, p, 64, 1000);
	CHECK(!cg_fec_tx_breaks(&tx, 6, 1));
	CHECK(cg_fec_tx_breaks(&tx, 6, 0));  /* a link it holds */
	CHECK(cg_fec_tx_breaks(&tx, 7, 1));  /* not the next sequence */
	CHECK(cg_fec_tx_breaks(&tx, 6, -1)); /* on several links */
	CHECK(!cg_fec_tx_due(&tx, 1000 + 9999, 10000));
	CHECK(cg_fec_tx_due(&tx, 1000 + 10000, 10000));
	CHECK_EQ(cg_fec_tx_close(&tx, par), CG_FEC_HDR + 64);
	CHECK(!cg_fec_tx_due(&tx, 1 << 30, 10000)); /* closed */
	CHECK(par[0] == 0 && par[3] == 5 && par[4] == 1);
}

static void test_fec_bad(void)
{
	uint8_t par[CG_FEC_HDR + 8] = { 0, 0, 0, 1, 0 };
	uint8_t out[CG_FEC_PLEN_MAX];
	uint32_t s;
	uint16_t l;

	rx_reset();
	CHECK_EQ(cg_fec_rx_parity(&rx, par, 3), -1);              /* short */
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1);    /* n 0 */
	par[4] = CG_FEC_KMAX + 1;
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1);    /* n too large */
	CHECK_EQ(rx.bad, 3);
	CHECK_EQ(cg_fec_rx_try(&rx, -1, out, &s, &l), -1);
	CHECK_EQ(cg_fec_rx_try(&rx, CG_FEC_PAR, out, &s, &l), -1);
	/* A length that does not fit the parity: not rebuilt. */
	par[4] = 1;
	par[5] = 0x7f;
	par[6] = 0xff;
	CHECK_EQ(cg_fec_rx_try(&rx, cg_fec_rx_parity(&rx, par, sizeof(par)), out, &s, &l), -1);
	CHECK_EQ(rx.bad, 4);
	/* Slots taken over before their group was rebuilt are counted. */
	par[5] = par[6] = 0;
	par[6] = 8;
	for (int i = 0; i < CG_FEC_PAR + 3; i++) {
		par[3] = (uint8_t)(10 + 2 * i); /* groups of 1 that never complete... */
		par[4] = 2;                      /* ...as groups of 2 with both missing */
		cg_fec_rx_parity(&rx, par, sizeof(par));
	}
	CHECK_EQ(rx.unrecovered, 3);
}

void test_fec(void)
{
	memset(&tx, 0, sizeof(tx));
	test_fec_groups();
	test_fec_rebuild();
	test_fec_bad();
}
