#!/usr/bin/env python3
"""Post-processing of the bonding lab (bench/lab.d/bond.sh, story 012).

  bondtab.py line  OPTIONS    one run -> its JSON line (stdout); exit 1 when the run
                              had no traffic or no ping through the tunnel
  bondtab.py show  FILE       the last line of FILE in one line of text
  bondtab.py delta BEFORE AFTER
                              the engine counters between two snapshots (JSON, stdout)
  bondtab.py zero  FILE NAME...
                              exit 1 when a counter NAME (too_old, wireguard_drops,
                              pool_full, lost_all, ...) is above 0 in a line of FILE
  bondtab.py table FILE [--ref A,B]
                              the summary tables (Markdown) of the lines of FILE; each
                              variant against its family's references, or with --ref
                              against the first of A,B its group has

A snapshot is a prefix: PREFIX.client.json and PREFIX.server.json (the engine's
status files), PREFIX.wgc.json and PREFIX.wgs.json (ip -s -j link show of the
WireGuard interfaces). Anything missing, an engine without the reorder status
object included, gives null and never an error.

The rules of the tables (what "two modes" means, the comparison against a
reference variant) are in the docstrings of modes() and compare(); the JSON
lines are described in bench/README.md.
"""

import argparse
import json
import math
import re
import statistics
import sys

sys.dont_write_bytecode = True

# ---- the engine's counters in the status JSON (spec 7.3) --------------------------

# flat name -> path in the "reorder" object
RO_PATHS = {
    "in_order": ("in_order",),
    "held_total": ("held_total",),
    "bypassed": ("bypassed",),
    "split_rx": ("split_rx",),
    "lost_rack": ("lost", "rack"),
    "lost_cap": ("lost", "cap"),
    "lost_overflow": ("lost", "overflow"),
    "lost_flush": ("lost", "flush"),
    "late_after_rack": ("late", "after_rack"),
    "late_after_cap": ("late", "after_cap"),
    "late_after_overflow": ("late", "after_overflow"),
    "late_old": ("late", "old"),
    "far": ("far",),
    "resyncs": ("resyncs",),
    "pool_full": ("pool_full",),
    "held_max": ("held_max",),
    "max_burst": ("max_burst",),
    "asked": ("rescue", "asked"),
    "rescued": ("rescue", "rescued"),
    "ask_lost": ("rescue", "lost"),
    "ask_repaired": ("rescue", "repaired"),
}
GAUGES = ("held_max", "max_burst")  # maxima since the engine started: not differences
HISTS = ("wait", "late_by")  # 10 buckets each
# What an end sent: data packets ("sent"), the sends of them over all its links
# ("tx_links": sent once per link a packet went on) and the sends a link's socket
# refused for a full queue ("tx_drops"). tx_links over sent is how many times a
# packet went out: 1 for a pure split, the number of links when copied.
SENDS = ["sent", "tx_links", "tx_drops", "tx_bytes"]
# The loss repair (story 023): what the rescue sent again, what the erasure
# codes rebuilt, the parity asked for again (PARQ) and the TCP retransmissions
# the sender kept to one link.
REPAIR = ["resc_sent", "repaired", "parq_asked", "parq_sent", "retx_one", "retx_copied", "e2e_holes", "e2e_beyond",
          "left_tcp"]
# tcp_ack_rule (tcpack.h, story 023): the sending end's retransmissions by what was
# known of their originals (tr_*), of them copied (trc_*), and the ACKs the exit end
# marked CG_F_TUNHOLE (e2e_marked, where TCP leaves the tunnel).
RULE_ROWS = ["unknown", "spurious", "delivered", "tunnel"]
E2E_MORE = [("e2e_marked", "marked"), ("e2e_own", "tunnel"), ("e2e_inflight", "in_flight"), ("e2e_unvouched", "unvouched")]
REPAIR += ["tr_" + r for r in RULE_ROWS] + ["trc_" + r for r in RULE_ROWS] + [k for k, _ in E2E_MORE]
# What the codes rebuilt wrong (story 025): the decoders' bad counts, what the
# server did not write to the TUN for not being IP, and the guard on rebuilt
# packets (rebuilt.h): passed, garbage, not confirmable. Server only.
GUARD = [("rlc_bad", ("repair", "rlc", "bad")), ("rs_bad", ("repair", "rs", "bad")), ("not_ip", ("not_ip",)),
         ("rb_ok", ("rebuilt", "ok")), ("rb_bad", ("rebuilt", "bad")), ("rb_unsure", ("rebuilt", "unsure"))]
REPAIR += [k for k, _ in GUARD]
# FEC (story 026): the XOR parity's and FEC v3's repairs at the sending end (the
# client's upload; the server's download, story 026's second part), its window's
# DATA bytes, the candidates its guard took out, the repairs without a link, the
# batches its lone link's gate kept off; at the receiving end what the XOR parity
# and the RLC rebuilt, the wide repairs, the unknowns evicted for room, the most at
# once (a gauge). (name, the client's path, the session's path)
FEC3 = [("fec_par", ("upload", "fec", "parities"), ("download_fec", "parities")),
        ("fec_par_bytes", ("upload", "fec", "bytes"), ("download_fec", "bytes")),
        ("f3_rep", ("upload", "fec", "rlc", "repairs"), ("download_fec", "rlc", "repairs")),
        ("f3_bytes", ("upload", "fec", "rlc", "repair_bytes"), ("download_fec", "rlc", "repair_bytes")),
        ("f3_data", ("upload", "fec", "rlc", "data_bytes"), ("download_fec", "rlc", "data_bytes")),
        ("f3_guarded", ("upload", "fec", "rlc", "guarded"), ("download_fec", "rlc", "guarded")),
        ("f3_nolink", ("upload", "fec", "rlc", "no_link"), ("download_fec", "rlc", "no_link")),
        ("f3_gated", ("upload", "fec", "rlc", "gated"), ("download_fec", "rlc", "gated")),
        ("parq_bytes", ("rescue", "parq", "bytes"), ("download_fec", "parq_bytes")),
        ("xor_rec", ("download_fec", "xor_recovered"), ("fec", "recovered")),
        ("rlc_rec", ("download_fec", "rlc_recovered"), ("repair", "rlc", "recovered")),
        ("rlc_wide", ("download_fec", "rlc_wide"), ("repair", "rlc", "wide")),
        ("rlc_evicted", ("download_fec", "rlc_evicted"), ("repair", "rlc", "evicted")),
        ("rlc_peak", ("download_fec", "rlc_peak"), ("repair", "rlc", "peak"))]
REPAIR += [k for k, _, _ in FEC3]
GAUGES = GAUGES + ("rlc_peak",)
COUNTERS = [k for k in RO_PATHS if k not in GAUGES] + ["too_old", "wireguard_drops", "lost_all"] + SENDS + REPAIR
NAMES = list(RO_PATHS) + ["too_old", "wireguard_drops", "lost_all"] + SENDS + REPAIR


def num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def dig(d, *path):
    for k in path:
        if not isinstance(d, dict) or k not in d:
            return None
        d = d[k]
    return d


def load(path):
    """A JSON file as a dict (or list), None when it is missing, empty or broken."""
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def flat_reorder(ro):
    """The status "reorder" object as flat names (None for what it does not have)."""
    out = {k: num(dig(ro, *p)) for k, p in RO_PATHS.items()}
    for h in HISTS:
        v = dig(ro, h)
        out[h] = list(v) if isinstance(v, list) and all(num(x) is not None for x in v) else None
    return out


def link_sum(links, key):
    """The sum of key over a status "links" list, None when a link lacks it."""
    if not isinstance(links, list) or not links:
        return None
    vs = [num(dig(l, key)) for l in links]
    return None if any(v is None for v in vs) else sum(vs)


def flat_client(d):
    out = flat_reorder(dig(d, "reorder"))
    out["too_old"] = num(dig(d, "download", "too_old"))
    out["wireguard_drops"] = num(dig(d, "download", "wireguard_drops"))
    out["lost_all"] = num(dig(d, "lost_all"))
    out["sent"] = num(dig(d, "upload", "packets"))
    out["tx_links"] = link_sum(dig(d, "links"), "tx_packets")
    out["tx_drops"] = link_sum(dig(d, "links"), "tx_drops")
    out["tx_bytes"] = link_sum(dig(d, "links"), "tx_bytes")
    out["resc_sent"] = num(dig(d, "rescue", "sent"))
    out["repaired"] = None
    out["parq_asked"] = None
    out["parq_sent"] = num(dig(d, "rescue", "parq", "repairs"))
    out["retx_one"] = num(dig(d, "upload", "tcp", "retx_one"))
    out["retx_copied"] = num(dig(d, "upload", "tcp", "retx_copied"))
    out["e2e_holes"] = num(dig(d, "tcp_e2e", "sack_holes"))
    out["e2e_beyond"] = num(dig(d, "tcp_e2e", "beyond"))
    out["left_tcp"] = num(dig(d, "rescue", "left_to_tcp"))
    for r in RULE_ROWS:
        out["tr_" + r] = num(dig(d, "upload", "tcp", "rule", r))
        out["trc_" + r] = num(dig(d, "upload", "tcp", "rule", "copied", r))
    for k, j in E2E_MORE:
        out[k] = num(dig(d, "tcp_e2e", j))
    for k, _ in GUARD:
        out[k] = None
    for k, cp, _ in FEC3:
        out[k] = num(dig(d, *cp))
    return out


def flat_session(s):
    out = flat_reorder(dig(s, "reorder"))
    out["wireguard_drops"] = num(dig(s, "wireguard_drops"))
    out["lost_all"] = num(dig(s, "lost_all"))
    out["too_old"] = None  # the server counts it for all sessions: rx.too_old
    out["sent"] = num(dig(s, "download_packets"))
    out["tx_links"] = link_sum(dig(s, "links"), "tx_packets")
    out["tx_drops"] = link_sum(dig(s, "links"), "tx_drops")
    out["tx_bytes"] = link_sum(dig(s, "links"), "tx_bytes")
    out["resc_sent"] = num(dig(s, "rescue", "sent"))
    out["repaired"] = num(dig(s, "repair", "recovered"))
    out["parq_asked"] = num(dig(s, "repair", "parq", "asked"))
    out["parq_sent"] = None
    out["retx_one"] = num(dig(s, "tcp", "retx_one"))
    out["retx_copied"] = num(dig(s, "tcp", "retx_copied"))
    out["e2e_holes"] = None
    out["e2e_beyond"] = None
    out["left_tcp"] = num(dig(s, "rescue", "left_to_tcp"))
    for r in RULE_ROWS:
        out["tr_" + r] = num(dig(s, "tcp", "rule", r))
        out["trc_" + r] = num(dig(s, "tcp", "rule", "copied", r))
    for k, _ in E2E_MORE:
        out[k] = None
    for k, path in GUARD:
        out[k] = num(dig(s, *path))
    for k, _, sp in FEC3:
        out[k] = num(dig(s, *sp))
    return out


def sub(a, b):
    """a - b for the counters of two flat dicts; the gauges and histograms of a, or their
    differences. b None: a is all there is (a session that is new). A counter that went
    down (the engine restarted) counts as its new value."""
    out = {}
    for k in NAMES:
        x = a.get(k)
        y = None if b is None else b.get(k)
        if x is None or (b is not None and y is None):
            out[k] = None
        elif k in GAUGES or b is None:
            out[k] = x
        else:
            out[k] = x - y if x >= y else x
    for h in HISTS:
        x = a.get(h)
        y = None if b is None else b.get(h)
        if x is None or (b is not None and (y is None or len(x) != len(y))):
            out[h] = None
        elif b is None:
            out[h] = list(x)
        else:
            out[h] = [p - q if p >= q else p for p, q in zip(x, y)]
    return out


def add(items):
    """Sums of the deltas of several sessions: counters and histograms add up, gauges are
    the largest; None when no session had the field."""
    out = {}
    for k in NAMES:
        vs = [i[k] for i in items if i.get(k) is not None]
        out[k] = (max(vs) if k in GAUGES else sum(vs)) if vs else None
    for h in HISTS:
        vs = [i[h] for i in items if i.get(h) is not None]
        out[h] = [sum(c) for c in zip(*vs)] if vs else None
    return out


def delta_client(b, a):
    if not isinstance(a, dict) or not isinstance(b, dict):
        return sub({}, None)  # a snapshot is missing: nothing is known
    return sub(flat_client(a), flat_client(b))


def delta_server(b, a):
    """The server's counters: its sessions' (new ones from zero) and rx.too_old."""
    if not isinstance(a, dict) or not isinstance(b, dict):
        return sub({}, None)
    bs = {}
    if isinstance(b.get("sessions"), list):
        bs = {s.get("id"): s for s in b["sessions"] if isinstance(s, dict)}
    sess = a.get("sessions")
    items = []
    for s in sess if isinstance(sess, list) else []:
        if isinstance(s, dict):
            items.append(sub(flat_session(s), flat_session(bs[s.get("id")]) if s.get("id") in bs else None))
    out = add(items)
    old_a, old_b = num(dig(a, "rx", "too_old")), num(dig(b, "rx", "too_old"))
    out["too_old"] = None if old_a is None or old_b is None else (old_a - old_b if old_a >= old_b else old_a)
    # where the upload's TCP leaves the tunnel: the server's, for all sessions (story 023)
    for k, path in [("e2e_holes", ("tcp_e2e", "sack_holes")), ("e2e_beyond", ("tcp_e2e", "beyond"))] + \
            [(k, ("tcp_e2e", j)) for k, j in E2E_MORE]:
        x, y = num(dig(a, *path)), num(dig(b, *path))
        out[k] = None if x is None or y is None else (x - y if x >= y else x)
    return out


def rx_dropped(path):
    j = load(path)
    return num(dig(j[0] if isinstance(j, list) and j else None, "stats64", "rx", "dropped"))


def delta(before, after):
    """The engine counters between two snapshots (prefixes): {"eng": {"cli", "srv"}, "wg_rx_dropped"}."""
    wg = {}
    for side, name in (("cli", "wgc"), ("srv", "wgs")):
        x, y = rx_dropped(f"{after}.{name}.json"), rx_dropped(f"{before}.{name}.json")
        wg[side] = None if x is None or y is None else (x - y if x >= y else x)
    return {
        "eng": {
            "cli": delta_client(load(f"{before}.client.json"), load(f"{after}.client.json")),
            "srv": delta_server(load(f"{before}.server.json"), load(f"{after}.server.json")),
        },
        "wg_rx_dropped": wg,
    }


# ---- one run -------------------------------------------------------------------

def pings(path):
    try:
        rtts = [float(m) for m in re.findall(r"time=([0-9.]+) ms", open(path).read())]
    except OSError:
        return []
    rtts.sort()
    return rtts


def pct(rtts, p):
    return rtts[min(len(rtts) - 1, int(p * len(rtts)))] if rtts else None


def udpgen_fields(path):
    """The key=value pairs of a udpgen summary line, as ints."""
    try:
        text = open(path).read()
    except OSError:
        return {}
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def tail_mbps(j, secs):
    """iperf3's throughput over the run's second half (its 1 s intervals), None when
    it has none: the steady state, after the start from zero of the capacities."""
    try:
        iv = [x["sum"] for x in j.get("intervals", []) if x["sum"]["start"] >= secs / 2]
        t = sum(x["seconds"] for x in iv)
        return round(sum(x["bytes"] for x in iv) * 8 / t / 1e6, 2) if t > 0 else None
    except (KeyError, TypeError, AttributeError):
        return None


def end_links(d, side):
    """The links of an end's status: the client's, or the server's busiest session's."""
    if not isinstance(d, dict):
        return None
    if side == "cli":
        return d.get("links") if isinstance(d.get("links"), list) else None
    best = None
    for s in d.get("sessions") or []:
        if isinstance(s, dict) and isinstance(s.get("links"), list):
            v = (num(s.get("download_bytes")) or 0) + (num(s.get("upload_bytes")) or 0)
            if best is None or v > best[0]:
                best = (v, s)
    return best[1]["links"] if best else None


def end_reorder(d, side):
    if not isinstance(d, dict):
        return None
    if side == "cli":
        return flat_client(d)
    for s in d.get("sessions") or []:
        if isinstance(s, dict) and isinstance(s.get("links"), list):
            return flat_session(s)
    return None


def sig_summary(sdir, secs, tx, i, half):
    """Link i's observations (linksig.h): the fraction of the second half's samples
    (half) with each queue_owner and loss_kind, and its verdicts from the run's first
    sample to its last (ep_mine, ep_others, ep_unknown)."""
    pre = {"cli": "c", "srv": "s"}[tx]
    out = {}
    sigs = [ls[i]["sig"] for ls in (end_links(s, tx) for s in half)
            if ls and i < len(ls) and isinstance(ls[i].get("sig"), dict)]
    for key, vals in (("queue_owner", ("mine", "others", "unknown")), ("loss_kind", ("random", "congestive", "none"))):
        for v in vals:
            out[v] = round(sum(1 for s in sigs if s.get(key) == v) / len(sigs), 3) if sigs else None
    run = [load(f"{sdir}/{pre}{k}.json") for k in range(1, int(secs) + 1)]
    run = [ls[i]["sig"] for ls in (end_links(s, tx) for s in run if isinstance(s, dict))
           if ls and i < len(ls) and isinstance(ls[i].get("sig"), dict)]
    for k in ("mine", "others", "unknown"):
        a, b = (num(run[0].get("episodes_" + k)), num(run[-1].get("episodes_" + k))) if len(run) >= 2 else (None, None)
        out["ep_" + k] = None if a is None or b is None else b - a
    return out


def samples_summary(sdir, secs, direction):
    """Each link over the run's second half, from BOND_SAMPLE's status files (cN.json,
    sN.json, one a second): at the end that sends the bulk (the client up, the server
    down) its share of the bytes, mean capacity, queue and expected-arrival rate, and
    the fraction of samples it was lossy (capacity.h) and, when the engine has them,
    its observations (linksig.h): the fraction of samples with each queue_owner and
    loss_kind, and its verdicts over the whole run (ep_*); at the receiving end its
    late copies (rx_late) and the reorderer's gaps given up and late arrivals."""
    tx, rx = ("cli", "srv") if direction == "up" else ("srv", "cli")
    pre = {"cli": "c", "srv": "s"}
    first = int(secs / 2) + 1
    snaps = {k: [load(f"{sdir}/{pre[k]}{i}.json") for i in range(first, int(secs) + 1)] for k in ("cli", "srv")}
    snaps = {k: [x for x in v if isinstance(x, dict)] for k, v in snaps.items()}
    if len(snaps[tx]) < 2:
        return None, None
    a, b = end_links(snaps[tx][0], tx), end_links(snaps[tx][-1], tx)
    if not a or not b or len(a) != len(b):
        return None, None
    sent = [max(0, (num(y.get("tx_bytes")) or 0) - (num(x.get("tx_bytes")) or 0)) for x, y in zip(a, b)]
    total = sum(sent) or 1
    out = []
    for i, l in enumerate(b):
        def mean(key, sub=None, i=i):
            vs = []
            for snap in snaps[tx]:
                ls = end_links(snap, tx)
                if ls and i < len(ls):
                    v = ls[i].get(sub, {}).get(key) if sub else ls[i].get(key)
                    if isinstance(v, bool):
                        v = 1.0 if v else 0.0
                    if num(v) is not None:
                        vs.append(v)
            return round(sum(vs) / len(vs), 3) if vs else None
        out.append({"type": l.get("type"), "share": round(sent[i] / total, 3), "mbit": round(sent[i] * 8 / 1e6 / max(1, len(snaps[tx]) - 1), 2),
                    "cap": mean("capacity_mbit"), "eta": mean("eta_mbit"), "queue": mean("queue_ms"),
                    "lossy": mean("lossy", "cap_diag")})
        if isinstance(l.get("sig"), dict):
            out[-1]["sig"] = sig_summary(sdir, secs, tx, i, snaps[tx])
    # netem's own drops on each link, the sending side (bond.sh's lN in cli, sN in srv)
    tq = [load(f"{sdir}/t{pre[tx]}{i}.json") for i in (first, int(secs))]
    if all(isinstance(x, list) for x in tq):
        def qd(snap, dev):
            for q in snap:
                if isinstance(q, dict) and q.get("dev") == dev and q.get("kind") == "netem":
                    return num(q.get("drops")), num(q.get("packets"))
            return None, None
        for k, o in enumerate(out):
            dev = ("l%d" if tx == "cli" else "s%d") % (k + 1)
            (d0, p0), (d1, p1) = qd(tq[0], dev), qd(tq[1], dev)
            if None not in (d0, p0, d1, p1):
                o["drops"] = d1 - d0
                o["drop_pct"] = round(100 * (d1 - d0) / max(1, (p1 - p0) + (d1 - d0)), 3)
    ra = end_links(snaps[rx][0], rx) if len(snaps[rx]) >= 2 else None
    rb = end_links(snaps[rx][-1], rx) if len(snaps[rx]) >= 2 else None
    if ra and rb and len(ra) == len(rb) == len(out):
        for o, x, y in zip(out, ra, rb):
            for k in ("rx_late", "rx_reorder_late", "rx_first"):
                vx, vy = num(x.get(k)), num(y.get(k))
                o[k] = None if vx is None or vy is None else vy - vx
    tail = None
    if len(snaps[rx]) >= 2:
        d = sub(end_reorder(snaps[rx][-1], rx) or {}, end_reorder(snaps[rx][0], rx) or {})
        tail = {k: d.get(k) for k in ("held_total", "lost_rack", "lost_cap", "lost_overflow", "late_after_rack",
                                      "late_after_cap", "late_old")}
    return out, tail


def line(a):
    # What does not apply to a kind of run stays null: TCP does not say how many
    # packets it sent, and QUIC has no "retransmits" but the packets it declared lost.
    extra = {"pkts_sent": None, "spurious": None, "srtt_ms": None, "min_rtt_ms": None, "quic_cc": None}
    segs = retrans_pct = mbps_tail = None
    j = None
    try:
        j = json.load(open(a.result))
        if a.cc in ("live", "soak"):  # story 024: livesim.py merge's, the far stream's numbers (025: soak too)
            far = j["far"]
            mbps, retrans = far["mbps"], far.get("send", {}).get("total_retrans")
            extra["srtt_ms"] = far.get("send", {}).get("srtt_ms")
        elif a.cc == "quic":
            mbps, retrans = j["mbps"], j["lost"]
            extra = {"pkts_sent": j["sent"], "spurious": j["spurious"], "srtt_ms": j["srtt_ms"],
                     "min_rtt_ms": j["min_rtt_ms"], "quic_cc": j["cc"]}
            if j["sent"]:
                retrans_pct = round(100.0 * retrans / j["sent"], 4)
        else:
            end = j["end"]
            mbps = end["sum_received"]["bits_per_second"] / 1e6
            mbps_tail = tail_mbps(j, int(a.secs))
            retrans = end["sum_sent"].get("retransmits")
            snd = end["streams"][0]["sender"]  # the sender's TCP_INFO RTTs, in microseconds
            us = lambda k: round(snd[k] / 1000, 2) if snd.get(k) else None
            extra.update(srtt_ms=us("mean_rtt"), min_rtt_ms=us("min_rtt"))
            # The segments the sender wrote: its bytes over the connection's MSS (iperf3
            # does not count segments); the retransmitted ones come on top.
            mss = dig(j, "start", "tcp_mss_default")
            if retrans is not None and num(mss) and end["sum_sent"].get("bytes"):
                segs = int(end["sum_sent"]["bytes"] / mss)
                if segs:
                    retrans_pct = round(100.0 * retrans / segs, 4)
    except Exception:
        mbps, retrans = 0.0, None
    rtts = pings(a.ping)
    big = pings(a.ping_big) if a.ping_big else []
    out = {"case": a.case, "variant": a.variant, "dir": a.dir, "cc": a.cc, "rep": int(a.rep), "secs": int(a.secs),
           "mbps": round(mbps, 2), "mbps_tail": mbps_tail, "retrans": retrans, **extra, "segs": segs, "retrans_pct": retrans_pct,
           "rtt_p50": pct(rtts, 0.5), "rtt_p99": pct(rtts, 0.99), "pings": len(rtts),
           "rtt_big_p50": pct(big, 0.5), "rtt_big_p99": pct(big, 0.99), "pings_big": len(big),
           "cpu_client_pct": round(100 * int(a.cpu_client) / int(a.hz) / int(a.secs), 1),
           "cpu_server_pct": round(100 * int(a.cpu_server) / int(a.hz) / int(a.secs), 1), "netem": a.netem == "yes"}
    if a.small_rx:  # the flow of small packets alongside (BOND_SMALL)
        rx, tx = udpgen_fields(a.small_rx), udpgen_fields(a.small_tx) if a.small_tx else {}
        out["small"] = {"sent": tx.get("sent"), "uniq": rx.get("uniq"), "p50_us": rx.get("p50_us"),
                        "p99_us": rx.get("p99_us"), "reord": rx.get("reord")} if rx else None
    if a.cc in ("live", "soak") and isinstance(j, dict):
        out["live"] = j
    if a.pep:  # story 024's hosts: each leg's retransmissions, the proxies' memory
        try:
            out["pep"] = json.load(open(a.pep))
        except (OSError, ValueError):
            out["pep"] = None
    if a.before and a.after:
        out.update(delta(a.before, a.after))
    if a.samples:  # BOND_SAMPLE: each link over the second half
        out["links"], out["tail"] = samples_summary(a.samples, int(a.secs), a.dir)
    print(json.dumps(out))
    return 0 if mbps > 0.5 and rtts else 1


def show(path):
    rows = [json.loads(ln) for ln in open(path) if ln.strip()]
    d = rows[-1]
    sp = "" if d.get("spurious") is None else " (spurious {})".format(d["spurious"])
    rate = "" if d.get("retrans_pct") is None else " ({:.2f} %)".format(d["retrans_pct"])
    big = "" if d.get("rtt_big_p50") is None else " big {} / {} ms".format(d["rtt_big_p50"], d["rtt_big_p99"])
    sm = d.get("small")
    small = " small {} / {} us".format(sm["p50_us"], sm["p99_us"]) if sm else ""
    print("   {case:<10} {variant:<12} {dir:<4} {cc:<5} #{rep} {mbps:7.1f} Mbit/s retrans {retrans}".format(**d)
          + rate + sp + " rtt p50 {rtt_p50} p99 {rtt_p99} ms".format(**d) + big + small
          + " cpu {cpu_client_pct}/{cpu_server_pct} %".format(**d))
    return 0


def zero(path, names):
    """Exit status 1 when a counter in names is above 0, on any side, in a line of path."""
    bad = []
    for ln in open(path):
        if not ln.strip():
            continue
        d = json.loads(ln)
        for side, vals in (d.get("eng") or {}).items():
            for n in names:
                if (vals or {}).get(n):
                    bad.append("{} {} {} {} #{}: {} {} = {}".format(d["case"], d["variant"], d["dir"], d["cc"],
                                                                    d["rep"], side, n, vals[n]))
        for side, v in (d.get("wg_rx_dropped") or {}).items():
            if "wg_rx_dropped" in names and v:
                bad.append("{} {} {} {} #{}: {} wg_rx_dropped = {}".format(d["case"], d["variant"], d["dir"],
                                                                         d["cc"], d["rep"], side, v))
    for b in bad:
        print("   " + b)
    return 1 if bad else 0


# ---- the summary ---------------------------------------------------------------

MODE_MIN_N = 6  # runs a cell needs before it can be told to have two modes

# The least a gap between two modes has to be to count, by metric: below it the
# modes are noise of the measurement (the counts of the story 012 runs differ by 200 or
# more, the goodputs by 15 Mbit/s, the RTTs by 25 ms).
MODE_FLOOR = {"mbps": 1.0, "retrans": 10, "spurious": 10, "rtt_p50": 5.0, "rtt_big_p50": 5.0}


def quartiles(xs):
    """(p25, median, p75) of a sorted list, the inclusive method (the table of step 0)."""
    if len(xs) == 1:
        return xs[0], xs[0], xs[0]
    q = statistics.quantiles(xs, n=4, method="inclusive")
    return q[0], statistics.median(xs), q[2]


def cell(xs, p=1, spread=True):
    """The median and, with several values, (p25-p75), or (min-max) with fewer than 4."""
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return "-"
    num_ = lambda x: f"{x:.{p}f}"
    if len(xs) == 1 or not spread:
        return num_(statistics.median(xs))
    if len(xs) < 4:
        lo, hi = xs[0], xs[-1]
    else:
        q = quartiles(xs)
        lo, hi = q[0], q[2]
    return f"{num_(statistics.median(xs))} ({num_(lo)}–{num_(hi)})"


def modes(xs, floor):
    """The two modes of a cell, (low runs, high runs), or None when it has one.

    The rule: with at least MODE_MIN_N runs, sort them and split at the widest gap
    between neighbours that leaves two or more runs on each side. It is two modes when
    that gap is wider than the wider of the two groups' own ranges (max - min), so that
    it is "large relative to the spread" of each, and at least `floor` (what the
    measurement cannot tell apart). Examples of story 012: reorder150 with Cubic, 30
    retransmissions in one half of the runs and 247-395 in the other (gap 216 against
    a range of 148); starlink15 with BBR, 63.5 or 80 Mbit/s. One odd run is an outlier
    and not a mode (a group needs two). It is a description of 10 runs, not a test."""
    xs = sorted(x for x in xs if x is not None)
    if len(xs) < MODE_MIN_N:
        return None
    gap, at = max((xs[i] - xs[i - 1], i) for i in range(len(xs) - 2, 1, -1))  # i: first of the high group
    lo, hi = xs[:at], xs[at:]
    span = max(lo[-1] - lo[0], hi[-1] - hi[0])
    if gap > span and gap >= floor:
        return lo, hi
    return None


def mode_text(group, p):
    med = statistics.median(group)
    return f"{len(group)} runs near {med:.{p}f} ({min(group):.{p}f}–{max(group):.{p}f})"


def mwu(a, b):
    """Two-sided Mann-Whitney U p-value (normal approximation, ties and continuity
    corrected); None under 5 runs on either side or when every run is the same."""
    a, b = [x for x in a if x is not None], [x for x in b if x is not None]
    n1, n2 = len(a), len(b)
    if min(n1, n2) < 5:
        return None
    allv = sorted([(x, 0) for x in a] + [(x, 1) for x in b])
    n = len(allv)
    r1 = 0.0
    tie = 0
    i = 0
    while i < n:
        j = i
        while j < n and allv[j][0] == allv[i][0]:
            j += 1
        t = j - i
        rank = (i + 1 + j) / 2  # the average of ranks i+1 .. j
        r1 += rank * sum(1 for k in range(i, j) if allv[k][1] == 0)
        tie += t**3 - t
        i = j
    u1 = r1 - n1 * (n1 + 1) / 2
    mu = n1 * n2 / 2
    var = n1 * n2 / 12 * ((n + 1) - tie / (n * (n - 1)))
    if var <= 0:
        return None
    z = max(abs(u1 - mu) - 0.5, 0) / math.sqrt(var)
    return math.erfc(z / math.sqrt(2))


def key_of(r):
    return (r["case"], r["variant"], r["dir"], r["cc"])


def md_table(head, rows, left=4):
    out = ["| " + " | ".join(head) + " |", "| " + " | ".join(["---"] * left + ["---:"] * (len(head) - left)) + " |"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return out


# the columns of the main table: (header, field, decimals)
def main_columns(rows):
    has = lambda f: any(r.get(f) is not None for r in rows)
    cols = [("Mbit/s", "mbps", 1), ("retrans", "retrans", 0)]
    if has("retrans_pct"):
        cols.append(("retrans %", "retrans_pct", 2))
    if has("spurious"):
        cols.append(("spurious", "spurious", 0))
    cols += [("RTT p50", "rtt_p50", 1), ("RTT p99", "rtt_p99", 1)]
    if has("rtt_big_p50"):
        cols += [("1200 B RTT p50", "rtt_big_p50", 1), ("1200 B RTT p99", "rtt_big_p99", 1)]
    if any(isinstance(r.get("small"), dict) for r in rows):
        cols += [("small p50 µs", "small.p50_us", 0), ("small p99 µs", "small.p99_us", 0)]
    return cols


def field(r, f):
    if "." in f:
        a, b = f.split(".", 1)
        return num(dig(r, a, b))
    return r.get(f)


def engine_side(r):
    """The end whose reorderer carried the bulk of the run: the server for upload, the client for download."""
    return "srv" if r["dir"] == "up" else "cli"


def tables(rows, refs):
    keys = []
    for r in rows:
        if key_of(r) not in keys:
            keys.append(key_of(r))
    by = {k: [r for r in rows if key_of(r) == k] for k in keys}
    out = []
    cols = main_columns(rows)
    many = any(len(v) > 1 for v in by.values())
    flagged = []  # (key, header, text)
    body = []
    for k in keys:
        rs = by[k]
        cells = [k[0], k[1], k[2], k[3], str(len(rs))]
        for hdr, f, p in cols:
            vals = [field(r, f) for r in rs]
            c = cell(vals, p)
            m = modes(vals, MODE_FLOOR[f]) if f in MODE_FLOOR else None
            if m:
                c += " †"
                flagged.append((k, hdr, f"{mode_text(m[0], p)} and {mode_text(m[1], p)}"))
            cells.append(c)
        med = lambda name: cell([r.get(name) for r in rs], spread=False)
        cells.append(f"{med('cpu_client_pct')}/{med('cpu_server_pct')}")
        body.append(cells)
    head = ["case", "variant", "dir", "cc", "n"] + [c[0] for c in cols] + ["CPU cli/srv %"]
    out += md_table(head, body)
    notes = []
    if many:
        notes.append("n > 1: median (p25–p75), or (min–max) with n < 4.")
    if any(r.get("spurious") is not None for r in rows):
        notes.append("retrans of quic: packets declared lost; spurious: of those, the ones acknowledged afterwards.")
    if any(r.get("retrans_pct") is not None for r in rows):
        notes.append("retrans %: retransmissions over the segments the sender wrote (iperf3 bytes / MSS; quic: lost / sent).")
    if flagged:
        notes.append("† two modes (rule in bondtab.py, modes()), listed below.")
    if notes:
        out += ["", " ".join(notes)]
    if flagged:
        out += [""] + [f"- {k[0]} {k[1]} {k[2]} {k[3]}, {h}: {t}" for k, h, t in flagged]

    # The engine's counters: medians of each run's difference, at the end that received.
    if any(isinstance(r.get("eng"), dict) and any(v is not None for s in r["eng"].values() for v in s.values()) for r in rows):
        # The columns of the reorderer only when some engine had it (today's has not).
        ro = any(((r.get("eng") or {}).get(s) or {}).get("held_total") is not None for r in rows for s in ("cli", "srv"))
        out += ["", "Engine counters per run (median of the differences between snapshots)"
                + (", the reorderer's at the receiving end (server for up, client for down); held max and burst are "
                   "maxima since the case started:" if ro else ":"), ""]
        head = ["case", "variant", "dir", "cc", "n"]
        if ro:
            head += ["held", "bypassed", "split_rx", "lost rack/cap/ovf/flush", "late rack/cap/ovf/old",
                     "far/resyncs/pool_full", "held max/burst"]
        head += ["too_old cli/srv", "wg drops cli/srv", "lost_all cli/srv", "wg rx_dropped cli/srv",
                 "sends per packet", "send drops"]
        body = []
        for k in keys:
            rs = by[k]
            side = engine_side(rs[0])

            def eng(s, n, rs=rs):
                return [((r.get("eng") or {}).get(s) or {}).get(n) for r in rs]

            def slash(names, s=side, eng=eng):
                return "/".join(cell(eng(s, n), 0, spread=False) for n in names)

            def pair(n, eng=eng):
                return "{}/{}".format(cell(eng("cli", n), 0, spread=False), cell(eng("srv", n), 0, spread=False))

            wgd = [(r.get("wg_rx_dropped") or {}) for r in rs]
            wgp = "{}/{}".format(cell([w.get("cli") for w in wgd], 0, spread=False),
                                 cell([w.get("srv") for w in wgd], 0, spread=False))
            cells = [k[0], k[1], k[2], k[3], str(len(rs))]
            if ro:
                cells += [slash(["held_total"]), slash(["bypassed"]), slash(["split_rx"]),
                          slash(["lost_rack", "lost_cap", "lost_overflow", "lost_flush"]),
                          slash(["late_after_rack", "late_after_cap", "late_after_overflow", "late_old"]),
                          slash(["far", "resyncs", "pool_full"]), slash(["held_max", "max_burst"])]
            # At the end that sent the bulk: the client for upload, the server for download.
            tx = "cli" if side == "srv" else "srv"
            per = []
            for r in rs:
                e = (r.get("eng") or {}).get(tx) or {}
                per.append(e["tx_links"] / e["sent"] if e.get("tx_links") is not None and e.get("sent") else None)
            body.append(cells + [pair("too_old"), pair("wireguard_drops"), pair("lost_all"), wgp,
                                 cell(per, 2, spread=False), cell(eng(tx, "tx_drops"), 0, spread=False)])
        out += md_table(head, body)
        bad = []
        for k in keys:
            for r in by[k]:
                for s, v in (r.get("eng") or {}).items():
                    for n in ("too_old", "pool_full", "wireguard_drops"):
                        if (v or {}).get(n):
                            bad.append(f"{k[0]} {k[1]} {k[2]} {k[3]} #{r['rep']}: {s} {n} = {v[n]}")
        if bad:
            out += ["", "Above 0 where it should not be (too_old, pool_full, wireguard_drops):"] + [f"- {b}" for b in bad[:20]]
            if len(bad) > 20:
                out.append(f"- ... and {len(bad) - 20} more")
    out += compare(rows, by, keys, refs)
    return out


# What each of bond.sh's variants is compared with (story 012, step 1 spec 8.3): the
# redundancy variants with redundancy without the reorderer (CPU <= 1.05x, +0 ms), the
# split with one link alone (rate <= 1.2x) and with its control (the split without the
# reorderer), the controls with one link alone. solo is the reference of all.
FAMILY_REFS = {
    "bonded": ("redund-ro0",),
    "redund": ("redund-ro0",),
    "redund-dw20": ("redund-ro0",),
    "redund-ro0": ("solo",),
    "split": ("solo", "splitnoro"),
    "splitnoro": ("solo",),
    "solo": (),
}
DEFAULT_REFS = ("solo", "redund-ro0", "splitnoro")
# bond.sh's -bpN (reorder_bypass), -spN (split_spread_ms), -nc (split_spread_copy = no), -gov (governor)
SUFFIX = re.compile(r"^(.+)-(?:(?:bp|sp)[0-9]+|nc|gov)$")


def references(variant, present, refs=None):
    """The variants of a group (present: its variant names) that variant is compared
    with, one row each, in order; never itself.

    - refs (--ref A,B): the first of them the group has, for every variant (the rule
      before the families);
    - a variant with suffixes (-bpN, -spN, -nc): the nearest of the variants without
      its last ones that the group has (split-sp0-bp64: split-sp0, else split),
      else as the variant without any;
    - the variant's FAMILY_REFS the group has;
    - none of those (or a variant the table does not know): the first of
      DEFAULT_REFS the group has."""
    def first(rs):
        r = next((x for x in rs if x in present), None)
        return [r] if r is not None and r != variant else []

    if refs is not None:
        return first(refs)
    base = variant
    m = SUFFIX.match(base)
    while m:
        base = m.group(1)
        if base in present:
            return [base]
        m = SUFFIX.match(base)
    got = [r for r in FAMILY_REFS.get(base, ()) if r in present and r != variant]
    return got or first(DEFAULT_REFS)


def compare(rows, by, keys, refs):
    """Each variant against its reference variants (references(): its family's, or
    the first of --ref) of the same case, direction and congestion control, one row
    per reference. Ratios of the medians (goodput, retransmission rate, CPU), the
    difference of the RTT medians, and the Mann-Whitney p-value of the goodput and of
    the retransmission rate (when both sides have 5 runs or more)."""
    groups = []
    for k in keys:
        g = (k[0], k[2], k[3])
        if g not in groups:
            groups.append(g)
    body = []
    med = lambda rs, f: statistics.median([x for x in (field(r, f) for r in rs) if x is not None] or [math.nan])

    def ratio(x, y):
        return "-" if math.isnan(x) or math.isnan(y) or not y else f"{x / y:.2f}×"

    def diff(x, y):
        return "-" if math.isnan(x) or math.isnan(y) else f"{x - y:+.1f}".replace("-0.0", "+0.0")

    def pv(rs, rr, f):
        p = mwu([field(r, f) for r in rs], [field(r, f) for r in rr])
        return "-" if p is None else f"{p:.3f}"

    for g in groups:
        members = [k for k in keys if (k[0], k[2], k[3]) == g]
        named = {k[1]: k for k in members}
        for k in members:
            rs = by[k]
            for name in references(k[1], named, refs):
                rr = by[named[name]]
                body.append([k[0], k[1], k[2], k[3], name,
                             ratio(med(rs, "mbps"), med(rr, "mbps")), pv(rs, rr, "mbps"),
                             ratio(med(rs, "retrans_pct"), med(rr, "retrans_pct")), pv(rs, rr, "retrans_pct"),
                             diff(med(rs, "rtt_p50"), med(rr, "rtt_p50")), diff(med(rs, "rtt_big_p50"), med(rr, "rtt_big_p50")),
                             ratio(med(rs, "cpu_client_pct"), med(rr, "cpu_client_pct")),
                             ratio(med(rs, "cpu_server_pct"), med(rr, "cpu_server_pct"))])
    if not body:
        return []
    head = ["case", "variant", "dir", "cc", "against", "Mbit/s", "p", "retrans %", "p", "RTT p50 Δms",
            "1200 B RTT p50 Δms", "CPU cli", "CPU srv"]
    return ["", "Against the reference variant (ratios and differences of the medians; p: Mann-Whitney, given with 5 runs or more on each side):",
            ""] + md_table(head, body, left=5)


def table(path, refs):
    rows = [json.loads(ln) for ln in open(path) if ln.strip()]
    if not rows:
        print("(no runs)")
        return 0
    print("\n".join(tables(rows, refs)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog="bondtab.py")
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("line")
    p.add_argument("--result", required=True)
    p.add_argument("--ping", required=True)
    p.add_argument("--ping-big")
    p.add_argument("--small-tx")
    p.add_argument("--small-rx")
    p.add_argument("--before")
    p.add_argument("--after")
    p.add_argument("--samples", help="BOND_SAMPLE's directory of status files, one a second")
    p.add_argument("--pep", help="pepstat.py's JSON (story 024's hosts, BOND_PEP)")
    for n in ("case", "variant", "dir", "cc", "rep", "secs", "cpu-client", "cpu-server", "hz", "netem"):
        p.add_argument("--" + n, required=True)
    p = sp.add_parser("show")
    p.add_argument("file")
    p = sp.add_parser("delta")
    p.add_argument("before")
    p.add_argument("after")
    p = sp.add_parser("zero")
    p.add_argument("file")
    p.add_argument("names", nargs="+")
    p = sp.add_parser("table")
    p.add_argument("file")
    p.add_argument("--ref", default=None, help="A,B: every variant against the first of these its group has "
                   "(default: each against its family's, see references())")
    a = ap.parse_args(argv)
    if a.cmd == "line":
        return line(a)
    if a.cmd == "show":
        return show(a.file)
    if a.cmd == "delta":
        print(json.dumps(delta(a.before, a.after)))
        return 0
    if a.cmd == "zero":
        return zero(a.file, a.names)
    return table(a.file, a.ref.split(",") if a.ref else None)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
