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
within a run. Run: python3 bench/fieldrec_test.py
"""

import collections
import gzip
import io
import json
import os
import sys
import tempfile
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


def bond_lines(soft_reset=None):
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
    check("report: the engine's other counter is explained", "had a link of id 8 or more" in text)
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
    print("fieldrec_test: " + ("ok" if not FAILS else f"FAILED: {', '.join(FAILS)}"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
