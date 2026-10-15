/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "rescue.h"
#include "test.h"

#define N 8 /* entries in the test ring */

static uint8_t mem[N * CG_RESC_ENTRY];
static uint32_t sess[N], seqs[N], ia[CG_RESC_IDX], ib[CG_RESC_IDX];
static uint16_t lens[N];
static uint8_t links[N];

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
	struct cg_resc_ring r = { .mem = mem, .sess = sess, .seq = seqs, .len = lens, .link = links, .n = N };
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
