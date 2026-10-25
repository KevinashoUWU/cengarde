#!/usr/bin/env python3
"""peptab: story 024's tables (PEP mode A against the transparent tunnel) from
labq.py's all.jsonl of bond.sh runs with BOND_PEP.

    python3 -I bench/peptab.py ALL.jsonl [--ref T-copies,T-rlc] [--cases ...]

One table per case and kind of run: bulk TCP (cc cubic: iperf3 from the LAN
host to the far one) and live (cc live: livesim.py's two streams). Each cell is
the median over the runs (n in its column); after the main numbers, the p of a
two-sided Mann-Whitney U against the reference arm of the same family (an arm
NAME-SUFFIX against the first --ref with that suffix: T-copies for every
*-copies, T-rlc for every *-rlc). Retransmissions are per leg (pepstat.py):
"lan" the sender's host (the whole path when transparent), "mid" the router's
(the split's middle leg), "far" the VPS's (the leg to the far host).
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bondtab import mwu  # noqa: E402

sys.dont_write_bytecode = True


def dig(d, *path):
    for p in path:
        if not isinstance(d, dict):
            return None
        d = d.get(p)
    return d


def med(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def fmt(x, nd=1):
    if x is None:
        return "-"
    if isinstance(x, float) and nd == 0:
        return str(int(round(x)))
    return ("{:.%df}" % nd).format(x).replace(".", ",")


def fmt_p(p):
    if p is None:
        return "-"
    return ("%.3f" % p).replace(".", ",") if p >= 0.001 else "<0,001"


BULK = [("Mbit/s", lambda r: r.get("mbps"), 1, True),
        ("retx lan", lambda r: dig(r, "pep", "retrans", "lan"), 0, False),
        ("retx mid", lambda r: dig(r, "pep", "retrans", "cli"), 0, False),
        ("retx far", lambda r: dig(r, "pep", "retrans", "srv"), 0, False),
        ("RTT p99", lambda r: r.get("rtt_p99"), 0, True),
        ("mem router KB", lambda r: dig(r, "pep", "mem_kb", "cli", "peak"), 0, False),
        ("mem VPS KB", lambda r: dig(r, "pep", "mem_kb", "srv", "peak"), 0, False),
        ("CPU proxies %", lambda r: (dig(r, "pep", "cpu_cli_pct") or 0) + (dig(r, "pep", "cpu_srv_pct") or 0), 1, False)]


def live_col(k, f):
    return lambda r: dig(r, "live", k, *f) if isinstance(f, tuple) else dig(r, "live", k, f)


LIVE = [("far Mbit/s", live_col("far", "mbps"), 2, True),
        ("far s p5", live_col("far", "sec_p5"), 2, True),
        ("far stalls", live_col("far", "stalls"), 1, True),
        ("far stall s", live_col("far", "stall_s"), 2, False),
        ("far delay p50", live_col("far", "delay_p50"), 0, True),
        ("far delay p99", live_col("far", "delay_p99"), 0, True),
        ("far dropped %", live_col("far", ("send", "drop_pct")), 2, True),
        ("near Mbit/s", live_col("near", "mbps"), 2, True),
        ("near s p5", live_col("near", "sec_p5"), 2, False),
        ("near delay p99", live_col("near", "delay_p99"), 0, True),
        ("near dropped %", live_col("near", ("send", "drop_pct")), 2, False),
        ("retx mid", lambda r: dig(r, "pep", "retrans", "cli"), 0, False),
        ("retx far", lambda r: dig(r, "pep", "retrans", "srv") if dig(r, "pep", "mode") != "transparent"
         else dig(r, "pep", "retrans", "lan"), 0, False),
        ("RTT p99", lambda r: r.get("rtt_p99"), 0, False),
        ("mem router KB", lambda r: dig(r, "pep", "mem_kb", "cli", "peak"), 0, False),
        ("mem VPS KB", lambda r: dig(r, "pep", "mem_kb", "srv", "peak"), 0, False)]


def ref_of(arm, refs):
    suf = arm.split("-", 1)[1] if "-" in arm else ""
    for r in refs:
        if r.split("-", 1)[-1] == suf:
            return r
    return refs[0] if refs else None


def table(rows, case, cc, cols, refs, arms):
    sel = [r for r in rows if r["case"] == case and r["cc"] == cc]
    if not sel:
        return ""
    by = {}
    for r in sel:
        by.setdefault(r["arm"], []).append(r)
    order = [a for a in arms if a in by] + sorted(a for a in by if a not in arms)
    pcols = [c for c in cols if c[3]]
    head = ["arm", "n"] + [c[0] for c in cols] + ["p " + c[0] for c in pcols]
    out = ["### %s, %s" % (case, "bulk TCP" if cc != "live" else "live"), "",
           "| " + " | ".join(head) + " |", "| --- | " + " | ".join("---:" for _ in head[1:]) + " |"]
    for a in order:
        rs = by[a]
        ref = ref_of(a, refs)
        cells = [a, str(len(rs))]
        for name, f, nd, _ in cols:
            cells.append(fmt(med([f(r) for r in rs]), nd))
        for name, f, nd, _ in pcols:
            if ref is None or ref == a or ref not in by:
                cells.append("")
            else:
                cells.append(fmt_p(mwu([f(r) for r in rs], [f(r) for r in by[ref]])))
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(prog="peptab.py")
    ap.add_argument("file")
    ap.add_argument("--ref", default="T-copies,T-rlc")
    ap.add_argument("--cases", default=None)
    a = ap.parse_args(argv)
    rows = [json.loads(ln) for ln in open(a.file) if ln.strip()]
    for r in rows:
        r.setdefault("arm", r.get("variant"))
    arms = []
    for r in rows:
        if r["arm"] not in arms:
            arms.append(r["arm"])
    cases = a.cases.split(",") if a.cases else sorted({r["case"] for r in rows})
    refs = a.ref.split(",")
    for c in cases:
        for cc, cols in (("cubic", BULK), ("live", LIVE)):
            t = table(rows, c, cc, cols, refs, arms)
            if t:
                print(t)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
