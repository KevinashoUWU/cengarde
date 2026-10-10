/* SPDX-License-Identifier: GPL-2.0-only */
#include "clients.h"
#include "test.h"

/* The clients a packet with hint h tries, in order, as a bit mask. */
static uint64_t chain(const struct cg_hintidx *x, uint8_t h)
{
	uint64_t m = 0;
	int steps = 0;

	for (int c = x->first[h]; c >= 0 && steps <= CG_MAX_CLIENTS; c = x->next[c], steps++)
		m |= 1ull << c;
	CHECK(steps <= CG_MAX_CLIENTS);
	return m;
}

void test_clients(void)
{
	struct cg_hintidx x;
	uint8_t hint[CG_MAX_CLIENTS], used[CG_MAX_CLIENTS];
	int prev, ordered;

	/* No client: every hint is nobody's. */
	memset(used, 0, sizeof(used));
	cg_hintidx_build(&x, hint, used, 0);
	for (int h = 0; h < 256; h++)
		CHECK_EQ(x.first[h], -1);

	/* Three clients, two sharing hint 0x2a, one slot unused in between. */
	memset(hint, 0, sizeof(hint));
	hint[0] = 0x2a, used[0] = 1;
	hint[1] = 0x2a, used[1] = 0;
	hint[2] = 0x07, used[2] = 1;
	hint[3] = 0x2a, used[3] = 1;
	cg_hintidx_build(&x, hint, used, 4);
	CHECK_EQ(chain(&x, 0x2a), (1u << 0) | (1u << 3));
	CHECK_EQ(chain(&x, 0x07), 1u << 2);
	CHECK_EQ(chain(&x, 0x00), 0);
	CHECK_EQ(x.first[0x2a], 0); /* in index order */
	CHECK_EQ(x.next[0], 3);
	CHECK_EQ(x.next[3], -1);

	/* Every slot used, all with one hint: one chain of CG_MAX_CLIENTS in
	 * order, and a rebuild forgets the previous index. */
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		hint[i] = 0xff, used[i] = 1;
	cg_hintidx_build(&x, hint, used, CG_MAX_CLIENTS);
	CHECK_EQ(chain(&x, 0xff), ~0ull);
	CHECK_EQ(chain(&x, 0x2a), 0);
	ordered = 1, prev = -1;
	for (int c = x.first[0xff]; c >= 0; c = x.next[c])
		ordered &= c > prev, prev = c;
	CHECK(ordered);

	/* Each client its own hint. */
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		hint[i] = (uint8_t)(i * 3);
	cg_hintidx_build(&x, hint, used, CG_MAX_CLIENTS);
	for (int i = 0; i < CG_MAX_CLIENTS; i++)
		CHECK_EQ(chain(&x, (uint8_t)(i * 3)), 1ull << i);

	/* Session keys: the same id of two clients differs, and the mix undoes. */
	CHECK(cg_sesskey(0x12345678, 7) != cg_sesskey(0x9abcdef0, 7));
	CHECK_EQ(cg_sesskey(0x12345678, cg_sesskey(0x12345678, 0xdeadbeef)), 0xdeadbeef);
}
