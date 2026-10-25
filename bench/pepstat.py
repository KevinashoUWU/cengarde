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

Each netns has its own TCP counters, so the retransmissions split by leg (for
an upload): lan is the sender's host (the whole path when transparent, the leg
to the router when split), mid the router (the redirector's TCP to the VPS),
far the VPS (the server end's TCP to the far host). retrans_pct divides by the
segments of payload delivered (what reached the far and near hosts, from
--result: iperf3's JSON, or livesim.py merge's for --cc live, over 1380 bytes,
the tunnel's MSS).
mem_kb is each netns's TCP memory (rmem_alloc + wmem_queued over its sockets:
the proxies' buffers, kernel side) at its peak and median over the samples;
rss_kb the proxies' resident memory at its peak; mid_rtt_ms and far_rtt_ms the
median smoothed RTT of the split legs' senders (the redirector's socket to the
VPS, the server end's sockets to the far host); cpu_*_pct the proxies' CPU
(ticks over --hz and --secs).
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
        if cc == "live":
            return sum((j[k].get("delivered") or 0) * (j[k].get("size") or 0) for k in ("far", "near") if j.get(k))
        return j["end"]["sum_received"]["bytes"]
    except (OSError, ValueError, KeyError, TypeError):
        return 0


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
                if ns == "cli" and peer.endswith(":8388"):
                    rtts["mid"].append(float(r.group(1)))
                elif ns == "srv" and peer.startswith("172.30.1.2:"):
                    rtts["far"].append(float(r.group(1)))
                elif ns == "srv" and peer.startswith("172.30.2.2:"):
                    rtts["near"].append(float(r.group(1)))
            mem[ns].append(tot)
    rss = {}
    for f in glob.glob(os.path.join(d, "rss-*")):
        try:
            for ln in open(f):
                p = ln.split()
                if len(p) == 2 and p[1].isdigit():
                    rss[p[0]] = max(rss.get(p[0], 0), int(p[1]))
        except OSError:
            pass
    kb = lambda x: None if x is None else round(x / 1024)
    out = {"mode": a.mode, "midcc": a.midcc, "farcc": a.farcc, "retrans": retrans, "retrans_pct": rpct,
           "outsegs": outsegs,
           "mem_kb": {ns: {"peak": kb(max(v)) if v else None, "p50": kb(median(v))} for ns, v in mem.items()},
           "rss_kb": rss,
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
    return out


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
