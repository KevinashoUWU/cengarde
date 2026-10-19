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
 * The global knobs (eta_buffer, capacity_loss_pct and the clockwork
 * audit's experiments) still apply to every link, on top of the preset, or
 * with exp_link_types only to the links of some types: cg_link_model_k.
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

	/* The clockwork audit's mixed fixes (2026-10-17/18), each brought down
	 * to the links where it helps (story 015, "las correcciones mixtas por
	 * tipo"). Each is the global knob of the same name for this link; 0
	 * everywhere is the engine without them. */
	uint8_t g4_episode;   /* cap_g4_episode: a G4 leave for the queue counts for fast start (capacity.h) */
	uint8_t rfull_hyst;   /* rescue_full_hyst: the rescue's full bit with hysteresis (capacity.h) */
	uint8_t qpred_mode;   /* split_qpred_mode */
	uint8_t eta_keep;     /* eta_keep: the ETA backlog over a stale queue (eta.h) */
	uint16_t qpred_ms;    /* split_qpred_ms: the ETA takes its queue where its trend puts it */
	uint16_t dwell_ms;    /* split_dwell_ms: G4 keeps it in this long once back by a recovery (split.h) */
	uint16_t dwell_oq_ms; /* split_dwell_oq_ms: ... only while its own queue is at most this, 0: any */
	uint16_t eta_keep_ms; /* eta_keep_ms */
	uint16_t pace_ms;     /* redund_pace_ms: a copy goes on it only if it arrives within this of the first (eta.h) */
};

/* starlink's hold_over_ms (story 015, n = 8 against a null control): 40,
 * 100 and 200 all gained 27-58 % where Starlink's queue is shallow and its
 * base delay shorter (shallowq, starsat), but 100 lost 6-10 % and 200
 * 20-38 % where the delays are equal (shallowqd), the cellular queues
 * filled; 40 stayed even or better there (+2-5 %) and behind a deep queue
 * (shallowqdeep). */
#define CG_STARLINK_OVER_MS 40

struct cg_link_preset {
	const char *name;
	struct cg_link_model m;
};

/* The presets, by type. Only what measured as a win goes in (story 015,
 * n >= 8 against a null control); the rest keep auto's zeros as hooks. */
static const struct cg_link_preset cg_link_presets[CG_LTYPE_COUNT] = {
	[CG_LTYPE_AUTO] = { "auto", { 0 } },
	[CG_LTYPE_CELLULAR] = { "cellular", { 0 } }, /* hook: the scheduler, the deep queue, the signal */
	[CG_LTYPE_STARLINK] = { "starlink",
				{ .lossy_noprobe = 1, .lossy_hold = 1, .hold_over_ms = CG_STARLINK_OVER_MS } },
	[CG_LTYPE_FIBER] = { "fiber", { 0 } },       /* hook */
	[CG_LTYPE_ETHERNET] = { "ethernet", { 0 } }, /* hook */
	[CG_LTYPE_RADIO] = { "radio", { 0 } },       /* hook */
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

/* The global knobs that reach a link's model (config.h): each set (not 0)
 * is the value of every link of the types in types, over its preset's. */
struct cg_link_knobs {
	int buffer;          /* eta_buffer: lossy_noprobe and lossy_hold */
	uint32_t loss_pct;   /* capacity_loss_pct */
	uint32_t g4_episode; /* cap_g4_episode */
	uint32_t rfull_hyst; /* rescue_full_hyst */
	uint32_t qpred_ms;   /* split_qpred_ms, with split_qpred_mode */
	uint32_t qpred_mode;
	uint32_t dwell_ms; /* split_dwell_ms, with split_dwell_oq_ms */
	uint32_t dwell_oq_ms;
	uint32_t eta_keep; /* eta_keep, with eta_keep_ms */
	uint32_t eta_keep_ms;
	uint32_t pace_ms; /* redund_pace_ms */
	/* exp_link_types: a bit per type (1 << CG_LTYPE_...) the knobs above
	 * apply to; 0: every type, as before the types. To try a knob on one
	 * type of link before it goes in that type's preset. */
	uint32_t types;
};

static inline uint16_t cg_link_u16(uint32_t v)
{
	return (uint16_t)(v > 65535 ? 65535 : v);
}

static inline uint8_t cg_link_u8(uint32_t v, uint32_t max)
{
	return (uint8_t)(v > max ? max : v);
}

/* A link's model: its type's preset, with the global knobs k (NULL: none)
 * on top. A global eta_buffer turns lossy_noprobe and lossy_hold on (what
 * it did before the types); any other knob set (not 0) is the link's value
 * over its preset's, and a knob's companion (split_qpred_mode,
 * split_dwell_oq_ms, eta_keep_ms) goes with it. All of it only for the
 * types in k->types (0: every one). */
static inline struct cg_link_model cg_link_model_k(unsigned type, const struct cg_link_knobs *k)
{
	unsigned t = cg_link_type_norm(type);
	struct cg_link_model m = cg_link_presets[t].m;

	if (!k || (k->types && !(k->types >> t & 1)))
		return m;
	if (k->buffer)
		m.lossy_noprobe = m.lossy_hold = 1;
	if (k->loss_pct)
		m.loss_pct = cg_link_u8(k->loss_pct, 100);
	if (k->g4_episode)
		m.g4_episode = cg_link_u8(k->g4_episode, 3);
	if (k->rfull_hyst)
		m.rfull_hyst = cg_link_u8(k->rfull_hyst, 2);
	if (k->qpred_ms) {
		m.qpred_ms = cg_link_u16(k->qpred_ms);
		m.qpred_mode = cg_link_u8(k->qpred_mode, 2);
	}
	if (k->dwell_ms) {
		m.dwell_ms = cg_link_u16(k->dwell_ms);
		m.dwell_oq_ms = cg_link_u16(k->dwell_oq_ms);
	}
	if (k->eta_keep) {
		m.eta_keep = cg_link_u8(k->eta_keep, 3);
		m.eta_keep_ms = cg_link_u16(k->eta_keep_ms);
	}
	if (k->pace_ms)
		m.pace_ms = cg_link_u16(k->pace_ms);
	return m;
}

/* The same with only eta_buffer and capacity_loss_pct as globals. */
static inline struct cg_link_model cg_link_model_of(unsigned type, int g_buffer, uint32_t g_loss_pct)
{
	struct cg_link_knobs k = { .buffer = g_buffer, .loss_pct = g_loss_pct };

	return cg_link_model_k(type, &k);
}

/* The models of the links of type[0..n) (n <= 16) into m[0..n). */
static inline void cg_link_models(const uint8_t *type, unsigned n, const struct cg_link_knobs *k,
				  struct cg_link_model *m)
{
	for (unsigned i = 0; i < n && i < 16; i++)
		m[i] = cg_link_model_k(type[i], k);
}

/* Whether any of m[0..n) paces its copies (pace_ms): redundancy then keeps
 * an expected arrival per link (cg_eta_copies). */
static inline int cg_link_any_pace(const struct cg_link_model *m, unsigned n)
{
	for (unsigned i = 0; i < n && i < 16; i++)
		if (m[i].pace_ms)
			return 1;
	return 0;
}

/* What capacity.h's tick takes from a model: whether it tracks the link's
 * losses (lossy) and, lossy, still probes it. */
static inline int cg_link_tracks_loss(const struct cg_link_model *m)
{
	return m->lossy_noprobe || m->lossy_hold;
}

/* The links of m[0..n) (n <= 16) whose model holds them to their queue
 * (lossy_hold), and in over_us[0..n) what going past it costs each, in us
 * (0: last resort). */
static inline uint16_t cg_link_hold_m(const struct cg_link_model *m, unsigned n, uint32_t *over_us)
{
	uint16_t h = 0;

	for (unsigned i = 0; i < n && i < 16; i++) {
		over_us[i] = (uint32_t)m[i].hold_over_ms * 1000;
		if (m[i].lossy_hold)
			h |= (uint16_t)(1u << i);
	}
	return h;
}

/* The same from the links' types and a global eta_buffer. */
static inline uint16_t cg_link_hold(const uint8_t *type, unsigned n, int g_buffer, uint32_t *over_us)
{
	struct cg_link_model m[16];
	struct cg_link_knobs k = { .buffer = g_buffer };

	n = n > 16 ? 16 : n;
	cg_link_models(type, n, &k, m);
	return cg_link_hold_m(m, n, over_us);
}

/* exp_link_types' mask from a list of type names ("starlink cellular",
 * spaces or commas): 0, and the mask in *mask; -1 for a name it does not
 * know. */
static inline int cg_link_types_parse(const char *s, uint32_t *mask)
{
	char w[16];
	uint32_t m = 0;

	while (*s) {
		size_t n = 0;
		int t;

		while (*s == ' ' || *s == ',' || *s == '\t')
			s++;
		if (!*s)
			break;
		while (s[n] && s[n] != ' ' && s[n] != ',' && s[n] != '\t')
			n++;
		if (n >= sizeof(w))
			return -1;
		memcpy(w, s, n);
		w[n] = 0;
		s += n;
		t = cg_link_type_parse(w);
		if (t < 0)
			return -1;
		m |= 1u << t;
	}
	*mask = m;
	return 0;
}

#endif
