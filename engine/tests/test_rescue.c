/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "rescue.h"
#include "test.h"

#define N 8 /* entries in the test ring */

static uint8_t mem[N * CG_RESC_ENTRY];
static uint32_t sess[N], seqs[N], ia[CG_RESC_IDX], ib[CG_RESC_IDX];
static uint16_t lens[N];
static uint8_t links[N];
static uint32_t sents[N];
static uint8_t evs[N];

static void put(struct cg_resc_ring *r, uint32_t *idx, uint32_t s, uint32_t q)
{
	uint8_t *p = cg_resc_put(r, idx, s, q, 100, (uint8_t)(q % 3));

	memset(p, (int)(q & 0xff), 100);
}

static int has(const struct cg_resc_ring *r, const uint32_t *idx, uint32_t s, uint32_t q)
{
	uint16_t len = 0;
	uint8_t link = CG_RESC_NOLINK;
	const uint8_t *p = cg_resc_get(r, idx, s, q, &len, &link);

	return p && len == 100 && link == q % 3 && p[0] == (q & 0xff) && p[99] == (q & 0xff);
}

void test_rescue(void)
{
	struct cg_resc_ring r = { .mem = mem, .sess = sess, .seq = seqs, .len = lens, .link = links, .sent = sents, .ev = evs, .n = N };
	struct cg_resc_budget b = { 0 };

	memset(lens, 0, sizeof(lens));
	memset(ia, 0, sizeof(ia));
	memset(ib, 0, sizeof(ib));

	/* Nothing kept yet. */
	CHECK(!has(&r, ia, 1, 5));
	/* Two sessions, their packets interleaved, around the 32-bit wrap. */
	for (uint32_t i = 0; i < 3; i++) {
		put(&r, ia, 1, 0xfffffffeu + i);
		put(&r, ib, 2, 0xfffffffeu + i);
	}
	for (uint32_t i = 0; i < 3; i++) {
		CHECK(has(&r, ia, 1, 0xfffffffeu + i));
		CHECK(has(&r, ib, 2, 0xfffffffeu + i));
	}
	CHECK(!has(&r, ia, 2, 0xfffffffeu)); /* another session's index: not there */
	CHECK(!has(&r, ia, 1, 0xfffffffeu + 3)); /* not sent yet */
	/* The ring goes round: the oldest entries are overwritten, and their
	 * lookups miss (never another packet). */
	put(&r, ia, 1, 1);
	put(&r, ia, 1, 2);
	put(&r, ia, 1, 3);
	CHECK(!has(&r, ia, 1, 0xfffffffeu)); /* the 9th packet took its entry */
	CHECK(has(&r, ib, 2, 0xfffffffeu));  /* the next oldest is still there */
	put(&r, ia, 1, 4);
	CHECK(!has(&r, ib, 2, 0xfffffffeu)); /* until the 10th */
	CHECK(has(&r, ia, 1, 3) && has(&r, ia, 1, 4));
	/* A sequence CG_RESC_IDX later takes the same index slot. */
	put(&r, ia, 1, 3 + CG_RESC_IDX);
	CHECK(has(&r, ia, 1, 3 + CG_RESC_IDX));
	CHECK(!has(&r, ia, 1, 3));

	/* A NACK on two paths: the second asks for nothing new within
	 * CG_RESC_DUP_MS; a retry after it does. */
	CHECK(has(&r, ia, 1, 4));
	CHECK(!cg_resc_dup(&r, ia, 4, 1000));
	CHECK(cg_resc_dup(&r, ia, 4, 1005));
	CHECK(!cg_resc_dup(&r, ia, 4, 1000 + CG_RESC_DUP_MS + 1));
	put(&r, ia, 1, 4 + CG_RESC_IDX); /* a new packet in that entry: never sent again yet */
	CHECK(!cg_resc_dup(&r, ia, 4 + CG_RESC_IDX, 1006));

	/* The evidence of a loss gathers from either side, kept with the
	 * packet; a new packet in the entry starts with none. */
	CHECK_EQ(cg_resc_ev(&r, ia, 1, 4 + CG_RESC_IDX, CG_RESC_EV_LINK), CG_RESC_EV_LINK);
	CHECK_EQ(cg_resc_ev(&r, ia, 1, 4 + CG_RESC_IDX, CG_RESC_EV_ASKED), CG_RESC_EV_LINK | CG_RESC_EV_ASKED);
	CHECK_EQ(cg_resc_ev(&r, ia, 1, 4 + CG_RESC_IDX, 0), CG_RESC_EV_LINK | CG_RESC_EV_ASKED);
	CHECK_EQ(cg_resc_ev(&r, ia, 1, 4, CG_RESC_EV_LINK), 0); /* not kept any more */
	CHECK_EQ(cg_resc_ev(&r, ia, 2, 4 + CG_RESC_IDX, CG_RESC_EV_LINK), 0); /* another session's */
	put(&r, ia, 1, 4);
	CHECK_EQ(cg_resc_ev(&r, ia, 1, 4, 0), 0);

	/* How sure: the link alone where nothing rebuilds; with a code, the
	 * link and the receiver's ask; the ask alone only as the knob says;
	 * never twice. */
	CHECK_EQ(cg_resc_sure(0, 0, 1), CG_RESC_UNSURE);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK, 0, 0), CG_RESC_SURE_LINK);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK, 1, 0), CG_RESC_SURE_LATER); /* a suspect, once */
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK | CG_RESC_EV_SUSPECT, 1, 1), CG_RESC_UNSURE);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK | CG_RESC_EV_SUSPECT | CG_RESC_EV_ASKED, 1, 0), CG_RESC_SURE_LINK);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_ASKED, 1, 0), CG_RESC_UNSURE);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_ASKED, 0, 1), CG_RESC_SURE_ASKED);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK | CG_RESC_EV_ASKED, 1, 0), CG_RESC_SURE_LINK);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_LINK | CG_RESC_EV_ASKED | CG_RESC_EV_DONE, 0, 1), CG_RESC_UNSURE);
	CHECK_EQ(cg_resc_sure(CG_RESC_EV_ASKED | CG_RESC_EV_DONE, 0, 1), CG_RESC_UNSURE);

	/* The budget: pct % of what goes down, up to the cap. */
	cg_resc_earn(&b, 1400, 5);
	CHECK_EQ(b.credit, 70);
	CHECK(!cg_resc_spend(&b, 1400));
	for (int i = 0; i < 19; i++)
		cg_resc_earn(&b, 1400, 5);
	CHECK_EQ(b.credit, 1400);
	CHECK(cg_resc_spend(&b, 1400));
	CHECK_EQ(b.credit, 0);
	for (int i = 0; i < 100000; i++)
		cg_resc_earn(&b, 1400, 50);
	CHECK_EQ(b.credit, CG_RESC_CREDIT_MAX);
}
