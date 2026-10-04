/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "srvpick.h"
#include "test.h"
#include "util.h"

#define V4 (1u << AF_INET)
#define V6 (1u << AF_INET6)
#define FAILOVER 10000

static struct sockaddr_storage addr(const char *s)
{
	struct sockaddr_storage a;
	char err[128];

	CHECK_EQ(cg_addr_parse(s, 0, &a, 1, err, sizeof(err)), 1);
	return a;
}

/* A link on its sockets, as client.c drives it: cand is the address it
 * uses, opened when its socket opened, replied when the last reply came. */
struct sim {
	struct sockaddr_storage cand[CG_MAX_CANDS];
	int n, idx;
	uint64_t opened, replied;
	uint32_t silent;
};

/* reconcile at now: picks, and reopens on a move. Returns the index. */
static int step(struct sim *s, uint64_t now)
{
	int k = cg_srv_pick(s->cand, s->n, s->idx < 0 ? NULL : &s->cand[s->idx], now, s->opened, s->replied, FAILOVER,
			    &s->silent);

	if (k != s->idx) {
		s->idx = k;
		s->opened = now;
		s->replied = 0;
	}
	return k;
}

/* A verified reply at now; returns 1 when the link goes back to the first. */
static int reply(struct sim *s, uint64_t now)
{
	s->replied = now;
	if (!cg_srv_on_reply(&s->silent, s->n, s->idx))
		return 0;
	s->idx = 0;
	s->opened = now;
	s->replied = 0;
	return 1;
}

void test_srvpick(void)
{
	struct sockaddr_storage list[CG_MAX_CANDS + 8], other[4];
	struct sim s;
	uint64_t t = 1000000;
	uint32_t silent = 0;
	int n;

	list[0] = addr("[2001:db8::4]:59402");
	list[1] = addr("203.0.113.10:59402");
	list[2] = addr("[2001:db8::5]:59402");
	list[3] = addr("198.51.100.7:59402");
	list[4] = addr("203.0.113.10:59402"); /* listed twice, or a name's address listed literally */
	list[5] = addr("203.0.113.10:59403"); /* another port is another address */

	/* Candidates: in order, the families the link has, each once. */
	memset(&s, 0, sizeof(s));
	n = cg_cands_build(list, 6, V4, s.cand);
	CHECK_EQ(n, 3);
	CHECK(cg_addr_equal(&s.cand[0], &list[1]) && cg_addr_equal(&s.cand[1], &list[3]) &&
	      cg_addr_equal(&s.cand[2], &list[5]));
	CHECK_EQ(cg_cands_build(list, 6, V6, s.cand), 2);
	CHECK(cg_addr_equal(&s.cand[0], &list[0]) && cg_addr_equal(&s.cand[1], &list[2]));
	CHECK_EQ(cg_cands_build(list, 6, V4 | V6, s.cand), 5);
	CHECK(cg_addr_equal(&s.cand[4], &list[5]));
	CHECK_EQ(cg_cands_build(list, 6, 0, s.cand), 0);
	CHECK_EQ(cg_srv_pick(s.cand, 0, NULL, t, 0, 0, FAILOVER, &silent), -1);

	/* The cap: 40 addresses give 32 candidates, the first 32. */
	for (int i = 0; i < CG_MAX_CANDS + 8; i++) {
		char text[32];

		snprintf(text, sizeof(text), "10.0.%d.%d:1", i / 200, 1 + i % 200);
		list[i] = addr(text);
	}
	CHECK_EQ(cg_cands_build(list, CG_MAX_CANDS + 8, V4, s.cand), CG_MAX_CANDS);
	CHECK(cg_addr_equal(&s.cand[CG_MAX_CANDS - 1], &list[CG_MAX_CANDS - 1]));

	/* An IPv6 entry first on an IPv4-only link: skipped, the link starts on
	 * the first IPv4 one and stays while replies come. */
	memset(&s, 0, sizeof(s));
	list[0] = addr("[2001:db8::4]:59402");
	list[1] = addr("203.0.113.10:59402");
	list[2] = addr("198.51.100.7:59402");
	s.n = cg_cands_build(list, 3, V4, s.cand);
	s.idx = -1;
	CHECK_EQ(s.n, 2);
	CHECK_EQ(step(&s, t), 0);
	CHECK(cg_addr_equal(&s.cand[0], &list[1]));
	for (uint64_t now = t + 1000; now < t + 60000; now += 1000) {
		reply(&s, now);
		CHECK_EQ(step(&s, now), 0);
	}

	/* No reply for failover_ms since the last one: the next candidate. */
	t += 60000 - 1000;
	CHECK_EQ(step(&s, t + FAILOVER - 1), 0);
	CHECK_EQ(step(&s, t + FAILOVER), 1);
	CHECK_EQ(s.silent, 1);
	/* Since the socket opened when nothing ever came: then it wraps. */
	t += FAILOVER;
	CHECK_EQ(step(&s, t + FAILOVER - 1), 1);
	CHECK_EQ(step(&s, t + FAILOVER), 0);
	CHECK_EQ(s.silent, 2);
	CHECK(cg_srv_dead_round(s.silent, s.n));

	/* failover_ms 0: never moves. A single candidate never moves either. */
	CHECK_EQ(cg_srv_pick(s.cand, s.n, &s.cand[1], t + 999999, t, 0, 0, &silent), 1);
	CHECK(!cg_srv_due(t + 999999, t, 0, 0));
	silent = 0;
	CHECK_EQ(cg_srv_pick(s.cand, 1, &s.cand[0], t + 999999, t, 0, FAILOVER, &silent), 0);
	CHECK_EQ(silent, 0);
	CHECK(!cg_srv_dead_round(5, 1));

	/* The current address is kept by address: the same set in another
	 * order (a name resolved again) does not move the link. */
	other[0] = s.cand[1];
	other[1] = s.cand[0];
	silent = 0;
	CHECK_EQ(cg_srv_pick(other, 2, &s.cand[1], t + 100, t, t + 50, FAILOVER, &silent), 0);
	CHECK_EQ(cg_srv_pick(other, 2, &s.cand[0], t + 100, t, t + 50, FAILOVER, &silent), 1);
	/* No current, or one no longer in the list: the first, and the silent
	 * moves start again. */
	silent = 7;
	CHECK_EQ(cg_srv_pick(other, 2, NULL, t, t, 0, FAILOVER, &silent), 0);
	CHECK_EQ(silent, 0);
	silent = 7;
	other[2] = addr("192.0.2.99:1");
	CHECK_EQ(cg_srv_pick(other, 2, &other[2], t, t, 0, FAILOVER, &silent), 0);
	CHECK_EQ(silent, 0);
	CHECK(cg_cands_same(other, 2, other, 2));
	CHECK(!cg_cands_same(other, 2, s.cand, 2)); /* the same set, another order */
	CHECK(!cg_cands_same(other, 2, other, 1));
	CHECK(cg_cands_same(other, 0, s.cand, 0));

	/* A broken first address: the link moves on and stays on the second. */
	memset(&s, 0, sizeof(s));
	s.n = cg_cands_build(list, 3, V4, s.cand); /* A, B */
	s.idx = -1;
	t = 5000000;
	CHECK_EQ(step(&s, t), 0);
	CHECK_EQ(step(&s, t + FAILOVER), 1); /* A never answered */
	t += FAILOVER;
	for (uint64_t now = t + 500; now < t + 120000; now += 1000) {
		CHECK(!reply(&s, now));
		CHECK_EQ(step(&s, now), 1);
	}
	t += 120000;

	/* An outage of the link (every address silent for 25 s, its lease
	 * kept), on B: B, then A, then B fail, a dead round; the first reply
	 * after it, on B, sends the link back to the first. */
	t -= 500; /* the last reply */
	CHECK_EQ(step(&s, t + FAILOVER), 0);
	CHECK_EQ(s.silent, 1);
	CHECK(!cg_srv_dead_round(s.silent, s.n));
	CHECK_EQ(step(&s, t + 2 * FAILOVER), 1);
	CHECK(cg_srv_dead_round(s.silent, s.n));
	CHECK_EQ(step(&s, t + 2 * FAILOVER + 4000), 1);
	CHECK(reply(&s, t + 25000)); /* back to A */
	CHECK_EQ(s.idx, 0);
	CHECK_EQ(s.silent, 0);
	/* If A is still broken, the link moves on to B once more and stays:
	 * one silent move is no dead round. */
	CHECK_EQ(step(&s, t + 25000 + FAILOVER), 1);
	for (uint64_t now = t + 25000 + FAILOVER + 100; now < t + 25000 + 6 * FAILOVER; now += 1000) {
		CHECK(!reply(&s, now));
		CHECK_EQ(step(&s, now), 1);
	}
	CHECK_EQ(s.silent, 0);

	/* A reply that ends a dead round on the first candidate: nothing to go
	 * back to, but the count starts again. */
	silent = 2;
	CHECK(!cg_srv_on_reply(&silent, 2, 0));
	CHECK_EQ(silent, 0);
	silent = 1;
	CHECK(!cg_srv_on_reply(&silent, 2, 1)); /* half a round */
	silent = 3;
	CHECK(cg_srv_on_reply(&silent, 3, 2));

	/* Three candidates: a dead round takes three silent moves. */
	memset(&s, 0, sizeof(s));
	list[3] = addr("192.0.2.1:59402");
	s.n = cg_cands_build(list, 4, V4, s.cand); /* A, B, C */
	s.idx = -1;
	t = 9000000;
	CHECK_EQ(step(&s, t), 0);
	CHECK_EQ(step(&s, t + FAILOVER), 1);
	CHECK_EQ(step(&s, t + 2 * FAILOVER), 2);
	CHECK(!reply(&s, t + 2 * FAILOVER + 100)); /* C answers within the round: stays */
	CHECK_EQ(step(&s, t + 2 * FAILOVER + 200), 2);
	CHECK_EQ(step(&s, t + 3 * FAILOVER + 100), 0);
	CHECK_EQ(step(&s, t + 4 * FAILOVER + 100), 1);
	CHECK_EQ(step(&s, t + 5 * FAILOVER + 100), 2);
	CHECK(cg_srv_dead_round(s.silent, s.n));
	CHECK(reply(&s, t + 5 * FAILOVER + 200));
	CHECK_EQ(s.idx, 0);
}
