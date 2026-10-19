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
	       a->hold_over_ms == b->hold_over_ms && a->loss_pct == b->loss_pct && a->pace_ms == b->pace_ms &&
	       a->pace_peers_ms == b->pace_peers_ms;
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

/* A lossy link is held to its highest queue only when its own model has the
 * buffer (cg_cap_eta's mask): the other links, lossy or not, are as before. */
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
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x0, NULL, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x1, NULL, 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x2, NULL, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x3, NULL, 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 12 * MS);
	/* in the mask but not lossy: no hold */
	l[0].lossy = 0;
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x3, NULL, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
	CHECK(e.l[0].over_us == 0 && e.l[1].over_us == 0); /* NULL: the last resort */
	/* each held link takes its own cost */
	{
		const uint32_t over[CG_MAX_LINKS] = { 0, 40 * MS };

		cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x3, over, 0);
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

/* pace_peers_ms: the starlink preset paces the copies to the other links of
 * its session, not its own; every link takes the smaller window set (its
 * own pace_ms or a peer's pace_peers_ms); a session without one is as
 * before (no window anywhere). */
static void test_linkmodel_pace_windows(void)
{
	struct cg_link_model ms[CG_MAX_LINKS];
	uint8_t type[CG_MAX_LINKS] = { 0 };
	uint32_t w[CG_MAX_LINKS];
	const struct cg_link_model st = cg_link_presets[CG_LTYPE_STARLINK].m;

	CHECK(st.pace_ms == 0 && st.pace_peers_ms == CG_STARLINK_PACE_PEERS_MS);
	CHECK_EQ(CG_STARLINK_PACE_PEERS_MS, 60);
	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++)
		if (t != CG_LTYPE_STARLINK)
			CHECK(cg_link_presets[t].m.pace_ms == 0 && cg_link_presets[t].m.pace_peers_ms == 0);
	/* every link auto, or cellular: no window, the engine as before */
	CHECK(!cg_link_pace_types(type, CG_MAX_LINKS, 0, w));
	type[0] = type[1] = CG_LTYPE_CELLULAR;
	CHECK(!cg_link_pace_types(type, CG_MAX_LINKS, 0, w));
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		CHECK_EQ(w[i], 0);
	/* a starlink link: the others paced at 60 ms, not itself */
	type[2] = CG_LTYPE_STARLINK;
	CHECK(cg_link_pace_types(type, CG_MAX_LINKS, 0, w));
	CHECK(w[0] == 60 * MS && w[1] == 60 * MS && w[2] == 0 && w[3] == 60 * MS);
	/* the global redund_pace_ms is every link's own, the smaller one wins */
	CHECK(cg_link_pace_types(type, CG_MAX_LINKS, 30, w));
	CHECK(w[0] == 30 * MS && w[2] == 30 * MS);
	CHECK(cg_link_pace_types(type, CG_MAX_LINKS, 100, w));
	CHECK(w[0] == 60 * MS && w[2] == 100 * MS);
	type[2] = CG_LTYPE_AUTO;
	CHECK(cg_link_pace_types(type, CG_MAX_LINKS, 100, w));
	CHECK(w[0] == 100 * MS && w[2] == 100 * MS);
	/* by models: the other links take it, the smaller one set */
	memset(ms, 0, sizeof(ms));
	ms[0].pace_peers_ms = 30;
	ms[2].pace_ms = 50;
	CHECK(cg_link_pace_windows(ms, 3, w));
	CHECK(w[0] == 0 && w[1] == 30 * MS && w[2] == 30 * MS);
	ms[2].pace_ms = 20;
	cg_link_pace_windows(ms, 3, w);
	CHECK(w[2] == 20 * MS);
	ms[0].pace_peers_ms = 0; /* none: only link 2's own */
	CHECK(cg_link_pace_windows(ms, 3, w));
	CHECK(w[0] == 0 && w[1] == 0 && w[2] == 20 * MS);
	ms[2].pace_ms = 0;
	CHECK(!cg_link_pace_windows(ms, 3, w));
}

/* cg_eta_copies' windows: the fastest link always gets the copy; a slow one
 * with a window only while its copy arrives within it; one without a window
 * or without a rate, always. */
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
	/* a link outside the set gets none */
	CHECK_EQ(cg_eta_copies(&e, 0x1, 1000, 20 * MS, w), 0x1);
}

void test_linkmodel(void)
{
	test_linkmodel_over();
	test_linkmodel_probe();
	test_linkmodel_table();
	test_linkmodel_global();
	test_linkmodel_eta_mask();
	test_linkmodel_pace_windows();
	test_linkmodel_copies();
}
