/* Splitting the send direction: sharing the packets out among the links
 * instead of copying each one to all of them (story 012, step 1 of the
 * bonding plan). This is the decision only; the send paths of the client and
 * the server (client.c, server.c) call it once per packet.
 *
 * - Eligible links: the live, active (not muted) links with a weight above
 *   zero whose peer still answers on them (guard G3 below), and of those the
 *   ones within split_spread_ms of the fastest take a share (guard G4
 *   below; the others carry a copy of every packet, or with
 *   split_spread_copy off only what every carrier carries: the
 *   duplicated packets). The caller builds the mask from its link masks and
 *   health (health.h), not from cg_health_carriers: its fallbacks (a muted
 *   link when none is active, every link when none is live) must end in
 *   duplication, which is what a packet gets here when no link is eligible.
 * - Smooth weighted round-robin (nginx's): each pick adds every eligible
 *   link's weight to its credit, takes the link with the most credit and
 *   charges it the sum of the weights. Weights 5:1:1 give a a b a c a a, not
 *   a a a a a b c, so a link's share is spread out instead of sent in bursts
 *   that fill its queue. The credits of the links that left the set are
 *   zeroed, so a link that comes back starts even.
 * - Duplicated packets (pick -1): WireGuard's handshake messages (types 1 to
 *   3: a lost one costs a retry of seconds), small datagrams (keepalives, TCP
 *   ACKs: their loss costs more than the bytes saved) and everything while
 *   no link is eligible. They go to every carrier, as without a split.
 * - Guard G2: with fewer than 2 eligible links everything goes to every
 *   carrier, as up to 0.6.2. With split_spread_copy off, one eligible link
 *   takes every packet that would be split instead, so long as it is not
 *   every carrier there is (then nothing would be skipped, and the packet
 *   is a duplicated one): the others are out for a reason, late (G4), muted
 *   (G1) or not answering (G3), and on the Pi's 5G links (story 012,
 *   2026-10-13) copying loaded the links that were already late with the
 *   whole stream: their queues grew to 2.5 s, the sockets refused thousands
 *   of packets and the split fell apart.
 * - Weight 0 (libRIST's meaning): the link carries a copy of every packet,
 *   split or not. These links are dup0 and are never picked.
 * - CG_F_SPLIT (proto.h) marks a packet that skips at least one carrier. It
 *   is under the MAC, so it is decided before the header is written.
 * - Whether an end splits at all (cg_split_active) depends on its split
 *   setting (config.h) and on what the peer's newest probe tail says
 *   (cg_probe_bond, proto.h; cg_split_probe_tail and cg_split_reply_tail are
 *   what each end writes in its own):
 *   force always (the lab); a client's on once the server says it reorders,
 *   with its [link] weights; a server's client when the client asks for it
 *   and says it reorders, with the weights the client sends per link; off
 *   never. A peer without the tail says neither, so nothing is split towards
 *   an engine that cannot put it back in order.
 *
 * Without the reorderer at the receiving end, and the steps that follow (loss
 * detection per link, a queue per link), a split stream is slower and less
 * reliable than redundancy: that is why it is experimental. Pure functions,
 * no clock and no I/O (docs/historias/012).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_SPLIT_H
#define CG_SPLIT_H

#include <stdint.h>
#include <string.h>

#include "arrival.h"
#include "eta.h" /* CG_MAX_LINKS */
#include "config.h"  /* enum cg_split_mode */
#include "health.h"  /* struct cg_hlink: each link's delay */
#include "proto.h"   /* CG_F_SPLIT, struct cg_probe_bond */
#include "wgsize.h"  /* CG_WGS_DATA */

/* Guard G3: a link with no verified packet from the peer for more than this
 * many probe intervals is not eligible, though it still carries the
 * duplicated packets. The stall detection (health.h) takes the same three
 * intervals plus a second of slack; this shortens the time a dead link keeps
 * its share by that second. */
#define CG_SPLIT_STALE_PROBES 3

struct cg_split {
	int32_t cur[CG_MAX_LINKS]; /* the credits of the smooth round-robin */
};

/* Back to an even start, e.g. when the weights change. */
static inline void cg_split_reset(struct cg_split *s)
{
	memset(s, 0, sizeof(*s));
}

/* Where one packet goes, given the masks of cg_split_spread_apply.
 * carriers: the links that carry payload at all (cg_health_carriers), for
 * G2, or 0 when one link left must not take a packet alone (then it goes to
 * every carrier); eligible: the links that may take a share (above); dup0:
 * the links that carry every packet (weight 0, and with split_spread_copy
 * the ones G4 keeps out; any of them in eligible is ignored); w: the weights
 * (0: none); wgtype and len: the WireGuard message type (its first byte, 0
 * for an empty datagram) and the datagram's length, 32 bytes of overhead
 * included as in wgsize.h; small_len: datagrams of up to this many bytes
 * are duplicated.
 *
 * Returns the link that carries the packet, to which cg_split_dst adds the
 * weight-0 links, and ORs CG_F_SPLIT into *flags (NULL: not asked), the
 * other bits left as they are. Returns -1, with *flags untouched, when the
 * packet goes to every carrier.
 *
 * With eta (not NULL), among 2 or more links the packet goes where it
 * arrives first (eta.h), sent at now_us; by the weights when a link has no
 * rate yet. */
static inline int cg_split_pick_eta(struct cg_split *s, struct cg_eta *eta, uint64_t now_us, uint16_t carriers,
				    uint16_t eligible, uint16_t dup0, const uint16_t w[CG_MAX_LINKS], uint8_t wgtype,
				    uint32_t len, uint32_t small_len, uint8_t *flags)
{
	uint16_t set = 0, keep, copies = dup0;
	int32_t total = 0;
	int best = -1;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((eligible >> i & 1) && !(dup0 >> i & 1)) {
			if (w[i])
				set |= (uint16_t)(1u << i);
			else
				copies |= (uint16_t)(1u << i); /* weight 0 as well, named in dup0 or not */
		}

	/* Nothing to share out among fewer than 2 links: start even when a
	 * second one comes. */
	keep = set & (set - 1) ? set : 0;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (!(keep >> i & 1))
			s->cur[i] = 0;

	if (wgtype != CG_WGS_DATA || len <= small_len || !set)
		return -1;
	if (!keep) {
		/* G2: the one link left takes the packet, when that skips a carrier
		 * (never with carriers 0: split_spread_copy). */
		for (best = 0; !(set >> best & 1); best++)
			;
		if (!(carriers & (uint16_t)~((1u << best) | copies)))
			return -1;
		if (flags)
			*flags |= CG_F_SPLIT;
		return best;
	}

	if (eta) {
		int k = cg_eta_pick(eta, keep, len, now_us);

		if (k >= 0) {
			if (flags)
				*flags |= CG_F_SPLIT;
			return k;
		}
	}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!(keep >> i & 1))
			continue;
		s->cur[i] += w[i];
		total += w[i];
		if (best < 0 || s->cur[i] > s->cur[best])
			best = i;
	}
	if (best < 0)
		return -1;
	s->cur[best] -= total;

	/* At least 2 links share the set, so the other one is skipped. */
	if (flags)
		*flags |= CG_F_SPLIT;
	return best;
}

static inline int cg_split_pick(struct cg_split *s, uint16_t carriers, uint16_t eligible, uint16_t dup0,
				const uint16_t w[CG_MAX_LINKS], uint8_t wgtype, uint32_t len, uint32_t small_len,
				uint8_t *flags)
{
	return cg_split_pick_eta(s, NULL, 0, carriers, eligible, dup0, w, wgtype, len, small_len, flags);
}

/* The links a packet is sent on, given what cg_split_pick returned: every
 * carrier for -1, else the picked link and the weight-0 carriers. carriers:
 * the links that carry payload at all (dup0 may name links that do not). */
static inline uint16_t cg_split_dst(int pick, uint16_t carriers, uint16_t dup0)
{
	if (pick < 0)
		return carriers;
	return (uint16_t)(((1u << pick) | dup0) & carriers);
}

/* The one link a packet goes on (cg_split_dst of the same arguments), or -1
 * when it goes on several or none: what its next packet tells the receiver
 * (cg_prev_flags, proto.h). */
static inline int cg_split_only(int pick, uint16_t carriers, uint16_t dup0)
{
	uint16_t d = cg_split_dst(pick, carriers, dup0);

	return d && !(d & (d - 1)) ? __builtin_ctz(d) : -1;
}

/* Whether an end with split setting mode (enum cg_split_mode) splits what it
 * sends, given the peer's newest probe tail: peer_reorders and peer_asks are
 * its CG_BOND_REORDERS and CG_BOND_SPLIT_ME (0 without a tail). */
static inline int cg_split_active(int mode, int peer_reorders, int peer_asks)
{
	switch (mode) {
	case CG_SPLIT_FORCE:
		return 1;
	case CG_SPLIT_ON: /* a client: once the server reorders */
		return !!peer_reorders;
	case CG_SPLIT_CLIENT: /* a server: when the client asks and reorders */
		return peer_reorders && peer_asks;
	default:
		return 0;
	}
}

/* Guard G4 (split_spread_ms): only links of similar delay share the split.
 * The receiver hands packets on in sequence, so a split stream goes at the
 * pace of its slowest link. In the lab (bond.yml with netem, split = force,
 * 3 runs) two equal links gave 1.98x one link for Cubic and BBR; 50 ms apart
 * (reorder50) Cubic got about 1.15x, BBR 1.47x and QUIC 1-4 Mbit/s, and 150 ms
 * apart (reorder150) all of them collapsed (BBR about 1 Mbit/s, Cubic 7-59),
 * with the reorderer's counters clean. While the pipe is nearly empty the
 * packets on the fast link are not held and the inner flow measures the fast
 * RTT; under load everything waits for the slow link: BBR sizes its window
 * on the minimum RTT and spirals down, Cubic's HyStart leaves slow start
 * early. Until a scheduler that sends by arrival time (step 3 of the plan),
 * the split stays among links within spread of the fastest:
 * - the delay is the send direction's, the one muting reads (health.h: what
 *   the receiver reports, filtered), behind the fastest candidate with a
 *   fresh one (candidates: the links that may take a share, G1 to G3);
 * - a candidate more than spread behind leaves the split at once; one that
 *   left comes back once it stays within 3/4 of spread for settle_ms (the
 *   mute settle time), so that a link near the edge does not flap. A link
 *   without a fresh delay is out until it has one, and then in at once if
 *   within spread: only one that left waits;
 * - a link out of the split carries a copy of every packet, as one of
 *   weight 0 does, to fill the others' gaps (split_spread_copy, the
 *   default, as up to 0.6.2). With split_spread_copy off it carries only
 *   the duplicated packets (handshakes and small datagrams), as one that is
 *   not eligible does, and one link left in the split takes every packet
 *   (G2): a link falls behind most often because its queue is full, and the
 *   whole stream on top of its share then makes it worse. In the first
 *   field test (story 012, 2026-10-13, 4 links of 5G with deep queues) a
 *   link that left kept falling behind, up to 2.6 s, and the sockets of the
 *   router refused 4000 to 53000 packets a run, where a split without the
 *   guard refused none; in the lab (bond "bloat", netem queues of 1 to 2 s)
 *   leaving the copies out helped BBR with spread 30 and hurt QUIC, so the
 *   old way stays the default until the field says otherwise;
 * - spread 0: no guard, every candidate takes its share, delay or not.
 * Evaluated with health, every tick; the send path reads the result. */
#define CG_SPLIT_REJOIN_NUM 3 /* a link that left comes back within 3/4 of the spread */
#define CG_SPLIT_REJOIN_DEN 4

struct cg_spread {
	uint16_t in;      /* candidates in the split at the last evaluation */
	uint16_t left;    /* links that left for their delay: they wait to come back */
	uint16_t holding; /* of those, within 3/4 of the spread since since_ms */
	uint16_t behind;  /* links whose behind_us the last evaluation measured */
	int32_t behind_us[CG_MAX_LINKS]; /* their delay behind the fastest candidate */
	uint64_t since_ms[CG_MAX_LINKS];
	uint8_t stress;   /* a link that was in left for its queue: copies help (cg_sdup) */
	uint8_t recover;  /* the fastest link is one that left: it comes back now (rejoin) */
	uint16_t qleft;   /* links that left after being in: their queue grew, not their base delay */
	uint64_t n_recover, n_qleave; /* how often each kind of stress began, for the status */
	uint64_t n_stall; /* links that left in a stall (cg_split_stalled): no copies */
	uint32_t owd_prev[CG_MAX_LINKS]; /* each link's delay at the last evaluation */
	uint16_t have_prev;
	uint64_t t_prev_ms;
};

#define CG_SPLIT_STALL_NUM 3 /* a delay growing at least 3/5 as fast as time: a stall */
#define CG_SPLIT_STALL_DEN 5

/* Whether a link whose delay went from prev to now in dt_ms stalled (its
 * service stopped: the packets just wait, and the delay grows as fast as
 * time, or jumps) rather than queued (it serves, slower than it is sent:
 * the delay grows at the excess's rate). The delay is health.h's, smoothed
 * by a quarter each report: a jump of 300 ms shows as 75 in the first
 * 100 ms, hence 3/5 and not 1. A stalled link's copies only pile
 * up behind the stall and delay what it carries once it comes back: in the
 * lab (ltespike, a link jumping by 300 ms) copies on it cost the download
 * 9 % against coming back at once alone. */
static inline int cg_split_stalled(uint32_t prev, uint32_t now, uint64_t dt_ms)
{
	int32_t g = cg_owd_diff(now, prev);

	return g > 0 && (uint64_t)g * CG_SPLIT_STALL_DEN >= dt_ms * 1000 * CG_SPLIT_STALL_NUM;
}

/* What a link has to do with the split, for the status. */
enum cg_split_share {
	CG_SHARE_NO = 0,  /* not eligible (G1 to G3): only the duplicated packets */
	CG_SHARE_YES,     /* takes its share */
	CG_SHARE_WEIGHT0, /* weight 0: a copy of every packet */
	CG_SHARE_SPREAD,  /* left for its delay (G4): a copy of every packet, or only the duplicated ones */
	CG_SHARE_WAITING, /* no delay measured yet (G4): the same */
};

static inline const char *cg_split_share_name(int v)
{
	static const char *const n[] = { "no", "yes", "weight 0", "spread", "waiting" };

	return v >= 0 && v <= CG_SHARE_WAITING ? n[v] : "?";
}

/* A link (re)starts: it has not left. */
static inline void cg_split_spread_reset(struct cg_spread *s, unsigned link)
{
	uint16_t keep = (uint16_t)~(1u << link);

	s->in &= keep;
	s->left &= keep;
	s->holding &= keep;
	s->behind &= keep;
}

/* One evaluation of guard G4, every tick. cand: the links that may take a
 * share otherwise (eligible, weight above 0); h: each link's send-direction
 * health (its filtered delay); spread_us: split_spread_ms, 0 off; settle_ms:
 * how long a link that left stays within 3/4 of the spread before it comes
 * back. Sets s->in and returns the links that left or came back.
 *
 * The delays are measured behind the fastest link still in, not the
 * fastest of all: a link that left and has since become the fastest is
 * waiting settle_ms to come back, and measured against it every link in
 * left too, the split was left with none and every packet went on every
 * link, as the network filled (2.6 % of the packets of a 25 Mbit/s stream
 * in a replay of the testnet's download, 0.5 % of its status samples; GPU
 * sweep of 2026-10-17). With no fresh link in, the fastest is taken in at
 * once: the split never empties while a link has a delay.
 *
 * What emptied it is kept as a signal, s->stress: the fastest link is one
 * that left and every link in is more than the spread behind it, a queue
 * drained while the others filled. Copying every packet then (what the
 * empty split did by accident) carried a lab link that jumped by 300 ms, or
 * deep queues, 7-23 % faster than splitting, at 1.5-2.4 times the bytes;
 * cg_sdup gives it a budget. */
static inline uint16_t cg_split_spread_eval_r(struct cg_spread *s, const struct cg_hlink *h, uint16_t cand,
					      uint32_t spread_us, uint32_t settle_ms, uint64_t now_ms, int rejoin)
{
	uint16_t fresh = 0, in = 0, was = s->left;
	int32_t back = (int32_t)((uint64_t)spread_us * CG_SPLIT_REJOIN_NUM / CG_SPLIT_REJOIN_DEN);
	int best;

	s->behind = 0;
	if (!spread_us) {
		s->left = s->holding = 0;
		s->in = cand;
		return was;
	}
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((cand >> i & 1) && cg_health_fresh(&h[i], now_ms))
			fresh |= (uint16_t)(1u << i);
	/* Recovery: the fastest link is one that left, and every link in is
	 * more than the spread behind it (its delay came back while theirs
	 * grew). With rejoin it comes back now instead of after settle_ms:
	 * waiting, the split sent everything to the links that were queueing
	 * (lab, ltespike: 40 Mbit/s and 300 TCP retransmissions, against 43
	 * and 60 coming back at once). */
	best = cg_health_fastest(h, CG_MAX_LINKS, fresh, now_ms);
	{
		uint8_t rec = best >= 0 && (s->left >> best & 1);

		for (int i = 0; rec && i < CG_MAX_LINKS; i++)
			if ((fresh & ~s->left) >> i & 1 && cg_owd_diff(h[i].owd, h[best].owd) <= (int32_t)spread_us)
				rec = 0;
		s->n_recover += rec && !s->recover;
		s->recover = rec;
		if (rec && rejoin) {
			s->left &= (uint16_t)~(1u << best);
			s->holding &= (uint16_t)~(1u << best);
			s->qleft &= (uint16_t)~(1u << best);
		}
	}
	best = cg_health_fastest(h, CG_MAX_LINKS, (uint16_t)(fresh & ~s->left), now_ms);
	if (best < 0 && fresh) {
		/* every fresh link left: the fastest comes back now */
		best = cg_health_fastest(h, CG_MAX_LINKS, fresh, now_ms);
		s->left &= (uint16_t)~(1u << best);
		s->holding &= (uint16_t)~(1u << best);
	}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		uint16_t b = (uint16_t)(1u << i);
		int32_t d;

		if (!(fresh & b)) {
			s->holding &= (uint16_t)~b; /* gone, or no delay: a wait starts over */
			s->qleft &= (uint16_t)~b;
			continue;
		}
		d = cg_owd_diff(h[i].owd, h[best].owd);
		s->behind_us[i] = d;
		s->behind |= b;
		if (!(s->left & b)) {
			if (d > (int32_t)spread_us) {
				s->left |= b;
				/* it was in: its queue grew (a link slow by its base delay
				 * never gets in, and copies to it would never stop); unless
				 * it stalled, which the rejoin answers */
				if ((s->in & b) && (s->have_prev & b) && now_ms > s->t_prev_ms &&
				    cg_split_stalled(s->owd_prev[i], h[i].owd, now_ms - s->t_prev_ms)) {
					s->n_stall++;
				} else if (s->in & b) {
					s->n_qleave += !s->qleft;
					s->qleft |= b;
				}
			} else {
				in |= b;
			}
		} else if (d > back) {
			s->holding &= (uint16_t)~b;
		} else {
			if (!(s->holding & b)) {
				s->holding |= b;
				s->since_ms[i] = now_ms;
			}
			if (now_ms - s->since_ms[i] >= settle_ms) {
				s->left &= (uint16_t)~b;
				s->holding &= (uint16_t)~b;
				s->qleft &= (uint16_t)~b;
				in |= b;
			}
		}
	}
	s->in = in;
	s->have_prev = fresh;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (fresh >> i & 1)
			s->owd_prev[i] = h[i].owd;
	s->t_prev_ms = now_ms;
	s->qleft &= s->left;
	s->stress = s->qleft != 0;
	return (uint16_t)(was ^ s->left);
}

/* Copies under stress (cg_split_spread_eval's s->stress), on a budget: each
 * split packet adds pct % of its bytes, each copied one spends its extra
 * copies' bytes, at most CG_SDUP_CAP saved. split_stress_dup (config.h) is
 * pct: 0 never copies, 100 lets the copies double the bytes while the
 * stress lasts. */
#define CG_SDUP_CAP 1048576 /* bytes of copies the budget holds at most */

struct cg_sdup {
	uint64_t tokens;      /* bytes of copies allowed now */
	uint64_t pkts, bytes; /* copied, and the extra bytes they cost */
};

/* Whether a split packet of len bytes is copied to its copies + 1 links. */
static inline int cg_sdup(struct cg_sdup *b, int stress, uint32_t len, unsigned links, uint32_t pct)
{
	uint64_t cost = (uint64_t)len * (links > 1 ? links - 1 : 0);

	if (!pct)
		return 0;
	b->tokens += (uint64_t)len * pct / 100;
	if (b->tokens > CG_SDUP_CAP)
		b->tokens = CG_SDUP_CAP;
	if (!stress || !cost || b->tokens < cost)
		return 0;
	b->tokens -= cost;
	b->pkts++;
	b->bytes += cost;
	return 1;
}

static inline uint16_t cg_split_spread_eval(struct cg_spread *s, const struct cg_hlink *h, uint16_t cand,
					    uint32_t spread_us, uint32_t settle_ms, uint64_t now_ms)
{
	return cg_split_spread_eval_r(s, h, cand, spread_us, settle_ms, now_ms, 0);
}

/* The eligible links the guard keeps out of the split (dup0, the weight-0
 * ones, aside), for cg_split_spread_apply. */
static inline uint16_t cg_split_spread_out(const struct cg_spread *s, uint16_t eligible, uint16_t dup0,
					   uint32_t spread_us)
{
	return spread_us ? (uint16_t)(eligible & ~dup0 & ~s->in) : 0;
}

/* The masks cg_split_pick and cg_split_dst take. */
struct cg_split_masks {
	uint16_t carriers; /* for G2: 0 when one link left must not take a packet alone */
	uint16_t eligible;
	uint16_t dup; /* the links that carry every packet */
};

/* What split_spread_copy (config.h) makes of the links G4 keeps out (out,
 * cg_split_spread_out) and of G2. copy (the default, as up to 0.6.2): they
 * carry a copy of every packet, as the weight-0 ones (dup0) do, and fewer
 * than 2 links in the split duplicate everything. Not copy: they leave the
 * eligible links and carry only the duplicated packets, and one link left
 * takes every split packet. carriers and eligible as for cg_split_pick. */
static inline struct cg_split_masks cg_split_spread_apply(int copy, uint16_t out, uint16_t carriers,
							  uint16_t eligible, uint16_t dup0)
{
	struct cg_split_masks m = { carriers, eligible, dup0 };

	if (copy) {
		m.carriers = 0;
		m.dup = (uint16_t)(dup0 | out);
	} else {
		m.eligible = (uint16_t)(eligible & ~out);
	}
	return m;
}

/* What link has to do with a split (the end splits): eligible and dup0 as
 * for cg_split_pick. */
static inline int cg_split_share(const struct cg_spread *s, unsigned link, uint16_t eligible, uint16_t dup0,
				 uint32_t spread_us)
{
	uint16_t b = (uint16_t)(1u << link);

	if (!(eligible & b))
		return CG_SHARE_NO;
	if (dup0 & b)
		return CG_SHARE_WEIGHT0;
	if (!spread_us || (s->in & b))
		return CG_SHARE_YES;
	return s->left & b ? CG_SHARE_SPREAD : CG_SHARE_WAITING;
}

/* Which bonding tail an end follows: the one of the newest control message
 * sent, by the peer's control sequence (one counter over every link: the
 * server's replies, the client's probes), not of the last one to arrive.
 * After a reload the messages sent before it on a slow link arrive after
 * newer ones on a fast link and would turn back what those said, until the
 * slow link's next message: an end would split towards a peer that no
 * longer reorders, and back, for as long as the slowest link's delay. */
struct cg_split_tail {
	uint32_t seq; /* of the message taken last */
	uint8_t seen; /* one was taken */
};

/* 1 when the verified control message of sequence seq is newer than the
 * last one taken, which it then becomes: its tail (or its lack of one) is
 * what the peer says now. 0 for an older one: its tail is left aside. The
 * replay window drops repeats; a server back from a restart starts its
 * replies 2^20 past the newest the client took, and a client back from one
 * is a new session. */
static inline int cg_split_tail_newer(struct cg_split_tail *t, uint32_t seq)
{
	if (t->seen && (int32_t)(seq - t->seq) <= 0)
		return 0;
	t->seq = seq;
	t->seen = 1;
	return 1;
}

/* The tail a client writes in its probes on a link (proto.h): weight, the
 * link's share of a split ([link] weight), its own reorder setting, and the
 * request for a split download when its split is on. */
static inline struct cg_probe_bond cg_split_probe_tail(int mode, int reorders, uint32_t weight)
{
	struct cg_probe_bond b = { 0 };

	b.weight = (uint16_t)(weight > CG_BOND_WEIGHT_MAX ? CG_BOND_WEIGHT_MAX : weight);
	b.reorders = !!reorders;
	b.split_me = mode == CG_SPLIT_ON;
	return b;
}

/* The tail a server writes in a reply: its reorder setting only. It never
 * asks for anything: the client's upload split follows the server's
 * reordering alone. */
static inline struct cg_probe_bond cg_split_reply_tail(int reorders)
{
	struct cg_probe_bond b = { 0 };

	b.reorders = !!reorders;
	return b;
}

/* Guard G3: whether a link whose peer was last heard from at last_rx_ms
 * (0: never) is too silent to take a share, now_ms later, with the peer
 * probing every probe_interval_ms. */
static inline int cg_split_stale(uint64_t last_rx_ms, uint64_t now_ms, uint32_t probe_interval_ms)
{
	return !last_rx_ms ||
	       (now_ms > last_rx_ms &&
		now_ms - last_rx_ms > (uint64_t)CG_SPLIT_STALE_PROBES * probe_interval_ms);
}

#endif
