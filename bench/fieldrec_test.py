#!/usr/bin/env python3
"""Tests for bench/fieldrec.py on a made-up recording whose answers are known:
links a and b fail together (b also alone), c fails on its own, sl is a
Starlink that loses 1.5 s and changes its RTT at seconds 12, 27, 42 and 57;
the engine restarts once, the recorder stops for a minute, the clock is set
by NTP after the first 100 s, one file comes from an engine without time_ms
and the last one was cut by a power loss. That recording is also an old one,
without the engine's passive instruments (joint, lost_all, sizes): the new
sections say there is no data. A second, bigger recording has them, with the
answers known by construction: three hours, five links (ids not in name
order, one with a label, one with id 9 that only shows in "other"), c and d
late together in the first hour and independent after it, d missing from a
quarter of the packets' expect, an engine restart and a counter that restarts
within a run. A third one is for the radio section (--rf): two hours of four links
with outages and mutes at known times, and the RF metrics of cell-monitor in
line protocol and annotated CSV, shaped like its output: a link whose RAT
changes come just before its outages, one whose changes do not, one mapped by
its carrier, one without radio, series that map to no link, a data hole, a
modem that was down, and RF data that starts after the recording does and
ends after it. Run: python3 bench/fieldrec_test.py
"""

import bisect
import collections
import csv
import gzip
import io
import json
import os
import random
import re
import sys
import tempfile
import time
import contextlib

sys.dont_write_bytecode = True  # no bench/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fieldrec  # noqa: E402

T0 = 1791500000000  # wall ms at the first sample: 2026-10-08, second 20 of a minute
DUR = 3600

A = [(130 + 170 * k, 3.0) for k in range(20)]
B = A + [(80 + 400 * k, 2.0) for k in range(5)]
C = [(1105 + 230 * k, 4.0) for k in range(10)]
RESTART, GAP = 1800, (2400, 2460)


def sl_out(t_wall_s):
    """Starlink: out 1.5 s from seconds 12, 27, 42 and 57 of every minute."""
    s = t_wall_s % 60
    return any(b <= s < b + 1.5 for b in (12, 27, 42, 57))


def sl_rtt(t_wall_s):
    return 25.0 if int((t_wall_s - 12) // 15) % 2 == 0 else 55.0


def out_in(events, t):
    return any(s <= t < s + d for s, d in events)


def last_rx(t, is_out):
    """The last probe reply (every 100 ms) not inside an outage."""
    x = int(t * 10) / 10
    while is_out(x) and x > t - 3600:
        x -= 0.1
    return x


def make(dirn):
    lines = []
    rx = {n: [0, 0, 0] for n in ("a", "b", "c", "sl")}  # rx copies, tx, server rx
    down = 0
    for k in range(DUR):
        t = k + 0.137                      # seconds since the start
        if GAP[0] <= t < GAP[1]:
            continue
        run = 0 if t < RESTART else 1
        up = int((t - (0 if run == 0 else RESTART - 5)) * 1000)
        wall = T0 + int(t * 1000)
        if t < 100:
            wall -= 3 * 3600 * 1000        # before NTP
        down += 500
        links = []
        for n, ev, rtt in (("a", A, 30.0), ("b", B, 40.0), ("c", C, 60.0), ("sl", None, None)):
            tw = (T0 / 1000) + t
            if ev is None:
                is_out = lambda x: sl_out((T0 / 1000) + x)  # noqa: E731
                r = sl_rtt(tw)
            else:
                is_out = lambda x, ev=ev: out_in(ev, x)  # noqa: E731
                r = rtt
            ago = int(1000 * (t - last_rx(t, is_out)))
            out_now = is_out(t)
            c = rx[n]
            c[1] += 100
            if not out_now:
                c[0] += 500
                c[2] += 100
            # c's engine has not called it stalled yet: only its silence tells.
            state = "stalled" if ago > 300 and n != "c" else "live"
            links.append({"id": len(links), "name": n, "label": "", "state": state,
                          "rtt_ms": r, "last_rx_ms_ago": ago, "upload": "active", "upload_behind_ms": 0.0,
                          "upload_mutes": 0, "download_muted": False, "tx_packets": c[1], "rx_first": c[0] // 2,
                          "rx_duplicate": c[0] - c[0] // 2, "rx_late": 0, "rx_missed": 0,
                          "server_view": {"age_ms": 50, "rx": c[2] % 2**32, "first": 0, "lag_ms": 0.0}})
        r = {"mode": "client", "uptime_ms": up, "time_ms": wall, "session": f"s{run}",
             "probe_interval_ms": 100, "download": {"packets": down}, "links": links}
        lines.append(r)
    # Files: hourly-ish chunks; one without time_ms (an older engine: "t"),
    # one cut short, and the current one plain.
    chunks = [lines[:900], lines[900:1700], lines[1700:3000], lines[3000:]]
    for i, ch in enumerate(chunks[:3]):
        data = "".join(json.dumps(x) + "\n" for x in ch).encode()
        if i == 1:
            data = "".join(json.dumps({**{kk: vv for kk, vv in x.items() if kk != "time_ms"},
                                       "t": x["time_ms"] // 1000}) + "\n" for x in ch).encode()
        z = gzip.compress(data)
        if i == 2:
            z = z[:len(z) - 64]           # a power loss
        with open(os.path.join(dirn, f"rec-{i + 1:06d}-x.jsonl.gz"), "wb") as fh:
            fh.write(z)
    with open(os.path.join(dirn, "current.jsonl"), "w") as fh:
        for x in chunks[3]:
            fh.write(json.dumps(x) + "\n")
        fh.write('{"mode":"client","upt')   # the line being written


# --- the passive instruments: joint lateness, lost on every link, sizes ---------

T1 = 1791504000000            # 2026-10-08 00:00:00 UTC
NS, RESTART_AT = 5400, 2700   # samples 2 s apart: three hours; the engine restarts at 01:30
IDS = {"a": 2, "b": 5, "c": 0, "d": 7, "e": 3, "z": 9}   # z: id 9, only in "other"
LABEL = {"c": "5g"}           # shown as "c (5g)"
RTT = {"a": 60.0, "b": 70.0, "c": 20.0, "d": 25.0, "e": 90.0, "z": 10.0}
DLS = (50, 100, 150)
SHARE = {50: dict(a=.05, b=.05, e=.03, cd=.02),     # late shares per deadline
         100: dict(a=.02, b=.02, e=.015, cd=.01),
         150: dict(a=.01, b=.01, e=.0075, cd=.005)}
PKTS, OTHER, LOST_PER = 20000, 500, 40   # per sample; lost on every link in the first hour only
EDGES = [32, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1440]
REP = [32, 70, 112, 160, 224, 320, 448, 640, 896, 1152, 1360, 1500]   # a size inside each bucket
UP = [1000, 3000, 300, 200, 100, 100, 100, 100, 100, 100, 100, 800]
DOWN = [100, 500, 100, 100, 100, 100, 100, 200, 300, 400, 1000, 3000]
LINKS5 = ["a", "b", "c (5g)", "d", "e"]


def bit(*names):
    return sum(1 << IDS[n] for n in names)


def late_dist(groups):
    """{mask of link ids: share} for independent groups of links that are late together."""
    out = {0: 1.0}
    for names, p in groups:
        new = collections.defaultdict(float)
        for m, q in out.items():
            new[m] += q * (1 - p)
            new[m | bit(*names)] += q * p
        out = new
    return out


def regime(hour, d):
    """[(packets per sample, expect mask, late shares)]: in the first hour c and
    d are late together; after it they are independent and a quarter of the
    packets are not sent on d."""
    s = SHARE[d]
    one = [(["a"], s["a"]), (["b"], s["b"]), (["e"], s["e"])]
    if hour == 0:
        return [(PKTS, bit("a", "b", "c", "d", "e"), late_dist(one + [(["c", "d"], s["cd"])]))]
    ind = one + [(["c"], s["cd"])]
    return [(15000, bit("a", "b", "c", "d", "e"), late_dist(ind + [(["d"], s["cd"])])),
            (5000, bit("a", "b", "c", "e"), late_dist(ind))]


BOND_MEMO = {}


def bond_lines(soft_reset=None):
    """The lines of _bond_lines, made once (nothing changes them)."""
    if soft_reset not in BOND_MEMO:
        BOND_MEMO[soft_reset] = _bond_lines(soft_reset)
    return BOND_MEMO[soft_reset]


def _bond_lines(soft_reset=None):
    """Status lines every 2 s for three hours; counters accumulate as floats and
    are rounded when written, so the totals are exact to a packet."""
    lines, st = [], None
    for k in range(NS):
        if k in (0, RESTART_AT):
            r0 = k                                 # a new run: the uptime starts over too
        if k in (0, RESTART_AT, soft_reset):       # counters that start over (soft_reset: in the same run)
            st, j0 = dict(c=collections.defaultdict(float)), k
        j = k - j0
        run = int(k >= RESTART_AT)
        hour = (k * 2) // 3600
        c = st["c"]
        if j:
            for n, em, _ in regime(hour, 50):
                c["pk"] += n
                c[("e", em)] += n
            for di, d in enumerate(DLS):
                for n, em, dist in regime(hour, d):
                    for m, sh in dist.items():
                        c[("l", di, m)] += n * sh
            c["pk"] += OTHER
            c["other"] += OTHER
            c["lost"] += LOST_PER if hour == 0 else 0
            c["down"] += PKTS
            for i in range(12):
                c[("ud", i)] += UP[i]
                c[("ub", i)] += UP[i] * REP[i]
                c[("dd", i)] += DOWN[i]
                c[("db", i)] += DOWN[i] * REP[i]
            c["init"] += j % 50 == 0
            c["response"] += j % 50 == 0
            c["cookie"] += j % 700 == 0
            c["uother"] += j % 1000 == 0

        def v(key):
            return int(c[key] + 0.5)

        def hist(sel):
            return {str(key[-1]): v(key) for key in sorted(c, key=str) if isinstance(key, tuple) and sel(key) and v(key)}

        links = [{"id": i, "name": n, "label": LABEL.get(n, ""), "state": "live", "rtt_ms": RTT[n],
                  "last_rx_ms_ago": 20, "rx_first": v("down")} for n, i in IDS.items()]
        lines.append({
            "mode": "client", "uptime_ms": (100000 if run == 0 else 4000) + 2000 * (k - r0),
            "time_ms": T1 + 2000 * k, "session": f"s{run}", "probe_interval_ms": 100,
            "download": {"packets": v("down")}, "links": links, "lost_all": v("lost"),
            "joint": {"deadlines_ms": list(DLS), "packets": v("pk"), "other": v("other"),
                      "expect": hist(lambda key: key[0] == "e"),
                      "late": [hist(lambda key, di=di: key[0] == "l" and key[1] == di) for di in range(3)]},
            "sizes": {"edges": EDGES,
                      "up": {"data": [v(("ud", i)) for i in range(12)], "data_bytes": [v(("ub", i)) for i in range(12)],
                             "init": v("init"), "response": 0, "cookie": 0, "other": v("uother")},
                      "down": {"data": [v(("dd", i)) for i in range(12)], "data_bytes": [v(("db", i)) for i in range(12)],
                               "init": 0, "response": v("response"), "cookie": v("cookie"), "other": 0}}})
    return lines


def analyze_lines(lines, utc=0.0):
    """The whole pipeline on status lines written to a directory."""
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "current.jsonl"), "w") as fh:
            for x in lines:
                fh.write(json.dumps(x) + "\n")
        notes = []
        segs = fieldrec.segments(fieldrec.read_lines(fieldrec.files_of([d]), notes), notes)
        return fieldrec.summarize(fieldrec.analyze(segs, utc)), notes


def rendered(s, notes, utc=0.0):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        fieldrec.report(s, notes, utc)
    return out.getvalue()


FAILS = []


def check(what, ok, got=None):
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f": {got}"))
    if not ok:
        FAILS.append(what)


def near(x, y, rel=0.03, ab=0.0):
    return x is not None and abs(x - y) <= max(ab, rel * abs(y))


def sub(view, di, *names):
    """The entry of a set of links in a view's deadline di."""
    return next(x for x in view["deadlines"][di]["subsets"] if x["set"] == list(names))


def bond_checks():
    lines = bond_lines()
    s, notes = analyze_lines(lines)
    j, ov = s["joint"], s["joint"]["overall"]
    n_int = 2 * (NS // 2 - 1)    # two runs of 2700 samples: 2699 intervals each
    check("joint: an engine restart does not make a negative or a lost interval",
          j["intervals"] == n_int and j["skipped_intervals"] == 0 and j["packets"] == (PKTS + OTHER) * n_int,
          (j["intervals"], j["skipped_intervals"], j["packets"]))
    check("joint: the deadlines come from the engine", j["deadlines_ms"] == list(DLS), j["deadlines_ms"])
    check("joint: five links, in name order; id 9 only in other", j["links"] == LINKS5 and "z" not in j["link_packets"],
          j["links"])
    check("joint: other counts the packets outside the histograms",
          j["other"] == OTHER * n_int and near(j["other_pct"], 100 * OTHER / (PKTS + OTHER), 0.01), (j["other"], j["other_pct"]))
    check("joint: 26 sets of 2 to 5 links per deadline", all(len(v["subsets"]) == 26 for v in ov["deadlines"]),
          [len(v["subsets"]) for v in ov["deadlines"]])
    check("joint: the RTT p50 is kept for the best pair by RTT", j["rtt_p50_ms"]["c (5g)"] == 20.0, j["rtt_p50_ms"])
    # --- hour 0 (c and d late together), against what the generator was told.
    h0, h1 = j["by_hour"][0], j["by_hour"][1]
    check("joint: by hour of the day (UTC)", sorted(j["by_hour"]) == [0, 1, 2], sorted(j["by_hour"]))
    for di, d in enumerate(DLS):
        sh = SHARE[d]
        cd = sub(h0, di, "c (5g)", "d")
        check(f"{d} ms, hour 0: P(c and d late) = their own share, lift 1/share",
              near(cd["late_pct"], 100 * sh["cd"]) and near(cd["lift"], 1 / sh["cd"], 0.04), cd)
        ab = sub(h0, di, "a", "b")
        check(f"{d} ms, hour 0: a and b are independent (lift 1)",
              near(ab["late_pct"], 100 * sh["a"] * sh["b"]) and near(ab["lift"], 1.0, 0.04), ab)
        ac = sub(h0, di, "a", "c (5g)")
        check(f"{d} ms, hour 0: a and c are independent (lift 1)", near(ac["lift"], 1.0, 0.04), ac)
    # P(all five late) is 1.5e-6 at 50 ms (54 packets); the later deadlines are too rare to judge here.
    al = sub(h0, 0, *LINKS5)
    sh = SHARE[50]
    check("50 ms, hour 0: P(all five late) = a*b*(c and d)*e, lift 1/share of c and d",
          near(al["late_pct"], 100 * sh["a"] * sh["b"] * sh["cd"] * sh["e"], 0.06) and al["packets"] == PKTS * 1799
          and near(al["lift"], 1 / sh["cd"], 0.08), al)
    b = h0["deadlines"][0]["best"]
    check("best pair by single lateness is c and d, whose P is 2 %",
          b["by_single"]["set"] == ["c (5g)", "d"] and near(b["by_single"]["late_pct"], 2.0), b["by_single"])
    check("best pair by RTT is c and d too (not z, which has the lowest RTT but id 9)",
          b["by_rtt"]["set"] == ["c (5g)", "d"], b["by_rtt"])
    check("best pair by joint lateness has e and one of c or d: 0.06 %",
          b["by_joint"]["set"] in (["c (5g)", "e"], ["d", "e"]) and near(b["by_joint"]["late_pct"], 100 * sh["cd"] * sh["e"]), b["by_joint"])
    check("choosing by single figures costs 33x here (2 / 0.06), by RTT too",
          near(b["ratio_single"], 1 / sh["e"], 0.04) and near(b["ratio_rtt"], 1 / sh["e"], 0.04), b)
    check("pair over all late: 0.06 % over 0.00015 %", near(b["pair_over_all"], sh["cd"] * sh["e"] / (sh["a"] * sh["b"] * sh["cd"] * sh["e"]), 0.08)
          and b["all"]["set"] == LINKS5, b)
    # --- hour 1: independent, d not in a quarter of the expect.
    cd = sub(h1, 0, "c (5g)", "d")
    check("hour 1: c and d independent (lift 1) over the packets sent on both",
          near(cd["late_pct"], 100 * sh["cd"] ** 2) and near(cd["lift"], 1.0, 0.05) and abs(cd["packets"] - 15000 * 1799) <= 2, cd)
    sg = h1["deadlines"][0]["single"]
    check("hour 1: a link's own share is over the packets sent on it",
          abs(sg["c (5g)"]["packets"] - PKTS * 1799) <= 2 and abs(sg["d"]["packets"] - 15000 * 1799) <= 2
          and near(sg["c (5g)"]["late_pct"], 2.0) and near(sg["d"]["late_pct"], 2.0), (sg["c (5g)"], sg["d"]))
    b1 = h1["deadlines"][0]["best"]
    check("hour 1: the best pair by joint lateness is c and d, the same as by single: ratio 1",
          b1["by_joint"]["set"] == ["c (5g)", "d"] and b1["by_single"]["set"] == ["c (5g)", "d"] and b1["ratio_single"] == 1.0, b1)
    ov_cd = sub(ov, 0, "c (5g)", "d")
    w0, w1 = PKTS * 1799, 15000 * 3599
    want = (sh["cd"] * w0 + sh["cd"] ** 2 * w1) / (w0 + w1)
    check("overall: all the hours added up, over the packets sent on the set",
          near(ov_cd["late_pct"], 100 * want, 0.02) and abs(ov_cd["packets"] - (w0 + w1)) <= 4, (ov_cd, 100 * want))
    # --- lost on every link
    la = s["lost_all"]
    lost0 = LOST_PER * 1799
    check("lost_all: overall and per hour, with the restart",
          la["overall"]["lost"] == lost0 and la["overall"]["delivered"] == PKTS * n_int and la["by_hour"][0]["lost"] == lost0
          and la["by_hour"][1]["lost"] == 0 and la["by_hour"][2]["lost"] == 0, la)
    check("lost_all: loss is lost over lost plus delivered",
          near(la["by_hour"][0]["loss_pct"], 100 * LOST_PER / (LOST_PER + PKTS), 0.001) and la["by_hour"][1]["loss_pct"] == 0.0, la["by_hour"])
    # --- sizes
    z = s["sizes"]
    check("sizes: edges, direction totals and mean size", z["edges"] == EDGES and z["up"]["data_packets"] == sum(UP) * n_int
          and z["down"]["data_packets"] == sum(DOWN) * n_int
          and near(z["up"]["mean_bytes"], sum(u * r for u, r in zip(UP, REP)) / sum(UP), 0.001), (z["edges"], z["up"]["mean_bytes"]))
    ub = z["up"]["buckets"]
    tb = sum(u * r for u, r in zip(UP, REP))
    check("sizes up: 4000 of 6000 packets are 96 bytes or less, and their share of the bytes",
          [x["up_to"] for x in ub] == EDGES + [None] and near(ub[1]["cum_packets_pct"], 100 * 4000 / 6000, 0.001)
          and near(ub[1]["cum_bytes_pct"], 100 * (1000 * 32 + 3000 * 70) / tb, 0.001) and near(ub[-1]["cum_packets_pct"], 100.0, 0.001),
          (ub[1], ub[-1]))
    db = z["down"]["buckets"]
    check("sizes down: another distribution, 3000 of 6000 above 1280 bytes",
          near(100 - db[9]["cum_packets_pct"], 100 * (1000 + 3000) / 6000, 0.001) and near(db[0]["packets_pct"], 100 * 100 / 6000, 0.001), db[9])
    hs = (z["up"]["handshakes"], z["down"]["handshakes"])
    check("sizes: handshake counts add up over the two runs (every 50, 700 and 1000 samples of a run)",
          hs[0] == dict(init=2 * (n_int // 2 // 50), response=0, cookie=0, other=2 * (n_int // 2 // 1000))
          and hs[1] == dict(init=0, response=2 * (n_int // 2 // 50), cookie=2 * (n_int // 2 // 700), other=0), hs)
    text = rendered(s, notes)
    for what in ("## Joint lateness", "### Each link alone", "### Sets of links, 50 ms after the first copy",
                 "### Best pair", "### By hour (UTC+0)", "### Pairs by hour (UTC+0)", "#### 100 ms", "| hour | a + b | a + c (5g) |", "## Lost on every link", "## WireGuard sizes", "### up:", "### down:",
                 "| a + b |", "| a + c (5g) + d + e |", "<= 32 (keepalive)", "<= 96 (empty ACK)", "> 1440", "Handshakes: init"):
        check(f"report has {what!r}", what in text)
    check("report: the engine's other counter is explained", "the engine could not judge" in text)
    # --- --utc-offset moves the hours.
    s3, _ = analyze_lines(lines, -3)
    check("--utc-offset shifts the hours (21, 22 and 23 for UTC-3)", sorted(s3["joint"]["by_hour"]) == [21, 22, 23]
          and sorted(s3["lost_all"]["by_hour"]) == [21, 22, 23], sorted(s3["joint"]["by_hour"]))
    # --- the JSON is strict and has the three sections.
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "current.jsonl"), "w") as fh:
            for x in lines:
                fh.write(json.dumps(x) + "\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = fieldrec.main([d, "--json", os.path.join(d, "s.json"), "--utc-offset", "-3"])

        def bad(c):
            raise ValueError(c)
        js = json.load(open(os.path.join(d, "s.json")), parse_constant=bad)
    check("--json: joint, lost_all and sizes, machine-readable", rc == 0 and js["joint"]["links"] == LINKS5
          and js["joint"]["by_hour"]["21"]["deadlines"][0]["best"]["by_single"]["set"] == ["c (5g)", "d"]
          and js["lost_all"]["by_hour"]["21"]["lost"] == lost0 and js["sizes"]["up"]["buckets"][0]["up_to"] == 32, list(js))
    # --- a counter that restarts within a run (no uptime change): that interval is skipped, the rest kept.
    s4, _ = analyze_lines(bond_lines(soft_reset=1000))
    check("a counter reset within a run: one interval skipped in each section, the others added up",
          s4["joint"]["intervals"] == n_int - 1 and s4["joint"]["skipped_intervals"] == 1
          and s4["lost_all"]["skipped_intervals"] == 1 and s4["lost_all"]["overall"]["lost"] == lost0 - LOST_PER
          and s4["sizes"]["skipped_intervals"] == 1 and s4["sizes"]["intervals"] == n_int - 1,
          (s4["joint"]["intervals"], s4["joint"]["skipped_intervals"], s4["lost_all"], s4["sizes"]["skipped_intervals"]))
    # --- the engine was upgraded: the first run has no instruments, the second has them.
    mixed = [{k: v for k, v in x.items() if k not in ("joint", "lost_all", "sizes")} if i < RESTART_AT else x
             for i, x in enumerate(lines)]
    s5, _ = analyze_lines(mixed)
    check("a run without the fields and one with them: only the second counts",
          s5["joint"]["intervals"] == n_int // 2 and s5["joint"]["packets"] == (PKTS + OTHER) * (n_int // 2)
          and sorted(s5["joint"]["by_hour"]) == [1, 2] and s5["lost_all"]["overall"]["delivered"] == PKTS * (n_int // 2)
          and s5["sizes"]["up"]["data_packets"] == sum(UP) * (n_int // 2), (s5["joint"]["intervals"], sorted(s5["joint"]["by_hour"])))
    # --- three samples of a run without the fields (a status written by another build): their intervals are left out.
    gap = [{k: v for k, v in x.items() if k not in ("joint", "lost_all", "sizes")} if 100 <= i < 103 else x
           for i, x in enumerate(lines)]
    s8, _ = analyze_lines(gap)
    check("samples without the fields inside a run: the 4 intervals touching them are left out, no crash",
          s8["joint"]["intervals"] == n_int - 4 and s8["lost_all"]["intervals"] == n_int - 4 and s8["sizes"]["intervals"] == n_int - 4
          and s8["joint"]["skipped_intervals"] == 0, (s8["joint"]["intervals"], s8["joint"]["skipped_intervals"]))
    # --- an old recording: the three sections say there is no data.
    old = [{k: v for k, v in x.items() if k not in ("joint", "lost_all", "sizes")} for x in lines]
    s6, n6 = analyze_lines(old)
    t6 = rendered(s6, n6)
    check("an old recording: no joint, lost_all or sizes, and the report says so",
          s6["joint"] is None and s6["lost_all"] is None and s6["sizes"] is None
          and all(f"No data: the recording has no `{w}` field" in t6 for w in ("joint", "lost_all", "sizes"))
          and "## Links" in t6 and "## By hour" in t6, t6[-400:])
    # --- fields that are there but not as they should be are as if absent.
    odd = [dict(x, joint={"deadlines_ms": [50], "late": "x", "expect": {}, "packets": 1}, sizes={"edges": [32]}, lost_all=-1) for x in lines]
    s7, _ = analyze_lines(odd)
    check("malformed fields are ignored, not a crash", s7["joint"] is None and s7["sizes"] is None and s7["lost_all"] is None)


# --- the radio section: cell-monitor's RF metrics against the links ---------------------

R0 = 1791590400               # 2026-10-10 00:00:00 UTC, the start of the recording
RN = 7200                     # two hours, a status every second (at x.25 s), but for a gap of 30 s from 900 s
GAP_R = range(900, 930)
RF_FROM = 1800                # wom and entel: RF data from here to 9000, so it starts late and ends after the recording
EXCL = {"cell_foo"}           # a measurement nobody knows

# (start, duration) in seconds after R0
OUT_W = [(405 + 450 * k, 5) for k in range(15)]             # each follows a RAT change at 400 + 450 k
MUTE_W = [(402 + 450 * k, 20) for k in (3, 5, 7, 9)]        # upload mutes right after a RAT change
OUT_E = [(300 + 550 * k, 5) for k in range(13)]             # entel: its RAT changes (1430 + 600 k) never come near
DMUTE_E = [(2100, 30), (3300, 30), (4500, 30)]
OUT_C = [(2000 + 400 * k + d, 5) for k in range(12) for d in (0, 25)]   # claro: pairs of outages 25 s apart; a band change around the even pairs
OUT_SL = [(3000, 5)]
RLINKS = [(0, "eth0.10", "WOM", OUT_W, MUTE_W, []), (1, "eth0.20", "Entel 5G", OUT_E, [], DMUTE_E),
          (2, "eth0.30", "Claro B5", OUT_C, [], []), (3, "starlink", "", OUT_SL, [], [])]


def span_at(events, t):
    return next((s for s, d in events if s <= t < s + d), None)


def radio_status():
    """The engine's status every second for RN seconds, one run."""
    lines = []
    for k in range(RN):
        if k in GAP_R:
            continue
        t = k + 0.25
        links = []
        for i, name, label, out, mute, dmute in RLINKS:
            o, m = span_at(out, t), span_at(mute, t)
            links.append({"id": i, "name": name, "label": label, "state": "stalled" if o is not None else "live",
                          "rtt_ms": 30.0, "last_rx_ms_ago": int((t - o) * 1000) if o is not None else 20,
                          "upload": "muted" if m is not None else "active",
                          "upload_mutes": sum(1 for s, _ in mute if s <= t),
                          "upload_state_ms": int((t - m) * 1000) if m is not None else 60000,
                          "download_muted": span_at(dmute, t) is not None})
        lines.append({"mode": "client", "uptime_ms": 5000 + int(t * 1000), "time_ms": R0 * 1000 + int(t * 1000),
                      "session": "s0", "probe_interval_ms": 100, "download": {"packets": 0}, "links": links})
    return lines


# The RF series: tags as cell-monitor writes them (leg and carrier only when set).
SERIES = {
    "wom": dict(cpe_id="cpe03", modem_id="rm520n_01", leg="wom", carrier="wom"),          # = the label of eth0.10
    "entel": dict(cpe_id="cpe03", modem_id="rm520n_02", leg="eth0.20", carrier="entel"),  # = the NAME of eth0.20
    "claro": dict(cpe_id="cpe05", modem_id="rm502q_05", leg="", carrier="claro"),         # by carrier: "Claro B5"
    "x1": dict(cpe_id="cpe04", modem_id="rm520n_09", leg="isp9", carrier="wom"),          # a leg nobody has
    "x2": dict(cpe_id="cpe04", modem_id="modem1", leg="", carrier=""),                    # no tags at all
    "x3": dict(cpe_id="cpe06", modem_id="modem1", leg="", carrier="bitel"),               # a carrier nobody has
}


def wom_changes():
    """Sample times at which wom's RAT (and band) flips: 400 + 450 k and 30 s later."""
    return sorted(c for k in range(15) for c in (400 + 450 * k, 430 + 450 * k) if c > RF_FROM)


def entel_rat_changes():
    return sorted(c for k in range(13) for c in (1430 + 600 * k, 1450 + 600 * k) if RF_FROM < c < 9000)


def claro_band_changes():
    return sorted(c for k in range(0, 12, 2) for c in (2000 + 400 * k - 10, 2000 + 400 * k + 10))


def lte_lines(sinr, cellid, i, serving=True, nsa=False):
    """What cell-monitor writes for an LTE modem, as (measurement, extra tags, fields)."""
    v = sinr
    pts = [("cell_signal", {}, {"csq": 22, "csq_dbm": -69}),
           ("cell_rxpath", {"rat": "lte"}, {"rsrp1": -95 + i % 3, "rsrp2": -101, "rsrp3": -108, "rsrp_paths": 3}),
           ("cell_rxpath", {"rat": "lte"}, {"rsrq1": -11, "rsrq2": -13, "rsrq3": -15, "rsrq_paths": 3}),
           ("cell_rxpath", {"rat": "lte"}, {"sinr1": v + 1, "sinr2": v, "sinr3": v - 1, "sinr_paths": 3})]
    if serving:
        pts.append(("cell_serving_lte", {"state": "connect", "duplex": "fdd", "band": "b7"},
                    {"mcc": 730, "mnc": 1, "cellid": cellid, "pci": 187, "earfcn": 3100, "ul_bw_code": 5, "dl_bw_code": 5,
                     "tac": 3202, "rsrp": -95 + i % 3, "rsrq": -11, "rssi": -68, "sinr": v, "cqi": 10, "srxlev": 23}))
        pts.append(("cell_ca", {"rat": "lte", "cc_type": "pcc", "cc_seq": 1, "band": "b7"},
                    {"arfcn": 3100, "bw_code": 75, "bw_mhz": 15.0, "cell_state": 1, "pci": 187, "rsrp": -95, "rsrq": -11,
                     "rssi": -68, "sinr": v}))
    if nsa:
        pts.append(("cell_serving_nr", {"state": "connect", "mode": "nsa", "band": "n78"},
                    {"mcc": 730, "mnc": 1, "pci": 321, "rsrp": -88, "sinr": 18, "rsrq": -10, "arfcn": 632448,
                     "nr_bw_code": 1, "scs_code": 1, "scs_khz": 30}))
    return pts


def rf_samples(name):
    """[(second after R0, [(measurement, extra tags, fields)])] of one series, in the engine's clock."""
    out = []
    if name == "wom":
        for i in range(721):
            t = RF_FROM + 10 * i
            nr = any(O <= t < O + 30 for O in (400 + 450 * k for k in range(15)))
            low = any(O - 60 <= t < O for O in (405 + 450 * k for k in range(15)))
            sinr = 2 + i % 3 if low else 12 + i % 5 - 2       # falls in the minute before each outage
            pts = lte_lines(sinr, 4097, i, serving=i % 7 != 3, nsa=nr)
            pts.append(("cell_nwinfo", {"rat": "tdd_nr5g", "band": "nr5g_band_78"} if nr else {"rat": "fdd_lte", "band": "lte_band_7"},
                        {"arfcn": 632448 if nr else 3100, "plmn": "73001"}))
            pts += [("cell_temp", {"sensor": "modem-ambient-usr"}, {"temp_c": 41}), ("cell_data", {}, {"tx_bytes": 1000 * i, "rx_bytes": 9000 * i}),
                    ("cell_net", {"operator": "wom_wom"}, {"act_code": 7, "cereg_stat": 1, "c5greg_stat": 0, "plmn": "73001", "wan_ip": "10.0.0.1"})]
            if i == 5:
                pts.append(("cell_foo", {"x": "y"}, {"z": 1}))
            out.append((t, pts + [("cell_monitor", {}, {"up": 1, "lp_lines": len(pts)})]))
    elif name == "entel":
        for i in range(721):
            t = RF_FROM + 10 * i
            if 4000 <= t <= 4020:
                continue                                          # the CPE rebooted: nothing at all
            cell = 4097 if t < 3000 else 4098 if t < 4030 else 4099 if t < 5000 else 4100   # 4098 -> 4099 across the hole
            wc = any(s <= t < s + 20 for s in (1430 + 600 * k for k in range(13)))
            noserv = t in (2500, 2510, 2520)                      # "No Service": QNWINFO has no quotes, the script skips it
            if wc:
                pts = [("cell_nwinfo", {"rat": "wcdma", "band": "wcdma_band_1"}, {"arfcn": 10700, "plmn": "73001"})]
            elif noserv:
                pts = []
            else:
                pts = lte_lines(9 + (t // 60) % 3, cell, i)
                pts.append(("cell_nwinfo", {"rat": "fdd_lte", "band": "lte_band_7"}, {"arfcn": 3100, "plmn": "73001"}))
            out.append((t, pts + [("cell_monitor", {}, {"up": 1, "lp_lines": len(pts)})]))
    elif name == "claro":
        rng = random.Random(5)
        for i in range(551):
            t = 1500 + 10 * i
            if t in (3000, 3010, 3020):
                out.append((t, [("cell_monitor", {}, {"up": 0})]))   # the modem's port was gone
                continue
            b28 = any(2000 + 400 * k - 10 <= t < 2000 + 400 * k + 10 for k in range(0, 12, 2))
            pts = lte_lines(8 + rng.choice((-2, -1, 0, 1, 2)), 0, i, serving=False)         # no QENG
            pts.append(("cell_nwinfo", {"rat": "fdd_lte", "band": "lte_band_28" if b28 else "lte_band_5"}, {"arfcn": 9410, "plmn": "73001"}))
            out.append((t, pts + [("cell_monitor", {}, {"up": 1, "lp_lines": len(pts)})]))
    else:                                                          # the unmapped: a few samples
        for i in range(8):
            t = 3000 + 10 * i
            out.append((t, [("cell_nwinfo", {"rat": "fdd_lte", "band": "lte_band_3"}, {"arfcn": 1300, "plmn": "73001"}),
                            ("cell_monitor", {}, {"up": 1, "lp_lines": 2})]))
    return out


def rf_points(skew=0, only=None):
    """Every point of every series: (second, measurement, ordered tags, fields). skew: the seconds that
    InfluxDB's clock is ahead of the engine's."""
    out = []
    for name, tg in SERIES.items():
        if only and name not in only:
            continue
        base = {"site": "terreno", "cpe_id": tg["cpe_id"], "host": tg["cpe_id"], "modem_id": tg["modem_id"]}
        if tg["leg"]:
            base["leg"] = tg["leg"]
        if tg["carrier"]:
            base["carrier"] = tg["carrier"]
        for t, pts in rf_samples(name):
            for meas, extra, fields in pts:
                out.append((R0 + t + skew, meas, {**base, **extra}, fields))
    return out


def lp_field(v):
    return f"{v}i" if isinstance(v, int) else f'"{v}"' if isinstance(v, str) else repr(v)


def rf_lp(points, stamp="s"):
    """Line protocol as cell-monitor's emit writes it, with a timestamp (stamp s or ns) or without ("")."""
    out = []
    for t, meas, tags, fields in points:
        ts = {"s": f" {t}", "ns": f" {t}000000000", "": ""}[stamp]
        out.append(f"{meas},{','.join(f'{k}={v}' for k, v in tags.items())} {','.join(f'{k}={lp_field(v)}' for k, v in fields.items())}{ts}")
    return "\n".join(out) + "\n"


def csv_t(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def vtype(v):
    return "long" if isinstance(v, int) else "string" if isinstance(v, str) else "double"


def rf_csv(points, wide=False):
    """Annotated CSV like `influx query --raw` gives: one table per series and field (or, pivoted, per series),
    the tables with the same columns under one header, a blank line and new annotations where they change."""
    start, stop = csv_t(R0 - 100), csv_t(R0 + 20000)
    series = collections.OrderedDict()
    for t, meas, tags, fields in points:
        series.setdefault((meas, tuple(sorted(tags.items()))), []).append((t, fields))
    tables = []                       # (schema, rows)
    for (meas, tags), rows in series.items():
        names = tuple(k for k, _ in tags)
        if wide:
            cols = sorted({k for _, f in rows for k in f})
            kinds = tuple(next(vtype(f[c]) for _, f in rows if c in f) for c in cols)
            tables.append(((meas, names, tuple(cols), kinds), tags, rows))
        else:
            for fld in sorted({k for _, f in rows for k in f}):
                vals = [(t, f[fld]) for t, f in rows if fld in f]
                tables.append(((meas, names, vtype(vals[0][1])), tags, (fld, vals)))
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\r\n")
    last, n = None, 0
    for schema, tags, body in sorted(tables, key=lambda x: str(x[0])):
        names = schema[1]
        if schema != last:
            if last is not None:
                w.writerow([])
            if wide:
                cols, kinds = schema[2], schema[3]
                w.writerow(["#group", "false", "false", "true", "true", "false", "true"] + ["true"] * len(names) + ["false"] * len(cols))
                w.writerow(["#datatype", "string", "long", "dateTime:RFC3339", "dateTime:RFC3339", "dateTime:RFC3339", "string"]
                           + ["string"] * len(names) + list(kinds))
                w.writerow(["#default", "_result"] + [""] * (5 + len(names) + len(cols)))
                w.writerow(["", "result", "table", "_start", "_stop", "_time", "_measurement"] + list(names) + list(cols))
            else:
                w.writerow(["#group", "false", "false", "true", "true", "false", "false", "true", "true"] + ["true"] * len(names))
                w.writerow(["#datatype", "string", "long", "dateTime:RFC3339", "dateTime:RFC3339", "dateTime:RFC3339", schema[2],
                            "string", "string"] + ["string"] * len(names))
                w.writerow(["#default", "_result"] + [""] * (7 + len(names)))
                w.writerow(["", "result", "table", "_start", "_stop", "_time", "_value", "_field", "_measurement"] + list(names))
            last = schema
        tv = [v for _, v in tags]
        if wide:
            for t, f in body:
                w.writerow(["", "", n, start, stop, csv_t(t), schema[0]] + tv + [f.get(c, "") for c in schema[2]])
        else:
            for t, v in body[1]:
                w.writerow(["", "", n, start, stop, csv_t(t), v, body[0], schema[0]] + tv)
        n += 1
    w.writerow([])
    return out.getvalue()


def analyze_raw(lines, utc=0.0):
    """The engine side of the pipeline on status lines: what summarize_radio takes."""
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "current.jsonl"), "w") as fh:
            for x in lines:
                fh.write(json.dumps(x) + "\n")
        notes = []
        segs = fieldrec.segments(fieldrec.read_lines(fieldrec.files_of([d]), notes), notes)
        return fieldrec.analyze(segs, utc)


RF_CACHE = {}


def radio_of(a, text, name="rf.txt", offset=0.0, window=None, maps=(), utc=0.0, precision="auto", binary=None, local=fieldrec.RF_LOCAL_S):
    """summarize_radio on an RF file with this content (a file read once, as summarize_radio does not change it)."""
    key = (hash(text), hash(binary), name, precision)
    if key not in RF_CACHE:
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, name)
            if binary is not None:
                with open(path, "wb") as fh:
                    fh.write(binary)
            else:
                with open(path, "w", newline="") as fh:
                    fh.write(text)
            RF_CACHE[key] = fieldrec.load_rf([path], precision)
    return fieldrec.summarize_radio(a, RF_CACHE[key], fieldrec.RadioOpts(offset, window, maps, local), utc)


def same_radio(x, y):
    """Two summaries equal but for how the data was read."""
    return all(x[k] == y[k] for k in x if k not in ("files", "records", "offset_s"))


def oracle(events, changes, V, N, step=0.5):
    """Hits and chance by brute force: the events in V with a change within N s (before: in [e-N, e),
    after: in [e, e+N]), and the share of V (on a grid) within N s of a change."""
    ev = [e for e in events if any(a <= e <= b for a, b in V)]
    near = lambda e, lo, hi: any(e + lo <= c <= e + hi for c in changes)  # noqa: E731
    num = den = 0
    for a, b in V:
        t = a + step / 2
        while t < b:
            den += 1
            num += any(abs(t - c) <= N for c in changes)
            t += step
    return dict(judged=len(ev), hits=sum(near(e, -N, N) for e in ev), before=sum(any(e - N <= c < e for c in changes) for e in ev),
                after=sum(near(e, 0, N) for e in ev), chance=100 * num / den if den else 0.0)


def radio_checks():
    a = analyze_raw(radio_status())
    pts = rf_points()
    lp = rf_lp(pts)
    base = radio_of(a, lp)
    links = base["links"]
    wom, ent, cla = links["eth0.10 (WOM)"], links["eth0.20 (Entel 5G)"], links["eth0.30 (Claro B5)"]
    # --- the mapping: by label (case blind), by name when the label is not it, by carrier when there is no leg.
    rows = {(r["cpe_id"], r["modem_id"]): r for r in base["series"]}
    check("map: leg = the label (wom / WOM), leg = the name (eth0.20), no leg and the carrier in the label (claro)",
          rows[("cpe03", "rm520n_01")]["link"] == "eth0.10 (WOM)" and rows[("cpe03", "rm520n_01")]["how"] == "label"
          and rows[("cpe03", "rm520n_02")]["link"] == "eth0.20 (Entel 5G)" and rows[("cpe03", "rm520n_02")]["how"] == "name"
          and rows[("cpe05", "rm502q_05")]["link"] == "eth0.30 (Claro B5)" and rows[("cpe05", "rm502q_05")]["how"] == "carrier", rows)
    um = {(r["cpe_id"], r["modem_id"]): r["reason"] for r in base["unmapped"]}
    check("map: three series map to no link and say why (a leg nobody has, no tags, a carrier nobody has)",
          sorted(um) == [("cpe04", "modem1"), ("cpe04", "rm520n_09"), ("cpe06", "modem1")]
          and "isp9" in um[("cpe04", "rm520n_09")] and "neither" in um[("cpe04", "modem1")] and "bitel" in um[("cpe06", "modem1")], um)
    check("map: starlink has no RF data and is listed", base["links_without_rf"] == ["starlink"], base["links_without_rf"])
    check("series: interval, samples and the modem-down samples of claro",
          rows[("cpe05", "rm502q_05")]["interval_s"] == 10.0 and rows[("cpe05", "rm502q_05")]["samples"] == 551
          and rows[("cpe05", "rm502q_05")]["modem_down_samples"] == 3 and rows[("cpe03", "rm520n_02")]["samples"] == 718,
          rows[("cpe05", "rm502q_05")])
    check("notes: the unknown measurement is named, nothing else is wrong",
          any("cell_foo" in n for n in base["notes"]) and len(base["notes"]) == 1, base["notes"])
    # --- the same answer from every format: line protocol (s or ns), annotated CSV (a row per field, pivoted), gzip.
    long_csv, wide_csv = rf_csv(pts), rf_csv(pts, wide=True)
    ns = radio_of(a, rf_lp(pts, "ns"))
    csv_l = radio_of(a, long_csv, "rf.csv")
    csv_w = radio_of(a, wide_csv, "rf.csv")
    gz = radio_of(a, "", "rf.csv.gz", binary=gzip.compress(long_csv.encode()))
    check("formats: line protocol in ns gives what s gives", same_radio(base, ns) and ns["files"][0]["format"] == "line protocol")
    check("formats: annotated CSV, a row per field (tables of 2 schemas or more in one file)",
          same_radio(base, csv_l) and csv_l["files"][0]["format"] == "annotated CSV" and csv_l["files"][0]["records"] > len(pts), csv_l["files"])
    check("formats: annotated CSV, pivoted", same_radio(base, csv_w) and csv_w["files"][0]["records"] == len(pts), csv_w["files"])
    check("formats: gzip, told apart by content", same_radio(base, gz))
    check("formats: more than one block in the CSV", long_csv.count("#datatype") > 5 and long_csv.count("\r\n\r\n") > 5)
    # --- the time shared with the recording: RF data starts after it (wom, entel) or before it ends (claro), and runs past it.
    check("overlap: the RF data runs 00:25:00 to 02:30:10, the recording 00:00:00 to 01:59:59, 5699 s in common",
          base["rf_range"] == ["2026-10-10 00:25:00", "2026-10-10 02:30:10"] and base["recording_range"] == ["2026-10-10 00:00:00", "2026-10-10 01:59:59"]
          and base["overlap_hours"] == round(5699.25 / 3600, 3), (base["rf_range"], base["recording_range"], base["overlap_hours"]))
    check("overlap: the hours of each link are the ones both cover (wom from 1800 s, entel too but for its 10 s hole, claro from 1500 s)",
          wom["hours"] == round(5399.25 / 3600, 3) and ent["hours"] == round(5389.25 / 3600, 3) and cla["hours"] == round(5510 / 3600, 3)
          and wom["recorded_hours"] == round(7168 / 3600, 3) and wom["rf_hours"] == round(7210 / 3600, 3), (wom["hours"], cla["hours"]))
    # --- radio per link
    check("RAT and band shares: wom is 6 % on 5G (30 s in 450), entel is 3 % on wcdma",
          abs(wom["rat_pct"]["nr5g"] - 100 * 330 / 5399.25) < 0.01 and abs(wom["band_pct"]["n78"] - wom["rat_pct"]["nr5g"]) < 1e-4
          and set(wom["rat_pct"]) == {"lte", "nr5g"} and abs(sum(wom["rat_pct"].values()) - 100) < 1e-3
          and abs(ent["rat_pct"]["wcdma"] - 100 * 180 / 5389.25) < 0.01 and abs(ent["rat_pct"]["unknown"] - 100 * 30 / 5389.25) < 0.01
          and ent["band_pct"]["wcdma_band_1"] == ent["rat_pct"]["wcdma"], (wom["rat_pct"], ent["rat_pct"], ent["band_pct"]))
    check("RAT and band shares: claro is on band 28 for 120 s and unknown for the 30 s its modem was down",
          abs(cla["band_pct"]["b28"] - 100 * 120 / 5510) < 0.01 and abs(cla["rat_pct"]["unknown"] - 100 * 30 / 5510) < 0.01
          and abs(cla["band_pct"]["b5"] - 100 * (5510 - 150) / 5510) < 0.01, cla["band_pct"])
    check("NSA share: the NR leg next to LTE is there as long as wom is on 5G", abs(wom["nr_nsa_pct"] - wom["rat_pct"]["nr5g"]) < 1e-4
          and ent["nr_nsa_pct"] == 0, wom["nr_nsa_pct"])
    check("SINR p10/p50/p90 of wom: from the serving cell line, 6 samples in 7 (the others lost their QENG)",
          wom["sources"] == ["serving"] and wom["sinr"]["p50"] == 12.0 and wom["sinr"]["p10"] < 6 and wom["sinr"]["p90"] == 14.0
          and 440 <= wom["sinr"]["n"] <= 480, wom["sinr"])
    check("SINR of claro comes from the rx paths (no QENG): the mean of 3 paths, one below the first",
          cla["sources"] == ["rxpath"] and cla["rsrq"]["p50"] == -13.0 and cla["rsrp"]["p50"] == -101.0, (cla["sources"], cla["rsrq"], cla["rsrp"]))
    # --- changes
    check("changes: wom has 22 RAT and band changes, 0 of cell; the rate is per hour in which one could be seen",
          wom["changes"]["rat"]["n"] == 22 == wom["changes"]["band"]["n"] and wom["changes"]["cell"]["n"] == 0
          and abs(wom["changes"]["rat"]["per_hour"] - 22 / (5399.25 / 3600)) < 0.05, wom["changes"])
    check("changes: entel has 18 RAT changes; its cell changed 3 times but the one across the 40 s hole is not counted",
          ent["changes"]["rat"]["n"] == 18 and ent["changes"]["cell"]["n"] == 2 and ent["changes"]["any"]["n"] == 20, ent["changes"])
    check("changes: entel's 'No Service' samples (3, RAT unknown) are not a change: only the 18 flips to wcdma and back in the recording",
          ent["changes"]["rat"]["n"] == len([c for c in entel_rat_changes() if c < 7199.25]), ent["changes"])
    check("changes: claro has no cell (no QENG) and its 12 band changes, none of RAT through its 3 samples down",
          cla["changes"]["rat"]["n"] == 0 and cla["changes"]["band"]["n"] == 12 and cla["changes"]["cell"]["n"] == 0
          and cla["cell_key"] == [] and wom["cell_key"] == ["cellid"], (cla["changes"], cla["cell_key"]))
    # --- the crossing, against a brute-force count of the same
    Vw = [(1820, 7199.25)]
    Ve = [(1820, 4000), (4050, 7199.25)]
    Vc = [(1520, 6990)]
    cases = (("wom outages", wom, "outage", [s for s, _ in OUT_W], wom_changes(), Vw, "rat"),
             ("wom upload mutes", wom, "upload_mute", [s for s, _ in MUTE_W], wom_changes(), Vw, "rat"),
             ("entel outages", ent, "outage", [s for s, _ in OUT_E], entel_rat_changes(), Ve, "rat"),
             ("entel outages, cell", ent, "outage", [s for s, _ in OUT_E], [3000, 5000], Ve, "cell"),
             ("entel download mutes", ent, "download_mute", [s for s, _ in DMUTE_E], entel_rat_changes(), Ve, "rat"),
             ("claro outages, band", cla, "outage", [s for s, _ in OUT_C], claro_band_changes(), Vc, "band"))
    for what, link, kind, evs, chs, V, ck in cases:
        want, got = oracle(evs, chs, V, 20), link["events"][kind]["changes"][ck]
        check(f"crossing {what}: judged {want['judged']}, hits {want['hits']}, before {want['before']}, after {want['after']}, chance {want['chance']:.1f} %",
              got["n"] == want["judged"] and got["hits"] == want["hits"] and got["before"] == want["before"]
              and got["after"] == want["after"] and abs(got["chance_pct"] - want["chance"]) < 0.1
              and abs(got["expected"] - got["n"] * got["chance_pct"] / 100) < 1e-4, (got, want))
        if what.startswith("claro"):
            check("the p-value is binomial(episodes, chance), not (events, chance): 6 of 12 at 6.6 % is 1e-5, 12 of 24 would be 1e-10",
                  1e-6 < got["p"] < 1e-4, got["p"])
    w_ = wom["events"]["outage"]
    check("crossing: wom's 15 outages, 11 with RF data around them, every one with a RAT change just before (and none after)",
          w_["events"] == 15 and w_["in_rf"] == 11 and w_["changes"]["rat"]["hits"] == 11 and w_["changes"]["rat"]["before"] == 11
          and w_["changes"]["rat"]["after"] == 0 and w_["changes"]["rat"]["reading"] == "more than chance"
          and w_["changes"]["rat"]["p"] < 1e-6 and 14 < w_["changes"]["rat"]["chance_pct"] < 14.5, w_["changes"]["rat"])
    check("crossing: wom has no cell change at all, and the report says so rather than a p-value",
          w_["changes"]["cell"]["reading"] == "no changes" and w_["changes"]["cell"]["p"] is None, w_["changes"]["cell"])
    e_ = ent["events"]["outage"]
    check("crossing: entel's 13 outages, 10 judged (one is in the data hole), none near a change: about chance",
          e_["events"] == 13 and e_["in_rf"] == 10 and e_["changes"]["rat"]["hits"] == 0 and e_["changes"]["rat"]["reading"] == "about chance"
          and e_["changes"]["any"]["reading"] == "about chance" and e_["changes"]["rat"]["p"] == 1.0, e_["changes"]["rat"])
    c_ = cla["events"]["outage"]
    check("crossing: claro, mapped by carrier: its 24 outages come in 12 pairs; a band change within 20 s of 12 of them (50 %) against 6.6 % by chance; "
          "the test is on the 12 episodes (6 with a change), not on 24 events",
          c_["changes"]["band"]["hits"] == 12 and c_["in_rf"] == 24 and c_["events"] == 24 and c_["episodes"] == 12
          and c_["changes"]["band"]["episodes"] == 12 and c_["changes"]["band"]["episode_hits"] == 6 and c_["changes"]["band"]["share_pct"] == 50.0
          and c_["changes"]["band"]["before"] == 12 and c_["changes"]["band"]["after"] == 6 and c_["changes"]["band"]["reading"] == "more than chance"
          and c_["changes"]["rat"]["reading"] == "no changes", c_["changes"]["band"])
    m_ = wom["events"]["upload_mute"]
    check("crossing: 4 upload mutes (one before the RF data starts) are too few to read, though all 3 judged have a RAT change",
          m_["events"] == 4 and m_["in_rf"] == 3 and m_["episodes"] == 3 and m_["changes"]["rat"]["hits"] == 3
          and m_["changes"]["rat"]["reading"] == "too few events", m_)
    check("crossing: download mutes are found too (entel, 3), outages of starlink have no RF", ent["events"]["download_mute"]["events"] == 3
          and "download_mute" not in wom["events"] and "upload_mute" not in ent["events"], list(ent["events"]))
    check("crossing: episodes are the events at least 2N or 60 s apart (here all of them), and the gap is said",
          w_["episodes"] == 11 and w_["episode_gap_s"] == 60 and wom["window_s"] == 20, (w_["episodes"], w_["episode_gap_s"]))
    # --- SINR before the events
    sw, se, sc = (x["events"]["outage"]["sinr_before"] for x in (wom, ent, cla))
    check("SINR before: wom's minute before an outage is 3 dB against 12, every episode under the link's p10, the trend falls",
          sw["level"]["median"] == 3.0 and sw["level"]["baseline_median"] == 12.0 and sw["level"]["under_p10"] == 11
          and sw["level"]["expected_under_p10"] < 2 and sw["level"]["mean_rank_pct"] < 5 and sw["level"]["p"] < 1e-6
          and sw["level"]["reading"] == "lower than usual" and sw["trend"]["median"] < -8 and sw["trend"]["reading"] == "falls", sw)
    check("SINR before: entel's and claro's do not differ from their usual (rank near 50 %)",
          se["level"]["reading"] == "no clear difference" and sc["level"]["reading"] == "no clear difference"
          and 30 < se["level"]["mean_rank_pct"] < 70 and 30 < sc["level"]["mean_rank_pct"] < 70
          and se["trend"]["reading"] == "no clear trend" and sc["trend"]["reading"] == "no clear trend", (se["level"], sc["level"]))
    check("SINR before: three episodes are too few to read, though the numbers are there",
          wom["events"]["upload_mute"]["sinr_before"]["level"]["reading"] == "too few events"
          and wom["events"]["upload_mute"]["sinr_before"]["level"]["n"] == 3, wom["events"]["upload_mute"]["sinr_before"])
    # --- by hour
    bh = wom["by_hour"]
    want0 = [c for c in wom_changes() if c < 3600]
    check("by hour: wom's changes, outages and mutes per hour of the day (UTC)",
          sorted(bh) == ["0", "1"] and bh["0"]["rat"] == len(want0) and bh["1"]["rat"] == 22 - len(want0)
          and bh["0"]["outage"] == sum(1 for s, _ in OUT_W if 1800 <= s < 3600) and bh["1"]["outage"] == sum(1 for s, _ in OUT_W if 3600 <= s < 7199)
          and bh["0"]["upload_mute"] == 2 and bh["1"]["upload_mute"] == 1 and bh["0"]["rf_hours"] == 0.5 and bh["0"]["sinr_p50"] == 12.0, bh)
    a3 = analyze_raw(radio_status(), -3)
    r3 = radio_of(a3, lp, utc=-3)
    check("by hour: --utc-offset moves the hours (21 and 22 for UTC-3)", sorted(r3["links"]["eth0.10 (WOM)"]["by_hour"]) == ["21", "22"], list(r3["links"]["eth0.10 (WOM)"]["by_hour"]))
    # --- the clock: InfluxDB's 30 s ahead breaks the crossing, --rf-offset puts it back
    skew = radio_of(a, rf_lp(rf_points(skew=30)))
    fixed = radio_of(a, rf_lp(rf_points(skew=30)), offset=-30)
    sk = skew["links"]["eth0.10 (WOM)"]["events"]["outage"]["changes"]["rat"]
    check("clock: a 30 s skew pushes the RAT changes outside the window (no hit), --rf-offset -30 gives the same answer as no skew",
          sk["hits"] == 0 and sk["reading"] != "more than chance" and same_radio(base, fixed) and fixed["offset_s"] == -30.0, sk)
    # --- the window
    wide = radio_of(a, lp, window=40)["links"]["eth0.10 (WOM)"]["events"]["outage"]["changes"]["rat"]
    check("--rf-window 40: the window and the chance grow, the hits stay", wide["hits"] == 11 and wide["chance_pct"] > w_["changes"]["rat"]["chance_pct"], wide)
    # --- by hand: a mapping that overlaps another series says so
    hand = radio_of(a, lp, maps=[("isp9", "eth0.30")])
    check("--rf-map: a series that maps by hand is used, and two series over the same time of a link are noted",
          hand["links"]["eth0.30 (Claro B5)"]["series"] == 2 and any("same time" in n for n in hand["notes"])
          and not any(r["leg"] == "isp9" for r in hand["unmapped"]), hand["notes"])
    bad = radio_of(a, lp, maps=[("isp9", "nosuch")])
    check("--rf-map: to a link that does not exist is said, and the series stays unmapped",
          any(r["leg"] == "isp9" and "no link has that label" in r["reason"] for r in bad["unmapped"]), bad["unmapped"])
    # --- data that cannot be placed
    nots = radio_of(a, rf_lp(pts, ""))
    check("line protocol without timestamps (cell-monitor -n) is counted and nothing is made of it",
          nots["links"] == {} and nots["series"] == [] and any("without a timestamp" in n for n in nots["notes"])
          and any("no RF series" in n for n in nots["notes"]) and not any("check the clocks" in n for n in nots["notes"]), nots["notes"])
    none = radio_of(a, rf_lp(rf_points(only=("wom",)), "s").replace("rm520n_01", "rm520n_0x"), maps=[])
    check("one series, one link: the others are listed as without RF data",
          list(none["links"]) == ["eth0.10 (WOM)"] and none["links_without_rf"] == ["eth0.20 (Entel 5G)", "eth0.30 (Claro B5)", "starlink"], none["links_without_rf"])
    far = radio_of(a, rf_lp([(t + 86400, m, tg, f) for t, m, tg, f in rf_points(only=("wom",))]))
    check("a day away from the recording: no overlap, said, and nothing to compute",
          far["links"]["eth0.10 (WOM)"]["hours"] == 0 and far["overlap_hours"] == 0 and any("share no time" in n for n in far["notes"]), far["notes"])
    # --- the report and the JSON
    text = io.StringIO()
    s = fieldrec.summarize(a)
    s["radio"] = base
    with contextlib.redirect_stdout(text):
        fieldrec.report(s, [], 0.0)
    t = text.getvalue()
    for what in ("## Radio", "### RF series and links", "### Radio per link", "### Changes", "### Radio changes and link events",
                 "### SINR before link events", "### Radio by hour (UTC+0)", "| cpe04/rm520n_09 | isp9 | wom | NOT MAPPED |",
                 "- not mapped: cpe06/modem1", "- links without RF data: starlink", "| eth0.10 (WOM) | outage | 11 of 15 | rat | 11 (100 %) | 14.3 % (1.6) | 11 / 0 | 11 | 11 | 1.6 / 1.6 | <0.001 / <0.001 | more than chance (local) |",
                 "| eth0.30 (Claro B5) | outage | 24 of 24 | band | 12 (50 %) | 6.6 % (1.6) | 12 / 6 | 12 | 6 | 0.8 / 0.8 | <0.001 / <0.001 | more than chance (local) |",
                 "lower than usual", "no clear difference", "no changes", "too few events"):
        check(f"report has {what!r}", what in t)
    check("report: the radio goes between the hours and the joint lateness", t.index("## By hour") < t.index("## Radio") < t.index("## Joint lateness"))
    with tempfile.TemporaryDirectory() as d:
        os.mkdir(os.path.join(d, "rec"))
        with open(os.path.join(d, "rec", "current.jsonl"), "w") as fh:
            for x in radio_status():
                fh.write(json.dumps(x) + "\n")
        for nm, data in (("rf.lp", lp), ("rf.csv", wide_csv)):
            with open(os.path.join(d, nm), "w") as fh:
                fh.write(data)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = fieldrec.main([os.path.join(d, "rec"), "--rf", os.path.join(d, "rf.lp"), os.path.join(d, "rf.csv"), "--rf-window", "30",
                                "--json", os.path.join(d, "s.json")])

        def nan(c):
            raise ValueError(c)
        js = json.load(open(os.path.join(d, "s.json")), parse_constant=nan)
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc_missing = fieldrec.main([os.path.join(d, "rec"), "--rf", os.path.join(d, "nosuch.csv")])
            try:
                fieldrec.main(["--rf", os.path.join(d, "rf.lp"), os.path.join(d, "rec")])
                rc_order = 0
            except SystemExit as e:
                rc_order = e.code
    check("main: --rf with two files (the same data twice), --rf-window and --json, strict JSON with the radio in it",
          rc == 0 and "## Radio" in out.getvalue() and js["radio"]["links"]["eth0.10 (WOM)"]["window_s"] == 30
          and [f_["format"] for f_ in js["radio"]["files"]] == ["line protocol", "annotated CSV"] and "joint" in js, list(js))
    check("main: a missing RF file is an error, not a report without radio", rc_missing == 1 and "cannot read the RF data" in err.getvalue(), err.getvalue())
    check("main: the recordings after --rf are taken for RF files, and it says to put them first", rc_order == 2 and "before --rf" in err.getvalue(), (rc_order, err.getvalue()))
    # --- without --rf nothing changes
    plain = io.StringIO()
    with contextlib.redirect_stdout(plain):
        fieldrec.report(fieldrec.summarize(a), [], 0.0)
    check("without --rf there is no radio section", "## Radio" not in plain.getvalue() and "radio" not in fieldrec.summarize(a))
    # --- events from the recording, as the analysis finds them (the engine's own timestamps)
    L = a["L"]
    check("events: outage starts at the last reply, upload mutes by the counter, download mutes by the flag",
          [x // 1000 - R0 for x in L["eth0.10 (WOM)"]["starts"]] == [s for s, _ in OUT_W]
          and [x - R0 * 1000 for x in L["eth0.10 (WOM)"]["mute_starts"]] == [s * 1000 for s, _ in MUTE_W]
          and [x // 1000 - R0 for x in L["eth0.20 (Entel 5G)"]["dmute_starts"]] == [s for s, _ in DMUTE_E]
          and [x - R0 * 1000 for x in L["eth0.10 (WOM)"]["starts"]][:2] == [405000, 855000], [x - R0 * 1000 for x in L["eth0.10 (WOM)"]["starts"]][:3])
    check("events: where the recording watched a link: from the first interval to the last of each run (a gap of 30 s splits it in two)",
          L["eth0.10 (WOM)"]["cover"] == [[R0 * 1000 + 250, R0 * 1000 + 899250], [R0 * 1000 + 930250, R0 * 1000 + 7199250]], L["eth0.10 (WOM)"]["cover"])


def rf_of(text, csv_=False, precision="auto"):
    rf = fieldrec.RF()
    (fieldrec.read_rf_csv if csv_ else lambda fh, r: fieldrec.read_rf_lp(fh, r, precision))(io.StringIO(text, newline=""), rf)
    return rf


# --- the local chance: a link whose outages and RAT changes both come thick in the same hour ------------------

CL_N = 21600                                       # six hours, a status every second
CL_BAD = (7200, 10800)                             # the changes and the outages are all in this hour
CL_SINR_BAD = (5400, 12600)                        # and the SINR is low half an hour before and after it too (so a local baseline is fair)
CL_CHANGES = [7200 + 100 * j for j in range(36)]   # the RAT flips every 100 s: windows of +-20 s cover 40 % of that hour
CL_OUT = [(7305 + 170 * k, 5) for k in range(20)]  # 170 s apart; phase (5 + 70 k) mod 100 runs through all of 5, 15, ..., 95: 4 in 10 near a change


def cluster_status():
    lines = []
    for k in range(CL_N):
        t = k + 0.25
        o = span_at(CL_OUT, t)
        lines.append({"mode": "client", "uptime_ms": 5000 + int(t * 1000), "time_ms": R0 * 1000 + int(t * 1000), "session": "s0",
                      "probe_interval_ms": 100, "download": {"packets": 0},
                      "links": [{"id": 0, "name": "wwan0", "label": "Car", "state": "stalled" if o is not None else "live", "rtt_ms": 30.0,
                                 "last_rx_ms_ago": int((t - o) * 1000) if o is not None else 20, "upload": "active"}]})
    return lines


def cluster_rf():
    rng = random.Random(11)
    base = {"site": "terreno", "cpe_id": "cpe09", "host": "cpe09", "modem_id": "m1", "leg": "car", "carrier": "claro"}
    pts = []
    for i in range(2159):
        t = 10 + 10 * i
        flip = CL_BAD[0] <= t < CL_BAD[1] and ((t - CL_BAD[0]) // 100) % 2 == 0
        sinr = (3 if CL_SINR_BAD[0] <= t < CL_SINR_BAD[1] else 12) + rng.choice((-1, 0, 1))
        for meas, extra, f in lte_lines(sinr, 4097, i):
            pts.append((R0 + t, meas, {**base, **extra}, f))
        pts.append((R0 + t, "cell_nwinfo", {**base, "rat": "wcdma" if flip else "fdd_lte", "band": "wcdma_band_1" if flip else "lte_band_7"},
                    {"arfcn": 1, "plmn": "73001"}))
    return pts


def oracle_q(e, changes, V, N, L, step=0.5):
    """The local chance of an event at e by brute force: the share of V within L of it (on a grid) within N of a change."""
    num = den = 0
    for a, b in V:
        t, hi = max(a, e - L) + step / 2, min(b, e + L)
        while t < hi:
            i = bisect.bisect_left(changes, t)
            den += 1
            num += (i < len(changes) and changes[i] - t <= N) or (i > 0 and t - changes[i - 1] <= N)
            t += step
    return num / den


def pb_oracle(qs, k):
    """P(X >= k) and P(X <= k) from the whole distribution of the number of hits, built by convolution."""
    dist = [1.0]
    for q in qs:
        new = [0.0] * (len(dist) + 1)
        for i, x in enumerate(dist):
            new[i] += x * (1 - q)
            new[i + 1] += x * q
        dist = new
    return sum(dist[k:]), sum(dist[:k + 1])


def oracle_local_level(eps, sinr, L, N, need=2):
    """The local SINR rank of each episode by direct computation. sinr: [(time, value)]. The 'level' of a time t is the mean of the values in
    [t-60, t) (at least need of them); an episode's baseline is the levels at the sample times within L of it but not in [e-60, e+N]."""
    ts = [t for t, _ in sinr]

    def level(t):
        v = [x for u, x in sinr if t - 60 <= u < t]
        return sum(v) / len(v) if len(v) >= need else None
    lev = [(t, level(t)) for t in ts]
    ranks, under, expect = [], 0, 0.0
    for e in eps:
        v = level(e)
        b = sorted(m for t, m in lev if m is not None and e - L <= t <= e + L and not e - 60 <= t <= e + N)
        if v is None or len(b) < 10:
            continue
        less, eq = sum(1 for x in b if x < v), sum(1 for x in b if x == v)
        ranks.append((less + eq / 2) / len(b))
        thr = b[min(len(b) - 1, int(0.1 * len(b)))]
        under += v < thr
        expect += sum(1 for x in b if x < thr) / len(b)
    return ranks, under, expect


def local_checks():
    a = analyze_raw(cluster_status())
    pts = cluster_rf()
    lp = rf_lp(pts)
    glob = radio_of(a, lp, local=0)["links"]["wwan0 (Car)"]
    loc = radio_of(a, lp)["links"]["wwan0 (Car)"]
    g, l_ = glob["events"]["outage"]["changes"]["rat"], loc["events"]["outage"]["changes"]["rat"]
    hits = sum(1 for k in range(20) if (5 + 70 * k) % 100 in (5, 15, 85, 95))
    check("cluster: 20 outages in one hour, 8 within 20 s of a RAT flip (4 in 10), 36 flips in that hour: the global chance is 6.7 %, the hits 40 %",
          hits == 8 and g["n"] == 20 and g["hits"] == 8 and g["episodes"] == 20 and g["episode_hits"] == 8
          and loc["changes"]["rat"]["n"] == 36 and 6 < g["chance_pct"] < 7.5 and g["share_pct"] == 40.0, (g, loc["changes"]["rat"]))
    check("cluster: with the global chance alone (--rf-local 0) it reads 'more than chance' (p ~ 1e-5) and says it is the global one",
          glob["events"]["outage"]["changes"]["rat"]["reading"] == "more than chance" and glob["events"]["outage"]["changes"]["rat"]["basis"] == "global"
          and g["p"] < 1e-4 and glob["events"]["outage"]["changes"]["rat"]["p_local"] is None and glob["local_window_s"] == 0, glob["events"]["outage"]["changes"]["rat"])
    check("cluster: with the local chance it reads 'about chance' (the same hits against what the neighbourhoods give), and says it is the local one",
          l_["reading"] == "about chance" and l_["basis"] == "local" and l_["p"] == g["p"] and 0.1 < l_["p_local"] < 0.9
          and 5 < l_["local_expected"] < 7.5 and l_["local_chance_pct"] > 4 * l_["chance_pct"] and loc["local_window_s"] == 1800, l_)
    # --- q_e by brute force, the expected hits and the tail from the whole distribution
    V = [(30, 21580)]
    eps = [7305 + 170 * k for k in range(20)]
    qs = [oracle_q(e, CL_CHANGES, V, 20, 1800) for e in eps]
    mine = fieldrec.local_shares(eps, CL_CHANGES, 20, [[30, 21580]], 1800)
    check("local chance: the share of the 1800 s around each episode within 20 s of a change, against a grid count (20 episodes, 0.21 to 0.40)",
          max(abs(x - y) for x, y in zip(qs, mine)) < 5e-4 and 0.2 < min(qs) < 0.24 and max(qs) > 0.39, (min(qs), max(qs)))
    pu, pl = pb_oracle(qs, 8)
    check("local chance: the expected hits are the sum of those shares, the p-value the Poisson-binomial tail (a full convolution as the oracle)",
          abs(l_["local_expected"] - sum(qs)) < 0.02 and abs(l_["p_local"] - pu) < 0.02 and l_["p_method"] == "exact" and pl > 0.5,
          (l_["local_expected"], sum(qs), l_["p_local"], pu))
    # --- SINR: low for 3 hours, the outages in the middle 2: against the whole link it is low, against its neighbourhood it is not
    sg, sl = glob["events"]["outage"]["sinr_before"]["level"], loc["events"]["outage"]["sinr_before"]["level"]
    check("SINR cluster: against the whole link the minute before an outage reads 'lower than usual' (rank ~ 17 %: the low 2 h are a third of the link), on the global rank alone",
          sg["reading"] == "lower than usual" and sg["reading_basis"] == "global" and sg["mean_rank_pct"] < 35 and sg["p"] < 1e-5 and "local" not in sg, sg)
    check("SINR cluster: against its own neighbourhood (leaving out [e-60, e+N]) it is 'no clear difference' (rank near 50 %), read on the local rank",
          sl["reading"] == "no clear difference" and sl["reading_basis"] == "local" and sl["local"]["n"] == 20 and 35 < sl["local"]["mean_rank_pct"] < 65
          and sl["local"]["p"] > 0.1 and sl["mean_rank_pct"] == sg["mean_rank_pct"] and sl["p"] == sg["p"], sl)
    rng_ = random.Random(11)             # cluster_rf's own draws, in its order: the sinr of the serving line at each RF sample
    sinr = []
    for i_ in range(2159):
        t_ = 10 + 10 * i_
        sinr.append((t_, (3 if CL_SINR_BAD[0] <= t_ < CL_SINR_BAD[1] else 12) + rng_.choice((-1, 0, 1))))
    ranks, under, expect = oracle_local_level(eps, sinr, 1800, 20)
    check("SINR local baseline: the mean rank, the count under p10 and the expected count are those of a direct computation that leaves out [e-60, e+N] "
          "(20 episodes; with the exclusion lifted they differ)",
          len(ranks) == 20 and abs(sl["local"]["mean_rank_pct"] - 100 * sum(ranks) / 20) < 1e-3 and sl["local"]["under_p10"] == under
          and abs(sl["local"]["expected_under_p10"] - expect) < 1e-3, (sl["local"], 100 * sum(ranks) / 20, under, expect))
    # --- the precursor links are still precursors locally
    base = radio_of(analyze_raw(radio_status()), rf_lp(rf_points()))["links"]
    wo = base["eth0.10 (WOM)"]["events"]["outage"]["changes"]["rat"]
    check("precursor link (wom): 'more than chance' on the local chance too, and the SINR 'lower than usual' on the local rank",
          wo["reading"] == "more than chance" and wo["basis"] == "local" and wo["p_local"] < 1e-6 and wo["local_expected"] < 3
          and base["eth0.10 (WOM)"]["events"]["outage"]["sinr_before"]["level"]["reading"] == "lower than usual"
          and base["eth0.10 (WOM)"]["events"]["outage"]["sinr_before"]["level"]["reading_basis"] == "local", wo)
    ent = base["eth0.20 (Entel 5G)"]["events"]["outage"]["changes"]["rat"]
    check("no-precursor link (entel): 'about chance' locally too", ent["reading"] == "about chance" and ent["basis"] == "local" and ent["p_local"] > 0.5, ent)
    few = base["eth0.10 (WOM)"]["events"]["upload_mute"]["changes"]["rat"]
    check("too few episodes: the reading says so and names no basis, but the local p is there", few["reading"] == "too few events" and few["basis"] == ""
          and few["p_local"] is not None, few)
    # --- the option
    with tempfile.TemporaryDirectory() as d:
        os.mkdir(os.path.join(d, "rec"))
        with open(os.path.join(d, "rec", "current.jsonl"), "w") as fh:
            for x in cluster_status():
                fh.write(json.dumps(x) + "\n")
        with open(os.path.join(d, "rf.lp"), "w") as fh:
            fh.write(lp)
        res = {}
        for opt in ("0", "30", "900"):
            with contextlib.redirect_stdout(io.StringIO()):
                fieldrec.main([os.path.join(d, "rec"), "--rf", os.path.join(d, "rf.lp"), "--rf-local", opt, "--json", os.path.join(d, f"s{opt}.json")])
            res[opt] = json.load(open(os.path.join(d, f"s{opt}.json")))["radio"]["links"]["wwan0 (Car)"]
    check("--rf-local: 0 is global only, a value under 2N is raised to 2N (40 s), 900 s gives a smaller neighbourhood and a different local chance",
          res["0"]["local_window_s"] == 0 and res["30"]["local_window_s"] == 40 and res["900"]["local_window_s"] == 900
          and res["900"]["events"]["outage"]["changes"]["rat"]["local_expected"] != l_["local_expected"]
          and res["0"]["events"]["outage"]["sinr_before"]["level"]["reading_basis"] == "global", {k: v["local_window_s"] for k, v in res.items()})


def local_unit_checks():
    rng = random.Random(4)
    qs = [rng.random() * 0.6 for _ in range(12)]
    ok = all(abs(fieldrec.pb_tail(qs, k, True)[0] - pb_oracle(qs, k)[0]) < 1e-12 and abs(fieldrec.pb_tail(qs, k, False)[0] - pb_oracle(qs, k)[1]) < 1e-12
             for k in range(0, 13))
    check("pb_tail: both tails for every k against the whole distribution (12 trials), the edges 0 and n included", ok)
    check("pb_tail: equal probabilities give the binomial tail",
          abs(fieldrec.pb_tail([0.1] * 10, 1, True)[0] - (1 - 0.9 ** 10)) < 1e-12 and abs(fieldrec.pb_tail([0.2] * 30, 9, True)[0] - fieldrec.binom_tail(9, 30, 30, 0.2)) < 1e-12)
    far, want = fieldrec.pb_tail([0.1] * 100, 60, True)[0], fieldrec.binom_tail(60, 100, 100, 0.1)
    check("pb_tail: an upper tail far out is not 0 nor noise (nothing is subtracted from 1): 60 hits in 100 trials of 0.1 is ~2e-34, as the binomial says",
          1e-35 < far < 1e-33 and abs(far - want) < 1e-9 * want, (far, want))
    check("pb_tail: certain and impossible trials", fieldrec.pb_tail([1.0, 1.0, 0.0], 2, True)[0] == 1.0 and fieldrec.pb_tail([1.0, 1.0, 0.0], 3, True)[0] == 0.0
          and fieldrec.pb_tail([1.0, 1.0, 0.0], 1, False)[0] == 0.0 and fieldrec.pb_tail([], 0, True)[0] == 1.0)
    big = [0.3 + 0.2 * rng.random() for _ in range(60)]
    exact = fieldrec.pb_tail(big, 25, True)
    saved, fieldrec.PB_CELLS = fieldrec.PB_CELLS, 1
    try:
        approx, lower = fieldrec.pb_tail(big, 25, True), fieldrec.pb_tail(big, 25, False)
        degenerate = fieldrec.pb_tail([1.0] * 5 + [0.0] * 5, 5, True), fieldrec.pb_tail([1.0] * 5 + [0.0] * 5, 6, True)
    finally:
        fieldrec.PB_CELLS = saved
    check("pb_tail: above the size it can pay it falls back to a normal approximation, close to the exact tail (60 trials)",
          exact[1] == "exact" and approx[1] == "normal" and abs(approx[0] - exact[0]) < 0.03 and abs(lower[0] - fieldrec.pb_tail(big, 25, False)[0]) < 0.03
          and degenerate == ((1.0, "normal"), (0.0, "normal")), (exact, approx, lower, degenerate))
    check("local_shares: with a neighbourhood far bigger than V every episode has the global chance (80 s of 300); a small one the local (30 s of 110); none where V is empty",
          all(abs(x - 80 / 300) < 1e-9 for x in fieldrec.local_shares([50, 150], [100, 200], 20, [[0, 300]], 10000))
          and abs(fieldrec.local_shares([50], [100, 200], 20, [[0, 300]], 60)[0] - 30 / 110) < 1e-9
          and fieldrec.local_shares([500], [100], 20, [[0, 40], [1000, 1100]], 10) == [None])
    c = fieldrec.IvCum([[0, 10], [20, 30]])
    check("IvCum: lengths inside any span", c.between(5, 25) == 10 and c.between(-5, 100) == 20 and c.between(12, 18) == 0 and c.between(25, 26) == 1)


# --- --export: tidy CSVs for R and pandas ----------------------------------------------------

# The columns, written out: a column renamed, moved or dropped fails here before it breaks somebody's script.
H_SAMPLES = [
    'segment', 'session', 'sample', 'time_utc', 'time_epoch', 'uptime_ms', 'clock_synced', 'interval_ms', 'link',
    'label', 'link_id', 'state', 'counted', 'out', 'last_rx_ms_ago', 'rtt_ms', 'rtt_dev_ms', 'rtt_step_ms',
    'rtt_behind_best_ms', 'upload', 'upload_muted', 'upload_mutes', 'upload_mutes_delta', 'upload_state_ms',
    'upload_behind_ms', 'download_muted', 'tx_packets_delta', 'server_rx_delta', 'rx_first_delta', 'rx_copies_delta',
    'loss_down']
H_EVENTS = ['kind', 'link', 'label', 'segment', 'start_utc', 'start_epoch', 'end_utc', 'end_epoch', 'duration_s', 'ends']
H_JOINT = ['segment', 'time_utc', 'time_epoch', 'uptime_ms', 'clock_synced', 'interval_ms', 'deadline_ms', 'mask', 'set', 'expect', 'late']
H_TUNNEL = ['segment', 'time_utc', 'time_epoch', 'uptime_ms', 'clock_synced', 'interval_ms', 'down_packets_delta', 'lost_all_delta']
H_RF = [
    'time_utc', 'time_epoch', 'rf_time_epoch', 'link', 'label', 'cpe_id', 'modem_id', 'leg', 'carrier', 'recorded',
    'modem_up', 'rat', 'band', 'cell_kind', 'cell', 'nsa', 'sinr', 'rsrq', 'rsrp', 'source']
H_RF_CHANGES = ['time_utc', 'time_epoch', 'link', 'label', 'cpe_id', 'modem_id', 'kind', 'from', 'to', 'recorded']
EDGE_NAMES = ['le_32', 'le_96', 'le_128', 'le_192', 'le_256', 'le_384', 'le_512', 'le_768', 'le_1024', 'le_1280', 'le_1440', 'gt_1440']
# per direction, in this order: packets per bucket, bytes per bucket, handshakes
H_TUNNEL_SIZES = H_TUNNEL + [c for w in ("up", "down") for c in
                             [f"{w}_{k}_{e}" for k in ("pkts", "bytes") for e in EDGE_NAMES] + [f"{w}_hs_{h}" for h in ("init", "response", "cookie", "other")]]


def read_table(text):
    rows = list(csv.reader(io.StringIO(text, newline="")))
    return rows[0], [dict(zip(rows[0], r)) for r in rows[1:]], rows[1:]


def export_run(src, extra=(), gz=False):
    """main() with --export on a recording (a directory): (exit code, the report, {file name: text}, the JSON summary)."""
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "ex")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = fieldrec.main([src, "--export", out, "--json", os.path.join(d, "s.json")] + list(extra) + (["--export-gzip"] if gz else []))
        with open(os.path.join(d, "s.json")) as fh:
            summary = json.load(fh)
        files = {}
        for name in sorted(os.listdir(out)):
            with open(os.path.join(out, name), "rb") as fh:
                raw = fh.read()
            files[name] = gzip.decompress(raw).decode() if name.endswith(".gz") else raw.decode()
        return rc, buf.getvalue(), files, summary


def write_dir(d, lines):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "current.jsonl"), "w") as fh:
        for x in lines:
            fh.write(json.dumps(x) + "\n")
    return d


def sets_of(d):
    """{mask: (expect, late)} per deadline of a joint.csv, summed."""
    out = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for r in d:
        e = out[int(r["deadline_ms"])][int(r["mask"])]
        e[0] += int(r["expect"])
        e[1] += int(r["late"])
    return out


def export_checks():
    # --- the first recording: a restart, a gap, the clock before NTP, an engine without time_ms, a cut file; no instruments
    with tempfile.TemporaryDirectory() as d:
        make(d)
        notes = []
        segs = fieldrec.segments(fieldrec.read_lines(fieldrec.files_of([d]), notes), notes)
        rc, rep, fl, summ = export_run(d)
        plain = io.StringIO()
        with contextlib.redirect_stdout(plain):
            fieldrec.main([d])
    check("export: exits 0, writes the four files and README.txt (no --rf: no rf files); the report is the same with and without it",
          rc == 0 and sorted(fl) == ["README.txt", "events.csv", "joint.csv", "samples.csv", "tunnel.csv"] and rep == plain.getvalue(), sorted(fl))
    hs, sm, smr = read_table(fl["samples.csv"])
    he, ev, _ = read_table(fl["events.csv"])
    hj, jt, _ = read_table(fl["joint.csv"])
    ht, tn, _ = read_table(fl["tunnel.csv"])
    check("export: the headers are as written down (an old recording has no sizes: the tunnel has no size columns)",
          hs == H_SAMPLES and he == H_EVENTS and hj == H_JOINT and ht == H_TUNNEL, (hs, he, hj, ht))
    nlinks = sum(len(x.links) for seg in segs for x in seg)
    check("export: a samples row per link per status sample (the first of each of 3 runs included), a tunnel row per interval, a joint file with no rows",
          len(sm) == nlinks and len(tn) == sum(len(seg) - 1 for seg in segs) and jt == [] and {r["segment"] for r in sm} == {"0", "1", "2"}
          and sum(r["sample"] == "0" for r in sm) == 3 * 4 and all(r["counted"] == "0" and r["interval_ms"] == "" for r in sm if r["sample"] == "0"),
          (len(sm), nlinks))
    check("export: the samples before NTP are marked (clock_synced 0), the others not; a time_utc is its time_epoch",
          sum(r["clock_synced"] == "0" for r in sm) == 4 * sum(not x.synced for seg in segs for x in seg) > 0
          and all(time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(float(r["time_epoch"]))) == r["time_utc"][:19] and r["time_utc"].endswith("Z") for r in sm[::97]),
          sum(r["clock_synced"] == "0" for r in sm))
    check("export: a field the status lacks is an empty cell (upload_state_ms), and so is what needs 30 RTTs or the previous sample (rtt_dev_ms first)",
          all(r["upload_state_ms"] == "" for r in sm) and all(r["rtt_dev_ms"] == "" for r in sm[:100]) and any(r["rtt_dev_ms"] != "" for r in sm)
          and all(r["tx_packets_delta"] == "" for r in sm if r["sample"] == "0"), None)
    ok = True
    for n in ("a", "b", "c", "sl"):
        ls = [float(r["loss_down"]) for r in sm if r["link"] == n and r["loss_down"] != ""]
        want = summ["links"][n]["loss_down_pct"]
        ok &= (not ls and want is None) or (len(ls) > 100 and abs(100 * sum(ls) / len(ls) - want) < 0.01)
    check("export: loss_down per interval (fraction) averages to the report's download loss of each link, and is empty where the report does not judge it (the first sample)",
          ok and any(float(r["loss_down"]) > 0 for r in sm if r["loss_down"] != "") and all(r["loss_down"] == "" for r in sm if r["sample"] == "0"), None)
    check("export: rtt_step_ms is there on the samples before NTP too (it needs no clock), empty at the first sample of a run",
          any(r["rtt_step_ms"] != "" for r in sm if r["clock_synced"] == "0" and r["counted"] == "1") and all(r["rtt_step_ms"] == "" for r in sm if r["sample"] == "0"))
    check("export: the joint and lost_all of an old recording are empty cells in the tunnel", all(r["lost_all_delta"] == "" for r in tn) and tn and all(r["down_packets_delta"] != "" for r in tn))
    n_ev = collections.Counter(r["kind"] for r in ev)
    gap = next(r for r in ev if r["kind"] == "gap")
    rst = next(r for r in ev if r["kind"] == "restart")
    check("export events: the one restart and the one gap of the recording, with their durations (1 s between runs, 61 s)",
          n_ev["restart"] == 1 == n_ev["gap"] and float(gap["duration_s"]) == 61.0 and float(rst["duration_s"]) == 1.0 and gap["link"] == "" and gap["ends"] == "", (gap, rst))
    ok = True
    for n, v in summ["links"].items():
        mine = [r for r in ev if r["kind"] == "outage" and r["link"] == n]
        durs = [float(r["duration_s"]) for r in mine]
        ok &= len(mine) == v["outages"] and abs(max(durs, default=0) - (v["outage_s"]["max"] or 0)) < 1e-3 \
            and abs((fieldrec.pct(durs, 50) or 0) - (v["outage_s"]["p50"] or 0)) < 1e-3
    check("export events: the outages of each link are the report's: their number, their median and longest duration", ok and n_ev["outage"] == sum(v["outages"] for v in summ["links"].values()))
    check("export events: sorted by segment and start", [(int(r["segment"]), r["start_epoch"]) for r in ev] == sorted((int(r["segment"]), r["start_epoch"]) for r in ev) or
          all((int(x["segment"]), x["start_epoch"] or "0") <= (int(y["segment"]), y["start_epoch"] or "0") for x, y in zip(ev, ev[1:])))

    # --- the radio recording: known outages and mutes, a gap, and the RF data
    a_lines = radio_status()
    pts = rf_points()
    lp = rf_lp(pts)
    with tempfile.TemporaryDirectory() as d:
        write_dir(os.path.join(d, "rec"), a_lines)
        with open(os.path.join(d, "rf.lp"), "w") as fh:
            fh.write(lp)
        rc, rep_rf, fr, summ_rf = export_run(os.path.join(d, "rec"), ["--rf", os.path.join(d, "rf.lp")])
    check("export --rf: rf.csv and rf_changes.csv are there, with the headers written down",
          sorted(fr) == ["README.txt", "events.csv", "joint.csv", "rf.csv", "rf_changes.csv", "samples.csv", "tunnel.csv"]
          and read_table(fr["rf.csv"])[0] == H_RF and read_table(fr["rf_changes.csv"])[0] == H_RF_CHANGES, sorted(fr))
    _, ev, _ = read_table(fr["events.csv"])
    by = lambda link, kind: sorted((float(r["start_epoch"]) for r in ev if r["link"] == link and r["kind"] == kind))   # noqa: E731
    check("export events: WOM's 15 outages start where the engine says (to the ms), its 4 upload mutes at the stamped start, "
          "Entel's 3 download mutes at the sample where they were seen",
          by("eth0.10", "outage") == [R0 + s_ for s_, _ in OUT_W] and by("eth0.10", "upload_mute") == [R0 + s_ for s_, _ in MUTE_W]
          and by("eth0.20", "download_mute") == [R0 + s_ + 0.25 for s_, _ in DMUTE_E] and by("eth0.20", "outage") == [R0 + s_ for s_, _ in OUT_E]
          and by("eth0.30", "outage") == [R0 + s_ for s_, _ in OUT_C], by("eth0.10", "upload_mute"))
    shown = {"eth0.10": "eth0.10 (WOM)", "eth0.20": "eth0.20 (Entel 5G)", "eth0.30": "eth0.30 (Claro B5)", "starlink": "starlink"}
    check("export events: each link's outage and mute rows are the report's counts (outages, mutes)",
          all(sum(r["kind"] == "outage" and r["link"] == v for r in ev) == summ_rf["links"][shown[v]]["outages"]
              and sum(r["kind"] == "upload_mute" and r["link"] == v for r in ev) == summ_rf["links"][shown[v]]["mutes"] for v in shown), None)
    um = [r for r in ev if r["kind"] == "upload_mute"][0]
    dm = [r for r in ev if r["kind"] == "download_mute"][0]
    og = [r for r in ev if r["kind"] == "outage"][0]
    check("export events: durations by the same rule as the report's (halfway between the last sample out and the first back): 4.75 s, 19.75 s, 29.5 s; ends 'recovered'",
          float(og["duration_s"]) == 4.75 and float(um["duration_s"]) == 19.75 and float(dm["duration_s"]) == 29.5
          and {r["ends"] for r in ev if r["kind"] != "gap"} == {"recovered"}, (og, um, dm))
    gp = [r for r in ev if r["kind"] == "gap"]
    check("export events: the 30 s hole in the recording is a gap of 31 s between the segments 0 and 1", len(gp) == 1 and gp[0]["segment"] == "1" and float(gp[0]["duration_s"]) == 31.0, gp)
    _, rf_rows, _ = read_table(fr["rf.csv"])
    wom = [r for r in rf_rows if r["link"] == "eth0.10"]
    cla = [r for r in rf_rows if r["link"] == "eth0.30"]
    check("export rf.csv: a row per RF sample of the three series that map (721 + 718 + 551), none of the unmapped; the first row as it is read",
          len(rf_rows) == 721 + 718 + 551 and {r["link"] for r in rf_rows} == {"eth0.10", "eth0.20", "eth0.30"} and
          {k: wom[0][k] for k in ("time_utc", "rf_time_epoch", "label", "cpe_id", "modem_id", "leg", "carrier", "recorded", "modem_up", "rat", "band",
                                  "cell_kind", "cell", "nsa", "sinr", "rsrq", "rsrp", "source")}
          == dict(time_utc="2026-10-10T00:30:00.000Z", rf_time_epoch=f"{R0 + 1800}.000", label="WOM", cpe_id="cpe03", modem_id="rm520n_01", leg="wom",
                  carrier="wom", recorded="1", modem_up="1", rat="lte", band="b7", cell_kind="cellid", cell="4097", nsa="0", sinr="10.0", rsrq="-11.0",
                  rsrp="-95.0", source="cell_serving_lte"), wom[0])
    check("export rf.csv: recorded is 0 after the recording ended (181 samples of wom), the modem that was down is modem_up 0 with the rest empty, "
          "the source of claro is its rx paths and its cell empty",
          sum(r["recorded"] == "0" for r in wom) == 181 and [r["modem_up"] for r in cla if r["modem_up"] == "0"] == ["0"] * 3
          and all(r["rat"] == "" and r["sinr"] == "" and r["source"] == "" for r in cla if r["modem_up"] == "0")
          and {r["source"] for r in cla if r["modem_up"] == "1"} == {"cell_rxpath"} and {r["cell"] for r in cla} == {""} and {r["cell_kind"] for r in cla} == {""},
          [r["modem_up"] for r in cla][:5])
    _, chg, _ = read_table(fr["rf_changes.csv"])
    cnt = collections.Counter((r["link"], r["kind"], r["recorded"]) for r in chg)
    check("export rf_changes.csv: the changes of the report (wom 22 rat and 22 band; entel 18 rat, 2 cell of its 3 -- not the one across the data hole; claro 12 band) "
          "with recorded 1, and entel's 6 RAT changes after the recording with recorded 0",
          cnt[("eth0.10", "rat", "1")] == 22 and cnt[("eth0.10", "band", "1")] == 22 and cnt[("eth0.20", "rat", "1")] == 18 and cnt[("eth0.20", "rat", "0")] == 6
          and cnt[("eth0.20", "cell", "1")] == 2 and cnt[("eth0.30", "band", "1")] == 12 and ("eth0.10", "cell", "1") not in cnt, cnt)
    first = next(r for r in chg if r["link"] == "eth0.10" and r["kind"] == "rat")
    cell = [(r["from"], r["to"]) for r in chg if r["link"] == "eth0.20" and r["kind"] == "cell"]
    bnd = next(r for r in chg if r["link"] == "eth0.10" and r["kind"] == "band")
    check("export rf_changes.csv: from and to (lte to nr5g, b7 to n78, the cell id 4097 to 4098 and 4099 to 4100)",
          (first["from"], first["to"]) == ("lte", "nr5g") and (bnd["from"], bnd["to"]) == ("b7", "n78") and cell == [("4097", "4098"), ("4099", "4100")], cell)
    # the offset: time_epoch is the exported time plus it
    with tempfile.TemporaryDirectory() as d:
        write_dir(os.path.join(d, "rec"), a_lines)
        with open(os.path.join(d, "rf.lp"), "w") as fh:
            fh.write(rf_lp(rf_points(skew=30)))
        rc2, _, fo, _ = export_run(os.path.join(d, "rec"), ["--rf", os.path.join(d, "rf.lp"), "--rf-offset", "-30"])
    _, off_rows, _ = read_table(fo["rf.csv"])
    check("export --rf-offset: time_epoch is the exported time plus the offset (rf_time_epoch keeps it), and it is the same table as with no skew",
          all(float(r["time_epoch"]) == float(r["rf_time_epoch"]) - 30 for r in off_rows[::31])
          and [r["time_epoch"] for r in off_rows] == [r["time_epoch"] for r in rf_rows], off_rows[0])
    # README.txt: every file, every column, the clocks
    rd = fr["README.txt"]
    cols = {"samples.csv": H_SAMPLES, "events.csv": H_EVENTS, "joint.csv": H_JOINT, "rf.csv": H_RF, "rf_changes.csv": H_RF_CHANGES}
    check("README.txt: each file with its rows and each of its columns; the row counts are the files'",
          all(f"{n}: " in rd and all(re.search(rf"^  {re.escape(c)} +\[", rd, re.M) for c in cs) for n, cs in cols.items())
          and f"{len(wom) + len(cla) + len([r for r in rf_rows if r['link'] == 'eth0.20'])} rows. Columns:" in rd
          and f"{len(read_table(fr['samples.csv'])[1])} rows. Columns:" in rd, rd[:300])
    check("README.txt: says how the times relate (the engine's time_ms, InfluxDB's clock at the POST, the offset), the command and the commit",
          "time_ms" in rd and "InfluxDB's clock" in rd and "--rf-offset" in rd and "(here 0 s)" in rd and "Command: " in rd and "--export" in rd
          and re.search(r"commit (unknown|[0-9a-f]{7,})", rd) and "uptime_ms" in rd, rd[:400])
    check("README.txt: with --rf-offset it says which", "(here -30 s)" in fo["README.txt"])

    # --- the bonding recording: joint, lost_all, sizes, a restart and (second run) a counter that starts over within a run
    for soft in (1000,):                    # the interval where the counters start over is skipped by the report, and is empty cells here
        lines = bond_lines(soft_reset=soft)
        with tempfile.TemporaryDirectory() as d:
            write_dir(d, lines)
            rc, _, fb, s = export_run(d)
        _, jr, _ = read_table(fb["joint.csv"])
        ht, tr, _ = read_table(fb["tunnel.csv"])
        tag = "" if soft is None else " with a counter that starts over (that interval is no row in the report, and is empty here)"
        check("export tunnel.csv: the size columns, one per bucket edge, for packets and bytes and the handshakes, up and down, in that order" + tag, ht == H_TUNNEL_SIZES, ht[:12])
        names = {(r["link"], r["label"]) for r in read_table(fb["samples.csv"])[1]}
        disp = {(f"{n} ({lb})" if lb else n): n for n, lb in names}
        want = s["joint"]["overall"]["deadlines"]
        per = sets_of(jr)
        p50 = per[50]
        ok = len(per) == 3
        full = 0
        for di, v in enumerate(want):
            dl = per[v["deadline_ms"]]
            for sub in v["subsets"]:
                m_s = sum(1 << IDS[disp[x]] for x in sub["set"])
                ok &= sum(e for m, (e, _) in dl.items() if m & m_s == m_s) == sub["packets"] and sum(la for m, (_, la) in dl.items() if m & m_s == m_s) == sub["late"]
            full = max(full, max(dl))
        check("export joint.csv: the packets sent on every set of links and the late ones, summed from the rows over the masks that contain the set, "
              "are the report's for all 26 sets at 3 deadlines" + tag, ok and len(want[0]["subsets"]) == 26, None)
        edge = [r for r in jr if r["deadline_ms"] == "50" and r["mask"] == str(bit("a", "b", "c", "d", "e"))]
        check("export joint.csv: the mask is the engine's (link ids 2, 5, 0, 7, 3), its set the names in id order; expect is the same at every deadline; mask 0 has no set"
              + tag, edge and edge[0]["set"] == "c+a+e+b+d" and int(edge[0]["mask"]) == bit("a", "b", "c", "d", "e")
              and p50[0][1] > 0 and all(p50[m][0] == per[150][m][0] for m in p50)
              and all(r["set"] == "" for r in jr if r["mask"] == "0"), edge[:1])
        lost, sz = s["lost_all"], s["sizes"]
        got_l = sum(int(r["lost_all_delta"]) for r in tr if r["lost_all_delta"] != "")
        got_d = sum(int(r["down_packets_delta"]) for r in tr if r["lost_all_delta"] != "")
        check("export tunnel.csv: lost_all and the packets delivered, summed, are the report's" + tag, got_l == lost["overall"]["lost"] and got_d == lost["overall"]["delivered"], (got_l, got_d))
        okz = True
        for w in ("up", "down"):
            for i, b_ in enumerate(sz[w]["buckets"]):
                nm = EDGE_NAMES[i]
                okz &= sum(int(r[f"{w}_pkts_{nm}"]) for r in tr if r[f"{w}_pkts_{nm}"] != "") == b_["packets"] \
                    and sum(int(r[f"{w}_bytes_{nm}"]) for r in tr if r[f"{w}_bytes_{nm}"] != "") == b_["bytes"]
            for h in ("init", "response", "cookie", "other"):
                okz &= sum(int(r[f"{w}_hs_{h}"]) for r in tr if r[f"{w}_hs_{h}"] != "") == sz[w]["handshakes"][h]
        check("export tunnel.csv: the WireGuard sizes per bucket (packets and bytes) and the handshakes, summed, are the report's" + tag, okz, None)
        if soft:
            skipped = [r for r in tr if r["lost_all_delta"] == "" and r["up_pkts_le_32"] == ""]
            check("export tunnel.csv: the interval where the counters started over is an empty lost_all and empty sizes, still with its packets delta (and no joint rows)",
                  len(skipped) == 1 and skipped[0]["down_packets_delta"] != "" and not [r for r in jr if (r["segment"], r["uptime_ms"]) == (skipped[0]["segment"], skipped[0]["uptime_ms"])], skipped[:1])
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            fieldrec.main(["x", "--export-gzip"])
            rc_g = 0
        except SystemExit as e:
            rc_g = e.code
    check("export --export-gzip without --export is an error", rc_g == 2)
    mute_edges()


def mute_edges():
    """The ways a mute can end or not be seen, in a few made-up samples of one link."""
    seq = [  # state, upload, mutes, upload_state_ms, download_muted, last_rx_ms_ago
        ("live", "active", 0, 60000, False, 20), ("live", "active", 2, 60000, False, 20),         # t1: two mutes began and ended between samples
        ("live", "muted", 3, 400, False, 20), ("live", "muted", 3, 1400, True, 20),                # t2: muted, 400 ms ago; t3: and the server mutes it for download
        ("live", "muted", 4, 300, True, 20), ("live", "active", 4, 60000, False, 20),              # t4: unmuted and muted again; t5: back
        ("live", "muted", 5, 200, False, 20), ("paused", "muted", 5, 1200, False, 20),             # t6: muted; t7: paused
        ("live", "active", 5, 60000, False, 20), ("stalled", "muted", 6, 100, False, 500),         # t8: back; t9: muted, and out for good
        ("stalled", "muted", 6, 1100, False, 1500)]                                                 # t10: the run ends here
    lines = [{"mode": "client", "uptime_ms": 5000 + 1000 * k, "time_ms": R0 * 1000 + 1000 * k, "session": "s", "probe_interval_ms": 100, "download": {"packets": 0},
              "links": [{"id": 0, "name": "x", "label": "", "state": st, "rtt_ms": 30.0, "last_rx_ms_ago": ago, "upload": up, "upload_mutes": mu,
                         "upload_state_ms": ms, "download_muted": dm}]} for k, (st, up, mu, ms, dm, ago) in enumerate(seq)]
    with tempfile.TemporaryDirectory() as d:
        write_dir(d, lines)
        _, _, fl, summ = export_run(d)
        _, _, fz, _ = export_run(d, gz=True)
    with tempfile.TemporaryDirectory() as d2:
        write_dir(os.path.join(d2, "rec"), lines)
        for gz_ in (True, False):
            with contextlib.redirect_stdout(io.StringIO()):
                fieldrec.main([os.path.join(d2, "rec"), "--export", os.path.join(d2, "out")] + (["--export-gzip"] if gz_ else []))
        left = sorted(os.listdir(os.path.join(d2, "out")))
    check("export: a plain export over a gzipped one leaves no .csv.gz of the earlier (and the other way round would be as clean)",
          left == ["README.txt", "events.csv", "joint.csv", "samples.csv", "tunnel.csv"], left)
    check("export --export-gzip: .csv.gz files with the same rows as the plain ones (README.txt stays plain and says so)",
          sorted(fz) == sorted(x + ".gz" if x.endswith(".csv") else x for x in fl) and all(fz[n + ".gz"] == fl[n] for n in fl if n.endswith(".csv"))
          and "gzipped (.csv.gz)" in fz["README.txt"] and "samples.csv.gz: " in fz["README.txt"] and len(fl["samples.csv"]) > 500, sorted(fz))
    _, ev, _ = read_table(fl["events.csv"])
    um = [(r["ends"], r["duration_s"], round(float(r["start_epoch"]) - R0, 3)) for r in ev if r["kind"] == "upload_mute"]
    check("export events, upload mutes: one row per mute the report counts (6): two that began and ended between samples (no end), one cut by a new mute "
          "(remuted), one recovered (halfway), one cut by a pause, and the one the run ended in (closed at the last sample muted)",
          um == [("within_interval", "", 1.0), ("within_interval", "", 1.0), ("remuted", "2.1", 1.6), ("recovered", "0.8", 3.7), ("paused", "0.2", 5.8),
                 ("segment_end", "1.1", 8.9)] and summ["links"]["x"]["mutes"] == 6, (um, summ["links"]["x"]["mutes"]))
    dm = [(r["ends"], r["duration_s"], round(float(r["start_epoch"]) - R0, 3)) for r in ev if r["kind"] == "download_mute"]
    check("export events, download mutes: from the sample where the flag was first seen to halfway to the first one back", dm == [("recovered", "1.5", 3.0)], dm)
    og = [(r["ends"], r["duration_s"], round(float(r["start_epoch"]) - R0, 3)) for r in ev if r["kind"] == "outage"]
    check("export events: an outage still going when the run ends ends at its last sample out (segment_end), and starts at the last reply", og == [("segment_end", "1.5", 8.5)], og)
    _, sm, _ = read_table(fl["samples.csv"])
    pz = [r for r in sm if r["state"] == "paused"]
    check("export samples: a paused link has a row (counted 0, out empty), and its counters still there",
          len(pz) == 1 and pz[0]["counted"] == "0" and pz[0]["out"] == "" and pz[0]["upload_mutes"] == "5", pz)


# What cell-monitor.sh really wrote (2026-10-11, `cell-monitor -n` under dash and busybox ash, against its
# modem-sim.py; MODEM1_LEG="WOM 5G", MODEM1_CARRIER=Wom, MODEM1_ID="RM520N 01"), to pin the format.
REAL_LP = [
    'cell_signal,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom csq=22i,csq_dbm=-69i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=lte rsrp1=-95i,rsrp2=-101i,rsrp3=-108i,rsrp_paths=3i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=nr5g rsrp1=-88i,rsrp2=-92i,rsrp_paths=2i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=lte rsrq1=-11i,rsrq2=-13i,rsrq3=-15i,rsrq_paths=3i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=nr5g rsrq1=-10i,rsrq2=-12i,rsrq_paths=2i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=lte sinr1=14i,sinr2=9i,sinr3=3i,sinr_paths=3i',
    'cell_rxpath,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=nr5g sinr1=18i,sinr2=15i,sinr_paths=2i',
    'cell_nwinfo,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=fdd_lte,band=lte_band_7 arfcn=3100i,plmn="73001"',
    'cell_serving_lte,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,state=noconn,duplex=fdd,band=b7 mcc=730i,mnc=01i,cellid=209847201i,pci=187i,earfcn=3100i,ul_bw_code=5i,dl_bw_code=5i,tac=3202i,rsrp=-95i,rsrq=-11i,rssi=-68i,sinr=14i,cqi=10i,srxlev=23i',
    'cell_serving_nr,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,state=noconn,mode=nsa,band=n78 mcc=730i,mnc=01i,pci=321i,rsrp=-88i,sinr=18i,rsrq=-10i,arfcn=632448i,nr_bw_code=1i,scs_code=1i,scs_khz=30i',
    'cell_ca,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=lte,cc_type=pcc,cc_seq=1,band=b7 arfcn=3100i,bw_code=75i,bw_mhz=15.0,cell_state=1i,pci=187i,rsrp=-95i,rsrq=-11i,rssi=-68i,sinr=14i',
    'cell_ca,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,rat=lte,cc_type=scc,cc_seq=2,band=b3 arfcn=1300i,bw_code=100i,bw_mhz=20.0,cell_state=1i,pci=187i,rsrp=-99i,rsrq=-12i,rssi=-72i,sinr=11i',
    'cell_temp,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,sensor=modem-ambient-usr temp_c=41i',
    'cell_temp,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,sensor=cpuss-0-usr temp_c=46i',
    'cell_data,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom tx_bytes=184320117i,rx_bytes=982340551i',
    'cell_net,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom,operator=entel_pcs act_code=7i,cereg_stat=1i,c5greg_stat=1i,plmn="73001",wan_ip="10.144.22.87"',
    'cell_monitor,site=terreno,cpe_id=cpe03,host=cpe03,modem_id=rm520n_01,leg=wom_5g,carrier=wom up=1i,lp_lines=16i'
]


def real_format_checks():
    rf = fieldrec.RF()
    fieldrec.read_rf_lp(io.StringIO("".join(f"{x} 1791592200\n" for x in REAL_LP)), rf, "auto")
    (key, ser), = rf.series.items()
    r = fieldrec.derive(ser.samples[1791592200])
    check("real cell-monitor output: all 17 lines read, none bad, one series with the sanitized tags (leg wom_5g, modem rm520n_01)",
          rf.records == 17 and rf.bad == 0 and key == ("cpe03", "rm520n_01", "wom_5g", "wom") and rf.unknown == {}, (rf.records, rf.bad, key))
    check("real cell-monitor output: LTE on band 7 with an NR leg (NSA), the cell id in decimal, SINR 14 from the serving line, the rx paths' mean and the primary carrier",
          r["rat"] == "lte" and r["band"] == "b7" and r["nsa"] and r["up"] == 1 and r["cells"] == {"cellid": 209847201, "pci": (187, 3100.0), "pcc": (187, 3100.0)}
          and r["m"]["serving"] == (-95.0, -11.0, 14.0) and r["m"]["ca"] == (-95.0, -11.0, 14.0)
          and r["m"]["rxpath"] == (-101.33333333333333, -13.0, 26 / 3), r)
    meta = {"eth0.10 (WOM 5G)": ("eth0.10", "WOM 5G"), "eth0.20 (Entel 5G)": ("eth0.20", "Entel 5G")}
    check("real cell-monitor output: its leg wom_5g is the label 'WOM 5G' of a link, and without the leg its carrier wom is in that label",
          fieldrec.map_series(key, meta, [])[:2] == ("eth0.10 (WOM 5G)", "label")
          and fieldrec.map_series((key[0], key[1], "", key[3]), meta, [])[:2] == ("eth0.10 (WOM 5G)", "carrier"), fieldrec.map_series(key, meta, []))


def radio_unit_checks():
    lp = fieldrec.lp_parse
    # --- line protocol
    check("lp: a line as cell-monitor writes it",
          lp('cell_nwinfo,site=terreno,leg=wom,rat=fdd_lte,band=lte_band_7 arfcn=3100i,plmn="73001" 1791592200')
          == ("cell_nwinfo", {"site": "terreno", "leg": "wom", "rat": "fdd_lte", "band": "lte_band_7"}, {"arfcn": 3100, "plmn": "73001"}, 1791592200))
    check("lp: escapes (space, comma, equals), a string with a comma, a space and a quote, floats, booleans, unsigned",
          lp(r'm\ x,t\,1=a\=b f="a b, c\" d",g=1.5,h=t,u=7u 5') == ("m x", {"t,1": "a=b"}, {"f": 'a b, c" d', "g": 1.5, "h": True, "u": 7}, 5))
    some = rf_lp(rf_points(only=("wom", "entel", "claro")), "s").splitlines()[::37] + rf_lp(rf_points(only=("claro",)), "").splitlines()[::50]
    check(f"lp: the fast reading of {len(some)} lines as cell-monitor writes them (with and without timestamp) is the full reading's",
          all(lp(x) == fieldrec.lp_parse_full(x) for x in some) and fieldrec.LP_FAST.fullmatch(some[0]) is not None
          and fieldrec.LP_FAST.fullmatch('m,t=a\\ b x=1') is None and fieldrec.LP_FAST.fullmatch('m x="a\\"b"') is None, some[:2])
    check("lp: no timestamp; an empty tag value (cell-monitor writes band= when the modem gave none) is no tag",
          lp("m,a=1,band= x=1i")[1:] == ({"a": "1"}, {"x": 1}, None))
    check("lp: what is no line (no fields, no tag name, a field without a value, one word) raises",
          all(_raises(lp, x) for x in ("m,a=1", "m,=1 x=1", "m x=", "garbage", "m x=1 2 3", "m x=abc")))
    check("lp: timestamps by size are s, ms, us or ns; or by the unit asked",
          [fieldrec.lp_seconds(x, "auto") for x in (1791592200, 1791592200123, 1791592200123456, 1791592200123456789)]
          == [1791592200, 1791592200.123, 1791592200.123456, 1791592200.123456789]
          and fieldrec.lp_seconds(1791592200123, "ms") == 1791592200.123 and fieldrec.lp_seconds(5, "ns") == 5e-9)
    rf = rf_of('# a comment\n\ncell_nwinfo,cpe_id=c,modem_id=m,rat=fdd_lte,band=b arfcn=1i 100\ncell_monitor,cpe_id=c,modem_id=m up=1i,lp_lines=3i 100\n'
               'cell_serving_lte,cpe_id=c,modem_id=m,band=b7 sinr=5i,mcc=730i 100\nnonsense\ncell_serving_lte,cpe_id=c,modem_id=m x=1i\n')
    smp = rf.series[("c", "m", "", "")].samples
    check("lp: lines of one second are one sample; fields not used are dropped; comments and blank lines pass; bad and unstamped lines are counted",
          list(smp) == [100] and smp[100].nw == ("fdd_lte", "b") and smp[100].up == 1 and smp[100].lte == ["b7", None, None, None, None, None, 5, None]
          and rf.bad == 1 and rf.no_time == 1, (smp[100].nw, smp[100].up, smp[100].lte, rf.bad, rf.no_time))
    # --- annotated CSV
    csv_hdr = "#group,false,false,true,true,false,false,true,true,true,true\r\n#datatype,string,long,dateTime:RFC3339,dateTime:RFC3339,dateTime:RFC3339,long,string,string,string,string\r\n" \
              "#default,_result,,,,,,,,,\r\n,result,table,_start,_stop,_time,_value,_field,_measurement,cpe_id,modem_id\r\n"
    rf = rf_of("﻿" if False else csv_hdr + ",,0,s,e,2026-10-10T00:00:10Z,14,sinr,cell_serving_lte,c,m\r\n,,0,s,e,2026-10-10T00:00:20.5Z,15,sinr,cell_serving_lte,c,m\r\n"
               ",,1,s,e,2026-10-10T00:00:10Z,-95,rsrp,cell_serving_lte,c,m\r\n,,2,s,e,2026-10-10T00:00:10Z,3,mcc,cell_serving_lte,c,m\r\n"
               "\r\n" + csv_hdr + ",,3,s,e,2026-10-10T01:00:10+01:00,7,sinr,cell_serving_nr,c,m\r\n", True)
    smp = rf.series[("c", "m", "", "")].samples
    t10 = fieldrec.csv_time("2026-10-10T00:00:10Z")
    check("csv: CRLF, tables of one block, a second block, a row per field, fractional seconds and zones, fields not used dropped",
          sorted(smp) == [int(t10), int(t10) + 11] and smp[int(t10)].lte == [None, None, None, None, -95, None, 14, None] and smp[int(t10) + 11].lte[6] == 15
          and smp[int(t10)].nsa and not smp[int(t10) + 11].nsa and rf.records == 5, (smp, rf.records))
    nohdr = rf_of(",result,table,_time,_value,_field,_measurement,cpe_id,modem_id,leg\n,,0,2026-10-10T00:00:10Z,5,sinr,cell_serving_lte,c,m,wom\n", True)
    check("csv: a header with no annotations works (tags by name)", ("c", "m", "wom", "") in nohdr.series, list(nohdr.series))
    dflt = rf_of("#group,false,false,true,false,true,true\n#datatype,string,long,dateTime:RFC3339,long,string,string\n#default,_result,,,,,cell_monitor\n"
                 ",result,table,_time,_value,_field,_measurement\n,,0,2026-10-10T00:00:10Z,0,up,\n", True)
    check("csv: a #default fills the empty cell (here the measurement)", list(dflt.series.values())[0].samples[int(t10)].up == 0 if dflt.series else False, dflt.series)
    nb = rf_of("#group,false,false,true,false,true,true\n#datatype,string,long,dateTime:RFC3339,long,string,string\n#default,_result,,,,,cell_monitor\n"
               ",result,table,_time,_value,_field,_measurement\n,,0,2026-10-10T00:00:10Z,0,up,\n"
               "#group,false,false,true,false,true,true\n#datatype,string,long,dateTime:RFC3339,long,string,string\n"
               ",result,table,_time,_value,_field,_measurement\n,,0,2026-10-10T00:00:20Z,0,up,\n", True)
    check("csv: a block that follows another with no blank line does not inherit its #default (the empty measurement stays unknown)",
          list(list(nb.series.values())[0].samples) == [int(t10)] and nb.unknown[""] == 1, (nb.series, nb.unknown))
    pv = rf_of("#group,false,false,true,true,false,true,true,true,false,false,false\n#datatype,string,long,dateTime:RFC3339,dateTime:RFC3339,dateTime:RFC3339,string,string,string,long,double,string\n"
               "#default,_result,,,,,,,,,,\n,result,table,_start,_stop,_time,_measurement,cpe_id,modem_id,sinr,weird,note\n"
               ",,0,s,e,2026-10-10T00:00:10Z,cell_serving_lte,c,m,14,1.5,hello\n,,0,s,e,2026-10-10T00:00:20Z,cell_serving_lte,c,m,,2.5,\n", True)
    sm = pv.series[("c", "m", "", "")].samples
    check("csv: pivoted, a column of something unknown is ignored, a row whose only used field is empty makes no sample", sm[int(t10)].lte == [None, None, None, None, None, None, 14, None] and list(sm) == [int(t10)], sm)
    nocol = rf_of("#group,false,false,false,false\n#datatype,string,long,string,double\n#default,_result,,,\n,result,table,_field,_value\n,,0,sinr,5\n", True)
    check("csv: a table with no _time or _measurement is counted, not guessed", nocol.no_cols == 1 and not nocol.series, (nocol.no_cols, nocol.series))
    t = fieldrec.csv_time
    check("csv: times in RFC 3339 with Z, a fraction, an offset or none (UTC), a bare number; garbage is None",
          t("2026-10-10T00:00:10Z") == 1791590410 and t("2026-10-10T00:00:10.25Z") == 1791590410.25 and t("2026-10-10T01:00:10+01:00") == 1791590410
          and t("2026-10-09T20:30:10-03:30") == 1791590410 and t("2026-10-10 00:00:10") == 1791590410 and t("1791590410") == 1791590410 and t("soon") is None)
    # --- what the tags look like, and what they mean
    sn = fieldrec.san_tag
    check("san_tag does what cell-monitor's does", sn("Entel PCS") == "entel_pcs" and sn(' WOM  5G! ') == "_wom_5g_" and sn("Telefónica") == "telefnica"
          and sn('a"b\r') == "ab" and sn("eth0.10") == "eth0.10" and sn("a-b_c") == "a-b_c" and sn(None) == "")
    nr, nb = fieldrec.norm_rat, fieldrec.norm_band
    check("normalizers: fdd_lte, tdd_lte and lte are lte; tdd_nr5g and nr5g-sa are nr5g; nr5g-nsa apart; others as they are",
          [nr(x) for x in ("fdd_lte", "tdd_lte", "lte", "tdd_nr5g", "nr5g-sa", "nr5g-nsa", "wcdma", "", None)]
          == ["lte", "lte", "lte", "nr5g", "nr5g", "nr5g-nsa", "wcdma", None, None])
    check("normalizers: lte_band_7 and b7 are b7, nr5g_band_78 and n78 are n78, 'b' alone (no band) is none",
          [nb(x) for x in ("lte_band_7", "b7", "LTE_BAND_28", "nr5g_band_78", "n78", "wcdma_band_1", "b", "n", "", None)]
          == ["b7", "b7", "b28", "n78", "n78", "wcdma_band_1", None, None, None, None])
    # --- the mapping
    meta = {"eth0.10 (WOM)": ("eth0.10", "WOM"), "eth0.20 (Entel 5G)": ("eth0.20", "Entel 5G"), "usb0 (Entel 4G)": ("usb0", "Entel 4G"), "sl": ("sl", "")}
    mp = lambda leg, car="", maps=(): fieldrec.map_series(("c", "m", leg, car), meta, maps)  # noqa: E731
    check("map: by label, case blind and sanitized (Entel 5G is entel_5g); by name when no label is it",
          mp("WOM")[:2] == ("eth0.10 (WOM)", "label") and mp("entel_5g")[:2] == ("eth0.20 (Entel 5G)", "label") and mp("sl")[:2] == ("sl", "name") and mp("ETH0.10")[:2] == ("eth0.10 (WOM)", "name"))
    check("map: a leg that is no one's is not rescued by the carrier (that would hide the typo)",
          mp("isp7", "wom")[0] is None and "isp7" in mp("isp7", "wom")[2])
    check("map: no leg, the carrier in a label; two labels with it, or none, is unmapped with the reason",
          mp("", "wom")[:2] == ("eth0.10 (WOM)", "carrier") and mp("", "entel")[0] is None and "2 links" in mp("", "entel")[2]
          and mp("", "claro")[0] is None and "no link's label" in mp("", "claro")[2] and mp("", "")[0] is None)
    cross = {"p (y)": ("p", "y"), "q (z)": ("q", "z")}      # one link's name is another's label: the label wins
    check("map: the label is tried before the name (leg 'y' is the label of p, though it is no one's name; leg 'q' is a name)",
          fieldrec.map_series(("c", "m", "y", ""), {"p (y)": ("p", "y"), "y (w)": ("y", "w")}, [])[:2] == ("p (y)", "label")
          and fieldrec.map_series(("c", "m", "q", ""), cross, [])[:2] == ("q (z)", "name"))
    check("map: a label shared by two links is ambiguous", fieldrec.map_series(("c", "m", "a", ""), {"x (A)": ("x", "A"), "y (A)": ("y", "A")}, [])[0] is None)
    check("map: --rf-map by leg, carrier or modem_id, to a label or a name; a wrong target is said",
          mp("isp1", "", [("isp1", "wom")])[:2] == ("eth0.10 (WOM)", "map") and mp("", "claro", [("claro", "sl")])[:2] == ("sl", "map")
          and fieldrec.map_series(("c", "rm1", "", ""), meta, [("rm1", "usb0")])[:2] == ("usb0 (Entel 4G)", "map")
          and mp("isp1", "", [("isp1", "x")])[0] is None and "no link has" in mp("isp1", "", [("isp1", "x")])[2])
    rows = [dict(cpe_id="c", modem_id="1", leg="wom", carrier="entel", link="eth0.10 (WOM)"),            # crossed with the next
            dict(cpe_id="c", modem_id="2", leg="entel_5g", carrier="wom", link="eth0.20 (Entel 5G)"),
            dict(cpe_id="d", modem_id="3", leg="usb0", carrier="entel", link="usb0 (Entel 4G)"),         # right
            dict(cpe_id="d", modem_id="4", leg="sl", carrier="starlink", link="sl"),                      # a label with no name in it
            dict(cpe_id="d", modem_id="5", leg="zz", carrier="wom", link=None)]                           # unmapped: nothing to say
    pn = fieldrec.pairing_notes(rows, meta)
    check("pairing: two series whose carrier tags are each other's label are flagged; the right pair, a label with no carrier name and an unmapped series are not",
          len(pn) == 2 and "c/1" in pn[0] and "c/2" in pn[1] and "crossed" in pn[0], pn)
    # --- statistics
    bt = fieldrec.binom_tail
    check("binomial: P(X>=1 | 10, 0.1) = 1 - 0.9^10, P(X<=0), the whole range, and the edges q = 0 and 1",
          abs(bt(1, 10, 10, 0.1) - (1 - 0.9 ** 10)) < 1e-12 and abs(bt(0, 0, 10, 0.1) - 0.9 ** 10) < 1e-12 and abs(bt(0, 10, 10, 0.3) - 1) < 1e-12
          and bt(1, 5, 5, 0) == 0.0 and bt(0, 0, 5, 0) == 1.0 and bt(5, 5, 5, 1) == 1.0 and bt(0, 4, 5, 1) == 0.0 and bt(11, 12, 10, 0.5) == 0.0)
    big = bt(200, 5000, 5000, 0.01)
    check("binomial: n = 5000 does not overflow nor underflow into nonsense (200 hits for 50 expected is ~1e-60 to 1e-70)", 0 < big < 1e-50, big)
    check("binomial: the sum of the two tails around a point is 1 + its probability", abs(bt(3, 10, 10, 0.2) + bt(0, 3, 10, 0.2) - 1 - bt(3, 3, 10, 0.2)) < 1e-12)
    check("thin: events closer than the gap to the last kept are dropped (the last kept, not the last seen)",
          fieldrec.thin([0, 30, 59, 61, 100, 130, 500], 60) == [0, 61, 130, 500] and fieldrec.thin([], 60) == [])
    A, B = fieldrec.iv_merge([[5, 9], [0, 3], [2, 4], [9, 12], [20, 20]]), fieldrec.iv_merge([[1, 6], [10, 30]])
    check("intervals: merge (touching ones too, empty ones gone), intersection, length, erosion, membership, clip",
          A == [[0, 4], [5, 12]] and fieldrec.iv_and(A, B) == [[1, 4], [5, 6], [10, 12]] and fieldrec.iv_len(A) == 11
          and fieldrec.iv_erode([[0, 10], [20, 24]], 2) == [[2, 8]] and fieldrec.iv_has(A, [0, 5], 4) and not fieldrec.iv_has(A, [0, 5], 4.5)
          and fieldrec.iv_has(A, [0, 5], 12) and not fieldrec.iv_has(A, [0, 5], -1) and fieldrec.iv_clip(3, 7, A, [0, 5]) == 3, (A, fieldrec.iv_and(A, B)))
    base = sorted(range(100))
    mr, z, pv_ = fieldrec.rank_test([49.5, 50.5, 20, 80], base)
    mr2, z2, pv2 = fieldrec.rank_test(list(range(0, 12)), base)
    check("rank test: values from the baseline's middle are at 50 % with p ~ 1; the 12 lowest are at 6 % with p tiny and z negative",
          abs(mr - 0.5) < 0.01 and pv_ > 0.9 and mr2 < 0.07 and z2 < -5 and pv2 < 1e-6 and fieldrec.rank_test([], base) == (None, None, None), (mr, pv_, mr2, z2, pv2))
    check("rank: ties take half", fieldrec.rank_of(5, [1, 5, 5, 9]) == 0.5 and fieldrec.rank_of(0, [1, 2]) == 0.0 and fieldrec.rank_of(9, [1, 2]) == 1.0)
    # --- a series with a single sample, and with nothing the section needs, does not break it
    a = analyze_raw(radio_status())
    one = radio_of(a, "cell_monitor,cpe_id=c,modem_id=m,leg=wom up=1i " + str(R0 + 3000) + "\n")
    check("a single sample is no coverage: mapped, noted, and the link has no hours", one["series"][0]["link"] == "eth0.10 (WOM)" and one["links"]["eth0.10 (WOM)"]["hours"] == 0
          and any("single sample" in n for n in one["notes"]), one["notes"])
    ts = "".join(f"cell_monitor,cpe_id=c,modem_id=m,leg=wom up=1i {R0 + 3000 + 10 * i}\n" for i in range(20))
    only = radio_of(a, ts)
    lw = only["links"]["eth0.10 (WOM)"]
    check("a series with only heartbeats: hours, no RAT, no SINR, events still counted, nothing divides by zero",
          lw["hours"] > 0.05 and lw["rat_pct"] == {"unknown": 100.0} and lw["sinr"]["n"] == 0 and lw["sinr"]["p50"] is None
          and lw["events"]["outage"]["sinr_before"] is None and lw["changes"]["rat"]["n"] == 0, lw["events"]["outage"])
    odd = "cell_serving_lte,cpe_id=c,modem_id=m,leg=wom sinr=90i " + "\ncell_serving_lte,cpe_id=c,modem_id=m,leg=wom sinr=95i ".join([str(R0 + 3000), str(R0 + 3010)]) + "\n"
    big_sinr = radio_of(a, odd)
    check("SINR above 50 is suspected to be the 0-250 scale and said", any("not dB" in n for n in big_sinr["notes"]), big_sinr["notes"])


def _raises(fn, *args):
    try:
        fn(*args)
    except ValueError:
        return True
    return False


def unit_checks():
    # Best pair by single figures is not the best by joint lateness, and the ratio is infinite.
    # x, y, z over 100000 packets; bit 0 = x, 1 = y, 2 = z.
    expect = collections.Counter({7: 100000})
    late = [collections.Counter({0: 98800, 3: 100, 1: 100, 2: 400, 4: 600})]
    v = fieldrec.joint_view(["x", "y", "z"], [50], expect, late, 100000, 0, {})
    b = v["deadlines"][0]["best"]
    check("unit: best pair by single is x+y (0.1 % late together), by joint x+z (never together)",
          b["by_single"]["set"] == ["x", "y"] and near(b["by_single"]["late_pct"], 0.1) and b["by_joint"]["set"] == ["x", "z"]
          and b["by_joint"]["late_pct"] == 0.0 and b["by_rtt"] is None, b)
    check("unit: an infinite ratio is null in the JSON and 'inf' in the report", b["ratio_single"] is None
          and fieldrec.fx(0.1, 0.0) == "inf" and fieldrec.fx(0.0, 0.0) == "1" and fieldrec.fx(None, 1.0) == "-")
    y_z = next(x for x in v["deadlines"][0]["subsets"] if x["set"] == ["y", "z"])
    check("unit: a set never late together has lift 0, 'if indep.' counts what independence would give",
          y_z["late"] == 0 and y_z["lift"] == 0.0 and near(y_z["late_if_indep"], 100000 * 0.005 * 0.006, 0.01), y_z)
    # Too few packets sent on a set: no probability for it, nor a best pair.
    v = fieldrec.joint_view(["x", "y"], [50], collections.Counter({3: 500}), [collections.Counter({3: 5})], 500, 0, {})
    b = v["deadlines"][0]["best"]
    check("unit: under the minimum of packets there is no figure", v["deadlines"][0]["subsets"][0]["late_pct"] is None
          and b["by_joint"] is None and b["by_single"] is None and b["all"]["late_pct"] is None, b)
    check("unit: a mask bit with no link of that id is named idN",
          fieldrec.mask_names(0b1101, {0: "a", 3: "d"}) == frozenset({"a", "id2", "d"}))
    subs = [dict(set=[str(i) for i in range(len(bin(m)) - 2) if m >> i & 1], late=m) for m in range(1, 256) if bin(m).count("1") >= 2]
    rows = fieldrec.pick_rows(subs)
    check("unit: with 8 links the report prints the pairs, the full set and the most frequent others, 40 in all",
          len(subs) == 247 and len(rows) == fieldrec.MD_SUBSETS and sum(len(x["set"]) == 2 for x in rows) == 28
          and any(len(x["set"]) == 8 for x in rows))


def main():
    with tempfile.TemporaryDirectory() as d:
        make(d)
        notes = []
        segs = fieldrec.segments(fieldrec.read_lines(fieldrec.files_of([d]), notes), notes)
        s = fieldrec.summarize(fieldrec.analyze(segs, -3))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fieldrec.report(s, notes, -3)
            rc = fieldrec.main([d, "--json", os.path.join(d, "s.json")])
        text = out.getvalue()
    L = s["links"]
    check("three runs (a restart, a gap)", s["segments"] == 3, s["segments"])
    check("the cut file is noted", any("cut short" in n for n in notes), notes)
    check("the line being written is skipped and noted", any("not JSON" in n for n in notes), notes)
    check("the older engine's wall clock is noted", any("without time_ms" in n for n in notes), notes)
    check("samples before NTP are kept out of the folds", any("before the clock" in n for n in notes), notes)
    # The cut file loses its tail: a's outages there are not counted.
    check("a: its outages, ~3 s each", 15 <= L["a"]["outages"] <= 20 and 2.5 <= L["a"]["outage_s"]["p50"] <= 3.5,
          L["a"]["outage_s"])
    check("b: a's outages and its own", L["b"]["outages"] == L["a"]["outages"] + 5, L["b"]["outages"])
    check("c: its own, from its silence alone", 8 <= L["c"]["outages"] <= 10, L["c"]["outages"])
    pairs = {(p["a"], p["b"]): p for p in s["pairs"]}
    ab, ac = pairs[("a", "b")], pairs[("a", "c")]
    check("P(b out | a out) is 1", ab["b_given_a"] >= 0.95, ab)
    check("every outage of a has one of b", ab["a_events_with_b"] == 1.0, ab)
    check("a and b: lift far above independence", ab["lift"] > 10, ab)
    check("a and c never fail together", ac["both_pct"] == 0 and not ac["a_events_with_b"], ac)
    sl = L["sl"]["starlink_outages"]
    check("Starlink: outages fold on seconds 12/27/42/57", sl and sl["peak"] == 12 and sl["share"] > 0.9, sl)
    st = L["sl"]["starlink_steps"]
    check("Starlink: RTT steps fold there too", st and st["peak"] in (11, 12, 13) and st["share"] > 0.8, st)
    for n in ("a", "b", "c"):
        f = L[n]["starlink_outages"]
        check(f"{n}: no 15 s pattern", f is None or f["share"] < 0.6, f)
    check("spread between all live links", s["spread_ms"][50] in (30.0, 35.0), s["spread_ms"])
    check("spread between the best two", s["spread_best_two_ms"][50] in (5.0, 10.0), s["spread_best_two_ms"])
    check("RTT: a at 30 ms", L["a"]["rtt_ms"][50] == 30.0, L["a"]["rtt_ms"])
    check("download loss of a ~ its time out", 0.5 < (L["a"]["loss_down_pct"] or 0) < 3, L["a"]["loss_down_pct"])
    check("upload loss of c > 0", (L["c"]["loss_up_pct"] or 0) > 0, L["c"]["loss_up_pct"])
    live = s["simultaneous"]
    check("mostly 4 of 4 live", (live.get("4 of 4 live") or 0) > 85, live)
    check("by hour, local time", all(0 <= h < 24 for h in L["a"]["by_hour"]) and L["a"]["by_hour"], L["a"]["by_hour"])
    check("main() writes the report and the JSON", rc == 0 and "# Field recording" in text and "| a | b |" in text)
    check("this old recording has no instruments: the three sections say no data",
          s["joint"] is None and s["lost_all"] is None and s["sizes"] is None
          and rendered(s, notes, -3).count("No data: the recording has no") == 3)
    bond_checks()
    unit_checks()
    radio_checks()
    radio_unit_checks()
    local_checks()
    local_unit_checks()
    export_checks()
    real_format_checks()
    print("fieldrec_test: " + ("ok" if not FAILS else f"FAILED: {', '.join(FAILS)}"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
