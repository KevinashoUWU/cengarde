#!/usr/bin/env python3
"""Starlink's 15 s slots from the field recorder (bench/fieldrec.py's
rec-*.jsonl.gz, the engine's status at 1 Hz), story 015.

  starfield.py [--label STARLINK] [--edge 12] REC.jsonl.gz ...

Since 2026-10-19 each link's "probes" holds its last 16 probe replies as
[age ms, up us, down us]: the one-way delays carry the two clocks' offset,
but their sum does not, so (up + down) mod 2^32 us is the probe's RTT. A
reply shows up in several records: deduplicated by (up, down). Its time is
the record's time_ms minus its age; its slot phase, that time minus the
edge (:12, :27, :42, :57 of each minute, UTC: --edge 12) modulo 15 s.

Prints what bench/starslot.py models: the slot's base level (the 10th
percentile of each second, its median over the slot) and its quantiles,
the drift within a slot (a line through the seconds 1 to 14), the step at a
switch, the probes lost by phase and the ones sent just before a switch,
and the gaps of 300 ms or more (outages). Only changes of up and down
count, never their values.
"""

import argparse
import gzip
import json
import statistics as st
import sys

sys.dont_write_bytecode = True

SLOT = 15.0


def q(a, x):
    a = sorted(a)
    return a[min(len(a) - 1, int(len(a) * x))] if a else float("nan")


def probes(paths, label):
    """[(t_ms, up_us, down_us, rtt_us)] of the link named label (or by name), sorted
    and deduplicated."""
    seen, out = {}, []
    for p in paths:
        with gzip.open(p, "rt") if p.endswith(".gz") else open(p) as f:
            for ln in f:
                try:
                    d = json.loads(ln)
                except ValueError:
                    continue
                t = d.get("time_ms")
                for lk in d.get("links") or []:
                    if label not in (lk.get("label"), lk.get("name")) or t is None:
                        continue
                    for pr in lk.get("probes") or []:
                        age, up, down = pr
                        # the same reply in the next records: the same delays, its
                        # time within a few ms (the age is rounded); another probe
                        # with the same two delays is at least an interval away
                        if (up, down) in seen and abs(seen[(up, down)] - (t - age)) < 50:
                            continue
                        seen[(up, down)] = t - age
                        out.append((t - age, up, down, (up + down) % (1 << 32)))
    out.sort()
    return out


def phase(t_ms, edge):
    return (t_ms / 1000 - edge) % SLOT


def slot_stats(P, edge):
    """Per slot: the level of each second (p10 of its RTTs, ms) and the drift."""
    slots = {}
    for t, _, _, r in P:
        slots.setdefault(int((t / 1000 - edge) // SLOT), []).append((phase(t, edge), r / 1000))
    rows = {}
    for s, v in slots.items():
        lv = []
        for sec in range(15):
            w = [x[1] for x in v if sec <= x[0] < sec + 1]
            lv.append(q(w, 0.1) if len(w) >= 5 else None)
        xs = [i + 0.5 for i in range(1, 14) if lv[i] is not None]
        ys = [lv[i] for i in range(1, 14) if lv[i] is not None]
        if len(xs) < 8:
            continue
        mx, my = st.mean(xs), st.mean(ys)
        slope = sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / sum((a - mx) ** 2 for a in xs)
        rows[s] = (lv, slope, st.median(ys))
    return rows


def report(P, edge):
    out = []
    if len(P) < 100:
        return ["too few probes: %d" % len(P)]
    rtt = [p[3] / 1000 for p in P]
    span = sum(b[0] - a[0] for a, b in zip(P, P[1:]) if b[0] - a[0] < 60000)  # the recorder's gaps out
    out.append("probes %d over %.1f h; RTT ms p5 %.1f p50 %.1f p95 %.1f" % (
        len(P), span / 3.6e6, q(rtt, .05), q(rtt, .5), q(rtt, .95)))
    rows = slot_stats(P, edge)
    lev = [r[2] for r in rows.values()]
    sl = [r[1] for r in rows.values()]
    out.append("slots %d; level (RTT floor) ms p5 %.1f p25 %.1f p50 %.1f p75 %.1f p95 %.1f" % (
        (len(rows),) + tuple(q(lev, x) for x in (.05, .25, .5, .75, .95))))
    ks = sorted(rows)
    levs = [rows[k][2] for k in ks]
    if len(levs) > 2:
        m = st.mean(levs)
        den = sum((x - m) ** 2 for x in levs) or 1
        out.append("level lag-1 autocorrelation %.2f" % (sum((levs[i] - m) * (levs[i + 1] - m)
                                                           for i in range(len(levs) - 1)) / den))
    out.append("drift ms/s p10 %.2f p25 %.2f p50 %.2f p75 %.2f p90 %.2f; over 13 s |drift| p50 %.1f p90 %.1f ms" % (
        tuple(q(sl, x) for x in (.1, .25, .5, .75, .9)) + (13 * q([abs(x) for x in sl], .5),
                                                          13 * q([abs(x) for x in sl], .9))))
    steps = [rows[k + 1][0][1] - rows[k][0][14] for k in ks
             if k + 1 in rows and rows[k][0][14] is not None and rows[k + 1][0][1] is not None]
    if steps:
        out.append("step at a switch ms p10 %.1f p50 %.1f p90 %.1f; |step| p50 %.1f p90 %.1f" % (
            q(steps, .1), q(steps, .5), q(steps, .9), q([abs(x) for x in steps], .5), q([abs(x) for x in steps], .9)))
    qs = [(2 * i + 1) / 32 for i in range(16)]
    med = q(lev, .5)
    out.append("LEVEL_MS (one-way, 16 quantiles): " + " ".join("%.1f" % ((q(lev, x) - med) / 2) for x in qs))
    out.append("SLOPE_MS_S (one-way, 16 quantiles): " + " ".join("%.2f" % (q(sl, x) / 2) for x in qs))
    # lost probes: the gaps between replies (10 Hz), by the phase of their send time
    lost, near, bins = 0, [], [0] * 30
    gaps = []
    for a, b in zip(P, P[1:]):
        g = b[0] - a[0]
        if 300 <= g < 60000:
            gaps.append((g, a[0]))
        if 150 < g < 1000:
            n = round(g / 100) - 1
            lost += n
            for j in range(n):
                ts = a[0] + (j + 1) * g / (n + 1) - a[3] / 1000
                ph = phase(ts, edge)
                bins[int(ph / SLOT * 30) % 30] += 1
                if ph > SLOT - 1 or ph < 1:
                    near.append(((ph + 1) % SLOT) - 1)
    out.append("lost probes %d (%.2f %%), by phase (0.5 s bins): %s" % (lost, 100 * lost / (lost + len(P)), bins))
    if near:
        out.append("lost within 1 s of a switch: %d, send time from the switch s p10 %.3f p50 %.3f p90 %.3f" % (
            len(near), q(near, .1), q(near, .5), q(near, .9)))
    out.append("gaps of 0.3-60 s: %d %s" % (len(gaps), " ".join("%d ms (phase %.1f)" % (g, phase(t, edge))
                                                              for g, t in gaps[:20])))
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description="Starlink's slots from field recorder files (see the docstring).")
    p.add_argument("files", nargs="+")
    p.add_argument("--label", default="STARLINK")
    p.add_argument("--edge", type=float, default=12.0, help="a slot edge, seconds past the minute (UTC)")
    a = p.parse_args(argv)
    print("\n".join(report(probes(a.files, a.label), a.edge)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
