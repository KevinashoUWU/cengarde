/* The receiver's reorderer (reorder.h): the scenarios of story 012's step 1
 * plan, each run at two bases (the second wraps sequences, arrival times and
 * the sender's clock within its first second), then random streams against
 * a slow model of the same rules.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h" /* CG_BUF */
#include "reorder.h"
#include "test.h"

_Static_assert(CG_RO_BUF == CG_BUF, "a pool buffer holds what a receive buffer holds");

#define MS 1000u
#define BIG 1400
#define SMALL 64
#define SPLIT CG_F_SPLIT
#define DATA 0   /* a packet that may wait (cg_ro_pkt.nohold 0) */
#define NOHOLD 1 /* one that never waits: a protocontroller stream's message */
#define N CG_RO_SLOTS
#define LEAP (1u << 20) /* CG_SEQ_LEAP: a server back from a restart */

static uint8_t pool_mem[(size_t)CG_RO_SLOTS * CG_RO_BUF];
static uint16_t pool_stack[CG_RO_SLOTS];

static uint32_t rnd_state = 0x2545f491u;

static uint32_t rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

static uint32_t rn(uint32_t n)
{
	return rnd() % n;
}

/* ---- a reorderer and its caller ---- */

struct base {
	uint32_t sb, tb, tsb; /* sequence, local clock, sender's clock */
};

static const struct base bases[] = {
	{ 1000, 5000000, 0x10000 },
	{ 0xffffffffu - 1000, 0xffffffffu - 900000, 0xffffffffu - 600000 },
};

#define OUT_MAX 70000

static struct {
	struct cg_reorder r;
	struct cg_ro_pool p;
	struct cg_ro_cfg cf;
	uint16_t rset;
	struct base b;
	uint32_t delay[CG_MAX_LINKS]; /* us */
	uint32_t out[OUT_MAX];        /* sequences handed on, from b.sb */
	char how[OUT_MAX];            /* 'n' a NOW verdict, 'r' cg_ro_next, 'f' cg_ro_flush */
	unsigned nout, agains, bad;
	uint16_t back[CG_RO_SLOTS];
} R;

static void rig(unsigned pool_n, const struct base *b, uint32_t max_wait, uint32_t dup_wait, uint32_t margin,
		uint32_t bypass, uint16_t rset)
{
	cg_ro_init(&R.r);
	CHECK_EQ(cg_ro_pool_init(&R.p, pool_mem, pool_stack, pool_n), 0);
	R.cf = (struct cg_ro_cfg){ .max_wait_us = max_wait, .dup_wait_us = dup_wait, .margin_us = margin,
				   .bypass_len = bypass };
	R.rset = rset;
	R.b = *b;
	memset(R.delay, 0, sizeof(R.delay));
	R.nout = R.agains = R.bad = 0;
}

static uint8_t pat(uint32_t seq, unsigned i)
{
	return (uint8_t)(seq * 131u + i * 7u + (seq >> 13));
}

/* What the caller copies in on HOLD, as far as the tests look at it. */
static void fill(uint8_t *b, uint32_t seq, unsigned len)
{
	for (unsigned i = 0; i < len && i < 12; i++)
		b[i] = pat(seq, i);
	if (len > 12)
		b[len - 1] = pat(seq, len - 1);
}

static int intact(const uint8_t *b, uint32_t seq, unsigned len)
{
	for (unsigned i = 0; i < len && i < 12; i++)
		if (b[i] != pat(seq, i))
			return 0;
	return len <= 12 || b[len - 1] == pat(seq, len - 1);
}

static void emit(uint32_t seq, char how)
{
	if (R.nout < OUT_MAX) {
		R.out[R.nout] = seq - R.b.sb;
		R.how[R.nout] = how;
	}
	R.nout++;
}

/* The caller's drain at local time t: everything that may go, in order; the
 * buffers come back after the send of the whole drain. */
static unsigned drain(uint32_t t)
{
	struct cg_ro_out o;
	unsigned n = 0;

	while (cg_ro_next(&R.r, &R.cf, R.rset, R.b.tb + t, &o)) {
		R.bad += !intact(cg_ro_pool_buf(&R.p, o.buf), o.seq, o.len);
		emit(o.seq, 'r');
		R.back[n++] = o.buf;
	}
	for (unsigned i = 0; i < n; i++)
		CHECK_EQ(cg_ro_pool_put(&R.p, R.back[i]), 0);
	return n;
}

static unsigned flush_all(void)
{
	struct cg_ro_out o;
	unsigned n = 0;

	while (cg_ro_flush(&R.r, &o)) {
		R.bad += !intact(cg_ro_pool_buf(&R.p, o.buf), o.seq, o.len);
		emit(o.seq, 'f');
		R.back[n++] = o.buf;
	}
	for (unsigned i = 0; i < n; i++)
		CHECK_EQ(cg_ro_pool_put(&R.p, R.back[i]), 0);
	return n;
}

static struct cg_ro_pkt pkt(uint32_t seq, uint32_t ts, uint32_t at, unsigned link, uint8_t flags, uint8_t type,
			    uint16_t len)
{
	return (struct cg_ro_pkt){ .seq = R.b.sb + seq, .ts = R.b.tsb + ts, .t_arr = R.b.tb + at, .len = len,
				   .link = (uint8_t)link, .flags = flags, .nohold = type,
				   .prev = (uint8_t)(cg_prev_link(flags) + 1) };
}

/* cg_ro_in alone, as a caller that has not drained yet. */
static enum cg_ro_v raw_in(uint32_t seq, uint32_t at, unsigned link, uint8_t flags, uint16_t len)
{
	struct cg_ro_pkt k = pkt(seq, at, at, link, flags, DATA, len);
	uint16_t buf;
	enum cg_ro_v v = cg_ro_in(&R.r, &R.p, &R.cf, &k, R.rset, &buf);

	if (v == CG_RO_HOLD)
		fill(cg_ro_pool_buf(&R.p, buf), k.seq, len);
	else if (v == CG_RO_NOW)
		emit(k.seq, 'n');
	return v;
}

/* One packet as the caller handles it: AGAIN drains and tries once more,
 * HOLD copies, NOW goes; then a drain at its arrival. */
static enum cg_ro_v in_pkt(struct cg_ro_pkt k, uint32_t at)
{
	uint16_t buf;
	uint16_t len = k.len;
	enum cg_ro_v v = cg_ro_in(&R.r, &R.p, &R.cf, &k, R.rset, &buf);

	if (v == CG_RO_AGAIN) {
		R.agains++;
		drain(at);
		v = cg_ro_in(&R.r, &R.p, &R.cf, &k, R.rset, &buf);
		CHECK(v != CG_RO_AGAIN);
	}
	if (v == CG_RO_HOLD) {
		CHECK(buf < R.p.n);
		fill(cg_ro_pool_buf(&R.p, buf), k.seq, len);
	} else {
		CHECK_EQ(buf, CG_RO_NOBUF);
		emit(k.seq, 'n');
	}
	drain(at);
	return v;
}

static enum cg_ro_v in(uint32_t seq, uint32_t ts, uint32_t at, unsigned link, uint8_t flags, uint8_t type,
		       uint16_t len)
{
	return in_pkt(pkt(seq, ts, at, link, flags, type, len), at);
}

/* A TCP retransmission marked free (retx 1), or a free ACK of full size
 * (retx 0, as a caller that found no TCP data), on link 0. */
static enum cg_ro_v in_free(uint32_t seq, uint32_t ts, uint32_t at, uint8_t retx)
{
	struct cg_ro_pkt k = pkt(seq, ts, at, 0, CG_F_FREE, DATA, BIG);

	k.retx = retx;
	return in_pkt(k, at);
}

/* A full-size data packet sent at ts on link: it arrives delay[link] later. */
static enum cg_ro_v sendp(uint32_t seq, uint32_t ts, unsigned link, uint8_t flags)
{
	return in(seq, ts, ts + R.delay[link], link, flags, DATA, BIG);
}

/* A verified control message on link that took delay. */
static void owd(unsigned link, uint32_t delay)
{
	cg_ro_owd(&R.r, link, R.b.tb - R.b.tsb + delay);
}

static int32_t due(uint32_t t)
{
	return cg_ro_due_us(&R.r, &R.cf, R.rset, R.b.tb + t);
}

/* The rule at the top of reorder.h: every PASSED slot is ahead of the head,
 * within the window, and passed_ahead counts them. */
static int marks_ok(const struct cg_reorder *r)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < CG_RO_SLOTS; i++) {
		const struct cg_ro_slot *s = &r->slot[i];

		if (s->state != CG_RO_PASSED)
			continue;
		if (s->seq % CG_RO_SLOTS != i || s->seq - r->head >= CG_RO_SLOTS)
			return 0;
		n++;
	}
	return n == r->passed_ahead;
}

/* What was handed on, from the k-th packet on, is want[0..n-1] and nothing more. */
static int out_from(unsigned k, const uint32_t *want, unsigned n)
{
	if (R.nout != k + n || R.nout > OUT_MAX)
		return 0;
	for (unsigned i = 0; i < n; i++)
		if (R.out[k + i] != want[i])
			return 0;
	return 1;
}

#define OUT_IS(...) out_from(0, (const uint32_t[]){ __VA_ARGS__ }, sizeof((uint32_t[]){ __VA_ARGS__ }) / 4)
#define OUT_TAIL(k, ...) out_from(k, (const uint32_t[]){ __VA_ARGS__ }, sizeof((uint32_t[]){ __VA_ARGS__ }) / 4)

/* The counters that partition the packets taken. */
static uint64_t taken(const struct cg_ro_stats *s)
{
	return s->in_order + s->held_total + s->bypassed + s->late_rack + s->late_cap + s->late_overflow +
	       s->late_old + s->far + s->resyncs + s->pool_full;
}

static uint64_t lost(const struct cg_ro_stats *s)
{
	return s->lost_rack + s->lost_cap + s->lost_overflow + s->lost_flush;
}

static uint64_t hsum(const uint64_t h[CG_RO_HIST])
{
	uint64_t n = 0;

	for (unsigned i = 0; i < CG_RO_HIST; i++)
		n += h[i];
	return n;
}

static void shuffle(uint32_t *a, unsigned n)
{
	for (unsigned i = n; i > 1; i--) {
		unsigned j = rn(i);
		uint32_t t = a[i - 1];

		a[i - 1] = a[j];
		a[j] = t;
	}
}

/* ---- scenarios ---- */

/* 1: in order, on two links of equal delay: only NOW, nothing copied. */
static void sc_in_order(const struct base *b)
{
	unsigned now_n = 0, ok = 1;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = R.delay[1] = 5 * MS;
	for (uint32_t i = 0; i < 10000; i++)
		now_n += sendp(i, i * 10, i % 2, i % 3 ? 0 : SPLIT) == CG_RO_NOW;
	CHECK_EQ(now_n, 10000);
	CHECK_EQ(R.nout, 10000);
	for (uint32_t i = 0; i < 10000; i++)
		ok &= R.out[i] == i && R.how[i] == 'n';
	CHECK(ok);
	CHECK_EQ(R.r.st.in_order, 10000);
	CHECK_EQ(R.r.st.split_rx, 3334);
	CHECK_EQ(R.r.st.held_total + R.r.st.held_max + lost(&R.r.st), 0);
	CHECK_EQ(R.r.head, R.b.sb + 10000);
	CHECK_EQ(R.p.nfree, CG_RO_SLOTS);
	CHECK_EQ(R.p.full, 0);
	CHECK_EQ(due(200 * MS), INT32_MAX);
	CHECK_EQ(R.r.link[0].owd, R.b.tb - R.b.tsb + 5 * MS); /* the same sample every time: exact */
	CHECK_EQ(R.r.link[1].hw, R.b.sb + 9999);
}

/* 2: swaps and permutations within the window come out in order, and the
 * wait is the time held. */
static void sc_permute(const struct base *b)
{
	static const unsigned widths[] = { 2, 3, 8, 64, 1000, CG_RO_SLOTS };
	static uint32_t perm[CG_RO_SLOTS];

	/* 2 waits 3 ms for 1. No rset: no RACK, no delays, dl = t_arr + C. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	CHECK_EQ(in(0, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 0, 1 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(1 * MS), 200 * MS);
	CHECK_EQ(in(1, 0, 4 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_IS(0, 1, 2));
	CHECK_EQ(R.how[2], 'r');
	CHECK_EQ(R.r.st.wait[2], 1); /* (2, 5] ms */
	CHECK_EQ(hsum(R.r.st.wait), 1);
	CHECK_EQ(R.bad, 0);

	for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		unsigned holds = 0, ok = 1, n = 1 + 3 * widths[w];
		uint32_t t = 0;

		rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
		in(0, 0, 0, 0, SPLIT, DATA, BIG);
		for (unsigned blk = 0; blk < 3; blk++) {
			for (unsigned i = 0; i < widths[w]; i++)
				perm[i] = 1 + blk * widths[w] + i;
			shuffle(perm, widths[w]);
			for (unsigned i = 0; i < widths[w]; i++, t++)
				holds += in(perm[i], 0, t, perm[i] % 3, SPLIT, DATA, (uint16_t)(200 + perm[i] % 1200)) ==
					 CG_RO_HOLD;
		}
		CHECK_EQ(R.nout, n);
		for (unsigned i = 0; i < n && i < OUT_MAX; i++)
			ok &= R.out[i] == i;
		CHECK(ok);
		CHECK_EQ(R.r.st.held_total, holds);
		CHECK_EQ(hsum(R.r.st.wait), holds);
		CHECK_EQ(lost(&R.r.st) + R.r.st.late_old + R.r.st.late_cap + R.agains, 0);
		CHECK_EQ(R.r.held, 0);
		CHECK_EQ(R.p.nfree, CG_RO_SLOTS);
		CHECK_EQ(R.bad, 0);
		if (widths[w] <= 64)
			CHECK_EQ(R.r.st.wait[0], holds); /* within 64 us */
	}

	/* Histogram edges: inclusive upper edges. */
	CHECK_EQ(cg_ro_bucket(0), 0);
	CHECK_EQ(cg_ro_bucket(1000), 0);
	CHECK_EQ(cg_ro_bucket(1001), 1);
	CHECK_EQ(cg_ro_bucket(20000), 4);
	CHECK_EQ(cg_ro_bucket(500000), 8);
	CHECK_EQ(cg_ro_bucket(500001), 9);
	CHECK_EQ(cg_ro_bucket(UINT32_MAX), 9);
}

struct ev {
	uint32_t seq, at;
	unsigned link;
};

static int ev_cmp(const void *a, const void *b)
{
	const struct ev *x = a, *y = b;

	return x->at != y->at ? (x->at < y->at ? -1 : 1) : (x->seq < y->seq ? -1 : x->seq > y->seq);
}

/* 3: two links 50 ms apart, split: in order, each early packet waits for
 * the slow link; a gap goes at its deadline, not a microsecond before. */
static void sc_spread(const struct base *b)
{
	static struct ev e[400];
	unsigned ok = 1;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = 10 * MS;
	R.delay[1] = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 60 * MS);
	for (uint32_t k = 0; k < 400; k++)
		e[k] = (struct ev){ k, k * MS + R.delay[k % 2], k % 2 };
	qsort(e, 400, sizeof(e[0]), ev_cmp);
	for (unsigned i = 0; i < 400; i++)
		in(e[i].seq, e[i].seq * MS, e[i].at, e[i].link, SPLIT, DATA, BIG);
	CHECK_EQ(R.nout, 400);
	for (uint32_t i = 0; i < 400; i++)
		ok &= R.out[i] == i;
	CHECK(ok);
	CHECK_EQ(lost(&R.r.st), 0);
	CHECK_EQ(R.r.st.held_total, 199); /* the even ones but 0 */
	CHECK_EQ(R.r.st.wait[5], 199);    /* 49 ms each: (20, 50] */
	CHECK_EQ(R.r.held, 0);

	/* Seq 1 lost on the slow link: dl = ts(2) + M + margin = 2 + 60 + 10. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = 10 * MS;
	R.delay[1] = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 60 * MS);
	sendp(0, 0, 0, SPLIT);
	CHECK_EQ(sendp(2, 2 * MS, 0, SPLIT), CG_RO_HOLD); /* at 12 ms */
	CHECK_EQ(due(12 * MS), 60 * MS);
	CHECK_EQ(due(72 * MS - 1), 1);
	CHECK_EQ(drain(72 * MS - 1), 0);
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(drain(72 * MS), 1);
	CHECK_EQ(R.r.st.lost_cap, 1);
	CHECK(OUT_IS(0, 2));
	CHECK_EQ(R.r.st.wait[6], 1); /* 60 ms */
	CHECK_EQ(due(72 * MS), INT32_MAX);

	/* The cap below the spread: dl = t_arr + 40 ms. */
	rig(CG_RO_SLOTS, b, 40 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = 10 * MS;
	owd(0, 10 * MS);
	owd(1, 60 * MS);
	sendp(0, 0, 0, SPLIT);
	sendp(2, 2 * MS, 0, SPLIT);
	CHECK_EQ(due(12 * MS), 40 * MS);
	CHECK_EQ(drain(52 * MS - 1), 0);
	CHECK_EQ(drain(52 * MS), 1);
}

/* 3b: a link whose delay swings: its packets come up to tens of ms above
 * its mean, and the deadline waits for them (story 013); a packet lost on
 * it still goes, by the cap at the latest. */
static void sc_swing(const struct base *b)
{
	uint32_t m;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = 10 * MS;
	for (int i = 0; i < 40; i++) {
		owd(0, 10 * MS);
		owd(1, (i & 1 ? 80 : 40) * MS); /* a mean of 60, 20 off either way */
	}
	CHECK(R.r.link[0].dev == 0 && R.r.link[1].dev > 18 * MS && R.r.link[1].dev < 23 * MS);
	CHECK(cg_ro_owd_max(&R.r, 0x3, &m) && m - (R.b.tb - R.b.tsb) > 120 * MS);
	sendp(0, 0, 0, SPLIT);
	CHECK_EQ(sendp(2, 2 * MS, 0, SPLIT), CG_RO_HOLD);
	/* Seq 1, on the slow link 35 ms above its mean: past the old deadline
	 * (ts(2) + 60 + 10 = 72 ms), within the new one. */
	CHECK_EQ(drain(95 * MS), 0);
	in(1, 1 * MS, 96 * MS, 1, SPLIT, DATA, BIG);
	CHECK(OUT_IS(0, 1, 2));
	CHECK_EQ(lost(&R.r.st), 0);

	/* Lost for good: it goes at its deadline, never after the cap. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	R.delay[0] = 10 * MS;
	for (int i = 0; i < 40; i++) {
		owd(0, 10 * MS);
		owd(1, (i & 1 ? 80 : 40) * MS);
	}
	sendp(0, 0, 0, SPLIT);
	sendp(2, 2 * MS, 0, SPLIT);
	CHECK(due(12 * MS) > (int32_t)(100 * MS) && due(12 * MS) <= (int32_t)(200 * MS));
	CHECK_EQ(drain(212 * MS), 1);
	CHECK_EQ(R.r.st.lost_cap, 1);
	CHECK(OUT_IS(0, 2));
	/* It comes after all: late, and counted on the link that brought it. */
	in(1, 1 * MS, 300 * MS, 1, SPLIT, DATA, BIG);
	CHECK(R.r.st.late_cap == 1 && R.r.link[1].late == 1 && R.r.link[0].late == 0);
}

/* 4: RACK. */
/* by_link (CG_F_PREV): a gap waits for the link the packet after it names,
 * not the slowest of rset. Links 0 (10 ms) and 1 (150 ms). */
static void sc_by_link(const struct base *b)
{
	const uint8_t SP = SPLIT;

	for (int on = 0; on < 2; on++) {
		/* A: seq 1 went on the fast link and is lost; seq 2, on the slow one,
		 * names it. With by_link the gap is past link 0's deadline when seq 2
		 * comes (2 + 10 + 10 ms): it goes at once. Without, it waits for the
		 * slowest (2 + 150 + 10 ms). */
		rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
		R.cf.by_link = (uint8_t)on;
		owd(0, 10 * MS);
		owd(1, 150 * MS);
		CHECK_EQ(in(0, 0, 10 * MS, 0, SP, DATA, BIG), CG_RO_NOW);
		CHECK_EQ(in(2, 2 * MS, 152 * MS, 1, SP | cg_prev_flags(0), DATA, BIG), on ? CG_RO_NOW : CG_RO_HOLD);
		CHECK_EQ(R.r.st.lost_cap, (uint64_t)on);
		if (!on) {
			CHECK(due(152 * MS) > 0);
			drain(163 * MS);
			CHECK_EQ(R.r.st.lost_cap, 1);
		}
		CHECK_EQ(R.r.head, b->sb + 3);

		/* B: seq 1 lost on link 0, and link 0 brings seq 2 (FIFO): the gap is
		 * lost now (RACK on its link), where the slow link still says nothing. */
		rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
		R.cf.by_link = (uint8_t)on;
		owd(0, 10 * MS);
		owd(1, 150 * MS);
		CHECK_EQ(in(0, 0, 10 * MS, 0, SP, DATA, BIG), CG_RO_NOW);
		CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SP | cg_prev_flags(0), DATA, BIG), on ? CG_RO_NOW : CG_RO_HOLD);
		CHECK_EQ(R.r.st.lost_rack, (uint64_t)on);
		if (on)
			CHECK_EQ(due(12 * MS), INT32_MAX);

		/* C: seq 1 went on the slow link and comes: named, it waits for that
		 * link, as without by_link, and nothing is lost. */
		rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
		R.cf.by_link = (uint8_t)on;
		owd(0, 10 * MS);
		owd(1, 150 * MS);
		CHECK_EQ(in(0, 0, 10 * MS, 0, SP, DATA, BIG), CG_RO_NOW);
		CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SP | cg_prev_flags(1), DATA, BIG), CG_RO_HOLD);
		CHECK(due(12 * MS) > (int32_t)(100 * MS));
		CHECK_EQ(drain(100 * MS), 0);
		CHECK_EQ(in(1, 1 * MS, 151 * MS, 1, SP | cg_prev_flags(0), DATA, BIG), CG_RO_NOW);
		CHECK_EQ(R.r.head, b->sb + 3);
		CHECK_EQ(R.r.st.lost_cap + R.r.st.lost_rack, 0);
		CHECK_EQ(R.nout, 3);
	}
}

/* Rescue: a gap about to be given up is asked for (r.ask), and waits
 * rescue_us more. Links 0 (10 ms) and 1 (30 ms). */
static void sc_rescue(const struct base *b)
{
	for (int come = 0; come < 2; come++) {
		/* seq 1 lost; seq 2 at 12 ms waits until 2 + 30 + 10 = 42 ms. */
		rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
		R.cf.rescue_us = 60 * MS;
		owd(0, 10 * MS);
		owd(1, 30 * MS);
		CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
		CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
		CHECK_EQ(R.r.nask, 0);
		CHECK_EQ(drain(41 * MS), 0);
		CHECK_EQ(R.r.nask, 0);
		/* Due: asked for, not given up. */
		CHECK_EQ(drain(43 * MS), 0);
		CHECK_EQ(R.r.nask, 1);
		CHECK_EQ(R.r.ask[0], b->sb + 1);
		CHECK_EQ(R.r.st.asked, 1);
		CHECK_EQ(R.r.st.lost_cap, 0);
		CHECK(due(43 * MS) > (int32_t)(55 * MS) && due(43 * MS) <= (int32_t)(60 * MS));
		R.r.nask = 0; /* the caller sent the NACK */
		CHECK_EQ(drain(80 * MS), 0);
		CHECK_EQ(R.r.nask, 0); /* asked once */
		if (come) {
			/* The copy comes on the fast link: in order, nothing lost. */
			CHECK_EQ(in(1, 1 * MS, 90 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
			CHECK_EQ(R.r.st.rescued, 1);
			CHECK_EQ(R.r.st.lost_cap + R.r.st.lost_rack + R.r.st.ask_lost, 0);
		} else {
			/* It does not come: given up at 43 + 60 ms, as due then (cap). */
			CHECK_EQ(drain(102 * MS), 0);
			CHECK_EQ(drain(104 * MS), 1);
			CHECK_EQ(R.r.st.ask_lost, 1);
			CHECK_EQ(R.r.st.lost_cap, 1);
			CHECK_EQ(R.r.st.rescued, 0);
		}
		CHECK_EQ(R.r.head, b->sb + 3);
	}

	/* rescue_tries 2: asked for again at half the wait (ask2), once. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	R.cf.rescue_tries = 2;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(43 * MS), 0);
	CHECK(R.r.nask == 1 && R.r.nask2 == 0);
	R.r.nask = 0;
	CHECK(due(43 * MS) > 0 && due(43 * MS) <= (int32_t)(30 * MS)); /* the retry at 73 ms */
	CHECK_EQ(drain(72 * MS), 0);
	CHECK_EQ(R.r.nask2, 0);
	CHECK_EQ(drain(74 * MS), 0);
	CHECK_EQ(R.r.nask2, 1);
	CHECK_EQ(R.r.ask2[0], b->sb + 1);
	R.r.nask2 = 0;
	CHECK_EQ(drain(90 * MS), 0);
	CHECK_EQ(R.r.nask2, 0); /* twice at most */
	CHECK_EQ(R.r.st.asked, 2);
	CHECK_EQ(drain(104 * MS), 1); /* given up at 43 + 60 ms */
	CHECK_EQ(R.r.st.ask_lost, 1);

	/* The sender says it will not come (CG_F_GONE): it goes at once, not
	 * at 43 + 60 ms, and is asked for no more. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	R.cf.rescue_tries = 2;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(43 * MS), 0);
	R.r.nask = 0;
	CHECK(!cg_ro_refused(&R.r, &R.cf, b->sb + 5, R.b.tb + 50 * MS)); /* not asked for */
	CHECK(cg_ro_refused(&R.r, &R.cf, b->sb + 1, R.b.tb + 50 * MS));
	CHECK(!cg_ro_refused(&R.r, &R.cf, b->sb + 1, R.b.tb + 50 * MS)); /* once */
	CHECK_EQ(due(50 * MS), 0);
	CHECK_EQ(drain(50 * MS), 1);
	CHECK(R.r.st.ask_refused == 1 && R.r.st.ask_lost == 1 && R.r.nask2 == 0);
	CHECK_EQ(R.r.head, b->sb + 3);

	/* Several gaps before the held packet: asked for together. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(4, 4 * MS, 14 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(50 * MS), 0);
	CHECK_EQ(R.r.nask, 3);
	CHECK(R.r.ask[0] == b->sb + 1 && R.r.ask[1] == b->sb + 2 && R.r.ask[2] == b->sb + 3);

	/* Nothing waits (redundancy, dup_wait 0): a gap goes at once, unasked. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.asked, 0);
	CHECK_EQ(R.r.st.lost_cap, 1);

	/* Story 023: redundancy, nothing waits, but the packet after the gap
	 * says it went on link 0 alone (its copies cut) and comes on link 0:
	 * lost on every link, asked for at once, and what follows waits for
	 * it, rescue_us; it comes (rescued), rebuilt (repaired), or not. */
	for (int how = 0; how < 3; how++) {
		struct cg_ro_pkt k;

		rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
		R.cf.rescue_us = 60 * MS;
		owd(0, 10 * MS);
		owd(1, 2000 * MS); /* a link seconds behind: its RACK never comes in time */
		CHECK_EQ(in(0, 0, 10 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
		CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, cg_prev_flags(0), DATA, BIG), CG_RO_HOLD);
		CHECK_EQ(R.r.nask, 1);
		CHECK_EQ(R.r.ask[0], b->sb + 1);
		R.r.nask = 0;
		CHECK_EQ(in(3, 3 * MS, 13 * MS, 0, 0, DATA, BIG), CG_RO_HOLD);
		CHECK(due(13 * MS) > (int32_t)(50 * MS) && due(13 * MS) <= (int32_t)(59 * MS));
		CHECK_EQ(drain(40 * MS), 0);
		if (how < 2) {
			k = pkt(1, 1 * MS, 45 * MS, 0, 0, DATA, BIG);
			k.rebuilt = (uint8_t)how;
			CHECK_EQ(in_pkt(k, 45 * MS), CG_RO_NOW);
			CHECK_EQ(R.nout, 4);
			CHECK(R.out[1] == 1 && R.out[2] == 2 && R.out[3] == 3);
			CHECK_EQ(how ? R.r.st.ask_repaired : R.r.st.rescued, 1);
			CHECK_EQ(how ? R.r.st.rescued : R.r.st.ask_repaired, 0);
		} else {
			CHECK_EQ(drain(71 * MS), 0);
			CHECK_EQ(drain(73 * MS), 2); /* given up at 12 + 60 ms */
			CHECK_EQ(R.r.st.ask_lost, 1);
		}
		CHECK_EQ(R.r.head, b->sb + 4);
		CHECK(marks_ok(&R.r));
	}
	/* the hint on a packet from another link: nothing is known yet */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 32 * MS, 1, cg_prev_flags(0), DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.asked, 0);

	/* Asked at the hold cap (the slowest link seconds behind, split): it
	 * still waits rescue_us beyond the cap, not nothing (before story 023
	 * it went at once). */
	rig(CG_RO_SLOTS, b, 100 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.rescue_us = 60 * MS;
	owd(0, 10 * MS);
	owd(1, 3000 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(111 * MS), 0);
	CHECK_EQ(R.r.nask, 0);
	CHECK_EQ(drain(113 * MS), 0); /* due at 12 + 100: asked */
	CHECK_EQ(R.r.nask, 1);
	R.r.nask = 0;
	CHECK(due(113 * MS) > (int32_t)(55 * MS));
	CHECK_EQ(drain(160 * MS), 0);
	CHECK_EQ(in(1, 1 * MS, 165 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.rescued, 1);
	CHECK_EQ(R.r.st.ask_lost, 0);
}

/* adaptive: a link's margin grows to how late its late packets come, and
 * halves after CG_RO_ADAPT_DECAY_US without one. Links 0 (10 ms) and 1
 * (30 ms); margin 10 ms, cap 400 ms. */
static void sc_adapt(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	R.cf.adaptive = 1;
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	/* seq 1, on link 1, comes 200 ms after its gap went (2 + 30 + 10 ms) */
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(43 * MS), 1);
	CHECK_EQ(R.r.st.lost_cap, 1);
	CHECK_EQ(in(1, 1 * MS, 243 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_cap, 1);
	CHECK_EQ(R.r.link[1].adapt_us, 250 * MS); /* 200 ms and a quarter */
	CHECK_EQ(R.r.link[0].adapt_us, 0);
	/* The next gap waits for link 1's delay and that: seq 4 at 260 ms is
	 * still held at 270 + 30 + 250 + 10 - 1 ms, and seq 3 comes. */
	CHECK_EQ(in(4, 260 * MS, 270 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(549 * MS), 0);
	CHECK_EQ(in(3, 259 * MS, 520 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.lost_cap, 1);
	CHECK_EQ(R.r.head, b->sb + 5);
	/* Never past the cap: a packet 1 s late teaches 400 ms. */
	CHECK_EQ(in(6, 600 * MS, 610 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(1100 * MS), 1);
	CHECK_EQ(in(5, 599 * MS, 2100 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.link[1].adapt_us, 400 * MS);
	/* 5 s without a late packet on link 1: half. */
	CHECK_EQ(in(7, 7100 * MS, 7130 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.link[1].adapt_us, 200 * MS);
	/* Off: nothing learned. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 0, 0x3);
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(drain(43 * MS), 1);
	CHECK_EQ(in(1, 1 * MS, 243 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.link[1].adapt_us, 0);
}

/* RACK from another class (story 030): a link that brought no later DATA
 * but a STREAM sent after the lowest held packet is past the gaps below it. */
static void sc_note(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	in(0, 0, 100, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 2 * MS, 200, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* link 1: nothing yet */
	cg_ro_note(&R.r, 1, R.b.tsb + 1 * MS); /* sent before 2: says nothing about 1 */
	CHECK_EQ(drain(300), 0);
	CHECK_EQ(R.r.held, 1);
	cg_ro_note(&R.r, 1, R.b.tsb + 2 * MS); /* sent with it: not after */
	CHECK_EQ(drain(300), 0);
	cg_ro_note(&R.r, 1, R.b.tsb + 3 * MS); /* after 2: 1 is not coming on link 1 */
	CHECK_EQ(drain(400), 1);
	CHECK(OUT_IS(0, 2));
	CHECK_EQ(R.r.st.lost_rack, 1);
	CHECK_EQ(R.r.held, 0);
	/* only the gaps below the lowest held packet: one above it waits */
	CHECK_EQ(in(4, 4 * MS, 500, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(6, 6 * MS, 600, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	cg_ro_note(&R.r, 1, R.b.tsb + 5 * MS);
	CHECK_EQ(drain(700), 1); /* 3 goes, 4 out; 5 waits for link 1 */
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(R.r.st.lost_rack, 2);
	cg_ro_note(&R.r, 1, R.b.tsb + 7 * MS);
	CHECK_EQ(drain(800), 1);
	CHECK_EQ(R.r.st.lost_rack, 3);
	CHECK_EQ(R.r.held, 0);
}

static void sc_rack(const struct base *b)
{
	uint64_t held_total;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x7);
	in(0, 0, 100, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 0, 200, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* links 1 and 2: nothing yet */
	CHECK_EQ(in(3, 0, 300, 1, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.r.held, 2);
	CHECK_EQ(due(300), 200 * MS - 100); /* from 2's arrival */
	/* The last link of rset past the gap: given up at once, no wait. */
	CHECK_EQ(in(4, 0, 400, 2, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK(OUT_IS(0, 2, 3, 4));
	CHECK_EQ(R.r.st.lost_rack, 1);
	CHECK_EQ(R.r.held, 0);

	/* A link behind the gap blocks; out of rset it no longer does. */
	CHECK_EQ(in(6, 0, 500, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(7, 0, 600, 1, SPLIT, DATA, BIG), CG_RO_HOLD); /* link 2 is at 4 */
	CHECK_EQ(R.r.held, 2);
	R.rset = 0x3;
	CHECK_EQ(drain(600), 2);
	CHECK_EQ(R.r.st.lost_rack, 2);

	/* A link of rset that never brought anything (hw_ok 0) blocks. */
	R.rset = 0xb;
	CHECK_EQ(in(9, 0, 700, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(10, 0, 800, 1, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.r.held, 2);
	CHECK(due(800) > 0);
	R.rset = 0x3;
	CHECK_EQ(due(800), 0);
	CHECK_EQ(drain(800), 2);
	CHECK_EQ(R.r.st.lost_rack, 3);
	CHECK(OUT_TAIL(4, 6, 7, 9, 10));

	/* One link in rset: its own packet is past the gap, so the packet goes
	 * at once, without a copy. */
	R.rset = 0x1;
	held_total = R.r.st.held_total;
	CHECK_EQ(in(12, 0, 900, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.lost_rack, 4);
	CHECK_EQ(R.r.st.held_total, held_total);

	/* Only the gaps below every link's newest go: 13 (link 1 at 14), not 15. */
	R.rset = 0x3;
	CHECK_EQ(in(16, 0, 1000, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(14, 0, 1100, 1, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.r.st.lost_rack, 5);
	CHECK_EQ(R.r.head, R.b.sb + 15);
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(R.how[R.nout - 1], 'r'); /* 14 */
	CHECK_EQ(R.out[R.nout - 1], 14);
	CHECK(due(1100) > 0);
}

/* 5: deadlines: a huge delay is capped, dup_wait for what is not split, the
 * window after a split packet, a zero cap that holds nothing. */
static void sc_deadline(const struct base *b)
{
	/* A link 10 s behind: capped at t_arr + max_wait. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 10000 * MS);
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(12 * MS), 200 * MS);
	CHECK_EQ(drain(212 * MS - 1), 0);
	CHECK_EQ(drain(212 * MS), 1);
	CHECK_EQ(R.r.st.lost_cap, 1);
	/* Late after the cap, 7 ms after it was given up. */
	CHECK_EQ(in(1, 1 * MS, 219 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_cap, 1);
	CHECK_EQ(R.r.st.late_by[3], 1);

	/* Not split: dup_wait. */
	rig(CG_RO_SLOTS, b, 200 * MS, 20 * MS, 10 * MS, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 10000 * MS);
	in(0, 0, 10 * MS, 0, 0, DATA, BIG);
	CHECK_EQ(in(2, 2 * MS, 12 * MS, 0, 0, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(12 * MS), 20 * MS);
	CHECK_EQ(drain(32 * MS - 1), 0);
	CHECK_EQ(drain(32 * MS), 1);

	/* The delays decide when they come first: 2 + 15 + 2 = 19 ms. */
	rig(CG_RO_SLOTS, b, 200 * MS, 20 * MS, 2 * MS, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 15 * MS);
	in(0, 0, 10 * MS, 0, 0, DATA, BIG);
	in(2, 2 * MS, 12 * MS, 0, 0, DATA, BIG);
	CHECK_EQ(due(12 * MS), 7 * MS);
	/* Without a delay for every link of rset, only the cap. */
	R.rset = 0x7;
	CHECK_EQ(due(12 * MS), 20 * MS);
	R.rset = 0;
	CHECK_EQ(due(12 * MS), 20 * MS);

	/* After a split packet, max_wait for max_wait (on either side), then
	 * dup_wait again. No rset: dl = t_arr + C. */
	rig(CG_RO_SLOTS, b, 200 * MS, 20 * MS, 0, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 0, 150 * MS, 0, 0, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(150 * MS), 200 * MS);
	in(1, 0, 150 * MS, 1, 0, DATA, BIG);
	CHECK_EQ(in(4, 0, 250 * MS, 0, 0, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(250 * MS), 20 * MS);
	CHECK_EQ(in(5, 0, 260 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* split after 4: 4 waits max_wait */
	CHECK_EQ(due(260 * MS), 190 * MS);
	in(3, 0, 270 * MS, 1, 0, DATA, BIG);
	CHECK_EQ(R.r.held, 0);
	/* The edge: exactly max_wait after the split packet is out of the window. */
	CHECK_EQ(in(7, 0, 460 * MS, 0, 0, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(460 * MS), 20 * MS);
	in(6, 0, 460 * MS, 0, 0, DATA, BIG);
	CHECK_EQ(in(9, 0, 460 * MS - 1, 0, 0, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(460 * MS - 1), 200 * MS);
	in(8, 0, 460 * MS, 0, 0, DATA, BIG);
	/* A split packet long ago is forgotten, before the clock comes round. */
	CHECK(R.r.split_seen);
	in(10, 0, 260 * MS + CG_RO_STALE, 0, 0, DATA, BIG);
	CHECK(R.r.split_seen);
	in(11, 0, 260 * MS + CG_RO_STALE + 1, 0, 0, DATA, BIG);
	CHECK(!R.r.split_seen);

	/* dup_wait 0, nothing held, not split: the gap goes at once and the
	 * packet with it, never copied; the gap's packets come late. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 30 * MS);
	in(0, 0, 10 * MS, 0, 0, DATA, BIG);
	CHECK_EQ(in(3, 3 * MS, 13 * MS, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.lost_cap, 2);
	CHECK_EQ(R.r.st.held_total, 0);
	CHECK_EQ(R.p.nfree, CG_RO_SLOTS);
	CHECK_EQ(in(1, 1 * MS, 31 * MS, 1, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 2 * MS, 32 * MS, 1, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_cap, 2);
	CHECK_EQ(R.r.st.late_by[4], 2); /* 18 and 19 ms: (10, 20] */
	CHECK(OUT_IS(0, 3, 1, 2));

	/* Margin 0 and the packet on the slowest link, when that link should
	 * bring it: nothing left to wait for, so no copy either. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 0, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 60 * MS);
	in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 2 * MS, 62 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.lost_cap, 1);
	R.cf.margin_us = 1;
	CHECK_EQ(in(4, 4 * MS, 64 * MS, 1, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(due(64 * MS), 1);
}

/* 6: late after RACK, cap and overflow; old once the slot was reused. */
static void sc_late(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	in(2, 0, 100, 0, SPLIT, DATA, BIG);
	in(3, 0, 1 * MS, 1, SPLIT, DATA, BIG); /* RACK: 1 given up at 1 ms */
	CHECK_EQ(R.r.st.lost_rack, 1);
	CHECK_EQ(in(1, 0, 3500, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_rack, 1);
	CHECK_EQ(R.r.st.late_by[2], 1); /* 2.5 ms */
	CHECK(OUT_IS(0, 2, 3, 1));

	/* Given up by the cap, then its slot taken by seq + N: old. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	in(2, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(drain(200 * MS), 1);
	for (uint32_t s = 3; s <= N; s++)
		in(s, 0, 200 * MS, 0, SPLIT, DATA, BIG);
	in(N + 2, 0, 200 * MS, 0, SPLIT, DATA, BIG); /* gap N + 1: slot 1 */
	CHECK_EQ(drain(400 * MS), 1);
	CHECK_EQ(R.r.st.lost_cap, 2);
	CHECK_EQ(in(1, 0, 401 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_old, 1);
	CHECK_EQ(R.r.st.late_cap, 0);
	CHECK_EQ(in(N + 1, 0, 401 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_cap, 1);
	CHECK_EQ(R.r.st.late_by[0], 1);
	/* Its slot is emptied: a second late copy (never, the anti-replay
	 * window drops it) would count as old, and no mark is left behind the
	 * head (sc_wrap). */
	CHECK_EQ(R.r.slot[(R.b.sb + N + 1) % N].state, CG_RO_EMPTY);
	CHECK(marks_ok(&R.r));
	CHECK_EQ(hsum(R.r.st.late_by), 1); /* old ones have no lateness */
}

/* 7: the window overflows: AGAIN, the forced gaps, the retry; then an IMIX
 * stream over links 140 ms apart at 35 kpps, with small packets bypassing:
 * the window overflows again and again, and nothing is lost. */
static void sc_overflow(const struct base *b)
{
	static struct cg_ro_pkt e[40000];
	static uint8_t seen[40000];
	unsigned n = 40000, once = 1, descents = 0, big_last_set = 0, nb = 0;
	uint32_t big_last = 0;
	struct cg_ro_stats *st = &R.r.st;

	/* 1 lost, 2..1000 held, then N + 500: AGAIN; the gaps before 501 go
	 * (just 1), 2..1000 with them, and the retry is held behind 1001. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	for (uint32_t s = 2; s <= 1000; s++)
		CHECK_EQ(in(s, 0, s, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(N, 0, 1001, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* d = N - 1 still fits */
	CHECK_EQ(R.agains, 0);
	CHECK_EQ(in(N + 500, 0, 1002, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* AGAIN, then fits */
	CHECK_EQ(R.agains, 1);
	CHECK_EQ(st->lost_overflow, 1);
	CHECK_EQ(R.nout, 1000);
	CHECK(R.out[0] == 0 && R.out[1] == 2 && R.out[999] == 1000);
	CHECK_EQ(R.r.held, 2);
	CHECK_EQ(R.r.head, R.b.sb + 1001);
	CHECK_EQ(st->max_burst, 999);
	CHECK(!R.r.force);
	CHECK_EQ(in(1, 0, 1500, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(st->late_overflow, 1);
	CHECK_EQ(st->late_by[0], 1);
	/* A small packet a window ahead overflows too (bypass is no way out):
	 * 1001 goes, and then it fits as a bypass. */
	CHECK_EQ(in(1001 + N, 0, 2000, 0, 0, DATA, SMALL), CG_RO_NOW);
	CHECK_EQ(R.agains, 2);
	CHECK_EQ(st->lost_overflow, 2);
	CHECK_EQ(st->bypassed, 1);
	CHECK_EQ(R.r.held, 2);
	CHECK_EQ(R.r.passed_ahead, 1);
	CHECK_EQ(R.r.head, R.b.sb + 1002);
	/* The forced gap's slot is the one of the packet that forced it, N
	 * ahead: when that packet is kept there, the gap's record is gone and
	 * a late copy of it counts as old (see the IMIX run below). */
	CHECK_EQ(in(N + 1002, 0, 2100, 0, SPLIT, DATA, BIG), CG_RO_HOLD); /* AGAIN: 1002 forced */
	CHECK_EQ(st->lost_overflow, 3);
	CHECK_EQ(in(1002, 0, 2200, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(st->late_old, 1);
	CHECK_EQ(R.p.nfree, CG_RO_SLOTS - R.r.held);
	CHECK_EQ(R.bad, 0);

	/* Everything held below the forced edge: the forced gaps still go up to
	 * it (an overflow, not a resync), and the packet is held there. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	for (uint32_t s = 2; s <= 10; s++)
		in(s, 0, s, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(N + 500, 0, 20, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.agains, 1);
	CHECK_EQ(st->lost_overflow, 1 + 490); /* 1, and 11..500 */
	CHECK_EQ(st->resyncs, 0);
	CHECK_EQ(R.r.head, R.b.sb + 501);
	CHECK_EQ(R.r.held, 1);

	/* The edges of FAR while holding: FAR - 1 overflows, FAR passes. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	in(2, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(1 + CG_RO_FAR, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(st->far, 1);
	CHECK_EQ(R.agains, 0);
	CHECK_EQ(R.r.head, R.b.sb + 1);
	CHECK_EQ(in(CG_RO_FAR, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.agains, 1);
	CHECK_EQ(st->far, 1);
	CHECK_EQ(R.r.head, R.b.sb + N + 1);

	/* A second force before a drain keeps the further edge. */
	rig(2, b, 200 * MS, 0, 10 * MS, 128, 0);
	raw_in(0, 0, 0, SPLIT, BIG);
	raw_in(2, 0, 0, SPLIT, BIG);
	raw_in(3, 0, 0, SPLIT, BIG);
	CHECK_EQ(raw_in(N + 100, 0, 0, SPLIT, BIG), CG_RO_AGAIN);
	CHECK_EQ(R.r.force_to, R.b.sb + 101);
	CHECK_EQ(raw_in(5, 0, 0, SPLIT, BIG), CG_RO_NOW); /* pool full: would force to 2 */
	CHECK_EQ(R.r.force_to, R.b.sb + 101);
	drain(0);
	CHECK_EQ(R.r.head, R.b.sb + 101);
	CHECK_EQ(st->lost_overflow, 100 - 3);

	/* IMIX: 7 x 64, 4 x 576, 1 x 1400; the small ones (bypass) duplicated,
	 * so the fast link brings them first, the others split 1:1. */
	rig(CG_RO_SLOTS, b, 400 * MS, 0, 10 * MS, 128, 0x3);
	owd(0, 10 * MS);
	owd(1, 150 * MS);
	memset(seen, 0, sizeof(seen));
	for (uint32_t i = 0; i < n; i++) {
		unsigned m = i % 12;
		uint16_t len = m < 7 ? SMALL : m < 11 ? 576 : BIG;
		unsigned link = len == SMALL ? 0 : nb++ % 2;
		uint32_t ts = i * 28 / (CG_RO_SLOTS / 4096); /* 140 ms of spread over more than the window */

		e[i] = pkt(i, ts, ts + (link ? 150 * MS : 10 * MS), link, len == SMALL ? 0 : SPLIT, DATA, len);
	}
	/* By arrival: the fast link's run, then the slow one's, interleaved
	 * by a merge (both FIFO). */
	{
		static struct ev order[40000];

		for (uint32_t i = 0; i < n; i++)
			order[i] = (struct ev){ i, e[i].t_arr - R.b.tb, e[i].link };
		qsort(order, n, sizeof(order[0]), ev_cmp);
		for (uint32_t i = 0; i < n; i++) {
			const struct cg_ro_pkt *k = &e[order[i].seq];

			in(k->seq - R.b.sb, k->ts - R.b.tsb, k->t_arr - R.b.tb, k->link, k->flags, k->nohold, k->len);
		}
	}
	flush_all();
	CHECK_EQ(R.nout, n);
	for (unsigned i = 0; i < R.nout && i < OUT_MAX; i++) {
		uint32_t s = R.out[i];

		if (s >= n || seen[s]++) {
			once = 0;
			continue;
		}
		if (s % 12 >= 7) { /* not small */
			descents += big_last_set && s < big_last;
			big_last = s;
			big_last_set = 1;
		}
	}
	CHECK(once);
	CHECK_EQ(taken(st), n);
	CHECK(R.agains > 0);
	CHECK(st->lost_overflow > 0);
	/* A steady overflow forces the gap N behind each new packet, whose slot
	 * that packet takes: the slow link's copies come late as old. */
	CHECK(st->late_old > 0);
	CHECK(st->late_overflow + st->late_old >= st->lost_overflow / 2);
	CHECK(st->bypassed > 0);
	CHECK_EQ(st->pool_full, 0);
	CHECK_EQ(st->far + st->resyncs, 0);
	CHECK(descents <= st->late_overflow + st->late_old + st->late_cap + st->late_rack);
	CHECK_EQ(R.p.nfree, CG_RO_SLOTS);
	CHECK_EQ(R.bad, 0);
}

/* 9: a server back 2^20 ahead while holding: far, the window stays; the old
 * packets go at their deadline (or by RACK), and the first one after that
 * resyncs. With nothing held it resyncs at once. */
static void sc_leap(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 0, 100, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(LEAP + 5, 0, 200, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(LEAP + 6, 0, 300, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.far, 2);
	CHECK_EQ(R.r.head, R.b.sb + 1);
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(drain(200 * MS + 99), 0);
	CHECK_EQ(drain(200 * MS + 100), 1);
	CHECK_EQ(in(LEAP + 7, 0, 201 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.resyncs, 1);
	CHECK_EQ(R.r.head, R.b.sb + LEAP + 8);
	CHECK_EQ(in(LEAP + 8, 0, 202 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.in_order, 2);
	CHECK(OUT_IS(0, LEAP + 5, LEAP + 6, 2, LEAP + 7, LEAP + 8));
	/* An old packet still on its way: late. */
	CHECK_EQ(in(3, 0, 203 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.late_old, 1);

	/* RACK: the new packets are past the old gap on every link. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	in(0, 0, 0, 1, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 0, 100, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(LEAP + 5, 0, 200, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.lost_rack, 1);
	CHECK(OUT_IS(0, LEAP + 5, 2));
	CHECK_EQ(in(LEAP + 6, 0, 300, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.resyncs, 1);

	/* Nothing held: at once, and the marks ahead go with the old window. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, 0, DATA, BIG);
	in(1, 0, 0, 0, 0, DATA, BIG);
	CHECK_EQ(in(3, 0, 0, 0, 0, DATA, SMALL), CG_RO_NOW);
	CHECK_EQ(R.r.passed_ahead, 1);
	CHECK_EQ(in(LEAP, 0, 0, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.resyncs, 1);
	CHECK_EQ(R.r.passed_ahead, 0);
	CHECK_EQ(R.r.slot[(R.b.sb + 3) % N].state, CG_RO_EMPTY);
	CHECK(marks_ok(&R.r));
	CHECK_EQ(R.r.head, R.b.sb + LEAP + 1);
	CHECK_EQ(in(LEAP + 1, 0, 0, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.in_order, 3);
	/* N ahead is a resync too; N - 1 is a gap. */
	CHECK_EQ(in(LEAP + 2 + N, 0, 0, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.resyncs, 2);
	CHECK_EQ(in(LEAP + 3 + 2 * N - 1, 0, 0, 0, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.resyncs, 2);
	CHECK_EQ(R.r.st.lost_cap, N - 1);
}

/* 10: flush: everything held, in order; then a clean window. */
static void sc_flush(const struct base *b)
{
	static const struct cg_ro_slot zero[CG_RO_SLOTS];
	uint64_t bits = 0;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	CHECK_EQ(flush_all(), 0); /* nothing yet */
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(flush_all(), 0);
	CHECK(!R.r.init); /* even with nothing held: what passes meanwhile is not seen */
	CHECK_EQ(in(1, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.in_order, 2);
	CHECK_EQ(in(3, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(4, 0, 0, 0, 0, DATA, SMALL), CG_RO_NOW);
	CHECK_EQ(in(6, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(7, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(10, 0, 0, 0, 0, DATA, SMALL), CG_RO_NOW);
	CHECK_EQ(R.r.held, 3);
	CHECK_EQ(R.r.passed_ahead, 2);
	CHECK_EQ(flush_all(), 3);
	CHECK(OUT_TAIL(4, 3, 6, 7));
	CHECK(R.how[4] == 'f' && R.how[6] == 'f');
	CHECK_EQ(R.r.st.lost_flush, 2); /* 2 and 5: before held packets */
	CHECK(!R.r.init);
	CHECK_EQ(R.r.held + R.r.passed_ahead + R.r.force + R.r.head, 0);
	for (unsigned i = 0; i < CG_RO_WORDS; i++)
		bits |= R.r.held_bits[i];
	CHECK_EQ(bits, 0);
	CHECK(!memcmp(R.r.slot, zero, sizeof(zero)));
	CHECK_EQ(R.r.st.held_total, 3); /* statistics stay */
	CHECK_EQ(R.p.nfree, CG_RO_SLOTS);
	CHECK_EQ(R.bad, 0);
	/* The next packet starts the window again, wherever it is. */
	CHECK_EQ(in(5, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.head, R.b.sb + 6);
	CHECK_EQ(in(7, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(6, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(10, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.r.held, 1);
	CHECK_EQ(R.r.passed_ahead, 0);
}

/* 11: bypass: handed on at once, stepped over later, never twice. */
static void sc_bypass(const struct base *b)
{
	uint8_t count[16] = { 0 };
	unsigned once = 1;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	CHECK_EQ(in(2, 0, 0, 0, 0, DATA, 100), CG_RO_NOW);
	CHECK_EQ(R.r.st.bypassed, 1);
	CHECK_EQ(R.r.passed_ahead, 1);
	CHECK_EQ(in(3, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(1, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_IS(0, 2, 1, 3));
	CHECK_EQ(R.r.passed_ahead, 0);
	CHECK_EQ(R.r.head, R.b.sb + 4);
	CHECK(marks_ok(&R.r)); /* the mark stepped over is gone with it */

	/* What never waits always, even with bypass_len 0; 32 bytes then held. */
	R.cf.bypass_len = 0;
	CHECK_EQ(in(5, 0, 0, 0, 0, NOHOLD, 148), CG_RO_NOW);
	CHECK_EQ(in(6, 0, 0, 0, 0, NOHOLD, 92), CG_RO_NOW);
	CHECK_EQ(in(7, 0, 0, 0, 0, NOHOLD, 64), CG_RO_NOW);
	CHECK_EQ(in(8, 0, 0, 0, SPLIT, DATA, 32), CG_RO_HOLD);
	CHECK_EQ(R.r.st.bypassed, 4);
	CHECK_EQ(in(4, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_TAIL(4, 5, 6, 7, 4, 8));
	for (unsigned i = 0; i < R.nout; i++)
		if (R.out[i] >= 16 || count[R.out[i]]++)
			once = 0;
	CHECK(once);
	CHECK_EQ(R.nout, 9);
	CHECK_EQ(R.r.passed_ahead, 0);
	CHECK(marks_ok(&R.r));

	/* Late and in order are what they are, bypass or not. */
	CHECK_EQ(in(9, 0, 0, 0, 0, NOHOLD, 148), CG_RO_NOW);
	CHECK_EQ(R.r.st.in_order, 4); /* 0, 1, 4 and 9 */
	CHECK_EQ(R.r.st.bypassed, 4);

	R.cf.bypass_len = 128;
	CHECK(cg_ro_bypass(&R.cf, DATA, 128, 0));
	CHECK(!cg_ro_bypass(&R.cf, DATA, 129, 0));
	CHECK(cg_ro_bypass(&R.cf, DATA, 1400, CG_F_FREE)); /* marked free (tcpfree.h) */
	CHECK(!cg_ro_bypass(&R.cf, DATA, 1400, CG_F_SPLIT));
	CHECK(cg_ro_bypass(&R.cf, NOHOLD, 2000, 0));
	CHECK(cg_ro_bypass(&R.cf, 0, 0, 0));
	CHECK(cg_ro_bypass(&R.cf, 5, 1400, 0));
}

/* 12: a pool of 4: the 5th passes at once (pool_full) and the gap before
 * what is held goes, so the buffers come back. */
static void sc_pool(const struct base *b)
{
	uint16_t taken_bufs[4];

	rig(4, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	for (uint32_t s = 2; s <= 5; s++)
		CHECK_EQ(in(s, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(R.p.nfree, 0);
	CHECK_EQ(in(6, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.pool_full, 1);
	CHECK_EQ(R.p.full, 1);
	CHECK_EQ(R.r.st.lost_overflow, 1);
	CHECK(OUT_IS(0, 6, 2, 3, 4, 5));
	CHECK_EQ(R.r.head, R.b.sb + 7); /* 6 stepped over */
	CHECK_EQ(R.r.passed_ahead, 0);
	CHECK(marks_ok(&R.r));
	CHECK_EQ(R.p.nfree, 4);
	CHECK_EQ(R.bad, 0);

	/* A shared pool emptied elsewhere, nothing held here: the packet passes,
	 * nothing is forced, and the head steps over it later. */
	for (unsigned i = 0; i < 4; i++)
		taken_bufs[i] = cg_ro_pool_get(&R.p);
	CHECK_EQ(in(9, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.st.pool_full, 2);
	CHECK(!R.r.force);
	CHECK_EQ(in(7, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(8, 0, 0, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_TAIL(6, 9, 7, 8));
	CHECK_EQ(R.r.head, R.b.sb + 10);
	CHECK_EQ(R.r.st.lost_overflow, 1);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(cg_ro_pool_put(&R.p, taken_bufs[i]), 0);
	/* A put that cannot be: the pool is whole, or the index is not its. */
	CHECK_EQ(cg_ro_pool_put(&R.p, 0), -1);
	CHECK_EQ(R.p.nfree, 4);
	CHECK_EQ(cg_ro_pool_get(&R.p), taken_bufs[3]); /* LIFO */
	CHECK_EQ(cg_ro_pool_put(&R.p, 4), -1);
	CHECK_EQ(cg_ro_pool_put(&R.p, taken_bufs[3]), 0);
	CHECK_EQ(cg_ro_pool_init(&R.p, pool_mem, pool_stack, CG_RO_POOL_MAX + 1), -1);
}

/* 14: the delay filter, wrap-safe, from control messages and data. */
static void sc_owd(const struct base *b)
{
	struct cg_reorder *r = &R.r;
	uint32_t m = 0;

	cg_ro_init(r);
	cg_ro_owd(r, 0, 0xfffffff0u);
	CHECK_EQ(r->link[0].owd, 0xfffffff0u);
	cg_ro_owd(r, 0, 0x10); /* +32 across the wrap */
	CHECK_EQ(r->link[0].owd, 0xfffffff4u);
	cg_ro_owd(r, 1, 100);
	cg_ro_owd(r, 1, 93); /* -7 / 8 is 0; a shift would give -1 */
	CHECK_EQ(r->link[1].owd, 100);
	cg_ro_owd(r, 1, 20);
	CHECK_EQ(r->link[1].owd, 90);
	/* The deviation, against the delay before each sample: 0, then 7 / 4,
	 * then 1 - 1 / 4 + 80 / 4. */
	CHECK_EQ(r->link[1].dev, 21);
	cg_ro_owd(r, 16, 5);
	cg_ro_owd(r, 255, 5);
	CHECK(!r->link[2].owd_ok);
	/* M: the largest, compared around the wrap. */
	cg_ro_owd(r, 2, 0x7ffffff0u);
	cg_ro_owd(r, 3, 0x80000010u);
	CHECK(cg_ro_owd_max(r, 0xc, &m) && m == 0x80000010u);
	cg_ro_owd(r, 4, 0xfffffff0u);
	cg_ro_owd(r, 5, 0x10u);
	CHECK(cg_ro_owd_max(r, 0x30, &m) && m == 0x10u);
	CHECK(cg_ro_owd_max(r, 0x20, &m) && m == 0x10u);
	CHECK(!cg_ro_owd_max(r, 0x40, &m));
	CHECK(!cg_ro_owd_max(r, 0x60, &m));
	CHECK(!cg_ro_owd_max(r, 0, &m));

	/* Data first copies feed the same filter: arrival minus the sender's ts. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 7 * MS, 2, 0, DATA, BIG);
	CHECK_EQ(R.r.link[2].owd, R.b.tb - R.b.tsb + 7 * MS);
	in(1, 1 * MS, 16 * MS, 2, 0, DATA, BIG);
	CHECK_EQ(R.r.link[2].owd, R.b.tb - R.b.tsb + 8 * MS);
	owd(2, 16 * MS);
	CHECK_EQ(R.r.link[2].owd, R.b.tb - R.b.tsb + 9 * MS);
	/* A link id past CG_MAX_LINKS (the server's is not authenticated) takes
	 * no per-link state, but the packet goes. */
	CHECK_EQ(in(2, 0, 0, 200, 0, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(R.r.head, R.b.sb + 3);
}

/* 15: cg_ro_due_us. */
static void sc_due(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x4); /* link 2 never: no RACK, no M */
	CHECK_EQ(due(0), INT32_MAX);
	raw_in(0, 0, 0, SPLIT, BIG);
	CHECK_EQ(due(0), INT32_MAX);
	CHECK_EQ(raw_in(2, 0, 1, SPLIT, BIG), CG_RO_HOLD);
	CHECK_EQ(due(50 * MS), 150 * MS);
	CHECK_EQ(due(200 * MS), 0);
	CHECK_EQ(due(300 * MS), -100 * (int32_t)MS);
	CHECK_EQ(raw_in(1, 0, 0, SPLIT, BIG), CG_RO_NOW); /* not drained: 2 is at the head */
	CHECK_EQ(due(0), 0);
	CHECK_EQ(drain(0), 1);
	CHECK_EQ(due(0), INT32_MAX);

	/* Forced (pool full), not drained yet. */
	rig(1, b, 200 * MS, 0, 10 * MS, 128, 0);
	raw_in(0, 0, 0, SPLIT, BIG);
	CHECK_EQ(raw_in(2, 0, 0, SPLIT, BIG), CG_RO_HOLD);
	CHECK(due(0) > 0);
	CHECK_EQ(raw_in(3, 0, 0, SPLIT, BIG), CG_RO_NOW);
	CHECK_EQ(due(0), 0);
	CHECK_EQ(drain(0), 1);

	/* RACK, not drained yet. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
	raw_in(0, 0, 0, SPLIT, BIG);
	CHECK_EQ(raw_in(2, 0, 0, SPLIT, BIG), CG_RO_HOLD);
	CHECK_EQ(raw_in(3, 0, 1, SPLIT, BIG), CG_RO_HOLD);
	CHECK_EQ(due(0), 0);
	CHECK_EQ(drain(0), 2);
	CHECK_EQ(due(0), INT32_MAX);

	/* Marks only: nothing held, nothing to wake up for. */
	CHECK_EQ(raw_in(6, 0, 0, 0, SMALL), CG_RO_NOW);
	CHECK_EQ(R.r.passed_ahead, 1);
	CHECK_EQ(due(0), INT32_MAX);
}

/* The stale sweep: a link that brought nothing for 2^30 sequences forgets
 * its newest before serial arithmetic would put it ahead. */
static void sc_sweep(const struct base *b)
{
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x7);
	in(0, 0, 0, 0, 0, DATA, BIG);
	R.r.link[2].hw = R.r.head - CG_RO_STALE - 10;
	R.r.link[2].hw_ok = 1;
	R.r.link[1].hw = R.r.head - CG_RO_STALE + 100000;
	R.r.link[1].hw_ok = 1;
	for (uint32_t s = 1; s <= 65536; s++)
		in(s, 0, 0, 0, 0, DATA, BIG);
	CHECK(!R.r.link[2].hw_ok);
	CHECK(R.r.link[1].hw_ok);
	CHECK(R.r.link[0].hw_ok);
	/* And the forced edge never outlives the head. */
	CHECK(!R.r.force);
}

/* What the in-order path does with the packets up to seq, without sending
 * them: the head moves to seq and each link of links brought one of the last
 * of them. That path writes no slot, so this is how a test comes round 2^32
 * sequences in a few steps. */
static void come_round(uint32_t seq, uint16_t links)
{
	R.r.head = R.b.sb + seq;
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		if (links >> i & 1) {
			R.r.link[i].hw = R.r.head - 1 - i;
			R.r.link[i].hw_ok = 1;
		}
	}
}

/* The head comes round to g, 2^32 sequences after a mark there was left
 * behind (or not): the last N + 1000 packets before g in order, through the
 * reorderer, then g is missing and g + 1 (split) is held for it, no matter
 * what the slot of g says; g fills the gap and both go in order, nothing
 * given up, nothing late. t: the sender's clock, past what was sent. */
static void wrap_tail(uint32_t g, uint32_t t)
{
	uint64_t lost0 = lost(&R.r.st), old0 = R.r.st.late_old;
	uint32_t from = g - N - 1000;
	unsigned k, ok = 1;

	come_round(from, 0x3);
	for (uint32_t s = from; s != g; s++, t += 10)
		ok &= sendp(s, t, s & 1, 0) == CG_RO_NOW;
	CHECK(ok);
	CHECK_EQ(R.r.head, R.b.sb + g);
	k = R.nout;
	CHECK_EQ(sendp(g + 1, t + 10, 0, SPLIT), CG_RO_HOLD);
	CHECK_EQ(R.r.held, 1); /* still held after the drain at its arrival */
	CHECK_EQ(R.r.passed_ahead, 0);
	CHECK(due(t + 10 + 10 * MS) > 0);
	CHECK_EQ(in(g, t, t + 12 * MS, 1, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_TAIL(k, g, g + 1));
	CHECK_EQ(lost(&R.r.st), lost0);
	CHECK_EQ(R.r.st.late_old, old0);
	CHECK(marks_ok(&R.r));
}

/* 17: no mark outlives its sequence. Each way the head leaves a PASSED slot
 * behind empties it: the walk steps over a bypassed packet, a gap given up
 * at a packet's arrival steps over one, a late packet takes its slot, a
 * resync leaves the window. A mark left there would step the head over g
 * without a wait when the session's sequence came round 2^32 later. */
static void sc_wrap(const struct base *b)
{
	for (unsigned way = 0; way < 4; way++) {
		uint32_t g = way == 2 ? 1 : 2;

		rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
		R.delay[0] = R.delay[1] = 10 * MS;
		sendp(0, 0, 0, 0);
		switch (way) {
		case 0: /* bypass, then the walk (cg_ro_next) steps over it */
			CHECK_EQ(in(2, 2, 2 + 10 * MS, 1, 0, DATA, SMALL), CG_RO_NOW);
			CHECK_EQ(sendp(1, 1, 0, 0), CG_RO_NOW);
			CHECK_EQ(R.r.head, R.b.sb + 3);
			break;
		case 1: /* bypass, then the gap before 3 goes at its arrival (cg_ro_skip) */
			CHECK_EQ(in(2, 1, 1 + 10 * MS, 0, 0, DATA, SMALL), CG_RO_NOW);
			CHECK_EQ(sendp(3, 2, 0, 0), CG_RO_NOW); /* dup_wait 0 */
			CHECK_EQ(R.r.head, R.b.sb + 4);
			CHECK_EQ(R.r.st.lost_cap, 1);
			break;
		case 2: /* given up, then late (cg_ro_late) */
			CHECK_EQ(sendp(2, 2, 0, 0), CG_RO_NOW);
			CHECK_EQ(sendp(1, 1, 1, 0), CG_RO_NOW);
			CHECK_EQ(R.r.st.late_cap, 1);
			break;
		default: /* bypass ahead of a gap, then a resync */
			CHECK_EQ(in(2, 1, 1 + 10 * MS, 1, 0, DATA, SMALL), CG_RO_NOW);
			CHECK_EQ(R.r.passed_ahead, 1);
			CHECK_EQ(sendp(N + 904, 3, 0, 0), CG_RO_NOW);
			CHECK_EQ(R.r.st.resyncs, 1);
			break;
		}
		CHECK_EQ(R.r.passed_ahead, 0);
		CHECK(R.r.slot[(R.b.sb + g) % N].state != CG_RO_PASSED);
		CHECK(marks_ok(&R.r));
		wrap_tail(g, 10 * MS);
	}
	/* A mark ahead still does its job once come round: a packet passed
	 * ahead of the gap is stepped over when the gap fills. Link 2 never
	 * brings anything: no RACK, the gaps wait. */
	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x7);
	R.delay[0] = R.delay[1] = 10 * MS;
	sendp(0, 0, 0, 0);
	come_round(5, 0x3);
	CHECK_EQ(in(7, 2, 2 + 10 * MS, 1, 0, DATA, SMALL), CG_RO_NOW);
	CHECK_EQ(sendp(8, 3, 0, SPLIT), CG_RO_HOLD);
	CHECK_EQ(sendp(6, 1, 1, SPLIT), CG_RO_HOLD);
	CHECK_EQ(sendp(5, 0, 0, SPLIT), CG_RO_NOW);
	CHECK(OUT_TAIL(1, 7, 5, 6, 8));
	CHECK_EQ(R.r.passed_ahead + R.r.held, 0);
	CHECK(marks_ok(&R.r));
}

/* 18: max_burst counts what one drain handed on. The engine's loop stops a
 * drain at a budget per pass (CG_RO_PASS_MAX) and ends it there with
 * cg_ro_burst_end; the rest goes in the next passes, each a drain of its
 * own. 2000 held behind a gap, in passes of 512: 512, not 2000. */
static void sc_burst(const struct base *b)
{
	struct cg_ro_out o;
	unsigned passes = 0, most = 0, ok = 1;

	rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0);
	in(0, 0, 0, 0, SPLIT, DATA, BIG);
	for (uint32_t s = 2; s <= 2001; s++)
		ok &= in(s, 0, s, 0, SPLIT, DATA, BIG) == CG_RO_HOLD;
	CHECK(ok);
	CHECK_EQ(R.r.st.max_burst, 0);
	for (;;) {
		unsigned n = 0;

		while (n < 512 && cg_ro_next(&R.r, &R.cf, R.rset, R.b.tb + 200 * MS + 2, &o)) {
			R.back[n++] = o.buf;
			emit(o.seq, 'r');
		}
		cg_ro_burst_end(&R.r);
		for (unsigned i = 0; i < n; i++)
			CHECK_EQ(cg_ro_pool_put(&R.p, R.back[i]), 0);
		if (!n)
			break;
		passes++;
		most = n > most ? n : most;
		CHECK_EQ(R.r.st.max_burst, most);
	}
	CHECK_EQ(passes, 4); /* 512, 512, 512, 464 */
	CHECK_EQ(most, 512);
	CHECK_EQ(R.r.st.max_burst, 512);
	CHECK_EQ(R.r.held, 0);
	CHECK_EQ(R.r.st.lost_cap, 1);
	CHECK_EQ(R.nout, 2001);
	/* A drain that ran to its end needs no call; another one changes nothing. */
	cg_ro_burst_end(&R.r);
	CHECK_EQ(R.r.st.max_burst, 512);
	CHECK_EQ(R.r.burst, 0);
}

/* 19: reorder off for longer than 2^31 sequences, then on again (a reload):
 * the flush forgets each link's newest sequence, which nothing sweeps while
 * the window is off and which would then read as ahead of the new window,
 * so that RACK gave up every gap at once. The delays stay. */
static void sc_offon(const struct base *b)
{
	static const uint32_t gone[] = { 1000, 1u << 30, 0x80000100u, 0x90000000u, 0xf0000000u };

	for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
		uint32_t s = 0, t = 0;
		uint64_t rack0;
		unsigned k, ok = 1;

		rig(CG_RO_SLOTS, b, 200 * MS, 0, 10 * MS, 128, 0x3);
		R.delay[0] = R.delay[1] = 10 * MS;
		for (; s < 2000; s++, t += 36)
			ok &= sendp(s, t, s & 1, SPLIT) == CG_RO_NOW;
		CHECK(ok);
		CHECK_EQ(flush_all(), 0);
		CHECK(!R.r.link[0].hw_ok && !R.r.link[1].hw_ok);
		CHECK(R.r.link[0].owd_ok && R.r.link[1].owd_ok);
		CHECK(!R.r.split_seen);
		/* gone[i] packets go by while reorder is off: the reorderer sees none */
		s += gone[i];
		t += 1000 * MS;
		for (unsigned j = 0; j < 100; j++, s++, t += 36)
			ok &= sendp(s, t, j & 1, SPLIT) == CG_RO_NOW;
		CHECK(ok);
		rack0 = R.r.st.lost_rack;
		k = R.nout;
		/* s + 1 on link 1 before s on link 0: held for it */
		CHECK_EQ(sendp(s + 1, t + 36, 1, SPLIT), CG_RO_HOLD);
		CHECK_EQ(R.r.held, 1);
		CHECK_EQ(sendp(s, t, 0, SPLIT), CG_RO_NOW);
		CHECK(OUT_TAIL(k, s, s + 1));
		CHECK_EQ(R.r.st.lost_rack, rack0);
		CHECK_EQ(R.r.st.late_rack + R.r.st.late_old, 0);
	}
}

/* 22: TCP retransmissions (tcp_aware, story 014). Marked free like an ACK,
 * a retransmission still takes its place in sequence: handed on ahead of
 * what is held, it made TCP send that again. */
static void sc_retx(const struct base *b)
{
	/* seq 1 missing, 2 and 3 held: the retransmission 4 waits with them,
	 * a free ACK goes at once. */
	rig(CG_RO_SLOTS, b, 500 * MS, 0, 10 * MS, 0, 0);
	CHECK_EQ(in(0, 0, 10 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK_EQ(in(2, 1 * MS, 11 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in(3, 2 * MS, 12 * MS, 0, SPLIT, DATA, BIG), CG_RO_HOLD);
	CHECK_EQ(in_free(4, 40 * MS, 50 * MS, 1), CG_RO_HOLD);
	CHECK_EQ(in_free(5, 41 * MS, 51 * MS, 0), CG_RO_NOW);
	CHECK(OUT_IS(0, 5));
	CHECK_EQ(R.r.st.retx, 1);
	CHECK_EQ(R.r.st.bypassed, 1);
	CHECK_EQ(in(1, 0, 60 * MS, 0, SPLIT, DATA, BIG), CG_RO_NOW);
	CHECK(OUT_IS(0, 5, 1, 2, 3, 4));
	CHECK_EQ(R.r.held, 0);
	CHECK_EQ(taken(&R.r.st), 6);
	CHECK(marks_ok(&R.r));
}

/* ---- 13: random streams against a slow model ---- */

/* The rules of reorder.h written again the slow way: 64-bit sequences and
 * times that never wrap (the real one gets their low 32 bits plus an
 * offset), the held packets in a sorted array, the marks ahead in a bitmap
 * over the sequence, and every rule worked out from scratch each time. */
struct m_pkt {
	uint64_t seq, ts, t;
	uint32_t id;
	uint16_t len;
	uint8_t link, flags, type;
};

#define M_SPAN (1u << 21) /* sequences a trial may cover: 2^20 of leap and a few thousand */

static struct {
	int init, force, split_seen;
	uint64_t head, force_to, last_split, base;
	struct m_pkt held[CG_RO_SLOTS];
	unsigned nheld, npassed, pool_free;
	uint8_t passed[M_SPAN / 8];
	/* the newest sequence written in each class modulo N, for late packets */
	uint64_t wseq[CG_RO_SLOTS], wt[CG_RO_SLOTS];
	uint8_t wst[CG_RO_SLOTS];
	uint64_t hw[CG_MAX_LINKS];
	int64_t owd[CG_MAX_LINKS];
	uint64_t dev[CG_MAX_LINKS];
	uint8_t hw_ok[CG_MAX_LINKS], owd_ok[CG_MAX_LINKS];
	uint16_t tick;
	uint32_t burst;
	struct cg_ro_stats st;
} M;

static unsigned m_bucket(int64_t us)
{
	static const int64_t edge_us[] = { 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000 };
	unsigned i = 0;

	while (i < 9 && us > edge_us[i])
		i++;
	return i;
}

static int m_passed(uint64_t s)
{
	uint64_t i = s - M.base;

	return i < M_SPAN && (M.passed[i / 8] >> (i % 8) & 1);
}

static void m_mark(uint64_t s, int on)
{
	uint64_t i = s - M.base;

	if (i >= M_SPAN)
		return;
	if (on)
		M.passed[i / 8] |= (uint8_t)(1u << (i % 8));
	else
		M.passed[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static void m_clear_marks(void)
{
	for (uint64_t s = M.head; s < M.head + N; s++)
		m_mark(s, 0);
	M.npassed = 0;
}

static void m_write(uint64_t seq, uint8_t st, uint64_t t)
{
	M.wseq[seq % N] = seq;
	M.wst[seq % N] = st;
	M.wt[seq % N] = t;
}

static void m_owd_sample(unsigned l, int64_t raw)
{
	if (!M.owd_ok[l]) {
		M.owd[l] = raw;
		M.dev[l] = 0;
		M.owd_ok[l] = 1;
	} else {
		int64_t d = raw - M.owd[l];

		M.dev[l] = M.dev[l] - M.dev[l] / 4 + (uint64_t)(d < 0 ? -d : d) / 4;
		M.owd[l] += d / 8;
	}
}

static void m_learn(const struct m_pkt *k)
{
	if (k->link < CG_MAX_LINKS) {
		if (!M.hw_ok[k->link] || k->seq > M.hw[k->link]) {
			M.hw[k->link] = k->seq;
			M.hw_ok[k->link] = 1;
		}
		m_owd_sample(k->link, (int64_t)(k->t - k->ts));
	}
	if (k->flags & SPLIT) {
		M.last_split = k->t;
		M.split_seen = 1;
		M.st.split_rx++;
	} else if (M.split_seen && (int64_t)(k->t - M.last_split) > (int64_t)CG_RO_STALE) {
		M.split_seen = 0;
	}
	if (!++M.tick && M.init)
		for (unsigned i = 0; i < CG_MAX_LINKS; i++)
			if (M.hw_ok[i] && (int64_t)(M.head - M.hw[i]) > (int64_t)CG_RO_STALE)
				M.hw_ok[i] = 0;
}

static int m_forced(uint64_t g)
{
	return M.force && g < M.force_to;
}

static void m_force_upto(uint64_t to)
{
	if (!M.force || to > M.force_to)
		M.force_to = to;
	M.force = 1;
}

static void m_moved(void)
{
	if (M.force && M.head >= M.force_to)
		M.force = 0;
}

static int m_rack(uint16_t rset, uint64_t g)
{
	if (!rset)
		return 0;
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		if ((rset >> i & 1) && (!M.hw_ok[i] || M.hw[i] <= g))
			return 0;
	return 1;
}

static uint32_t m_cap(const struct cg_ro_cfg *cf, const struct m_pkt *p)
{
	int64_t d = (int64_t)(p->t - M.last_split);

	if (p->flags & SPLIT)
		return cf->max_wait_us;
	if (M.split_seen && (d < 0 ? -d : d) < (int64_t)cf->max_wait_us)
		return cf->max_wait_us;
	return cf->dup_wait_us;
}

static int64_t m_deadline(const struct cg_ro_cfg *cf, uint16_t rset, const struct m_pkt *p)
{
	int64_t dl = (int64_t)p->t + m_cap(cf, p), top = INT64_MIN;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		if (!(rset >> i & 1))
			continue;
		int64_t v;

		if (!M.owd_ok[i])
			return dl;
		v = M.owd[i] + (int64_t)(M.dev[i] > UINT32_MAX / CG_RO_DEV_K / 2 ? UINT32_MAX / 2 : CG_RO_DEV_K * M.dev[i]);
		if (v > top)
			top = v;
	}
	if (rset && (int64_t)p->ts + top + cf->margin_us < dl)
		dl = (int64_t)p->ts + top + cf->margin_us;
	return dl;
}

/* Whether the deadline of the gap before p is the cap itself (no delay for
 * every link of rset, or the slowest one's beyond it). */
static int m_capped(const struct cg_ro_cfg *cf, uint16_t rset, const struct m_pkt *p)
{
	return m_deadline(cf, rset, p) == (int64_t)p->t + m_cap(cf, p);
}

static uint16_t m_wide(void);

/* Whether gap g, given up under rset at now (p the lowest held packet above
 * it), would still wait under the wide set. */
static int m_narrowed(const struct cg_ro_cfg *cf, uint16_t rset, uint64_t g, const struct m_pkt *p, uint64_t now)
{
	uint16_t wide = m_wide();

	if (!wide || !(wide & (uint16_t)~rset) || m_rack(wide, g))
		return 0;
	return m_cap(cf, p) && (int64_t)now < m_deadline(cf, wide, p);
}

static void m_lose(uint64_t g, uint8_t why, uint64_t t)
{
	m_write(g, why, t);
	if (why == CG_RO_LOST_RACK)
		M.st.lost_rack++;
	else if (why == CG_RO_LOST_CAP)
		M.st.lost_cap++;
	else
		M.st.lost_overflow++;
}

static int m_take_mark(uint64_t g)
{
	if (!m_passed(g))
		return 0;
	m_mark(g, 0);
	M.npassed--;
	m_write(g, CG_RO_EMPTY, 0);
	return 1;
}

static void m_pass(uint64_t s)
{
	m_mark(s, 1);
	M.npassed++;
	m_write(s, CG_RO_PASSED, 0);
}

static enum cg_ro_v m_in(const struct cg_ro_cfg *cf, const struct m_pkt *k, uint16_t rset)
{
	int64_t d = (int64_t)(k->seq - M.head);
	unsigned c = (unsigned)(k->seq % N), i;

	if (M.init && d == 0) {
		m_learn(k);
		M.head++;
		M.st.in_order++;
		m_moved();
		return CG_RO_NOW;
	}
	if (M.init && M.nheld && d >= N && d < 2 * N) {
		m_force_upto(k->seq - N + 1);
		return CG_RO_AGAIN;
	}
	m_learn(k);
	if (!M.init) {
		M.init = 1;
		M.head = k->seq + 1;
		M.st.in_order++;
		return CG_RO_NOW;
	}
	if (d < 0) {
		if (M.wseq[c] == k->seq && M.wst[c] >= CG_RO_LOST_RACK) {
			if (M.wst[c] == CG_RO_LOST_RACK)
				M.st.late_rack++;
			else if (M.wst[c] == CG_RO_LOST_CAP)
				M.st.late_cap++;
			else
				M.st.late_overflow++;
			M.st.late_by[m_bucket((int64_t)(k->t - M.wt[c]))]++;
			M.wst[c] = CG_RO_EMPTY;
		} else {
			M.st.late_old++;
		}
		return CG_RO_NOW;
	}
	if (d >= N) {
		if (M.nheld) {
			M.st.far++;
			return CG_RO_NOW;
		}
		M.st.resyncs++;
		m_clear_marks();
		M.head = k->seq + 1;
		M.force = 0;
		return CG_RO_NOW;
	}
	if (k->type != DATA || k->len <= cf->bypass_len) {
		m_pass(k->seq);
		M.st.bypassed++;
		return CG_RO_NOW;
	}
	if (!M.nheld) {
		uint64_t last = k->seq - 1;
		int go = m_forced(last) || m_rack(rset, last) || !m_cap(cf, k) ||
			 (int64_t)k->t >= m_deadline(cf, rset, k);

		if (go) {
			for (uint64_t g = M.head; g < k->seq; g++)
				if (!m_take_mark(g)) {
					if (!m_forced(g) && m_narrowed(cf, rset, g, k, k->t))
						M.st.due_narrow++;
					m_lose(g, m_forced(g) ? CG_RO_LOST_OVF : m_rack(rset, g) ? CG_RO_LOST_RACK : CG_RO_LOST_CAP,
					       k->t);
				}
			M.head = k->seq + 1;
			M.st.in_order++;
			m_moved();
			return CG_RO_NOW;
		}
	}
	if (!M.pool_free) {
		m_pass(k->seq);
		M.st.pool_full++;
		if (M.nheld)
			m_force_upto(M.held[0].seq);
		return CG_RO_NOW;
	}
	for (i = M.nheld; i > 0 && M.held[i - 1].seq > k->seq; i--)
		M.held[i] = M.held[i - 1];
	M.held[i] = *k;
	M.nheld++;
	M.pool_free--;
	m_write(k->seq, CG_RO_HELD, k->t);
	M.st.held_total++;
	if (M.nheld > M.st.held_max)
		M.st.held_max = M.nheld;
	return CG_RO_HOLD;
}

static void m_pop(struct m_pkt *out)
{
	*out = M.held[0];
	memmove(&M.held[0], &M.held[1], (M.nheld - 1) * sizeof(M.held[0]));
	M.nheld--;
	M.pool_free++;
	m_write(out->seq, CG_RO_EMPTY, 0);
}

static int m_next(const struct cg_ro_cfg *cf, uint16_t rset, uint64_t now, struct m_pkt *out)
{
	while (M.nheld || M.npassed || M.force) {
		uint64_t g = M.head;
		uint8_t why;

		if (M.nheld && M.held[0].seq == g) {
			m_pop(out);
			M.head++;
			m_moved();
			M.st.wait[m_bucket((int64_t)(now - out->t))]++;
			M.burst++;
			return 1;
		}
		if (m_take_mark(g)) {
			M.head++;
			m_moved();
			continue;
		}
		if (m_forced(g))
			why = CG_RO_LOST_OVF;
		else if (!M.nheld)
			break;
		else if (m_rack(rset, g))
			why = CG_RO_LOST_RACK;
		else if ((int64_t)now >= m_deadline(cf, rset, &M.held[0]))
			why = CG_RO_LOST_CAP;
		else
			break;
		if (why != CG_RO_LOST_OVF) {
			M.st.due_narrow += (uint64_t)m_narrowed(cf, rset, g, &M.held[0], now);
			M.st.due_capped += why == CG_RO_LOST_CAP && m_cap(cf, &M.held[0]) && m_capped(cf, rset, &M.held[0]);
		}
		m_lose(g, why, now);
		M.head++;
		m_moved();
	}
	if (M.burst > M.st.max_burst)
		M.st.max_burst = M.burst;
	M.burst = 0;
	return 0;
}

static int32_t m_due(const struct cg_ro_cfg *cf, uint16_t rset, uint64_t now)
{
	int64_t left;

	if (!M.nheld)
		return INT32_MAX;
	if (M.held[0].seq == M.head || m_passed(M.head) || m_forced(M.head) || m_rack(rset, M.head))
		return 0;
	left = m_deadline(cf, rset, &M.held[0]) - (int64_t)now;
	return left >= INT32_MAX ? INT32_MAX - 1 : (int32_t)left;
}

static int m_flush(struct m_pkt *out)
{
	if (!M.init)
		return 0;
	while (M.nheld) {
		uint64_t g = M.head++;

		if (M.held[0].seq == g) {
			m_pop(out);
			return 1;
		}
		if (!m_take_mark(g))
			M.st.lost_flush++;
	}
	m_clear_marks();
	M.init = M.force = M.split_seen = 0;
	M.head = M.force_to = 0;
	if (M.burst > M.st.max_burst)
		M.st.max_burst = M.burst;
	M.burst = 0;
	memset(M.hw_ok, 0, sizeof(M.hw_ok));
	memset(M.wseq, 0, sizeof(M.wseq));
	memset(M.wst, 0, sizeof(M.wst));
	memset(M.wt, 0, sizeof(M.wt));
	return 0;
}

/* A trial: packets sent over links of random delay, jitter and loss, in
 * redundancy or split, received in time order, first copies only, through
 * an emulated anti-replay window; timers from cg_ro_due_us. */
/* Scaled with the window, so that the trials that overflow it still do. */
#define T_SCALE (CG_RO_SLOTS / 4096)
#define T_PKTS (6000 * T_SCALE)
#define T_LINKS 4
#define T_EVS (T_PKTS * T_LINKS + 20000)

struct tpkt {
	uint64_t seq, ts;
	uint16_t len;
	uint8_t type, flags;
};

struct tev {
	uint64_t t, ts; /* arrival; a control message's send time */
	uint32_t pkt, n;
	uint8_t link, ctl;
};

static struct tpkt tp[T_PKTS];
static struct tev tq[T_EVS];
static uint8_t t_seen[T_PKTS], t_out[T_PKTS];

static struct {
	struct cg_ro_cfg cf;
	uint16_t rset, wide; /* wide: rset before a narrowing (reorder.h's due_narrow), 0: not counted */
	uint32_t soff, toff, tsoff;
	uint64_t now, last_ord;
	int have_ord;
	unsigned fails, steps, timers, early_checks;
} T;

static uint16_t m_wide(void)
{
	return T.wide;
}

#define RC(cond)                                                                                     \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			if (T.fails++ < 10)                                                          \
				fprintf(stderr, "%s:%d: step %u: %s\n", __FILE__, __LINE__, T.steps, #cond); \
		}                                                                                    \
	} while (0)

static int tev_cmp(const void *a, const void *b)
{
	const struct tev *x = a, *y = b;

	return x->t != y->t ? (x->t < y->t ? -1 : 1) : (x->n < y->n ? -1 : x->n > y->n);
}

static uint32_t s32(uint64_t s)
{
	return (uint32_t)(s + T.soff);
}

static uint32_t t32(uint64_t t)
{
	return (uint32_t)(t + T.toff);
}

/* Packet ids in the buffer: the first 4 bytes, the rest the usual pattern. */
static void t_fill(uint8_t *b, uint32_t id, uint32_t seq, unsigned len)
{
	fill(b, seq, len);
	memcpy(b, &id, 4);
}

static uint32_t t_id(const uint8_t *b)
{
	uint32_t id;

	memcpy(&id, b, 4);
	return id;
}

static int t_intact(const uint8_t *b, uint32_t seq, unsigned len)
{
	for (unsigned i = 4; i < len && i < 12; i++)
		if (b[i] != pat(seq, i))
			return 0;
	return len <= 12 || b[len - 1] == pat(seq, len - 1);
}

static void t_compare(void)
{
	RC(R.r.init == M.init);
	RC(!M.init || R.r.head == s32(M.head));
	RC(R.r.held == M.nheld);
	RC(R.r.passed_ahead == M.npassed);
	RC(R.r.force == M.force && (!M.force || R.r.force_to == s32(M.force_to)));
	RC(R.p.nfree == M.pool_free);
	RC(!memcmp(&R.r.st, &M.st, sizeof(M.st)));
}

/* An ordered output: after everything ordered before it. */
static void t_ordered(uint64_t seq)
{
	RC(!T.have_ord || seq > T.last_ord);
	T.last_ord = seq;
	T.have_ord = 1;
}

/* Both drain at now; the buffers come back after the whole drain. */
static unsigned t_drain(void)
{
	struct cg_ro_out o;
	struct m_pkt mo;
	unsigned n = 0;

	for (;;) {
		int a = cg_ro_next(&R.r, &T.cf, T.rset, t32(T.now), &o);
		int c = m_next(&T.cf, T.rset, T.now, &mo);

		RC(a == c);
		if (!a || !c)
			break;
		RC(o.seq == s32(mo.seq) && o.len == mo.len);
		RC(t_id(cg_ro_pool_buf(&R.p, o.buf)) == mo.id);
		RC(t_intact(cg_ro_pool_buf(&R.p, o.buf), o.seq, o.len));
		if (mo.id < T_PKTS)
			t_out[mo.id]++;
		t_ordered(mo.seq);
		R.back[n++] = o.buf;
	}
	for (unsigned i = 0; i < n; i++)
		RC(cg_ro_pool_put(&R.p, R.back[i]) == 0);
	return n;
}

static void t_flush(void)
{
	struct cg_ro_out o;
	struct m_pkt mo;
	unsigned n = 0;

	for (;;) {
		int a = cg_ro_flush(&R.r, &o), c = m_flush(&mo);

		RC(a == c);
		if (!a || !c)
			break;
		RC(o.seq == s32(mo.seq) && t_id(cg_ro_pool_buf(&R.p, o.buf)) == mo.id);
		if (mo.id < T_PKTS)
			t_out[mo.id]++;
		t_ordered(mo.seq);
		R.back[n++] = o.buf;
	}
	for (unsigned i = 0; i < n; i++)
		RC(cg_ro_pool_put(&R.p, R.back[i]) == 0);
	T.have_ord = 0;
	t_compare();
}

/* The timers due before until: each one checked a microsecond early (no
 * change) and on time (the head moves). A control message may have moved a
 * deadline into the past: due now, then. */
static void t_timers(uint64_t until)
{
	for (;;) {
		int32_t a = cg_ro_due_us(&R.r, &T.cf, T.rset, t32(T.now)), c = m_due(&T.cf, T.rset, T.now);
		uint64_t at;
		uint32_t head = R.r.head;

		RC(a == c);
		if (a == INT32_MAX)
			return;
		if (a <= 0) {
			t_drain();
			RC(R.r.head != head);
			t_compare();
			if (R.r.head == head)
				return;
			continue;
		}
		at = T.now + (uint64_t)a;
		if (at > until)
			return;
		if (a > 1 && rn(2)) {
			T.now = at - 1;
			RC(t_drain() == 0);
			RC(R.r.head == head);
			T.early_checks++;
		}
		T.now = at;
		t_drain();
		RC(R.r.head != head);
		T.timers++;
		t_compare();
	}
}

static void t_take(const struct tev *e)
{
	const struct tpkt *p = &tp[e->pkt];
	struct m_pkt mk = { .seq = p->seq, .ts = p->ts, .t = e->t, .id = e->pkt, .len = p->len, .link = e->link,
			    .flags = p->flags, .type = p->type };
	struct cg_ro_pkt k = { .seq = s32(p->seq), .ts = (uint32_t)(p->ts + T.tsoff), .t_arr = t32(e->t),
			       .len = p->len, .link = e->link, .flags = p->flags, .nohold = p->type };
	struct cg_ro_stats before = R.r.st;
	uint16_t buf;
	enum cg_ro_v v = cg_ro_in(&R.r, &R.p, &T.cf, &k, T.rset, &buf), mv = m_in(&T.cf, &mk, T.rset);

	RC(v == mv);
	if (v == CG_RO_AGAIN) {
		RC(!memcmp(&before, &R.r.st, sizeof(before))); /* nothing taken */
		R.agains++;
		t_drain();
		before = R.r.st;
		v = cg_ro_in(&R.r, &R.p, &T.cf, &k, T.rset, &buf);
		mv = m_in(&T.cf, &mk, T.rset);
		RC(v == mv && v != CG_RO_AGAIN);
	}
	/* Exactly one counter of the partition moved, by one. */
	RC(taken(&R.r.st) == taken(&before) + 1);
	if (v == CG_RO_HOLD) {
		RC(R.r.st.held_total == before.held_total + 1);
		t_fill(cg_ro_pool_buf(&R.p, buf), e->pkt, k.seq, p->len);
	} else {
		/* Ordered (in order, a resync's first) or a counted exception
		 * (bypass, late, far, pool full). */
		RC(buf == CG_RO_NOBUF && R.r.st.held_total == before.held_total);
		t_out[e->pkt]++;
		if (R.r.st.in_order != before.in_order)
			t_ordered(p->seq);
		else if (R.r.st.resyncs != before.resyncs)
			T.last_ord = p->seq, T.have_ord = 1;
	}
	/* No mark left behind: after each resync, and every 256 steps (a mark
	 * the head left behind stays until its slot is written again). */
	if (R.r.st.resyncs != before.resyncs || !(T.steps % 256))
		RC(marks_ok(&R.r));
	t_compare();
	t_drain();
	t_compare();
}

static void trial(int idx)
{
	static const uint32_t max_waits[] = { 0, 2 * MS, 30 * MS, 200 * MS };
	static const uint32_t margins[] = { 0, 1 * MS, 10 * MS };
	static const uint32_t bypasses[] = { 0, 128, 256 };
	static const unsigned pools[] = { 4, 64, 512, CG_RO_SLOTS };
	uint32_t base_d[T_LINKS], jit[T_LINKS], loss[T_LINKS];
	uint64_t last_arr[T_LINKS] = { 0 }, top = 0, t = (uint64_t)1 << 34;
	int fifo[T_LINKS], dead[T_LINKS], have_top = 0;
	unsigned nl = 1 + rn(T_LINKS), npk = 1000 + rn(T_PKTS - 1000), ne = 0, rr = 0;
	unsigned leap_at = rn(3) ? npk : rn(npk), out_a = npk, out_len = 0;
	int ovf = idx % 4 == 3; /* the window overflows: full pool, fast, split, links far apart */
	int split = ovf || rn(2), fast = ovf || !rn(4);

	if (ovf && nl < 2)
		nl = 2 + rn(T_LINKS - 1);
	T.cf.max_wait_us = ovf ? (200 * MS + rn(200 * MS)) * T_SCALE : max_waits[rn(4)];
	T.cf.dup_wait_us = rn(3) ? 0 : rn(2) ? T.cf.max_wait_us : T.cf.max_wait_us / 4;
	T.cf.margin_us = margins[rn(3)];
	T.cf.bypass_len = bypasses[rn(3)];
	if (!rn(4)) {
		static const unsigned lens[] = { 100, 3000 * T_SCALE, 5000 * T_SCALE, 9000 * T_SCALE };

		out_a = rn(npk);
		out_len = lens[rn(4)];
	}
	/* Half the trials wrap the sequence and the clocks partway through. */
	T.soff = idx % 2 ? (uint32_t)(0u - (uint32_t)((uint64_t)1 << 33) - rn(npk)) : rnd();
	T.toff = idx % 2 ? (uint32_t)(0u - (uint32_t)t - rn(2000000)) : rnd();
	T.tsoff = rnd();
	for (unsigned l = 0; l < nl; l++) {
		base_d[l] = ovf ? (l ? (100 * MS + rn(100 * MS)) * T_SCALE : rn(2 * MS)) : rn(5) ? rn(150 * MS) : rn(2 * MS);
		jit[l] = rn(4) ? rn(3 * MS) : 0;
		loss[l] = rn(3) ? rn(30) : 0; /* per mille */
		fifo[l] = rn(6) != 0;
		dead[l] = nl > 1 && !rn(10);
	}
	T.rset = (uint16_t)((1u << nl) - 1);
	if (!rn(8))
		T.rset |= 1u << 5; /* a link that never brings anything */
	if (!rn(8) && nl > 1)
		T.rset &= (uint16_t)~1u; /* one left out */
	/* Half the trials count what a narrowing let go sooner: wide holds
	 * rset and more (a link out of rset, one that never brings anything). */
	T.wide = rn(2) ? (uint16_t)(T.rset | (rn(2) ? 1u << 5 : 0) | (rn(2) ? 1u : 0) | (rnd() & ((1u << nl) - 1))) : 0;

	for (unsigned i = 0; i < npk; i++) {
		struct tpkt *p = &tp[i];
		unsigned m = rn(12), only = T_LINKS;

		t += fast ? 5 + rn(30) : 20 + rn(2000);
		p->seq = ((uint64_t)1 << 33) + i + (i >= leap_at ? LEAP : 0);
		p->ts = t;
		p->type = DATA;
		p->len = m < 7 ? SMALL : m < 11 ? 576 : BIG;
		if (!rn(50)) {
			static const uint16_t edges[] = { 128, 129, 256, 257 };

			p->len = edges[rn(4)];
		}
		if (!rn(200)) {
			(void)rn(3); /* the draw that picked a WireGuard handshake type before 0.12 */
			p->type = NOHOLD;
			p->len = 148;
		}
		p->flags = 0;
		if (split && p->type == DATA && p->len > 256 && nl > 1) {
			only = rr++ % nl;
			p->flags = SPLIT;
		}
		for (unsigned l = 0; l < nl; l++) {
			uint64_t at;

			if ((only < T_LINKS && l != only) || dead[l] || rn(1000) < loss[l] ||
			    (i >= out_a && i < out_a + out_len))
				continue;
			at = t + base_d[l] + (jit[l] ? rn(jit[l]) : 0);
			if (fifo[l]) {
				if (at < last_arr[l])
					at = last_arr[l];
				last_arr[l] = at;
			}
			tq[ne] = (struct tev){ .t = at, .pkt = i, .n = ne, .link = (uint8_t)l };
			ne++;
		}
	}
	/* Control messages: each live link every 1-5 ms. */
	for (unsigned l = 0; l < nl; l++) {
		if (dead[l])
			continue;
		for (uint64_t s = ((uint64_t)1 << 34) + rn(5 * MS); s < t && ne < T_EVS; s += MS + rn(4 * MS)) {
			tq[ne] = (struct tev){ .t = s + base_d[l] + (jit[l] ? rn(jit[l]) : 0), .ts = s, .n = ne,
					       .link = (uint8_t)l, .ctl = 1 };
			ne++;
		}
	}
	qsort(tq, ne, sizeof(tq[0]), tev_cmp);

	cg_ro_init(&R.r);
	R.r.wide = T.wide;
	cg_ro_pool_init(&R.p, pool_mem, pool_stack, ovf ? CG_RO_SLOTS : pools[rn(4)]);
	memset(&M, 0, sizeof(M));
	M.base = (uint64_t)1 << 33;
	M.pool_free = R.p.n;
	memset(t_seen, 0, sizeof(t_seen));
	memset(t_out, 0, sizeof(t_out));
	T.now = (uint64_t)1 << 34;
	T.have_ord = 0;
	T.steps = 0;
	R.agains = 0;

	for (unsigned i = 0; i < ne; i++) {
		const struct tev *e = &tq[i];
		const struct tpkt *p = &tp[e->pkt];

		t_timers(e->t);
		T.now = e->t;
		if (e->ctl) {
			cg_ro_owd(&R.r, e->link, t32(e->t) - (uint32_t)(e->ts + T.tsoff));
			m_owd_sample(e->link, (int64_t)(e->t - e->ts));
			continue;
		}
		/* The anti-replay window: first copies, and OLD dropped. */
		if (t_seen[e->pkt] || (have_top && top > p->seq && top - p->seq >= CG_REPLAY_WINDOW))
			continue;
		t_seen[e->pkt] = 1;
		if (!have_top || p->seq > top)
			top = p->seq;
		have_top = 1;
		t_take(e);
		T.steps++;
		if (!rn(4000))
			t_flush();
		if (!rn(2000)) {
			T.rset = (uint16_t)(rnd() & ((1u << nl) - 1));
			if (T.wide)
				R.r.wide = T.wide = (uint16_t)(T.wide | T.rset);
			t_drain();
		}
	}
	RC(marks_ok(&R.r));
	t_flush();
	for (unsigned i = 0; i < npk; i++)
		RC(t_out[i] == t_seen[i]);
	RC(R.p.nfree == R.p.n);
}

static void test_random(void)
{
	uint64_t agains = 0, late = 0, held = 0, lost_all = 0, far = 0, resyncs = 0, pool_full = 0, flushed = 0;
	uint64_t narrow = 0, capped = 0;

	memset(&T, 0, sizeof(T));
	for (int i = 0; i < 800; i++) {
		trial(i);
		agains += R.agains;
		held += R.r.st.held_total;
		late += R.r.st.late_rack + R.r.st.late_cap + R.r.st.late_overflow + R.r.st.late_old;
		lost_all += R.r.st.lost_rack + R.r.st.lost_cap + R.r.st.lost_overflow;
		far += R.r.st.far;
		resyncs += R.r.st.resyncs;
		pool_full += R.r.st.pool_full;
		flushed += R.r.st.lost_flush;
		narrow += R.r.st.due_narrow;
		capped += R.r.st.due_capped;
	}
	CHECK_EQ(T.fails, 0);
	/* The trials went through every rule. */
	CHECK(agains > 0 && late > 0 && held > 0 && lost_all > 0 && far > 0 && resyncs > 0 && pool_full > 0);
	CHECK(flushed > 0 && T.timers > 0 && T.early_checks > 0);
	CHECK(narrow > 0 && capped > 0);
}

void test_reorder(void)
{
	for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		const struct base *b = &bases[i];

		sc_in_order(b);
		sc_permute(b);
		sc_spread(b);
		sc_swing(b);
		sc_rack(b);
		sc_note(b);
		sc_by_link(b);
		sc_rescue(b);
		sc_adapt(b);
		sc_deadline(b);
		sc_late(b);
		sc_overflow(b);
		sc_leap(b);
		sc_flush(b);
		sc_bypass(b);
		sc_pool(b);
		sc_owd(b);
		sc_due(b);
		sc_sweep(b);
		sc_wrap(b);
		sc_burst(b);
		sc_offon(b);
		sc_retx(b);
	}
	test_random();
}
