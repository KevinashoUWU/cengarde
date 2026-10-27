/* The links the sender carries DATA on, told to the receiver's reorderer
 * (active.h, clockwork audit C2): the sender's mask, the receiver's rule,
 * the reorderer's guarantee kept (a link without a delay stays: only the
 * cap), and random streams over links the sender starts and stops using,
 * where no gap may go before its packet arrives.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "active.h"
#include "proto.h"
#include "test.h"

#define MS 1000u
#define HOLD_US (CG_ACT_HOLD_MS * 1000u)

static uint32_t rnd_state = 0x9e3779b9u;

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

static struct cg_reorder ro; /* about 322 KiB: static */

/* ---- sender ---- */

static void test_tx(void)
{
	struct cg_act_tx a;
	uint64_t t = 5000000;

	memset(&a, 0, sizeof(a));
	CHECK_EQ(cg_act_tx_mask(&a, t, HOLD_US), 0xffff); /* never started: every link */
	cg_act_tx_start(&a, t);
	CHECK_EQ(cg_act_tx_mask(&a, t + HOLD_US - 1, HOLD_US), 0xffff); /* the hold after a start */
	CHECK_EQ(cg_act_tx_mask(&a, t + HOLD_US, HOLD_US), 0);
	cg_act_tx_sent(&a, 2, t + 300 * MS);
	cg_act_tx_sent(&a, 16, t + 300 * MS); /* no such link: nothing */
	CHECK_EQ(cg_act_tx_mask(&a, t + 300 * MS, HOLD_US), 0x4);
	CHECK_EQ(cg_act_tx_mask(&a, t + 300 * MS + HOLD_US - 1, HOLD_US), 0x4);
	CHECK_EQ(cg_act_tx_mask(&a, t + 300 * MS + HOLD_US, HOLD_US), 0);
	/* a mark after the probe's time (read before it) is newer: in */
	CHECK_EQ(cg_act_tx_mask(&a, t + 299 * MS, HOLD_US), 0x4);
	/* hold 0: nothing is ever in it but what was marked after */
	CHECK_EQ(cg_act_tx_mask(&a, t + 300 * MS, 0), 0);
	/* hours later, a 64-bit clock: still out */
	CHECK_EQ(cg_act_tx_mask(&a, t + 3600ull * 1000000 * 5, HOLD_US), 0);
	/* a reload: every link again for the hold */
	cg_act_tx_start(&a, t + 400 * MS);
	CHECK_EQ(cg_act_tx_mask(&a, t + 401 * MS, HOLD_US), 0xffff);
}

static void test_counts(void)
{
	uint32_t owd[CG_MAX_LINKS] = { 0 };

	owd[0] = 0xfffffff0u; /* raw delays wrap: link 0 is 16 us before 0 */
	owd[1] = 5;
	owd[2] = 3;
	owd[3] = 3;
	/* one link: that one, known or not */
	CHECK_EQ(cg_act_tx_counts(0x2, 0x0, owd), 0x2);
	CHECK_EQ(cg_act_tx_counts(0, 0xf, owd), 0);
	/* copies: the one of least delay (wrap-safe), a tie to the lower id */
	CHECK_EQ(cg_act_tx_counts(0x3, 0xf, owd), 0x1);
	CHECK_EQ(cg_act_tx_counts(0x6, 0xf, owd), 0x4);
	CHECK_EQ(cg_act_tx_counts(0xe, 0xf, owd), 0x4);
	/* and every one whose delay is not known */
	CHECK_EQ(cg_act_tx_counts(0x16, 0xf, owd), 0x14);
	CHECK_EQ(cg_act_tx_counts(0x6, 0x0, owd), 0x6);
}

static void test_wire(void)
{
	uint8_t p[CG_PROBE_ACT_LEN];
	struct cg_probe_act a = { 0xa5c3, 200 }, b;

	memset(p, 0xee, sizeof(p));
	cg_probe_act_write(p, &a);
	CHECK_EQ(p[CG_PROBE_BOND_LEN], 0xa5);
	CHECK_EQ(p[CG_PROBE_BOND_LEN + 1], 0xc3);
	CHECK_EQ(p[CG_PROBE_BOND_LEN + 2], 0);
	CHECK_EQ(p[CG_PROBE_BOND_LEN + 3], 200);
	CHECK_EQ(p[CG_PROBE_BOND_LEN - 1], 0xee); /* the tail before it untouched */
	CHECK_EQ(cg_probe_act_read(&b, p, sizeof(p)), 1);
	CHECK_EQ(b.mask, 0xa5c3);
	CHECK_EQ(b.hold_ms, 200);
	CHECK_EQ(cg_probe_act_read(&b, p, CG_PROBE_ACT_LEN - 1), 0); /* a peer without it */
	CHECK_EQ(b.mask, 0);
	CHECK_EQ(b.hold_ms, 0);
	CHECK_EQ(cg_probe_act_read(&b, p, CG_PROBE_BOND_LEN), 0);
}

/* ---- receiver ---- */

/* link l's delay on the reorderer: d raw (local minus sender), no swing */
static void delay(unsigned l, uint32_t d)
{
	for (int i = 0; i < 8; i++)
		cg_ro_owd(&ro, l, d);
}

static void test_rx(void)
{
	struct cg_act_rx a;
	const uint32_t ts = 0xfffff000u, off = 1000000; /* the sender's clock wraps; local = sender + offset */
	const uint32_t margin = 10 * MS;
	uint64_t now = 77000000; /* the 64-bit local clock, for staleness */
	uint32_t until = 0;

	cg_ro_init(&ro);
	cg_act_rx_init(&a);
	delay(0, off + 20 * MS);
	delay(1, off + 80 * MS);
	/* No mask: every link of base counts, open. */
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x7, now, ts + off), 0x7);
	CHECK(cg_act_rx_open(&a, 1, now));
	CHECK_EQ(cg_act_rx_next_us(&a, &ro, margin, 0x7, now, ts + off), INT32_MAX);

	/* A mask at ts with link 0 only, hold 200 ms: link 1 carried nothing
	 * after ts - 200 ms, so it counts until ts - 200 + 80 + 10 (local),
	 * then not. Link 2 has no delay: it stays (only the cap then). */
	cg_act_rx_mask(&a, ts, 0x1, 200, now);
	CHECK(!cg_act_rx_open(&a, 1, now));
	CHECK(cg_act_rx_open(&a, 0, now));
	CHECK_EQ(a.quiet[1], ts - 200 * MS);
	CHECK_EQ(cg_act_rx_until(&a, &ro, margin, 1, &until), 1);
	CHECK_EQ(until, ts - 200 * MS + off + 80 * MS + margin);
	CHECK_EQ(cg_act_rx_until(&a, &ro, margin, 2, &until), 0);
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x7, now, ts + off - 110 * MS), 0x7);
	CHECK_EQ(cg_act_rx_next_us(&a, &ro, margin, 0x7, now, ts + off - 110 * MS - 1), 2);
	CHECK_EQ(cg_act_rx_next_us(&a, &ro, margin, 0x7, now, ts + off - 110 * MS), 1);
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x7, now, ts + off - 110 * MS + 1), 0x5);
	CHECK_EQ(cg_act_rx_next_us(&a, &ro, margin, 0x7, now, ts + off - 110 * MS + 1), INT32_MAX);
	/* base rules: a muted link is out whatever the mask says */
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x2, now, ts + off), 0);

	/* The next mask 100 ms later closes it again: the quiet spans touch,
	 * the horizon stays. */
	cg_act_rx_mask(&a, ts + 100 * MS, 0x1, 200, now + 100 * MS);
	CHECK_EQ(a.quiet[1], ts - 200 * MS);
	/* One 300 ms later (a lost probe, hold 200): they do not touch. */
	cg_act_rx_mask(&a, ts + 400 * MS, 0x1, 200, now + 400 * MS);
	CHECK_EQ(a.quiet[1], ts + 200 * MS);
	now += 400 * MS;

	/* Evidence: DATA on link 1 sent before the mask says nothing new (it
	 * stays closed, its horizon from the later of the two); one sent at or
	 * after it opens the link at once, in fresh. */
	CHECK_EQ(cg_act_rx_data(&a, 1, ts + 250 * MS, -1, now), 0);
	CHECK(!cg_act_rx_open(&a, 1, now));
	CHECK_EQ(cg_act_rx_until(&a, &ro, margin, 1, &until), 1);
	CHECK_EQ(until, ts + 250 * MS + off + 80 * MS + margin);
	CHECK_EQ(a.fresh, 0);
	CHECK_EQ(cg_act_rx_data(&a, 1, ts + 400 * MS, -1, now), 0x2);
	CHECK(cg_act_rx_open(&a, 1, now));
	CHECK_EQ(a.fresh, 0x2);
	CHECK_EQ(a.opened, 1);
	CHECK_EQ(cg_act_rx_data(&a, 1, ts + 401 * MS, -1, now), 0); /* open already */
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x7, now, ts + off + 1000 * MS), 0x7);

	/* CG_F_PREV: DATA on link 0 naming link 3 (closed, no delay yet) opens it. */
	a.fresh = 0;
	cg_act_rx_mask(&a, ts + 500 * MS, 0x1, 200, now + 100 * MS);
	now += 100 * MS;
	CHECK(!cg_act_rx_open(&a, 1, now)); /* the newer mask closed it again */
	CHECK(!cg_act_rx_open(&a, 3, now));
	CHECK_EQ(cg_act_rx_data(&a, 0, ts + 510 * MS, 3, now), 0x8);
	CHECK(cg_act_rx_open(&a, 3, now));
	/* evidence on link 16 or a prev of -1: nothing */
	CHECK_EQ(cg_act_rx_data(&a, 16, ts + 520 * MS, -1, now), 0);

	/* Stale: no mask for CG_ACT_STALE_US, every link open again; a clock a
	 * little behind the mask's still finds it. */
	CHECK(!cg_act_rx_open(&a, 1, now));
	CHECK(!cg_act_rx_open(&a, 1, now - 5 * MS));
	CHECK(cg_act_rx_open(&a, 1, now + CG_ACT_STALE_US));
	CHECK_EQ(cg_act_rx_rset(&a, &ro, margin, 0x7, now + CG_ACT_STALE_US, ts + off + 5000 * MS), 0x7);

	/* A mask from far away (a server's other clock): evidence that far from
	 * its ts goes, and does not keep a link open. */
	cg_act_rx_mask(&a, ts + 500 * MS + 2 * CG_ACT_FORGET_US, 0x0, 200, now + CG_ACT_STALE_US + 1);
	CHECK_EQ(a.ev_ok, 0);
	CHECK(!cg_act_rx_open(&a, 0, now + CG_ACT_STALE_US + 1));
	/* the next stale mask after a gap starts its quiet afresh */
	CHECK_EQ(a.quiet[0], ts + 500 * MS + 2 * CG_ACT_FORGET_US - 200 * MS);

	/* A reload forgets the links, keeps the counters. */
	cg_act_rx_forget(&a);
	CHECK(cg_act_rx_open(&a, 0, now));
	CHECK_EQ(a.opened, 2);
	CHECK_EQ(a.masks, 5);
}

/* The reorderer's guarantee (test_reorder's sc_rack): without a delay for
 * every link of rset, a gap waits for the cap only. A closed link without a
 * delay stays in rset, so the narrowing never takes it away; once measured
 * and past its horizon, it goes, and RACK and the deadline follow the
 * links left. */
static void test_keeps_rack(void)
{
	static uint8_t mem[64 * CG_RO_BUF];
	static uint16_t stack[64];
	struct cg_ro_pool p;
	struct cg_ro_cfg cf = { .max_wait_us = 200 * MS, .dup_wait_us = 200 * MS, .margin_us = 10 * MS };
	struct cg_act_rx a;
	struct cg_ro_pkt k = { .len = 1400, .flags = CG_F_SPLIT, .wgtype = CG_RO_WG_DATA };
	struct cg_ro_out o;
	uint16_t buf, rset;
	uint64_t now = 1000000000;
	const uint32_t t0 = 1000000;

	cg_ro_init(&ro);
	cg_ro_pool_init(&p, mem, stack, 64);
	cg_act_rx_init(&a);
	cg_act_rx_mask(&a, t0, 0x1, 200, now); /* link 1 idle, never measured */
	delay(0, 20 * MS);
	/* 10 on link 0, then 12: 11 missing */
	k.seq = 10, k.ts = t0, k.t_arr = t0 + 20 * MS, k.link = 0;
	ro.wide = 0x3;
	rset = cg_act_rx_rset(&a, &ro, cf.margin_us, 0x3, now, k.t_arr);
	CHECK_EQ(rset, 0x3); /* link 1 has no delay: it stays */
	CHECK_EQ(cg_ro_in(&ro, &p, &cf, &k, rset, &buf), CG_RO_NOW);
	k.seq = 12, k.ts = t0 + 2 * MS, k.t_arr = t0 + 22 * MS;
	CHECK_EQ(cg_ro_in(&ro, &p, &cf, &k, rset, &buf), CG_RO_HOLD);
	/* only the cap: no delay for link 1 */
	CHECK_EQ(cg_ro_due_us(&ro, &cf, rset, k.t_arr), (int32_t)(200 * MS));
	CHECK(cg_ro_capped(&ro, &cf, rset, k.ts, k.t_arr, cf.max_wait_us));
	/* measured now (its probes): 50 ms; it carried nothing after t0 - 200,
	 * so it counts until t0 - 200 + 50 + 10 < now: out, and RACK on link 0
	 * alone gives 11 up at once, sooner than the wide set would */
	delay(1, 50 * MS);
	rset = cg_act_rx_rset(&a, &ro, cf.margin_us, 0x3, now, k.t_arr);
	CHECK_EQ(rset, 0x1);
	CHECK_EQ(cg_ro_next(&ro, &cf, rset, k.t_arr, &o), 1);
	CHECK_EQ(o.seq, 12);
	CHECK_EQ(ro.st.lost_rack, 1);
	CHECK_EQ(ro.st.due_narrow, 1);
	CHECK_EQ(ro.st.due_capped, 0);
	cg_ro_pool_put(&p, o.buf);
}

/* ---- random streams ---- */

#define S_LINKS 4
#define S_PKTS 4000
#define S_PROBES 3000
#define S_EVS (S_PKTS * 2 + S_PROBES)
#define S_POOL 4096

struct spkt {
	uint64_t t;     /* sent */
	uint8_t links;  /* went on */
	int8_t prev;    /* CG_F_PREV: the link the one before went on alone, -1 */
};

struct sprobe {
	uint64_t t;
	uint32_t ctl;
	uint16_t mask, hold_ms;
	uint8_t link, lost;
};

struct sev {
	uint64_t t;
	uint32_t i, n;
	uint8_t link, probe;
};

static struct spkt sp[S_PKTS];
static struct sprobe spr[S_PROBES];
static struct sev sq[S_EVS];
static uint8_t s_seen[S_PKTS], s_out[S_PKTS];
static uint8_t s_mem[(size_t)S_POOL * CG_RO_BUF];
static uint16_t s_stack[S_POOL];

enum { RULE_ACTIVE, RULE_BASE, RULE_MASK_ONLY, RULE_NO_EVIDENCE };

static struct {
	struct cg_ro_pool p;
	struct cg_ro_cfg cf;
	struct cg_act_rx a;
	int rule;
	uint16_t base;
	uint32_t toff, tsoff, soff;
	uint64_t now;
} S;

static int sev_cmp(const void *x, const void *y)
{
	const struct sev *a = x, *b = y;

	if (a->t != b->t)
		return a->t < b->t ? -1 : 1;
	return a->n < b->n ? -1 : a->n > b->n;
}

static uint32_t l32(uint64_t t)
{
	return (uint32_t)(t + S.toff);
}

static uint16_t s_rset(void)
{
	uint16_t m = S.base;

	ro.wide = S.base;
	switch (S.rule) {
	case RULE_BASE:
		return m;
	case RULE_MASK_ONLY: /* the mask alone: no horizon, no evidence */
		return cg_act_rx_on(&S.a, S.now) ? (uint16_t)(m & S.a.mask) : m;
	default:
		return cg_act_rx_rset(&S.a, &ro, S.cf.margin_us, m, S.now, l32(S.now));
	}
}

static void s_drain(void)
{
	struct cg_ro_out o;
	uint16_t rset = s_rset();

	while (cg_ro_next(&ro, &S.cf, rset, l32(S.now), &o)) {
		s_out[o.seq - S.soff]++;
		cg_ro_pool_put(&S.p, o.buf);
	}
}

static void s_timers(uint64_t until)
{
	for (int guard = 0; guard < 100000; guard++) {
		uint16_t rset = s_rset();
		int32_t due = cg_ro_due_us(&ro, &S.cf, rset, l32(S.now)), act = INT32_MAX;

		if (S.rule == RULE_ACTIVE || S.rule == RULE_NO_EVIDENCE)
			act = cg_act_rx_next_us(&S.a, &ro, S.cf.margin_us, S.base, S.now, l32(S.now));
		if (act < due)
			due = act;
		if (due == INT32_MAX)
			return;
		if (due <= 0) {
			uint32_t head = ro.head, held = ro.held;

			s_drain();
			if (ro.head == head && ro.held == held)
				return; /* nothing more now */
			continue;
		}
		if (S.now + (uint64_t)due > until)
			return;
		S.now += (uint64_t)due;
		s_drain();
	}
}

/* One trial: rule decides rset. Returns the late packets (given up, then
 * came). jitter: links swing up to 3 ms (FIFO all the same). */
static uint64_t s_trial(int rule, uint32_t seed, int jitter, unsigned loss, uint64_t *narrow, uint64_t *gone)
{
	static const uint16_t holds[] = { 0, 50, 200, 1000 };
	uint64_t t = (uint64_t)1 << 33, end, last_arr[S_LINKS] = { 0 }, on_until[S_LINKS];
	uint32_t d[CG_MAX_LINKS] = { 0 }, jit[S_LINKS], ctl = 0;
	unsigned nl, npk, npr = 0, ne = 0, rr = 0, reload_at;
	uint16_t hold_ms;
	int on[S_LINKS], prev_only = -1;
	struct cg_act_tx tx;
	uint32_t newest_ctl = 0;
	int have_ctl = 0;

	rnd_state = seed;
	nl = 2 + rn(S_LINKS - 1);
	npk = 1000 + rn(S_PKTS - 1000);
	hold_ms = holds[rn(4)];
	reload_at = rn(3) ? npk : rn(npk);
	S.rule = rule;
	S.cf = (struct cg_ro_cfg){ .max_wait_us = 400 * MS, .dup_wait_us = 400 * MS, .margin_us = rn(2) ? MS : 0,
				   .by_link = (uint8_t)rn(2) };
	S.toff = rnd();
	S.tsoff = rnd();
	S.soff = rnd();
	S.base = (uint16_t)((1u << nl) - 1);
	for (unsigned l = 0; l < nl; l++) {
		d[l] = 1 * MS + rn(150 * MS);
		jit[l] = jitter ? rn(3 * MS) : 0;
		on[l] = !l || rn(2);
		on_until[l] = t + rn(800 * MS);
	}
	cg_act_tx_start(&tx, t);
	/* The sender: link 0 always on (copies go there), the others start
	 * and stop; each packet on the next link that is on, alone, or
	 * sometimes copied on link 0 too. */
	for (unsigned i = 0; i < npk; i++) {
		struct spkt *p = &sp[i];
		unsigned l;

		t += 200 + rn(2500);
		if (i == reload_at)
			cg_act_tx_start(&tx, t);
		for (unsigned k = 1; k < nl; k++)
			if (t >= on_until[k]) {
				on[k] = !on[k];
				on_until[k] = t + (on[k] ? 20 * MS + rn(600 * MS) : 50 * MS + rn(1500 * MS));
			}
		do
			l = rr++ % nl;
		while (!on[l]);
		p->t = t;
		p->links = (uint8_t)(1u << l);
		/* a copy on link 0, which the masks always have (hold 0: none,
		 * and a copy on links not counted is the known gap, active.h) */
		if (l && hold_ms && !rn(20))
			p->links |= 1;
		p->prev = (int8_t)prev_only;
		prev_only = (p->links & (p->links - 1)) ? -1 : (int)l;
		for (unsigned k = 0; k < nl; k++) {
			uint64_t at;

			if (!(p->links >> k & 1))
				continue;
			if (cg_act_tx_counts(p->links, S.base, d) >> k & 1)
				cg_act_tx_sent(&tx, k, t);
			if (loss && rn(1000) < loss)
				continue; /* sent, lost on the way */
			at = t + d[k] + (jit[k] ? rn(jit[k]) : 0);
			if (at < last_arr[k])
				at = last_arr[k];
			last_arr[k] = at;
			sq[ne] = (struct sev){ .t = at, .i = i, .n = ne, .link = (uint8_t)k };
			ne++;
		}
		/* probes, each link every 20-100 ms, a fifth lost */
		while (npr < S_PROBES && rn(40) == 0) {
			struct sprobe *q = &spr[npr];
			unsigned k = rn(nl);
			uint64_t at;

			q->t = t;
			q->ctl = ++ctl;
			q->link = (uint8_t)k;
			q->hold_ms = hold_ms;
			q->mask = cg_act_tx_mask(&tx, t, hold_ms * 1000u);
			q->lost = !rn(5);
			at = t + d[k] + (jit[k] ? rn(jit[k]) : 0);
			if (at < last_arr[k])
				at = last_arr[k];
			last_arr[k] = at;
			sq[ne] = (struct sev){ .t = at, .i = npr, .n = ne, .link = (uint8_t)k, .probe = 1 };
			ne++;
			npr++;
		}
	}
	end = t;
	qsort(sq, ne, sizeof(sq[0]), sev_cmp);

	cg_ro_init(&ro);
	cg_ro_pool_init(&S.p, s_mem, s_stack, S_POOL);
	cg_act_rx_init(&S.a);
	memset(s_seen, 0, sizeof(s_seen));
	memset(s_out, 0, sizeof(s_out));
	S.now = sq[0].t;
	for (unsigned e = 0; e < ne; e++) {
		const struct sev *v = &sq[e];

		s_timers(v->t);
		S.now = v->t;
		if (v->probe) {
			const struct sprobe *q = &spr[v->i];
			uint32_t ts = (uint32_t)(q->t + S.tsoff);

			if (q->lost)
				continue;
			cg_ro_owd(&ro, v->link, l32(v->t) - ts);
			if (!have_ctl || (int32_t)(q->ctl - newest_ctl) > 0) {
				newest_ctl = q->ctl;
				have_ctl = 1;
				cg_act_rx_mask(&S.a, ts, q->mask, q->hold_ms, S.now);
			}
			s_drain();
			continue;
		}
		if (s_seen[v->i])
			continue; /* a second copy: the replay window drops it */
		s_seen[v->i] = 1;
		{
			const struct spkt *p = &sp[v->i];
			struct cg_ro_pkt k = { .seq = S.soff + v->i,
					       .ts = (uint32_t)(p->t + S.tsoff),
					       .t_arr = l32(v->t),
					       .len = 1400,
					       .link = v->link,
					       .flags = (uint8_t)(CG_F_SPLIT | cg_prev_flags(p->prev)),
					       .wgtype = CG_RO_WG_DATA,
					       .prev = (uint8_t)(p->prev + 1) };
			uint16_t buf, rset;
			enum cg_ro_v r;

			if (S.rule != RULE_NO_EVIDENCE)
				cg_act_rx_data(&S.a, v->link, k.ts, p->prev, S.now);
			rset = s_rset();
			r = cg_ro_in(&ro, &S.p, &S.cf, &k, rset, &buf);
			if (r == CG_RO_AGAIN) {
				s_drain();
				r = cg_ro_in(&ro, &S.p, &S.cf, &k, rset, &buf);
			}
			if (r == CG_RO_NOW)
				s_out[v->i]++;
			s_drain();
		}
	}
	s_timers(end + 2000 * MS);
	{
		struct cg_ro_out o;

		while (cg_ro_flush(&ro, &o)) {
			s_out[o.seq - S.soff]++;
			cg_ro_pool_put(&S.p, o.buf);
		}
	}
	for (unsigned i = 0; i < npk; i++)
		CHECK_EQ(s_out[i], s_seen[i]); /* every packet out once */
	CHECK_EQ(S.p.nfree, S.p.n);
	*narrow += ro.st.due_narrow;
	*gone += ro.st.lost_rack + ro.st.lost_cap;
	/* given up, then came (late.old: before the window started, the
	 * first packet's) */
	return ro.st.late_rack + ro.st.late_cap + ro.st.late_overflow + ro.st.late_retx;
}

static void test_streams(void)
{
	uint64_t late[4] = { 0 }, narrow[4] = { 0 }, gone[4] = { 0 }, jl = 0, jn = 0, jg = 0;
	uint64_t ll[2] = { 0 }, ln[2] = { 0 }, lg[2] = { 0 };

	for (uint32_t i = 0; i < 120; i++) {
		uint32_t seed = 0x1234567u + i * 7919u;

		for (int rule = 0; rule < 4; rule++) {
			uint64_t l = s_trial(rule, seed, 0, 0, &narrow[rule], &gone[rule]);

			late[rule] += l;
			/* the guarantee, trial by trial: nothing given up early */
			if (rule == RULE_ACTIVE || rule == RULE_BASE) {
				CHECK_EQ(l, 0);
			}
		}
		jl += s_trial(RULE_ACTIVE, seed, 1, 0, &jn, &jg); /* jitter: exactly once, whatever comes late */
		/* loss: gaps given up for real, sooner with the narrowing */
		ll[0] += s_trial(RULE_ACTIVE, seed, 0, 5, &ln[0], &lg[0]);
		ll[1] += s_trial(RULE_BASE, seed, 0, 5, &ln[1], &lg[1]);
	}
	/* Without loss nothing is given up; the rules that keep less give
	 * gaps up early (the test can tell): the mask alone, or without the
	 * evidence of what came (a link the sender starts using). */
	CHECK_EQ(narrow[RULE_ACTIVE] + gone[RULE_ACTIVE] + gone[RULE_BASE], 0);
	CHECK(late[RULE_MASK_ONLY] > 0);
	CHECK(late[RULE_NO_EVIDENCE] > 0);
	/* With loss (0.5 %), the same gaps go, part of them sooner (about a
	 * fifth here), and in these streams none early. */
	CHECK_EQ(lg[0], lg[1]);
	CHECK(ln[0] > 0 && ln[1] == 0);
	CHECK_EQ(ll[0], 0);
	CHECK_EQ(ll[1], 0);
	(void)jl;
	(void)jn;
	(void)jg;
}

void test_active(void)
{
	test_tx();
	test_counts();
	test_wire();
	test_rx();
	test_keeps_rack();
	test_streams();
}
