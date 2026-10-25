#!/usr/bin/env python3
"""Tests for bench/bondtab.py on made-up status files and runs whose answers are
known: the deltas of the engine's counters (client, server with sessions that come
and go, an engine without the reorder object, a missing snapshot), a run's JSON
line (iperf3, quic and story 024's live streams with pepstat.py's legs, the
1200-byte ping, the flow of small packets), the rule for
two modes against the numbers of story 012, the Mann-Whitney p-value against an
exact count, the tables, and which variant each one is compared with. Run:
python3 bench/bondtab_test.py
"""

import contextlib
import io
import itertools
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True  # no bench/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bondtab  # noqa: E402
import pepstat  # noqa: E402

fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def approx(a, b, tol=1e-9):
    return a is not None and b is not None and abs(a - b) <= tol


def reorder(k=1, held_max=7, burst=3):
    """A status "reorder" object whose counters are k times these numbers."""
    return {"enabled": True, "held": 0, "held_max": held_max, "pool_buffers": 4096, "pool_free": 4096,
            "in_order": 1000 * k, "held_total": 100 * k, "bypassed": 50 * k, "split_rx": 120 * k,
            "lost": {"rack": 2 * k, "cap": 3 * k, "overflow": 4 * k, "flush": 5 * k},
            "late": {"after_rack": 6 * k, "after_cap": 7 * k, "after_overflow": 8 * k, "old": 9 * k},
            "far": 10 * k, "resyncs": 11 * k, "pool_full": 12 * k, "max_burst": burst,
            "edges_ms": [1, 2, 5, 10, 20, 50, 100, 200, 500],
            "wait": [k] * 10, "late_by": [2 * k] * 10}


def client(k, with_reorder=True, old=0, drops=0, lost_all=0, **kw):
    d = {"mode": "client", "download": {"packets": 1, "too_old": old, "wireguard_drops": drops}, "lost_all": lost_all}
    if with_reorder:
        d["reorder"] = reorder(k, **kw)
    return d


def session(sid, k, drops=0, lost_all=0, with_reorder=True, **kw):
    s = {"id": sid, "wireguard_drops": drops, "lost_all": lost_all}
    if with_reorder:
        s["reorder"] = reorder(k, **kw)
    return s


def write(path, obj):
    with open(path, "w") as f:
        f.write(obj if isinstance(obj, str) else json.dumps(obj))


def ipj(dropped):
    return [{"ifname": "wgc", "stats64": {"rx": {"bytes": 1, "packets": 1, "errors": 0, "dropped": dropped}}}]


def snap(d, prefix, cli, srv, wgc=None, wgs=None):
    if cli is not None:
        write(f"{prefix}.client.json", cli)
    if srv is not None:
        write(f"{prefix}.server.json", srv)
    if wgc is not None:
        write(f"{prefix}.wgc.json", ipj(wgc))
    if wgs is not None:
        write(f"{prefix}.wgs.json", ipj(wgs))


def test_delta(d):
    b, a = os.path.join(d, "b"), os.path.join(d, "a")
    # The client: counters are differences, the maxima of the end, the buckets of the histograms
    snap(d, b, client(1, old=1, drops=2, lost_all=10, held_max=5, burst=2),
         {"mode": "server", "rx": {"too_old": 3}, "sessions": [session("aa", 1, drops=1, lost_all=4)]}, 100, 5)
    snap(d, a, client(4, old=1, drops=2, lost_all=25, held_max=9, burst=8),
         {"mode": "server", "rx": {"too_old": 3}, "sessions": [session("aa", 3, drops=1, lost_all=9)]}, 130, 5)
    r = bondtab.delta(b, a)
    c, s = r["eng"]["cli"], r["eng"]["srv"]
    check(c["held_total"] == 300 and c["in_order"] == 3000 and c["bypassed"] == 150, "client counters are differences")
    check(c["lost_rack"] == 6 and c["lost_cap"] == 9 and c["lost_overflow"] == 12 and c["lost_flush"] == 15, "lost.*")
    check(c["late_after_rack"] == 18 and c["late_after_cap"] == 21 and c["late_after_overflow"] == 24 and c["late_old"] == 27, "late.*")
    check(c["far"] == 30 and c["resyncs"] == 33 and c["pool_full"] == 36 and c["split_rx"] == 360, "far, resyncs, pool_full, split_rx")
    check(c["held_max"] == 9 and c["max_burst"] == 8, "the maxima are the end's, not differences")
    check(c["too_old"] == 0 and c["wireguard_drops"] == 0 and c["lost_all"] == 15, "download.too_old, wireguard_drops, lost_all")
    check(c["wait"] == [3] * 10 and c["late_by"] == [6] * 10, "histograms by bucket")
    check(s["held_total"] == 200 and s["lost_all"] == 5 and s["wireguard_drops"] == 0 and s["too_old"] == 0, "server session counters")
    check(r["wg_rx_dropped"] == {"cli": 30, "srv": 0}, "the WireGuard interfaces' rx_dropped")

    # The server: a session that appears is counted from zero, one that goes is not counted; rx.too_old
    snap(d, b, None, {"rx": {"too_old": 3}, "sessions": [session("aa", 1, lost_all=4), session("gone", 5, lost_all=100)]})
    snap(d, a, None, {"rx": {"too_old": 7}, "sessions": [session("aa", 2, lost_all=6), session("new", 2, lost_all=3)]})
    s = bondtab.delta(b, a)["eng"]["srv"]
    check(s["held_total"] == 100 + 200 and s["lost_all"] == 2 + 3, "sessions: the new one from zero, the gone one out")
    check(s["too_old"] == 4, "rx.too_old of the server")
    check(s["held_max"] == 7, "the largest held_max of the sessions")

    # An engine without the reorder object (today's): null, but the rest is there
    snap(d, b, client(0, with_reorder=False, lost_all=1), {"rx": {"too_old": 0}, "sessions": [session("aa", 0, lost_all=1, with_reorder=False)]})
    snap(d, a, client(0, with_reorder=False, lost_all=6), {"rx": {"too_old": 0}, "sessions": [session("aa", 0, lost_all=9, with_reorder=False)]})
    r = bondtab.delta(b, a)
    check(r["eng"]["cli"]["held_total"] is None and r["eng"]["cli"]["wait"] is None, "no reorder object: null")
    check(r["eng"]["cli"]["lost_all"] == 5 and r["eng"]["srv"]["lost_all"] == 8, "no reorder object: the rest is counted")
    check(r["eng"]["srv"]["pool_full"] is None, "no reorder object at the server: null")

    # Nothing to read: null all over, no error
    r = bondtab.delta(os.path.join(d, "nothing"), os.path.join(d, "nothing-either"))
    check(all(v is None for v in r["eng"]["cli"].values()) and all(v is None for v in r["eng"]["srv"].values()), "no files: null")
    check(r["wg_rx_dropped"] == {"cli": None, "srv": None}, "no ip -s output: null")
    write(f"{b}.client.json", "")  # an empty file, as bond_snap leaves it
    write(f"{a}.client.json", "{not json")
    check(bondtab.delta(b, a)["eng"]["cli"]["held_total"] is None, "broken files: null")
    # A missing before is not "all there is": null
    write(f"{a}.client.json", client(2))
    os.remove(f"{b}.client.json")
    check(bondtab.delta(b, a)["eng"]["cli"]["held_total"] is None, "no before snapshot: null")
    # A counter that went down (the engine restarted) counts as the new value
    snap(d, b, client(5), {"sessions": []})
    snap(d, a, client(2), {"sessions": []})
    check(bondtab.delta(b, a)["eng"]["cli"]["held_total"] == 200, "a restart counts from zero")
    # odd values do not break it
    snap(d, a, {"reorder": {"held_total": "x", "lost": [], "wait": [1, "a"]}, "download": 5, "lost_all": True}, {"sessions": "no"})
    check(bondtab.delta(b, a)["eng"]["cli"]["held_total"] is None, "odd types: null")


IPERF = {"start": {"tcp_mss_default": 1328},
         "end": {"streams": [{"sender": {"mean_rtt": 52000, "min_rtt": 50000}}],
                 "sum_sent": {"bytes": 132800000, "retransmits": 500},
                 "sum_received": {"bits_per_second": 44.5e6}}}
PING = "".join(f"64 bytes from 10.79.0.1: icmp_seq={i} ttl=64 time={50 + i / 10:.1f} ms\n" for i in range(100))
PING_BIG = "".join(f"1228 bytes from 10.79.0.1: icmp_seq={i} ttl=64 time={60 + i}.0 ms\n" for i in range(50))


def run_line(d, extra, result, cc="cubic", ping=PING, pingbig=PING_BIG):
    write(os.path.join(d, "res.json"), result)
    write(os.path.join(d, "ping"), ping)
    write(os.path.join(d, "pingbig"), pingbig)
    argv = ["line", "--result", os.path.join(d, "res.json"), "--ping", os.path.join(d, "ping"),
            "--case", "reorder50", "--variant", "split", "--dir", "up", "--cc", cc, "--rep", "3", "--secs", "10",
            "--cpu-client", "50", "--cpu-server", "100", "--hz", "100", "--netem", "yes"] + extra
    if pingbig is not None:
        argv += ["--ping-big", os.path.join(d, "pingbig")]
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = bondtab.main(argv)
    return rc, json.loads(out.getvalue())


def test_line(d):
    b, a = os.path.join(d, "lb"), os.path.join(d, "la")
    snap(d, b, client(1), {"rx": {"too_old": 0}, "sessions": [session("aa", 1)]}, 5, 6)
    snap(d, a, client(2), {"rx": {"too_old": 0}, "sessions": [session("aa", 2)]}, 5, 9)
    rc, j = run_line(d, ["--before", b, "--after", a], IPERF)
    check(rc == 0, "a run with traffic and pings succeeds")
    check(j["case"] == "reorder50" and j["variant"] == "split" and j["rep"] == 3 and j["dir"] == "up", "identity")
    check(approx(j["mbps"], 44.5) and j["retrans"] == 500, "goodput and retransmissions")
    check(j["segs"] == 100000 and approx(j["retrans_pct"], 0.5), "segments = bytes / MSS, rate = retrans / segments")
    check(j["pkts_sent"] is None and j["spurious"] is None and j["srtt_ms"] == 52.0, "TCP fields")
    check(j["pings"] == 100 and j["rtt_p50"] == 55.0 and j["rtt_p99"] == 59.9, "the 84-byte ping")
    check(j["pings_big"] == 50 and j["rtt_big_p50"] == 85.0 and j["rtt_big_p99"] == 109.0, "the 1200-byte ping")
    check(j["cpu_client_pct"] == 5.0 and j["cpu_server_pct"] == 10.0 and j["netem"] is True, "cpu and netem")
    check(j["eng"]["cli"]["held_total"] == 100 and j["eng"]["srv"]["held_total"] == 100 and j["wg_rx_dropped"]["srv"] == 3, "deltas in the line")
    check("small" not in j, "no small flow, no field")

    # quic: lost is retrans, its rate is over the packets sent
    quic = {"mbps": 40.0, "lost": 30, "sent": 3000, "spurious": 12, "srtt_ms": 80.0, "min_rtt_ms": 50.0, "cc": "cubic"}
    rc, j = run_line(d, [], quic, cc="quic")
    check(rc == 0 and j["retrans"] == 30 and j["spurious"] == 12 and j["pkts_sent"] == 3000, "quic fields")
    check(approx(j["retrans_pct"], 1.0) and j["segs"] is None, "quic rate = lost / sent")
    check("eng" not in j, "no snapshots, no engine fields")

    # a run with no traffic fails but still says so; no big ping, a small flow
    tx = os.path.join(d, "tx.out")
    rx = os.path.join(d, "rx.out")
    write(tx, "sent=500 send_err=0 rx=0 uniq=0 dup=0 reord=0\n")
    write(rx, "sent=0 send_err=0 rx=498 uniq=498 dup=0 reord=2 lat_avg_us=50 p50_us=45 p99_us=1200 p999_us=1500 over200ms=0 over50ms=0 big=0 bigord=0\n")
    rc, j = run_line(d, ["--small-tx", tx, "--small-rx", rx], {"junk": 1}, ping="", pingbig=None)
    check(rc == 1 and j["mbps"] == 0.0 and j["retrans"] is None, "no traffic: exit 1")
    check(j["rtt_p50"] is None and j["rtt_big_p50"] is None and j["pings_big"] == 0, "no pings: null")
    check(j["small"] == {"sent": 500, "uniq": 498, "p50_us": 45, "p99_us": 1200, "reord": 2}, "the small flow")

    # the one-line summary and the check of zero counters
    path = os.path.join(d, "bond.jsonl")
    with open(path, "w") as f:
        f.write(json.dumps(run_line(d, ["--before", b, "--after", a], IPERF)[1]) + "\n")
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        bondtab.main(["show", path])
    check("reorder50" in out.getvalue() and "0.50 %" in out.getvalue() and "big 85.0 / 109.0 ms" in out.getvalue(), "show")
    with contextlib.redirect_stdout(io.StringIO()):
        check(bondtab.main(["zero", path, "too_old", "wireguard_drops"]) == 0, "zero: nothing above 0")
        check(bondtab.main(["zero", path, "pool_full"]) == 1, "zero: pool_full is above 0 in the test line")
        check(bondtab.main(["zero", path, "wg_rx_dropped"]) == 1, "zero: the interface's rx_dropped")


def test_modes():
    f = bondtab.MODE_FLOOR
    # story 012, "Medidas con dispersión": reorder150 with Cubic
    retrans = [28, 30, 31, 33, 29, 247, 310, 395, 330, 280]
    rtt = [104, 104, 103, 105, 104, 79, 79, 78, 80, 79]
    m = bondtab.modes(retrans, f["retrans"])
    check(m is not None and len(m[0]) == 5 and len(m[1]) == 5 and max(m[0]) == 33 and min(m[1]) == 247, "reorder150 retransmissions: two modes")
    m = bondtab.modes(rtt, f["rtt_p50"])
    check(m is not None and len(m[0]) == 5 and max(m[0]) == 80 and min(m[1]) == 103, "reorder150 RTT: two modes")
    # starlink15 with BBR: 63.5 or 80 Mbit/s, and the retransmissions that go with them
    mbps = [63.5, 63.4, 63.6, 63.5, 63.5, 63.4, 63.6, 80.1, 80.0, 79.9]
    m = bondtab.modes(mbps, f["mbps"])
    check(m is not None and len(m[0]) == 7 and len(m[1]) == 3, "starlink15 BBR goodput: 7 and 3")
    m = bondtab.modes([970, 1380, 1100, 1250, 1000, 2470, 3350, 2900, 1050, 3000], f["retrans"])
    check(m is not None and len(m[0]) == 6 and len(m[1]) == 4 and max(m[0]) == 1380 and min(m[1]) == 2470,
          "starlink15 BBR retransmissions: 6 and 4")
    # no mode: a steady cell, few runs, one odd run, a gap under the floor, a spread-out cell
    check(bondtab.modes([43.5] * 10, f["mbps"]) is None, "equal: one mode")
    check(bondtab.modes([30, 31, 30, 29, 400], f["retrans"]) is None, "5 runs: not enough")
    check(bondtab.modes([30, 31, 30, 29, 32, 30, 31, 30, 29, 400], f["retrans"]) is None, "one odd run is an outlier")
    check(bondtab.modes([0, 0, 0, 0, 0, 3, 4, 5], f["retrans"]) is None, "a gap under the floor")
    check(bondtab.modes([1, 2, 3, 3, 4, 5, 7, 12, 20, 35], f["retrans"]) is None, "a tail is not a mode")
    check(bondtab.modes([10, 40, 70, 100, 130, 160, 190, 220, 250, 280], f["retrans"]) is None, "an even spread is not two modes")
    check(bondtab.modes([None, 1, 2], 1) is None, "nulls are left out")
    check(bondtab.modes([30, 30, 30, 30, 100, 100, 100, 100], 10) is not None, "identical runs in each mode")


def exact_p(a, b):
    """The exact two-sided Mann-Whitney p-value, by counting every split (no ties)."""
    allv = a + b
    n1 = len(a)
    ranks = {v: i + 1 for i, v in enumerate(sorted(allv))}
    u = lambda xs: sum(ranks[x] for x in xs) - n1 * (n1 + 1) / 2
    u0 = u(a)
    mu = n1 * len(b) / 2
    ext = n = 0
    for c in itertools.combinations(allv, n1):
        n += 1
        if abs(u(list(c)) - mu) >= abs(u0 - mu) - 1e-9:
            ext += 1
    return ext / n


def test_mwu():
    a, b = [1, 2, 3, 4, 5], [10, 11, 12, 13, 14]
    p = bondtab.mwu(a, b)
    check(p is not None and abs(p - exact_p(a, b)) < 0.01, f"separated 5 and 5: {p} against exact {exact_p(a, b):.4f}")
    a, b = [1.0, 3.2, 4.1, 6.5, 8.0, 9.9, 12.3], [2.0, 3.0, 5.5, 7.1, 8.8, 10.5, 11.0]
    p = bondtab.mwu(a, b)
    check(p is not None and abs(p - exact_p(a, b)) < 0.05 and p > 0.5, f"interleaved: {p} against exact {exact_p(a, b):.4f}")
    a, b = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0], [4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5]
    p = bondtab.mwu(a, b)
    check(p is not None and abs(p - exact_p(a, b)) < 0.03, f"shifted: {p} against exact {exact_p(a, b):.4f}")
    check(approx(bondtab.mwu(a, b), bondtab.mwu(b, a)), "symmetric")
    check(bondtab.mwu([1, 2, 3, 4], [5, 6, 7, 8, 9]) is None, "under 5 runs: None")
    check(bondtab.mwu([3] * 6, [3] * 6) is None, "all alike: None")
    check(bondtab.mwu([1, 1, 1, 2, 2, 2], [1, 1, 2, 2, 2, 2]) is not None, "ties are fine")
    check(bondtab.mwu([None, 1, 2, 3, 4, 5], [9, 9, 9, 9, 9]) is not None, "nulls are left out")


def row(case, var, dirn, cc, rep, mbps, retrans, pct, rtt=50.0, big=None, cpu=(10.0, 20.0), eng=None, **kw):
    r = {"case": case, "variant": var, "dir": dirn, "cc": cc, "rep": rep, "secs": 10, "mbps": mbps, "retrans": retrans,
         "pkts_sent": None, "spurious": None, "srtt_ms": None, "min_rtt_ms": None, "quic_cc": None, "segs": None,
         "retrans_pct": pct, "rtt_p50": rtt, "rtt_p99": rtt * 2, "pings": 99, "rtt_big_p50": big,
         "rtt_big_p99": None if big is None else big * 2, "pings_big": 99, "cpu_client_pct": cpu[0],
         "cpu_server_pct": cpu[1], "netem": True}
    if eng is not None:
        r["eng"] = eng
        r["wg_rx_dropped"] = {"cli": 0, "srv": 0}
    r.update(kw)
    return r


def test_sends():
    c = bondtab.flat_client({"upload": {"packets": 10},
                             "links": [{"tx_packets": 7, "tx_drops": 1}, {"tx_packets": 8, "tx_drops": 0}]})
    check(c["sent"] == 10 and c["tx_links"] == 15 and c["tx_drops"] == 1, "the client's sends: " + str(c))
    s = bondtab.flat_session({"download_packets": 4, "links": [{"tx_packets": 4, "tx_drops": 2}, {"tx_drops": 0}]})
    check(s["sent"] == 4 and s["tx_links"] is None and s["tx_drops"] == 2, "a link without tx_packets: unknown: " + str(s))
    check(bondtab.flat_client({})["tx_links"] is None, "no links: unknown")


def test_tables(d):
    rows = []
    zero_side = {k: 0 for k in bondtab.NAMES}
    for i in range(10):
        # solo: steady. split: twice the goodput, a retransmission rate in two modes, reordering counters
        rows.append(row("reorder150", "solo", "up", "cubic", i + 1, 43.5, 30 + i, 0.03 + i / 1000, rtt=100.0, big=101.0))
        low = i < 5
        eng = {"cli": dict(zero_side, wireguard_drops=0, lost_all=1, sent=1000, tx_links=1500, tx_drops=3),
               "srv": dict(zero_side, held_total=1000 + i, bypassed=500, lost_cap=7, late_after_cap=7, held_max=40, max_burst=12,
                           too_old=0, wireguard_drops=0, lost_all=2)}
        rows.append(row("reorder150", "split", "up", "cubic", i + 1, 80.0 + i / 10, 100 + i if low else 900 + 10 * i,
                        0.1 if low else 0.9, rtt=60.0 + i / 10 if low else 85.0 + i / 10, big=130.0, cpu=(12.0, 25.0), eng=eng))
    text = "\n".join(bondtab.tables(rows, ["solo", "redund-ro0", "splitnoro"]))
    check("| reorder150 | solo | up | cubic | 10 |" in text, "a row per cell")
    check("43.5 (43.5–43.5)" in text, "median and p25-p75 (the step 0 form)")
    check("retrans % " in text and "1200 B RTT p50" in text, "the rate and the big ping are columns")
    modes_listed = [ln for ln in text.splitlines() if ln.startswith("- ")]
    check("†" in text, "a cell with two modes is marked")
    check("- reorder150 split up cubic, retrans: 5 runs near 102 (100–104) and 5 runs near 970 (950–990)" in modes_listed,
          "the retransmission modes are listed: " + str(modes_listed))
    check("- reorder150 split up cubic, RTT p50: 5 runs near 60.2 (60.0–60.4) and 5 runs near 85.7 (85.5–85.9)" in modes_listed,
          "the RTT modes too: " + str(modes_listed))
    check(len(modes_listed) == 2 and not any("solo" in ln for ln in modes_listed), "nothing else is a mode (goodput, solo)")
    check("Engine counters per run" in text, "the engine table")
    check("| reorder150 | split | up | cubic | 10 | 1004 | 500 | 0 | 0/7/0/0 | 0/7/0/0 | 0/0/0 | 40/12 | 0/0 | 0/0 | 1/2 | 0/0 |" in text,
          "engine medians at the receiving end (server for up): " + str([ln for ln in text.splitlines() if "| split |" in ln]))
    check("| 1/2 | 0/0 | 1.50 | 3 |" in text,
          "sends per packet and send drops at the sending end (client for up): "
          + str([ln for ln in text.splitlines() if "| split |" in ln]))
    check("Against the reference variant" in text and "| reorder150 | split | up | cubic | solo | 1.85× |" in text,
          "the comparison against solo: " + str([ln for ln in text.splitlines() if "| solo |" in ln]))
    check("| 0.001 |" in text or "| 0.000 |" in text, "a rank-test p-value for the goodput")
    # a cell with an invariant broken is called out
    rows[1]["eng"]["srv"]["too_old"] = 3
    text = "\n".join(bondtab.tables(rows, ["solo"]))
    check("Above 0 where it should not be" in text and "srv too_old = 3" in text, "too_old above 0 is called out")

    # The old kind of file (step 0: no new fields) still gets its table; so does an engine without reorder
    old = [{"case": "ltespike", "variant": "bonded", "dir": "down", "cc": "cubic", "rep": 1, "secs": 4, "mbps": 44.82,
            "retrans": 21, "rtt_p50": 80.6, "rtt_p99": 107.0, "pings": 50, "cpu_client_pct": 32.2, "cpu_server_pct": 17.0,
            "netem": False}]
    text = "\n".join(bondtab.tables(old, ["solo"]))
    check(text.splitlines()[0] == "| case | variant | dir | cc | n | Mbit/s | retrans | RTT p50 | RTT p99 | CPU cli/srv % |", "the table of step 0, unchanged")
    check(text.splitlines()[2] == "| ltespike | bonded | down | cubic | 1 | 44.8 | 21 | 80.6 | 107.0 | 32.2/17.0 |", "a row of step 0, unchanged")
    check("Engine counters" not in text and "Against" not in text, "nothing else without data")
    check("-" == bondtab.cell([], 1) and bondtab.cell([1, 2, 3], 1) == "2.0 (1.0–3.0)", "cell: min-max under 4")
    check(bondtab.cell(list(range(1, 11)), 0) == "6 (3–8)", "cell: median and p25-p75 from 4 on: " + bondtab.cell(list(range(1, 11)), 0))
    path = os.path.join(d, "t.jsonl")
    write(path, "\n".join(json.dumps(r) for r in rows[:4]) + "\n\n")
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        bondtab.main(["table", path])
    check(out.getvalue().startswith("| case |"), "bondtab.py table")
    write(path, "")
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        check(bondtab.main(["table", path]) == 0, "an empty file")
    check(out.getvalue().strip() == "(no runs)", "an empty file says so")


def test_references(d):
    """Each variant against its family's references, all in one run (solo with them):
    redundancy against redundancy without the reorderer, the split against one link
    alone and against its control."""
    cpu = {"solo": 10.0, "redund": 17.5, "redund-ro0": 17.0, "split": 18.0, "splitnoro": 17.8, "split-bp256": 18.9,
           "bonded": 17.0, "redund-dw20": 17.5}
    rows = []
    for case, variants in (("equal", ("redund", "redund-ro0", "split", "splitnoro", "solo", "split-bp256")),
                           ("starlink15", ("redund", "redund-ro0", "split", "splitnoro")),  # bond_settle: no solo
                           ("het", ("bonded", "solo"))):  # BOND_VARIANTS empty: what CI runs
        for v in variants:
            for i in range(5):
                rows.append(row(case, v, "up", "cubic", i + 1, 40.0 + i, 10, 0.1 + i / 100, cpu=(cpu[v], cpu[v])))
    text = "\n".join(bondtab.tables(rows, None))
    comp = text[text.index("Against the reference variant"):].splitlines()
    pairs = [tuple(c.strip() for c in ln.split("|")[1:6]) for ln in comp if ln.startswith("| ") and "---" not in ln][1:]
    against = lambda case, v: [p[4] for p in pairs if p[0] == case and p[1] == v]
    check(against("equal", "redund") == ["redund-ro0"], "redund against redund-ro0 with solo there: " + str(pairs))
    check("| equal | redund | up | cubic | redund-ro0 | 1.00× | " in text and text.count("| 1.03× | 1.03× |") >= 1,
          "the CPU criterion row (17.5 / 17.0): " + str([ln for ln in comp if "| redund |" in ln]))
    check(against("equal", "split") == ["solo", "splitnoro"], "split against solo and its control: " + str(against("equal", "split")))
    check(against("equal", "redund-ro0") == ["solo"] and against("equal", "splitnoro") == ["solo"], "the controls against solo")
    check(against("equal", "solo") == [], "solo against nothing")
    check(against("equal", "split-bp256") == ["split"], "a -bpN variant against its base")
    check(not any(p[1] == "split" and p[4] == "redund-ro0" for p in pairs), "never split against redund-ro0")
    # No solo (starlink15): the families still pair up; a control without its
    # reference falls back to the first of solo, redund-ro0, splitnoro there
    check(against("starlink15", "redund") == ["redund-ro0"] and against("starlink15", "split") == ["splitnoro"],
          "starlink15 without solo: " + str([p for p in pairs if p[0] == "starlink15"]))
    check(against("starlink15", "redund-ro0") == [] and against("starlink15", "splitnoro") == ["redund-ro0"],
          "the fallback, first of the defaults that is not itself")
    # The CI's default run: bonded (no redund-ro0 there) against solo, as before
    check(against("het", "bonded") == ["solo"], "bonded against solo when there is no redund-ro0")
    # --ref: every variant against the first of these the group has, as before the families
    text = "\n".join(bondtab.tables(rows, ["solo", "redund-ro0"]))
    comp = text[text.index("Against the reference variant"):].splitlines()
    refd = [tuple(c.strip() for c in ln.split("|")[1:6]) for ln in comp if ln.startswith("| ") and "---" not in ln][1:]
    check({p[4] for p in refd if p[0] == "equal"} == {"solo"} and len([p for p in refd if p[0] == "equal"]) == 5,
          "--ref solo,...: all five against solo in equal: " + str(refd))
    check({p[4] for p in refd if p[0] == "starlink15"} == {"redund-ro0"}, "and against redund-ro0 without solo")
    # through main, as bond.sh calls it (no --ref) and with one
    path = os.path.join(d, "ref.jsonl")
    write(path, "\n".join(json.dumps(r) for r in rows) + "\n")
    for argv, want in ((["table", path], "| equal | split | up | cubic | splitnoro |"),
                       (["table", path, "--ref", "redund-ro0"], "| equal | split | up | cubic | redund-ro0 |")):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            bondtab.main(argv)
        check(want in out.getvalue(), f"{argv}: {want}")
    # the rule alone
    check(bondtab.references("split", {"solo", "split", "splitnoro"}) == ["solo", "splitnoro"], "references: split")
    check(bondtab.references("split-bp64", {"solo", "split-bp64", "splitnoro"}) == ["solo", "splitnoro"],
          "references: a -bpN variant without its base is its base")
    check(bondtab.references("mystery", {"mystery", "redund-ro0"}) == ["redund-ro0"], "references: an unknown variant")
    # -spN (split_spread_ms) as -bpN: split-sp0, the split without its delay guard, against split
    check(bondtab.references("split-sp0", {"solo", "split", "split-sp0", "splitnoro"}) == ["split"], "references: split-sp0")
    check(bondtab.references("split-sp0", {"solo", "split-sp0", "splitnoro"}) == ["solo", "splitnoro"],
          "references: split-sp0 without split is split")
    check(bondtab.references("split-sp0-bp64", {"split-sp0-bp64", "split-sp0", "split"}) == ["split-sp0"],
          "references: the nearest without its last suffix")
    check(bondtab.references("split-sp0-bp64", {"split-sp0-bp64", "split", "solo"}) == ["split"],
          "references: else further down")
    check(bondtab.references("splitnoro-sp0", {"splitnoro-sp0", "solo"}) == ["solo"], "references: a control's -spN")
    # -nc (split_spread_copy = no) against the same variant with the copies
    check(bondtab.references("split-sp30-nc", {"split-sp30-nc", "split-sp30", "split"}) == ["split-sp30"],
          "references: -nc against its copies")
    check(bondtab.references("split-nc", {"split-nc", "split", "solo"}) == ["split"], "references: split-nc")
    check(bondtab.references("split-ncx", {"split-ncx", "split", "solo"}) == ["solo"], "references: -ncx is no suffix")
    check(bondtab.references("solo", {"solo", "redund-ro0"}) == [], "references: solo")
    check(bondtab.references("split", {"split", "redund"}) == [], "references: nothing to compare with")


def test_sig(d):
    """linksig.h's observations in BOND_SAMPLE's per-link summary (bond.sh's sig* cases)."""
    sd = os.path.join(d, "sigsamples")
    os.makedirs(sd, exist_ok=True)
    for i in range(1, 11):
        sig = {"queue_owner": "others" if i >= 8 else "mine", "loss_kind": "random" if i >= 6 else "none",
               "episodes_mine": 2, "episodes_others": 1 if i >= 8 else 0, "episodes_unknown": 0}
        write(os.path.join(sd, f"c{i}.json"), {"links": [{"tx_bytes": 1000 * i, "sig": sig}, {"tx_bytes": 1000 * i}]})
        write(os.path.join(sd, f"s{i}.json"), {"sessions": [{"links": [{}, {}]}]})
    out, _ = bondtab.samples_summary(sd, 10, "up")
    g = out[0]["sig"]
    check(approx(g["others"], 0.6) and approx(g["mine"], 0.4) and g["unknown"] == 0, "sig: queue_owner over the second half")
    check(g["random"] == 1 and g["none"] == 0, "sig: loss_kind over the second half")
    check(g["ep_mine"] == 0 and g["ep_others"] == 1 and g["ep_unknown"] == 0, "sig: verdicts over the whole run")
    check("sig" not in out[1], "sig: an engine without it, no key")


def test_pep(d):
    """Story 024: pepstat.py's legs and buffers, and the live line."""
    pd = os.path.join(d, "pep")
    os.makedirs(pd, exist_ok=True)
    head = "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors\n"
    for tag, base in (("before", 0), ("after", 1)):
        for i, ns in enumerate(pepstat.NSS):
            write(os.path.join(pd, "snmp-%s-%s" % (tag, ns)),
                  head + "Tcp: 1 200 120000 -1 0 0 0 0 1 0 %d %d 0 0 0\n" % (1000 * base * (i + 1), 10 * base * i))
    ss1 = ("ESTAB 0 0 10.79.0.2:40000 10.79.0.1:8388\n"
           "\t skmem:(r0,rb131072,t0,tb87040,f0,w300000,o0,bl0,d0) cubic rtt:25.5/2 bytes_acked:100\n"
           "ESTAB 700000 0 192.168.79.1:1080 192.168.79.2:5000\n"
           "\t skmem:(r724000,rb6291456,t0,tb46080,f0,w0,o0,bl0,d0) cubic rtt:0.1/0.05\n")
    write(os.path.join(pd, "ss-cli-1"), ss1)
    write(os.path.join(pd, "ss-cli-2"), "")
    write(os.path.join(pd, "ss-srv-1"), "ESTAB 0 0 172.30.1.1:5 172.30.1.2:5201\n"
          "\t skmem:(r0,rb1,t0,tb1,f0,w102400,o0,bl0,d0) bbr rtt:101.0/1 bytes_acked:5\n")
    write(os.path.join(pd, "rss-1"), "ss-redir 3000\nss-server 4000\n")
    write(os.path.join(pd, "rss-2"), "ss-redir 3500\n")
    iperf = os.path.join(d, "pep-iperf.json")
    write(iperf, {"end": {"sum_received": {"bytes": 1380000}}})
    a = pepstat.argparse.Namespace(dir=pd, mode="tcp", midcc="cubic", farcc="bbr", secs=10, result=iperf,
                                   cc="cubic", cpu_cli=50, cpu_srv=100, hz=100)
    j = pepstat.summary(a)
    check(j["retrans"] == {"lan": 0, "cli": 10, "srv": 20, "far": 30}, "pep: retransmissions per netns")
    check(j["retrans_pct"]["srv"] == 2.0, "pep: over the delivered segments (bytes / 1380)")
    check(j["mem_kb"]["cli"]["peak"] == round((724000 + 300000) / 1024) and j["mem_kb"]["srv"]["peak"] == 100,
          "pep: rmem_alloc + wmem_queued, the peak over the samples")
    check(j["mid_rtt_ms"] == 25.5 and j["far_rtt_ms"] == 101.0 and j["near_rtt_ms"] is None, "pep: the legs' RTT")
    check(j["rss_kb"] == {"ss-redir": 3500, "ss-server": 4000}, "pep: the proxies' RSS at its peak")
    check(j["cpu_cli_pct"] == 5.0 and j["cpu_srv_pct"] == 10.0 and "kcp" not in j, "pep: CPU, no kcptun")
    live = {"far": {"mbps": 7.5, "sec_p5": 6.0, "delivered": 100, "size": 1250, "send": {"total_retrans": 7, "srtt_ms": 3.0}},
            "near": {"mbps": 8.0, "delivered": 60, "size": 1250, "send": {}}}
    write(iperf, live)
    check(pepstat.rx_bytes(iperf, "live") == 160 * 1250, "pep: live bytes are both streams' records")
    write(os.path.join(d, "pep.json"), j)
    rc, line = run_line(d, ["--pep", os.path.join(d, "pep.json")], live, cc="live")
    check(rc == 0 and line["mbps"] == 7.5 and line["retrans"] == 7 and line["srtt_ms"] == 3.0, "live: the far stream")
    check(line["live"]["near"]["mbps"] == 8.0 and line["pep"]["retrans"]["cli"] == 10, "live: the streams and the legs")


def main():
    with tempfile.TemporaryDirectory() as d:
        test_delta(d)
        test_line(d)
        test_modes()
        test_mwu()
        test_sends()
        test_tables(d)
        test_references(d)
        test_sig(d)
        test_pep(d)
    print(f"bondtab_test: {checks - fails}/{checks} ok" if not fails else f"bondtab_test: {fails} of {checks} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
