/* cengarde client: one socket per uplink, every IP packet from the TUN
 * (tunnel = native) goes out on every carrying uplink, and the first copy of
 * each server datagram goes back to the TUN.
 *
 * Upload: a batch read from the TUN, MAC each packet once, then one
 * non-blocking sendmmsg per uplink, patching only the link byte. A full
 * uplink drops its own copies; it never stalls the others. Link health
 * (health.h) mutes uplinks that lag far behind the fastest one, from the
 * delays the server reports in its probe replies. With split (split.h) a
 * big data packet goes on one link instead, by smooth round-robin among the
 * live, active links whose server answers: always with force (the lab,
 * evenly), and with on by the [link] weights once the server's replies say
 * it reorders. The probes say whether this end reorders and, with on, ask
 * the server to split the download by those weights (proto.h's tail).
 * Download: duplicates are recognised by sequence number before the MAC is
 * checked, so only the first copy of each packet pays for verification. A
 * server that started over with a sequence behind the window is recognised
 * from its probe replies (epoch.h), and the window starts again. The first
 * copies go to the TUN in sequence (reorder.h, story 012): one that comes
 * in ahead of a missing one waits in a pool buffer until the missing one
 * comes, every link brought a later one, or its deadline, which the loop's
 * epoll timeout keeps; with nothing split and reorder_dup_wait_ms 0 nothing
 * waits. One output batch per pass keeps that order (down_deliver).
 * Server addresses: each link sends to one address of the server list, of a
 * family it has, from its best source address for it (addrpick.h), and
 * moves to the next one when the server stops answering there (srvpick.h).
 * The names in the list are looked up here, through the links (resolve.h,
 * names.c); when a name's addresses change, the links whose list changed
 * start again from its first address, as after a reload.
 *
 * Link sockets (link_threads): "legacy" reads them in this loop, as 0.4
 * did; "off" and "on" hand them to pumps (pump.h), which read them into
 * receive rings this loop then takes in turn ("off": one pump, run inline
 * here; "on": a thread per link). Every mode makes the same per-packet
 * decisions (clientpath.h); this loop keeps the session, the window, health
 * and probes, and still sends upload copies itself.
 *
 * A reload (SIGHUP or "cengarde ctl reload") applies the new configuration
 * in place, keeping the session; what was set up only once makes the process
 * start again (cg_config_restart_needed).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#ifndef SO_MEMINFO
#define SO_MEMINFO 55
#endif

#include "addrpick.h"
#include "arrival.h"
#include "clientpath.h"
#include "ctl.h"
#include "protoctl.h"
#include "protoctlio.h"
#include "decrec.h"
#include "engine.h"
#include "epoch.h"
#include "health.h"
#include "hist.h"
#include "linkmodel.h"
#include "log.h"
#include "names.h"
#include "netlink.h"
#include "pair.h"
#include "pump.h"
#include "replay.h"
#include "sock.h"
#include "capacity.h"
#include "dominant.h"
#include "linksig.h"
#include "tcpack.h"
#include "tcpfree.h"
#include "fec.h"
#include "fec3.h"
#include "gf256.h"
#include "repair.h"
#include "rebuilt.h"
#include "rlc.h"
#include "rs.h"
#include "harq.h"
#include "rescue.h"
#include "spare.h"
#include "pace.h"
#include "govern.h"
#include "split.h"
#include "srvpick.h"
#include "status.h"
#include "thrplan.h"
#include "active.h"
#include "ippkt.h"
#include "tun.h"
#include "util.h"

#define RECONCILE_MS 5000
/* The output batch to the TUN: a hub pass takes up to CG_BATCH slots of
 * each pump; a fuller batch (held packets released) is sent early. */
#define DOUT_MAX (CG_MAX_PUMPS * CG_BATCH)

/* split = force shares the packets out evenly (split.h): every link weighs
 * 1. The configured weights are for split = on. */
static const uint16_t split_equal[CG_MAX_LINKS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
_Static_assert(CG_MAX_LINKS == 16, "split_equal has a weight per link");

#define CG_PROBE_RAW 16 /* raw probe replies a link keeps for the status */

struct link {
	int used;
	char ifname[IFNAMSIZ];
	int fd;
	int seen; /* still wanted in the current reconcile pass */
	int why;  /* WHY_*: why it carries the tunnel or not, as of the last reconcile */
	/* Of the last socket; kept once it closes, to tell a new local address.
	 * pick is the source the interface's addresses put first toward
	 * remote (addrpick.h), which local is unless that one had no route
	 * through the interface (link_socket); a new pick is a new address. */
	struct sockaddr_storage local, remote, pick;
	/* The server address it uses (srvpick.h), kept by address; none: the
	 * first candidate, as after an outage of the link (down, without an
	 * address, or gone) or a new list. */
	struct sockaddr_storage cand;
	int cand_idx, ncand;    /* where cand is among the link's candidates, and how many there are */
	unsigned families;      /* 1 << AF_* of the interface's usable addresses, as of the last reconcile */
	uint32_t silent_moves;  /* failovers since the last reply: a dead round once every candidate had one */
	int back_to_first;      /* a reply ended a dead round: the first candidate at the next reconcile */
	/* A detour: the socket could not be opened toward want, the candidate
	 * the link was due on, or not from its pick (no route through the
	 * interface yet), and it took the next one that opened. Tried again
	 * every RECONCILE_MS: a socket that opens costs no packet. */
	int detour;
	struct sockaddr_storage want;
	uint64_t detour_retry_ms;
	uint64_t cand_since_ms; /* when its socket to cand opened */
	uint64_t failovers;
	uint32_t path_mtu; /* IP_MTU or IPV6_MTU of its socket, read every RECONCILE_MS */
	uint64_t up_since_ms, last_reply_ms, last_probe_ms, retry_ms;
	uint64_t hello_probe_ms; /* the probe sent at once with a HELLO's cookie */
	uint64_t srtt8_us; /* 8 x smoothed RTT */
	uint32_t rtt_us;
	uint32_t unanswered; /* probes sent since the last reply */
	uint64_t first_unanswered_ms;
	uint32_t probe_announced; /* interval_ms of the last probe */
	uint32_t probe_before;    /* and of the one before it */
	uint32_t down_owd;        /* trip of the newest reply, for the next probe */
	/* the last CG_PROBE_RAW probe replies, raw (unsmoothed) for the status and
	 * the recorder: when, the trip up (the server's clock minus ours) and down;
	 * a Starlink slot's delay ramp hides in the smoothed figures */
	uint64_t raw_ms[CG_PROBE_RAW];
	uint32_t raw_up[CG_PROBE_RAW], raw_down[CG_PROBE_RAW];
	uint8_t raw_at;
	int have_down_owd;
	int peer_muted; /* the server carries no download on this link */
	uint64_t tx_pkts, tx_bytes, tx_drops, tx_errors;
	uint64_t tx_fec_pkts, tx_fec_bytes; /* parities sent on it (fec.h), apart from tx_pkts and tx_bytes */
	uint64_t f3_rep, f3_bytes;          /* of them, FEC v3's repairs (fec3.h, story 026) */
	uint64_t tx_split; /* of tx_pkts, split packets (split.h): its share, or all of them with weight 0 */
	uint64_t paced;    /* the pacing rate set on its socket, bytes/s (pace.h); CG_PACE_NONE: none */
	struct cg_probe_info peer_view; /* server's view of this path (upload) */
	uint64_t peer_view_ms;
	struct cg_rxl rxl; /* as the download sees it: generation, last packet, probe echoes */
	uint8_t pump;      /* its pump (off and on), once it had a socket: sticky */
	uint8_t has_pump;
	uint64_t open_failed; /* sockets its pump could not poll */
	/* The newest command its pump has not taken yet because the pump is
	 * stalled (op 0: none); an OPEN here holds a socket the pump never saw. */
	struct cg_pump_cmd pend;
	struct cg_ratelimit rl, rl_move, rl_mtu;
};

#define CG_FEC_Q (CG_BATCH + 1) /* parities of one batch: at most one a packet, and the group before it */
#define CG_DHQ_N 256            /* download gaps waiting for the code (harq.h), as the server's CG_HQ_N */
/* a queued parity or repair's payload, at most */
#define CG_FECQ_PLEN (CG_FEC_HDR + CG_FEC_PLEN_MAX > CG_REP_HDR + CG_RS_BYTES_MAX ? CG_FEC_HDR + CG_FEC_PLEN_MAX \
										    : CG_REP_HDR + CG_RS_BYTES_MAX)

struct client {
	struct cg_config *cfg; /* replaced by a reload */
	const struct cg_run *run;
	uint32_t session;
	struct cg_txs txs; /* sequence and key up */
	struct cg_rxs rxs; /* window, arrivals and counters down (clientpath.h) */
	int ep, tun_fd, tfd; /* tun_fd: the TUN device */
	uint64_t ticks_missed, tick_max_us; /* timer expirations the tick ran late for, and its longest run */
	struct cg_nl nl;
	struct link link[CG_MAX_LINKS];
	struct cg_hlink uh[CG_MAX_LINKS]; /* upload health, by link id */
	struct cg_cap_link cap[CG_MAX_LINKS]; /* each link's upload capacity (capacity.h), by link id */
	struct cg_sig_link sig[CG_MAX_LINKS]; /* whose its queue is, what kind of loss (linksig.h): status only */
	uint16_t cw[CG_MAX_LINKS];            /* the weights from them, with capacity_weights */
	struct cg_cap_total ctot;             /* their total, for a shaper on the tunnel (upload) */
	struct cg_eta eta;                    /* the split by expected arrival, with split_eta */
	struct cg_tickle tk;                  /* with split_eta, an idle link tickled (tickle.h) */
	struct cg_eta pace;                   /* the copies' expected arrivals, with a link's pace_ms */
	struct cg_dom dom;                    /* the dominant link (dominant.h): while on, the split steps aside */
	struct cg_link_model lm[CG_MAX_LINKS]; /* each link's model (link_models) */
	uint32_t pace_w[CG_MAX_LINKS];        /* each link's copy window (cg_link_pace_windows), us, 0: every copy */
	uint8_t pace_any;                     /* a link with one */
	struct cg_hcfg hcfg;
	struct cg_status_writer sw;
	struct cg_ctl ctl;
	struct cg_ovr_table ovr; /* links paused or forced on by hand */
	struct cg_loader loader;
	struct cg_names names; /* the server names' lookups, across reloads */
	int reload_again;
	int server_pass; /* IP pass in the server's replies: -2 no reply yet, -1 none */
	char config_error[600]; /* why the last reload was refused */
	uint64_t start_ms, next_status_ms, next_reconcile_ms, next_path_ms, last_traffic_ms;

	uint64_t up_pkts, up_bytes, up_toobig;
	uint64_t up_split;   /* of up_pkts, packets split (split.h) instead of copied to every link */
	uint8_t prev_only;   /* 1 + the link the previous one went on alone, 0: several (proto.h) */
	struct cg_split sp;  /* the split's round-robin credits */
	uint16_t lw[CG_MAX_LINKS]; /* the [link] weights of the links in use (split = on), 1 without a section */
	uint16_t dup0;             /* of those, the weight-0 links: they carry every packet */
	uint8_t ltype[CG_MAX_LINKS]; /* the [link] types of the links in use (linkmodel.h), auto without a section */
	struct cg_spread spread;   /* guard G4: the links of similar delay that share the split (split.h) */
	struct cg_sdup sdup;       /* copies under G4's stress, on a budget */
	/* The upload's last decisions (decision.h), and the 0/1 states they
	 * were recorded at: G3 per link, G4's stress and recovery, the copies'
	 * budget, the rescue's. */
	struct cg_decring dec;
	uint8_t dec_stale[CG_MAX_LINKS];
	uint8_t dec_stress, dec_recover, dec_dry, dec_rfull;
	struct cg_gov gov;         /* the upload's weights from each link's queue (govern.h), with governor */
	uint16_t gw[CG_MAX_LINKS]; /* what it set at the last tick */
	int peer_bond;             /* the newest probe reply had a bonding tail (proto.h) */
	int peer_reorders;         /* and it said the server reorders: split = on splits */
	int peer_rescue;           /* and that it sends DATA again on a NACK (rescue.h) */
	int peer_tcpmark;          /* and that it marks its TCP ACKs CG_F_TUNHOLE (tcpack.h) */
	uint64_t nacks_sent;       /* NACK messages sent, for the rescue */
	uint64_t retx_one, retx_copied; /* tcp_aware in redundancy: TCP retransmissions sent on one link; copied (story 023) */
	/* tcp_ack_rule (tcpack.h): the upload's ACKs as they come down, and its
	 * retransmissions by what was known of their originals; of them, copied */
	struct cg_tcpa tcpa;
	uint64_t retx_row[CG_TCPA_ROWS], retx_row_copied[CG_TCPA_ROWS];
	/* The parity asked for again (harq.h, story 023): the RS block encoded
	 * again from the rescue ring (with rescue_kb and Leopard), the seeds of
	 * fresh combinations, and what was asked and sent. */
	struct cg_rs_scratch *parq_rs;
	uint16_t parq_seed;
	uint64_t ask_tcp; /* download gaps left to TCP: no answer could come before its retransmission */
	uint64_t parq_rx, parq_bad, parq_req, parq_sent, parq_bytes, parq_missing, parq_budget, parq_nolink;
	struct cg_tcpe tcpe; /* where TCP leaves the tunnel (the download): round trips, losses beyond (tcpfree.h) */
	struct cg_split_tail tail; /* which reply that was (split.h) */
	int8_t pick[CG_BATCH]; /* per packet of an upload batch: its one link (split), or -1 (every carrier) */
	uint16_t cmask[CG_BATCH]; /* with a link's pace_ms: the carriers a packet of every carrier goes on */
	uint32_t up_max;     /* largest packet from the TUN since the last path check */
	uint32_t up_largest; /* the same over the last check period, for the status */
	uint64_t down_pkts, down_bytes, down_tun_drops;
	uint64_t down_not_ip; /* DATA that was not an IP packet, never written to the TUN */
	struct cg_pctl *pctl;   /* the protocontroller (variant B) (protoctlio.h, story 025), NULL: off */
	uint64_t down_pctl_off; /* protocontroller stream messages that came with the protocontroller off */
	/* tcp_aware: inner TCP's retransmissions and segments without
	 * data go marked CG_F_FREE (tcpfree.h); the flow hash's seed */
	struct cg_tcpf tcpf;
	uint32_t flow_seed;
	struct cg_ratelimit rl_auth, rl_big, rl_tun, rl_full, rl_hello, rl_refused;

	/* Link sockets (pump.h): the mode in use, and the pumps of off and on. */
	int lt;                          /* CG_LT_LEGACY, CG_LT_OFF or CG_LT_ON */
	int ncpus;                       /* in the affinity mask, at start */
	cpu_set_t cpus;                  /* the process's, before the cpu knob pinned the hub */
	struct cg_bell bell;             /* rung by the pump threads (on) */
	struct cg_pump *pump[CG_MAX_PUMPS];
	uint8_t pump_links[CG_MAX_PUMPS]; /* links each pump serves */
	struct pump_stats {
		uint64_t pkts, paused, errors; /* totals of the pump's 32-bit counters */
		uint32_t l_pkts, l_paused, l_errors;
		uint32_t stalled_ms; /* how long work waited for it with no loop pass */
		struct cg_stall stall;
		uint32_t hop_snap[CG_HIST_N], hop_win[CG_HIST_N]; /* hop over the last 5 s */
		struct cg_cpuwin cpu;
		struct cg_ratelimit rl_stall, rl_err;
	} pst[CG_MAX_PUMPS];
	struct cg_cpuwin hub_cpu, sw_cpu;
	unsigned guards;                 /* thrplan.h warnings already given */
	uint64_t next_second_ms, next_hop_ms;
	int npumps;
	unsigned drain_first;            /* the pump taken first in the next pass */
	unsigned send_first;             /* rotates the order links send a batch in */
	struct cg_hist hop[CG_MAX_PUMPS]; /* download: from a pump's read to this loop's take, us */

	/* The output batch to the TUN, every mode: the first copies of a pass
	 * and the held packets it releases, in delivery order (DOUT_MAX entries,
	 * dq used), and the pool buffers dnbuf of them are in, given back only
	 * once they were written. */
	struct mmsghdr *dmsg;
	struct iovec *diov;
	int dq, dnbuf;
	uint16_t dbuf[DOUT_MAX];
	/* Reordering of the download (reorder.h; the state is rxs.ro): the
	 * pool of held packets, allocated once, its settings, and the releases
	 * left in this loop pass (CG_RO_PASS_MAX). */
	struct cg_ro_pool pool;
	/* Which links the server carries the download on lately (active.h,
	 * clockwork audit C2): its replies' masks and what came, the links rset
	 * was last narrowed from (act_base); and the links this end carries the
	 * upload on, told in its probes. */
	struct cg_act_rx act_rx;
	struct cg_act_tx act_tx;
	uint16_t act_base;
	/* The upload's rescue (rescue.h): what it sent lately, kept for the
	 * server's NACKs (rescue_kb), and what it may cost. */
	struct cg_resc_ring resc;
	uint32_t *resc_idx;
	struct cg_resc_budget resc_b;
	uint64_t nacks_rx, resc_sent, resc_miss, resc_budget, resc_congested;
	uint64_t resc_gone_tx, resc_gone_rx; /* CG_F_GONE said to the server; asked gaps it said were gone */
	uint64_t resc_grit, resc_grit_bytes; /* asked for, a GRIT message: its loss to its stream, not sent again (story 025) */
	/* The upload's parities (fec.h): the open group, and the parities of
	 * the groups closed in this batch, sent after its DATA. */
	struct cg_fec_tx fectx;
	struct {
		uint16_t mask; /* the links of its group */
		uint16_t plen;
		uint8_t type;  /* CG_T_FEC, or CG_T_REPAIR (redund_spare = rs | rlc) */
		uint8_t f3;    /* a repair of FEC v3 (fec_code = rlc) */
		uint8_t p[CG_HDR_LEN + CG_FECQ_PLEN];
	} fecq[CG_FEC_Q];
	int nfecq;
	uint64_t fec_parities, fec_bytes, fec_nolink;
	uint64_t fec_size[CG_FEC_KMAX + 1]; /* groups closed, by the packets in them */
	uint64_t fec_why[4];               /* closed full, evicted (span or no slot), at the deadline, split off */
	struct cg_fec_share fec_share;     /* each link's recent share, for fec_share_max */
	uint64_t fec_capped;               /* split packets a link at the cap did not take */
	uint64_t plan_n[CG_PLAN_N];        /* split packets by the step that set their links last (split.h's cg_split_plan) */
	uint32_t fec_ia_us;                /* mean gap between protected packets, for the deadline */
	unsigned fec_k;                    /* members of a full group now */
	uint64_t fec_last_us;
	/* redund_spare (spare.h, story 020): each link's allowance for what
	 * it carries besides the first link, the upload's data rate, the link
	 * where packets arrive first, and the parity group over the packets
	 * that went on it alone. Per packet of a batch: its sequence and
	 * whether it is a priority one (small, or tcp_aware's CG_F_FREE). */
	struct cg_spare_link spare[CG_MAX_LINKS];
	uint64_t sp_bytes, sp_data_bps, sp_tick_ms;
	uint16_t sp_carry; /* the carriers of the last batch */
	uint8_t sp_first1; /* 1 + the link where packets arrive first lately, 0: not known */
	uint64_t sp_cut_ms; /* the last tick the first link was in a microcut */
	uint64_t sp_choke_ms; /* since when the first link is choked, 0: it is not */
	uint8_t sp_full;      /* the others' allowance held the whole upload at the last tick */
	uint64_t sp_ban_ms[CG_MAX_LINKS]; /* when each was last banned from first for its queue */
	/* the last tick's view, for the status: each link's base and lag
	 * (spare.h), the links measured fresh, choked and banned, and why the
	 * pinned link is or is not the first (CG_SPARE_PIN_*) */
	uint32_t sp_base[CG_MAX_LINKS], sp_lag[CG_MAX_LINKS];
	uint16_t sp_known, sp_choked, sp_banned;
	int sp_pin_why;
	int sp_how;     /* CG_SPARE_COPIES or CG_SPARE_PARITY at the last tick */
	unsigned sp_k;  /* the parity's group size */
	struct cg_fec_grp sp_grp;
	uint64_t sp_parities, sp_par_skip, sp_par_nolink, sp_copies, sp_copy_skip, sp_fed;
	/* redund_spare = rs | rlc (story 021): the plan of the last tick
	 * (spare.h), the first link's average loss (permil x 16) and the time
	 * of the capacity sample it last took, the packets that went on the
	 * first link alone (a rate per tick), the open block or the window,
	 * and the repairs' counts */
	struct cg_spare_plan sp_plan;
	uint32_t sp_loss16;
	uint64_t sp_loss_ms;
	uint64_t sp_pkts, sp_pps;
	struct cg_rs_tx sp_rs;
	struct cg_rlc_tx sp_rlc;
	struct cg_gf sp_gf;
	uint64_t sp_rep_sent, sp_rep_bytes, sp_rep_skip, sp_rep_nolink, sp_rep_fail;
	/* FEC v3 (fec = yes, fec_code = rlc, story 026): every DATA of the
	 * window (rlc.h's wide sender, ~2 MB, allocated with the knob), the
	 * decisions (fec3.h), the links it repairs on now, the last repair's
	 * window and density, and the counts: repairs and their bytes, the
	 * DATA bytes it covered, tail repairs, candidates the guard took out,
	 * repairs without a link, windows with nothing in them, batches the
	 * lone link's gate kept off */
	struct cg_rlcw_tx *f3w;
	struct cg_f3 f3;
	uint16_t f3_links;
	unsigned f3_span;
	uint32_t f3_lag; /* the last repair's window ended this long before now, us (cg_f3_lag_us) */
	uint8_t f3_dn;
	uint64_t f3_rep, f3_bytes, f3_data, f3_tail, f3_guarded, f3_nolink, f3_empty, f3_gated, f3_quiet, f3_switches;
	uint16_t f3_ins_port; /* variant A's middle TCP with BBR: its port (f3_apply), 0: none */
	/* FEC v3 down (story 026): the server's repairs of the download,
	 * rebuilt as the server rebuilds the upload (with fec): the DATA got
	 * lately (fec.h's ring, 4 MB) and the RLC's equations (~600 KB); the
	 * gaps that wait for the code (harq.h, as the server's hq), the newest
	 * wide repair's top and window and how far apart they come, its seal;
	 * the counts */
	struct cg_fec_rx *dfec;
	struct cg_rlc_rx *drlc;
	struct {
		uint32_t seq, t_us;
		uint8_t rel; /* given up at the reorderer (TCP flows: not held), followed for a late PARQ */
	} dhq[CG_DHQ_N];
	uint32_t dhq_n;
	uint32_t df3_top, df3_span, df3_gap16, drep_top;
	uint8_t df3_have, drep_top_ok, drebuilding;
	uint64_t df3_last_ms, df3_span_ms;
	uint64_t drep_unused, drep_bad, d_rb_ok, d_rb_bad, d_rb_unsure, d_fec_dups;
	uint64_t dhq_waited, dhq_none, dhq_tcp, dhq_full, dhq_nonack, dparq_req, dparq_want, dparq_msgs;
	uint32_t sseq[CG_BATCH];
	uint32_t sts[CG_BATCH];   /* and its header's ts */
	uint8_t sflags[CG_BATCH]; /* and its flags, but CG_F_PREV (seal_batch) */
	uint8_t tcpk[CG_BATCH];   /* its class for tcp_aware (tcpfree.h's CG_TCPF_*) */
	uint8_t tcpr[CG_BATCH];   /* a retransmission's row (tcpack.h's CG_TCPA_*) */
	uint8_t prio[CG_BATCH];
	uint8_t pnc[CG_BATCH]; /* a protocontroller stream's DATA: no copies of a microcut (its ARQ repairs it, story 025) */
	uint8_t prc[CG_BATCH]; /* a protocontroller stream's DATA sent again a second time: a priority copy, on a link without a queue */
	struct cg_ro_cfg rocf;
	uint32_t ro_budget;

	struct cg_rxbatch in;
	uint8_t hdr[CG_BATCH][CG_HDR_LEN];
	struct mmsghdr out[CG_BATCH];
	struct iovec oiov[CG_BATCH][2];
};

/* Why an interface does or does not carry the tunnel. */
enum { WHY_OK, WHY_PAUSED, WHY_EXCLUDED, WHY_DOWN, WHY_NOADDR, WHY_NOFAMILY, WHY_UNRESOLVED, WHY_UNUSABLE, WHY_GONE };
static const char *const why_name[] = {
	"ok",
	"paused",
	"excluded",
	"down",
	"no address",
	"no address of the server's family",
	"server name not resolved",
	"unusable",
	"absent",
};

/* What a link may send to: its candidates (srvpick.h). */
struct cands {
	unsigned families; /* 1 << AF_* of the interface's usable addresses */
	int n;
	struct sockaddr_storage a[CG_MAX_CANDS];
};

static uint32_t srtt_ms(const struct link *l)
{
	return (uint32_t)(l->srtt8_us / 8000);
}

/* It has a socket its pump (or this loop) reads: not one waiting for a
 * stalled pump to take it. */
static int link_up(const struct link *l)
{
	return l->fd >= 0 && l->pend.op != CG_PUMP_OPEN;
}

/* Live: the server answers our probes on it, so it works both ways. */
static int link_live(const struct link *l, uint64_t now_ms)
{
	return link_up(l) && l->last_reply_ms &&
	       !cg_probes_stalled(l->unanswered, l->first_unanswered_ms, now_ms, srtt_ms(l));
}

static const char *link_state(const struct client *c, const struct link *l, uint64_t now_ms)
{
	if (l->fd < 0)
		return cg_ovr_get(&c->ovr, l->ifname) == CG_OVR_OFF ? "paused" : "down";
	if (!l->last_reply_ms)
		return "waiting";
	return link_live(l, now_ms) ? "live" : "stalled";
}

static void link_masks(const struct client *c, uint64_t now_ms, uint16_t *present, uint16_t *live)
{
	*present = *live = 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!link_up(&c->link[i]))
			continue;
		*present |= (uint16_t)(1u << i);
		if (link_live(&c->link[i], now_ms))
			*live |= (uint16_t)(1u << i);
	}
}

/* For what goes up (the data, its repairs, the rescue): a link whose
 * probes stalled but that still brings packets down is live (health.h's
 * cg_link_rx_alive: a microcut losing 60 % each way stalls the probes now
 * and then; leaving it moved everything to the cellular queue). */
static void link_masks_tx(const struct client *c, uint64_t now_ms, uint16_t *present, uint16_t *live)
{
	link_masks(c, now_ms, present, live);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];
		uint64_t rx = l->rxl.last_rx_ms;

		if (!(*present >> i & 1) || (*live >> i & 1) || !l->last_reply_ms || !rx || rx > now_ms)
			continue;
		if (cg_link_rx_alive((uint32_t)(now_ms - rx < UINT32_MAX ? now_ms - rx : UINT32_MAX),
				     now_ms - l->first_unanswered_ms, srtt_ms(l)))
			*live |= (uint16_t)(1u << i);
	}
}

/* Links the server should be sending each download packet on. */
static uint16_t expect_mask(const struct client *c)
{
	uint16_t m = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (link_up(&c->link[i]) && !c->link[i].peer_muted)
			m |= (uint16_t)(1u << i);
	return m;
}

/* Probe fast while there is traffic, slowly when the tunnel is idle. */
static uint32_t probe_interval(const struct client *c, uint64_t now_ms)
{
	return c->last_traffic_ms && now_ms - c->last_traffic_ms < c->cfg->probe_idle_ms ? c->cfg->probe_interval_ms
											: c->cfg->probe_idle_ms;
}

static int same_ip(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return 0;
	if (a->ss_family == AF_INET)
		return ((const struct sockaddr_in *)a)->sin_addr.s_addr == ((const struct sockaddr_in *)b)->sin_addr.s_addr;
	return !memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr, &((const struct sockaddr_in6 *)b)->sin6_addr, 16);
}

/* The link's socket, as the loop and the download see it. */
static void link_set_fd(struct link *l, int fd)
{
	l->fd = fd;
	l->rxl.open = fd >= 0;
}

/* Whether a pump holds the link sockets (link_threads off and on). */
static int pumped(const struct client *c)
{
	return c->lt != CG_LT_LEGACY;
}

/* Hands a command for link l to its pump. A stalled pump (CG_PUMP_CMDS
 * commands it has not taken) gets it later, from tick: only the newest
 * state of each link waits (cg_pump_send), so the hub never piles up
 * sockets however often failover and reconcile run, and a socket the pump
 * never saw is still the hub's to close when a newer state replaces it. */
static void link_cmd(struct client *c, struct link *l, uint8_t op, int fd)
{
	struct cg_pump_cmd cmd = { .op = op, .link = (uint8_t)(l - c->link), .gen = l->rxl.gen, .fd = fd };
	int unsent = cg_pump_send(c->pump[l->pump], &l->pend, &cmd);

	if (unsent >= 0)
		close(unsent);
}

/* tick: what stalled pumps can take now. */
static void link_cmds_retry(struct client *c)
{
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].pend.op)
			cg_pump_send_pending(c->pump[c->link[i].pump], &c->link[i].pend);
}

/* why: for the log, NULL for none. */
static void link_close(struct client *c, struct link *l, const char *why)
{
	if (l->fd < 0)
		return;
	if (pumped(c)) {
		/* Its pump closes it; what the pump read from it is stale from
		 * now on, so no reply of a closed link reaches its health. */
		l->rxl.gen++;
		link_cmd(c, l, CG_PUMP_CLOSE, -1);
	} else {
		epoll_ctl(c->ep, EPOLL_CTL_DEL, l->fd, NULL);
		close(l->fd);
	}
	link_set_fd(l, -1);
	if (why)
		cg_info("link %s down: %s", l->ifname, why);
}

static struct link *link_find(struct client *c, const char *ifname)
{
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].used && !strcmp(c->link[i].ifname, ifname))
			return &c->link[i];
	return NULL;
}

static struct link *link_get(struct client *c, const char *ifname)
{
	struct link *l = link_find(c, ifname);

	if (l)
		return l;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (c->link[i].used)
			continue;
		l = &c->link[i];
		memset(l, 0, sizeof(*l));
		l->used = 1;
		link_set_fd(l, -1);
		snprintf(l->ifname, sizeof(l->ifname), "%s", ifname);
		return l;
	}
	return NULL;
}

/* Whether the configuration wants an interface: a [link] section overrides
 * the interfaces and exclude patterns. */
static int configured(const struct cg_config *cfg, const char *ifname)
{
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifname);

	return lc ? lc->enabled
		  : cg_match_any(ifname, cfg->include, cfg->ninclude) && !cg_match_any(ifname, cfg->exclude, cfg->nexclude);
}

/* The server list a link sends to: its [link] one, or the global one. */
static struct cg_srvlist servers_of(const struct cg_config *cfg, const char *ifname)
{
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifname);

	if (lc && lc->nsrv)
		return (struct cg_srvlist){ lc->server, lc->nserver, lc->entry_n, lc->nentry };
	return (struct cg_srvlist){ cfg->server, cfg->nserver, cfg->entry_n, cfg->nentry };
}

/* The entries of that list, as written. */
static const struct cg_srvent *entries_of(const struct cg_config *cfg, const char *ifname, int *n)
{
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifname);

	*n = lc && lc->nsrv ? lc->nsrv : cfg->nsrv;
	return lc && lc->nsrv ? lc->srv : cfg->srv;
}

/* Whether a link's list has a name without any address yet. */
static int unresolved(const struct client *c, const char *ifname)
{
	int n;
	const struct cg_srvent *e = entries_of(c->cfg, ifname, &n);

	for (int i = 0; i < n; i++)
		if (e[i].a.ss_family == AF_UNSPEC && !cg_names_resolved(&c->names, e[i].name))
			return 1;
	return 0;
}

/* Whether ifc carries the tunnel and, when it does, which server addresses
 * it may send to. A manual override (ctl.h) goes before the configuration.
 * No side effects: reconcile applies the choice. */
static int eligible(struct client *c, const struct cg_iface *ifc, struct cands *e)
{
	const struct cg_config *cfg = c->cfg;
	enum cg_ovr o = cg_ovr_get(&c->ovr, ifc->name);
	struct cg_srvlist list;

	e->families = 0;
	e->n = 0;
	if (!cg_ovr_wanted(o, configured(cfg, ifc->name)))
		return o == CG_OVR_OFF ? WHY_PAUSED : WHY_EXCLUDED;
	if (!(ifc->flags & IFF_UP) || !(ifc->flags & IFF_RUNNING))
		return WHY_DOWN;
	e->families = cg_src_families(ifc->addr, ifc->naddr);
	if (!e->families)
		return WHY_NOADDR;
	list = servers_of(cfg, ifc->name);
	e->n = cg_cands_build(list.a, list.n, e->families, e->a);
	return e->n ? WHY_OK : unresolved(c, ifc->name) ? WHY_UNRESOLVED : WHY_NOFAMILY;
}

static uint32_t sock_path_mtu(int fd, int family)
{
	int v = 0;
	socklen_t len = sizeof(v);

	if (family == AF_INET6 ? getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &v, &len)
			       : getsockopt(fd, IPPROTO_IP, IP_MTU, &v, &len))
		return 0;
	return v > 0 ? (uint32_t)v : 0;
}

static struct cg_pump *pump_new(void)
{
	size_t size = (sizeof(struct cg_pump) + CG_CACHELINE - 1) / CG_CACHELINE * CG_CACHELINE;
	struct cg_pump *p = aligned_alloc(CG_CACHELINE, size);

	if (p)
		memset(p, 0, size);
	return p;
}

/* The guards of thrplan.h for the threads there are now, each warning once. */
static void guards_check(struct client *c)
{
	int pins[CG_MAX_PUMPS];
	unsigned g, fresh;

	for (int k = 0; k < c->npumps; k++)
		pins[k] = c->pump[k]->cpu;
	g = cg_thr_guards(1 + c->npumps, c->ncpus, c->cfg->rt_priority, c->cfg->busy_poll_us, pins, c->npumps);
	fresh = g & ~c->guards;
	c->guards |= g;
	if (fresh & CG_TG_RT_ALL)
		cg_warn("rt_priority on %d data threads with %d CPUs: the kernel's own work may starve", 1 + c->npumps,
			c->ncpus);
	if (fresh & CG_TG_BUSY_HUB)
		cg_warn("busy_poll_us: %d data threads do not fit %d CPUs with one to spare, the link threads do not poll",
			1 + c->npumps, c->ncpus);
	if (fresh & CG_TG_PIN_SHARED)
		cg_warn("two link threads pinned to the same CPU");
}

/* link_threads = on: a thread for link l, the first time it gets a socket,
 * or a place on the pump with the fewest links once CG_MAX_PUMPS run
 * (thrplan.h). Returns 0 or -1. */
static int link_pump(struct client *c, struct link *l)
{
	const struct cg_link_cfg *lc = cg_config_link(c->cfg, l->ifname);
	struct cg_pump *p;
	char name[8 + IFNAMSIZ]; /* the thread's name keeps 15 characters */
	uint32_t busy;
	int k;

	if (l->has_pump)
		return 0;
	k = c->lt == CG_LT_ON ? cg_pump_pick(c->pump_links, c->npumps, CG_MAX_PUMPS) : 0;
	if (k == c->npumps) {
		p = pump_new();
		if (!p || cg_pump_init(p, 1, -1, CG_PUMP_RXQ, c->cfg->io_queue) < 0) {
			free(p);
			return -1;
		}
		p->cpu = lc ? lc->cpu : -1;
		p->rt_priority = c->cfg->rt_priority;
		p->cpus = c->cpus;
		p->have_cpus = 1;
		/* Busy polling only while the data threads fit the CPUs with one
		 * to spare (thrplan.h). */
		busy = cg_pump_busy_us(c->npumps + 1, c->ncpus, c->cfg->busy_poll_us);
		atomic_store_explicit(&p->busy_poll_us, busy, memory_order_relaxed);
		snprintf(name, sizeof(name), "cg-%s", l->ifname);
		if (cg_pump_start(p, name, &c->bell) < 0) {
			cg_pump_free(p);
			free(p);
			return -1;
		}
		c->pump[c->npumps++] = p;
		/* Past that, the pumps that polled stop too: the hub alone polls. */
		if (!busy)
			for (int i = 0; i < c->npumps; i++)
				atomic_store_explicit(&c->pump[i]->busy_poll_us, 0, memory_order_relaxed);
		guards_check(c);
	}
	l->pump = (uint8_t)k;
	l->has_pump = 1;
	c->pump_links[k]++;
	return 0;
}

/* A socket's kernel receive queue and the datagrams it dropped
 * (SO_MEMINFO). Returns 0, or -1 when the kernel does not say. */
static int sock_meminfo(int fd, uint32_t *rmem, uint32_t *drops)
{
	uint32_t m[9] = { 0 }; /* SK_MEMINFO_RMEM_ALLOC is 0, SK_MEMINFO_DROPS 8 */
	socklen_t len = sizeof(m);

	if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_MEMINFO, m, &len) < 0 || len < sizeof(m))
		return -1;
	*rmem = m[0];
	*drops = m[8];
	return 0;
}

/* Whether to log a move: each one in the first round, then once a minute
 * while no address answers (an outage would fill the log). */
static int move_loud(struct link *l, int n, uint64_t now_ms)
{
	return l->silent_moves <= (uint32_t)n || cg_ratelimit_ok(&l->rl_move, now_ms, 60000);
}

/* A socket on ifc to dst from the best of ifc's addresses the kernel
 * routes there through ifc: its pick (addrpick.h), or, when connect finds
 * no route from it, the next by rank (cg_src_noroute). netifd puts a
 * prefix's source route on one interface, so when two uplinks get the same
 * prefix, an address of it routes out through the other one only, and
 * bound to its own interface the socket cannot connect from it; an
 * address of the uplink's own (its DHCPv6 one) can. *pick gets the pick,
 * *local the address bound. Returns the socket, or -1 with err (of the
 * pick's try). */
static int link_socket(const struct cg_iface *ifc, const struct sockaddr_storage *dst, struct sockaddr_storage *pick,
		       struct sockaddr_storage *local, char *err, size_t errlen)
{
	char more[256];
	uint32_t skip = 0;
	int fd = -1, k;

	for (int tries = 0; (k = cg_iface_pick_skip(ifc, dst, skip, local)) >= 0 && k < 32; tries++) {
		if (!tries)
			*pick = *local;
		fd = cg_udp_link(ifc->name, local, dst, tries ? more : err, tries ? sizeof(more) : errlen);
		if (fd >= 0 || !cg_src_noroute(errno))
			break;
		skip |= 1u << k;
	}
	return fd;
}

/* Points l at its server address among the candidates e of its interface
 * (srvpick.h) and opens its socket there, or keeps the one it has. When a
 * socket cannot be opened (an IPv6 address with no route, say: bound to
 * its interface, an IPv4 socket connects all the same), it tries the next
 * candidate at once, with failover on, and tries again later where it
 * could not open (a detour). */
static void link_open(struct client *c, struct link *l, const struct cg_iface *ifc, const struct cands *e,
		      uint64_t now_ms)
{
	const struct cg_config *cfg = c->cfg;
	const struct cg_link_cfg *lc = cg_config_link(cfg, ifc->name);
	const struct sockaddr_storage *cur = &l->cand;
	struct cg_srvlist list = servers_of(cfg, ifc->name);
	struct sockaddr_storage local, pick, bound;
	char err[256], a[64], b[64];
	int i, k, want, quiet = 0, skipped = 0, reopen = 0;

	/* Back to the first candidate when a family the link gained or lost
	 * changes its candidates, when its local address toward the server
	 * changed (a new lease or prefix), and when a reply ended a dead round.
	 * A new list goes there from apply_config. */
	if (cg_cands_changed(list.a, list.n, l->families, e->families)) {
		cur = NULL;
	} else if (l->back_to_first) {
		cur = NULL;
		cg_info("link %s: the server answers again after a round of its addresses without replies, back to %s",
			l->ifname, cg_addr_str(&e->a[0], a, sizeof(a)));
	} else if (l->pick.ss_family && cg_addr_equal(&l->remote, &l->cand) &&
		   cg_iface_pick(ifc, &l->cand, &local) == 0 &&
		   (!same_ip(&local, &l->pick) || (l->fd >= 0 && !cg_iface_has(ifc, &l->local)))) {
		cur = NULL;
	} else if (l->fd >= 0 && l->detour && now_ms >= l->detour_retry_ms &&
		   cg_cand_find(e->a, e->n, &l->want) >= 0) {
		/* Where the link was due: if a socket opens there from its pick
		 * now, the link moves there; else it stays on its detour. */
		int fd = link_socket(ifc, &l->want, &pick, &bound, err, sizeof(err));

		l->detour_retry_ms = now_ms + RECONCILE_MS;
		if (fd >= 0) {
			close(fd);
			/* Better: the address it was due on, or its pick there. */
			if (!cg_addr_equal(&l->want, &l->remote) || same_ip(&bound, &pick)) {
				char d[64];

				cg_info("link %s: a socket to %s opens from %s now, back there from %s", l->ifname,
					cg_addr_str(&l->want, a, sizeof(a)), cg_addr_str(&bound, b, sizeof(b)),
					cg_addr_str(&l->local, d, sizeof(d)));
				l->cand = l->want;
				reopen = 1;
			}
		}
	}
	l->families = e->families;
	l->back_to_first = 0;
	i = cur ? cg_cand_find(e->a, e->n, cur) : -1;
	k = cg_srv_pick(e->a, e->n, cur, now_ms, reopen ? now_ms : l->cand_since_ms, l->last_reply_ms,
			l->fd >= 0 ? cfg->server_failover_ms : 0, &l->silent_moves);
	want = k;
	if (i >= 0 && k != i) {
		uint64_t since = l->last_reply_ms > l->cand_since_ms ? l->last_reply_ms : l->cand_since_ms;

		l->failovers++;
		quiet = !move_loud(l, e->n, now_ms);
		if (!quiet)
			cg_info("link %s: no reply from %s for %u s, trying %s", l->ifname,
				cg_addr_str(&e->a[i], a, sizeof(a)), (unsigned)((now_ms - since) / 1000),
				cg_addr_str(&e->a[k], b, sizeof(b)));
	}
	for (int tries = 1;; tries++) {
		l->cand = e->a[k];
		l->cand_idx = k;
		l->ncand = e->n;
		if (cg_iface_pick(ifc, &l->cand, &local) < 0)
			return; /* never: the candidates are of the families ifc has */
		if (!reopen && l->fd >= 0 && same_ip(&l->pick, &local) && cg_addr_equal(&l->remote, &l->cand))
			return;
		if (l->fd >= 0 && (quiet || reopen))
			link_close(c, l, NULL);
		else if (l->fd >= 0)
			link_close(c, l, same_ip(&l->pick, &local) ? "server address changed" : "address changed");
		reopen = 0;
		if (now_ms < l->retry_ms)
			return;
		link_set_fd(l, link_socket(ifc, &l->cand, &pick, &bound, err, sizeof(err)));
		if (l->fd >= 0)
			break;
		if (e->n > 1 && cfg->server_failover_ms && tries < e->n) {
			int next = cg_srv_next(k, e->n, &l->silent_moves);

			skipped++;
			quiet = !move_loud(l, e->n, now_ms);
			if (!quiet)
				cg_info("link %s: cannot use %s (%s), trying %s", l->ifname, cg_addr_str(&e->a[k], a, sizeof(a)),
					err, cg_addr_str(&e->a[next], b, sizeof(b)));
			k = next;
			continue;
		}
		if (cg_ratelimit_ok(&l->rl, now_ms, 30000))
			cg_warn("link %s unusable: %s", ifc->name, err);
		l->retry_ms = now_ms + RECONCILE_MS;
		return;
	}
	/* Counted only now: a link that can open none of them is unusable, and
	 * its retries every RECONCILE_MS are no failovers. The silent moves
	 * count all the same, so the first reply still ends a dead round. */
	l->failovers += (uint64_t)skipped;
	cg_sock_buffers(l->fd, cfg->rcvbuf, cfg->sndbuf);
	{
		socklen_t len = sizeof(local);

		getsockname(l->fd, (struct sockaddr *)&local, &len);
	}
	l->local = local;
	l->pick = pick;
	l->remote = l->cand;
	l->want = e->a[want];
	l->detour = k != want || !same_ip(&local, &pick);
	l->detour_retry_ms = now_ms + RECONCILE_MS;
	if (!same_ip(&local, &pick) && !quiet)
		cg_info("link %s: no route from %s through it to %s, sending from %s", l->ifname,
			cg_addr_str(&pick, a, sizeof(a)), cg_addr_str(&l->cand, b, sizeof(b)),
			cg_addr_str(&local, err, sizeof(err)));
	l->up_since_ms = l->cand_since_ms = now_ms;
	l->path_mtu = sock_path_mtu(l->fd, l->remote.ss_family);
	/* The newest packet of any link stays in rxs.newest_ms (epoch.h). */
	l->rxl.last_rx_ms = l->last_reply_ms = l->last_probe_ms = 0;
	memset(&l->rxl.probes, 0, sizeof(l->rxl.probes));
	l->srtt8_us = 0;
	l->rtt_us = l->unanswered = l->probe_announced = l->probe_before = 0;
	l->have_down_owd = l->peer_muted = 0;
	cg_health_reset(&c->uh[l - c->link], now_ms);
	cg_split_spread_reset(&c->spread, (unsigned)(l - c->link));
	cg_gov_link_reset(&c->gov, (unsigned)(l - c->link));
	cg_cap_reset(&c->cap[l - c->link]);
	cg_sig_reset(&c->sig[l - c->link]);
	/* redund_spare: a new socket is measured afresh, its ban from first
	 * and its allowance with it (spare.h) */
	c->sp_ban_ms[l - c->link] = 0;
	cg_spare_restart(&c->spare[l - c->link]);
	l->paced = CG_PACE_NONE; /* a new socket is not paced */
	if (pumped(c)) {
		if (link_pump(c, l) < 0) {
			/* Never handed over: still this loop's to close. */
			close(l->fd);
			link_set_fd(l, -1);
			l->retry_ms = now_ms + RECONCILE_MS;
			if (cg_ratelimit_ok(&l->rl, now_ms, 30000))
				cg_warn("link %s: cannot start its thread, trying again", l->ifname);
			return;
		}
		/* A failure to poll it comes back through open_failed (tick). */
		l->rxl.gen++;
		link_cmd(c, l, CG_PUMP_OPEN, l->fd);
	} else if (cg_epoll_add(c->ep, l->fd, CG_EV(CG_EV_LINK, l - c->link)) < 0) {
		link_close(c, l, "epoll");
		return;
	}
	if (quiet)
		return;
	cg_info("link %s%s%s%s up: %s -> %s (id %d)", l->ifname, lc && lc->label[0] ? " (" : "",
		lc && lc->label[0] ? lc->label : "", lc && lc->label[0] ? ")" : "", cg_addr_str(&l->local, a, sizeof(a)),
		cg_addr_str(&l->remote, b, sizeof(b)), (int)(l - c->link));
}

/* The [link] weights of the links in use, for split = on (1 without a
 * section), and the weight-0 ones; and their types (auto without a
 * section). Returns 1 when the weights changed, which starts the
 * round-robin even again. */
static int weights_update(struct client *c)
{
	uint16_t w[CG_MAX_LINKS] = { 0 }, dup0 = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_link_cfg *lc;

		if (!c->link[i].used)
			continue;
		lc = cg_config_link(c->cfg, c->link[i].ifname);
		w[i] = (uint16_t)(lc ? lc->weight : 1);
		if (!w[i])
			dup0 |= (uint16_t)(1u << i);
	}
	c->dup0 = dup0;
	/* Their types too, which change no weight: the capacity and split
	 * loops read them each tick, the probes send them. */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_link_cfg *lc = c->link[i].used ? cg_config_link(c->cfg, c->link[i].ifname) : NULL;

		c->ltype[i] = lc ? lc->type : CG_LTYPE_AUTO;
	}
	if (!memcmp(w, c->lw, sizeof(w)))
		return 0;
	memcpy(c->lw, w, sizeof(w));
	cg_split_reset(&c->sp);
	return 1;
}

/* Brings sockets in line with the interfaces netlink reports. */
static void reconcile(struct client *c, uint64_t now_ms)
{
	struct cands e;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		c->link[i].seen = 0;
		c->link[i].why = WHY_GONE;
	}
	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		struct link *l;
		int why;

		if (!ifc->name[0])
			continue;
		why = eligible(c, ifc, &e);
		/* A slot all the same when paused, or when its [link] section
		 * wants it and it has no address of the server's family, or none
		 * yet (the server's name), so that the status shows why. */
		if (why == WHY_OK || why == WHY_PAUSED ||
		    ((why == WHY_NOFAMILY || why == WHY_UNRESOLVED) && cg_config_link(c->cfg, ifc->name)))
			l = link_get(c, ifc->name);
		else
			l = link_find(c, ifc->name);
		if (!l) {
			if (why == WHY_OK && cg_ratelimit_ok(&c->rl_full, now_ms, 60000))
				cg_warn("more than %d links, ignoring %s", CG_MAX_LINKS, ifc->name);
			continue;
		}
		l->why = why;
		if (why == WHY_PAUSED)
			link_close(c, l, "paused");
		if (why != WHY_OK)
			continue;
		l->seen = 1;
		link_open(c, l, ifc, &e, now_ms);
		if (l->fd < 0)
			l->why = WHY_UNUSABLE;
	}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		/* Down, gone, or without an address of the server's family: an
		 * outage of the link, which comes back through its first server
		 * address, with the same local address or a new one (srvpick.h).
		 * Not a pause or an exclusion. */
		if (l->used && !l->seen && l->why != WHY_PAUSED && l->why != WHY_EXCLUDED) {
			memset(&l->cand, 0, sizeof(l->cand));
			l->back_to_first = 0;
		}
		if (l->used && l->fd >= 0 && !l->seen)
			link_close(c, l,
				   l->why == WHY_GONE   ? "interface gone"
				   : l->why == WHY_DOWN ? "interface down"
							: why_name[l->why]);
	}
	weights_update(c);
	c->next_reconcile_ms = now_ms + RECONCILE_MS;
}

/* ---- server names ---- */

/* The server lists of cfg from what its names have now. */
static void lists_build(struct client *c, struct cg_config *cfg)
{
	struct cg_nameaddrs a[CG_MAX_NAMES];
	const struct cg_nameaddrs *p[CG_MAX_NAMES];

	cg_names_addrs(&c->names, a, p);
	cg_config_build(cfg, p);
}

/* A name's addresses changed: a link goes back to its first address only
 * when the address it uses is gone or an entry before it changed (the
 * preferred server moved); otherwise it stays where the server answers,
 * whatever changed after it (cg_lists_keep). */
static void names_changed(struct client *c, uint64_t now_ms)
{
	struct cg_config *cfg = c->cfg;
	struct cg_nameaddrs a[CG_MAX_NAMES];
	const struct cg_nameaddrs *p[CG_MAX_NAMES];

	cg_names_addrs(&c->names, a, p);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		struct sockaddr_storage na[CG_MAX_CANDS];
		uint8_t en[CG_MAX_SERVERS];
		struct cg_srvlist old, now;
		const struct cg_srvent *e;
		int ne;

		if (!l->used)
			continue;
		if (!l->cand.ss_family)
			continue; /* nowhere yet: its first candidate anyway */
		e = entries_of(cfg, l->ifname, &ne);
		now = (struct cg_srvlist){ na, cg_srv_build(e, ne, p, na, en), en, ne };
		old = servers_of(cfg, l->ifname);
		if (!cg_lists_keep(&old, &now, l->families, &l->cand))
			memset(&l->cand, 0, sizeof(l->cand));
	}
	cg_config_build(cfg, p);
	reconcile(c, now_ms);
}

/* The links the names may be looked up through, live ones first: those that
 * would carry the tunnel, whether or not the server's name has an address
 * of their family yet, each with its resolvers of the families it has. */
static int dns_paths(struct client *c, struct cg_names_path *paths, uint64_t now_ms)
{
	const struct cg_config *cfg = c->cfg;
	struct cg_names_path rest[CG_MAX_LINKS];
	int n = 0, nrest = 0;

	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		const struct cg_link_cfg *lc;
		const struct link *l;
		struct cg_names_path *p;
		struct cands e;
		int why;

		if (!ifc->name[0] || n + nrest == CG_MAX_LINKS)
			continue;
		why = eligible(c, ifc, &e);
		if (why != WHY_OK && why != WHY_NOFAMILY && why != WHY_UNRESOLVED)
			continue;
		l = link_find(c, ifc->name);
		p = l && link_live(l, now_ms) ? &paths[n] : &rest[nrest];
		p->ifc = ifc;
		p->n = 0;
		lc = cg_config_link(cfg, ifc->name);
		for (int k = 0; lc && k < lc->nresolver && p->n < CG_NAMES_RES; k++)
			if (e.families >> lc->resolver[k].ss_family & 1)
				p->res[p->n++] = &lc->resolver[k];
		for (int k = 0; k < cfg->nresolver && p->n < CG_NAMES_RES; k++)
			if (e.families >> cfg->resolver[k].ss_family & 1)
				p->res[p->n++] = &cfg->resolver[k];
		if (!p->n)
			continue;
		if (p == &paths[n])
			n++;
		else
			nrest++;
	}
	memcpy(&paths[n], rest, sizeof(rest[0]) * (size_t)nrest);
	return n + nrest;
}

/* Each tick: which names the links are live on, and the lookups. */
static void names_tick(struct client *c, uint64_t now_ms)
{
	struct cg_names_path paths[CG_MAX_LINKS];

	if (!c->cfg->nnames)
		return;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (link_up(&c->link[i]))
			cg_names_seen(&c->names, &c->link[i].remote, link_live(&c->link[i], now_ms), now_ms);
	if (cg_names_need(&c->names, now_ms))
		cg_names_tick(&c->names, c->ep, paths, dns_paths(c, paths, now_ms), now_ms);
}

/* Every RECONCILE_MS: the path MTU of each socket, and a warning when the
 * largest packet from the TUN since the last check does not fit it. */
static void path_check(struct client *c, uint64_t now_ms)
{
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		int f = l->remote.ss_family;
		uint32_t need = cg_outer_len(f, c->up_max), wrap = cg_outer_len(f, 0);

		if (l->fd < 0)
			continue;
		l->path_mtu = sock_path_mtu(l->fd, f);
		if (c->up_max && l->path_mtu && need > l->path_mtu && cg_ratelimit_ok(&l->rl_mtu, now_ms, 600000))
			cg_warn("link %s: packets of %u bytes from the TUN make %u-byte packets over IPv%c, more than its "
				"path MTU of %u: lower tun_mtu to %u",
				l->ifname, c->up_max, need, f == AF_INET6 ? '6' : '4', l->path_mtu,
				l->path_mtu > wrap ? l->path_mtu - wrap : 0);
	}
	c->up_largest = c->up_max;
	c->up_max = 0;
	c->next_path_ms = now_ms + RECONCILE_MS;
}

/* Whether the upload is split now (split.h): always with force, once the
 * server's replies say it reorders with on. */
static int split_active(const struct client *c)
{
	return cg_split_active(c->cfg->split, c->peer_reorders, 0);
}

/* The weights of the split and its weight-0 links: even with force, the
 * [link] weights with on; with governor, the governor's (govern.h), the
 * weight-0 links of on kept. */
static const uint16_t *split_weights(const struct client *c, uint16_t *dup0)
{
	*dup0 = c->cfg->split == CG_SPLIT_FORCE ? 0 : c->dup0;
	if (c->cfg->capacity_weights)
		return c->cw;
	if (c->cfg->governor)
		return c->gw;
	return c->cfg->split == CG_SPLIT_FORCE ? split_equal : c->lw;
}

/* The links that may take a share of a split upload (split.h): live and
 * carrying, not muted for delay (guard G1: never a muted, standby or
 * stalled link), and whose server answered within CG_SPLIT_STALE_PROBES of
 * their probe interval (guard G3: a dead link loses its share in about
 * 300 ms instead of the 1.3 s a stall takes). The interval is the longer of
 * the last two announced: when traffic starts, the last reply may still be
 * one of the idle interval's. One with a weight takes every split packet,
 * none makes the split duplicate everything (guard G2). None while the
 * upload is not split. */
static uint16_t split_eligible_g3(const struct client *c, uint16_t live, uint16_t carry, uint64_t now_ms,
				  uint16_t *stale)
{
	uint16_t e = 0, st = 0;

	if (split_active(c))
		for (int i = 0; i < CG_MAX_LINKS; i++) {
			const struct link *l = &c->link[i];
			uint32_t every = l->probe_announced > l->probe_before ? l->probe_announced : l->probe_before;

			if (!((live & carry) >> i & 1) || c->uh[i].state != CG_H_ACTIVE)
				continue;
			if (cg_split_stale(l->last_reply_ms, now_ms, every ? every : c->cfg->probe_interval_ms))
				st |= (uint16_t)(1u << i);
			else
				e |= (uint16_t)(1u << i);
		}
	if (stale)
		*stale = st;
	return e;
}

static uint16_t split_eligible(const struct client *c, uint16_t live, uint16_t carry, uint64_t now_ms)
{
	return split_eligible_g3(c, live, carry, now_ms, NULL);
}

/* The masks a batch is split with: guard G4 applied as split_spread_copy
 * says (split.h). */
static struct cg_split_masks split_masks(const struct client *c, uint16_t carry, uint16_t elig, uint16_t dup0)
{
	return cg_split_spread_apply(c->cfg->split_spread_copy,
				     cg_split_spread_out(&c->spread, elig, dup0, c->cfg->split_spread_ms * 1000), carry,
				     elig, dup0);
}

/* What the split does with the upload now, for the status: splitting
 * (2 links or more share it), one link (guard G2 with split_spread_copy
 * off: the only one in the split takes it) or duplicating. */
static const char *split_state(const struct client *c, uint64_t now_ms)
{
	uint16_t present, live, carry, dup0;
	const uint16_t *w = split_weights(c, &dup0);
	struct cg_split_masks k;
	int n = 0;

	if (c->cfg->split == CG_SPLIT_OFF)
		return "off";
	if (!split_active(c))
		return "waiting for the peer"; /* on: no reply yet says the server reorders */
	link_masks(c, now_ms, &present, &live);
	carry = cg_health_carriers(c->uh, CG_MAX_LINKS, present, live);
	k = split_masks(c, carry, split_eligible(c, live, carry, now_ms), dup0);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		n += (k.eligible >> i & 1) && w[i] && !(k.dup >> i & 1);
	return n >= 2 ? "splitting" : n && k.carriers ? "one link" : "duplicating";
}

/* The links a protocontroller stream's DATA may go on: the carriers, and
 * the links whose probes stalled that brought anything down lately
 * (health.h's cg_link_rx_alive_arq). */
static uint16_t pnc_links(const struct client *c, uint16_t carry, uint64_t now_ms)
{
	uint16_t m = carry;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];
		uint64_t rx = l->rxl.last_rx_ms;

		if ((m >> i & 1) || !link_up(l) || !l->last_reply_ms || !rx || rx > now_ms || !l->unanswered)
			continue;
		if (cg_link_rx_alive_arq((uint32_t)(now_ms - rx < UINT32_MAX ? now_ms - rx : UINT32_MAX),
					 now_ms - l->first_unanswered_ms))
			m |= (uint16_t)(1u << i);
	}
	return m;
}

/* Where a protocontroller stream's DATA goes with redund_spare (set: the
 * links it may go on, pnc_links): the one of least delay without a queue
 * (the minimum of its one-way delay, capacity.h), whatever its loss or
 * its queue (the stream's rate answers its own queue, its ARQ the loss:
 * following the first link of the copies, which leaves a link that loses
 * or looks choked, the stream put up to 1 MB in the cellular link's queue
 * at a microcut, story 025); by RTT before any minimum; -1: none. */
static int pnc_first(const struct client *c, uint16_t set)
{
	int b = -1, br = -1;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_gov_link *g = &c->cap[i].q;
		uint32_t mi, mb;

		if (!(set >> i & 1))
			continue;
		if (c->link[i].srtt8_us && (br < 0 || c->link[i].srtt8_us < c->link[br].srtt8_us))
			br = i;
		if (!g->have_min)
			continue;
		mi = cg_owd_diff(g->min_prev, g->min_cur) < 0 ? g->min_prev : g->min_cur;
		if (b >= 0) {
			const struct cg_gov_link *h = &c->cap[b].q;

			mb = cg_owd_diff(h->min_prev, h->min_cur) < 0 ? h->min_prev : h->min_cur;
			if (cg_owd_diff(mi, mb) >= 0)
				continue;
		}
		b = i;
	}
	return b >= 0 ? b : br;
}

/* starlink_edge_copies (slot.h): the Starlink links a copied packet goes
 * on alone now, 0: every carrier. */
static uint16_t edge_solo(const struct client *c, uint16_t carry, uint64_t now_ms)
{
	struct cg_slot s = cg_config_slot(c->cfg);
	uint64_t wall = cg_wall_ms();
	uint16_t sl = 0, fit = 0;

	if (wall < CG_WALL_SET_MS)
		return 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_hlink *h = &c->uh[i];
		const struct cg_cap_link *k = &c->cap[i];

		if (!c->lm[i].slot_clock)
			continue;
		sl |= (uint16_t)(1u << i);
		if (h->state == CG_H_ACTIVE && h->report_ms && now_ms - h->report_ms < CG_SLOT_FIT_MS &&
		    !(k->lost_ms && now_ms - k->lost_ms < CG_SLOT_LOST_MS))
			fit |= (uint16_t)(1u << i);
	}
	return cg_slot_solo(&s, wall, sl, fit, carry);
}

static void spare_masks(struct client *c, int m, uint16_t carry, uint64_t now_us, const int8_t *first);
static void spare_close(struct client *c, uint64_t now_us);
static int code_service(struct client *c, uint64_t now_us);
static uint64_t code_due(const struct client *c);
static int f3_tail(struct client *c, uint64_t now_us);
static int dwide(const struct client *c, uint64_t now_ms);

/* tcp_aware in redundancy (story 023): how many links a TCP retransmission
 * goes on. With tcp_ack_rule, from what is known of its original
 * (tcpack.h): ACKed already or lost beyond the tunnel, one link; not
 * delivered by the tunnel, copied on every carrier; nothing known, as
 * tcp_retx_one alone. With tcp_retx_one alone: the first retransmission of
 * some bytes on one link, a repeat copied (tcpfree.h's classes). One link
 * is the fastest of those it would go on. */
static void retx_masks(struct client *c, int m, uint16_t carry, uint16_t dup)
{
	for (int j = 0; j < m; j++) {
		uint16_t d;
		int best = -1, copy, row = c->tcpr[j], multi;

		if (c->tcpk[j] != CG_TCPF_RETX1 && c->tcpk[j] != CG_TCPF_RETXN)
			continue;
		d = (uint16_t)(cg_split_dst(c->pick[j], carry, dup) & c->cmask[j]);
		multi = (d & (d - 1)) != 0;
		if (!c->cfg->tcp_retx_one)
			copy = 1;
		else if (c->cfg->tcp_ack_rule)
			copy = cg_tcpa_copy(row, c->tcpk[j]);
		else
			copy = c->tcpk[j] == CG_TCPF_RETXN;
		c->retx_row[row]++;
		if (copy || !multi) {
			c->retx_copied += multi && copy;
			c->retx_row_copied[row] += multi && copy;
			continue;
		}
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if ((d >> i & 1) && (best < 0 || (c->link[i].srtt8_us &&
						      (!c->link[best].srtt8_us || c->link[i].srtt8_us < c->link[best].srtt8_us))))
				best = i;
		if (best < 0)
			continue;
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if ((d >> i & 1) && i != best && c->pick[j] < 0 && c->oiov[j][1].iov_len >= CG_PACE_SMALL && c->pace_any)
				cg_eta_unadd(&c->pace, (unsigned)i, (uint32_t)c->oiov[j][1].iov_len);
		c->cmask[j] = (uint16_t)(1u << best);
		c->retx_one++;
	}
}

/* The headers of the m packets, now that each one's links are decided: the
 * link the previous one went on alone (CG_F_PREV, harq.h's cg_harq_only)
 * in its flags, the MAC; then each is kept for the server's NACKs and
 * PARQs (rescue.h), with its one link and its share of the rescue budget. */
static void seal_batch(struct client *c, int m, uint16_t carry, uint16_t dup)
{
	for (int j = 0; j < m; j++) {
		size_t len = c->oiov[j][1].iov_len;
		const uint8_t *p = c->oiov[j][1].iov_base;
		int only = cg_harq_only(c->pick[j], carry, dup, c->cmask[j], c->cfg->mute_trickle);

		cg_up_seal(&c->txs, c->hdr[j], CG_T_DATA, (uint8_t)(c->sflags[j] | cg_prev_flags((int)c->prev_only - 1)), 0,
			   c->sseq[j], c->sts[j], p, len);
		c->prev_only = (uint8_t)(only + 1);
		if (c->resc_idx && CG_HDR_LEN + len <= CG_RESC_ENTRY) {
			uint8_t *e = cg_resc_put(&c->resc, c->resc_idx, c->txs.session, c->sseq[j], (uint16_t)(CG_HDR_LEN + len),
						 only >= 0 ? (uint8_t)only : CG_RESC_NOLINK);

			memcpy(e, c->hdr[j], CG_HDR_LEN);
			memcpy(e + CG_HDR_LEN, p, len);
			/* the budget grows with the link's weight (capacity.h) */
			cg_resc_earn(&c->resc_b,
				     only >= 0 ? (uint32_t)((uint64_t)len *
							    cg_cap_rescue_w(&c->cap[only], c->cfg->rescue_target_ms * 1000) /
							    CG_CAP_W_ONE)
					       : (uint32_t)len,
				     c->cfg->rescue_pct);
		}
	}
}

/* Sends the m packets prepared in hdr/oiov: each on the links pick[] says
 * (every carrying link, health.h, or its one link and the links of dup
 * when split: weight 0, and with split_spread_copy the ones G4 keeps out),
 * and one duplicated packet in mute_trickle on the other live links. live
 * and carry as when the packets were picked. */
static void send_links(struct client *c, int m, uint64_t now_ms, uint16_t live, uint16_t carry, uint16_t dup)
{
	int ids[CG_MAX_LINKS], n = 0, first = 0;
	uint64_t mark_us;
	uint16_t counts = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((carry >> i & 1) || ((live >> i & 1) && c->cfg->mute_trickle))
			ids[n++] = i;
	/* With pumps the first link of a batch rotates: the one that goes
	 * first wins the race to the server on identical links. */
	if (n && pumped(c))
		first = (int)(c->send_first++ % (unsigned)n);
	if (c->pace_any) {
		uint64_t now_us = cg_now_us();
		int8_t fl[CG_BATCH]; /* where each packet arrives first (eta.h), -1: not known */

		for (int j = 0; j < m; j++) {
			int f = -1;

			c->cmask[j] = c->pick[j] >= 0 || c->oiov[j][1].iov_len < CG_PACE_SMALL
					      ? 0xffff
					      : cg_eta_copies_first(&c->pace, carry, (uint32_t)c->oiov[j][1].iov_len, now_us,
								    c->pace_w, &f);
			/* redund_spare: the first link of the last tick (spare_tick) */
			fl[j] = (int8_t)(c->cfg->redund_spare ? c->sp_first1 - 1 : f);
		}
		if (c->cfg->starlink_edge_copies) {
			uint16_t cm = cg_slot_copy_mask(edge_solo(c, carry, now_ms), c->peer_reorders && c->resc_idx);

			for (int j = 0; cm != 0xffff && j < m; j++)
				if (c->pick[j] < 0 && c->oiov[j][1].iov_len >= CG_PACE_SMALL)
					c->cmask[j] &= cm;
		}
		if (c->cfg->redund_spare)
			spare_masks(c, m, carry, now_us, fl);
	} else {
		for (int j = 0; j < m; j++)
			c->cmask[j] = 0xffff;
	}
	retx_masks(c, m, carry, dup);
	seal_batch(c, m, carry, dup);
	/* The links the batch counts on for the server's reorderer (active.h):
	 * a packet's one link, or of its copies the one of least delay up. */
	{
		uint32_t owd[CG_MAX_LINKS];
		uint16_t known = 0;

		for (int i = 0; i < CG_MAX_LINKS; i++) {
			owd[i] = c->uh[i].owd;
			known |= (uint16_t)(c->uh[i].have_owd ? 1u << i : 0);
		}
		for (int j = 0; j < m; j++)
			counts |= cg_act_tx_counts((uint16_t)(cg_split_dst(c->pick[j], carry, dup) & c->cmask[j]), known, owd);
	}
	mark_us = cg_now_us(); /* after every ts of the batch */
	for (int x0 = 0; x0 < n; x0++) {
		int i = ids[(first + x0) % n];
		struct link *l = &c->link[i];
		int sel[CG_BATCH], k = 0, s;

		if (carry >> i & 1) {
			for (int j = 0; j < m; j++)
				if ((cg_split_dst(c->pick[j], carry, dup) >> i & 1) && (c->cmask[j] >> i & 1))
					sel[k++] = j;
		} else if ((live >> i & 1) && c->cfg->mute_trickle) {
			for (int j = 0; j < m; j++)
				if (c->pick[j] < 0 && cg_trickle(&c->uh[i], c->cfg->mute_trickle))
					sel[k++] = j;
		}
		if (!k)
			continue;
		if (counts >> i & 1)
			cg_act_tx_sent(&c->act_tx, (unsigned)i, mark_us);
		for (int x = 0; x < k; x++) {
			cg_hdr_set_link(c->hdr[sel[x]], (uint8_t)i);
			memset(&c->out[x].msg_hdr, 0, sizeof(c->out[x].msg_hdr));
			c->out[x].msg_hdr.msg_iov = c->oiov[sel[x]];
			c->out[x].msg_hdr.msg_iovlen = 2;
		}
		s = sendmmsg(l->fd, c->out, (unsigned)k, MSG_DONTWAIT);
		if (s < 0) {
			if (errno == EAGAIN || errno == ENOBUFS) {
				l->tx_drops += (uint64_t)k;
			} else {
				l->tx_errors += (uint64_t)k;
				if (cg_ratelimit_ok(&l->rl, now_ms, 10000))
					cg_warn("link %s send: %s", l->ifname, strerror(errno));
			}
			continue;
		}
		l->tx_drops += (uint64_t)(k - s); /* the rest would not fit: drop, never wait */
		l->tx_pkts += (uint64_t)s;
		for (int x = 0; x < s; x++) {
			l->tx_bytes += c->oiov[sel[x]][1].iov_len;
			l->tx_split += c->pick[sel[x]] >= 0;
		}
	}
}

/* The parities of the groups closed in this batch (fec.h), each on the
 * live link of least RTT outside its group (the parity goes after the
 * DATA, so that the DATA never waits for it). */
static void fec_send(struct client *c, uint64_t now_us)
{
	uint16_t present, live;

	link_masks_tx(c, now_us / 1000, &present, &live);
	for (int q = 0; q < c->nfecq; q++) {
		struct link *best = NULL;

		for (int i = 0; i < CG_MAX_LINKS; i++) {
			struct link *l = &c->link[i];

			if (!(live >> i & 1) || (c->fecq[q].mask >> i & 1) || l->fd < 0)
				continue;
			if (!best || (l->srtt8_us && (!best->srtt8_us || l->srtt8_us < best->srtt8_us)))
				best = l;
		}
		if (!best) {
			c->fec_nolink++;
			continue;
		}
		cg_up_header(&c->txs, c->fecq[q].p, c->fecq[q].type, 0, (uint8_t)(best - c->link), (uint32_t)now_us,
			     c->fecq[q].p + CG_HDR_LEN, c->fecq[q].plen);
		cg_act_tx_sent(&c->act_tx, (unsigned)(best - c->link), now_us); /* it may fill a gap (active.h) */
		if (send(best->fd, c->fecq[q].p, CG_HDR_LEN + c->fecq[q].plen, MSG_DONTWAIT) ==
		    (ssize_t)(CG_HDR_LEN + c->fecq[q].plen)) {
			if (c->fecq[q].f3) {
				c->f3_rep++;
				c->f3_bytes += c->fecq[q].plen;
				best->f3_rep++;
				best->f3_bytes += c->fecq[q].plen;
			} else if (c->fecq[q].type == CG_T_REPAIR) {
				c->sp_rep_sent++;
				c->sp_rep_bytes += c->fecq[q].plen;
			} else {
				c->fec_parities++;
				c->fec_bytes += c->fecq[q].plen;
			}
			/* apart from DATA: the server counts a path's DATA (its
			 * rx in the probes), and a parity counted as sent looked
			 * lost to the capacity (capacity.h's short and loss) */
			best->tx_fec_pkts++;
			best->tx_fec_bytes += c->fecq[q].plen;
		} else {
			best->tx_drops++;
		}
	}
	c->nfecq = 0;
}

/* Closes open group i (why: an index of fec_why): its parity waits for
 * the end of the batch. */
static void fec_close(struct client *c, int i, int why, uint64_t now_us)
{
	struct cg_fec_grp *g = &c->fectx.g[i];

	if (!g->n)
		return;
	c->fec_size[g->n > CG_FEC_KMAX ? CG_FEC_KMAX : g->n]++;
	c->fec_why[why]++;
	/* A group of one has no partner: its parity would be a copy, which
	 * the split chose not to send (when it sends most on one link, story
	 * 013, groups of one were most of them, as many bytes as
	 * duplicating); the rescue covers it. */
	if (g->n < 2) {
		g->n = 0;
		return;
	}
	if (c->nfecq == CG_FEC_Q)
		fec_send(c, now_us);
	c->fecq[c->nfecq].mask = g->mask;
	c->fecq[c->nfecq].type = CG_T_FEC;
	c->fecq[c->nfecq].f3 = 0;
	c->fecq[c->nfecq].plen = (uint16_t)cg_fec_grp_close(g, c->fecq[c->nfecq].p + CG_HDR_LEN);
	c->nfecq++;
}

/* DATA seq (len bytes at p) went on link alone (-1: on several, which
 * needs no parity): into the oldest open group without that link, which
 * closes once it holds one packet of each split link but one, the
 * parity's (fec.h). elig: the links that share the split. */
static void fec_feed(struct client *c, uint32_t seq, int link, const uint8_t *p, size_t len, uint64_t now_us,
		     uint16_t elig)
{
	unsigned n = (unsigned)__builtin_popcount(elig), k = n > CG_FEC_KMAX ? CG_FEC_KMAX : n - 1;
	int i;

	if (n < 2) {
		for (i = 0; i < CG_FEC_OPEN; i++)
			fec_close(c, i, 3, now_us);
		return;
	}
	c->fec_k = k;
	if (link < 0 || link > 15 || !(elig >> link & 1))
		return;
	cg_fec_share_note(&c->fec_share, link);
	cg_fec_gap_note(&c->fec_ia_us, &c->fec_last_us, now_us, c->cfg->fec_wait_ms * 1000);
	while ((i = cg_fec_tx_evict(&c->fectx, seq, link)) >= 0)
		fec_close(c, i, 1, now_us);
	i = cg_fec_tx_add(&c->fectx, seq, link, p, (uint16_t)len, now_us);
	if (c->fectx.g[i].n >= k)
		fec_close(c, i, 0, now_us);
}

/* A group's deadline now: from the packet rate, with k the members of a
 * full group (fec.h), at most fec_wait_ms. */
static uint32_t fec_deadline(const struct client *c)
{
	return cg_fec_deadline_us(c->fec_ia_us, c->fec_k, 1000, c->cfg->fec_wait_ms * 1000);
}

/* The groups open longer than their deadline close, and their parities go. */
static void fec_service(struct client *c)
{
	uint64_t now_us = cg_now_us();
	int i, any = 0;

	while ((i = cg_fec_tx_due(&c->fectx, now_us, fec_deadline(c))) >= 0) {
		fec_close(c, i, 2, now_us);
		any = 1;
	}
	if (c->sp_grp.n && now_us - c->sp_grp.t0_us >= (uint64_t)c->cfg->fec_wait_ms * 1000) {
		spare_close(c, now_us); /* redund_spare's parity group */
		any = 1;
	}
	any |= code_service(c, now_us); /* rs | rlc: the block's or the window's deadline */
	any |= f3_tail(c, now_us);      /* FEC v3: the repair at the end of a burst */
	if (any)
		fec_send(c, now_us);
}

/* The ms until the oldest open group's deadline, -1: none open. */
static int fec_timeout(const struct client *c)
{
	uint64_t now_us, due = cg_fec_tx_next_us(&c->fectx, fec_deadline(c));

	if (c->sp_grp.n && (!due || c->sp_grp.t0_us + (uint64_t)c->cfg->fec_wait_ms * 1000 < due))
		due = c->sp_grp.t0_us + (uint64_t)c->cfg->fec_wait_ms * 1000;
	{
		uint64_t cd = code_due(c);

		if (cd && (!due || cd < due))
			due = cd;
	}
	if (c->f3w && c->f3.mode != CG_F3_OFF && c->f3.credit >= 0) {
		/* in debt, nothing is due until a DATA pays it (and that DATA
		 * wakes the loop): a deadline in the past here spun the loop
		 * (the client at 25-30 % of a core with fec_pct 5) */
		uint64_t fd = cg_f3_due_at(&c->f3, (uint64_t)c->cfg->fec_wait_ms * 1000);

		if (fd && (!due || fd < due))
			due = fd;
	}
	if (!due)
		return -1;
	now_us = cg_now_us();
	return due <= now_us ? 0 : (int)((due - now_us + 999) / 1000);
}

/* redund_spare (spare.h): the parity group closes; its parity goes on the
 * link out of it with the most room in its bucket, if that has room for
 * it (a priority take), after the batch's DATA. */
static void spare_close(struct client *c, uint64_t now_us)
{
	struct cg_fec_grp *g = &c->sp_grp;
	unsigned plen;
	int best = -1;

	if (!g->n)
		return;
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((c->sp_carry >> i & 1) && !(g->mask >> i & 1) && c->link[i].fd >= 0 &&
		    (best < 0 || c->spare[i].tok > c->spare[best].tok))
			best = i;
	if (best < 0) {
		g->n = 0;
		c->sp_par_nolink++;
		return;
	}
	if (c->nfecq == CG_FEC_Q)
		fec_send(c, now_us);
	plen = cg_fec_grp_close(g, c->fecq[c->nfecq].p + CG_HDR_LEN);
	if (!cg_spare_take(&c->spare[best], now_us, (plen + CG_HDR_LEN) * 8, 1, 0)) {
		c->sp_par_skip++;
		return;
	}
	c->fecq[c->nfecq].mask = (uint16_t)~(1u << best); /* fec_send: the one link out of the mask */
	c->fecq[c->nfecq].type = CG_T_FEC;
	c->fecq[c->nfecq].f3 = 0;
	c->fecq[c->nfecq].plen = (uint16_t)plen;
	c->nfecq++;
	c->sp_parities++;
}

/* DATA seq (len bytes at p) went on link alone: into the parity group,
 * which closes at sp_k packets (or CG_FEC_SPAN sequences, or its
 * deadline, fec_service). */
static void spare_feed(struct client *c, uint32_t seq, int link, const uint8_t *p, uint32_t len, uint64_t now_us)
{
	struct cg_fec_grp *g = &c->sp_grp;

	if (g->n && (seq - g->base >= CG_FEC_SPAN || g->n >= CG_FEC_KMAX))
		spare_close(c, now_us);
	cg_fec_grp_add(g, seq, link, p, (uint16_t)(len > CG_FEC_PLEN_MAX ? CG_FEC_PLEN_MAX : len), now_us);
	c->sp_fed++;
	if (g->n >= c->sp_k)
		spare_close(c, now_us);
}

/* redund_spare = rs | rlc (story 021): a repair (header h, its symbol
 * of slen bytes at sym, or already in place in the queue when sym is
 * NULL) on the link other than the first (f) with the most room in its
 * bucket, if that has room for it (a priority take), after the batch's
 * DATA. The repairs of a block spread over the links by their buckets. */
static void code_send(struct client *c, struct cg_rep_hdr *h, const uint8_t *sym, unsigned slen, int f, uint64_t now_us)
{
	int best = -1;
	uint8_t *p;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((c->sp_carry >> i & 1) && i != f && c->link[i].fd >= 0 &&
		    (best < 0 || c->spare[i].tok > c->spare[best].tok))
			best = i;
	if (best < 0) {
		c->sp_rep_nolink++;
		return;
	}
	if (!cg_spare_take(&c->spare[best], now_us, (CG_HDR_LEN + CG_REP_HDR + slen) * 8, 1, 0)) {
		c->sp_rep_skip++;
		return;
	}
	p = c->fecq[c->nfecq].p + CG_HDR_LEN;
	cg_rep_put(p, h);
	if (sym)
		memcpy(p + CG_REP_HDR, sym, slen);
	c->fecq[c->nfecq].mask = (uint16_t)~(1u << best);
	c->fecq[c->nfecq].type = CG_T_REPAIR;
	c->fecq[c->nfecq].f3 = 0;
	c->fecq[c->nfecq].plen = (uint16_t)(CG_REP_HDR + slen);
	c->nfecq++;
}

/* rs: the open block closes, and its repairs go (code_send). */
static void code_rs_close(struct client *c, int f, uint64_t now_us)
{
	struct cg_rep_hdr h;
	uint8_t *sym[CG_RS_MMAX];
	unsigned bytes = 0, m;

	if (!c->sp_rs.k)
		return;
	m = cg_rs_tx_close(&c->sp_rs, &h, sym, &bytes);
	if (!m)
		c->sp_rep_fail++;
	for (unsigned j = 0; j < m; j++) {
		if (c->nfecq == CG_FEC_Q)
			fec_send(c, now_us);
		h.idx = (uint16_t)j;
		code_send(c, &h, sym[j], bytes, f, now_us);
	}
}

/* rlc: a repair over the window, built in the queue's next slot. */
static void code_rlc_repair(struct client *c, int f, uint64_t now_us)
{
	struct cg_rep_hdr h;
	unsigned slen;

	if (c->nfecq == CG_FEC_Q)
		fec_send(c, now_us);
	slen = cg_rlc_tx_repair(&c->sp_gf, &c->sp_rlc, c->sp_plan.k,
				&h, c->fecq[c->nfecq].p + CG_HDR_LEN + CG_REP_HDR);
	if (slen)
		code_send(c, &h, NULL, slen, f, now_us);
}

/* DATA seq (len bytes at p) went on the first link f alone: into the block
 * (closed at the plan's k, or when seq would not fit it) or the window (a
 * repair every k/m packets, the credit of rlc.h). */
static void code_feed(struct client *c, uint32_t seq, int f, const uint8_t *p, uint32_t len, uint64_t now_us)
{
	uint16_t l = (uint16_t)(len > CG_SYM_PLEN ? CG_SYM_PLEN : len);

	c->sp_fed++;
	if (c->sp_how == CG_SPARE_RS) {
		if (!cg_rs_tx_fits(&c->sp_rs, seq))
			code_rs_close(c, f, now_us);
		cg_rs_tx_add(&c->sp_rs, seq, p, l, c->sp_plan.m, now_us);
		if (c->sp_rs.k >= c->sp_plan.k)
			code_rs_close(c, f, now_us);
		return;
	}
	cg_rlc_tx_add(&c->sp_rlc, seq, p, l, now_us);
	for (unsigned n = cg_rlc_tx_owed(&c->sp_rlc, c->sp_plan.k ? 256 * c->sp_plan.m / c->sp_plan.k : 0); n; n--)
		code_rlc_repair(c, f, now_us);
}

/* When the block (rs) or the oldest packet without a repair (rlc) is due:
 * redund_spare_code_ms after its first packet; 0: nothing waits. */
static uint64_t code_due(const struct client *c)
{
	uint64_t w = (uint64_t)c->cfg->redund_spare_code_ms * 1000;

	if (c->sp_rs.k)
		return c->sp_rs.t0_us + w;
	if (c->sp_rlc.fresh && c->sp_rlc.n)
		return c->sp_rlc.fresh_us + w;
	return 0;
}

/* The deadline: the block closes short, or the window gets a repair;
 * returns whether a repair was queued. */
static int code_service(struct client *c, uint64_t now_us)
{
	uint64_t due = code_due(c);
	int f = c->sp_first1 - 1, n = c->nfecq;

	if (!due || now_us < due)
		return 0;
	if (c->sp_rs.k)
		code_rs_close(c, f, now_us);
	else
		code_rlc_repair(c, f, now_us);
	c->sp_rlc.fresh = 0; /* due once, even when no link took it */
	return c->nfecq != n;
}

/* redund_spare (spare.h): of the m packets of a batch that go to every
 * carrier, what the links after the first (first[], from eta.h; -1: not
 * known, every copy) carry: a small packet always (counted), the rest by
 * each link's allowance, as copies or, with parity, only the priority
 * ones; with parity, a packet left on the first link alone goes into the
 * parity group. The copies' masks (cmask) come in from eta.h's windows. */
static void spare_masks(struct client *c, int m, uint16_t carry, uint64_t now_us, const int8_t *first)
{
	uint16_t pset = 0;
	int pset_done = 0;

	c->sp_carry = carry;
	for (int j = 0; j < m; j++) {
		uint32_t len = (uint32_t)c->oiov[j][1].iov_len, bits = (len + CG_HDR_LEN) * 8;
		int f = first[j], small = len < CG_PACE_SMALL;
		uint16_t mask;

		if (c->pick[j] >= 0)
			continue;
		/* a protocontroller stream's DATA: on one link (pnc_first; its
		 * ARQ answers loss: copied everywhere while no first link was
		 * known, half a second of them filled the cellular queue to
		 * 300 KB at a microcut's start, story 025); in the copies of a
		 * microcut, that link only
		 * (copied, a stream that keeps its rate through the cut filled
		 * the cellular queue to 2 s); else, on the first, as any DATA
		 * (its repairs too) */
		if (c->pnc[j]) {
			int pf;

			if (!pset_done) {
				pset = pnc_links(c, carry, now_us / 1000);
				pset_done = 1;
			}
			pf = pnc_first(c, pset);

			if (pf >= 0 && (pf != f || c->sp_how == CG_SPARE_COPIES)) {
				c->cmask[j] = (uint16_t)(1u << pf);
				continue;
			}
		}
		if (f < 0 || !(carry >> f & 1))
			continue;
		mask = cg_spare_mask(c->cmask[j], carry, f);
		if (!small && !(c->cmask[j] >> f & 1))
			cg_eta_add(&c->pace, (unsigned)f, len, now_us); /* not counted by the window */
		for (int i = 0; i < CG_MAX_LINKS; i++) {
			int go;

			if (i == f || !(mask >> i & 1))
				continue;
			/* a protocontroller stream's repeat on a queued link would come after its
			 * ARQ sends it again anyway (a microcut: 60 % lost, a repeat
			 * for every other packet, the cellular queue at 1 s) */
			if (c->prc[j] && c->cap[i].ql_us > (int32_t)(c->cfg->redund_spare_ms * 1000)) {
				mask &= (uint16_t)~(1u << i);
				c->sp_copy_skip++;
				continue;
			}
			/* a stream's repeat only within the allowance (not a
			 * priority take: as one, the repeats of a microcut held
			 * the cellular queue at 1 s, story 025) */
			if (small)
				go = cg_spare_take(&c->spare[i], now_us, bits, 1, 1);
			else if (c->sp_how == CG_SPARE_COPIES)
				go = cg_spare_take(&c->spare[i], now_us, bits, c->prio[j] && !c->prc[j], 0);
			else
				go = c->prio[j] && cg_spare_take(&c->spare[i], now_us, bits, !c->prc[j], 0);
			if (go) {
				c->sp_copies++;
				continue;
			}
			mask &= (uint16_t)~(1u << i);
			c->sp_copy_skip++;
			if (!small)
				cg_eta_unadd(&c->pace, (unsigned)i, len);
		}
		c->cmask[j] = mask;
		if (!small)
			c->sp_pkts++;
		if (c->sp_how == CG_SPARE_PARITY && !small && mask == (uint16_t)(1u << f))
			spare_feed(c, c->sseq[j], f, c->oiov[j][1].iov_base, len, now_us);
		else if ((c->sp_how == CG_SPARE_RS || c->sp_how == CG_SPARE_RLC) && !small && mask == (uint16_t)(1u << f))
			code_feed(c, c->sseq[j], f, c->oiov[j][1].iov_base, len, now_us);
	}
}

/* redund_spare, each tick: the upload's data rate, each link's allowance
 * but the first's (spare.h, from its queue over the minute's minimum),
 * and from the largest of them and the first link's loss, how the next
 * packets are protected and the parity's group size. The first link from
 * links measured now (cg_spare_fresh) and not seconds behind by their
 * probes' round trip (cg_spare_lag). */
static void spare_tick(struct client *c, uint16_t live, uint64_t now_ms)
{
	uint32_t dt = c->sp_tick_ms ? (uint32_t)(now_ms - c->sp_tick_ms) : 0, *base = c->sp_base, *lag = c->sp_lag,
		 ref = 0, rref = 0, mins[CG_MAX_LINKS], choke = 4 * c->cfg->redund_spare_ms * 1000;
	int32_t q[CG_MAX_LINKS];
	uint16_t known = 0, banned = 0, choked = 0, set;
	uint64_t room = 0, bps;
	unsigned loss = 0;
	int f, f0, hold, p = -1;

	c->sp_tick_ms = now_ms;
	if (!dt)
		return;
	bps = c->sp_bytes * 8 * 1000 / dt;
	c->sp_bytes = 0;
	c->sp_data_bps = c->sp_data_bps ? c->sp_data_bps - c->sp_data_bps / 4 + bps / 4 : bps;
	/* the pinned link, by its name */
	for (int i = 0; c->cfg->redund_spare_first[0] && i < CG_MAX_LINKS && p < 0; i++)
		if (c->link[i].used && c->link[i].fd >= 0 && !strcmp(c->link[i].ifname, c->cfg->redund_spare_first))
			p = i;
	/* each link's delay without a queue, over the smallest (as cg_cap_eta),
	 * and its queue over the minute's minimum, of the links with a fresh
	 * report; and its lag from its probes' round trip (spare.h) */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_gov_link *g = &c->cap[i].q;

		q[i] = c->cap[i].ql_us;
		mins[i] = cg_owd_diff(g->min_prev, g->min_cur) < 0 ? g->min_prev : g->min_cur;
		if (c->sp_ban_ms[i] && now_ms - c->sp_ban_ms[i] < cg_spare_ban_ms(i == p))
			banned |= (uint16_t)(1u << i);
		if (c->link[i].fd < 0 || !(live >> i & 1) || !g->have_min || !cg_spare_fresh(c->uh[i].report_ms, now_ms))
			continue;
		if (!known || cg_owd_diff(mins[i], ref) < 0)
			ref = mins[i];
		if (!known || c->link[i].srtt8_us / 8 < rref)
			rref = (uint32_t)(c->link[i].srtt8_us / 8);
		known |= (uint16_t)(1u << i);
	}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		base[i] = known >> i & 1 ? (uint32_t)cg_owd_diff(mins[i], ref) : 0;
		/* the smoothed round trip, or the last one when longer: the first
		 * replies of a link seconds behind move the smoothed one by an
		 * eighth (the lab: its download at 4.5 s, first again for 2 s) */
		lag[i] = c->link[i].fd >= 0 && known
				 ? cg_spare_lag(c->link[i].rtt_us > c->link[i].srtt8_us / 8 ? c->link[i].rtt_us
											     : (uint32_t)(c->link[i].srtt8_us / 8),
						rref)
				 : 0;
		if (cg_spare_choked(q[i], base[i], lag[i], choke))
			choked |= (uint16_t)(1u << i);
	}
	/* The first link in a microcut (or CG_SPARE_HOLD_CUT_MS after one) stays
	 * the first, and one choked for less than CG_SPARE_CHOKE_MS while the
	 * others copy everything; one choked that long, or under a load they do
	 * not copy whole, is banned, lost or not (spare.h). */
	f0 = f = c->sp_first1 - 1;
	hold = 0;
	if (f >= 0) {
		int lost = c->cap[f].loss_pm_ms && now_ms - c->cap[f].loss_pm_ms < CG_SPARE_HOLD_CUT_MS &&
			   c->cap[f].loss_pm > CG_SPARE_MIX_PM;
		int keep = cg_spare_keep(lost, choked >> f & 1, c->sp_full, now_ms, &c->sp_cut_ms, &c->sp_choke_ms);

		if (keep == CG_SPARE_KEEP_BAN) {
			c->sp_ban_ms[f] = now_ms;
			banned |= (uint16_t)(1u << f);
		}
		hold = keep == CG_SPARE_KEEP_HOLD || keep == CG_SPARE_KEEP_WAIT;
	}
	/* the links carrying now (health.h), not those of the last batch: on a
	 * quiet tunnel that one could be from before a link came back */
	set = (uint16_t)(live & cg_health_carriers(c->uh, CG_MAX_LINKS, live, live));
	f = cg_spare_first(base, known, q, lag, CG_MAX_LINKS, set, banned, choke, f, hold);
	c->sp_pin_why = cg_spare_pin_why(c->cfg->redund_spare_first[0] && p < 0 ? 16 : p, set, known, banned,
					 p >= 0 && (choked >> p & 1));
	if (c->sp_pin_why == CG_SPARE_PIN_OK)
		f = p;
	if (f != f0)
		c->sp_choke_ms = 0;
	c->sp_first1 = (uint8_t)(f + 1);
	c->sp_known = known;
	c->sp_choked = choked;
	c->sp_banned = banned;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (c->link[i].fd < 0 || !(live >> i & 1) || i == f)
			continue;
		cg_spare_tick(&c->spare[i], now_ms, dt, c->cap[i].ql_us, c->cfg->redund_spare_ms * 1000, c->sp_data_bps,
			      cg_cap_bps(&c->cap[i]));
		if (c->spare[i].rate_bps > room)
			room = c->spare[i].rate_bps;
	}
	if (f >= 0 && c->cap[f].loss_pm_ms && now_ms - c->cap[f].loss_pm_ms < 2000)
		loss = c->cap[f].loss_pm;
	c->sp_full = room >= c->sp_data_bps;
	c->sp_how = cg_spare_how((int)c->cfg->redund_spare, c->sp_data_bps, room, loss);
	c->sp_k = cg_spare_k(c->sp_data_bps, room, CG_FEC_KMAX);
	c->sp_pps = c->sp_pps ? c->sp_pps - c->sp_pps / 4 + c->sp_pkts * 1000 / dt / 4 : c->sp_pkts * 1000 / dt;
	c->sp_pkts = 0;
	if (c->cfg->redund_spare == CG_SPARE_RS || c->cfg->redund_spare == CG_SPARE_RLC) {
		uint64_t all = 0;
		int code = c->cfg->redund_spare == CG_SPARE_RS && cg_rs_ready() ? CG_SPARE_RS : CG_SPARE_RLC;

		/* the first link's average loss, a sample at a time */
		if (f >= 0 && c->cap[f].loss_pm_ms && c->cap[f].loss_pm_ms != c->sp_loss_ms) {
			c->sp_loss_ms = c->cap[f].loss_pm_ms;
			c->sp_loss16 = c->sp_loss16 - c->sp_loss16 / 8 + c->cap[f].loss_pm * 2;
		}
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if (c->link[i].fd >= 0 && (live >> i & 1) && i != f)
				all += c->spare[i].rate_bps;
		c->sp_plan = cg_spare_plan(code, c->sp_data_bps, all, loss, c->sp_loss16 / 16, c->sp_pps,
					   c->cfg->redund_spare_code_ms, code == CG_SPARE_RS ? CG_RS_KMAX : CG_RLC_WMAX,
					   CG_RS_MMAX);
		c->sp_how = c->sp_plan.how;
		/* the block of a code no longer in use, or of another first link, closes */
		if (c->sp_rs.k && (c->sp_how != CG_SPARE_RS || f != f0))
			code_rs_close(c, f0, cg_now_us());
	}
}

_Static_assert(CG_MAX_LINKS == 16, "struct cg_tcpe_view holds 16 links");

/* The download's links as tcpfree.h judges an upload ACK's holes against
 * them: per link the newest sequence of any copy, and those that brought
 * one lately (arrival.h). */
static const struct cg_tcpe_view *tcpe_view(const struct client *c, uint32_t now_us, struct cg_tcpe_view *v)
{
	v->rset = v->hw_ok = 0;
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct cg_link_rx *l = &c->rxs.rx[i];

		v->hw[i] = l->top;
		if (l->last_us && now_us - l->last_us <= CG_TCPE_RECENT_US)
			v->hw_ok |= (uint16_t)(1u << i);
	}
	v->rset = v->hw_ok;
	return v;
}

/* Where an upload batch goes, decided once per batch (tun_read, up_send). */
struct upctx {
	uint64_t now_ms;
	uint16_t present, live, carry, dup0, esolo;
	int split;
	struct cg_split_masks k;
	const uint16_t *w;
	struct cg_tcpe_view tv;
	const struct cg_tcpe_view *tvp;
	int tv_done;
};

/* ---- FEC v3 (fec = yes, fec_code = rlc; fec3.h, rlc.h, story 026) ---- */

/* Whether FEC v3 runs: the knobs, and its window allocated. */
static int f3_on(const struct client *c)
{
	return c->f3w && c->cfg->fec && c->cfg->fec_code == CG_FEC_RLC;
}

/* The window (fec_window_ms), us. */
static uint64_t f3_window_us(const struct client *c)
{
	return (uint64_t)c->cfg->fec_window_ms * 1000;
}

/* For a batch: how it protects (fec3.h's cg_f3_mode_of) over the links the
 * DATA goes on: the split's (its eligible and weight-0 links, among the
 * carriers) or every carrier; a lone link only while its gate is open. */
static void f3_begin(struct client *c, const struct upctx *u)
{
	int split = u->split && !c->dom.out;
	uint16_t set = split ? (uint16_t)((u->k.eligible | u->k.dup) & u->carry) : u->carry;
	int mode;

	if (!f3_on(c))
		return;
	/* the mode by the links that carry, not the split's set of the moment:
	 * a link left out for a delay spike made it jump to single (its gate)
	 * and back, 109 times in 180 s (ltespikerand); the repairs still go
	 * only where the split sends (set) */
	mode = cg_f3_mode_of(split, (unsigned)__builtin_popcount(split ? u->carry : set));
	if (mode == CG_F3_SINGLE) {
		int i = __builtin_ctz(u->carry);

		if (!cg_f3_gate(&c->f3, (unsigned)i, c->cfg->fec_single, c->sig[i].kind == CG_SIG_RANDOM, c->cfg->fec_pct,
				u->now_ms)) {
			mode = CG_F3_OFF;
			c->f3_gated++;
		}
	}
	if (mode != CG_F3_OFF) {
		uint16_t rnd = 0;

		/* the share, by the loss of the links it protects (fec_pct_min) */
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if ((set >> i & 1) && c->sig[i].kind == CG_SIG_RANDOM)
				rnd |= (uint16_t)(1u << i);
		if (!cg_f3_pct(&c->f3, set, rnd, c->cfg->fec_pct, c->cfg->fec_pct_min, u->now_ms)) {
			mode = CG_F3_OFF;
			c->f3_quiet++;
		}
	}
	if (mode == CG_F3_OFF && c->f3.mode != CG_F3_OFF)
		cg_f3_idle(&c->f3);
	if (mode != c->f3.mode)
		c->f3_switches++; /* on, off or another mode: a ping-pong shows here */
	c->f3.mode = (uint8_t)mode;
	c->f3_links = mode == CG_F3_OFF ? 0 : set;
}

/* DATA seq (len bytes at p) into the window; its share of repairs, while
 * FEC v3 protects. */
static void f3_feed(struct client *c, uint32_t seq, const uint8_t *p, size_t len, uint64_t now_us)
{
	cg_rlcw_add(c->f3w, seq, p, (uint16_t)(len > CG_SYM_PLEN ? CG_SYM_PLEN : len), now_us);
	/* the protocontroller's bytes, which loss does not hold back: B's
	 * stream, and A's middle TCP to the VPS with BBR (fec3.h's cg_f3_mix) */
	cg_f3_mix(&c->f3, (uint32_t)len,
		  cg_pctl_is(p, len) || (c->f3_ins_port && cg_f3_tcp_port(p, len, c->f3_ins_port, 1)));
	/* the parity the server asks for again comes from this window too
	 * (on_parq): without the rescue ring, the rescue's budget grows here */
	if (!c->resc_idx)
		cg_resc_earn(&c->resc_b, (uint32_t)len, c->cfg->rescue_pct);
	if (c->f3.mode == CG_F3_OFF)
		return;
	cg_f3_earn(&c->f3, (uint32_t)len, c->f3.pct, now_us);
	c->f3_data += len;
}

/* One repair over the window, on the link the round-robin picks among
 * those it protects whose queue is within the window (cg_f3_guard), after
 * the batch's DATA (fecq); its bytes spent from the credit either way. */
static void f3_repair(struct client *c, uint64_t now_us)
{
	uint64_t win = f3_window_us(c);
	struct cg_rep_hdr h;
	uint16_t cand = 0;
	unsigned w, slen;
	uint32_t top = 0, lag;
	uint8_t *p;
	int link;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		if (!(c->f3_links >> i & 1) || c->link[i].fd < 0)
			continue;
		if (cg_f3_guard(c->cap[i].ql_us, (uint32_t)win,
				!cg_cap_rescue_w(&c->cap[i], c->cfg->rescue_target_ms * 1000))) {
			c->f3_guarded++;
			continue;
		}
		cand |= (uint16_t)(1u << i);
	}
	link = cg_f3_pick(&c->f3, cand);
	if (link < 0) {
		c->f3_nolink++;
		cg_f3_spend(&c->f3, c->f3.cost ? c->f3.cost : CG_F3_COST0);
		return;
	}
	{
		/* the window, or its last quarter (cg_f3_span_us); it ends where
		 * the DATA of slower links would still be on their way when it
		 * arrives (cg_f3_lag_us), at most half its span back */
		uint64_t span = cg_f3_span_us(&c->f3, win);
		int32_t behind[CG_MAX_LINKS];
		uint16_t have = 0;

		for (int i = 0; i < CG_MAX_LINKS; i++) {
			behind[i] = c->uh[i].behind_us;
			have |= (uint16_t)(c->uh[i].have_behind ? 1u << i : 0);
		}
		lag = cg_f3_lag_us(behind, have, c->f3_links, (unsigned)link, (uint32_t)(span / 2));
		w = cg_rlcw_window(c->f3w, now_us > span ? now_us - span : 0, now_us - lag, CG_RLCW_SPAN, &top);
	}
	if (c->nfecq == CG_FEC_Q)
		fec_send(c, now_us);
	p = c->fecq[c->nfecq].p + CG_HDR_LEN;
	slen = w ? cg_rlcw_repair(&c->sp_gf, c->f3w, top, w, &h, p + CG_REP_HDR) : 0;
	if (!slen) {
		/* nothing sent between the window's start and its end (traffic
		 * paused, or all of it newer than the lag): paid all the same, so
		 * the credit does not pile up; what waits keeps waiting for the
		 * tail repair */
		c->f3_empty++;
		cg_f3_pay(&c->f3, c->f3.cost ? c->f3.cost : CG_F3_COST0);
		return;
	}
	cg_rep_put(p, &h);
	c->fecq[c->nfecq].mask = (uint16_t)~(1u << link); /* fec_send: the one link out of the mask */
	c->fecq[c->nfecq].type = CG_T_REPAIR;
	c->fecq[c->nfecq].f3 = 1;
	c->fecq[c->nfecq].plen = (uint16_t)(CG_REP_HDR + slen);
	c->nfecq++;
	cg_f3_spend(&c->f3, cg_f3_cost(slen));
	/* the split's expected arrivals count its bytes on that link */
	if (c->cfg->split_eta)
		cg_eta_add(&c->eta, (unsigned)link, cg_f3_cost(slen), now_us);
	c->f3_span = w;
	c->f3_dn = h.dn;
	c->f3_lag = lag;
}

/* After a batch: the repairs its DATA earned. */
static void f3_send(struct client *c, uint64_t now_us)
{
	if (!f3_on(c) || c->f3.mode == CG_F3_OFF)
		return;
	for (int n = 0; n < CG_F3_BURST && cg_f3_owed(&c->f3); n++)
		f3_repair(c, now_us);
}

/* The repair at the end of a burst (cg_f3_due); whether one was queued. */
static int f3_tail(struct client *c, uint64_t now_us)
{
	int n = c->nfecq;

	if (!f3_on(c) || c->f3.mode == CG_F3_OFF || !cg_f3_due(&c->f3, now_us, (uint64_t)c->cfg->fec_wait_ms * 1000))
		return 0;
	f3_repair(c, now_us);
	c->f3_tail += c->nfecq != n;
	return c->nfecq != n;
}

/* The window allocated or freed with the knobs (start and each reload),
 * never per packet. */
static void f3_apply(struct client *c)
{
	/* variant A's middle TCP, when its congestion control does not slow
	 * down for loss (BBR): its port, for cg_f3_mix */
	c->f3_ins_port = c->cfg->protocontroller == CG_PCTL_A && !strncmp(c->cfg->protocontroller_mid_cc, "bbr", 3)
				 ? (uint16_t)c->cfg->protocontroller_port : 0;
	if (c->cfg->fec && c->cfg->fec_code == CG_FEC_RLC) {
		if (!c->f3w && !(c->f3w = calloc(1, sizeof(*c->f3w))))
			cg_warn("fec_code = rlc: out of memory for its window, no repairs");
	} else if (c->f3w) {
		free(c->f3w);
		c->f3w = NULL;
		cg_f3_idle(&c->f3);
		c->f3.mode = CG_F3_OFF;
	}
}

static void up_begin(struct client *c, uint64_t now_ms, struct upctx *u)
{
	u->now_ms = now_ms;
	/* the split is decided per packet before its header (CG_F_SPLIT is
	 * under the MAC) */
	link_masks_tx(c, now_ms, &u->present, &u->live);
	u->carry = cg_health_carriers(c->uh, CG_MAX_LINKS, u->present, u->live);
	u->split = split_active(c);
	u->w = split_weights(c, &u->dup0);
	u->k = split_masks(c, u->carry, split_eligible(c, u->live, u->carry, now_ms), u->dup0);
	/* starlink_edge_copies with a server that reorders: outside an
	 * edge's window on Starlink alone, as split DATA (cg_slot_pick) */
	u->esolo = c->cfg->starlink_edge_copies && c->peer_reorders && c->resc_idx ? edge_solo(c, u->carry, now_ms) : 0;
	u->tvp = NULL;
	u->tv_done = 0;
}

/* Packet m of the batch: buf, len bytes (an IP packet from the TUN, or a
 * protocontroller stream's message, story 025: copy, a
 * priority copy where the links have room; split, on every carrier). */
static void up_one(struct client *c, struct upctx *u, int m, uint8_t *buf, size_t len, uint64_t now_us, int copy)
{
	uint16_t elig;
	uint8_t flags = 0;
	int stress, split = u->split;
	struct cg_split_plan plan;

	if (len > c->up_max)
		c->up_max = (uint32_t)len; /* for the path MTU check */
	/* With fec, a link over fec_share_max of the recent split DATA
	 * sits this packet out (fec.h). */
	/* The links it may take, in one order (split.h's cg_split_plan, audit
	 * C13): G3/G4 (the batch's), the lead alone under stress, the XOR's cap
	 * per link (only with the XOR), a copy on every carrier under stress */
	elig = cg_split_stress_elig(&c->spread, u->k.eligible, c->cfg->split_stress_single);
	{
		uint16_t share = c->cfg->fec && c->cfg->fec_code == CG_FEC_XOR && split
					 ? cg_fec_share_mask(&c->fec_share, elig, c->cfg->fec_share_max)
					 : elig;

		c->fec_capped += share != elig;
		/* Under G4's stress, on its budget: a copy on every carrier. */
		stress = split && cg_sdup(&c->sdup, c->spread.stress, (uint32_t)len,
					  (unsigned)__builtin_popcount(u->carry), c->cfg->split_stress_dup);
		plan = cg_split_plan(u->k.eligible, elig, share, stress);
		elig = plan.pick;
		if (split)
			c->plan_n[plan.by]++;
	}
	/* A dominant link (dominant.h): the split steps aside, every
	 * packet goes as in redundancy (the copies paced, eta.h). */
	c->pick[m] = (int8_t)(copy ? -1
			      : split && !c->dom.out
				      ? cg_split_pick_eta(&c->sp, c->cfg->split_eta ? &c->eta : NULL,
							  c->cfg->split_eta ? &c->tk : NULL, now_us, u->k.carriers,
							  elig, u->k.dup, u->w, (uint32_t)len, c->cfg->split_small, &flags)
				      : cg_slot_pick(u->esolo, (uint32_t)len, CG_PACE_SMALL, &flags));
	c->up_split += c->pick[m] >= 0;
	c->tcpk[m] = 0;
	c->tcpr[m] = CG_TCPA_UNKNOWN;
	if (c->cfg->tcp_aware) {
		struct cg_flow f;

		if (cg_flow_of(buf, len, c->flow_seed, &f)) {
			c->tcpk[m] = (uint8_t)cg_tcpf_class(&c->tcpf, &f, buf, len);
			if (c->tcpk[m])
				flags |= CG_F_FREE;
			/* what the ACKs said of a retransmission's original (tcpack.h) */
			if ((c->tcpk[m] == CG_TCPF_RETX1 || c->tcpk[m] == CG_TCPF_RETXN) && c->cfg->tcp_ack_rule)
				c->tcpr[m] = (uint8_t)cg_tcpa_retx(&c->tcpa, &f, buf, len);
		}
		/* a flow's ACKs (and SYN-ACK) into the tunnel (tcpfree.h),
		 * marked when the download did not deliver their holes */
		if (c->tcpk[m] == CG_TCPF_CTL && f.tcp && cg_flow_of_dir(buf, len, c->flow_seed, 1, &f)) {
			if (!u->tv_done) { /* once a batch */
				u->tvp = tcpe_view(c, (uint32_t)now_us, &u->tv);
				u->tv_done = 1;
			}
			if (cg_tcpe_in(&c->tcpe, &f, buf, len, (uint32_t)now_us, u->tvp) >= CG_TCPE_OWN)
				flags |= CG_F_TUNHOLE;
		}
	}
	/* The header is sealed in send_links, once the copies are
	 * decided: the link this one went on alone goes in the next
	 * one's flags (CG_F_PREV, harq.h), under the MAC. */
	c->sflags[m] = flags;
	c->sts[m] = (uint32_t)now_us;
	c->sseq[m] = cg_up_next(&c->txs);
	/* a protocontroller stream's message sent again a second time: a priority copy,
	 * on what the links have to spare (forced on every carrier, the copies
	 * of a microcut filled the cellular queue to 2 s) */
	c->prio[m] = (uint8_t)(len < CG_PACE_SMALL || (flags & CG_F_FREE) || copy);
	c->pnc[m] = (uint8_t)(!copy && len >= CG_PACE_SMALL && cg_pctl_is(buf, len));
	c->prc[m] = (uint8_t)copy;
	c->sp_bytes += len;
	if (f3_on(c)) {
		f3_feed(c, c->sseq[m], buf, len, now_us);
	} else if (c->cfg->fec && split) {
		int only = cg_split_only(c->pick[m], u->carry, u->k.dup);

		/* grouped by the links it may really take (cg_split_plan) */
		fec_feed(c, c->sseq[m], c->pick[m] < 0 && c->cfg->mute_trickle ? -1 : only, buf, len, now_us,
			 plan.xor_by);
	}
	c->oiov[m][0].iov_base = c->hdr[m];
	c->oiov[m][0].iov_len = CG_HDR_LEN;
	c->oiov[m][1].iov_base = buf;
	c->oiov[m][1].iov_len = len;
	c->up_pkts++;
	c->up_bytes += len;
}

static void tun_read(struct client *c)
{
	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_tun_rx(c->tun_fd, &c->in), m = 0;
		uint64_t now_us, now_ms;
		struct upctx u;

		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		up_begin(c, now_ms, &u);
		f3_begin(c, &u);
		for (int i = 0; i < n; i++) {
			size_t len = c->in.msg[i].msg_len;

			if (!len)
				continue;
			if ((c->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC) || len > CG_MAX_PAYLOAD) {
				c->up_toobig++;
				if (cg_ratelimit_ok(&c->rl_big, now_ms, 30000))
					cg_warn("packet larger than %d bytes dropped: lower tun_mtu", CG_MAX_PAYLOAD);
				continue;
			}
			up_one(c, &u, m++, c->in.buf[i], len, now_us, 0);
		}
		if (m) {
			c->last_traffic_ms = now_ms;
			send_links(c, m, now_ms, u.live, u.carry, u.k.dup);
			f3_send(c, now_us);
		}
		if (c->nfecq)
			fec_send(c, now_us);
		if (n < CG_BATCH)
			return;
	}
}

/* Variant A: where the middle TCP goes, the VPS's tunnel address
 * (protocontroller_vps; empty: the first address of tun_addr's prefix, the
 * VPS's by convention, story 010) and protocontroller_port. 0 or -1. */
static int pctl_vps_addr(const struct cg_config *cfg, struct sockaddr_storage *a, char *err, size_t errlen)
{
	uint8_t ad[16];
	int fam = 0;

	memset(a, 0, sizeof(*a));
	if (cfg->protocontroller_vps[0]) {
		if (inet_pton(AF_INET, cfg->protocontroller_vps, ad) == 1)
			fam = AF_INET;
		else if (inet_pton(AF_INET6, cfg->protocontroller_vps, ad) == 1)
			fam = AF_INET6;
		else {
			snprintf(err, errlen, "protocontroller_vps: '%s' is not an IP address", cfg->protocontroller_vps);
			return -1;
		}
	} else {
		for (int i = 0; i < cfg->ntun_addr && !fam; i++) {
			const struct cg_pfx *t = &cfg->tun_addr[i];
			unsigned n = t->family == AF_INET ? 4 : 16, len = t->len;

			if (t->family == AF_INET6 && i + 1 < cfg->ntun_addr)
				continue; /* IPv4 first */
			if (len + 1 >= n * 8) { /* OpenWrt's /32: no prefix to take it from */
				snprintf(err, errlen, "protocontroller = a: tun_addr is a single address; set protocontroller_vps");
				return -1;
			}
			memcpy(ad, t->a, n);
			for (unsigned b = len; b < n * 8; b++)
				ad[b / 8] &= (uint8_t)~(0x80u >> (b % 8));
			ad[n - 1] |= 1;
			if (!memcmp(ad, t->a, n)) {
				snprintf(err, errlen, "protocontroller = a: tun_addr is the VPS's own; set protocontroller_vps");
				return -1;
			}
			fam = t->family;
		}
		if (!fam) {
			snprintf(err, errlen, "protocontroller = a: no tun_addr to take the VPS's from; set protocontroller_vps");
			return -1;
		}
	}
	if (fam == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)a;

		s->sin_family = AF_INET;
		memcpy(&s->sin_addr, ad, 4);
		s->sin_port = htons((uint16_t)cfg->protocontroller_port);
	} else {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)a;

		s->sin6_family = AF_INET6;
		memcpy(&s->sin6_addr, ad, 16);
		s->sin6_port = htons((uint16_t)cfg->protocontroller_port);
	}
	return 0;
}

/* the protocontroller (variant B) (protoctlio.h): n stream messages into the tunnel, as one batch. */
static void pctl_send(void *ctx, uint32_t peer, uint32_t sid, uint8_t *const *msg, const uint16_t *len,
		     const uint8_t *copy, int n)
{
	struct client *c = ctx;
	uint64_t now_us = cg_now_us(), now_ms = now_us / 1000;
	struct upctx u;

	(void)peer;
	(void)sid;
	if (n <= 0)
		return;
	up_begin(c, now_ms, &u);
	/* with redund_spare, also the links stalled that still bring
	 * something down (pnc_links: the stream's ARQ answers their loss) */
	if (c->cfg->redund_spare) {
		uint16_t ps = (uint16_t)(pnc_links(c, u.carry, now_ms) & u.present);

		u.live |= ps;
		u.carry |= ps;
	}
	f3_begin(c, &u);
	for (int m = 0; m < n; m++)
		up_one(c, &u, m, msg[m], len[m], now_us, copy[m]);
	c->last_traffic_ms = now_ms;
	send_links(c, n, now_ms, u.live, u.carry, u.k.dup);
	f3_send(c, now_us);
	if (c->nfecq)
		fec_send(c, now_us);
}

/* The upload capacity of the links that carry the streams (capacity.h),
 * bytes/s: split, the sum of those in the split; with redund_spare, the
 * first link's (the others carry its repairs); else the largest carrier
 * (in redundancy every packet goes on every carrier, the fastest
 * delivers). Full: the engine found that capacity under a standing queue
 * (CG_CAP_CUT_FULL), so it is what the link carries and no more; a loss
 * cut (a microcut) is not a ceiling, the ARQ answers loss. */
/* The protocontroller's repair horizon for the upload (task #77, protoctlio.h's
 * ops.horizon): while the code sends repairs (FEC v3 with a share now, or
 * redund_spare's codes), how long a hole may wait at the VPS for them and
 * for the NACK after them (its rescue_code_ms and rescue wait, taken as
 * ours: the two ends are set alike); 0 without a code now. */
static uint32_t pctl_horizon(void *ctx, uint32_t peer, uint32_t sid)
{
	struct client *c = ctx;
	int code = (f3_on(c) && c->f3.mode != CG_F3_OFF && c->f3.pct) || c->cfg->redund_spare >= 2;

	(void)peer;
	(void)sid;
	return code ? (c->cfg->rescue_code_ms + cg_rescue_wait_ms(c->cfg)) * 1000 : 0;
}

static void pctl_hint(void *ctx, uint32_t peer, uint32_t sid, uint64_t *bps, int *full)
{
	struct client *c = ctx;
	uint64_t now_ms = cg_now_ms(), sum = 0, best = 0;
	struct upctx u;
	uint16_t set;
	int nfull = 0, n = 0, bi = -1;

	(void)peer;
	(void)sid;
	up_begin(c, now_ms, &u);
	set = u.split ? (uint16_t)((u.k.eligible | u.k.dup) & u.carry) : u.carry;
	if (!u.split && c->cfg->redund_spare && c->sp_first1 && (set >> (c->sp_first1 - 1) & 1))
		set = (uint16_t)(1u << (c->sp_first1 - 1));
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		uint64_t b;

		if (!(set >> i & 1) || !c->cap[i].have_rate)
			continue;
		b = cg_cap_bps(&c->cap[i]) / 8;
		sum += b;
		n++;
		nfull += c->cap[i].cut == CG_CAP_CUT_FULL;
		if (b > best) {
			best = b;
			bi = i;
		}
	}
	*bps = u.split ? sum : best;
	*full = u.split ? n && nfull == n : bi >= 0 && c->cap[bi].cut == CG_CAP_CUT_FULL;
}

static void rescue_apply(struct client *c);

/* A verified probe reply on l: payload, plen bytes (at least
 * CG_PROBE_INFO_LEN), read at now32 / now_ms. */
static void on_probe_reply(struct client *c, struct link *l, const struct cg_hdr *h, const uint8_t *payload,
			   size_t plen, uint32_t now32, uint64_t now_ms)
{
	struct cg_probe_info pi;
	struct cg_probe_bond pb;
	uint32_t rtt;

	cg_probe_info_read(&pi, payload);
	/* Whether the server reorders, from the newest reply sent (not the
	 * last to arrive: one sent before a reload may come later on a slow
	 * link): split = on follows it from the next batch (an older server
	 * sends no tail). */
	if (cg_split_tail_newer(&c->tail, h->seq)) {
		struct cg_probe_act pa;

		c->peer_bond = cg_probe_bond_read(&pb, payload, plen);
		c->peer_reorders = pb.reorders;
		c->peer_rescue = pb.rescue;
		c->peer_tcpmark = pb.tcpmark;
		rescue_apply(c);
		/* The links it carries the download on (active.h); none said:
		 * every link counts. */
		if (cg_probe_act_read(&pa, payload, plen))
			cg_act_rx_mask(&c->act_rx, h->ts, pa.mask, pa.hold_ms, now_ms * 1000);
		else
			c->act_rx.have = 0;
	}
	/* This path's losses the server will not send again now (CG_BOND_FULL):
	 * the reorderer does not ask for them. */
	{
		struct cg_probe_bond fb;
		uint16_t bit = (uint16_t)(1u << (l - c->link));

		if (cg_probe_bond_read(&fb, payload, plen) && fb.full)
			c->rxs.ro.full |= bit;
		else
			c->rxs.ro.full &= (uint16_t)~bit;
	}
	/* Each probe is answered once: a copy of this reply replayed later
	 * cannot count for epoch.h. */
	cg_echo_take(&l->rxl.probes, pi.echo_ts, now_ms, CG_ECHO_MAX_AGE_MS);
	rtt = now32 - pi.echo_ts;
	if (rtt < 10u * 1000 * 1000) {
		l->rtt_us = rtt;
		l->srtt8_us = l->srtt8_us ? l->srtt8_us - l->srtt8_us / 8 + rtt : (uint64_t)rtt * 8;
	}
	l->last_reply_ms = now_ms;
	l->unanswered = 0;
	switch (cg_srv_on_reply(&l->silent_moves, l->ncand, l->cand_idx)) {
	case CG_SRV_BACK:
		l->back_to_first = 1;
		c->next_reconcile_ms = 0; /* at the next tick, not while this socket is being read */
		break;
	case CG_SRV_RESUMED: {
		char a[64];

		/* Once per dead round; its later moves were not logged. */
		cg_info("link %s: the server answers again after a round of its addresses without replies, at %s",
			l->ifname, cg_addr_str(&l->remote, a, sizeof(a)));
		break;
	}
	}
	l->peer_view = pi;
	l->peer_view_ms = now_ms;
	l->peer_muted = !!(h->flags & CG_F_MUTED);
	c->server_pass = cg_pass_get(h->flags);
	/* The server timed our probe's trip up this link; we time the reply's
	 * trip down and report it in the next probe. */
	if (h->flags & CG_F_OWD)
		cg_health_report(&c->uh[l - c->link], pi.owd, now_ms,
				 cg_health_valid_ms(l->probe_announced > l->probe_before ? l->probe_announced : l->probe_before,
						    CG_STALL_PROBES * c->cfg->probe_idle_ms));
	l->down_owd = now32 - h->ts;
	l->have_down_owd = 1;
	l->raw_ms[l->raw_at] = now_ms;
	l->raw_up[l->raw_at] = (h->flags & CG_F_OWD) ? pi.owd : 0;
	l->raw_down[l->raw_at] = l->down_owd;
	l->raw_at = (uint8_t)((l->raw_at + 1) % CG_PROBE_RAW);
	/* The same trip for the reorderer's deadlines, on the clock of its
	 * arrival times (reorder.h): its own filter, so that a quiet download
	 * still knows how far apart the links are. */
	cg_ro_owd(&c->rxs.ro, (unsigned)(l - c->link), l->down_owd);
}

static void send_probe(struct client *c, struct link *l, uint64_t now_us, uint32_t interval);

/* A HELLO gave l a cookie: the probe that carries it goes at once instead of
 * at the next interval (a second when idle), at most once a second per
 * link, so a server that keeps asking cannot make the probes run at the
 * RTT. A refusal waits for the interval. */
static void on_hello(struct client *c, struct link *l, uint64_t now_us)
{
	uint64_t now_ms = now_us / 1000;

	if (l->rxl.refused || now_ms - l->hello_probe_ms < 1000)
		return;
	l->hello_probe_ms = now_ms;
	send_probe(c, l, now_us, probe_interval(c, now_ms));
}

/* Logs what a verdict of cg_rx_entry asks for, rate-limited. */
static void rx_verdict_log(struct client *c, struct link *l, enum cg_rxv v, uint64_t now_ms)
{
	if (v == CG_RXV_AUTH && cg_ratelimit_ok(&c->rl_auth, now_ms, 10000))
		cg_warn("link %s: packet failed authentication (wrong key?)", l->ifname);
	else if (v == CG_RXV_HELLO && l->rxl.refused && cg_ratelimit_ok(&c->rl_refused, now_ms, 60000))
		cg_warn("link %s: the server refuses this router (turned off there, or no room for a session)", l->ifname);
	else if (v == CG_RXV_HELLO && cg_ratelimit_ok(&c->rl_hello, now_ms, 10000))
		cg_dbg("link %s: the server asks for a cookie (a new session, link or address)", l->ifname);
}

/* Writes q packets to the TUN, never waiting: what does not fit is
 * dropped, as a full link drops its own copies. One write per packet, each
 * checked first: the MAC says who sent it, this that the kernel gets an IP
 * packet. */
static void tun_send(struct client *c, struct mmsghdr *m, int q, uint64_t now_ms)
{
	c->last_traffic_ms = now_ms;
	for (int i = 0; i < q; i++) {
		const struct iovec *v = m[i].msg_hdr.msg_iov;

		if (!cg_ip_check(v->iov_base, v->iov_len)) {
			c->down_not_ip++;
			continue;
		}
		if (cg_tun_tx(c->tun_fd, v->iov_base, v->iov_len) < 0) {
			c->down_tun_drops++;
			if (errno != EAGAIN && cg_ratelimit_ok(&c->rl_tun, now_ms, 10000))
				cg_warn("write to %s: %s", c->cfg->tun, strerror(errno));
		}
	}
}

static void tun_msg(struct client *c, struct mmsghdr *m, struct iovec *iov, uint8_t *payload, size_t len)
{
	iov->iov_base = payload;
	iov->iov_len = len;
	memset(&m->msg_hdr, 0, sizeof(m->msg_hdr));
	m->msg_hdr.msg_iov = iov;
	m->msg_hdr.msg_iovlen = 1;
	c->down_pkts++;
	c->down_bytes += len;
}

/* ---- download: to the TUN, in sequence (reorder.h) ---- */

/* The links the reorderer waits for at now32 (its clock): those the server
 * should send each packet on, and that answer, narrowed to those it carries
 * the download on lately (reorder_active, active.h: clockwork audit C2).
 * One left out may give a gap up early; one too many only waits for the
 * deadline. A caller holding the result through a batch adds the links the
 * batch's packets open (act_rx.fresh, from act_base), which this empties. */
static uint16_t ro_rset(struct client *c, uint64_t now_ms, uint32_t now32)
{
	uint16_t present, live, m;

	link_masks(c, now_ms, &present, &live);
	m = expect_mask(c) & live;
	/* reorder_idle_ms: not a link that brought no data for that long (the
	 * server's ETA may send it nothing; clockwork audit C2) */
	for (int i = 0; c->cfg->reorder_idle_ms && i < CG_MAX_LINKS; i++)
		if ((m >> i & 1) && cg_arr_idle(&c->rxs.rx[i], (uint32_t)(now_ms * 1000), c->cfg->reorder_idle_ms * 1000))
			m &= (uint16_t)~(1u << i);
	c->act_base = c->rxs.ro.wide = m;
	c->act_rx.fresh = 0;
	if (c->cfg->reorder_active)
		m = cg_act_rx_rset(&c->act_rx, &c->rxs.ro, c->rocf.margin_us, m, now_ms * 1000, now32);
	return m;
}

/* When a link of act_base leaves rset (its horizon, active.h): us from
 * now32, INT32_MAX when none will. */
static int32_t ro_act_next(const struct client *c, uint64_t now_ms, uint32_t now32)
{
	if (!c->cfg->reorder_active)
		return INT32_MAX;
	return cg_act_rx_next_us(&c->act_rx, &c->rxs.ro, c->rocf.margin_us, c->act_base, now_ms * 1000, now32);
}

/* Writes the output batch to the TUN, then gives back the pool buffers it
 * carried: only then, or a packet held meanwhile could take one still
 * waiting to be written. */
static void dout_flush(struct client *c, uint64_t now_ms)
{
	if (c->dq)
		tun_send(c, c->dmsg, c->dq, now_ms);
	for (int i = 0; i < c->dnbuf; i++)
		cg_ro_pool_put(&c->pool, c->dbuf[i]);
	c->dq = c->dnbuf = 0;
}

/* Appends a packet for the TUN to the output batch, after everything
 * already in it; buf: the pool buffer it is in, CG_RO_NOBUF when none (a
 * socket's batch or a pump's slot, both kept until the batch is sent). A
 * full batch goes first. */
static void dout_add(struct client *c, uint8_t *payload, size_t len, uint16_t buf, uint64_t now_ms, uint32_t seq,
		     uint8_t flags)
{
	if (c->dq == DOUT_MAX)
		dout_flush(c, now_ms);
	/* where the download's TCP leaves the tunnel (tcpfree.h, story 023) */
	if (c->cfg->tcp_aware) {
		struct cg_flow f;

		if (cg_flow_of(payload, len, c->flow_seed, &f) && f.tcp)
			cg_tcpe_out(&c->tcpe, &f, payload, len, (uint32_t)cg_now_us(), seq, cg_prev_link(flags));
	}
	tun_msg(c, &c->dmsg[c->dq], &c->diov[c->dq], payload, len);
	c->dq++;
	if (buf != CG_RO_NOBUF)
		c->dbuf[c->dnbuf++] = buf;
}

/* The rescue (rescue.h): the reorderer asks for a gap about to go only
 * when rescue_wait_ms is set and the server said it keeps what it sent. */
/* The round trip of the fastest live link, 0: none measured. */
static uint32_t fastest_rtt_us(struct client *c, uint64_t now_ms)
{
	uint16_t present, live;
	uint64_t best = 0;

	link_masks(c, now_ms, &present, &live);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((live >> i & 1) && c->link[i].fd >= 0 && c->link[i].srtt8_us &&
		    (!best || c->link[i].srtt8_us < best))
			best = c->link[i].srtt8_us;
	return (uint32_t)(best / 8);
}

/* TCP's budget for the download's gaps (harq.h), 0 when no TCP flow went
 * out of the tunnel lately or rescue_tcp_race is off. */
static uint32_t tcp_budget_us(struct client *c, uint64_t now_us)
{
	if (!c->cfg->rescue_tcp_race)
		return 0;
	return cg_harq_budget_us(
		cg_tcpe_rtt(&c->tcpe, fastest_rtt_us(c, now_us / 1000), (uint32_t)now_us, 10u * 1000 * 1000));
}

static void rescue_apply(struct client *c)
{
	uint64_t now_us = cg_now_us();
	uint32_t w = c->cfg->reorder && c->peer_rescue ? cg_rescue_wait_ms(c->cfg) * 1000 : 0,
		 code = dwide(c, now_us / 1000) ? c->cfg->rescue_code_ms * 1000 : 0, budget = tcp_budget_us(c, now_us);

	/* within TCP's budget (harq.h): held longer, its own repair comes
	 * first; with FEC v3's repairs of the download coming, the code's
	 * horizon too, with the rescue or alone (story 026) */
	c->rocf.rescue_us = w ? cg_harq_wait_us(w, code, budget) : cg_harq_code_wait_us(code, budget);
}

/* NACKs for seq[0..n) on the two live links of least RTT (retry:
 * CG_F_RETRY; the request waits for its answer: the fastest way there). */
static void nack_out(struct client *c, const uint32_t *seq, uint32_t n, int retry, uint64_t now_ms)
{
	uint16_t present, live;
	struct link *best[2] = { NULL, NULL };

	link_masks(c, now_ms, &present, &live);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !l->srtt8_us)
			continue;
		if (!best[0] || l->srtt8_us < best[0]->srtt8_us) {
			best[1] = best[0];
			best[0] = l;
		} else if (!best[1] || l->srtt8_us < best[1]->srtt8_us) {
			best[1] = l;
		}
	}
	for (uint32_t i = 0; best[0] && i < n;) {
		uint8_t pkt[CG_HDR_LEN + CG_NACK_LEN];
		unsigned used;
		size_t plen = cg_nack_write(pkt + CG_HDR_LEN, seq + i, n - i, &used);

		i += used;
		for (int b = 0; b < 2 && best[b]; b++) {
			cg_up_header(&c->txs, pkt, CG_T_NACK, retry ? CG_F_RETRY : 0, (uint8_t)(best[b] - c->link),
				     (uint32_t)cg_now_us(), pkt + CG_HDR_LEN, plen);
			if (send(best[b]->fd, pkt, CG_HDR_LEN + plen, MSG_DONTWAIT) == (ssize_t)(CG_HDR_LEN + plen))
				c->nacks_sent++;
		}
	}
}

static void d_send_asks(struct client *c, uint64_t now_ms);

/* The gaps the reorderer asked for, as NACKs (nack_out); with FEC v3's
 * repairs of the download coming, by harq.h's route (d_send_asks). */
static void send_nacks(struct client *c, uint64_t now_ms)
{
	struct cg_reorder *r = &c->rxs.ro;
	uint32_t budget, rtt;

	if (dwide(c, now_ms)) {
		d_send_asks(c, now_ms);
		return;
	}
	budget = tcp_budget_us(c, now_ms * 1000);
	rtt = fastest_rtt_us(c, now_ms);
	/* An answer that cannot come before TCP's own retransmission
	 * (harq.h): left to TCP, the gaps given up now. */
	if (budget && rtt >= budget) {
		for (uint32_t i = 0; i < r->nask; i++)
			c->ask_tcp += (uint64_t)cg_ro_refused(r, &c->rocf, r->ask[i], (uint32_t)cg_now_us());
		for (uint32_t i = 0; i < r->nask2; i++)
			c->ask_tcp += (uint64_t)cg_ro_refused(r, &c->rocf, r->ask2[i], (uint32_t)cg_now_us());
		r->nask = r->nask2 = 0;
		return;
	}
	/* First asks, then retries (CG_F_RETRY), each on the two fastest links. */
	if (r->nask)
		nack_out(c, r->ask, r->nask, 0, now_ms);
	if (r->nask2)
		nack_out(c, r->ask2, r->nask2, 1, now_ms);
	r->nask = r->nask2 = 0;
}

/* Whether link i's upload is full, so that what it loses is congestion for
 * TCP to see, not a loss to send again (rescue.h): its rescue weight
 * (capacity.h's cg_cap_rescue_w) is 0. */
static int up_full(const struct client *c, unsigned i)
{
	return cg_cap_full(&c->cap[i], c->cfg->rescue_target_ms * 1000, c->lm[i].rfull_hyst);
}

_Static_assert(CG_PCTL_LINKS == CG_MAX_LINKS && CG_PCTL_NOLINK == CG_RESC_NOLINK,
	       "cg_pctl_nack takes the rescue ring's link as it is");

/* A verified NACK from the server (rescue.h): each sequence the ring still
 * keeps goes again, within the budget, on the live link of least RTT;
 * not one lost on a full link, nor a GRIT message (its loss goes to its
 * stream, cg_pctl_nack); what does not go again is said gone. */
static void on_nack(struct client *c, uint8_t flags, const uint8_t *payload, size_t plen, uint64_t now_ms)
{
	uint32_t seq[CG_NACK_SEQ_MAX];
	unsigned n = cg_nack_read(seq, CG_NACK_SEQ_MAX, payload, plen);
	uint16_t present, live;
	struct link *best[2] = { NULL, NULL };

	uint32_t gone[CG_NACK_SEQ_MAX];
	unsigned ngone = 0;

	c->nacks_rx++;
	/* The server will not send these again: the reorderer gives them up
	 * now instead of waiting for them. */
	if (flags & CG_F_GONE) {
		for (unsigned i = 0; i < n; i++)
			c->resc_gone_rx += cg_ro_refused(&c->rxs.ro, &c->rocf, seq[i], (uint32_t)cg_now_us());
		return;
	}
	if (!c->resc_idx)
		return;
	link_masks_tx(c, now_ms, &present, &live);
	/* rescue_by_owd (audit C10): the link of least upload delay, as the
	 * server picks; by RTT while no link has one */
	for (int i = 0; c->cfg->rescue_by_owd && i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !c->uh[i].have_owd)
			continue;
		if (!best[0] || cg_owd_diff(c->uh[i].owd, c->uh[best[0] - c->link].owd) < 0) {
			best[1] = best[0];
			best[0] = l;
		} else if (!best[1] || cg_owd_diff(c->uh[i].owd, c->uh[best[1] - c->link].owd) < 0) {
			best[1] = l;
		}
	}
	for (int i = 0, by_rtt = !best[0]; by_rtt && i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !l->srtt8_us)
			continue;
		if (!best[0] || l->srtt8_us < best[0]->srtt8_us) {
			best[1] = best[0];
			best[0] = l;
		} else if (!best[1] || l->srtt8_us < best[1]->srtt8_us) {
			best[1] = l;
		}
	}
	for (unsigned i = 0; best[0] && i < n; i++) {
		uint16_t len;
		uint8_t went;
		uint8_t *e = (uint8_t *)cg_resc_get(&c->resc, c->resc_idx, c->txs.session, seq[i], &len, &went);

		if (!e) {
			c->resc_miss++;
			gone[ngone++] = seq[i];
			continue;
		}
		if (cg_resc_dup(&c->resc, c->resc_idx, seq[i], (uint32_t)now_ms))
			continue;
		/* a GRIT message (story 025): its stream retransmits what it
		 * loses, so the loss goes to it, and the server stops waiting */
		if (c->pctl) {
			int k = cg_pctl_nack(c->pctl, 0, 0, e + CG_HDR_LEN, (size_t)len - CG_HDR_LEN, went);

			if (k == CG_PCTL_NACK_DATA || k == CG_PCTL_NACK_ACK) {
				c->resc_grit++;
				c->resc_grit_bytes += len - CG_HDR_LEN;
				gone[ngone++] = seq[i];
				continue;
			}
		}
		if (went < CG_MAX_LINKS && up_full(c, went)) {
			c->resc_congested++;
			gone[ngone++] = seq[i];
			continue;
		}
		/* A retry goes on the two fastest links. */
		for (int b = 0; b < ((flags & CG_F_RETRY) ? 2 : 1) && best[b]; b++) {
			if (!cg_resc_spend(&c->resc_b, len - CG_HDR_LEN)) {
				c->resc_budget++;
				if (!b)
					gone[ngone++] = seq[i];
				break;
			}
			cg_hdr_set_link(e, (uint8_t)(best[b] - c->link));
			cg_act_tx_sent(&c->act_tx, (unsigned)(best[b] - c->link), cg_now_us());
			if (send(best[b]->fd, e, len, MSG_DONTWAIT) == (ssize_t)len) {
				best[b]->tx_pkts++;
				best[b]->tx_bytes += len - CG_HDR_LEN;
				c->resc_sent++;
			}
		}
	}
	/* What will not come again, said back so the server does not wait. */
	for (unsigned i = 0; best[0] && i < ngone;) {
		uint8_t pkt[CG_HDR_LEN + CG_NACK_LEN];
		unsigned used;
		size_t glen = cg_nack_write(pkt + CG_HDR_LEN, gone + i, ngone - i, &used);

		i += used;
		cg_up_header(&c->txs, pkt, CG_T_NACK, CG_F_GONE, (uint8_t)(best[0] - c->link), (uint32_t)cg_now_us(),
			     pkt + CG_HDR_LEN, glen);
		if (send(best[0]->fd, pkt, CG_HDR_LEN + glen, MSG_DONTWAIT) == (ssize_t)(CG_HDR_LEN + glen))
			c->resc_gone_tx++;
	}
}

/* The link a PARQ's repairs go on: live, not choked (spare.h) nor full
 * (capacity.h), of least RTT; NULL: none. */
static struct link *parq_link(struct client *c, uint64_t now_ms)
{
	uint16_t present, live;
	struct link *best = NULL;

	link_masks_tx(c, now_ms, &present, &live);
	/* never a choked or full link: what is asked for again would come
	 * after its queue, and fill it (a microcut: 1 500 repairs a second on
	 * the cellular link, its queue at 1.6 s, story 025) */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !l->srtt8_us)
			continue;
		if ((c->sp_choked >> i & 1) || up_full(c, (unsigned)i))
			continue;
		if (!best || l->srtt8_us < best->srtt8_us)
			best = l;
	}
	return best;
}

/* One repair of a PARQ (header h, slen bytes of symbol at sym) on link l,
 * within the rescue budget. */
static void parq_one(struct client *c, struct link *l, const struct cg_rep_hdr *h, const uint8_t *sym, unsigned slen)
{
	uint8_t pkt[CG_HDR_LEN + CG_REP_HDR + CG_RS_BYTES_MAX];
	size_t plen = CG_REP_HDR + slen;

	/* the rescue's budget (rescue_pct of the DATA: without the rescue
	 * ring, FEC v3's window earns it, f3_feed) */
	if (!cg_resc_spend(&c->resc_b, (uint32_t)plen)) {
		c->parq_budget++;
		return;
	}
	cg_rep_put(pkt + CG_HDR_LEN, h);
	memcpy(pkt + CG_HDR_LEN + CG_REP_HDR, sym, slen);
	cg_up_header(&c->txs, pkt, CG_T_REPAIR, 0, (uint8_t)(l - c->link), (uint32_t)cg_now_us(), pkt + CG_HDR_LEN, plen);
	cg_act_tx_sent(&c->act_tx, (unsigned)(l - c->link), cg_now_us());
	if (send(l->fd, pkt, CG_HDR_LEN + plen, MSG_DONTWAIT) == (ssize_t)(CG_HDR_LEN + plen)) {
		c->parq_sent++;
		c->parq_bytes += plen;
		l->tx_fec_pkts++;
		l->tx_fec_bytes += plen;
	} else {
		l->tx_drops++;
	}
}

/* A verified PARQ from the server (harq.h): the repairs it asks for,
 * rebuilt from the rescue ring: RLC, d combinations of the missing members
 * with fresh seeds; RS, the block encoded again (its geometry, m) and d
 * indices the server does not have; on a link with room. */
static void on_parq(struct client *c, const uint8_t *payload, size_t plen, uint64_t now_ms)
{
	struct cg_parq_req req[CG_PARQ_MAX];
	int n = cg_parq_read(req, payload, (unsigned)plen);
	struct link *l;

	c->parq_rx++;
	if (n < 0) {
		c->parq_bad++;
		return;
	}
	if (!c->resc_idx && !f3_on(c))
		return;
	if (!(l = parq_link(c, now_ms))) {
		c->parq_nolink++;
		return;
	}
	for (int q = 0; q < n; q++) {
		const struct cg_parq_req *r = &req[q];
		const uint8_t *p[CG_REP_SPAN];
		uint16_t len[CG_REP_SPAN];
		unsigned k = 0, slen = 0;
		int ok = 1;

		c->parq_req++;
		for (uint64_t m = r->map; m && ok; m &= m - 1, k++) {
			uint32_t sq = r->base + (unsigned)__builtin_ctzll(m);
			uint16_t el = 0;
			uint8_t went;
			const uint8_t *e = c->resc_idx ? cg_resc_get(&c->resc, c->resc_idx, c->txs.session, sq, &el, &went) : NULL;

			if (e && el > CG_HDR_LEN && el - CG_HDR_LEN <= CG_SYM_PLEN) {
				p[k] = e + CG_HDR_LEN;
				len[k] = (uint16_t)(el - CG_HDR_LEN);
			} else if (f3_on(c) && (p[k] = cg_rlcw_get(c->f3w, sq, &len[k])) != NULL) {
				/* FEC v3's window keeps every DATA lately (story 026) */
			} else {
				ok = 0;
				break;
			}
			if (len[k] + 2u > slen)
				slen = len[k] + 2u;
		}
		if (!ok) {
			c->parq_missing++;
			continue;
		}
		if (r->kind == CG_REP_RLC) {
			for (unsigned d = 0; d < r->d; d++) {
				uint8_t sym[CG_SYM_MAX];
				struct cg_rep_hdr h;
				unsigned sl;

				/* fresh seeds, from the top half: the window's count up from 0 */
				c->parq_seed = (uint16_t)(c->parq_seed + 1);
				sl = cg_rlc_combine(&c->sp_gf, r->base, r->map, (uint16_t)(0x8000u | c->parq_seed), p, len, &h, sym);
				parq_one(c, l, &h, sym, sl);
			}
		} else if (c->parq_rs) {
			struct cg_rs_scratch *x = c->parq_rs;
			const uint8_t *orig[CG_RS_KMAX];
			uint8_t *work[CG_RS_WORK_DEC], idx[CG_RS_MMAX];
			unsigned bytes = cg_rs_bytes(slen), ni;
			struct cg_rep_hdr h = { .base = r->base, .map = r->map, .kind = CG_REP_RS, .m = r->m };

			if (bytes > CG_RS_BYTES_MAX) {
				c->parq_missing++;
				continue;
			}
			for (unsigned i = 0; i < k; i++) {
				x->orig[i][0] = (uint8_t)(len[i] >> 8);
				x->orig[i][1] = (uint8_t)len[i];
				memcpy(x->orig[i] + 2, p[i], len[i]);
				memset(x->orig[i] + 2 + len[i], 0, bytes - 2u - len[i]);
				orig[i] = x->orig[i];
			}
			for (unsigned j = 0; j < CG_RS_WORK_DEC; j++)
				work[j] = x->work[j];
			if (cg_rs_encode(k, r->m, bytes, orig, work) < 0) {
				c->parq_missing++;
				continue;
			}
			ni = cg_parq_rs_pick(r->have, r->m, r->d, idx);
			for (unsigned i = 0; i < ni; i++) {
				h.idx = idx[i];
				parq_one(c, l, &h, work[idx[i]], bytes);
			}
		}
	}
}

/* What the reorderer may hand on at now (the pass's clock, as its arrival
 * times), after what the batch holds: everything with all (a window
 * overflow, taken again right after), else up to the pass's budget; the
 * next pass comes at once for the rest (cg_ro_due_us says 0). A drain the
 * budget cuts ends here for max_burst, which says what one drain gave
 * the TUN at once. */
static void ro_drain(struct client *c, uint16_t rset, uint32_t now, int all, uint64_t now_ms)
{
	struct cg_ro_out o;

	while ((all || c->ro_budget) && cg_ro_next(&c->rxs.ro, &c->rocf, rset, now, &o)) {
		dout_add(c, cg_ro_pool_buf(&c->pool, o.buf), o.len, o.buf, now_ms, o.seq, o.flags);
		if (c->ro_budget)
			c->ro_budget--;
	}
	cg_ro_burst_end(&c->rxs.ro);
	if (c->rxs.ro.nask || c->rxs.ro.nask2)
		send_nacks(c, now_ms);
}

/* A first copy for the TUN, payload (len bytes) of DATA h from link,
 * read at t_arr: into the batch now, or once what comes before it did
 * (copied into a pool buffer meanwhile), with what it releases after it.
 * The one place every link_threads mode delivers through. */
static void down_deliver(struct client *c, const struct cg_hdr *h, unsigned link, uint8_t *payload, size_t len,
			 uint32_t t_arr, uint16_t rset, uint32_t now, uint64_t now_ms)
{
	struct cg_ro_pkt k = { .seq = h->seq,
			       .ts = h->ts,
			       .t_arr = t_arr,
			       .len = (uint16_t)len,
			       .link = (uint8_t)link,
			       .flags = h->flags,
			       .prev = (uint8_t)(cg_prev_link(h->flags) + 1),
			       /* free with TCP data: a retransmission (tcp_aware) */
			       .retx = (uint8_t)((h->flags & CG_F_FREE) && cg_tcp_data(payload, len)),
			       .rebuilt = c->drebuilding };
	enum cg_ro_v v;
	uint16_t buf;

	/* What it says of the links the server carries the download on (its own,
	 * and the one CG_F_PREV names): one that opens counts from this packet
	 * on (active.h). */
	if (c->cfg->reorder) {
		cg_act_rx_data(&c->act_rx, link, h->ts, cg_prev_link(h->flags), now_ms * 1000);
		rset |= c->act_rx.fresh & c->act_base;
	}

	/* A protocontroller stream's message (story 025): to its stream at once, its
	 * sequence still counted by the reorderer (never held: it has its own
	 * order). */
	if (cg_pctl_is(payload, len)) {
		if (!c->pctl) {
			c->down_pctl_off++;
			return;
		}
		if (c->cfg->reorder) {
			k.nohold = 1;
			if (cg_ro_in(&c->rxs.ro, &c->pool, &c->rocf, &k, rset, &buf) == CG_RO_AGAIN) {
				ro_drain(c, rset, now, 1, now_ms);
				cg_ro_in(&c->rxs.ro, &c->pool, &c->rocf, &k, rset, &buf);
			}
		}
		cg_pctl_rx(c->pctl, 0, 0, payload, len, c->drebuilding ? CG_PCTL_NOLINK : link);
		if (c->cfg->reorder)
			ro_drain(c, rset, now, 0, now_ms);
		return;
	}

	/* free without data: the upload's ACKs, with the server's mark on
	 * their holes (tcpack.h) */
	if ((h->flags & CG_F_FREE) && !k.retx && c->cfg->tcp_aware) {
		struct cg_flow f;

		if (cg_flow_of_dir(payload, len, c->flow_seed, 1, &f) && f.tcp)
			cg_tcpa_ack(&c->tcpa, &f, payload, len, !!(h->flags & CG_F_TUNHOLE), c->peer_tcpmark);
	}

	if (!c->cfg->reorder) {
		dout_add(c, payload, len, CG_RO_NOBUF, now_ms, h->seq, h->flags);
		return;
	}
	v = cg_ro_in(&c->rxs.ro, &c->pool, &c->rocf, &k, rset, &buf);
	if (v == CG_RO_AGAIN) {
		/* The window overflowed: the gaps it forces out go, with what was
		 * held behind them, all of it; then the packet is taken. */
		ro_drain(c, rset, now, 1, now_ms);
		v = cg_ro_in(&c->rxs.ro, &c->pool, &c->rocf, &k, rset, &buf);
	}
	if (v == CG_RO_HOLD)
		memcpy(cg_ro_pool_buf(&c->pool, buf), payload, len);
	else
		dout_add(c, payload, len, CG_RO_NOBUF, now_ms, h->seq, h->flags); /* NOW (a second AGAIN cannot be: never dropped) */
	ro_drain(c, rset, now, 0, now_ms);
}

/* ---- FEC v3 down: what the server's repairs cover, rebuilt here (story 026) ---- */

static struct cg_rep_ring dring(const struct client *c)
{
	struct cg_rep_ring r = { c->dfec->seq, c->dfec->len, c->dfec->have, c->dfec->mem };

	return r;
}

/* Whether FEC v3's wide repairs protect the download (within the last two
 * seconds, as the server's harq_wide), the reorderer on. */
static int dwide(const struct client *c, uint64_t now_ms)
{
	return c->dfec && c->df3_last_ms && now_ms - c->df3_last_ms < 2000 && c->cfg->reorder;
}

/* DATA seq rebuilt (len bytes at p, in the ring) on to the reorderer as if
 * it had come on link with ts, unless it came or was given up meanwhile
 * (the replay window), and once the guard passed it (rebuilt.h: refused,
 * the ring forgets it and the equations go, as the server's fec_deliver). */
static void d_deliver(struct client *c, uint32_t seq, uint8_t *p, uint16_t len, uint8_t link, uint32_t ts,
		      uint16_t rset, uint32_t now, uint64_t now_ms)
{
	struct cg_hdr h = { .type = CG_T_DATA, .link = link, .session = c->rxs.session, .seq = seq, .ts = ts };
	int rb;

	if (cg_replay_check(&c->rxs.replay, seq) != CG_RP_NEW) {
		c->d_fec_dups++;
		return;
	}
	rb = cg_rebuilt_check(p, len);
	if (rb != CG_RB_OK) {
		uint32_t i = seq & (CG_FEC_RING - 1);

		if (rb == CG_RB_BAD)
			c->d_rb_bad++;
		else
			c->d_rb_unsure++;
		if (c->dfec->have[i] && c->dfec->seq[i] == seq)
			c->dfec->have[i] = 0;
		cg_rlc_rx_clear(c->drlc);
		return;
	}
	c->d_rb_ok++;
	cg_replay_mark(&c->rxs.replay, seq);
	c->drebuilding = 1;
	down_deliver(c, &h, link, p, len, now, rset, now, now_ms);
	c->drebuilding = 0;
}

static void d_rep_deliver(struct client *c, const uint32_t *seqs, unsigned n, uint8_t link, uint32_t ts, uint16_t rset,
			  uint32_t now, uint64_t now_ms)
{
	struct cg_rep_ring r = dring(c);

	for (unsigned i = 0; i < n; i++) {
		uint16_t len;
		uint8_t *p = (uint8_t *)cg_rep_data(&r, seqs[i], &len);

		d_deliver(c, seqs[i], p, len, link, ts, rset, now, now_ms);
	}
}

/* DATA seq went into the ring: out of the RLC's equations, and what that
 * settles on. */
static void d_rep_data(struct client *c, uint32_t seq, uint8_t link, uint32_t ts, uint16_t rset, uint32_t now,
		       uint64_t now_ms)
{
	struct cg_rep_ring r = dring(c);
	uint32_t out[CG_RLC_COLS];
	unsigned n = cg_rlc_rx_data(&c->sp_gf, c->drlc, &r, seq, out, CG_RLC_COLS);

	d_rep_deliver(c, out, n, link, ts, rset, now, now_ms);
}

static void d_harq_service(struct client *c, uint64_t now_us);

/* A repair of the download (plen bytes at p, repair.h) from link. */
static void d_rep_take(struct client *c, const uint8_t *p, unsigned plen, uint8_t link, uint32_t ts, uint16_t rset,
		       uint32_t now, uint64_t now_ms)
{
	struct cg_rep_hdr h;
	struct cg_rep_ring r;
	uint32_t out[CG_RLC_COLS];
	unsigned slen, n = 0;

	if (!c->dfec) {
		c->drep_unused++;
		return;
	}
	/* a repair may fill a gap: its link counts (active.h) */
	if (c->cfg->reorder) {
		cg_act_rx_data(&c->act_rx, link, ts, -1, now_ms * 1000);
		rset |= c->act_rx.fresh & c->act_base;
	}
	if (cg_rep_get(p, plen, &h, &slen) < 0) {
		c->drep_bad++;
		return;
	}
	/* what it seals (harq.h), and for the wide ones how far apart they
	 * come and their window (cg_harq_future) */
	if (!c->drep_top_ok || (int32_t)(h.base - c->drep_top) > 0) {
		c->drep_top = h.base;
		c->drep_top_ok = 1;
	}
	if (h.kind == CG_REP_RLCW) {
		uint32_t top = h.base + h.span - 1u;
		int start = !dwide(c, now_ms);

		c->df3_last_ms = now_ms;
		if (h.span >= c->df3_span || now_ms - c->df3_span_ms > 1000) {
			c->df3_span = h.span;
			c->df3_span_ms = now_ms;
		}
		if (!c->df3_have) {
			c->df3_top = top;
			c->df3_have = 1;
		} else if ((int32_t)(top - c->df3_top) >= 0) {
			uint32_t d = top - c->df3_top;

			if (d < 65536)
				c->df3_gap16 = c->df3_gap16 ? c->df3_gap16 - c->df3_gap16 / 8 + d * 16 / 8 : d * 16;
			c->df3_top = top;
		}
		if (start)
			rescue_apply(c); /* gaps wait for its repairs from now */
	}
	r = dring(c);
	if (h.kind == CG_REP_RLC || h.kind == CG_REP_RLCW)
		n = cg_rlc_rx_repair(&c->sp_gf, c->drlc, &r, &h, p + CG_REP_HDR, slen, out, CG_RLC_COLS);
	else
		c->drep_unused++;
	d_rep_deliver(c, out, n, link, ts, rset, now, now_ms);
	d_harq_service(c, cg_now_us());
}

/* The download's asks of a batch, as the server's harq_batch. */
struct dharq_batch {
	uint32_t nack[2][CG_RO_ASK_MAX + CG_DHQ_N];
	uint32_t nn[2];
	struct cg_parq_req req[CG_PARQ_MAX * 4];
	unsigned nr;
};

/* Download gap seq (asked at t_us; retry: asked again) by harq.h's route,
 * as the server's harq_one: 1 when it waits for the code (keep it). */
static int d_harq_one(struct client *c, struct dharq_batch *hb, uint32_t seq, uint32_t t_us, int retry, uint8_t *rel,
		      uint64_t now_us)
{
	const struct cg_ro_slot *sl = &c->rxs.ro.slot[seq % CG_RO_SLOTS];
	struct cg_harq_gap g = { 0 };
	uint32_t base = 0;
	uint64_t map = 0;

	if (!*rel && (sl->seq != seq || sl->state != CG_RO_ASKED))
		return 0; /* came, or went, meanwhile */
	g.code = (uint8_t)dwide(c, now_us / 1000);
	if (*rel && !g.code)
		return 0;
	if (g.code) {
		struct cg_rep_ring r = dring(c);

		g.here = (uint8_t)cg_rep_has(&r, seq);
		g.sealed = (uint8_t)(c->drep_top_ok && (int32_t)(c->drep_top - seq) > 0);
		g.deficit = (uint8_t)cg_rlc_rx_need(&c->sp_gf, c->drlc, seq, &base, &map);
		g.wide = 1;
		g.future = cg_harq_future(seq, c->df3_top, c->df3_span, c->df3_gap16);
	}
	g.age_us = (uint32_t)now_us - t_us;
	g.horizon_us = c->cfg->rescue_code_ms * 1000;
	g.eta_us = fastest_rtt_us(c, now_us / 1000);
	g.budget_us = tcp_budget_us(c, now_us);
	if (!c->cfg->parq)
		g.deficit = 0;
	switch (cg_harq_route(&g)) {
	case CG_HARQ_WAIT:
		if (!*rel && !cg_harq_hold(g.budget_us)) {
			cg_ro_refused(&c->rxs.ro, &c->rocf, seq, (uint32_t)now_us);
			*rel = 1;
		}
		return 1;
	case CG_HARQ_NONE:
		c->dhq_none++;
		return 0;
	case CG_HARQ_TCP:
		c->dhq_tcp++;
		c->ask_tcp++;
		if (!*rel)
			cg_ro_refused(&c->rxs.ro, &c->rocf, seq, (uint32_t)now_us);
		return 0;
	case CG_HARQ_PARQ: {
		unsigned sh;

		/* the unknowns another request of the batch asks for, out, and of
		 * a gap the window still covers only what its repairs will not
		 * bring (the server's harq_one) */
		if (!g.sealed && g.age_us < g.horizon_us && g.deficit > g.future) {
			unsigned want = g.deficit - g.future;
			uint64_t keep = 0, rest = map;

			for (unsigned k = 0; k < want && rest; k++, rest &= rest - 1)
				keep |= rest & (~rest + 1);
			map = keep;
		}
		for (unsigned i = 0; i < hb->nr; i++)
			map &= ~cg_parq_overlap(hb->req[i].base, hb->req[i].map, base);
		if (!map)
			return 0;
		if (hb->nr == sizeof(hb->req) / sizeof(hb->req[0]))
			break; /* no room: the packet */
		sh = (unsigned)__builtin_ctzll(map);
		base += sh;
		map >>= sh;
		hb->req[hb->nr++] = (struct cg_parq_req){ .kind = CG_REP_RLC,
							   .d = (uint8_t)__builtin_popcountll(map),
							   .base = base,
							   .map = map };
		c->dparq_req++;
		c->dparq_want += (uint64_t)__builtin_popcountll(map);
		return 0;
	}
	case CG_HARQ_NACK:
		break;
	}
	/* nothing to ask for again from a server that keeps nothing */
	if (!c->peer_rescue) {
		c->dhq_nonack++;
		if (!*rel)
			cg_ro_refused(&c->rxs.ro, &c->rocf, seq, (uint32_t)now_us);
		return 0;
	}
	hb->nack[retry][hb->nn[retry]++] = seq;
	return 0;
}

static void nack_out(struct client *c, const uint32_t *seq, uint32_t n, int retry, uint64_t now_ms);

/* PARQs for req[0..n) on the two links of least RTT (as the NACKs). */
static void parq_out(struct client *c, const struct cg_parq_req *req, unsigned n, uint64_t now_ms)
{
	uint16_t present, live;
	struct link *best[2] = { NULL, NULL };

	link_masks(c, now_ms, &present, &live);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (!(live >> i & 1) || l->fd < 0 || !l->srtt8_us)
			continue;
		if (!best[0] || l->srtt8_us < best[0]->srtt8_us) {
			best[1] = best[0];
			best[0] = l;
		} else if (!best[1] || l->srtt8_us < best[1]->srtt8_us) {
			best[1] = l;
		}
	}
	for (unsigned i = 0; best[0] && i < n; i += CG_PARQ_MAX) {
		uint8_t pkt[CG_HDR_LEN + CG_PARQ_LEN];
		unsigned k = n - i < CG_PARQ_MAX ? n - i : CG_PARQ_MAX, plen = cg_parq_write(pkt + CG_HDR_LEN, req + i, k);

		for (int b = 0; b < 2 && best[b]; b++) {
			cg_up_header(&c->txs, pkt, CG_T_PARQ, 0, (uint8_t)(best[b] - c->link), (uint32_t)cg_now_us(),
				     pkt + CG_HDR_LEN, plen);
			if (send(best[b]->fd, pkt, CG_HDR_LEN + plen, MSG_DONTWAIT) == (ssize_t)(CG_HDR_LEN + plen))
				c->dparq_msgs++;
		}
	}
}

static void d_harq_flush(struct client *c, struct dharq_batch *hb, uint64_t now_ms)
{
	for (int l = 0; l < 2; l++)
		if (hb->nn[l])
			nack_out(c, hb->nack[l], hb->nn[l], l, now_ms);
	if (hb->nr)
		parq_out(c, hb->req, hb->nr, now_ms);
}

/* The download gaps that wait for the code, again (a repair came, or the
 * reorderer's pass). */
static void d_harq_service(struct client *c, uint64_t now_us)
{
	struct dharq_batch hb;
	uint32_t k = 0;

	if (!c->dhq_n)
		return;
	hb.nn[0] = hb.nn[1] = 0;
	hb.nr = 0;
	for (uint32_t i = 0; i < c->dhq_n; i++)
		if (d_harq_one(c, &hb, c->dhq[i].seq, c->dhq[i].t_us, 0, &c->dhq[i].rel, now_us))
			c->dhq[k++] = c->dhq[i];
	c->dhq_n = k;
	d_harq_flush(c, &hb, now_us / 1000);
}

/* The download gaps the reorderer asked for while FEC v3 protects it:
 * each waits for the code, or asks for its deficit (PARQ), or for the
 * packet (NACK, from a server that keeps it), as the server's send_nacks. */
static void d_send_asks(struct client *c, uint64_t now_ms)
{
	struct cg_reorder *r = &c->rxs.ro;
	uint64_t now_us = cg_now_us();
	struct dharq_batch hb;

	hb.nn[0] = hb.nn[1] = 0;
	hb.nr = 0;
	for (int list = 0; list < 2; list++) {
		const uint32_t *seq = list ? r->ask2 : r->ask;
		uint32_t n = list ? r->nask2 : r->nask;

		for (uint32_t i = 0; i < n; i++) {
			uint8_t rel = 0;

			if (!d_harq_one(c, &hb, seq[i], (uint32_t)now_us, list, &rel, now_us))
				continue;
			if (list) { /* asked again while it waits: it waits once */
				uint32_t j = 0;

				while (j < c->dhq_n && c->dhq[j].seq != seq[i])
					j++;
				if (j < c->dhq_n)
					continue;
			}
			if (c->dhq_n == CG_DHQ_N) {
				c->dhq_full++;
				if (c->peer_rescue)
					hb.nack[list][hb.nn[list]++] = seq[i];
				continue;
			}
			c->dhq[c->dhq_n].seq = seq[i];
			c->dhq[c->dhq_n].t_us = (uint32_t)now_us;
			c->dhq[c->dhq_n].rel = rel;
			c->dhq_n++;
			c->dhq_waited++;
		}
	}
	r->nask = r->nask2 = 0;
	d_harq_flush(c, &hb, now_ms);
}

/* A probe reply moved what the reorderer knows: a link's delay, and rset
 * (the server's active mask). */
static void ro_reply(struct client *c, uint16_t *rset, uint32_t now, uint64_t now_ms)
{
	if (!c->cfg->reorder)
		return;
	*rset = ro_rset(c, now_ms, now);
	ro_drain(c, *rset, now, 0, now_ms);
}

/* The clock a gap's deadline is judged by: now, or the read time of the
 * oldest slot a pump holds that this loop has not taken yet, if earlier.
 * A packet waiting there arrived before now; judged by now, its gap would
 * be given up and the packet would come late a moment after (story 013:
 * half of the Pi's lost.cap came less than 1 ms late). By arrival times, a
 * gap given up at the clock is one whose packet had not come by then. */
static uint64_t ro_clock(struct client *c, uint64_t now_us)
{
	for (int k = 0; k < c->npumps; k++) {
		struct cg_pump *p = c->pump[k];

		if (cg_pump_rx_avail(p)) {
			uint64_t t = cg_pump_rx_slot(p, 0)->t_us;

			if (t < now_us)
				now_us = t;
		}
	}
	return now_us;
}

/* What became due while nothing arrived (a deadline, or the rest of a
 * drain the budget cut): after every loop pass, and every tick. */
static void ro_service(struct client *c)
{
	uint64_t now_us, now_ms;

	if (c->dhq_n)
		d_harq_service(c, cg_now_us()); /* gaps waiting for FEC v3's code */
	if (!c->cfg->reorder || !c->rxs.ro.held)
		return;
	now_us = cg_now_us();
	now_ms = now_us / 1000;
	{
		uint32_t clock = (uint32_t)ro_clock(c, now_us);

		ro_drain(c, ro_rset(c, now_ms, clock), clock, 0, now_ms);
	}
	dout_flush(c, now_ms);
}

/* The epoll timeout the reorderer needs: -1 when it holds nothing, 0 when
 * something may go now, else the ms until its next deadline, ceiled. Asked
 * again on every pass: a probe reply can bring a deadline forward. */
static int ro_timeout(struct client *c)
{
	uint64_t now_us;
	uint16_t rset;
	int32_t due, act;

	if (!c->cfg->reorder || !c->rxs.ro.held)
		return -1;
	now_us = cg_now_us();
	rset = ro_rset(c, now_us / 1000, (uint32_t)now_us);
	due = cg_ro_due_us(&c->rxs.ro, &c->rocf, rset, (uint32_t)now_us);
	/* a link leaving rset may bring a deadline forward */
	act = ro_act_next(c, now_us / 1000, (uint32_t)now_us);
	return cg_due_ms(act < due ? act : due);
}

/* Everything held, in order, and the window starts again with the next
 * packet: reorder turned off by a reload. */
static void ro_flush(struct client *c, uint64_t now_ms)
{
	struct cg_ro_out o;
	uint32_t held = c->rxs.ro.held;

	while (cg_ro_flush(&c->rxs.ro, &o))
		dout_add(c, cg_ro_pool_buf(&c->pool, o.buf), o.len, o.buf, now_ms, o.seq, o.flags);
	dout_flush(c, now_ms);
	if (held)
		cg_info("reorder off: %u held packets handed on to the TUN", held);
}

/* Today's loop (link_threads = legacy): reads a link's socket into the
 * client's batch, judges each datagram (clientpath.h) and sends the first
 * copies to the TUN, one output batch per socket batch (more when held
 * packets released make it fuller). */
static void link_read(struct client *c, struct link *l)
{
	unsigned id = (unsigned)(l - c->link);

	for (int round = 0; round < CG_MAX_ROUNDS; round++) {
		int n = cg_rx(l->fd, &c->in);
		uint64_t now_us, now_ms;
		uint32_t now32;
		uint16_t expect, rset;

		if (n < 0 && (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH))
			continue; /* an ICMP error from an earlier send; the queue may hold more */
		if (n <= 0)
			return;
		now_us = cg_now_us();
		now_ms = now_us / 1000;
		now32 = (uint32_t)now_us;
		expect = expect_mask(c);
		rset = c->cfg->reorder ? ro_rset(c, now_ms, now32) : 0;
		for (int i = 0; i < n; i++) {
			uint8_t *b = c->in.buf[i];
			size_t len = c->in.msg[i].msg_len;
			struct cg_hdr h;
			enum cg_rxv v = cg_rx_entry(&c->rxs, &l->rxl, id, l->rxl.gen, b, len,
						    !!(c->in.msg[i].msg_hdr.msg_flags & MSG_TRUNC), now_us, expect, &h);

			if (v == CG_RXV_DROP)
				continue;
			if (v != CG_RXV_DATA) {
				rx_verdict_log(c, l, v, now_ms);
				if (v == CG_RXV_REPLY) {
					on_probe_reply(c, l, &h, b + CG_HDR_LEN, len - CG_HDR_LEN, now32, now_ms);
					expect = expect_mask(c);
					ro_reply(c, &rset, now32, now_ms);
				} else if (v == CG_RXV_HELLO) {
					on_hello(c, l, now_us);
				} else if (v == CG_RXV_NACK) {
					on_nack(c, h.flags, b + CG_HDR_LEN, len - CG_HDR_LEN, now_ms);
				} else if (v == CG_RXV_PARQ) {
					on_parq(c, b + CG_HDR_LEN, len - CG_HDR_LEN, now_ms);
				} else if (v == CG_RXV_REPAIR) {
					d_rep_take(c, b + CG_HDR_LEN, (unsigned)(len - CG_HDR_LEN), (uint8_t)id, h.ts, rset, now32,
						   now_ms);
				}
				continue;
			}
			if (c->dfec) /* kept for the server's repairs (story 026) */
				cg_fec_rx_put(c->dfec, h.seq, b + CG_HDR_LEN, (uint16_t)(len - CG_HDR_LEN));
			down_deliver(c, &h, id, b + CG_HDR_LEN, len - CG_HDR_LEN, now32, rset, now32, now_ms);
			if (c->dfec)
				d_rep_data(c, h.seq, (uint8_t)id, h.ts, rset, now32, now_ms);
		}
		dout_flush(c, now_ms);
		if (n < CG_BATCH)
			return;
	}
}

/* One slot a pump read, judged; a first copy goes on (down_deliver). now:
 * the pass's clock, for what the reorderer releases. */
/* clock_us: what the reorderer judges deadlines by (ro_clock). */
static void hub_slot(struct client *c, struct cg_rxslot *sl, uint16_t *expect, uint16_t *rset, uint64_t clock_us)
{
	struct link *l = &c->link[sl->link];
	uint64_t now_ms = sl->t_us / 1000;
	struct cg_hdr h;
	enum cg_rxv v = cg_rx_entry(&c->rxs, &l->rxl, sl->link, sl->gen, sl->buf, sl->len, sl->trunc, sl->t_us,
				    *expect, &h);

	if (v == CG_RXV_DATA) {
		if (c->dfec) /* kept for the server's repairs (story 026) */
			cg_fec_rx_put(c->dfec, h.seq, sl->buf + CG_HDR_LEN, (uint16_t)(sl->len - CG_HDR_LEN));
		down_deliver(c, &h, sl->link, sl->buf + CG_HDR_LEN, sl->len - CG_HDR_LEN, (uint32_t)sl->t_us, *rset,
			     (uint32_t)clock_us, clock_us / 1000);
		if (c->dfec)
			d_rep_data(c, h.seq, (uint8_t)sl->link, h.ts, *rset, (uint32_t)clock_us, clock_us / 1000);
		return;
	}
	if (v == CG_RXV_DROP)
		return;
	rx_verdict_log(c, l, v, now_ms);
	if (v == CG_RXV_REPLY) {
		on_probe_reply(c, l, &h, sl->buf + CG_HDR_LEN, (size_t)sl->len - CG_HDR_LEN, (uint32_t)sl->t_us, now_ms);
		*expect = expect_mask(c);
		ro_reply(c, rset, (uint32_t)clock_us, clock_us / 1000);
	} else if (v == CG_RXV_HELLO) {
		on_hello(c, l, cg_now_us());
	} else if (v == CG_RXV_NACK) {
		on_nack(c, h.flags, sl->buf + CG_HDR_LEN, (size_t)sl->len - CG_HDR_LEN, now_ms);
	} else if (v == CG_RXV_PARQ) {
		on_parq(c, sl->buf + CG_HDR_LEN, (size_t)sl->len - CG_HDR_LEN, now_ms);
	} else if (v == CG_RXV_REPAIR) {
		d_rep_take(c, sl->buf + CG_HDR_LEN, (unsigned)(sl->len - CG_HDR_LEN), (uint8_t)sl->link, h.ts, *rset,
			   (uint32_t)clock_us, clock_us / 1000);
	}
}

/* Takes what the pumps read (link_threads off and on): each pump in turn,
 * starting with a different one on every pass, up to CG_BATCH slots each.
 * Judges each slot in the order of today's loop (clientpath.h), with the
 * time its pump read it, writes the first copies of the whole pass to
 * the TUN (one output batch, more with held packets released) straight out
 * of the slots, and only then gives the slots back. Returns the slots
 * taken. */
static int hub_drain(struct client *c)
{
	uint32_t took[CG_MAX_PUMPS];
	uint64_t now_us = cg_now_us(), clock = ro_clock(c, now_us);
	uint16_t expect = expect_mask(c), rset = c->cfg->reorder ? ro_rset(c, now_us / 1000, (uint32_t)clock) : 0;
	int total = 0;

	for (int k = 0; k < c->npumps; k++) {
		int pi = (int)((c->drain_first + (unsigned)k) % (unsigned)c->npumps);
		struct cg_pump *p = c->pump[pi];
		uint32_t n = cg_pump_rx_avail(p);
		uint64_t stamp = 0;

		if (n > CG_BATCH)
			n = CG_BATCH;
		for (uint32_t i = 0; i < n; i++) {
			struct cg_rxslot *sl = cg_pump_rx_slot(p, i);

			/* Hop stamps: once per batch the pump read. */
			if (sl->t_us != stamp) {
				stamp = sl->t_us;
				cg_hist_add(&c->hop[pi], now_us > stamp ? (uint32_t)(now_us - stamp) : 0);
			}
			hub_slot(c, sl, &expect, &rset, clock);
		}
		took[pi] = n;
		total += (int)n;
	}
	c->drain_first++;
	dout_flush(c, now_us / 1000);
	for (int pi = 0; total && pi < c->npumps; pi++)
		if (took[pi])
			cg_pump_rx_release(c->pump[pi], took[pi]);
	return total;
}

/* link_threads = off: the one pump reads the link that is ready, and this
 * loop takes the batch at once, as today's loop does. */
static void inline_read(struct client *c, unsigned idx)
{
	struct cg_pump *p = c->pump[0];

	for (int round = 0; round < CG_MAX_ROUNDS && p->fd[idx] >= 0; round++) {
		int n = cg_pump_rx(p, idx, 1);

		hub_drain(c);
		if (n < CG_BATCH)
			return;
	}
}

static void send_probe(struct client *c, struct link *l, uint64_t now_us, uint32_t interval)
{
	unsigned id = (unsigned)(l - c->link);
	uint64_t now_ms = now_us / 1000;
	uint8_t pkt[CG_HDR_LEN + CG_PROBE_ACT_LEN];
	/* The links the upload went on within the hold before now (active.h). */
	struct cg_probe_act pa = { cg_act_tx_mask(&c->act_tx, now_us, CG_ACT_HOLD_MS * 1000), CG_ACT_HOLD_MS };
	/* The bonding tail (proto.h): this link's weight, whether this end
	 * reorders, and with split = on the request for a split download. */
	struct cg_probe_bond pb = cg_split_probe_tail(c->cfg->split, c->cfg->reorder, c->lw[id]);
	/* The upload's rescue: it keeps what it sends; but not what this link
	 * loses now (full, or out of budget). */
	pb.rescue = c->resc_idx != NULL;
	pb.full = pb.rescue && (cg_resc_low(&c->resc_b) || up_full(c, id));
	/* Its type, so that the server treats its download alike (story 015). */
	pb.type = c->ltype[id];
	/* Its TCP ACKs marked when the download did not deliver their holes (tcpack.h). */
	pb.tcpmark = c->cfg->tcp_aware;
	const struct cg_link_rx *rx = &c->rxs.rx[id];
	/* The cookie, and where our windows stand, for a server that has to
	 * take the session up (proto.h). */
	struct cg_probe_info pi = { .echo_ts = 0,
				    .owd = l->have_down_owd ? l->down_owd : 0,
				    .interval_ms = interval,
				    .rx = (uint32_t)(rx->wins + rx->dups),
				    .wins = (uint32_t)rx->wins,
				    .lag_us = cg_lag_us(rx),
				    .cookie = l->rxl.cookie,
				    .rx_top = cg_rx_top(&c->rxs.replay),
				    .rx_top_ctl = cg_rx_top(&c->rxs.ctl),
				    .tx_next = c->txs.seq };
	uint8_t flags = (uint8_t)((l->have_down_owd ? CG_F_OWD : 0) | (c->uh[id].state == CG_H_MUTED ? CG_F_MUTED : 0) |
				  cg_pass_flags(c->cfg->passthrough));

	cg_probe_info_write(pkt + CG_HDR_LEN, &pi);
	cg_probe_bond_write(pkt + CG_HDR_LEN, &pb);
	cg_probe_act_write(pkt + CG_HDR_LEN, &pa);
	cg_up_header(&c->txs, pkt, CG_T_PROBE, flags, (uint8_t)id, (uint32_t)now_us, pkt + CG_HDR_LEN, CG_PROBE_ACT_LEN);
	/* A full queue drops the probe like any other packet: that is a measurement, not an error. */
	if (send(l->fd, pkt, sizeof(pkt), MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ENOBUFS &&
	    cg_ratelimit_ok(&l->rl, now_ms, 10000))
		cg_warn("link %s probe: %s", l->ifname, strerror(errno));
	cg_echo_push(&l->rxl.probes, (uint32_t)now_us, now_ms);
	l->have_down_owd = 0;
	l->last_probe_ms = now_ms;
	l->probe_before = l->probe_announced;
	l->probe_announced = interval;
	if (!l->unanswered++)
		l->first_unanswered_ms = now_ms;
}

/* Guard G4 (split.h), after health: which candidates of the upload split
 * are within split_spread_ms of the fastest. Logs a link that leaves or
 * comes back. */
static void spread_tick(struct client *c, uint16_t present, uint16_t live, uint64_t now_ms)
{
	uint16_t carry, dup0, cand, changed, stale;
	uint8_t before[CG_MAX_LINKS];

	carry = cg_health_carriers(c->uh, CG_MAX_LINKS, present, live);
	split_weights(c, &dup0);
	cand = (uint16_t)(split_eligible_g3(c, live, carry, now_ms, &stale) & ~dup0);
	/* G3, the copies' budget and the rescue's, as decisions (decision.h) */
	for (int i = 0; i < CG_MAX_LINKS; i++)
		cg_dec_edge(&c->dec, &c->dec_stale[i], stale >> i & 1, now_ms, (unsigned)i, CG_DEC_STALE, CG_DEC_FRESH,
			    c->link[i].last_reply_ms ? (int64_t)(now_ms - c->link[i].last_reply_ms) : -1);
	cg_dec_edge(&c->dec, &c->dec_dry, c->sdup.dry, now_ms, CG_DEC_NOLINK, CG_DEC_COPIES_DRY, CG_DEC_COPIES_OK,
		    (int64_t)c->sdup.tokens);
	if (c->resc_idx)
		cg_dec_edge(&c->dec, &c->dec_rfull, cg_resc_low(&c->resc_b), now_ms, CG_DEC_NOLINK,
			    CG_DEC_RESCUE_FULL_ON, CG_DEC_RESCUE_FULL_OFF, (int64_t)c->resc_b.credit);
	cg_decrec_g4_before(&c->spread, before);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		c->spread.dwell_ms[i] = c->lm[i].dwell_ms;
		c->spread.dwell_oq_us[i] = (int32_t)c->lm[i].dwell_oq_ms * 1000;
	}
	changed = cg_split_spread_eval_r(&c->spread, c->uh, cand, c->cfg->split_spread_ms * 1000, c->cfg->mute_settle_ms,
					 now_ms, c->cfg->split_stress_rejoin);
	cg_decrec_g4(&c->dec, &c->spread, changed, before, &c->dec_stress, &c->dec_recover, now_ms);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((changed >> i & 1) && c->spread.ev[i] == CG_G4_LEAVE_QUEUE)
			cg_cap_episode(&c->cap[i], now_ms, c->lm[i].g4_episode); /* audit C5 */
	for (int i = 0; changed; i++, changed >>= 1) {
		const char *name = c->link[i].ifname;
		int32_t ms = c->spread.behind_us[i] / 1000;

		if (!(changed & 1))
			continue;
		if (c->spread.left >> i & 1)
			cg_info("link %s: leaves the upload split, %d ms behind the fastest link (split_spread_ms %u)", name,
				ms, c->cfg->split_spread_ms);
		else if (!c->cfg->split_spread_ms)
			cg_info("link %s: back in the upload split, split_spread_ms is 0", name);
		else
			cg_info("link %s: back in the upload split, within %d ms of the fastest link", name, ms > 0 ? ms : 0);
	}
}

/* The governor (govern.h), after health: the upload's weights from each
 * link's queue, while the upload is split and carries traffic. */
static void gov_tick(struct client *c, uint16_t present, uint16_t live, uint64_t now_ms)
{
	struct cg_gov_cfg gc = { .target_us = c->cfg->governor_target_ms * 1000, .window_ms = c->cfg->governor_window_ms };
	uint16_t carry, dup0, cand, w0[CG_MAX_LINKS];
	int active;

	if (!c->cfg->governor)
		return;
	carry = cg_health_carriers(c->uh, CG_MAX_LINKS, present, live);
	split_weights(c, &dup0);
	cand = (uint16_t)(split_eligible(c, live, carry, now_ms) & ~dup0);
	active = split_active(c) && c->last_traffic_ms && now_ms - c->last_traffic_ms < 1000;
	cg_decrec_gov_before(&c->gov, w0);
	cg_decrec_gov(&c->dec, &c->gov, cg_gov_tick(&c->gov, c->uh, cand, active, now_ms, &gc), w0, now_ms);
	cg_gov_weights(&c->gov, dup0, c->gw);
}

/* SO_MAX_PACING_RATE on a link's socket (pace.h): fq on its way out paces
 * it. Kernels that take only 32 bits get it capped. */
static void pace_set(struct link *l, uint64_t rate)
{
	uint32_t r32 = rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;

	if (setsockopt(l->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate)) < 0 &&
	    setsockopt(l->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &r32, sizeof(r32)) < 0)
		return;
	l->paced = rate;
}

/* Each link's upload capacity (capacity.h), from what the server says it
 * received on it, and with capacity_weights the split's weights from them
 * (scaled by the governor's, with governor), after the governor's tick.
 * Each link with its own model (c->lm, link_models). */
static void cap_tick(struct client *c, uint16_t live, uint16_t in, uint64_t now_ms)
{
	struct cg_cap_cfg cc = { .window_ms = c->cfg->governor_window_ms, .sat_us = c->cfg->capacity_sat_ms * 1000 };
	struct cg_slot slot = cg_config_slot(c->cfg);
	uint64_t wall = cg_wall_ms();

	cc.seq_lag = (uint8_t)c->cfg->capacity_seq_lag;
	cc.tx_seq = c->txs.seq;
	cc.expire_ms = c->cfg->capacity_expire_ms;
	cc.lossq_us = c->cfg->capacity_loss_queue_ms * 1000;
	cc.stand = (uint8_t)c->cfg->capacity_stand;
	cc.real_off_ms = (int64_t)wall - (int64_t)now_ms;

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		if (l->fd >= 0) {
			const struct cg_link_model *m = &c->lm[i];
			uint64_t was = cg_cap_bps(&c->cap[i]);
			unsigned ev;

			cc.loss_pct = m->loss_pct;
			cc.lossy = cg_link_tracks_loss(m);
			cc.lossq_loaded = (uint8_t)cc.lossy; /* Starlink's preset: its loss without a queue is held, not cut */
			cc.lossy_probe = !m->lossy_noprobe;
			cc.rfull_mode = m->rfull_hyst;
			cc.rx_top = l->peer_view.rx_top;
			cc.behind_us = c->uh[i].have_behind ? c->uh[i].behind_us : 0;
			cc.slot = m->slot_clock && wall >= CG_WALL_SET_MS ? &slot : NULL;
			cc.out = !(in >> i & 1);
			ev = cg_cap_tick(&c->cap[i], now_ms, l->peer_view_ms, l->peer_view.rx, l->tx_pkts, l->tx_bytes,
					 &c->uh[i], &cc);
			cg_sig_cap(&c->sig[i], &c->cap[i], now_ms);
			/* FEC v3: where its repairs go, and a lone link's loss */
			cg_f3_weigh(&c->f3, (unsigned)i, l->tx_bytes);
			cg_f3_loss(&c->f3, (unsigned)i, c->cap[i].loss_pm, c->cap[i].loss_pm_ms, c->cap[i].ql_us,
				   c->sig[i].last_qmax > c->cap[i].ql_us ? c->sig[i].last_qmax : c->cap[i].ql_us,
				   c->cap[i].loaded_ms == c->cap[i].loss_pm_ms);

			cg_decrec_cap(&c->dec, &c->cap[i], (unsigned)i, ev, was, now_ms);
		}
	}
	cg_f3_mix_tick(&c->f3);
	if (c->cfg->capacity_weights)
		cg_cap_weights(c->cap, c->cfg->split == CG_SPLIT_FORCE ? 0 : c->dup0, c->cfg->governor ? c->gw : NULL,
			       cc.sat_us / 2, c->cw);
	cg_cap_total_tick(&c->ctot, c->cap, live, cc.sat_us);
	cg_dom_tick(&c->dom, c->cap, c->lm, live, now_ms);
	if (c->cfg->split_eta) {
		uint64_t tb[CG_MAX_LINKS], tc[CG_MAX_LINKS];
		uint16_t due;

		c->eta.tick_ms = CG_TICK_MS;
		cg_cap_eta(&c->eta, c->cap, live, cc.sat_us / 2, c->lm, c->cfg->split_stale_max,
			   cg_tickle_backoff(&c->tk), now_ms * 1000);
		cg_cap_tickle(c->cap, live, now_ms, &due, tb, tc);
		cg_tickle_tick(&c->tk, now_ms * 1000, (uint64_t)c->cfg->split_tickle_ms * 1000, due, tb, tc);
	}
	if (c->pace_any) /* the copies' expected arrivals (redund_pace_ms, per link) */
		cg_cap_eta(&c->pace, c->cap, live, cc.sat_us / 2, NULL, c->cfg->split_stale_max,
			   cg_tickle_backoff(&c->tk), now_ms * 1000);
	if (c->cfg->redund_spare) {
		uint16_t tpresent, tlive;

		/* the links the data goes on (link_masks_tx), for its first */
		link_masks_tx(c, now_ms, &tpresent, &tlive);
		spare_tick(c, tlive, now_ms);
	}
	/* With pacing, each link's socket at about its rate; without, as it
	 * was set, back to none (a reload turned it off). */
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		uint64_t r = c->cfg->pacing ? cg_pace_rate(c->cap[i].have_rate ? cg_cap_bps(&c->cap[i]) : 0)
					    : CG_PACE_NONE;

		if (l->fd >= 0 && cg_pace_changed(l->paced, r))
			pace_set(l, r);
	}
}

/* Each link's model (linkmodel.h): its type's preset, with the global
 * knobs on top (eta_buffer, capacity_loss_pct, the clockwork audit's
 * experiments, exp_link_types); the loops of a tick read it. */
static void link_models(struct client *c)
{
	struct cg_link_knobs k = cg_config_link_knobs(c->cfg);

	cg_link_models(c->ltype, CG_MAX_LINKS, &k, c->lm);
	c->pace_any = (uint8_t)cg_link_pace_windows(c->lm, CG_MAX_LINKS, c->pace_w);
	/* redund_spare decides the copies on the same expected arrivals */
	c->pace_any |= c->cfg->redund_spare != 0;
}

/* The links that carry the upload now, for the capacity's expiry
 * (capacity_expire_ms): live and carrying (not muted), and when the upload
 * is split, in the split or taking a copy of every packet. */
static uint16_t send_set(const struct client *c, uint16_t present, uint16_t live, uint64_t now_ms)
{
	uint16_t carry = (uint16_t)(cg_health_carriers(c->uh, CG_MAX_LINKS, present, live) & live), dup0;
	struct cg_split_masks k;

	if (!split_active(c))
		return carry;
	split_weights(c, &dup0);
	k = split_masks(c, carry, split_eligible(c, live, carry, now_ms), dup0);
	return (uint16_t)((k.eligible | k.dup) & carry);
}

/* mute_queue's inputs (health.h): each link's queue over its minute's
 * minimum (capacity.h, the last tick's) and the bound from its unanswered
 * probes over the fastest live link's round trip. */
static void health_inputs(struct client *c, uint16_t live, uint64_t now_ms)
{
	uint32_t ref = 0;
	int any = 0;

	for (int i = 0; i < CG_MAX_LINKS; i++)
		if ((live >> i & 1) && c->link[i].srtt8_us && (!any || srtt_ms(&c->link[i]) < ref)) {
			ref = srtt_ms(&c->link[i]);
			any = 1;
		}
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];

		c->uh[i].queue_us = l->fd >= 0 ? c->cap[i].ql_us : -1;
		c->uh[i].silent_us = l->fd >= 0 && any ? cg_health_silent_us(l->unanswered, l->first_unanswered_ms, now_ms, ref) : 0;
	}
}

static void health_tick(struct client *c, uint64_t now_ms)
{
	uint16_t present, live, changed;

	link_models(c);
	link_masks(c, now_ms, &present, &live);
	health_inputs(c, live, now_ms);
	changed = cg_health_eval(c->uh, CG_MAX_LINKS, live, now_ms, &c->hcfg);
	cg_decrec_health(&c->dec, c->uh, changed, now_ms);
	spread_tick(c, present, live, now_ms);
	gov_tick(c, present, live, now_ms);
	cap_tick(c, live, send_set(c, present, live, now_ms), now_ms);
	for (int i = 0; changed; i++, changed >>= 1) {
		const struct cg_hlink *h = &c->uh[i];

		if (!(changed & 1))
			continue;
		if (h->state == CG_H_MUTED)
			cg_info("link %s: upload muted, %d ms behind the fastest link", c->link[i].ifname,
				(h->have_late ? h->late_us : h->behind_us) / 1000);
		else if (h->unmuted_ms == now_ms)
			cg_info("link %s: upload unmuted, within %d ms of the fastest link", c->link[i].ifname,
				h->behind_us > 0 ? h->behind_us / 1000 : 0);
		else if (!c->hcfg.mute_behind_us)
			cg_info("link %s: upload unmuted, muting is off", c->link[i].ifname);
		else
			cg_info("link %s: upload unmuted, too few active links", c->link[i].ifname);
	}
}

/* -2 (no reply yet) and, unless none is set, -1 give null; -1: "none". */
static void json_pass(struct cg_json *j, const char *key, int pass, int none)
{
	if (pass < -1 || (pass < 0 && !none))
		cg_json_null(j, key);
	else
		cg_json_str(j, key, pass < 0 ? "none" : pass ? "on" : "off");
}

/* Tenths of a percent as a percentage with 1 decimal; null below 0. */
static void json_pct(struct cg_json *j, const char *key, int permille)
{
	if (permille < 0)
		cg_json_null(j, key);
	else
		cg_json_ms(j, key, (uint64_t)permille * 100); /* "12.300" for 123 */
}

/* A link's pump and its socket: which pump, the kernel's drops, a stalled
 * thread, its CPU, and the hop of its datagrams to this loop (p50 and p99
 * over the last 5 s; up: PR 3c). */
static void link_threads_json(struct client *c, const struct link *l, struct cg_json *j)
{
	const struct pump_stats *ps = l->has_pump ? &c->pst[l->pump] : NULL;
	uint32_t rmem, drops, p50, p99;

	if (ps)
		cg_json_u64(j, "pump", l->pump);
	else
		cg_json_null(j, "pump");
	if (sock_meminfo(l->fd, &rmem, &drops) == 0)
		cg_json_u64(j, "socket_drops", drops);
	else
		cg_json_null(j, "socket_drops");
	cg_json_u64(j, "open_failed", l->open_failed);
	cg_json_u64(j, "rx_paused", ps ? ps->paused : 0);
	cg_json_u64(j, "io_stalled_ms", ps && c->lt == CG_LT_ON ? ps->stalled_ms : 0);
	json_pct(j, "pump_cpu_pct", ps && c->lt == CG_LT_ON ? cg_cpuwin_permille(&ps->cpu) : -1);
	cg_json_obj(j, "hop_us");
	if (ps && cg_hist_pct(ps->hop_win, 50, &p50) && cg_hist_pct(ps->hop_win, 99, &p99)) {
		cg_json_obj(j, "down");
		cg_json_u64(j, "p50", p50);
		cg_json_u64(j, "p99", p99);
		cg_json_end(j, '}');
	} else {
		cg_json_null(j, "down");
	}
	cg_json_null(j, "up");
	cg_json_end(j, '}');
}

/* The newest HELLO on l refused the router, and it is recent: no reply came
 * after it, and it is at most 3 idle probe intervals (and a second) old. */
static int link_refused(const struct client *c, const struct link *l, uint64_t now_ms)
{
	return l->rxl.refused && l->rxl.hello_ms >= l->last_reply_ms &&
	       now_ms - l->rxl.hello_ms <= 3ull * c->cfg->probe_idle_ms + 1000;
}

/* The server refuses the router on some link: its session limit. */
static int vps_refusing(const struct client *c, uint64_t now_ms)
{
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].used && link_refused(c, &c->link[i], now_ms))
			return 1;
	return 0;
}

static void status_json(struct client *c, uint64_t now_ms, struct cg_json *j)
{
	uint16_t rset = ro_rset(c, now_ms, (uint32_t)cg_now_us()), present, live, s_elig, s_dup0;
	uint32_t spread_us = c->cfg->split_spread_ms * 1000;
	char buf[64];

	/* The upload split's eligible links, as a batch now would take them. */
	link_masks(c, now_ms, &present, &live);
	s_elig = split_eligible(c, live, cg_health_carriers(c->uh, CG_MAX_LINKS, present, live), now_ms);
	split_weights(c, &s_dup0);

	cg_json_obj(j, NULL);
	cg_json_str(j, "mode", "client");
	cg_json_str(j, "version", CG_VERSION);
	cg_json_str(j, "description", c->cfg->description);
	cg_json_u64(j, "uptime_ms", now_ms - c->start_ms);
	cg_json_u64(j, "ticks_missed", c->ticks_missed); /* the tick ran late (clockwork audit) */
	cg_json_u64(j, "tick_max_us", c->tick_max_us);
	cg_json_u64(j, "time_ms", cg_wall_ms());
	snprintf(buf, sizeof(buf), "%08x", c->session);
	cg_json_str(j, "session", buf);
	cg_json_str(j, "tunnel", "native");
	cg_json_str(j, "tun", c->cfg->tun);
	cg_json_u64(j, "probe_interval_ms", probe_interval(c, now_ms));
	cg_json_bool(j, "vps_refusing", vps_refusing(c, now_ms));
	cg_json_str(j, "config_error", c->config_error);
	cg_json_obj(j, "threads");
	cg_json_str(j, "setting", cg_lt_name(c->cfg->link_threads));
	cg_json_str(j, "mode", cg_lt_name(c->lt));
	cg_json_u64(j, "pumps", (uint64_t)c->npumps);
	json_pct(j, "hub_cpu_pct", cg_cpuwin_permille(&c->hub_cpu));
	/* The guards of thrplan.h that fired, as logged (guards_check): the
	 * knobs only change with a restart, so they hold for the process. */
	cg_json_arr(j, "warnings");
	for (unsigned b = 1; b <= CG_TG_PIN_SHARED; b <<= 1)
		if (c->guards & b)
			cg_json_str(j, NULL, cg_thr_guard_name(b));
	cg_json_end(j, ']');
	cg_json_end(j, '}');
	cg_json_obj(j, "passthrough");
	json_pass(j, "requested", c->cfg->passthrough, 0);
	/* "none": the server does not apply IP pass (no passthrough_file). */
	json_pass(j, "server", c->server_pass, 1);
	cg_json_end(j, '}');
	cg_json_obj(j, "upload");
	cg_json_u64(j, "packets", c->up_pkts);
	cg_json_u64(j, "bytes", c->up_bytes);
	cg_json_u64(j, "too_big", c->up_toobig);
	cg_json_u64(j, "largest", c->up_largest);
	/* tcp_aware (tcpfree.h): segments marked free, TCP retransmissions
	 * (and of them sent again), first ones on one link in redundancy and
	 * those copied (story 023) */
	cg_json_obj(j, "tcp");
	cg_json_u64(j, "free", c->tcpf.ctl);
	cg_json_u64(j, "retx", c->tcpf.retx);
	cg_json_u64(j, "retx_again", c->tcpf.retx_again);
	cg_json_u64(j, "retx_one", c->retx_one);
	cg_json_u64(j, "retx_copied", c->retx_copied);
	cg_json_tcpa(j, "rule", &c->tcpa, c->retx_row, c->retx_row_copied, c->peer_tcpmark);
	cg_json_end(j, '}');
	/* Splitting the upload (split.h). */
	cg_json_u64(j, "split", c->up_split);
	cg_json_str(j, "split_state", split_state(c, now_ms));
	cg_json_u64(j, "split_spread_ms", c->cfg->split_spread_ms);
	/* Copies under G4's stress (cg_sdup): the budget, now, and spent. */
	cg_json_obj(j, "stress_dup");
	cg_json_u64(j, "pct", c->cfg->split_stress_dup);
	cg_json_bool(j, "stress", c->spread.stress);
	cg_json_bool(j, "recover", c->spread.recover);
	cg_json_u64(j, "queue_leaves", c->spread.n_qleave);
	cg_json_u64(j, "recoveries", c->spread.n_recover);
	cg_json_u64(j, "stalls", c->spread.n_stall);
	cg_json_u64(j, "dwells", c->spread.n_dwell);
	cg_json_u64(j, "packets", c->sdup.pkts);
	cg_json_u64(j, "bytes", c->sdup.bytes);
	cg_json_end(j, '}');
	cg_json_bool(j, "split_spread_copy", c->cfg->split_spread_copy);
	cg_json_bool(j, "governor", c->cfg->governor);
	cg_json_bool(j, "capacity_weights", c->cfg->capacity_weights);
	cg_json_bool(j, "split_eta", c->cfg->split_eta);
	/* what decided each split packet so far (split.h, enum cg_split_by) */
	cg_json_obj(j, "split_by");
	cg_json_u64(j, "eta", c->sp.n_by[CG_SPLIT_BY_ETA]);
	cg_json_u64(j, "wrr", c->sp.n_by[CG_SPLIT_BY_WRR]);
	cg_json_u64(j, "g2", c->sp.n_by[CG_SPLIT_BY_G2]);
	cg_json_u64(j, "all", c->sp.n_by[CG_SPLIT_BY_ALL]);
	cg_json_u64(j, "tickle", c->sp.n_by[CG_SPLIT_BY_TICKLE]);
	cg_json_end(j, '}');
	/* which step set a split packet's links last (cg_split_plan, audit C13) */
	cg_json_obj(j, "split_plan");
	cg_json_u64(j, "split", c->plan_n[CG_PLAN_SPLIT]);
	cg_json_u64(j, "single", c->plan_n[CG_PLAN_SINGLE]);
	cg_json_u64(j, "share", c->plan_n[CG_PLAN_SHARE]);
	cg_json_u64(j, "copy", c->plan_n[CG_PLAN_COPY]);
	cg_json_end(j, '}');
	/* What picks the link of a split packet now (decrec.h): with "eta" the
	 * gov_weight and cap_weight of the links are not used. */
	if (split_active(c))
		cg_json_str(j, "weights_used",
			    cg_weights_used(c->cfg->split_eta, &c->eta, (uint16_t)(c->spread.in & s_elig & ~s_dup0),
					    c->cfg->capacity_weights, c->cfg->governor));
	else
		cg_json_null(j, "weights_used");
	/* G4's fastest link in the split (null: none) and the governor's
	 * shortest queue. */
	if (c->spread.lead >= 0)
		cg_json_u64(j, "lead", (uint64_t)c->spread.lead);
	else
		cg_json_null(j, "lead");
	if (c->cfg->governor && c->gov.qmin_us >= 0)
		cg_json_ms(j, "gov_qmin_ms", (uint64_t)c->gov.qmin_us);
	else
		cg_json_null(j, "gov_qmin_ms");
	cg_json_bool(j, "copies_dry", c->sdup.dry);
	cg_json_decisions(j, &c->dec, now_ms);
	/* Idle links tickled (tickle.h): how many times, the packets, the one now. */
	cg_json_obj(j, "tickle");
	cg_json_u64(j, "every_ms", c->cfg->split_eta ? c->cfg->split_tickle_ms : 0);
	cg_json_u64(j, "count", c->tk.count);
	cg_json_u64(j, "packets", c->tk.pkts);
	cg_json_u64(j, "grew", c->tk.grew);
	cg_json_arr(j, "backoff"); /* each link's doublings of every_ms */
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].fd >= 0)
			cg_json_u64(j, NULL, c->tk.shift[i]);
	cg_json_end(j, ']');
	if (c->tk.link >= 0 && c->tk.count)
		cg_json_u64(j, "link", (uint64_t)c->tk.link);
	else
		cg_json_null(j, "link");
	cg_json_end(j, '}');
	cg_json_dominant(j, &c->dom);
	cg_json_bool(j, "pacing", c->cfg->pacing);
	/* tcp_aware: inner TCP marked free (tcpfree.h) */
	cg_json_obj(j, "tcp_free");
	cg_json_bool(j, "on", c->cfg->tcp_aware);
	cg_json_u64(j, "control", c->tcpf.ctl);
	cg_json_u64(j, "retransmissions", c->tcpf.retx);
	cg_json_end(j, '}');
	/* redund_spare (spare.h): what the links after the first carry. */
	if (c->cfg->redund_spare) {
		static const char *const how[] = { "off", "copies", "parity", "mix", "rs", "rlc" };

		cg_json_obj(j, "spare");
		cg_json_str(j, "mode", how[c->cfg->redund_spare <= CG_SPARE_RLC ? c->cfg->redund_spare : 0]);
		cg_json_str(j, "now", how[c->sp_how >= 0 && c->sp_how <= CG_SPARE_RLC ? c->sp_how : 0]);
		cg_json_u64(j, "k", c->sp_k);
		cg_json_i64(j, "first", c->sp_first1 - 1);
		cg_json_str(j, "pinned", c->cfg->redund_spare_first);
		{
			/* why the pinned link is or is not the first (CG_SPARE_PIN_*) */
			static const char *const why[] = { "none", "first", "absent", "down", "unmeasured", "banned", "choked" };

			cg_json_str(j, "pin", why[c->sp_pin_why >= 0 && c->sp_pin_why <= CG_SPARE_PIN_CHOKED ? c->sp_pin_why : 0]);
		}
		cg_json_ms(j, "data_mbit", c->sp_data_bps / 1000); /* as capacity_mbit */
		cg_json_u64(j, "copies", c->sp_copies);
		cg_json_u64(j, "copies_skipped", c->sp_copy_skip);
		cg_json_u64(j, "fed", c->sp_fed);
		cg_json_u64(j, "parities", c->sp_parities);
		cg_json_u64(j, "parities_skipped", c->sp_par_skip);
		cg_json_u64(j, "parities_no_link", c->sp_par_nolink);
		/* rs | rlc (story 021): the plan and the repairs */
		cg_json_obj(j, "code");
		cg_json_u64(j, "k", c->sp_plan.k);
		cg_json_u64(j, "m", c->sp_plan.m);
		cg_json_u64(j, "need", c->sp_plan.need);
		cg_json_u64(j, "pps", c->sp_pps);
		cg_json_u64(j, "loss_avg_pm", c->sp_loss16 / 16);
		cg_json_u64(j, "repairs", c->sp_rep_sent);
		cg_json_u64(j, "repair_bytes", c->sp_rep_bytes);
		cg_json_u64(j, "skipped", c->sp_rep_skip);
		cg_json_u64(j, "no_link", c->sp_rep_nolink);
		cg_json_u64(j, "failed", c->sp_rep_fail);
		cg_json_end(j, '}');
		cg_json_end(j, '}');
	} else {
		cg_json_null(j, "spare");
	}
	/* Its parities (fec.h). */
	cg_json_obj(j, "fec");
	cg_json_bool(j, "on", c->cfg->fec);
	cg_json_u64(j, "parities", c->fec_parities);
	cg_json_u64(j, "bytes", c->fec_bytes);
	cg_json_u64(j, "no_link", c->fec_nolink);
	cg_json_u64(j, "capped", c->fec_capped);
	cg_json_u64(j, "deadline_us", c->cfg->fec ? fec_deadline(c) : 0);
	cg_json_arr(j, "sizes");
	for (int i = 1; i <= CG_FEC_KMAX; i++)
		cg_json_u64(j, NULL, c->fec_size[i]);
	cg_json_end(j, ']');
	cg_json_u64(j, "closed_full", c->fec_why[0]);
	cg_json_u64(j, "closed_evicted", c->fec_why[1]);
	cg_json_u64(j, "closed_deadline", c->fec_why[2]);
	cg_json_u64(j, "closed_split_off", c->fec_why[3]);
	cg_json_str(j, "code", c->cfg->fec_code == CG_FEC_RLC ? "rlc" : "xor");
	/* FEC v3 (fec_code = rlc, story 026): how it protects now, its window
	 * and the repairs */
	if (f3_on(c)) {
		static const char *const mode[] = { "off", "bonding", "single" };
		static const char *const single[] = { "auto", "yes", "no" };

		cg_json_obj(j, "rlc");
		cg_json_str(j, "mode", mode[c->f3.mode <= CG_F3_SINGLE ? c->f3.mode : 0]);
		cg_json_u64(j, "pct", c->f3.mode == CG_F3_OFF ? 0 : c->f3.pct); /* the share now (fec_pct_min to fec_pct) */
		cg_json_u64(j, "pct_max", c->cfg->fec_pct);
		cg_json_u64(j, "pct_min", c->cfg->fec_pct_min);
		cg_json_u64(j, "bursts", c->f3.bursts);
		cg_json_u64(j, "window_ms", c->cfg->fec_window_ms);
		cg_json_str(j, "single", single[c->cfg->fec_single >= 0 && c->cfg->fec_single <= 2 ? c->cfg->fec_single : 0]);
		cg_json_u64(j, "links", (uint64_t)__builtin_popcount(c->f3_links));
		cg_json_u64(j, "window", c->f3_span); /* the last repair's, sequences */
		cg_json_ms(j, "lag_ms", c->f3_lag);   /* and how long before now it ended */
		cg_json_u64(j, "density", c->f3_dn);
		cg_json_u64(j, "repairs", c->f3_rep);
		cg_json_u64(j, "repair_bytes", c->f3_bytes);
		cg_json_u64(j, "data_bytes", c->f3_data);
		cg_json_u64(j, "overhead_pm", c->f3_data ? c->f3_bytes * 1000 / c->f3_data : 0);
		cg_json_u64(j, "tail", c->f3_tail);
		cg_json_u64(j, "guarded", c->f3_guarded);
		cg_json_u64(j, "no_link", c->f3_nolink);
		cg_json_u64(j, "empty", c->f3_empty);
		cg_json_u64(j, "gated", c->f3_gated);
		cg_json_u64(j, "quiet", c->f3_quiet); /* batches without repairs: no burst nor random loss, fec_pct_min 0 */
		cg_json_u64(j, "switches", c->f3_switches);
		cg_json_u64(j, "insensitive_pm", (uint64_t)c->f3.ins_q10 * 1000 / 1024); /* the bytes loss does not hold back (the protocontroller's) */
		cg_json_end(j, '}');
	} else {
		cg_json_null(j, "rlc");
	}
	cg_json_end(j, '}');
	/* For a shaper on the tunnel (capacity.h): null until measured. */
	if (cg_cap_shape_bps(&c->ctot))
		cg_json_ms(j, "shape_mbit", cg_cap_shape_bps(&c->ctot) / 1000);
	else
		cg_json_null(j, "shape_mbit");
	/* What the newest probe reply says (null: no tail, an older server). */
	if (c->peer_bond)
		cg_json_bool(j, "server_reorders", c->peer_reorders);
	else
		cg_json_null(j, "server_reorders");
	cg_json_end(j, '}');
	cg_json_obj(j, "download");
	cg_json_u64(j, "packets", c->down_pkts);
	cg_json_u64(j, "bytes", c->down_bytes);
	cg_json_u64(j, "duplicates", c->rxs.dups);
	cg_json_u64(j, "too_old", c->rxs.old);
	cg_json_u64(j, "hellos", c->rxs.hellos);
	cg_json_u64(j, "auth_failures", c->rxs.auth_fail);
	cg_json_u64(j, "malformed", c->rxs.malformed + c->rxs.trunc);
	cg_json_u64(j, "foreign_session", c->rxs.foreign);
	cg_json_u64(j, "stale", c->rxs.stale_gen);
	cg_json_u64(j, "tun_drops", c->down_tun_drops);
	cg_json_u64(j, "not_ip", c->down_not_ip);
	cg_json_u64(j, "protocontroller_off", c->down_pctl_off);
	cg_json_end(j, '}');
	/* FEC v3's repairs of the download (story 026), as the server's
	 * session repair, rebuilt and rescue counts */
	cg_json_obj(j, "download_fec");
	cg_json_bool(j, "on", c->dfec != NULL);
	cg_json_bool(j, "fec3", dwide(c, now_ms));
	cg_json_u64(j, "xor_recovered", 0);
	cg_json_u64(j, "rlc_recovered", c->drlc ? c->drlc->recovered : 0);
	cg_json_u64(j, "rlc_wide", c->drlc ? c->drlc->wide : 0);
	cg_json_u64(j, "rlc_evicted", c->drlc ? c->drlc->evicted : 0);
	cg_json_u64(j, "rlc_peak", c->drlc ? c->drlc->peak : 0);
	cg_json_u64(j, "repairs", c->drlc ? c->drlc->repairs : 0);
	cg_json_u64(j, "useless", c->drlc ? c->drlc->useless : 0);
	cg_json_u64(j, "kept", c->drlc ? c->drlc->kept : 0);
	cg_json_u64(j, "dropped", c->drlc ? c->drlc->dropped : 0);
	cg_json_u64(j, "dependent", c->drlc ? c->drlc->dependent : 0);
	cg_json_u64(j, "bad", c->drlc ? c->drlc->bad : 0);
	cg_json_u64(j, "unknowns", c->drlc ? c->drlc->ncol : 0);
	cg_json_u64(j, "unused", c->drep_unused);
	cg_json_u64(j, "malformed", c->drep_bad);
	cg_json_u64(j, "already_there", c->d_fec_dups);
	cg_json_obj(j, "rebuilt");
	cg_json_u64(j, "ok", c->d_rb_ok);
	cg_json_u64(j, "bad", c->d_rb_bad);
	cg_json_u64(j, "unsure", c->d_rb_unsure);
	cg_json_end(j, '}');
	cg_json_u64(j, "waited_code", c->dhq_waited);
	cg_json_u64(j, "not_asked", c->dhq_none);
	cg_json_u64(j, "left_to_tcp", c->dhq_tcp);
	cg_json_u64(j, "wait_full", c->dhq_full);
	cg_json_u64(j, "no_nack", c->dhq_nonack);
	cg_json_u64(j, "waiting", c->dhq_n);
	cg_json_u64(j, "parq_asked", c->dparq_req);
	cg_json_u64(j, "parq_repairs_asked", c->dparq_want);
	cg_json_u64(j, "parq_messages", c->dparq_msgs);
	cg_json_end(j, '}');
	if (c->pctl)
		cg_pctl_json(c->pctl, j);
	/* Download: lost on every link, joint lateness, and the reorderer
	 * (story 012). */
	cg_json_u64(j, "lost_all", c->rxs.replay.lost);
	/* The rescue: whether the reorderer asks (rescue_wait_ms, and the server
	 * keeps what it sent), and the NACKs sent. */
	cg_json_obj(j, "rescue");
	cg_json_bool(j, "on", c->rocf.rescue_us != 0);
	cg_json_bool(j, "server_keeps", c->peer_rescue);
	cg_json_u64(j, "nacks_sent", c->nacks_sent);
	/* the upload's: what it kept and sent again for the server's NACKs */
	cg_json_bool(j, "keeps", c->resc_idx != NULL);
	cg_json_u64(j, "nacks_rx", c->nacks_rx);
	cg_json_u64(j, "sent", c->resc_sent);
	cg_json_u64(j, "missing", c->resc_miss);
	cg_json_u64(j, "over_budget", c->resc_budget);
	cg_json_u64(j, "congested", c->resc_congested);
	cg_json_u64(j, "gone_sent", c->resc_gone_tx);
	cg_json_u64(j, "gone_received", c->resc_gone_rx);
	/* asked-for GRIT messages handed to their streams, not sent again */
	cg_json_u64(j, "to_grit", c->resc_grit);
	cg_json_u64(j, "to_grit_bytes", c->resc_grit_bytes);
	cg_json_ms(j, "wait_ms", c->rocf.rescue_us);
	cg_json_ms(j, "tcp_budget_ms", tcp_budget_us(c, now_ms * 1000));
	cg_json_u64(j, "left_to_tcp", c->ask_tcp);
	/* the parity the server asked for again (harq.h, story 023) */
	cg_json_obj(j, "parq");
	cg_json_u64(j, "received", c->parq_rx);
	cg_json_u64(j, "malformed", c->parq_bad);
	cg_json_u64(j, "requests", c->parq_req);
	cg_json_u64(j, "repairs", c->parq_sent);
	cg_json_u64(j, "bytes", c->parq_bytes);
	cg_json_u64(j, "missing", c->parq_missing);
	cg_json_u64(j, "over_budget", c->parq_budget);
	cg_json_u64(j, "no_link", c->parq_nolink);
	cg_json_end(j, '}');
	cg_json_end(j, '}');
	cg_json_joint(j, "joint", &c->rxs.arr);
	cg_json_reorder(j, "reorder", &c->rxs.ro, c->cfg->reorder, &c->pool);
	/* which links it waits for: the server's mask (active.h, C2) */
	cg_json_active(j, "active", &c->act_rx, &c->rxs.ro, c->cfg->reorder_active, c->act_base, rset,
		       cg_act_tx_mask(&c->act_tx, cg_now_us(), CG_ACT_HOLD_MS * 1000), now_ms * 1000);
	cg_json_tcpe(j, "tcp_e2e", &c->tcpe); /* the download's TCP where it leaves the tunnel (story 023) */
	cg_json_arr(j, "overrides");
	for (int i = 0; i < c->ovr.n; i++) {
		cg_json_obj(j, NULL);
		cg_json_str(j, "name", c->ovr.e[i].name);
		cg_json_str(j, "override", cg_ovr_name(c->ovr.e[i].v));
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_names_json(&c->names, j, now_ms);
	cg_json_arr(j, "links");
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		const struct link *l = &c->link[i];
		const struct cg_hlink *h = &c->uh[i];
		const struct cg_link_cfg *lc;

		/* Not the links a reload took out, unless set by hand. */
		if (!l->used || (l->fd < 0 && cg_ovr_get(&c->ovr, l->ifname) == CG_OVR_AUTO &&
				 !configured(c->cfg, l->ifname)))
			continue;
		lc = cg_config_link(c->cfg, l->ifname);
		cg_json_obj(j, NULL);
		cg_json_u64(j, "id", (uint64_t)i);
		cg_json_str(j, "name", l->ifname);
		cg_json_str(j, "label", lc ? lc->label : "");
		cg_json_str(j, "state", link_state(c, l, now_ms));
		cg_json_str(j, "reason", l->fd >= 0 ? "" : why_name[l->why]);
		cg_json_str(j, "override", cg_ovr_name(cg_ovr_get(&c->ovr, l->ifname)));
		cg_json_str(j, "local", l->fd >= 0 ? cg_addr_str(&l->local, buf, sizeof(buf)) : "");
		cg_json_str(j, "remote", l->fd >= 0 ? cg_addr_str(&l->remote, buf, sizeof(buf)) : "");
		cg_json_str(j, "family", l->fd < 0 ? "" : l->remote.ss_family == AF_INET6 ? "ipv6" : "ipv4");
		/* Its place in the server list, as filtered by family: 0 is the
		 * first, a later one means it failed over (srvpick.h). */
		if (l->fd >= 0)
			cg_json_u64(j, "candidate", (uint64_t)l->cand_idx);
		else
			cg_json_null(j, "candidate");
		cg_json_u64(j, "candidates", (uint64_t)l->ncand);
		/* Away from where it was due, or from its pick, because a socket
		 * could not be opened there (link_open tries again). */
		cg_json_bool(j, "detour", l->fd >= 0 && l->detour);
		cg_json_u64(j, "failovers", l->failovers);
		link_threads_json(c, l, j);
		cg_json_u64(j, "path_mtu", l->fd >= 0 ? l->path_mtu : 0);
		cg_json_ms(j, "rtt_ms", l->srtt8_us / 8);
		/* the raw probe replies, oldest first: [age ms, up us, down us]
		 * (up and down carry the clocks' offset: their changes count) */
		cg_json_arr(j, "probes");
		for (int r = 0; r < CG_PROBE_RAW; r++) {
			int x = (l->raw_at + r) % CG_PROBE_RAW;

			if (!l->raw_ms[x])
				continue;
			cg_json_arr(j, NULL);
			cg_json_u64(j, NULL, now_ms - l->raw_ms[x]);
			cg_json_u64(j, NULL, l->raw_up[x]);
			cg_json_u64(j, NULL, l->raw_down[x]);
			cg_json_end(j, ']');
		}
		cg_json_end(j, ']');
		cg_json_u64(j, "last_rx_ms_ago", l->rxl.last_rx_ms ? now_ms - l->rxl.last_rx_ms : 0);
		cg_json_str(j, "upload", h->state == CG_H_MUTED ? "muted" : "active");
		cg_json_u64(j, "mute_backoff", h->backoff); /* unmute wait = 2 x settle << it */
		if (h->have_behind)
			cg_json_ms_signed(j, "upload_behind_ms", h->behind_us);
		else
			cg_json_null(j, "upload_behind_ms");
		/* mute_queue: the lateness the guard compared (health.h) */
		if (h->have_late)
			cg_json_ms_signed(j, "upload_late_ms", h->late_us);
		else
			cg_json_null(j, "upload_late_ms");
		cg_json_u64(j, "upload_mutes", h->mutes);
		cg_json_u64(j, "upload_state_ms", now_ms - h->changed_ms);
		cg_json_bool(j, "download_muted", l->peer_muted);
		/* The server answered with a HELLO lately: it is taking this
		 * link up (cookie), or refuses the router. */
		cg_json_bool(j, "cookie", l->rxl.cookie != 0);
		cg_json_bool(j, "refused", link_refused(c, l, now_ms));
		cg_json_u64(j, "tx_packets", l->tx_pkts);
		cg_json_u64(j, "tx_bytes", l->tx_bytes);
		cg_json_u64(j, "tx_drops", l->tx_drops);
		cg_json_u64(j, "tx_errors", l->tx_errors);
		cg_json_u64(j, "tx_split", l->tx_split);
		cg_json_u64(j, "tx_fec_packets", l->tx_fec_pkts); /* parities (fec.h), not in tx_packets */
		cg_json_u64(j, "tx_fec_bytes", l->tx_fec_bytes);
		cg_json_u64(j, "fec_repairs", l->f3_rep); /* of them, FEC v3's (story 026) */
		cg_json_u64(j, "fec_repair_bytes", l->f3_bytes);
		cg_json_ms(j, "fec_loss_pm", (uint64_t)c->f3.loss16[i] * 1000 / 16); /* its loss as FEC v3 smooths it, per mille */
		/* redund_spare: its allowance (spare.h) */
		if (c->cfg->redund_spare) {
			cg_json_obj(j, "spare");
			cg_json_ms(j, "mbit", c->spare[i].rate_bps / 1000);
			cg_json_u64(j, "cuts", c->spare[i].n_cut);
			cg_json_u64(j, "taken", c->spare[i].n_take);
			cg_json_u64(j, "skipped", c->spare[i].n_skip);
			/* the first link's choice at the last tick: measured fresh, its
			 * base and lag (null: not measured), choked, banned */
			cg_json_bool(j, "fresh", c->sp_known >> i & 1);
			if (c->sp_known >> i & 1) {
				cg_json_ms(j, "base_ms", c->sp_base[i]);
				cg_json_ms(j, "lag_ms", c->sp_lag[i]);
			} else {
				cg_json_null(j, "base_ms");
				cg_json_null(j, "lag_ms");
			}
			cg_json_bool(j, "choked", c->sp_choked >> i & 1);
			cg_json_bool(j, "banned", c->sp_banned >> i & 1);
			cg_json_end(j, '}');
		} else {
			cg_json_null(j, "spare");
		}
		cg_json_u64(j, "weight", c->lw[i]); /* its share with split = on */
		cg_json_str(j, "type", cg_link_type_name(c->ltype[i])); /* its link model (linkmodel.h) */
		/* The governor's weight and the queue it measures (null: off, or
		 * no delay yet). */
		if (c->cfg->governor)
			cg_json_u64(j, "gov_weight", c->gw[i]);
		else
			cg_json_null(j, "gov_weight");
		if (c->cfg->governor && c->gov.l[i].have_queue)
			cg_json_ms(j, "gov_queue_ms", (uint64_t)c->gov.l[i].queue_us);
		else
			cg_json_null(j, "gov_queue_ms");
		if (c->cfg->governor) {
			cg_json_obj(j, "gov_ticks");
			cg_json_u64(j, "down", c->gov.l[i].ticks_down);
			cg_json_u64(j, "up", c->gov.l[i].ticks_up);
			cg_json_u64(j, "hold", c->gov.l[i].ticks_hold);
			cg_json_end(j, '}');
		} else {
			cg_json_null(j, "gov_ticks");
		}
		if (c->cfg->capacity_weights)
			cg_json_u64(j, "cap_weight", c->cw[i]);
		else
			cg_json_null(j, "cap_weight");
		/* What it does in the upload split (null: not split now), and its
		 * delay behind the fastest candidate as guard G4 last saw it. */
		if (split_active(c))
			cg_json_str(j, "split_share",
				    cg_split_share_name(cg_split_share(&c->spread, (unsigned)i, s_elig, s_dup0, spread_us)));
		else
			cg_json_null(j, "split_share");
		if (c->spread.behind >> i & 1)
			cg_json_ms_signed(j, "split_behind_ms", c->spread.behind_us[i]);
		else
			cg_json_null(j, "split_behind_ms");
		cg_json_g4(j, &c->spread, (unsigned)i, now_ms);
		cg_json_owd_fresh(j, h, now_ms);
		cg_json_u64(j, "rx_first", c->rxs.rx[i].wins);
		cg_json_u64(j, "rx_duplicate", c->rxs.rx[i].dups);
		cg_json_u64(j, "rx_late", c->rxs.rx[i].late);
		cg_json_u64(j, "rx_missed", c->rxs.rx[i].missed);
		cg_json_ms(j, "rx_lag_ms", cg_lag_us(&c->rxs.rx[i]));
		/* Its download delay behind the fastest link the reorderer waits
		 * for, from the probe replies and the first copies. */
		cg_json_owd_spread(j, "rx_owd_spread_ms", &c->rxs.ro, (unsigned)i, rset);
		cg_json_owd_swing(j, "rx_owd_swing_ms", &c->rxs.ro, (unsigned)i);
		cg_json_u64(j, "rx_reorder_late", c->rxs.ro.link[i].late);
		cg_json_capacity(j, &c->cap[i], c->cfg->rescue_target_ms * 1000, now_ms);
		cg_json_sig(j, &c->sig[i], now_ms);
		if (l->paced != CG_PACE_NONE)
			cg_json_ms(j, "pace_mbit", l->paced * 8 / 1000);
		else
			cg_json_null(j, "pace_mbit");
		/* What the split by expected arrival sees of it (null: off, or no rate). */
		if (c->cfg->split_eta && c->eta.l[i].bps) {
			cg_json_ms(j, "eta_base_ms", c->eta.l[i].base_us);
			cg_json_ms(j, "eta_mbit", c->eta.l[i].bps / 1000);
		} else {
			cg_json_null(j, "eta_base_ms");
			cg_json_null(j, "eta_mbit");
		}
		cg_json_obj(j, "server_view");
		cg_json_u64(j, "age_ms", l->peer_view_ms ? now_ms - l->peer_view_ms : 0);
		cg_json_u64(j, "rx", l->peer_view.rx);
		cg_json_u64(j, "first", l->peer_view.wins);
		cg_json_ms(j, "lag_ms", l->peer_view.lag_us);
		cg_json_end(j, '}');
		cg_json_end(j, '}');
	}
	cg_json_end(j, ']');
	cg_json_end(j, '}');
}

static void write_status(struct client *c, uint64_t now_ms)
{
	struct cg_json j;

	cg_json_init(&j);
	status_json(c, now_ms, &j);
	cg_status_writer_submit(&c->sw, &j);
	cg_json_free(&j);
}

/* The STATE of an interface in "ctl links"; *up: its link when that carries
 * the tunnel, else NULL. */
static const char *row_state(struct client *c, const struct cg_iface *ifc, uint64_t now_ms, const struct link **up)
{
	struct cands e;
	int why = eligible(c, ifc, &e);
	const struct link *l = link_find(c, ifc->name);

	*up = why == WHY_OK && l && l->fd >= 0 ? l : NULL;
	return *up ? link_state(c, *up, now_ms) : why == WHY_OK ? "unusable" : why_name[why];
}

/* "cengarde ctl links": every interface netlink knows, as a table. The
 * addresses go last; STATE and LOCAL are as wide as their widest value (a
 * long reason, an IPv6 address). */
#define LINK_ROW "%-15s %-12s %-*s %-6s %-6s %9s %5s %9s  %-*s  %s\n"
static void links_text(struct client *c, uint64_t now_ms, struct cg_json *j)
{
	char a[64], b[64], remote[96], rtt[24], mtu[16], moves[24];
	const struct link *l;
	int ws = 10, w = 5;

	for (int i = 0; i < c->nl.nifs; i++) {
		int len;

		if (!c->nl.ifs[i].name[0])
			continue;
		len = (int)strlen(row_state(c, &c->nl.ifs[i], now_ms, &l));
		ws = len > ws ? len : ws;
		if (l) {
			len = (int)strlen(cg_addr_str(&l->local, a, sizeof(a)));
			w = len > w ? len : w;
		}
	}
	cg_json_raw(j, LINK_ROW, "INTERFACE", "LABEL", ws, "STATE", "MANUAL", "UPLOAD", "RTT", "MTU", "FAILOVERS", w,
		    "LOCAL", "REMOTE");
	for (int i = 0; i < c->nl.nifs; i++) {
		const struct cg_iface *ifc = &c->nl.ifs[i];
		const struct cg_link_cfg *lc;
		const struct link *slot;
		const char *state;
		enum cg_ovr o;

		if (!ifc->name[0])
			continue;
		lc = cg_config_link(c->cfg, ifc->name);
		o = cg_ovr_get(&c->ovr, ifc->name);
		state = row_state(c, ifc, now_ms, &l);
		if (l && l->srtt8_us)
			snprintf(rtt, sizeof(rtt), "%u.%u ms", (unsigned)(l->srtt8_us / 8000),
				 (unsigned)(l->srtt8_us / 800 % 10));
		else
			strcpy(rtt, "-");
		if (l && l->path_mtu)
			snprintf(mtu, sizeof(mtu), "%u", l->path_mtu);
		else
			strcpy(mtu, "-");
		slot = link_find(c, ifc->name);
		if (slot)
			snprintf(moves, sizeof(moves), "%llu", (unsigned long long)slot->failovers);
		else
			strcpy(moves, "-");
		/* With more than one candidate, which one: 1 is the first. */
		if (l && l->ncand > 1)
			snprintf(remote, sizeof(remote), "%s (%d/%d)", cg_addr_str(&l->remote, b, sizeof(b)), l->cand_idx + 1,
				 l->ncand);
		else
			snprintf(remote, sizeof(remote), "%s", l ? cg_addr_str(&l->remote, b, sizeof(b)) : "-");
		cg_json_raw(j, LINK_ROW, ifc->name, lc && lc->label[0] ? lc->label : "-", ws, state,
			    o == CG_OVR_AUTO ? "-" : cg_ovr_name(o),
			    l ? (c->uh[l - c->link].state == CG_H_MUTED ? "muted" : "active") : "-", rtt, mtu, moves, w,
			    l ? cg_addr_str(&l->local, a, sizeof(a)) : "-", remote);
	}
	for (int i = 0; i < c->ovr.n; i++)
		if (!cg_nl_find(&c->nl, c->ovr.e[i].name))
			cg_json_raw(j, LINK_ROW, c->ovr.e[i].name, "-", ws, "absent", cg_ovr_name(c->ovr.e[i].v), "-", "-",
				    "-", "-", w, "-", "-");
}

/* "cengarde ctl threads": the hub (this loop), its pumps and the status
 * writer, with their CPU from their own clocks. */
static void threads_text(struct client *c, struct cg_json *j)
{
	cg_threads_head(j);
	cg_threads_row(j, "cg-hub", cg_gettid(), cg_thread_cpu_ns(pthread_self(), 1), cg_cpuwin_permille(&c->hub_cpu));
	for (int k = 0; k < c->npumps; k++)
		if (c->pump[k]->threaded)
			cg_threads_row(j, c->pump[k]->name, atomic_load(&c->pump[k]->tid), cg_pump_cpu_ns(c->pump[k]),
				       cg_cpuwin_permille(&c->pst[k].cpu));
	if (c->sw.running)
		cg_threads_row(j, c->sw.name, c->sw.tid, cg_thread_cpu_ns(c->sw.thread, 0), cg_cpuwin_permille(&c->sw_cpu));
}

/* ---- reload ---- */

static void reload_start(struct client *c)
{
	if (cg_loader_start(&c->loader, c->run->path) < 0)
		c->reload_again = 1; /* once the load under way is done */
}

/* Puts next in place of the running configuration, keeping the session. */
static void apply_config(struct client *c, struct cg_config *next, uint64_t now_ms)
{
	struct cg_config *old = c->cfg;
	int bond;

	c->cfg = next;
	c->txs.k_tx = next->key;
	c->txs.hint = cg_client_hint(next->key);
	c->rxs.k_rx = next->key + CG_SIPHASH_KEY_LEN;
	c->hcfg = cg_hcfg_of(next);
	/* The reorderer's knobs apply from the next packet; turned off, what
	 * it holds goes on at once, in order. */
	c->rocf = cg_ro_cfg_of(next);
	/* the links may have moved: every link in the masks until new ones */
	cg_act_tx_start(&c->act_tx, cg_now_us());
	cg_act_rx_forget(&c->act_rx);
	rescue_apply(c);
	f3_apply(c);
	if (old->reorder && !next->reorder)
		ro_flush(c, now_ms);
	if (old->split != next->split)
		cg_split_reset(&c->sp);
	/* The upload follows split and the weights from the next batch; the
	 * server learns them, and reorder, from the probes that go out below. */
	bond = old->split != next->split || old->reorder != next->reorder || weights_update(c);
	cg_log_level_set(c->run->verbose ? CG_LOG_DEBUG : next->log_level);
	if (strcmp(old->status_file, next->status_file)) {
		cg_status_writer_stop(&c->sw);
		if (next->status_file[0] && cg_status_writer_start(&c->sw, next->status_file, "cg-status") < 0)
			cg_warn("status file %s: cannot start the writer thread", next->status_file);
	}
	if (old->rcvbuf != next->rcvbuf || old->sndbuf != next->sndbuf) {
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if (c->link[i].fd >= 0)
				cg_sock_buffers(c->link[i].fd, next->rcvbuf, next->sndbuf);
	}
	if (old->passthrough != next->passthrough && next->passthrough >= 0)
		cg_info("asking the server for IP pass %s", next->passthrough ? "on" : "off");
	/* Names still there keep what they had: the new lists are built with
	 * it. A link whose own list changed starts again from its first
	 * address, as seen with the families it last opened with: not for an
	 * entry of another family, nor for a name resolved again in another
	 * order (srvpick.h). A link never opened has none, and no address to
	 * keep. */
	cg_names_set(&c->names, c->ep, next);
	lists_build(c, next);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];
		struct cg_srvlist a, b;

		if (!l->used)
			continue;
		a = servers_of(old, l->ifname);
		b = servers_of(next, l->ifname);
		if (!cg_lists_same(&a, &b, l->families))
			memset(&l->cand, 0, sizeof(l->cand));
	}
	cg_config_free(old);
	free(old);
	reconcile(c, now_ms);
	/* A probe on every link at once: the server stops (or starts) splitting
	 * the download within a round trip, not at the next interval. */
	if (bond)
		for (int i = 0; i < CG_MAX_LINKS; i++)
			if (link_up(&c->link[i]))
				send_probe(c, &c->link[i], cg_now_us(), probe_interval(c, now_ms));
}

/* A load finished. Returns -1 when the process has to stop (a restart that
 * could not happen). */
static int reload_done(struct client *c)
{
	struct cg_config *next;
	const char *why = NULL;
	char msg[700];

	if (!cg_loader_done(&c->loader, &next))
		return 0;
	if (!next) {
		cg_err("reload refused, still running the previous configuration: %s", c->loader.err);
		snprintf(c->config_error, sizeof(c->config_error), "%s", c->loader.err);
		snprintf(msg, sizeof(msg), "error: %s\n", c->loader.err);
	} else {
		cg_log_warnings(c->run->path, c->loader.warn);
		c->config_error[0] = '\0';
		why = cg_config_restart_needed(c->cfg, next);
		if (why) {
			snprintf(msg, sizeof(msg), "ok: %s changed, restarting\n", why);
		} else {
			apply_config(c, next, cg_now_ms());
			cg_info("reload: configuration applied");
			snprintf(msg, sizeof(msg), "ok\n");
		}
	}
	if (!why && c->reload_again) {
		/* Asked again while loading: whoever waits gets the newer outcome. */
		c->reload_again = 0;
		reload_start(c);
		return 0;
	}
	cg_ctl_reply_waiting(&c->ctl, c->ep, msg);
	if (!why)
		return 0;
	cg_info("reload: %s changed, restarting", why);
	cg_config_free(next);
	free(next);
	cg_status_writer_stop(&c->sw);
	cg_ctl_close(&c->ctl, c->ep);
	cg_reexec(c->run->argv);
	cg_err("restart: %s", strerror(errno));
	return -1;
}

/* ---- control socket ---- */

static void ctl_command(struct client *c, int k, uint64_t now_ms)
{
	struct cg_ctl_cmd cmd;
	struct cg_json j;
	char err[256];

	cg_json_init(&j);
	if (cg_ctl_parse(c->ctl.c[k].in, &cmd, err, sizeof(err)) < 0) {
		cg_json_raw(&j, "error: %s\n", err);
	} else {
		switch (cmd.op) {
		case CG_CTL_STATUS:
			status_json(c, now_ms, &j);
			cg_json_raw(&j, "\n");
			break;
		case CG_CTL_LINKS:
			links_text(c, now_ms, &j);
			break;
		case CG_CTL_THREADS:
			threads_text(c, &j);
			break;
		case CG_CTL_LINK:
			if (cg_ovr_set(&c->ovr, cmd.ifname, cmd.ovr) < 0) {
				cg_json_raw(&j, "error: more than %d links set by hand; undo some with auto or reset\n",
					    CG_CTL_OVERRIDES);
				break;
			}
			cg_info("link %s: %s by hand", cmd.ifname,
				cmd.ovr == CG_OVR_OFF  ? "paused"
				: cmd.ovr == CG_OVR_ON ? "forced on"
						       : "back to the configuration");
			reconcile(c, now_ms);
			cg_json_raw(&j, "ok\n");
			break;
		case CG_CTL_RESET:
			cg_ovr_reset(&c->ovr);
			cg_info("every link back to the configuration");
			reconcile(c, now_ms);
			cg_json_raw(&j, "ok\n");
			break;
		case CG_CTL_RELOAD:
			cg_info("reloading %s", c->run->path);
			c->ctl.c[k].waiting = 1;
			c->ctl.c[k].deadline_ms = now_ms + CG_CTL_RELOAD_TIMEOUT_MS;
			reload_start(c);
			cg_json_free(&j);
			return; /* the reply goes out when the load is done */
		}
	}
	if (j.failed) {
		cg_json_free(&j);
		cg_ctl_reply(&c->ctl, c->ep, k, NULL, 0);
	} else {
		cg_ctl_reply(&c->ctl, c->ep, k, j.buf, j.len); /* takes the buffer */
	}
}

/* Links whose pump could not poll their new socket: closed (the pump lets
 * go of it), and the next reconcile tries again. */
static void take_open_failed(struct client *c, uint64_t now_ms)
{
	for (int k = 0; k < c->npumps; k++) {
		_Atomic uint32_t *f = &c->pump[k]->open_failed;
		uint32_t m = atomic_load_explicit(f, memory_order_relaxed) ? atomic_exchange(f, 0) : 0;

		for (unsigned i = 0; m; i++, m >>= 1) {
			struct link *l = &c->link[i];

			if (!(m & 1))
				continue;
			l->open_failed++;
			if (cg_ratelimit_ok(&l->rl, now_ms, 30000))
				cg_warn("link %s: its socket could not be polled, trying again", l->ifname);
			link_close(c, l, NULL);
		}
	}
}

/* How long work has waited for pump k with no loop pass (commands, or
 * datagrams in one of its sockets while its ring has room): 0 below 1 s,
 * counted from the first check that saw it waiting (thrplan.h). Its loop
 * clock is read before the work, so that a pass in between can only end
 * the count. */
static uint32_t pump_stalled_ms(struct client *c, int k, uint64_t now_ms)
{
	struct cg_pump *p = c->pump[k];
	uint32_t loop = atomic_load_explicit(&p->loop_ms, memory_order_relaxed), rmem, drops;
	int work = cg_pump_cmds_waiting(p) > 0;

	for (int i = 0; i < CG_MAX_LINKS && !work; i++) {
		const struct link *l = &c->link[i];

		if (l->has_pump && l->pump == k && link_up(l) && !atomic_load(&p->rx_blocked) &&
		    sock_meminfo(l->fd, &rmem, &drops) == 0 && rmem)
			work = 1;
	}
	return cg_stall_check(&c->pst[k].stall, loop, work, now_ms);
}

/* Once a second: the threads' CPU, the pumps' counters and liveness. */
static void threads_second(struct client *c, uint64_t now_ms)
{
	cg_cpuwin_add(&c->hub_cpu, now_ms, cg_thread_cpu_ns(pthread_self(), 1));
	if (c->sw.running)
		cg_cpuwin_add(&c->sw_cpu, now_ms, cg_thread_cpu_ns(c->sw.thread, 0));
	for (int k = 0; k < c->npumps; k++) {
		struct cg_pump *p = c->pump[k];
		struct pump_stats *ps = &c->pst[k];
		uint32_t errs = atomic_load_explicit(&p->rx_errors, memory_order_relaxed);

		cg_acc32(&ps->pkts, &ps->l_pkts, atomic_load_explicit(&p->rx_pkts, memory_order_relaxed));
		cg_acc32(&ps->paused, &ps->l_paused, atomic_load_explicit(&p->rx_paused, memory_order_relaxed));
		if (errs != ps->l_errors && cg_ratelimit_ok(&ps->rl_err, now_ms, 10000))
			cg_warn("%s receive: %s", p->threaded ? p->name : "link",
				strerror(atomic_load_explicit(&p->rx_errno, memory_order_relaxed)));
		cg_acc32(&ps->errors, &ps->l_errors, errs);
		if (!p->threaded)
			continue;
		cg_cpuwin_add(&ps->cpu, now_ms, cg_pump_cpu_ns(p));
		ps->stalled_ms = pump_stalled_ms(c, k, now_ms);
		if (ps->stalled_ms > 5000 && cg_ratelimit_ok(&ps->rl_stall, now_ms, 60000))
			cg_warn("thread %s has not run for %u s with work waiting", p->name, ps->stalled_ms / 1000);
	}
}

static void tick(struct client *c)
{
	uint64_t now_us = cg_now_us(), now_ms = now_us / 1000;
	uint32_t interval = probe_interval(c, now_ms);
	int failover = 0;

	take_open_failed(c, now_ms);
	link_cmds_retry(c);
	ro_service(c);
	if (c->dfec)
		rescue_apply(c); /* the code's wait on or off with FEC v3's repairs of the download */
	if (now_ms >= c->next_second_ms) {
		threads_second(c, now_ms);
		c->next_second_ms = now_ms + 1000;
	}
	if (now_ms >= c->next_hop_ms) {
		for (int k = 0; k < c->npumps; k++)
			cg_hist_window(c->hop[k].b, c->pst[k].hop_snap, c->pst[k].hop_win);
		c->next_hop_ms = now_ms + 5000;
	}

	for (int i = 0; i < CG_MAX_LINKS; i++) {
		struct link *l = &c->link[i];

		/* A new interval goes out at once, so the server never waits for a
		 * probe at the old rate. */
		if (link_up(l) &&
		    (interval != l->probe_announced || now_ms - l->last_probe_ms + CG_TICK_MS / 2 >= interval))
			send_probe(c, l, now_us, interval);
		/* Its server address went silent: reconcile moves it (srvpick.h). */
		if (l->fd >= 0 && l->ncand > 1 &&
		    cg_srv_due(now_ms, l->cand_since_ms, l->last_reply_ms, c->cfg->server_failover_ms))
			failover = 1;
	}
	health_tick(c, now_ms);
	names_tick(c, now_ms);
	if (c->pctl)
		cg_pctl_tick(c->pctl);
	if (failover || now_ms >= c->next_reconcile_ms)
		reconcile(c, now_ms);
	if (now_ms >= c->next_path_ms)
		path_check(c, now_ms);
	if (c->sw.running && now_ms >= c->next_status_ms) {
		write_status(c, now_ms);
		c->next_status_ms = now_ms + c->cfg->status_interval_ms;
	}
	cg_ctl_expire(&c->ctl, c->ep, now_ms);
}

/* Reads the signals: 1 to stop, 0 to go on (SIGHUP starts a reload). */
static int signals(struct client *c)
{
	struct signalfd_siginfo si;
	int stop = 0;

	while (read(c->run->sigfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		if (si.ssi_signo != SIGHUP) {
			stop = 1;
		} else {
			cg_info("SIGHUP: reloading %s", c->run->path);
			reload_start(c);
		}
	}
	return stop;
}

/* The output batch to the TUN (every mode), and the pump of link_threads
 * off (one, inline) or the bell of on (whose threads start with their
 * links). Returns 0 or -1. */
static int pumps_init(struct client *c)
{
	c->dmsg = calloc(DOUT_MAX, sizeof(*c->dmsg));
	c->diov = calloc(DOUT_MAX, sizeof(*c->diov));
	if (!c->dmsg || !c->diov)
		return -1;
	if (!pumped(c))
		return 0;
	if (c->lt == CG_LT_ON) {
		if (cg_initial_cpus(&c->cpus) < 0)
			CPU_ZERO(&c->cpus);
		return cg_bell_init(&c->bell) < 0 || cg_epoll_add(c->ep, c->bell.efd, CG_EV(CG_EV_BELL, 0)) < 0 ? -1 : 0;
	}
	c->pump[0] = pump_new();
	if (!c->pump[0] || cg_pump_init(c->pump[0], 0, c->ep, CG_PUMP_RXQ_INLINE, 0) < 0) {
		free(c->pump[0]);
		c->pump[0] = NULL;
		return -1;
	}
	c->npumps = 1;
	return 0;
}

/* Stops the pump threads, each within 1 s; one that does not makes the
 * process leave at once (procd starts it again), without freeing what that
 * thread may still use. */
static void pumps_stop(struct client *c)
{
	for (int k = 0; k < c->npumps; k++)
		if (cg_pump_stop(c->pump[k], 1000) < 0) {
			cg_err("thread %s did not stop within 1 s: leaving at once", c->pump[k]->name);
			_exit(1);
		}
}

/* Closes what the pumps hold, and the sockets stalled pumps never took,
 * and frees them. */
static void pumps_free(struct client *c)
{
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].pend.op == CG_PUMP_OPEN)
			close(c->link[i].pend.fd);
	for (int k = 0; k < c->npumps; k++) {
		cg_pump_free(c->pump[k]);
		free(c->pump[k]);
		c->pump[k] = NULL;
	}
	c->npumps = 0;
	free(c->dmsg);
	free(c->diov);
	c->dmsg = NULL;
	c->diov = NULL;
	cg_bell_free(&c->bell);
}

int cg_client_run(struct cg_config *cfg, const struct cg_run *run)
{
	struct client *c = aligned_alloc(CG_CACHELINE, sizeof(*c)); /* sizeof: a multiple of its alignment */
	char err[256], buf[64];
	uint64_t last_traffic_us = 0;
	uint32_t busy;
	int rc = 1;

	if (!c) {
		cg_err("out of memory");
		cg_config_free(cfg);
		free(cfg);
		return 1;
	}
	memset(c, 0, sizeof(*c));
	/* the erasure codes (story 021): GF(2^8)'s tables (rlc.h) and Leopard
	 * (rs.h), before any thread; rs falls back to rlc without Leopard */
	cg_gf_init(&c->sp_gf);
	if (cg_rs_init() < 0 && cfg->redund_spare == CG_SPARE_RS)
		cg_warn("redund_spare = rs: no Leopard here, rlc instead");
	if (cg_random(&c->flow_seed, sizeof(c->flow_seed)) < 0)
		c->flow_seed = (uint32_t)cg_now_us();
	c->cfg = cfg;
	c->run = run;
	c->bell.efd = -1;
	{
		cpu_set_t set;
		long n = sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : sysconf(_SC_NPROCESSORS_ONLN);

		c->ncpus = n > 0 ? (int)n : 1;
	}
	c->lt = cg_lt_resolve(cfg->link_threads, CG_LT_ARCH_MEASURED, c->ncpus, cfg->cpu >= 0, CG_LT_AUTO_ON);
	cg_gov_reset(&c->gov);
	cg_dom_reset(&c->dom);
	for (int i = 0; i < CG_MAX_LINKS; i++) {
		cg_cap_reset(&c->cap[i]);
		cg_sig_reset(&c->sig[i]);
		c->cw[i] = CG_GOV_MAX;
	}
	cg_gov_weights(&c->gov, 0, c->gw);
	c->txs.k_tx = cfg->key;
	c->txs.hint = cg_client_hint(cfg->key);
	c->rxs.k_rx = cfg->key + CG_SIPHASH_KEY_LEN;
	c->ep = c->tun_fd = c->tfd = -1;
	c->nl.fd = -1;
	cg_names_init(&c->names);
	c->server_pass = -2;
	c->hcfg = cg_hcfg_of(cfg);
	cg_ctl_init(&c->ctl);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		link_set_fd(&c->link[i], -1);
	if (cg_loader_init(&c->loader) < 0) {
		cg_err("eventfd: %s", strerror(errno));
		goto out;
	}
	cg_replay_reset(&c->rxs.replay);
	cg_replay_reset(&c->rxs.ctl);
	do {
		if (cg_random(&c->session, sizeof(c->session)) < 0 ||
		    cg_random(&c->txs.seq, sizeof(c->txs.seq)) < 0 ||
		    cg_random(&c->txs.seq_ctl, sizeof(c->txs.seq_ctl)) < 0) {
			cg_err("getrandom: %s", strerror(errno));
			goto out;
		}
	} while (!c->session);
	c->txs.session = c->rxs.session = c->session;
	c->start_ms = cg_now_ms();
	cg_act_tx_start(&c->act_tx, cg_now_us());

	c->tun_fd = cg_tun_open(cfg, err, sizeof(err));
	if (c->tun_fd < 0) {
		cg_err("%s", err);
		goto out;
	}
	if (cg_nl_open(&c->nl, err, sizeof(err)) < 0) {
		cg_err("%s", err);
		goto out;
	}
	c->ep = epoll_create1(EPOLL_CLOEXEC);
	c->tfd = cg_timerfd(CG_TICK_MS);
	if (c->ep < 0 || c->tfd < 0 || cg_epoll_add(c->ep, c->tun_fd, CG_EV(CG_EV_TUN, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->nl.fd, CG_EV(CG_EV_NL, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->tfd, CG_EV(CG_EV_TIMER, 0)) < 0 ||
	    cg_epoll_add(c->ep, run->sigfd, CG_EV(CG_EV_SIG, 0)) < 0 ||
	    cg_epoll_add(c->ep, c->loader.efd, CG_EV(CG_EV_LOAD, 0)) < 0) {
		cg_err("epoll setup: %s", strerror(errno));
		goto out;
	}
	if (pumps_init(c) < 0) {
		cg_err("link pumps: out of memory");
		goto out;
	}
	if (cg_ro_pool_alloc(&c->pool, cfg->reorder_pool_kb) < 0) {
		cg_err("reorder_pool_kb = %u: out of memory", cfg->reorder_pool_kb);
		goto out;
	}
	if (cfg->rescue_kb) {
		uint32_t n = (uint32_t)((uint64_t)cfg->rescue_kb * 1024 / CG_RESC_ENTRY);

		c->resc.mem = malloc((size_t)n * CG_RESC_ENTRY);
		c->resc.sess = calloc(n, sizeof(*c->resc.sess));
		c->resc.seq = calloc(n, sizeof(*c->resc.seq));
		c->resc.len = calloc(n, sizeof(*c->resc.len));
		c->resc.link = calloc(n, sizeof(*c->resc.link));
		c->resc.sent = calloc(n, sizeof(*c->resc.sent));
		c->resc_idx = calloc(CG_RESC_IDX, sizeof(*c->resc_idx));
		if (!c->resc.mem || !c->resc.sess || !c->resc.seq || !c->resc.len || !c->resc.link || !c->resc.sent ||
		    !c->resc_idx) {
			cg_err("rescue_kb = %u: out of memory", cfg->rescue_kb);
			goto out;
		}
		c->resc.n = n;
		/* the RS block of a PARQ, encoded again (harq.h) */
		if (cg_rs_ready() && !(c->parq_rs = aligned_alloc(64, sizeof(*c->parq_rs)))) {
			cg_err("rescue_kb = %u: out of memory", cfg->rescue_kb);
			goto out;
		}
	}
	c->rocf = cg_ro_cfg_of(cfg);
	rescue_apply(c);
	f3_apply(c);
	/* FEC v3's repairs of the download (story 026): the ring of the DATA got
	 * lately and the RLC's equations, with fec (it restarts the engine) */
	if (cfg->fec) {
		size_t n = CG_FEC_RING;
		uint8_t *b = calloc(1, sizeof(struct cg_fec_rx) + n * (sizeof(uint32_t) + sizeof(uint16_t) + 2) +
					       n * CG_FEC_PLEN_MAX);

		if (b) {
			c->dfec = (struct cg_fec_rx *)b;
			b += sizeof(*c->dfec);
			c->dfec->seq = (uint32_t *)b;
			b += n * sizeof(uint32_t);
			c->dfec->len = (uint16_t *)b;
			b += n * sizeof(uint16_t);
			c->dfec->have = b;
			b += n;
			c->dfec->pidx = b;
			b += n;
			c->dfec->mem = b;
			if (!(c->drlc = calloc(1, sizeof(*c->drlc)))) {
				free(c->dfec);
				c->dfec = NULL;
			}
		}
		if (!c->dfec)
			cg_warn("fec: out of memory for the download's ring, its repairs are dropped");
	}
	if (cfg->protocontroller) {
		struct cg_pctl_cfg pc = { .server = 0,
					 .port = (uint16_t)cfg->protocontroller_port,
					 .mss = cfg->tun_mtu - CG_PCTL_OVERHEAD,
					 .buf_kb = cfg->protocontroller_buf_kb,
					 .mem_mb = cfg->protocontroller_mem_mb,
					 .max_flows = cfg->protocontroller_max_flows,
					 .queue_ms = cfg->protocontroller_queue_ms,
					 .backlog_ms = cfg->protocontroller_backlog_ms,
					 .dead_s = cfg->protocontroller_dead_s,
					 .reo_ms = cfg->protocontroller_reo_ms,
					 /* the upload's codes: the spatial parity or FEC v3, or what
					  * redundancy's spare carries (parity, rs, rlc) */
					 .code_ms = cfg->fec || cfg->redund_spare >= 2 ? cfg->rescue_code_ms : 0,
					 .mode = cfg->protocontroller };
		struct cg_pctl_ops ops = { .ctx = c, .send = pctl_send, .hint = pctl_hint, .horizon = pctl_horizon };

		snprintf(pc.mid_cc, sizeof(pc.mid_cc), "%s", cfg->protocontroller_mid_cc);
		if (cfg->protocontroller == CG_PCTL_A && pctl_vps_addr(cfg, &pc.a_peer, err, sizeof(err)) < 0) {
			cg_err("%s", err);
			goto out;
		}
		if (!(c->pctl = cg_pctl_new(&pc, c->ep, &ops, err, sizeof(err)))) {
			cg_err("%s", err);
			goto out;
		}
		if (cfg->protocontroller == CG_PCTL_A)
			cg_info("protocontroller: taking the TCP redirected to port %u, a TCP to %s through the tunnel for each (variant A, story 025)",
				cfg->protocontroller_port, cg_addr_str(&pc.a_peer, buf, sizeof(buf)));
		else
			cg_info("protocontroller: taking the TCP redirected to port %u, a stream of cengarde's for each (variant B, story 025)",
				cfg->protocontroller_port);
	}
	/* The control socket is a convenience: the tunnel runs without it. */
	if (cfg->control_socket[0] && cg_ctl_open(&c->ctl, cfg->control_socket, c->ep, err, sizeof(err)) < 0)
		cg_warn("control socket: %s", err);
	if (cfg->status_file[0] && cg_status_writer_start(&c->sw, cfg->status_file, "cg-status") < 0)
		cg_warn("status file %s: cannot start the writer thread", cfg->status_file);
	cg_info("client %s: session %08x, native tunnel on %s (MTU %u; authenticated, not encrypted), "
		"link_threads %s",
		CG_VERSION, c->session, cfg->tun, cfg->tun_mtu, cg_lt_name(c->lt));
	if (cfg->passthrough >= 0)
		cg_info("asking the server for IP pass %s", cfg->passthrough ? "on" : "off");
	busy = cg_tune(cfg);
	cg_names_set(&c->names, c->ep, cfg);
	lists_build(c, cfg);
	reconcile(c, c->start_ms);

	for (;;) {
		struct epoll_event ev[32];
		int timeout = busy && cg_now_us() - last_traffic_us < busy ? 0 : -1, armed = 0, n, traffic = 0;
		int due = ro_timeout(c), fdue = fec_timeout(c), pdue = c->pctl ? cg_pctl_timeout(c->pctl) : -1;

		/* The reorderer's next deadline wakes the loop up, if nothing
		 * else does before (CG_TICK_MS is too coarse for it); and an
		 * open FEC group's. */
		if (due >= 0 && (timeout < 0 || due < timeout))
			timeout = due;
		if (fdue >= 0 && (timeout < 0 || fdue < timeout))
			timeout = fdue;
		if (pdue >= 0 && (timeout < 0 || pdue < timeout))
			timeout = pdue;
		c->ro_budget = CG_RO_PASS_MAX;
		/* link_threads = on: the pumps ring this loop's bell when they
		 * publish and it said it sleeps (ring.h), for good or until a
		 * deadline: a sleep with a timeout needs the bell too. */
		if (c->lt == CG_LT_ON && timeout != 0) {
			cg_bell_arm(&c->bell);
			armed = 1;
			for (int k = 0; k < c->npumps && timeout != 0; k++)
				if (cg_ring_has_work_sc(&c->pump[k]->rxq))
					timeout = 0;
		}
		n = epoll_wait(c->ep, ev, 32, timeout);
		if (armed)
			cg_bell_disarm(&c->bell);
		if (n < 0 && errno != EINTR) {
			cg_err("epoll_wait: %s", strerror(errno));
			goto out;
		}
		for (int i = 0; i < n; i++) {
			uint32_t kind = (uint32_t)(ev[i].data.u64 >> 32), idx = (uint32_t)ev[i].data.u64;

			switch (kind) {
			case CG_EV_TUN:
				tun_read(c);
				traffic = 1;
				break;
			case CG_EV_LINK:
				if (idx >= CG_MAX_LINKS)
					break;
				if (c->lt == CG_LT_OFF)
					inline_read(c, idx);
				else if (c->link[idx].fd >= 0)
					link_read(c, &c->link[idx]);
				traffic = 1;
				break;
			case CG_EV_NL:
				cg_nl_read(&c->nl);
				if (c->nl.changed) {
					c->nl.changed = 0;
					reconcile(c, cg_now_ms());
				}
				break;
			case CG_EV_TIMER: {
				uint64_t expirations = 0;

				uint64_t t0;

				if (read(c->tfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
					cg_warn("timerfd: %s", strerror(errno));
				else if (expirations > 1)
					c->ticks_missed += expirations - 1;
				t0 = cg_now_us();
				tick(c);
				t0 = cg_now_us() - t0;
				if (t0 > c->tick_max_us)
					c->tick_max_us = t0;
				break;
			}
			case CG_EV_CTL: {
				uint64_t now_ms = cg_now_ms();
				int k = cg_ctl_event(&c->ctl, c->ep, idx, now_ms);

				if (k >= 0)
					ctl_command(c, k, now_ms);
				break;
			}
			case CG_EV_LOAD:
				if (reload_done(c) < 0)
					goto out;
				break;
			case CG_EV_BELL:
				cg_bell_drain(&c->bell);
				break;
			case CG_EV_DNS: {
				uint64_t now_ms = cg_now_ms();

				if (cg_names_read(&c->names, c->ep, idx, now_ms))
					names_changed(c, now_ms);
				break;
			}
			case CG_EV_PCTL:
				if (c->pctl)
					cg_pctl_event(c->pctl, idx, ev[i].events);
				break;
			case CG_EV_SIG:
				if (signals(c)) {
					cg_info("client stopping");
					rc = 0;
					goto out;
				}
				break;
			}
		}
		/* What the pump threads read: a few passes, then epoll again for
		 * the TUN, the timer and the rest. */
		for (int r = 0; c->lt == CG_LT_ON && r < CG_MAX_ROUNDS && hub_drain(c) > 0; r++)
			traffic = 1;
		ro_service(c);
		fec_service(c);
		if (c->pctl)
			cg_pctl_service(c->pctl);
		if (traffic && busy)
			last_traffic_us = cg_now_us();
	}
out:
	cg_status_writer_stop(&c->sw);
	cg_pctl_free(c->pctl);
	cg_ctl_close(&c->ctl, c->ep);
	cg_names_close(&c->names, c->ep);
	cg_loader_free(&c->loader);
	for (int i = 0; i < CG_MAX_LINKS; i++)
		if (c->link[i].fd >= 0 && !pumped(c))
			close(c->link[i].fd);
	pumps_stop(c);
	pumps_free(c); /* they close the link sockets they hold */
	cg_ro_pool_free(&c->pool);
	free(c->f3w);
	free(c->dfec);
	free(c->drlc);
	free(c->resc.mem);
	free(c->parq_rs);
	free(c->resc.sess);
	free(c->resc.seq);
	free(c->resc.len);
	free(c->resc.link);
	free(c->resc.sent);
	free(c->resc_idx);
	if (c->tun_fd >= 0)
		close(c->tun_fd);
	if (c->tfd >= 0)
		close(c->tfd);
	if (c->ep >= 0)
		close(c->ep);
	cg_nl_close(&c->nl);
	cg_config_free(c->cfg);
	free(c->cfg);
	free(c);
	return rc;
}
