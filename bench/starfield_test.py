#!/usr/bin/env python3
"""Tests of bench/starfield.py on a synthetic recorder file (python3 bench/starfield_test.py)."""

import gzip
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import starfield  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def synth(path, slots=12):
    """Probes at 10 Hz on STARLINK: each slot a level of 20 + (slot % 3) ms of RTT and
    a drift of 0.2 ms/s, the probe sent 30 ms before each switch lost; the clocks 3000 s
    apart (up carries +offset, down -offset, mod 2^32). Each record holds the last 16."""
    off = 3000 * 1000000
    t0 = 1792443372000  # :12.000 of a minute, UTC
    sent = []
    for k in range(int(slots * 15 * 10)):
        ts = t0 + k * 100 + 70  # sent 70 ms past each tenth: one 30 ms before each switch
        ph = ((ts - t0) / 1000) % 15
        if abs(ph - 14.97) < 1e-6:
            continue
        s = (ts - t0) // 15000
        rtt_us = int((20 + s % 3 + 0.2 * (ph - 7.5)) * 1000)
        up = (rtt_us // 2 + off) % (1 << 32)
        down = (rtt_us - rtt_us // 2 - off) % (1 << 32)
        sent.append((ts + rtt_us // 1000, up, down))
    with gzip.open(path, "wt") as f:
        for i in range(0, len(sent), 10):
            now = sent[i][0] + 5
            last = [p for p in sent[:i + 1]][-16:]
            f.write(json.dumps({"time_ms": now, "links": [{"label": "STARLINK", "name": "sl",
                                "probes": [[now - p[0], p[1], p[2]] for p in last]}]}) + "\n")


def main():
    n = 0
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "rec-000001.jsonl.gz")
        synth(path)
        P = starfield.probes([path], "STARLINK")
        # deduplicated: every probe once, sorted, the RTT free of the clocks' offset
        check("dedup", 12 * 150 - 12 - 16 <= len(P) <= 12 * 150 - 12 and all(b[0] - a[0] >= 50 for a, b in zip(P, P[1:])))
        check("sorted", all(a[0] <= b[0] for a, b in zip(P, P[1:])))
        check("rtt", all(18000 <= p[3] <= 25000 for p in P))
        check("other label", starfield.probes([path], "LTE_NR_01") == [])
        n += 4
        rows = starfield.slot_stats(P, 12)
        k0 = min(rows)
        check("slots", len(rows) >= 10)
        check("levels", all(abs(rows[k][2] - (20 + (k - k0) % 3)) < 0.6 for k in rows))
        check("drift", all(abs(rows[k][1] - 0.2) < 0.05 for k in rows))
        n += 3
        out = "\n".join(starfield.report(P, 12))
        # the lost probes: at the switch (the last 0.5 s bin), sent ~30 ms before it
        line = [x for x in out.splitlines() if x.startswith("lost probes")][0]
        bins = json.loads(line.split(": ", 1)[1])
        check("lost at the switch", bins[-1] >= 10 and sum(bins[:-1]) == 0)
        near = [x for x in out.splitlines() if x.startswith("lost within")][0]
        check("lost before the switch", "p50 -0.03" in near)
        check("no gaps", "gaps of 0.3-60 s: 0" in out)
        n += 3
    if FAILS:
        print("starfield_test: FAILED: " + ", ".join(FAILS))
        return 1
    print("starfield_test: %d/%d ok" % (n, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
