#!/usr/bin/env python3
"""Tests of bench/multisl.py's schedule (python3 bench/multisl_test.py)."""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import multisl  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def args(*extra):
    a = multisl.parse(["plan", "--secs", "600", "--seed", "7", "--links", "1 2 3"] + list(extra))
    a.off = 0.0
    a.cut_t = (a.secs / 3, 2 * a.secs / 3)
    return a


def main():
    n = 0
    # deterministic and seeded
    a, b = args("--ep", "0.6"), args("--ep", "0.6")
    check("deterministic", multisl.plan(a) == multisl.plan(b))
    c = multisl.parse(["plan", "--secs", "600", "--seed", "8", "--links", "1 2 3", "--ep", "0.6"])
    c.off, c.cut_t = 0.0, (200, 400)
    check("seeded", multisl.plan(a) != multisl.plan(c))
    n += 2
    # rho moves the overlap, not each dish's loss: about --ep alone at any rho,
    # joint near 0 at rho 0 and equal to alone at rho 1
    j = {r: multisl.joint(args("--ep", "0.6", "--rho", str(r), "--edge", "0")) for r in (0, 0.5, 1)}
    for r, v in j.items():
        check("alone %s" % r, all(0.35 < x < 0.9 for x in v["alone_pct"]))
    check("rho 0 joint", j[0]["joint_pct"] < 0.02)
    check("rho 0.5 joint", 0.15 < j[0.5]["joint_pct"] < 0.45)
    check("rho 1 joint", abs(j[1]["joint_pct"] - j[1]["alone_pct"][0]) < 1e-3)
    n += 6
    # edges: with --edge 1 every dish loses 25 ms of each 15 s, all at once
    e = multisl.joint(args("--edge", "1"))
    check("edge alone", all(abs(x - 100 * 0.025 / 15) < 0.01 for x in e["alone_pct"]))
    check("edge joint", abs(e["joint_pct"] - e["alone_pct"][0]) < 0.01)
    h = multisl.joint(args("--edge", "0.5"))
    check("edge half", all(0.05 < x < 0.12 for x in h["alone_pct"]) and h["joint_pct"] < h["alone_pct"][0] / 2)
    n += 3
    # the edges fall at the slots' ends (plan time 0 is an edge, the wall clock's)
    ev = multisl.plan(args("--edge", "1"))
    lost = sorted({round(t, 3) for t, k, s in ev if s[3] == 100})
    check("edge times", lost[:3] == [14.955, 29.955, 44.955])
    n += 1
    # the cut: one dish or all, the run's middle third
    one = multisl.joint(args("--cut", "one", "--edge", "0"))
    check("cut one", abs(one["alone_pct"][0] - 20) < 0.1 and one["alone_pct"][1] == 0 and one["joint_pct"] == 0)
    allc = multisl.joint(args("--cut", "all", "--edge", "0"))
    check("cut all", all(abs(x - 20) < 0.1 for x in allc["alone_pct"]))
    n += 2
    # independent random loss: netem's, per packet, in every state
    ind = multisl.plan(args("--ind", "0.5", "--edge", "0"))
    check("ind", all(abs(s[3] - 0.5) < 1e-9 for t, k, s in ind))
    # the tc line
    line = multisl.tc_line("l1", 15.04, 25, 15, 0.25, 0)
    check("tc line", line == "qdisc replace dev l1 root netem delay 15.0ms rate 25mbit limit 43 loss 0.25%\n")
    n += 2
    for f in FAILS:
        print("FAIL:", f)
    print("multisl_test: %d of %d passed" % (n - len(FAILS), n))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
