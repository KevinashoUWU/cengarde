/* The link model (linkmodel.h, story 015): the preset table, the names,
 * how the global knobs combine with a type, and the masks the loops take.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "capacity.h"
#include "linkmodel.h"
#include "test.h"

#define MS 1000u

/* Two models alike, field by field (memcmp would compare their padding). */
static int same(const struct cg_link_model *a, const struct cg_link_model *b)
{
	return a->lossy_noprobe == b->lossy_noprobe && a->lossy_hold == b->lossy_hold &&
	       a->hold_over_ms == b->hold_over_ms && a->loss_pct == b->loss_pct && a->g4_episode == b->g4_episode &&
	       a->rfull_hyst == b->rfull_hyst && a->qpred_ms == b->qpred_ms && a->qpred_mode == b->qpred_mode &&
	       a->dwell_ms == b->dwell_ms && a->dwell_oq_ms == b->dwell_oq_ms && a->eta_keep == b->eta_keep &&
	       a->eta_keep_ms == b->eta_keep_ms && a->pace_ms == b->pace_ms && a->pace_peers_ms == b->pace_peers_ms;
}

/* The table: one entry per type, in wire order, with a name each; auto is
 * all zeros (today's behaviour), and so is every hook not yet measured. */
static void test_linkmodel_table(void)
{
	static const char *names[CG_LTYPE_COUNT] = { "auto", "cellular", "starlink", "fiber", "ethernet", "radio" };
	const struct cg_link_model zero = { 0 };

	CHECK_EQ(CG_LTYPE_COUNT, 6);
	CHECK(CG_LTYPE_COUNT <= CG_LTYPE_MAX + 1); /* fits CG_BOND_TYPE's 3 bits */
	CHECK_EQ(CG_LTYPE_AUTO, 0);
	CHECK_EQ(CG_LTYPE_STARLINK, 2); /* wire values: never renumbered */
	CHECK_EQ(CG_LTYPE_RADIO, 5);
	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++) {
		CHECK(cg_link_presets[t].name != NULL);
		CHECK(!strcmp(cg_link_presets[t].name, names[t]));
		CHECK(!strcmp(cg_link_type_name(t), names[t]));
		CHECK_EQ(cg_link_type_parse(names[t]), (int)t);
	}
	CHECK(same(&cg_link_presets[CG_LTYPE_AUTO].m, &zero));
	/* the hooks keep auto's values until measured */
	CHECK(same(&cg_link_presets[CG_LTYPE_CELLULAR].m, &zero));
	CHECK(same(&cg_link_presets[CG_LTYPE_FIBER].m, &zero));
	CHECK(same(&cg_link_presets[CG_LTYPE_ETHERNET].m, &zero));
	CHECK(same(&cg_link_presets[CG_LTYPE_RADIO].m, &zero));
	/* unknown names and values */
	CHECK_EQ(cg_link_type_parse("satellite"), -1);
	CHECK_EQ(cg_link_type_parse("Starlink"), -1);
	CHECK_EQ(cg_link_type_parse(""), -1);
	CHECK_EQ(cg_link_type_norm(6), CG_LTYPE_AUTO); /* a newer peer's type: auto here */
	CHECK_EQ(cg_link_type_norm(7), CG_LTYPE_AUTO);
	CHECK_EQ(cg_link_type_norm(255), CG_LTYPE_AUTO);
	CHECK(!strcmp(cg_link_type_name(7), "auto"));
}

/* The global knobs apply to every link on top of its preset: with them off,
 * a link's model is its preset; eta_buffer on turns the buffer on for all;
 * a global capacity_loss_pct is every link's. auto with the knobs off is
 * all zeros: exactly the engine from before. */
static void test_linkmodel_global(void)
{
	struct cg_link_model m;
	uint8_t type[CG_MAX_LINKS] = { 0 };
	uint32_t over[CG_MAX_LINKS];

	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++) {
		m = cg_link_model_of(t, 0, 0);
		CHECK(same(&m, &cg_link_presets[t].m));
		CHECK_EQ(cg_link_tracks_loss(&m), cg_link_presets[t].m.lossy_noprobe || cg_link_presets[t].m.lossy_hold);
		m = cg_link_model_of(t, 1, 0); /* eta_buffer: both, on every link */
		CHECK(m.lossy_noprobe == 1 && m.lossy_hold == 1 && cg_link_tracks_loss(&m));
		CHECK_EQ(m.loss_pct, cg_link_presets[t].m.loss_pct);
		m = cg_link_model_of(t, 0, 5);
		CHECK_EQ(m.loss_pct, 5);
		CHECK_EQ(m.lossy_noprobe, cg_link_presets[t].m.lossy_noprobe);
		CHECK_EQ(m.lossy_hold, cg_link_presets[t].m.lossy_hold);
	}
	m = cg_link_model_of(CG_LTYPE_AUTO, 0, 0);
	CHECK(m.lossy_noprobe == 0 && m.lossy_hold == 0 && m.loss_pct == 0 && !cg_link_tracks_loss(&m));
	m = cg_link_model_of(CG_LTYPE_AUTO, 0, 500); /* clamped */
	CHECK_EQ(m.loss_pct, 100);
	m = cg_link_model_of(7, 0, 0); /* unknown: auto */
	CHECK(m.lossy_noprobe == 0 && m.lossy_hold == 0 && m.loss_pct == 0);

	/* the masks: all auto, none; the global knob, all, as the last resort
	 * it always was; starlink, its own, with its cost */
	CHECK_EQ(cg_link_hold(type, CG_MAX_LINKS, 0, over), 0);
	CHECK_EQ(cg_link_hold(type, CG_MAX_LINKS, 1, over), 0xffff);
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		CHECK_EQ(over[i], 0);
	CHECK_EQ(cg_link_hold(type, 3, 1, over), 0x7);
	type[0] = CG_LTYPE_STARLINK;
	type[1] = type[2] = CG_LTYPE_CELLULAR;
	CHECK_EQ(cg_link_hold(type, CG_MAX_LINKS, 0, over), 0x1);
	CHECK_EQ(over[0], CG_STARLINK_OVER_MS * 1000u);
	CHECK(over[1] == 0 && over[2] == 0);
	CHECK_EQ(cg_link_hold(type, CG_MAX_LINKS, 1, over), 0xffff); /* the global knob too: starlink keeps its cost */
	CHECK_EQ(over[0], CG_STARLINK_OVER_MS * 1000u);
	type[0] = CG_LTYPE_AUTO;
	type[5] = CG_LTYPE_STARLINK;
	CHECK_EQ(cg_link_hold(type, CG_MAX_LINKS, 0, over), 0x20);
	/* the starlink preset: no probing and the hold while it loses, at a
	 * cost short of the last resort; no loss cut (not measured as a win) */
	m = cg_link_presets[CG_LTYPE_STARLINK].m;
	CHECK(m.lossy_noprobe == 1 && m.lossy_hold == 1 && m.loss_pct == 0);
	CHECK(m.hold_over_ms > 0 && m.hold_over_ms * 1000ull < CG_ETA_OVER_US);
}

/* Models with lossy_hold on the links of mask, each with over[i] ms of
 * cost (over NULL: 0, the last resort). */
static const struct cg_link_model *hold_models(uint16_t mask, const uint16_t *over)
{
	static struct cg_link_model m[CG_MAX_LINKS];

	memset(m, 0, sizeof(m));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		m[i].lossy_hold = (uint8_t)(mask >> i & 1);
		m[i].hold_over_ms = over ? over[i] : 0;
	}
	return m;
}

/* A lossy link is held to its highest queue only when its own model has the
 * buffer (lossy_hold): the other links, lossy or not, are as before. */
static void test_linkmodel_eta_mask(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_eta e;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	cg_eta_reset(&e);
	for (unsigned i = 0; i < 2; i++) {
		l[i].have_rate = 1;
		l[i].max_cur = l[i].max_prev = 50000000;
		l[i].q.have_min = 1;
		l[i].q.min_cur = l[i].q.min_prev = i * 25 * MS;
		l[i].queue_us = 30 * MS; /* not probed */
		l[i].lossy = 1;
		l[i].qhi_cur = 12 * MS;
	}
	cg_cap_eta(&e, l, 0x3, 10 * MS, NULL, 0); /* no models: auto */
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x0, NULL), 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x1, NULL), 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x2, NULL), 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
	cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x3, NULL), 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 12 * MS);
	/* in the mask but not lossy: no hold */
	l[0].lossy = 0;
	cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x3, NULL), 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
	CHECK(e.l[0].over_us == 0 && e.l[1].over_us == 0); /* NULL: the last resort */
	/* each held link takes its own cost */
	{
		const uint16_t over[CG_MAX_LINKS] = { 0, 40 };

		cg_cap_eta(&e, l, 0x3, 10 * MS, hold_models(0x3, over), 0);
		CHECK(e.l[1].limit_us == 12 * MS && e.l[1].over_us == 40 * MS);
	}
}

/* The cost of going past a held link's queue (eta.h's over_us): with the
 * last resort, the slower link takes everything past it, however deep its
 * own queue grows; with a cost, only while it would arrive within that
 * cost of the held link. */
static void test_linkmodel_over(void)
{
	struct cg_eta e;
	int n1 = 0;

	/* link 0: 8 Mbit/s (1 ms a 1000 B packet), held at 5 ms; link 1, 30 ms further */
	cg_eta_reset(&e);
	cg_eta_set(&e, 0, 8000000, 0, 0, 0);
	cg_eta_set(&e, 1, 8000000, 30 * MS, 0, 0);
	e.l[0].limit_us = 5 * MS;
	for (int i = 0; i < 200; i++)
		n1 += cg_eta_pick(&e, 0x3, 1000, 0) == 1;
	CHECK_EQ(n1, 195); /* past 5 packets, all of it: link 1's queue at 195 ms */
	cg_eta_reset(&e);
	cg_eta_set(&e, 0, 8000000, 0, 0, 0);
	cg_eta_set(&e, 1, 8000000, 30 * MS, 0, 0);
	e.l[0].limit_us = 5 * MS;
	e.l[0].over_us = 40 * MS;
	n1 = 0;
	for (int i = 0; i < 200; i++)
		n1 += cg_eta_pick(&e, 0x3, 1000, 0) == 1;
	/* link 1 takes packets while 30 ms + its queue stays within link 0's
	 * arrival plus 40 ms: its queue stays near link 0's plus 10 ms, and the
	 * rest goes on link 0, to be dropped there (what TCP has to see) */
	CHECK(n1 > 5 && n1 < 110);
	CHECK(e.l[1].backlog / 8000000 < e.l[0].backlog / 8000000 + 12 * MS);
}

/* The tick's side (capacity.h): a link tracked for loss and lossy is not
 * probed, unless lossy_probe; untracked (auto), a lossy sample changes
 * nothing. */
static void test_linkmodel_probe(void)
{
	struct cg_cap_cfg cc = { .window_ms = 10000, .sat_us = 20 * MS };
	struct cg_cap_link l;
	uint64_t t = 1000, rx = 0, pk = 0;

	for (int k = 0; k < 3; k++) {
		struct cg_link_model m = cg_link_model_of(k == 0 ? CG_LTYPE_AUTO : CG_LTYPE_STARLINK, k == 2, 0);

		cc.lossy = cg_link_tracks_loss(&m);
		cc.lossy_probe = !m.lossy_noprobe;
		cg_cap_reset(&l);
		t = 1000, rx = 0, pk = 0;
		for (int i = 0; i < 6; i++) {
			/* 1000 packets of 1000 B a sample, 2 % lost, no queue */
			pk += 1000;
			rx += 980;
			t += 1000;
			cg_cap_tick(&l, t, t, (uint32_t)rx, pk, pk * 1000, NULL, &cc);
		}
		if (k == 0)
			CHECK(!l.lossy && !l.noprobe);
		else
			CHECK(l.lossy && l.noprobe == cg_link_presets[CG_LTYPE_STARLINK].m.lossy_noprobe);
	}
}

/* The clockwork audit's knobs (cg_link_knobs): each set is the value of
 * every link over its preset, with its companion; exp_link_types narrows
 * them (and eta_buffer, capacity_loss_pct) to some types; none set, every
 * model is its preset. */
static void test_linkmodel_knobs(void)
{
	struct cg_link_knobs k = { 0 };
	struct cg_link_model m, ms[CG_MAX_LINKS];
	uint8_t type[CG_MAX_LINKS] = { CG_LTYPE_STARLINK, CG_LTYPE_CELLULAR, CG_LTYPE_AUTO };
	uint32_t mask = 0;

	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++) {
		m = cg_link_model_k(t, &k);
		CHECK(same(&m, &cg_link_presets[t].m));
		m = cg_link_model_k(t, NULL);
		CHECK(same(&m, &cg_link_presets[t].m));
	}
	k.qpred_ms = 300;
	k.qpred_mode = 1;
	k.dwell_ms = 1000;
	k.dwell_oq_ms = 0; /* the companion goes with it, 0 too */
	k.eta_keep = 2;
	k.eta_keep_ms = 400;
	k.g4_episode = 9; /* clamped to the modes */
	k.rfull_hyst = 2;
	k.pace_ms = 70000; /* clamped to 16 bits */
	m = cg_link_model_k(CG_LTYPE_CELLULAR, &k);
	CHECK(m.qpred_ms == 300 && m.qpred_mode == 1 && m.dwell_ms == 1000 && m.dwell_oq_ms == 0);
	CHECK(m.eta_keep == 2 && m.eta_keep_ms == 400 && m.g4_episode == 3 && m.rfull_hyst == 2);
	CHECK_EQ(m.pace_ms, 65535);
	m = cg_link_model_k(CG_LTYPE_STARLINK, &k); /* over its preset, which stays */
	CHECK(m.lossy_hold == 1 && m.hold_over_ms == CG_STARLINK_OVER_MS && m.qpred_ms == 300);
	/* only for starlink: the cellular and auto links keep their presets */
	CHECK_EQ(cg_link_types_parse("starlink", &mask), 0);
	k.types = mask;
	k.loss_pct = 5;
	k.buffer = 1;
	cg_link_models(type, CG_MAX_LINKS, &k, ms);
	CHECK(ms[0].qpred_ms == 300 && ms[0].loss_pct == 5 && ms[0].pace_ms == 65535);
	CHECK(same(&ms[1], &cg_link_presets[CG_LTYPE_CELLULAR].m));
	CHECK(same(&ms[2], &cg_link_presets[CG_LTYPE_AUTO].m));
	{
		uint32_t w[CG_MAX_LINKS];

		CHECK(cg_link_pace_windows(ms, CG_MAX_LINKS, w));
		CHECK(w[0] == 65535000u && w[1] == 0 && w[2] == 0);
		CHECK(!cg_link_pace_windows(ms + 1, CG_MAX_LINKS - 1, w));
		/* pace_peers_ms: the other links take it, the smaller one set */
		ms[0].pace_ms = 0;
		ms[0].pace_peers_ms = 30;
		ms[2].pace_ms = 50;
		CHECK(cg_link_pace_windows(ms, 3, w));
		CHECK(w[0] == 0 && w[1] == 30000 && w[2] == 30000);
		ms[2].pace_ms = 20;
		cg_link_pace_windows(ms, 3, w);
		CHECK(w[2] == 20000);
		ms[0].pace_peers_ms = 0; /* none: only link 2's own */
		ms[2].pace_ms = 0;
		CHECK(!cg_link_pace_windows(ms, 3, w));
	}
	/* the list of types */
	CHECK_EQ(cg_link_types_parse("cellular,starlink  radio", &mask), 0);
	CHECK_EQ(mask, (1u << CG_LTYPE_CELLULAR) | (1u << CG_LTYPE_STARLINK) | (1u << CG_LTYPE_RADIO));
	CHECK_EQ(cg_link_types_parse("", &mask), 0);
	CHECK_EQ(mask, 0);
	CHECK_EQ(cg_link_types_parse("starlink sat", &mask), -1);
	CHECK_EQ(cg_link_types_parse("averyveryverylongname", &mask), -1);
}

/* redund_pace_ms per link (cg_eta_copies' windows): the fastest link always
 * gets the copy; a slow one with a window only while its copy arrives within
 * it; one without a window or without a rate, always. */
static void test_linkmodel_copies(void)
{
	struct cg_eta e;
	uint32_t w[CG_MAX_LINKS] = { 0, 30 * MS };
	int n1 = 0;

	cg_eta_reset(&e);
	cg_eta_set(&e, 0, 80000000, 0, 0, 0);      /* 80 Mbit/s: 1000 B in 0.1 ms */
	cg_eta_set(&e, 1, 8000000, 20 * MS, 0, 0); /* 8 Mbit/s, 20 ms further: 1 ms each */
	/* at a low load (1 ms apart) both get every copy */
	for (int i = 0; i < 10; i++)
		CHECK_EQ(cg_eta_copies(&e, 0x3, 1000, (uint64_t)i * MS, w), 0x3);
	/* 1000 at once: link 1 takes copies only while they arrive in the window */
	for (int i = 0; i < 1000; i++) {
		uint16_t m = cg_eta_copies(&e, 0x3, 1000, 20 * MS, w);

		CHECK(m & 0x1);
		n1 += m >> 1 & 1;
	}
	/* within 30 ms of the fast copy, which queues 0.1 ms each: 20 + n1 <= 30 + 0.1 k */
	CHECK(n1 >= 100 && n1 <= 115);
	/* without a window, link 1 gets every copy however late */
	w[1] = 0;
	CHECK_EQ(cg_eta_copies(&e, 0x3, 1000, 20 * MS, w), 0x3);
	/* a link without a rate gets every copy */
	w[1] = 30 * MS;
	cg_eta_set(&e, 2, 0, 0, 0, 20 * MS);
	CHECK(cg_eta_copies(&e, 0x7, 1000, 20 * MS, w) & 0x4);
}

/* The per-link ETA knobs through cg_cap_eta: a link's qpred and keep come
 * from its own model only. */
static void test_linkmodel_eta_knobs(void)
{
	struct cg_cap_link l[CG_MAX_LINKS];
	struct cg_link_model m[CG_MAX_LINKS];
	struct cg_eta e;

	memset(m, 0, sizeof(m));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_cap_reset(&l[i]);
	cg_eta_reset(&e);
	for (unsigned i = 0; i < 2; i++) {
		l[i].have_rate = 1;
		l[i].max_cur = l[i].max_prev = 8000000;
		l[i].q.have_min = 1;
		l[i].queue_us = 30 * MS; /* not probed */
		l[i].have_s1 = 1;
		l[i].s1 = 100 * MS; /* rising 100 ms a second */
	}
	m[1].qpred_ms = 500;
	m[1].qpred_mode = 1;
	m[1].eta_keep = 1;
	m[1].eta_keep_ms = 400;
	cg_cap_eta(&e, l, 0x3, 10 * MS, m, 0);
	CHECK_EQ(e.l[0].backlog, 8000000ull * 30 * MS); /* its queue now */
	CHECK_EQ(e.l[1].backlog, 8000000ull * 80 * MS); /* where it will be in 500 ms */
	CHECK(e.l[0].keep == 0 && e.l[1].keep == 1 && e.l[1].keep_ms == 400);
}

void test_linkmodel(void)
{
	test_linkmodel_knobs();
	test_linkmodel_copies();
	test_linkmodel_eta_knobs();
	test_linkmodel_over();
	test_linkmodel_probe();
	test_linkmodel_table();
	test_linkmodel_global();
	test_linkmodel_eta_mask();
}
