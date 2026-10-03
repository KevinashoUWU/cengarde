/* SPDX-License-Identifier: GPL-2.0-only */
#include "idmap.h"
#include "test.h"

#define MAXN 8

/* Every live id resolves to its index and no dead one does. */
static void check_all(const struct cg_idmap *m, const uint32_t *ids, const int *live, int n)
{
	for (int k = 0; k < n; k++)
		CHECK_EQ(cg_idmap_get(m, ids[k]), live[k] ? k : -1);
}

void test_idmap(void)
{
	struct cg_idmap m = { 0 };
	uint32_t ids[64];
	int live[64] = { 0 }, count = 0;
	uint32_t rng = 12345;

	CHECK_EQ(cg_idmap_init(&m, MAXN), 0);
	CHECK_EQ(m.mask, 15);

	/* ids that are multiples of 16 all share home slot 0: long chains that
	 * wrap around the table end, the hard case for backward-shift deletion. */
	for (int k = 0; k < 64; k++)
		ids[k] = k < 32 ? (uint32_t)k * 16 : (uint32_t)k * 2654435761u;

	for (int step = 0; step < 20000; step++) {
		int k;

		rng = rng * 1103515245u + 12345u;
		k = (int)((rng >> 8) % 64);
		if (live[k]) {
			cg_idmap_del(&m, ids[k]);
			live[k] = 0;
			count--;
		} else if (count < MAXN) {
			cg_idmap_put(&m, ids[k], k);
			live[k] = 1;
			count++;
		}
		if (step % 7 == 0)
			check_all(&m, ids, live, 64);
	}
	check_all(&m, ids, live, 64);

	/* Removing an absent id is harmless. */
	cg_idmap_del(&m, 0xdeadbeef);
	check_all(&m, ids, live, 64);
	cg_idmap_free(&m);
}
