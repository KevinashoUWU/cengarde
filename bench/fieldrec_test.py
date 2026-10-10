#!/usr/bin/env python3
"""Tests for bench/fieldrec.py on a made-up recording whose answers are known:
links a and b fail together (b also alone), c fails on its own, sl is a
Starlink that loses 1.5 s and changes its RTT at seconds 12, 27, 42 and 57;
the engine restarts once, the recorder stops for a minute, the clock is set
by NTP after the first 100 s, one file comes from an engine without time_ms
and the last one was cut by a power loss. Run: python3 bench/fieldrec_test.py
"""

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


FAILS = []


def check(what, ok, got=None):
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f": {got}"))
    if not ok:
        FAILS.append(what)


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
    print("fieldrec_test: " + ("ok" if not FAILS else f"FAILED: {', '.join(FAILS)}"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
