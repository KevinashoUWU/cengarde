/* The link model (linkmodel.h, story 015): the preset table, the names,
 * how the global knobs combine with a type, and the masks the loops take.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "capacity.h"
#include "linkmodel.h"
#include "test.h"

#define MS 1000u

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
	CHECK(!memcmp(&cg_link_presets[CG_LTYPE_AUTO].m, &zero, sizeof(zero)));
	/* the hooks keep auto's values until measured */
	CHECK(!memcmp(&cg_link_presets[CG_LTYPE_CELLULAR].m, &zero, sizeof(zero)));
	CHECK(!memcmp(&cg_link_presets[CG_LTYPE_FIBER].m, &zero, sizeof(zero)));
	CHECK(!memcmp(&cg_link_presets[CG_LTYPE_ETHERNET].m, &zero, sizeof(zero)));
	CHECK(!memcmp(&cg_link_presets[CG_LTYPE_RADIO].m, &zero, sizeof(zero)));
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

	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++) {
		m = cg_link_model_of(t, 0, 0);
		CHECK(!memcmp(&m, &cg_link_presets[t].m, sizeof(m)));
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

	/* the masks: all auto, none; the global knob, all; starlink, its own */
	CHECK_EQ(cg_link_hold_mask(type, CG_MAX_LINKS, 0), 0);
	CHECK_EQ(cg_link_hold_mask(type, CG_MAX_LINKS, 1), 0xffff);
	CHECK_EQ(cg_link_hold_mask(type, 3, 1), 0x7);
	type[0] = CG_LTYPE_STARLINK;
	type[1] = type[2] = CG_LTYPE_CELLULAR;
	CHECK_EQ(cg_link_hold_mask(type, CG_MAX_LINKS, 0), cg_link_presets[CG_LTYPE_STARLINK].m.lossy_hold ? 0x1 : 0);
	type[0] = CG_LTYPE_AUTO;
	type[5] = CG_LTYPE_STARLINK;
	CHECK_EQ(cg_link_hold_mask(type, CG_MAX_LINKS, 0), cg_link_presets[CG_LTYPE_STARLINK].m.lossy_hold ? 0x20 : 0);
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
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x0, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x1, 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 0);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x2, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x3, 0);
	CHECK(e.l[0].limit_us == 12 * MS && e.l[1].limit_us == 12 * MS);
	/* in the mask but not lossy: no hold */
	l[0].lossy = 0;
	cg_cap_eta(&e, l, 0x3, 10 * MS, 1, 0, 0x3, 0);
	CHECK(e.l[0].limit_us == 0 && e.l[1].limit_us == 12 * MS);
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

void test_linkmodel(void)
{
	test_linkmodel_probe();
	test_linkmodel_table();
	test_linkmodel_global();
	test_linkmodel_eta_mask();
}
