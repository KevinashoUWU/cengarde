/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* Packet seq: its length and bytes, from the sequence. */
static uint16_t pkt(uint32_t seq, uint8_t *p)
{
	uint16_t len = (uint16_t)(60 + seq * 37 % 1300);

	for (uint16_t i = 0; i < len; i++)
		p[i] = (uint8_t)(seq * 7 + i * 13);
	return len;
}

/* Sends seq on link through the sender, as the client does with k; the
 * parity of a group it closes, if any, into par (returns its length). */
static unsigned send1(uint32_t seq, int link, unsigned k, uint64_t now, uint8_t *par)
{
	uint8_t p[CG_FEC_PLEN_MAX];
	unsigned plen = 0;
	int i;

	while ((i = cg_fec_tx_evict(&tx, seq, link)) >= 0)
		plen = cg_fec_grp_close(&tx.g[i], par);
	i = cg_fec_tx_add(&tx, seq, link, p, pkt(seq, p), now);
	if (tx.g[i].n >= k)
		plen = cg_fec_grp_close(&tx.g[i], par);
	return plen;
}

/* The members of a parity as a mask of distances from its base (< 64). */
static uint64_t seqs_of(const uint8_t *par)
{
	uint64_t m = 1;

	for (unsigned j = 1; j < par[4]; j++)
		m |= 1ull << (par[7 + 2 * (j - 1)] << 8 | par[8 + 2 * (j - 1)]);
	return m;
}

static uint32_t base_of(const uint8_t *par)
{
	return (uint32_t)par[0] << 24 | (uint32_t)par[1] << 16 | (uint32_t)par[2] << 8 | par[3];
}

/* The split sends several in a row on one link: the groups interleave and
 * still fill to k, one packet of each link. */
static void test_fec_interleave(void)
{
	uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX];

	memset(&tx, 0, sizeof(tx));
	/* 3 links, k = 2 */
	CHECK_EQ(send1(10, 0, 2, 1000, par), 0);
	CHECK_EQ(send1(11, 0, 2, 1001, par), 0); /* link 0 taken: a second group */
	CHECK(send1(12, 1, 2, 1002, par) > 0);   /* joins the oldest: 10 and 12 */
	CHECK(base_of(par) == 10 && seqs_of(par) == 0x5);
	CHECK(send1(13, 1, 2, 1003, par) > 0); /* 11 and 13 */
	CHECK(base_of(par) == 11 && seqs_of(par) == 0x5);
	CHECK_EQ(send1(14, 2, 2, 1004, par), 0);
	/* deadlines: the open group's */
	CHECK_EQ(cg_fec_tx_next_us(&tx, 10000), 1004 + 10000);
	CHECK_EQ(cg_fec_tx_due(&tx, 1004 + 9999, 10000), -1);
	CHECK(cg_fec_tx_due(&tx, 1004 + 10000, 10000) >= 0);
	/* every slot open with link 0: the oldest goes first */
	memset(&tx, 0, sizeof(tx));
	for (uint32_t s = 0; s < CG_FEC_OPEN; s++)
		CHECK_EQ(send1(20 + s, 0, 3, 2000 + s, par), 0);
	CHECK(send1(20 + CG_FEC_OPEN, 0, 3, 3000, par) > 0);
	CHECK(base_of(par) == 20 && seqs_of(par) == 1);
	/* a group spanning more than CG_FEC_SPAN closes */
	memset(&tx, 0, sizeof(tx));
	CHECK_EQ(send1(100, 0, 3, 1, par), 0);
	CHECK(send1(100 + CG_FEC_SPAN, 1, 3, 2, par) > 0);
	CHECK(base_of(par) == 100 && seqs_of(par) == 1);
	CHECK_EQ(cg_fec_tx_next_us(&tx, 5), 2 + 5);
	memset(&tx, 0, sizeof(tx));
	CHECK_EQ(cg_fec_tx_next_us(&tx, 5), 0);
}

/* Each member of a group of 3 sequences apart (100, 102, 105) rebuilt,
 * the parity first or last; two missing wait for one more. */
static void test_fec_rebuild(void)
{
	static const uint32_t m[3] = { 100, 102, 105 };
	uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX], p[CG_FEC_PLEN_MAX], out[CG_FEC_PLEN_MAX];
	uint32_t s;
	uint16_t l;
	unsigned plen = 0;

	for (unsigned miss = 0; miss < 3; miss++) {
		for (int parity_first = 0; parity_first < 2; parity_first++) {
			int k, got = 0;

			memset(&tx, 0, sizeof(tx));
			for (unsigned j = 0; j < 3; j++)
				plen = send1(m[j], (int)j, 3, 1000, par);
			CHECK(plen > CG_FEC_HDR && seqs_of(par) == 0x25);
			rx_reset();
			if (parity_first)
				cg_fec_rx_parity(&rx, par, plen);
			for (unsigned j = 0; j < 3; j++) {
				int slot;

				if (j == miss)
					continue;
				slot = cg_fec_rx_put(&rx, m[j], p, pkt(m[j], p));
				if (parity_first && slot >= 0 && cg_fec_rx_try(&rx, slot, out, &s, &l) == 1)
					got = 1;
			}
			if (!parity_first) {
				k = cg_fec_rx_parity(&rx, par, plen);
				got = cg_fec_rx_try(&rx, k, out, &s, &l) == 1;
			}
			CHECK(got);
			CHECK_EQ(s, m[miss]);
			CHECK_EQ(l, pkt(m[miss], p));
			CHECK(!memcmp(out, p, l));
			CHECK_EQ(rx.recovered, 1);
		}
	}
	/* members far apart (the split's runs): 100 and 400 */
	{
		int k;

		memset(&tx, 0, sizeof(tx));
		send1(100, 0, 2, 1, par);
		plen = send1(400, 1, 2, 2, par);
		CHECK(plen > CG_FEC_HDR && base_of(par) == 100 && par[4] == 2);
		rx_reset();
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_put(&rx, 400, p, pkt(400, p)), k);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 1);
		CHECK(s == 100 && l == pkt(100, p) && !memcmp(out, p, l));
		/* the group of 100, 102, 105 again */
		memset(&tx, 0, sizeof(tx));
		for (unsigned j = 0; j < 3; j++)
			plen = send1(m[j], (int)j, 3, 1000, par);
	}
	/* 101, 103 and 104 are not in it */
	rx_reset();
	cg_fec_rx_parity(&rx, par, plen);
	CHECK(cg_fec_rx_slot(&rx, 100) >= 0 && cg_fec_rx_slot(&rx, 105) >= 0);
	CHECK(cg_fec_rx_slot(&rx, 101) < 0 && cg_fec_rx_slot(&rx, 104) < 0);
	/* two missing: nothing until one comes */
	{
		int k = cg_fec_rx_parity(&rx, par, plen);

		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 0);
		CHECK_EQ(cg_fec_rx_put(&rx, 102, p, pkt(102, p)), k);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 0);
		CHECK_EQ(cg_fec_rx_put(&rx, 105, p, pkt(105, p)), k);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 1);
		CHECK(s == 100 && l == pkt(100, p) && !memcmp(out, p, l));
	}
	/* all there: let go, nothing rebuilt */
	{
		int k;

		rx_reset();
		for (unsigned j = 0; j < 3; j++)
			cg_fec_rx_put(&rx, m[j], p, pkt(m[j], p));
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), -1);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), -1);
		CHECK_EQ(rx.recovered, 0);
	}
	/* a group of one: its parity is the packet */
	{
		int k;

		memset(&tx, 0, sizeof(tx));
		send1(400, 0, 3, 1, par);
		plen = cg_fec_grp_close(&tx.g[cg_fec_tx_due(&tx, 99, 1)], par);
		rx_reset();
		k = cg_fec_rx_parity(&rx, par, plen);
		CHECK_EQ(cg_fec_rx_try(&rx, k, out, &s, &l), 1);
		CHECK(s == 400 && l == pkt(400, p) && !memcmp(out, p, l));
	}
}

static void test_fec_bad(void)
{
	uint8_t par[CG_FEC_HDR + 8] = { 0 };
	uint8_t out[CG_FEC_PLEN_MAX];
	uint32_t s;
	uint16_t l;

	rx_reset();
	par[3] = 1;
	CHECK_EQ(cg_fec_rx_parity(&rx, par, 3), -1);           /* short */
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1); /* n 0 */
	par[4] = CG_FEC_KMAX + 1;
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1); /* n too large */
	par[4] = 2;                                            /* a second member at distance 0 */
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1);
	par[7] = CG_FEC_SPAN >> 8;                             /* or too far */
	CHECK_EQ(cg_fec_rx_parity(&rx, par, sizeof(par)), -1);
	CHECK_EQ(rx.bad, 5);
	CHECK_EQ(cg_fec_rx_try(&rx, -1, out, &s, &l), -1);
	CHECK_EQ(cg_fec_rx_try(&rx, CG_FEC_PAR, out, &s, &l), -1);
	/* a length that does not fit the parity: not rebuilt */
	par[4] = 1;
	par[5] = 0x7f;
	par[6] = 0xff;
	par[7] = 0;
	CHECK_EQ(cg_fec_rx_try(&rx, cg_fec_rx_parity(&rx, par, sizeof(par)), out, &s, &l), -1);
	CHECK_EQ(rx.bad, 6);
	/* slots taken over before their group was rebuilt are counted */
	par[5] = 0;
	par[6] = 8;
	par[4] = 2; /* two members: base and base + 1 */
	par[8] = 1;
	for (int i = 0; i < CG_FEC_PAR + 3; i++) {
		par[3] = (uint8_t)(10 + 2 * i);
		cg_fec_rx_parity(&rx, par, sizeof(par));
	}
	CHECK_EQ(rx.unrecovered, 3);
}

void test_fec(void)
{
	test_fec_interleave();
	test_fec_rebuild();
	test_fec_bad();
}
