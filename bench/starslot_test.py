#!/usr/bin/env python3
"""Tests of bench/starslot.py's schedule (python3 bench/starslot_test.py)."""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import starslot  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def main():
    n = 0
    # the same seed draws the same schedule; another seed another one
    a = starslot.plan(61, 1001, 20, [45, 60, 75], [45, 60, 75])
    b = starslot.plan(61, 1001, 20, [45, 60, 75], [45, 60, 75])
    c = starslot.plan(61, 1002, 20, [45, 60, 75], [45, 60, 75])
    check("deterministic", a == b)
    check("seeded", a != c)
    n += 2
    # ordered, within the run, a step every 0.5 s
    check("ordered", all(x["t"] <= y["t"] for x, y in zip(a, a[1:])))
    check("within", all(0 <= e["t"] < 61 for e in a))
    check("steps", sum(1 for e in a if e["loss"] == 0 and abs(e["t"] * 2 - round(e["t"] * 2)) < 1e-9) >= 4 * 30 - 2)
    n += 3
    # each switch: 25 ms lost, then 20 ms held 55 ms more, then the next slot
    for edge in (15, 30, 45, 60):
        lost = [e for e in a if abs(e["t"] - (edge - 0.045)) < 1e-6]
        held = [e for e in a if abs(e["t"] - (edge - 0.020)) < 1e-6]
        nxt = [e for e in a if abs(e["t"] - edge) < 1e-6]
        check("cut %d" % edge, len(lost) == 1 and lost[0]["loss"] == 100)
        check("hold %d" % edge, len(held) == 1 and held[0]["loss"] == 0 and len(lost) == 1
              and abs(held[0]["delay"] - lost[0]["delay"] - starslot.HOLD_MS) < 0.01)
        check("next %d" % edge, len(nxt) == 1 and nxt[0]["loss"] == 0)
        n += 3
    # the delays stay within the measured spread: level and drift over half a slot
    lo = 20 + min(starslot.LEVEL_MS) - max(abs(x) for x in starslot.SLOPE_MS_S) * 7.5 - 0.1
    hi = 20 + max(starslot.LEVEL_MS) + max(abs(x) for x in starslot.SLOPE_MS_S) * 7.5 + starslot.HOLD_MS + 0.1
    check("delay range", all(lo <= e["delay"] <= hi for e in a))
    n += 1
    # one rate per slot, the same index up and down
    d = starslot.plan(61, 7, 11, [30, 40, 50], [300, 400, 500])
    check("rates paired", all(e["down"] == e["up"] * 10 for e in d))
    slots = {}
    for e in d:
        slots.setdefault(int(e["t"] // 15), set()).add(e["up"])
    check("rate per slot", all(len(v) == 1 for k, v in slots.items() if k < 4))
    n += 2
    # outages: rare (1.45 an hour), each a span of total loss of a measured length
    hours = 0
    outs = 0
    for seed in range(400):
        e = starslot.plan(61, seed, 20, [60], [60])
        hours += 61 / 3600
        spans = [x for x in e if x["loss"] == 100 and abs((x["t"] + 0.045) % 15) > 1e-6
                 and abs((x["t"] + 0.045) % 15 - 15) > 1e-6]
        outs += len({round(x["t"], 3) for x in spans if all(abs(x["t"] - y["t"] - 0.5) > 1e-6 or y["loss"] == 0
                                                            for y in spans)})
    check("outages rare", 0.3 < outs / hours < 4)
    n += 1
    e = starslot.plan(61, 1, 20, [60], [60], outages=False)
    check("no outages", all(x["loss"] == 0 or abs((x["t"] + 0.045) % 15) < 1e-6
                            or abs((x["t"] + 0.045) % 15 - 15) < 1e-6 for x in e))
    n += 1
    # a total cut of its own at each switch (starlink15ramp keeps starlink15's 1.5 s)
    r = starslot.plan(31, 3, 20, [100], [100], cut_ms=1500, outages=False)
    starts = [y["t"] for x, y in zip(r, r[1:]) if y["loss"] == 100 and x["loss"] == 0]
    check("cut-ms", starts == [13.5, 28.5])
    after = [x for x in r if 13.5 < x["t"] < 15]
    check("cut-ms holds", all(x["loss"] == 100 for x in after) if after else True)
    n += 2
    # the old guess: a rise of 10 to 25 ms within the slot
    m = starslot.plan(31, 3, 20, [100], [100], model="ramp", cut_ms=1500)
    first = [x for x in m if x["t"] < 13.5 and x["loss"] == 0]
    check("ramp rises", first[-1]["delay"] - first[0]["delay"] >= 9)
    n += 1
    # netem's limit as bond_link counts it
    check("limit", starslot.limit(60, 15, 20, 0) == 60 * 1000000 * 15 // 8000 // 1400 + 10)
    check("limit inflight", starslot.limit(60, 15, 20, 1) == starslot.limit(60, 15, 20, 0) + 60 * 1000000 * 20 // 8000 // 1400)
    n += 2
    if FAILS:
        print("starslot_test: FAILED: " + ", ".join(FAILS))
        return 1
    print("starslot_test: %d/%d ok" % (n, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
