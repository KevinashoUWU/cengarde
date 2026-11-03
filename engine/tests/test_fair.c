/* The protocontroller's sends by host before flow (fair.h, story 031).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "fair.h"
#include "test.h"

/* Walk the groups as protoctl.c does, one segment per group a round, each
 * flow sending while it has any; count each slot's segments in rounds. */
static void walk(const uint32_t *used, uint32_t n, const uint32_t *host, const int *want, int rounds, int *got)
{
	uint32_t ord[64], start[65], cur[64] = { 0 }, ng = cg_fair_group(used, n, 0, host, ord, start);
	int left[64];

	for (int s = 0; s < 64; s++) {
		left[s] = want[s];
		got[s] = 0;
	}
	for (int r = 0; r < rounds; r++)
		for (uint32_t g = 0; g < ng; g++) {
			uint32_t len = start[g + 1] - start[g];

			for (uint32_t t = 0; t < len; t++) {
				uint32_t s = ord[start[g] + (cur[g] + t) % len];

				if (left[s] > 0) {
					left[s]--;
					got[s]++;
					cur[g] = (cur[g] + t + 1) % len;
					break;
				}
			}
		}
}

void test_fair(void)
{
	uint32_t host[64] = { 0 }, used[64], ord[64], start[65];
	int want[64], got[64];
	uint8_t a[16] = { 192, 168, 1, 10 }, b[16] = { 192, 168, 1, 11 };

	CHECK(cg_fair_key(a) != cg_fair_key(b));
	CHECK_EQ(cg_fair_key(a), cg_fair_key(a));

	/* host 1 with 8 flows (slots 0..7), host 2 with one (slot 8) */
	for (uint32_t i = 0; i < 9; i++) {
		used[i] = i;
		host[i] = i < 8 ? 1 : 2;
	}
	CHECK_EQ(cg_fair_group(used, 9, 0, host, ord, start), 2);
	CHECK(start[0] == 0 && start[1] == 8 && start[2] == 9);
	CHECK_EQ(ord[8], 8);
	/* rotated: the order the rotation puts first */
	CHECK_EQ(cg_fair_group(used, 9, 8, host, ord, start), 2);
	CHECK(ord[0] == 8 && start[1] == 1 && ord[1] == 0 && ord[8] == 7);

	/* everyone wants 100 segments, 100 rounds: the lone flow gets half */
	for (int s = 0; s < 64; s++)
		want[s] = 100;
	walk(used, 9, host, want, 100, got);
	CHECK_EQ(got[8], 100);
	{
		int one = 0;

		for (int s = 0; s < 8; s++)
			one += got[s];
		CHECK_EQ(one, 100);  /* host 1's eight share its half */
		CHECK(got[0] >= 12 && got[0] <= 13);
	}
	/* a host that cannot send leaves its turns to the others */
	want[8] = 0;
	walk(used, 9, host, want, 100, got);
	CHECK_EQ(got[8], 0);

	/* one flow a host: as plain round robin */
	for (uint32_t i = 0; i < 5; i++)
		host[i] = 10 + i;
	CHECK_EQ(cg_fair_group(used, 5, 2, host, ord, start), 5);
	CHECK(ord[0] == 2 && ord[4] == 1);

	/* no flows */
	CHECK_EQ(cg_fair_group(used, 0, 0, host, ord, start), 0);
	CHECK_EQ(start[0], 0);
}
