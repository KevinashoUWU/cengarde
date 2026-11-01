/* dectrace: the engine's decisions, deterministic. A small simulator with a
 * virtual clock and a seeded random drives the pure headers the engine
 * decides with (health.h, capacity.h, linkcc.h, eta.h, dominant.h, fec3.h,
 * reorder.h) over links modelled as netem shapes them (bench/lab.d/bond.sh:
 * one-way delay, rate, a queue in ms, random loss), and prints what they
 * decided every tick, with a hash per piece. The same source, seed and
 * scenario give the same trace bit for bit: two engine revisions whose
 * traces match decided the same (bench/dectrace.sh compares two). It is not
 * a model of the field: the sender offers a fixed load (no TCP), and
 * client.c's and server.c's glue is written again here, the parts that
 * feed these headers only.
 *
 *   dectrace SCENARIO [SEED] [-v]
 *
 * SCENARIO: equal, het, fade, lossy, death, dominant, idle. -v prints every
 * tick's line; without it, the hashes and totals only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capacity.h"
#include "dominant.h"
#include "eta.h"
#include "fec3.h"
#include "health.h"
#include "linkcc.h"
#include "linkmodel.h"
#include "proto.h"
#include "reorder.h"

#define NL 4
#define PKT 1400u
#define SECS 30u
#define TICK_US 100000u   /* the engine's CG_TICK_MS */
#define SFACK_US 10000u   /* a link's SFACK every 10 ms (subflow.h) */
#define OFF 1000000u      /* the receiver's clock, ahead of the sender's */
#define GOV_TARGET_US 60000u
#define EV_MAX (1u << 20)

struct link {
	uint64_t rate_bps;  /* now; 0: dead */
	uint32_t delay_us;  /* one way, both directions */
	uint32_t queue_us;  /* netem's limit, as time at the rate */
	uint32_t loss_ppm;
	uint8_t type;       /* enum cg_link_type */
	uint64_t busy_us;   /* when its queue empties */
	/* the sender's */
	uint64_t tx_pkts, tx_bytes, picks;
	uint32_t sseq;
	struct cg_hlink h;
	struct cg_cap_link cap;
	struct cg_lc lc;
	uint64_t lsent, llost, f3s, f3l;
	/* the receiver's */
	uint32_t rx, rx_top, next_sseq;
	uint64_t ack_bytes, ack_lost, ack_t0;
	uint32_t owd_last, newest_sent;
	int have_new;
	/* the last report the sender has */
	uint32_t rep_rx, rep_top;
	uint64_t rep_ms;
};

enum { EV_ARRIVE, EV_REPORT, EV_SFACK };

struct ev {
	uint64_t t, n; /* time, then insertion order: the heap is stable */
	uint8_t kind, link;
	uint32_t seq, sseq, sent_us;
	uint32_t a, b;
	uint64_t c, d;
};

static struct link L[NL];
static int nl;
static struct ev *heap;
static unsigned nheap;
static uint64_t evn, rng;
static struct cg_eta eta;
static struct cg_dom dom;
static struct cg_f3 f3;
static struct cg_link_model lm[CG_MAX_LINKS];
static struct cg_reorder ro;
static struct cg_ro_pool pool;
static struct cg_ro_cfg rocf = { .max_wait_us = 200000, .margin_us = 10000 };
static uint8_t *pool_mem;
static uint16_t *pool_stack;
static uint32_t seq;
static uint64_t delivered, dropped_q, dropped_r, ro_out;
static int verbose;
static uint16_t carry_now = 0xffff; /* the carriers of the last tick */

/* FNV-1a, one per piece */
enum { H_HEALTH, H_CAP, H_LC, H_ETA, H_DOM, H_F3, H_RO, H_N };
static const char *const hname[H_N] = { "health", "capacity", "linkcc", "eta", "dominant", "fec3", "reorder" };
static uint64_t hv[H_N];

static void hadd(int h, const char *line)
{
	if (!hv[h])
		hv[h] = 1469598103934665603ull;
	for (const char *s = line; *s; s++)
		hv[h] = (hv[h] ^ (uint8_t)*s) * 1099511628211ull;
	if (verbose)
		fputs(line, stdout);
}

static uint64_t rnd(void) /* xorshift64* */
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ull;
}

static int ev_less(const struct ev *a, const struct ev *b)
{
	return a->t < b->t || (a->t == b->t && a->n < b->n);
}

static void push(struct ev e)
{
	unsigned i;

	if (nheap >= EV_MAX) {
		fprintf(stderr, "dectrace: too many events\n");
		exit(1);
	}
	e.n = evn++;
	i = nheap++;
	while (i && ev_less(&e, &heap[(i - 1) / 2])) {
		heap[i] = heap[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	heap[i] = e;
}

static struct ev pop(void)
{
	struct ev top = heap[0], last = heap[--nheap];
	unsigned i = 0;

	for (;;) {
		unsigned c = 2 * i + 1;

		if (c >= nheap)
			break;
		if (c + 1 < nheap && ev_less(&heap[c + 1], &heap[c]))
			c++;
		if (!ev_less(&heap[c], &last))
			break;
		heap[i] = heap[c];
		i = c;
	}
	heap[i] = last;
	return top;
}

/* The scenario at t: each link's rate and loss, and the offered load. */
static uint64_t scenario(const char *s, uint64_t t_us)
{
	uint64_t sec = t_us / 1000000, sum = 0;

	if (!strcmp(s, "fade")) /* link 0 fades from 50 to 5 Mbit/s and back */
		L[0].rate_bps = (uint64_t)(sec < 10 ? 50 : sec < 20 ? 50 - (sec - 10) * 45 / 10 : 5 + (sec - 20) * 45 / 10) *
				1000000;
	if (!strcmp(s, "death")) /* link 1 dies at 10 s and comes back at 20 */
		L[1].rate_bps = sec >= 10 && sec < 20 ? 0 : 30000000;
	for (int i = 0; i < nl; i++)
		sum += L[i].rate_bps;
	if (!strcmp(s, "idle") && sec >= 10 && sec < 20)
		return sum / 4;
	return sum + sum / 5; /* 1.2 times what the links carry */
}

static void setup(const char *s)
{
	static const struct {
		const char *name;
		int n;
		uint32_t mbit[NL], ms[NL], q[NL], loss[NL];
		uint8_t type[NL];
	} sc[] = {
		{ "equal", 4, { 25, 25, 25, 25 }, { 20, 20, 20, 20 }, { 100, 100, 100, 100 }, { 0 }, { 1, 1, 1, 1 } },
		{ "het", 3, { 60, 35, 10 }, { 20, 30, 40 }, { 100, 100, 100 }, { 0 }, { 1, 1, 1 } },
		{ "fade", 3, { 50, 30, 20 }, { 20, 25, 30 }, { 100, 100, 100 }, { 0 }, { 1, 1, 1 } },
		{ "lossy", 3, { 40, 40, 40 }, { 20, 25, 30 }, { 100, 100, 100 }, { 20000, 0, 10000 }, { 1, 1, 1 } },
		{ "death", 3, { 40, 30, 20 }, { 20, 25, 30 }, { 100, 100, 100 }, { 0 }, { 1, 1, 1 } },
		{ "dominant", 3, { 400, 25, 18 }, { 25, 30, 35 }, { 30, 100, 100 }, { 3000, 0, 0 }, { 2, 1, 1 } },
		{ "idle", 3, { 40, 30, 20 }, { 20, 25, 30 }, { 100, 100, 100 }, { 0 }, { 1, 1, 1 } },
	};
	unsigned k;

	for (k = 0; k < sizeof(sc) / sizeof(sc[0]) && strcmp(sc[k].name, s); k++)
		;
	if (k == sizeof(sc) / sizeof(sc[0])) {
		fprintf(stderr, "dectrace: no scenario %s\n", s);
		exit(2);
	}
	nl = sc[k].n;
	for (int i = 0; i < nl; i++) {
		struct link *l = &L[i];

		l->rate_bps = (uint64_t)sc[k].mbit[i] * 1000000;
		l->delay_us = sc[k].ms[i] * 1000;
		l->queue_us = sc[k].q[i] * 1000;
		l->loss_ppm = sc[k].loss[i];
		l->type = sc[k].type[i];
		cg_health_reset(&l->h, 0);
		cg_cap_reset(&l->cap);
		cg_lc_init(&l->lc);
		lm[i] = cg_link_model_of(l->type, 0, 0);
	}
	cg_eta_reset(&eta);
	eta.tick_ms = TICK_US / 1000;
	cg_dom_reset(&dom);
	memset(&f3, 0, sizeof(f3));
	cg_ro_init(&ro);
	pool_mem = calloc(4096, CG_RO_BUF);
	pool_stack = calloc(4096, sizeof(*pool_stack));
	heap = calloc(EV_MAX, sizeof(*heap));
	if (!pool_mem || !pool_stack || !heap || cg_ro_pool_init(&pool, pool_mem, pool_stack, 4096)) {
		fprintf(stderr, "dectrace: no memory\n");
		exit(1);
	}
}

/* The fastest link's one-way delay: the reports and SFACKs come back on it. */
static uint32_t back_us(void)
{
	uint32_t d = UINT32_MAX;

	for (int i = 0; i < nl; i++)
		if (L[i].rate_bps && L[i].delay_us < d)
			d = L[i].delay_us;
	return d == UINT32_MAX ? 50000 : d;
}

static uint16_t all_links(void)
{
	return (uint16_t)((1u << nl) - 1);
}

/* One packet onto link i at now: its queue, its loss, its arrival. */
static void send_on(int i, uint64_t now)
{
	struct link *l = &L[i];
	uint64_t start = l->busy_us > now ? l->busy_us : now, tx, dep;

	l->tx_pkts++;
	l->tx_bytes += PKT;
	l->picks++;
	cg_lc_sent(&l->lc, PKT);
	l->lsent++;
	seq++;
	l->sseq++;
	if (!l->rate_bps) {
		dropped_q++;
		return;
	}
	tx = (uint64_t)PKT * 8 * 1000000 / l->rate_bps;
	dep = start + tx;
	if (dep - now > l->queue_us) { /* netem's limit */
		dropped_q++;
		return;
	}
	l->busy_us = dep;
	if (l->loss_ppm && rnd() % 1000000 < l->loss_ppm) {
		dropped_r++;
		return;
	}
	push((struct ev){ .t = dep + l->delay_us, .kind = EV_ARRIVE, .link = (uint8_t)i, .seq = seq, .sseq = l->sseq,
			  .sent_us = (uint32_t)now });
}

/* The receiver: a packet arrives. */
static void arrive(const struct ev *e)
{
	struct link *l = &L[e->link];
	struct cg_ro_pkt k = { .seq = e->seq, .ts = e->sent_us, .t_arr = (uint32_t)(e->t + OFF), .len = PKT,
			       .link = e->link, .flags = CG_F_SPLIT };
	struct cg_ro_out o;
	uint16_t buf;
	enum cg_ro_v v;

	l->rx++;
	if ((int32_t)(e->seq - l->rx_top) > 0)
		l->rx_top = e->seq;
	if (l->next_sseq && (int32_t)(e->sseq - l->next_sseq) > 0)
		l->ack_lost += e->sseq - l->next_sseq; /* the gap: SFLOSS */
	l->next_sseq = e->sseq + 1;
	l->ack_bytes += PKT;
	l->owd_last = (uint32_t)(e->t + OFF - e->sent_us);
	l->newest_sent = e->sent_us;
	l->have_new = 1;
	delivered++;
	cg_ro_owd(&ro, e->link, l->owd_last);
	v = cg_ro_in(&ro, &pool, &rocf, &k, all_links(), &buf);
	if (v == CG_RO_AGAIN) {
		while (cg_ro_next(&ro, &rocf, all_links(), (uint32_t)(e->t + OFF), &o)) {
			cg_ro_pool_put(&pool, o.buf);
			ro_out++;
		}
		v = cg_ro_in(&ro, &pool, &rocf, &k, all_links(), &buf);
	}
	if (v == CG_RO_NOW)
		ro_out++;
	while (cg_ro_next(&ro, &rocf, all_links(), (uint32_t)(e->t + OFF), &o)) {
		cg_ro_pool_put(&pool, o.buf);
		ro_out++;
	}
}

/* The sender: an SFACK for link i, its rate learns of it (as on_sfack). */
static void sfack(const struct ev *e)
{
	struct link *l = &L[e->link];
	uint32_t rtt = e->sent_us ? (uint32_t)(e->t - e->sent_us) : 0;

	for (uint32_t x = 0; x < e->b; x++) {
		cg_lc_lost(&l->lc, PKT);
		l->llost++;
	}
	if (e->a)
		cg_lc_owd(&l->lc, e->t, (int32_t)e->a);
	cg_lc_acked(&l->lc, e->t, e->c, e->d, rtt);
	/* the split's expected arrival from it (lc_eta) */
	if (l->h.have_owd && l->lc.rtprop && eta.l[e->link].bps) {
		uint32_t ref = UINT32_MAX;

		for (int i = 0; i < nl; i++)
			if (L[i].lc.rtprop && L[i].lc.rtprop < ref)
				ref = L[i].lc.rtprop;
		cg_eta_own(&eta, e->link, cg_lc_rate(&l->lc) * 8, l->lc.rtprop > ref ? l->lc.rtprop - ref : 0,
			   cg_lc_queued(&l->lc), GOV_TARGET_US, e->t);
	}
}

/* Every 10 ms, each link's SFACK leaves the receiver. */
static void receiver_acks(uint64_t now)
{
	for (int i = 0; i < nl; i++) {
		struct link *l = &L[i];
		uint64_t dt = now - l->ack_t0;

		if (!l->have_new && !l->ack_lost)
			continue;
		push((struct ev){ .t = now + back_us(), .kind = EV_SFACK, .link = (uint8_t)i, .sent_us = l->newest_sent,
				  .a = l->have_new ? l->owd_last : 0, .b = (uint32_t)l->ack_lost, .c = l->ack_bytes,
				  .d = dt ? l->ack_bytes * 1000000 / dt : 0 });
		l->ack_bytes = l->ack_lost = 0;
		l->have_new = 0;
		l->ack_t0 = now;
	}
}

/* Every tick, each link's report leaves the receiver (the probe reply). */
static void receiver_reports(uint64_t now)
{
	for (int i = 0; i < nl; i++)
		push((struct ev){ .t = now + back_us(), .kind = EV_REPORT, .link = (uint8_t)i, .a = L[i].rx,
				  .b = L[i].rx_top, .c = L[i].owd_last });
}

static void report(const struct ev *e)
{
	struct link *l = &L[e->link];

	if (e->c)
		cg_health_report(&l->h, (uint32_t)e->c, e->t / 1000, 300);
	l->rep_rx = e->a;
	l->rep_top = e->b;
	l->rep_ms = e->t / 1000;
}

/* The sender's tick: health, capacity, rates, the split's arrivals, the
 * dominant link, FEC v3's loss; one line per piece. */
static void tick(uint64_t now)
{
	struct cg_hcfg hc = { .mute_behind_us = 0, .unmute_behind_us = 0, .settle_ms = 2000, .min_active = 1 };
	uint64_t ms = now / 1000;
	uint16_t live = all_links(), changed, carry;
	struct cg_cap_link dv[CG_MAX_LINKS];
	char b[512];
	int n;

	{
		struct cg_hlink hs[CG_MAX_LINKS];

		memset(hs, 0, sizeof(hs));
		for (int i = 0; i < nl; i++)
			hs[i] = L[i].h;
		changed = cg_health_eval(hs, nl, live, ms, &hc);
		for (int i = 0; i < nl; i++)
			L[i].h = hs[i];
		carry = carry_now = cg_health_carriers(hs, nl, live, live);
		n = snprintf(b, sizeof(b), "%llu health ch=%x carry=%x", (unsigned long long)ms, changed, carry);
		for (int i = 0; i < nl; i++)
			n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%u/%d", i, hs[i].state,
				      hs[i].have_behind ? hs[i].behind_us : -1);
		snprintf(b + n, sizeof(b) - (size_t)n, "\n");
		hadd(H_HEALTH, b);
	}
	n = snprintf(b, sizeof(b), "%llu cap", (unsigned long long)ms);
	for (int i = 0; i < nl; i++) {
		struct link *l = &L[i];
		struct cg_cap_cfg cc = { .window_ms = 10000, .sat_us = GOV_TARGET_US, .seq_lag = 1, .tx_seq = seq + 1,
					 .rx_top = l->rep_top };
		unsigned ev = l->rep_ms ? cg_cap_tick(&l->cap, ms, l->rep_ms, l->rep_rx, l->tx_pkts, l->tx_bytes, &l->h, &cc)
					: 0;

		n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%llu/%u/%x/%d", i, (unsigned long long)cg_cap_bps(&l->cap),
			      l->cap.cut, ev, l->cap.ql_us);
	}
	snprintf(b + n, sizeof(b) - (size_t)n, "\n");
	hadd(H_CAP, b);
	n = snprintf(b, sizeof(b), "%llu lc", (unsigned long long)ms);
	for (int i = 0; i < nl; i++) {
		struct cg_lc *c = &L[i].lc;

		cg_lc_target(c, GOV_TARGET_US);
		n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%llu/%u/%u/%u/%llu", i, (unsigned long long)c->bw, c->mode,
			      c->q_us, c->rtprop, (unsigned long long)c->inflight);
	}
	snprintf(b + n, sizeof(b) - (size_t)n, "\n");
	hadd(H_LC, b);
	/* the split's expected arrivals: the capacity's, then each link's own rate */
	{
		uint32_t ref = UINT32_MAX;

		for (int i = 0; i < CG_MAX_LINKS; i++)
			dv[i] = i < nl ? L[i].cap : (struct cg_cap_link){ 0 };
		cg_cap_eta(&eta, dv, carry, GOV_TARGET_US / 2, lm, 1, 0, now);
		for (int i = 0; i < nl; i++)
			if (L[i].lc.rtprop && L[i].lc.rtprop < ref)
				ref = L[i].lc.rtprop;
		for (int i = 0; i < nl; i++)
			if (L[i].lc.rtprop && eta.l[i].bps)
				cg_eta_own(&eta, (unsigned)i, cg_lc_rate(&L[i].lc) * 8,
					   L[i].lc.rtprop > ref ? L[i].lc.rtprop - ref : 0, cg_lc_queued(&L[i].lc), GOV_TARGET_US,
					   now);
	}
	n = snprintf(b, sizeof(b), "%llu eta", (unsigned long long)ms);
	for (int i = 0; i < nl; i++)
		n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%llu/%u/%llu", i, (unsigned long long)eta.l[i].bps,
			      eta.l[i].base_us, (unsigned long long)L[i].picks);
	snprintf(b + n, sizeof(b) - (size_t)n, "\n");
	hadd(H_ETA, b);
	/* the dominant link by each link's own rate (link_cc) */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		dv[i] = i < nl ? L[i].cap : (struct cg_cap_link){ 0 };
		if (i < nl) {
			dv[i].have_rate = (uint8_t)(L[i].lc.bw != 0);
			dv[i].rate_bps = dv[i].peak_bps = L[i].lc.bw * 8;
		}
	}
	cg_dom_tick(&dom, dv, lm, carry, ms);
	snprintf(b, sizeof(b), "%llu dom out=%x link=%d why=%u\n", (unsigned long long)ms, dom.out, dom.link, dom.why);
	hadd(H_DOM, b);
	/* FEC v3: each link's loss by its subflow, and the share */
	for (int i = 0; i < nl; i++) {
		struct link *l = &L[i];
		int pm = cg_lc_tick_loss(l->lsent, l->llost, &l->f3s, &l->f3l);
		int32_t q = (int32_t)cg_lc_f3_queue(&l->lc, (uint32_t)l->cap.ql_us);

		cg_f3_weigh(&f3, (unsigned)i, l->tx_bytes);
		if (pm >= 0)
			cg_f3_loss(&f3, (unsigned)i, (uint16_t)pm, ms, q, q, l->lc.have_owd && l->lc.q_us >= l->lc.target_us / 2);
	}
	cg_f3_mix_tick(&f3);
	snprintf(b, sizeof(b), "%llu f3 pct=%u\n", (unsigned long long)ms, cg_f3_pct(&f3, carry, carry, 20, 0, ms));
	hadd(H_F3, b);
	snprintf(b, sizeof(b), "%llu ro out=%llu held=%u rack=%llu cap=%llu late=%llu\n", (unsigned long long)ms,
		 (unsigned long long)ro_out, ro.held, (unsigned long long)ro.st.lost_rack, (unsigned long long)ro.st.lost_cap,
		 (unsigned long long)(ro.st.late_rack + ro.st.late_cap + ro.st.late_overflow));
	hadd(H_RO, b);
}

int main(int argc, char **argv)
{
	const char *sc = argc > 1 ? argv[1] : "het";
	uint64_t acc = 0;

	rng = argc > 2 && argv[2][0] != '-' ? strtoull(argv[2], NULL, 10) : 2026;
	rng = rng * 0x9e3779b97f4a7c15ull + 1;
	for (int i = 1; i < argc; i++)
		verbose |= !strcmp(argv[i], "-v");
	setup(sc);
	for (uint64_t now = 1000; now <= (uint64_t)SECS * 1000000; now += 1000) {
		uint64_t load = scenario(sc, now);
		uint16_t set;

		while (nheap && heap[0].t <= now) {
			struct ev e = pop();

			if (e.kind == EV_ARRIVE)
				arrive(&e);
			else if (e.kind == EV_SFACK)
				sfack(&e);
			else
				report(&e);
		}
		if (now % SFACK_US == 0)
			receiver_acks(now);
		if (now % TICK_US == 0) {
			receiver_reports(now);
			tick(now);
		}
		set = (uint16_t)(carry_now & all_links());
		if (dom.out)
			set &= (uint16_t)~dom.out;
		acc += load / 8 / 1000; /* bytes this ms */
		while (acc >= PKT) {
			int i = cg_eta_pick(&eta, set, PKT, now);

			acc -= PKT;
			if (i < 0) /* no rate yet: round robin */
				i = (int)(seq % (uint32_t)nl);
			send_on(i, now);
		}
	}
	printf("scenario %s seed %s: sent %u delivered %llu dropped %llu+%llu reordered-out %llu\n", sc,
	       argc > 2 && argv[2][0] != '-' ? argv[2] : "2026", seq, (unsigned long long)delivered,
	       (unsigned long long)dropped_q, (unsigned long long)dropped_r, (unsigned long long)ro_out);
	for (int h = 0; h < H_N; h++)
		printf("%-9s %016llx\n", hname[h], (unsigned long long)hv[h]);
	return 0;
}
