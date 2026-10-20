/* The link model (story 015): what kind of link each one is, and the
 * per-link parameters the capacity and split loops read for it.
 *
 * Every loop used to treat every link as cellular: a modem that, full,
 * queues deeply, so that its queue says it is full (capacity.h) and the
 * split by expected arrival (eta.h) spills to another link once the queue
 * passes the difference in base delay. Starlink has a shallow buffer
 * (~15 ms) and drops instead: the split never saw it full, the cellular
 * links stayed idle and Starlink dropped TCP's bursts (field, 2026-10-18:
 * the bond under Starlink alone; bench shallowq and starsat).
 *
 * A link's type is set in its [link] section (type = ..., config.h) and
 * sent to the server in the probes' bonding tail (CG_BOND_TYPE, proto.h),
 * so that the server treats the download of each path the same way. auto
 * (0, the default, and what a peer without the field sends) is exactly the
 * behaviour from before. The other types each take a preset; for now only
 * starlink differs from auto, and cellular, fiber, ethernet and radio are
 * hooks to tune from measurements: they keep auto's values.
 *
 * The global knobs (eta_buffer, capacity_loss_pct) still apply to every
 * link, on top of the preset: cg_link_model_of.
 *
 * Pure: no clock, no I/O, no allocation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LINKMODEL_H
#define CG_LINKMODEL_H

#include <stdint.h>
#include <string.h>

/* The types, as sent on the wire (3 bits, CG_BOND_TYPE): never renumber.
 * A value past the last known reads as auto. */
enum cg_link_type {
	CG_LTYPE_AUTO = 0,     /* today's behaviour, the default */
	CG_LTYPE_CELLULAR = 1, /* LTE/5G modem: deep queue when full */
	CG_LTYPE_STARLINK = 2, /* satellite: shallow buffer, drops instead of queueing */
	CG_LTYPE_FIBER = 3,    /* fibre to the home */
	CG_LTYPE_ETHERNET = 4, /* cable, DSL, an unmanaged uplink */
	CG_LTYPE_RADIO = 5,    /* point to point, wireless WAN */
	CG_LTYPE_COUNT
};
#define CG_LTYPE_MAX 7 /* the largest value 3 bits carry */

/* What a link's type changes in the loops. Zero everywhere is auto.
 *
 * Planned, each a field here once it measures as a win (story 015, "lo
 * que falta"): for starlink, a capacity ceiling learned from where loss
 * starts (its frontier) rather than from a queue, caution as soon as it
 * starts losing, the 15 s reconfiguration slots (FEC armed or traffic moved
 * just before a slot's edge) and the delay ramp within a slot; for
 * cellular, the cell scheduler's response (PF, RR, max-rate), the deep
 * queue and the loss that follows a weak signal (RSRP/SINR). */
struct cg_link_model {
	/* A link that lost over CG_CAP_LOSSY_PERMIL of what it was sent in
	 * the last CG_CAP_LOSSY_MS (capacity.h's lossy) is not probed at 5/4:
	 * its queue is short because it drops, not because it has room. */
	uint8_t lossy_noprobe;
	/* Such a link is held, in the split by expected arrival (cg_cap_eta),
	 * to the highest queue it showed over CG_CAP_QHI_MS: past it a packet
	 * would be dropped. */
	uint8_t lossy_hold;
	/* What going past that queue costs a packet in the split by expected
	 * arrival, in ms (eta.h's over_us): it goes on another link while it
	 * would arrive there within this of what it would here. 0: last
	 * resort (eta_buffer's), and the other links' deep queues took it all:
	 * the cellular links filled to 150-220 ms (story 015). */
	uint16_t hold_over_ms;
	/* capacity_loss_pct for this link: loaded and losing more than this
	 * (%) for CG_CAP_LOSS_N samples, it is full; 0: off. */
	uint8_t loss_pct;
	/* redund_pace_ms for this link: a copy goes on it only if it arrives
	 * within this of the first (eta.h's cg_eta_copies); 0: every copy. */
	uint16_t pace_ms;
	/* In a session with a link of this type, every other link paces its
	 * copies at this (as its pace_ms, the smaller of the two): the copies
	 * to a deep-queued link next to Starlink arrive in time for its gaps
	 * (cg_link_pace_windows), and a session without such a link is as
	 * before. 0: off. */
	uint16_t pace_peers_ms;
};

/* starlink's hold_over_ms (story 015, n = 8 against a null control): 40,
 * 100 and 200 all gained 27-58 % where Starlink's queue is shallow and its
 * base delay shorter (shallowq, starsat), but 100 lost 6-10 % and 200
 * 20-38 % where the delays are equal (shallowqd), the cellular queues
 * filled; 40 stayed even or better there (+2-5 %) and behind a deep queue
 * (shallowqdeep). */
#define CG_STARLINK_OVER_MS 40

/* starlink's pace_peers_ms (story 015, redundancy, n = 16 against a null
 * control): the copies to the cellular links next to it only while they
 * arrive within 60 ms of the first. The 1200 B ping's p99 in starsat fell
 * from ~360 to ~95 ms, with a throughput within 5 % in every Starlink case;
 * the cost is retransmissions (1.2-4x) where its queue is shallow. Pacing
 * every cellular link instead (pace_ms) cost 8-18 % in het: hence only next
 * to a Starlink. */
#define CG_STARLINK_PACE_PEERS_MS 60

struct cg_link_preset {
	const char *name;
	struct cg_link_model m;
};

/* The presets, by type. Only what measured as a win goes in (story 015,
 * n >= 8 against a null control); the rest keep auto's zeros as hooks.
 *                          noprobe hold over_ms loss_pct pace_ms pace_peers_ms */
static const struct cg_link_preset cg_link_presets[CG_LTYPE_COUNT] = {
	[CG_LTYPE_AUTO] = { "auto", { 0, 0, 0, 0, 0, 0 } },
	[CG_LTYPE_CELLULAR] = { "cellular", { 0, 0, 0, 0, 0, 0 } }, /* hook: the scheduler, the deep queue, the signal */
	[CG_LTYPE_STARLINK] = { "starlink", { 1, 1, CG_STARLINK_OVER_MS, 0, 0, CG_STARLINK_PACE_PEERS_MS } },
	[CG_LTYPE_FIBER] = { "fiber", { 0, 0, 0, 0, 0, 0 } },       /* hook */
	[CG_LTYPE_ETHERNET] = { "ethernet", { 0, 0, 0, 0, 0, 0 } }, /* hook */
	[CG_LTYPE_RADIO] = { "radio", { 0, 0, 0, 0, 0, 0 } },       /* hook */
};

/* A type as read from the wire or a config: past the known ones, auto. */
static inline unsigned cg_link_type_norm(unsigned t)
{
	return t < CG_LTYPE_COUNT ? t : CG_LTYPE_AUTO;
}

static inline const char *cg_link_type_name(unsigned t)
{
	return cg_link_presets[cg_link_type_norm(t)].name;
}

/* The type named s, or -1 for none. */
static inline int cg_link_type_parse(const char *s)
{
	for (unsigned t = 0; t < CG_LTYPE_COUNT; t++)
		if (!strcmp(s, cg_link_presets[t].name))
			return (int)t;
	return -1;
}

/* A link's model: its type's preset, with the global knobs on top. A
 * global eta_buffer turns lossy_noprobe and lossy_hold on for every link
 * (what it did before the types); a global capacity_loss_pct (not 0) is
 * every link's, over its preset's. */
static inline struct cg_link_model cg_link_model_of(unsigned type, int g_buffer, uint32_t g_loss_pct)
{
	struct cg_link_model m = cg_link_presets[cg_link_type_norm(type)].m;

	if (g_buffer)
		m.lossy_noprobe = m.lossy_hold = 1;
	if (g_loss_pct)
		m.loss_pct = (uint8_t)(g_loss_pct > 100 ? 100 : g_loss_pct);
	return m;
}

/* Each link's copy window into w_us[0..n) (n <= 16), cg_eta_copies': its
 * own pace_ms or another link's pace_peers_ms, the smaller one set, in us;
 * 0: every copy. Returns whether any link has one (redundancy then keeps an
 * expected arrival per link). Links not in use are auto: they set nothing. */
static inline int cg_link_pace_windows(const struct cg_link_model *m, unsigned n, uint32_t *w_us)
{
	int any = 0;

	n = n > 16 ? 16 : n;
	for (unsigned i = 0; i < n; i++) {
		uint32_t v = m[i].pace_ms;

		for (unsigned j = 0; j < n; j++)
			if (j != i && m[j].pace_peers_ms && (!v || m[j].pace_peers_ms < v))
				v = m[j].pace_peers_ms;
		w_us[i] = v * 1000;
		any |= v != 0;
	}
	return any;
}

/* The copy windows of the links of type[0..n) (n <= 16, at most 16 models),
 * with the global redund_pace_ms (g_pace_ms, not 0) as every link's pace_ms
 * over its preset, and the global pace_peers_ms (peers_ms, >= 0; 0: off) as
 * the pace_peers_ms of the types whose preset has one: cg_link_pace_windows. */
static inline int cg_link_pace_types(const uint8_t *type, unsigned n, uint32_t g_pace_ms, int32_t peers_ms,
				     uint32_t *w_us)
{
	struct cg_link_model m[16];

	n = n > 16 ? 16 : n;
	for (unsigned i = 0; i < n; i++) {
		m[i] = cg_link_presets[cg_link_type_norm(type[i])].m;
		if (g_pace_ms)
			m[i].pace_ms = (uint16_t)(g_pace_ms > 65535 ? 65535 : g_pace_ms);
		if (peers_ms >= 0 && m[i].pace_peers_ms)
			m[i].pace_peers_ms = (uint16_t)(peers_ms > 65535 ? 65535 : peers_ms);
	}
	return cg_link_pace_windows(m, n, w_us);
}

/* What capacity.h's tick takes from a model: whether it tracks the link's
 * losses (lossy) and, lossy, still probes it. */
static inline int cg_link_tracks_loss(const struct cg_link_model *m)
{
	return m->lossy_noprobe || m->lossy_hold;
}

/* The links of type[0..n) (n <= 16) whose model holds them to their queue
 * (lossy_hold), cg_cap_eta's mask, and in over_us[0..n) what going past it
 * costs each, in us (0: last resort). */
static inline uint16_t cg_link_hold(const uint8_t *type, unsigned n, int g_buffer, uint32_t *over_us)
{
	uint16_t m = 0;

	for (unsigned i = 0; i < n && i < 16; i++) {
		struct cg_link_model lm = cg_link_model_of(type[i], g_buffer, 0);

		over_us[i] = (uint32_t)lm.hold_over_ms * 1000;
		if (lm.lossy_hold)
			m |= (uint16_t)(1u << i);
	}
	return m;
}

#endif
