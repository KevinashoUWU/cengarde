/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "epoch.h"
#include "test.h"

#define THRESHOLD 2000 /* 2 x probe_idle_ms */

/* Probes on one link every interval_ms from t, n of them; the header ts is
 * the microsecond clock, as send_probe writes it. */
static void probes(struct cg_echo *r, uint64_t t, uint32_t interval_ms, int n)
{
	for (int i = 0; i < n; i++, t += interval_ms)
		cg_echo_push(r, (uint32_t)(t * 1000), t);
}

/* What the client does with a reply the window judged v: the echo is taken
 * only once the MAC verified. */
static int reply(struct cg_echo *r, enum cg_replay_verdict v, uint8_t type, int mac_ok, uint64_t sent_ms,
		 uint64_t now_ms, uint64_t last_new_ms)
{
	int echo = mac_ok && cg_echo_take(r, (uint32_t)(sent_ms * 1000), now_ms, CG_ECHO_MAX_AGE_MS);

	return cg_restart_reply(v, type, mac_ok, echo, now_ms - last_new_ms, THRESHOLD);
}

void test_epoch(void)
{
	static struct cg_echo a, b;
	const uint64_t t = 5000000; /* ms */

	/* OLD, verified, answering a recent probe, nothing NEW for 2 s: reset. */
	memset(&a, 0, sizeof(a));
	probes(&a, t, 100, 30);
	CHECK(reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 2900, t + 2950, t + 950));
	CHECK(cg_restart_reply(CG_RP_OLD, CG_T_PROBE_REPLY, 1, 1, THRESHOLD, THRESHOLD));

	/* Any one condition missing: no reset. */
	CHECK(!cg_restart_reply(CG_RP_NEW, CG_T_PROBE_REPLY, 1, 1, 60000, THRESHOLD));
	CHECK(!cg_restart_reply(CG_RP_DUP, CG_T_PROBE_REPLY, 1, 1, 60000, THRESHOLD));
	CHECK(!cg_restart_reply(CG_RP_OLD, CG_T_PROBE_REPLY, 0, 1, 60000, THRESHOLD));
	CHECK(!cg_restart_reply(CG_RP_OLD, CG_T_PROBE_REPLY, 1, 0, 60000, THRESHOLD));
	CHECK(!cg_restart_reply(CG_RP_OLD, CG_T_PROBE_REPLY, 1, 1, THRESHOLD - 1, THRESHOLD));
	CHECK(!reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 2800, t + 2950, t + 1000)); /* NEW 1950 ms ago */
	CHECK(!reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 2900, t + 2960, t)); /* answered already */
	CHECK(!reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 2950, t + 2960, t)); /* never sent */

	/* A reply that fails the MAC leaves the probe for the genuine one. */
	CHECK(!reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 0, t + 2700, t + 2950, t));
	CHECK(reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 2700, t + 2950, t));

	/* A DATA (or a probe) never resets, whatever else holds. */
	CHECK(!cg_restart_reply(CG_RP_OLD, CG_T_DATA, 1, 1, 60000, THRESHOLD));
	CHECK(!cg_restart_reply(CG_RP_OLD, CG_T_PROBE, 1, 1, 60000, THRESHOLD));

	/* A link more than the window behind (3 s of queue) while another link
	 * keeps delivering NEW: its old replies are genuine and answer its own
	 * probes, but no reset. Once nothing NEW comes for 2 s, the next one
	 * does reset. */
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	probes(&a, t, 100, 60);
	probes(&b, t, 100, 60);
	for (uint64_t now = t + 3000; now < t + 6000; now += 100)
		CHECK(!reply(&b, CG_RP_OLD, CG_T_PROBE_REPLY, 1, now - 3000, now, now - 50));
	CHECK(reply(&b, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 3000, t + 6000, t + 4000));
	/* The echo has to be in the ring of the link the reply came on. */
	memset(&b, 0, sizeof(b));
	CHECK(!reply(&b, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 5000, t + 6000, t));
	CHECK(reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 5000, t + 6000, t));

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
	CHECK(!reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 1000, t + 11001, t));

	/* An RTT of 1.5 s with probes every 100 ms (15 probes out) matches; so
	 * does the oldest probe in the ring, 64 back, and the 65th back was
	 * overwritten. */
	memset(&a, 0, sizeof(a));
	probes(&a, t, 100, 100); /* t .. t + 9900 */
	CHECK(reply(&a, CG_RP_OLD, CG_T_PROBE_REPLY, 1, t + 8500, t + 10000, t + 7000));
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
