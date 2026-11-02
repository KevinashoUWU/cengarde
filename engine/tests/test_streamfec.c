/* FEC v3 over STREAMs (streamfec.h, story 031): the CRC32C, the window's
 * symbols with it behind, and the receiver rebuilding a message lost.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "streamfec.h"
#include "test.h"

static struct cg_crc32c C;
static struct cg_gf G;
static struct cg_rlcw_tx T;

/* The reference: bit by bit. */
static uint32_t crc_slow(const uint8_t *p, size_t n)
{
	uint32_t x = 0xffffffffu;

	for (size_t i = 0; i < n; i++) {
		x ^= p[i];
		for (int k = 0; k < 8; k++)
			x = x & 1 ? x >> 1 ^ 0x82f63b78u : x >> 1;
	}
	return ~x;
}

void test_streamfec(void)
{
	struct cg_sfec_rx *R = calloc(1, sizeof(*R));
	struct cg_rep_ring ring;
	uint8_t msg[40][1400], sym[CG_SYM_MAX], big[CG_SYM_PLEN];
	uint32_t out[CG_RLC_COLS], top = 0;
	struct cg_rep_hdr h;
	unsigned n, sl, w, got = 0;
	int len;

	cg_crc32c_init(&C);
	cg_gf_init(&G);
	/* the check value of CRC32C (RFC 3720), and odd lengths and offsets */
	CHECK_EQ(cg_crc32c(&C, (const uint8_t *)"123456789", 9), 0xe3069283u);
	for (unsigned i = 0; i < sizeof(msg[0]); i++)
		msg[0][i] = (uint8_t)(i * 7 + 3);
	for (unsigned k = 0; k < 20; k++)
		CHECK_EQ(cg_crc32c(&C, msg[0] + k, 1000 + k * 13), crc_slow(msg[0] + k, 1000 + k * 13));
	CHECK_EQ(cg_crc32c(&C, msg[0], 0), 0);

	/* the symbol's length: the message and 4, or empty when it does not fit */
	CHECK_EQ(cg_sfec_slen(100), 104);
	CHECK_EQ(cg_sfec_slen(CG_SYM_PLEN - 4), CG_SYM_PLEN);
	CHECK_EQ(cg_sfec_slen(CG_SYM_PLEN - 3), 0);
	CHECK_EQ(cg_sfec_slen(0), 0);

	/* 40 messages of different lengths into the window; the receiver gets
	 * all but 7 and 23, then two repairs over the whole window */
	for (uint32_t s = 0; s < 40; s++) {
		uint16_t l = 0;
		const uint8_t *e;

		for (unsigned i = 0; i < sizeof(msg[s]); i++)
			msg[s][i] = (uint8_t)(s * 31 + i);
		cg_sfec_add(&T, &C, 1000 + s, msg[s], 200 + s * 30, 10 * s);
		e = cg_rlcw_get(&T, 1000 + s, &l);
		CHECK(e != NULL);
		CHECK_EQ(l, 200 + s * 30 + 4);
		if (s != 7 && s != 23)
			cg_sfec_keep(R, &C, 1000 + s, msg[s], 200 + s * 30);
	}
	ring = cg_sfec_ring(R);
	for (int k = 0; k < 6 && got < 2; k++) {
		w = cg_rlcw_window(&T, 0, 1000, CG_RLCW_SPAN, &top);
		CHECK_EQ(w, 40);
		sl = cg_rlcw_repair(&G, &T, top, w, &h, sym);
		CHECK(sl > 0);
		h.cls = CG_REP_STREAM;
		n = cg_rlc_rx_repair(&G, &R->rlc, &ring, &h, sym, sl, out, CG_RLC_COLS);
		for (unsigned i = 0; i < n; i++) {
			uint16_t l;
			const uint8_t *m;
			uint32_t s = out[i] - 1000;

			len = cg_sfec_check(R, &C, out[i]);
			CHECK(s == 7 || s == 23);
			CHECK_EQ(len, 200 + s * 30);
			m = cg_rep_data(&ring, out[i], &l);
			CHECK(len > 0 && !memcmp(m, msg[s], (size_t)len));
			got++;
		}
	}
	CHECK_EQ(got, 2);

	/* a rebuilt message whose CRC32C does not match is let go */
	ring.mem[(size_t)(1007 & (CG_REP_RING - 1)) * CG_SYM_PLEN + 5] ^= 1;
	CHECK_EQ(cg_sfec_check(R, &C, 1007), -1);
	CHECK(!cg_rep_has(&ring, 1007));
	CHECK_EQ(cg_sfec_check(R, &C, 1007), -1); /* not there at all */

	/* too long for its CRC32C: an empty symbol at both ends, the run whole */
	memset(big, 0x5a, sizeof(big));
	cg_sfec_add(&T, &C, 1040, big, sizeof(big), 500);
	{
		uint16_t l = 0;

		CHECK(cg_rlcw_get(&T, 1040, &l) != NULL);
		CHECK_EQ(l, 0);
		CHECK(cg_rlcw_get(&T, 1000, &l) != NULL); /* still in the run */
	}
	cg_sfec_keep(R, &C, 1040, big, sizeof(big));
	CHECK(cg_rep_has(&ring, 1040));
	CHECK_EQ(R->len[1040 & (CG_REP_RING - 1)], 0);
	CHECK_EQ(cg_sfec_check(R, &C, 1040), -1);
	free(R);
}
