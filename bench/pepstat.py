#!/usr/bin/env python3
"""pepstat: what the hosts of story 024's lab (bond.sh with BOND_PEP) saw in one
run, as one JSON object for bondtab.py's line (--pep).

    python3 -I bench/pepstat.py DIR --mode M --midcc C --farcc C --secs S
                                [--result FILE --cc CC] [--cpu-cli T --cpu-srv T --hz HZ]

DIR holds, for this run:

    snmp-before-NS, snmp-after-NS   /proc/net/snmp read inside each netns
                                    (NS: lan, cli, srv, far)
    ss-cli-I, ss-srv-I              "ss -tinmH" inside the router's and the
                                    VPS's netns, sample I (every 0.5 s)
    rss-I                           "NAME KB" for each proxy process, sample I
    kcp-before-SIDE.csv, kcp-after-SIDE.csv   kcptun's own counters (--snmplog,
                                    once a second), if any (SIDE: cli, srv)
    status-before-SIDE.json, status-after-SIDE.json, status-SIDE-I
                                    the engines' status files (modes cga and
                                    cgb, the protocontroller: their
                                    "protocontroller" object, story 025)

Each netns has its own TCP counters, so the retransmissions split by leg (for
an upload): lan is the sender's host (the whole path when transparent, the leg
to the router when split), mid the router (the redirector's TCP to the VPS),
far the VPS (the server end's TCP to the far host). retrans_pct divides by the
segments of payload delivered (what reached the far and near hosts, from
--result: iperf3's JSON, or livesim.py merge's for --cc live, over 1380 bytes,
the tunnel's MSS).
mem_kb is each netns's TCP memory (rmem_alloc + wmem_queued over its sockets:
the proxies' buffers, kernel side) at its peak and median over the samples;
rss_kb the proxies' resident memory at its peak (eng_rss_kb the engines', first,
largest and last sample); mid_rtt_ms and far_rtt_ms the
median smoothed RTT of the split legs' senders (the redirector's socket to the
VPS, the server end's sockets to the far host); cpu_*_pct the proxies' CPU
(ticks over --hz and --secs).

Modes cga and cgb (cg) (the protocontroller, story 025: the engines are the
proxies): "cg" holds, per side, the
engine's stream counters over the run (data messages sent, sent again,
copied, timer expiries, duplicates and out-of-order arrivals), the peak of its
streams' memory (the pool's chunks in use) and of the bytes they held each way,
and, for cgb, the median smoothed RTT of its pacer (mid_rtt_ms, the router's
for an upload; cga's is its middle TCP's, from ss).
"""

import argparse
import glob
import json
import os
import re
import sys

NSS = ("lan", "cli", "srv", "far")
SKMEM = re.compile(r"skmem:\(r(\d+),rb\d+,t\d+,tb\d+,f\d+,w(\d+)")
RTT = re.compile(r"\brtt:([\d.]+)/")


def snmp(path):
    """The Tcp line of /proc/net/snmp as a dict, {} when missing."""
    try:
        lines = [ln.split() for ln in open(path) if ln.startswith("Tcp:")]
    except OSError:
        return {}
    if len(lines) < 2:
        return {}
    return dict(zip(lines[0][1:], (int(x) for x in lines[1][1:])))


def sockets(path):
    """(local, peer, detail text) for each socket in an "ss -tinmH" dump."""
    out, cur = [], None
    try:
        for ln in open(path):
            if not ln.strip():
                continue
            if ln[0] not in " \t":
                f = ln.split()
                cur = [f[3] if len(f) > 3 else "", f[4] if len(f) > 4 else "", ""]
                out.append(cur)
            elif cur is not None:
                cur[2] += " " + ln.strip()
    except OSError:
        pass
    return out


def median(xs):
    xs = sorted(x for x in xs if x is not None)
    return xs[len(xs) // 2] if xs else None


def kcp_last(path):
    """The last row of kcptun's --snmplog CSV as a dict of ints."""
    try:
        rows = [ln.strip().split(",") for ln in open(path) if ln.strip()]
    except OSError:
        return None
    if len(rows) < 2:
        return None
    head = rows[0]
    try:
        return dict(zip(head, (int(x) for x in rows[-1])))
    except ValueError:
        return None


def rx_bytes(path, cc):
    """The bytes the far (and near) hosts received in the run, 0 when unknown."""
    try:
        j = json.load(open(path))
        if cc in ("live", "soak"):
            return sum((j[k].get("delivered") or 0) * (j[k].get("size") or 0) for k in ("far", "near") if j.get(k))
        return j["end"]["sum_received"]["bytes"]
    except (OSError, ValueError, KeyError, TypeError):
        return 0


def status_pep(path):
    """The "protocontroller" object of an engine's status file ("pep" before
    the rename), {} when missing."""
    try:
        j = json.load(open(path))
    except (OSError, ValueError):
        return {}
    if isinstance(j, list):
        j = j[0] if j else {}
    return j.get("protocontroller") or j.get("pep") or {}


CG_COUNTERS = ("data_tx", "data_rx", "retx", "copies", "rto", "dup_frames", "ooo_frames", "drop_frames",
               "acks_tx", "acks_rx", "opened", "closed", "rst_tx", "nomem", "sock_rx_bytes", "sock_tx_bytes",
               "a_opened", "a_bytes_in", "a_bytes_out", "bad")
# nested ones: why GRIT sent again (RACK, the timer, the tunnel's NACK), and
# the NACKs that were only a hint or a link's own loss (story 029)
CG_NESTED = (("retx_rack", "retx_by", "rack"), ("retx_rto", "retx_by", "rto"), ("retx_nack", "retx_by", "nack"),
             ("nack_hints", "nacks", "hints"), ("nack_subflow", "nacks", "subflow"),
             ("nack_delivered", "nacks", "delivered"))


def cg_summary(d):
    out = {}
    for side in ("cli", "srv"):
        b, e = status_pep(os.path.join(d, "status-before-%s.json" % side)), \
            status_pep(os.path.join(d, "status-after-%s.json" % side))
        if not e:
            continue
        o = {n: (e.get(n) or 0) - (b.get(n) or 0) for n in CG_COUNTERS}
        for n, k, sub in CG_NESTED:
            o[n] = ((e.get(k) or {}).get(sub) or 0) - ((b.get(k) or {}).get(sub) or 0)
        mem, htx, hrx, srtt = [], [], [], []
        for f in sorted(glob.glob(os.path.join(d, "status-%s-*" % side))):
            p = status_pep(f)
            if not p:
                continue
            mem.append(p.get("mem_bytes") or 0)
            htx.append(p.get("held_tx_bytes") or 0)
            hrx.append(p.get("held_rx_bytes") or 0)
            for pc in p.get("pacers") or []:
                if pc.get("srtt_ms"):
                    srtt.append(pc["srtt_ms"])
        o["mem_kb_peak"] = round(max(mem) / 1024) if mem else None
        o["mem_peak_kb"] = round((e.get("mem_peak_bytes") or 0) / 1024)
        o["held_tx_kb_peak"] = round(max(htx) / 1024) if htx else None
        o["held_rx_kb_peak"] = round(max(hrx) / 1024) if hrx else None
        o["srtt_ms"] = median(srtt)
        out[side] = o
    return out


def summary(a):
    d = a.dir
    rx = rx_bytes(a.result, a.cc) if a.result else 0
    seg = rx / 1380.0 if rx else None
    retrans, outsegs = {}, {}
    for ns in NSS:
        b, e = snmp(os.path.join(d, "snmp-before-" + ns)), snmp(os.path.join(d, "snmp-after-" + ns))
        if b and e:
            retrans[ns] = e["RetransSegs"] - b["RetransSegs"]
            outsegs[ns] = e["OutSegs"] - b["OutSegs"]
    rpct = {ns: (round(100.0 * n / seg, 3) if seg else None) for ns, n in retrans.items()}
    mem, rtts = {"cli": [], "srv": []}, {"mid": [], "far": [], "near": []}
    for ns in ("cli", "srv"):
        for f in sorted(glob.glob(os.path.join(d, "ss-%s-*" % ns))):
            tot = 0
            for local, peer, det in sockets(f):
                m = SKMEM.search(det)
                if m:
                    tot += int(m.group(1)) + int(m.group(2))
                r = RTT.search(det)
                if not r or "bytes_acked" not in det:
                    continue
                if ns == "cli" and (peer.endswith(":8388") or peer == "10.79.0.1:7801"):
                    rtts["mid"].append(float(r.group(1)))
                elif ns == "srv" and peer.startswith("172.30.1.2:"):
                    rtts["far"].append(float(r.group(1)))
                elif ns == "srv" and peer.startswith("172.30.2.2:"):
                    rtts["near"].append(float(r.group(1)))
            mem[ns].append(tot)
    rss, eng = {}, {}
    for f in sorted(glob.glob(os.path.join(d, "rss-*")), key=lambda x: int(x.rsplit("-", 1)[1])):
        try:
            for ln in open(f):
                p = ln.split()
                if len(p) == 2 and p[1].isdigit():
                    if p[0].startswith("engine-"):
                        eng.setdefault(p[0][7:], []).append(int(p[1]))
                    else:
                        rss[p[0]] = max(rss.get(p[0], 0), int(p[1]))
        except OSError:
            pass
    kb = lambda x: None if x is None else round(x / 1024)
    out = {"mode": a.mode, "midcc": a.midcc, "farcc": a.farcc, "retrans": retrans, "retrans_pct": rpct,
           "outsegs": outsegs,
           "mem_kb": {ns: {"peak": kb(max(v)) if v else None, "p50": kb(median(v))} for ns, v in mem.items()},
           "rss_kb": rss,
           # the engines' RSS (cc soak, story 025): first, largest and last sample
           "eng_rss_kb": {k: [v[0], max(v), v[-1]] for k, v in eng.items() if v},
           "mid_rtt_ms": median(rtts["mid"]), "far_rtt_ms": median(rtts["far"]), "near_rtt_ms": median(rtts["near"])}
    if a.hz:
        out["cpu_cli_pct"] = round(100.0 * a.cpu_cli / a.hz / a.secs, 1)
        out["cpu_srv_pct"] = round(100.0 * a.cpu_srv / a.hz / a.secs, 1)
    kcp = {}
    for side in ("cli", "srv"):
        b = kcp_last(os.path.join(d, "kcp-before-%s.csv" % side)) or {}
        e = kcp_last(os.path.join(d, "kcp-after-%s.csv" % side))
        if e:
            kcp[side] = {n: e.get(n, 0) - b.get(n, 0) for n in ("OutSegs", "RetransSegs", "FastRetransSegs",
                                                                 "EarlyRetransSegs", "LostSegs", "RepeatSegs")}
    if kcp:
        out["kcp"] = kcp
    if a.mode in ("cg", "cgb"):
        out["cg"] = cg_summary(d)
        out["mid_rtt_ms"] = dig_srtt(out["cg"])
    elif a.mode == "cga":
        out["cg"] = cg_summary(d)
    return out


def dig_srtt(cg):
    """The router's pacer RTT (an upload's middle leg), else the VPS's."""
    for side in ("cli", "srv"):
        v = (cg.get(side) or {}).get("srtt_ms")
        if v:
            return v
    return None


def main(argv):
    ap = argparse.ArgumentParser(prog="pepstat.py")
    ap.add_argument("dir")
    ap.add_argument("--mode", required=True)
    ap.add_argument("--midcc", default="")
    ap.add_argument("--farcc", default="")
    ap.add_argument("--secs", type=float, required=True)
    ap.add_argument("--result")
    ap.add_argument("--cc", default="")
    ap.add_argument("--cpu-cli", type=int, default=0)
    ap.add_argument("--cpu-srv", type=int, default=0)
    ap.add_argument("--hz", type=int, default=0)
    a = ap.parse_args(argv)
    print(json.dumps(summary(a)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
