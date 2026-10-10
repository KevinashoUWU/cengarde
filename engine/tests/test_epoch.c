/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "epoch.h"
#include "test.h"

/* Probes on one link every interval_ms from t, n of them; the header ts is
 * the microsecond clock, as send_probe writes it. */
static void probes(struct cg_echo *r, uint64_t t, uint32_t interval_ms, int n)
{
	for (int i = 0; i < n; i++, t += interval_ms)
		cg_echo_push(r, (uint32_t)(t * 1000), t);
}

void test_epoch(void)
{
	static struct cg_echo a;
	const uint64_t t = 5000000; /* ms */

	/* Each echo counts once. */
	memset(&a, 0, sizeof(a));
	probes(&a, t, 100, 10);
	CHECK(cg_echo_take(&a, (uint32_t)((t + 500) * 1000), t + 600, CG_ECHO_MAX_AGE_MS));
	CHECK(!cg_echo_take(&a, (uint32_t)((t + 500) * 1000), t + 600, CG_ECHO_MAX_AGE_MS));
	CHECK(cg_echo_take(&a, (uint32_t)((t + 400) * 1000), t + 600, CG_ECHO_MAX_AGE_MS));

	/* At most 10 s old: the boundary matches, one ms later does not. */
	memset(&a, 0, sizeof(a));
	probes(&a, t, 1000, 2);
	CHECK(cg_echo_take(&a, (uint32_t)(t * 1000), t + CG_ECHO_MAX_AGE_MS, CG_ECHO_MAX_AGE_MS));
	CHECK(!cg_echo_take(&a, (uint32_t)((t + 1000) * 1000), t + 1000 + CG_ECHO_MAX_AGE_MS + 1, CG_ECHO_MAX_AGE_MS));

	/* An RTT of 1.5 s with probes every 100 ms (15 probes out) matches; so
	 * does the oldest probe in the ring, 64 back, and the 65th back was
	 * overwritten. */
	memset(&a, 0, sizeof(a));
	probes(&a, t, 100, 100); /* t .. t + 9900 */
	CHECK(cg_echo_take(&a, (uint32_t)((t + 8500) * 1000), t + 10000, CG_ECHO_MAX_AGE_MS));
	CHECK(cg_echo_take(&a, (uint32_t)((t + 3600) * 1000), t + 10000, CG_ECHO_MAX_AGE_MS));
	CHECK(!cg_echo_take(&a, (uint32_t)((t + 3500) * 1000), t + 10000, CG_ECHO_MAX_AGE_MS));

	/* An empty ring matches nothing, ts 0 included. */
	memset(&a, 0, sizeof(a));
	CHECK(!cg_echo_take(&a, 0, 0, CG_ECHO_MAX_AGE_MS));
	CHECK(!cg_echo_take(&a, 0, 5000, CG_ECHO_MAX_AGE_MS));

	/* The 32-bit millisecond clock wraps between the probe and its reply. */
	memset(&a, 0, sizeof(a));
	cg_echo_push(&a, 0x1234, 0xffffff00ull);
	CHECK(!cg_echo_take(&a, 0x1234, 0x100000000ull + 20000, CG_ECHO_MAX_AGE_MS));
	CHECK(cg_echo_take(&a, 0x1234, 0x100000000ull + 100, CG_ECHO_MAX_AGE_MS));
}
