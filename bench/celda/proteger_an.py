#!/usr/bin/env python3
"""proteger_an.py: the table of proteger.py's runs (story 020), each arm
against a reference arm, per profile and traffic kind: medians (and min-max)
with the ratio and a two-sided Mann-Whitney p (scipy) where n >= 3.

  proteger_an.py FILE.jsonl [REF]      REF: the reference arm (full)"""
import json
import statistics as st
import sys

try:
    from scipy.stats import mannwhitneyu
except ImportError:  # the table without p
    mannwhitneyu = None

UDP = [("lost_pct", "perdido %", 3), ("lost_out_pct", "fuera del corte %", 3), ("lost_ep_pct", "en el corte %", 2),
       ("owd.1", "OWD p99 ms", 0), ("lte_rtt.1", "RTT LTE p95 ms", 0), ("cost", "bytes LTE/fijo", 2),
       ("fec_rec", "rehechos", 0), ("late_out.1", "tarde fuera p50 ms", 0), ("late_out.2", "tarde fuera p95 ms", 0)]
TCP = [("tcp_mbit", "TCP Mbit/s", 2), ("tcp_ep_mbit", "en el corte", 2), ("retrans", "retrans.", 0),
       ("ping.1", "ping p99 ms", 0), ("lte_rtt.1", "RTT LTE p95 ms", 0), ("cost", "bytes LTE/fijo", 2)]


def get(r, k):
    if "." in k:
        k, i = k.split(".")
        v = r.get(k)
        return v[int(i)] if isinstance(v, list) and len(v) > int(i) else None
    return r.get(k)


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None


def p(a, b):
    a = [x for x in a if x is not None]
    b = [x for x in b if x is not None]
    if not mannwhitneyu or len(a) < 3 or len(b) < 3 or (len(set(a + b)) == 1):
        return None
    return mannwhitneyu(a, b, alternative="two-sided").pvalue


def fmt(x, n):
    return "–" if x is None else f"{x:.{n}f}".replace(".", ",")


def main():
    rows = [json.loads(x) for x in open(sys.argv[1]) if x.strip()]
    ref = sys.argv[2] if len(sys.argv) > 2 else "full"
    groups = {}
    for r in rows:
        groups.setdefault((r.get("perfil"), r["kind"]), {}).setdefault(r["arm"], []).append(r)
    for (prof, kind), g in sorted(groups.items()):
        cols = UDP if kind == "udp" else TCP
        R = g.get(ref, [])
        print(f"\n### {prof}, {kind} (n por brazo; contra {ref})\n")
        print("| Brazo | n | " + " | ".join(c[1] for c in cols) + " |")
        print("|---|---:|" + "---:|" * len(cols))
        for a in sorted(g, key=lambda x: (x != ref, x)):
            T = g[a]
            cells = []
            for k, _, n in cols:
                xs = [get(x, k) for x in T]
                m = med(xs)
                lo = min((x for x in xs if x is not None), default=None)
                hi = max((x for x in xs if x is not None), default=None)
                c = fmt(m, n) + (f" ({fmt(lo, n)}–{fmt(hi, n)})" if lo is not None and len(T) > 1 else "")
                if a != ref:
                    pv = p(xs, [get(x, k) for x in R])
                    if pv is not None:
                        c += f" p {pv:.3f}".replace(".", ",") if pv >= 0.001 else " p <0,001"
                cells.append(c)
            print(f"| {a} | {len(T)} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
