#!/usr/bin/env python3
"""Story 026's tables (FEC v3): the bonding lab's JSON lines (bench/labq.py's
all.jsonl) by case, direction, cc and arm, against a reference arm.

  fectab.py FILE [--ref ARM] [--cases a,b] [--arms a,b] [--cc cubic] [--dir up]

Per arm: n, the medians of the TCP goodput (Mbit/s), its retransmissions (%),
the RTT p50 and p99 of the 84-byte ping and the p99 of the 1200-byte one (ms),
what the reorderer gave up at the receiving end (lost: rack + cap + overflow,
per run), the packets the codes rebuilt (rebuilt), the repair bytes per useful
byte (overhead: FEC v3's repairs, the XOR parities and the parity asked for
again, over the DATA bytes sent on all links, at the sending end) and the
sending end's CPU (%); and, against the reference, the goodput's ratio of
medians with its Mann-Whitney p (two-sided, bondtab.py's), and the same p for
the 1200-byte ping's p99.
"""

import argparse
import json
import os
import statistics
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bondtab  # noqa: E402


def g(d, *path):
    for k in path:
        if not isinstance(d, dict) or d.get(k) is None:
            return None
        d = d[k]
    return d


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fields(r):
    """One run's numbers."""
    send = "cli" if r["dir"] == "up" else "srv"
    recv = "srv" if r["dir"] == "up" else "cli"
    es, er = g(r, "eng", send) or {}, g(r, "eng", recv) or {}
    rep = sum(es.get(k) or 0 for k in ("f3_bytes", "fec_par_bytes", "parq_bytes"))
    data = es.get("tx_bytes")
    lost = sum(er.get(k) or 0 for k in ("lost_rack", "lost_cap", "lost_overflow"))
    rebuilt = (er.get("rlc_rec") or 0) + (er.get("xor_rec") or 0)
    return {
        "mbps": r.get("mbps"),
        "retx": r.get("retrans_pct"),
        "rtt50": r.get("rtt_p50"),
        "rtt99": r.get("rtt_p99"),
        "big99": r.get("rtt_big_p99"),
        "lost": lost if er else None,
        "rebuilt": rebuilt if er else None,
        "ovh": rep / data if data else None,
        "cpu": r.get("cpu_client_pct") if r["dir"] == "up" else r.get("cpu_server_pct"),
        "peak": er.get("rlc_peak"),
        "evicted": er.get("rlc_evicted"),
    }


def fmt(x, p=1):
    if x is None:
        return "–"
    return ("%." + str(p) + "f") % x


def pfmt(p):
    if p is None:
        return "–"
    return "<0.001" if p < 0.001 else "%.3f" % p


def main(argv=None):
    ap = argparse.ArgumentParser(description="Story 026's tables (see the docstring).")
    ap.add_argument("file")
    ap.add_argument("--ref", default="base")
    ap.add_argument("--cases"), ap.add_argument("--arms"), ap.add_argument("--cc"), ap.add_argument("--dir")
    a = ap.parse_args(argv)
    rows = [json.loads(line) for line in open(a.file) if line.strip()]
    pick = lambda v, s: not s or v in s.split(",")  # noqa: E731
    rows = [r for r in rows if pick(r["case"], a.cases) and pick(r.get("arm", r["variant"]), a.arms) and
            pick(r["cc"], a.cc) and pick(r["dir"], a.dir)]
    groups = {}
    for r in rows:
        groups.setdefault((r["case"], r["dir"], r["cc"]), {}).setdefault(r.get("arm", r["variant"]), []).append(fields(r))
    order = []
    for r in rows:
        arm = r.get("arm", r["variant"])
        if arm not in order:
            order.append(arm)
    if a.ref in order:
        order.remove(a.ref)
        order.insert(0, a.ref)
    head = ["arm", "n", "Mbit/s", "×ref", "p", "retx %", "RTT p50", "RTT p99", "1200 B p99", "p", "lost", "rebuilt",
            "overhead", "CPU %"]
    for key in sorted(groups):
        arms = groups[key]
        ref = arms.get(a.ref)
        print("\n### %s, %s, %s\n" % key)
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(["---"] + ["---:"] * (len(head) - 1)) + "|")
        for arm in order:
            fs = arms.get(arm)
            if not fs:
                continue
            col = lambda k: [f[k] for f in fs]  # noqa: E731
            m = med(col("mbps"))
            ratio = p = pb = None
            if ref and arm != a.ref:
                rm = med([f["mbps"] for f in ref])
                ratio = m / rm if m is not None and rm else None
                xs, ys = [x for x in col("mbps") if x is not None], [f["mbps"] for f in ref if f["mbps"] is not None]
                p = bondtab.mwu(xs, ys) if len(xs) >= 3 and len(ys) >= 3 else None
                xs, ys = [x for x in col("big99") if x is not None], [f["big99"] for f in ref if f["big99"] is not None]
                pb = bondtab.mwu(xs, ys) if len(xs) >= 3 and len(ys) >= 3 else None
            ovh = med(col("ovh"))
            print("| " + " | ".join([
                arm, str(len(fs)), fmt(m), fmt(ratio, 2), pfmt(p), fmt(med(col("retx")), 2), fmt(med(col("rtt50"))),
                fmt(med(col("rtt99"))), fmt(med(col("big99"))), pfmt(pb), fmt(med(col("lost")), 0),
                fmt(med(col("rebuilt")), 0), "–" if ovh is None else "%.3f" % ovh, fmt(med(col("cpu")))]) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
