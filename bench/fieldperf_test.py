#!/usr/bin/env python3
"""Tests for bench/fieldperf.py on a made-up fieldperf.sh directory whose
answers are known: two links on one trunk, a solo upload, an "all" download
with one iperf3 that failed, and a tunnel upload; pings in busybox's and in
iputils' words, one with losses and one with none answered; the counters
of the UDP sockets (IPv4 and IPv6), of each device, of its queue and of the
engine. Run: python3 bench/fieldperf_test.py
"""

import contextlib
import csv
import io
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True  # no bench/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fieldperf  # noqa: E402

fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def approx(a, b, tol=1e-6):
    return a is not None and b is not None and abs(a - b) <= tol


def put(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def busybox_ping(times, sent):
    lines = [f"PING 192.0.2.1 (192.0.2.1): 56 data bytes"]
    lines += [f"64 bytes from 192.0.2.1: seq={i} ttl=55 time={t:.3f} ms" for i, t in enumerate(times)]
    lines += ["", "--- 192.0.2.1 ping statistics ---",
              f"{sent} packets transmitted, {len(times)} packets received, "
              f"{100 * (sent - len(times)) // sent}% packet loss"]
    return "\n".join(lines) + "\n"


def iputils_ping(times, sent):
    lines = ["PING 10.79.0.1 (10.79.0.1) 56(84) bytes of data."]
    lines += [f"64 bytes from 10.79.0.1: icmp_seq={i + 1} ttl=64 time={t} ms" for i, t in enumerate(times)]
    lines += ["", "--- 10.79.0.1 ping statistics ---",
              f"{sent} packets transmitted, {len(times)} received, 0% packet loss, time 4005ms"]
    return "\n".join(lines) + "\n"


def iperf_json(mbps, retr=None, rtts=None):
    """An iperf3 -J output: the receiver's goodput, the sender's
    retransmissions and, per stream, (mean, max) RTT in ms."""
    streams = [{"sender": {"mean_rtt": m * 1000, "max_rtt": x * 1000}} for m, x in (rtts or [])]
    sent = {"bits_per_second": mbps * 1.05e6}
    if retr is not None:
        sent["retransmits"] = retr
    return json.dumps({"start": {}, "intervals": [], "end": {
        "streams": streams, "sum_sent": sent, "sum_received": {"bits_per_second": mbps * 1e6}}}, indent=1)


def snmp(sndbuf):
    return ("Ip: Forwarding DefaultTTL\nIp: 1 64\n"
            "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors\n"
            f"Udp: 100 0 0 200 0 {sndbuf} 0\n"
            "UdpLite: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors\n"
            "UdpLite: 0 0 0 0 0 99 0\n")


def stats(dropped):
    return f"tx_packets 1000\ntx_bytes 1500000\ntx_dropped {dropped}\ntx_errors 0\n"


def tc(dropped):
    return (f"qdisc fq_codel 0: root refcnt 2 limit 10240p flows 1024\n"
            f" Sent 123456 bytes 789 pkt (dropped {dropped}, overlimits 0 requeues 0)\n"
            f" backlog 0b 0p requeues 0\n"
            f"qdisc fq_codel 8001: parent 1: limit 10240p\n"
            f" Sent 1 bytes 1 pkt (dropped 555, overlimits 0 requeues 0)\n")


def status(l1, l2):
    """Engine status: (tx_bytes, tx_drops) of l1 and l2."""
    return json.dumps({"mode": "client", "links": [
        {"name": "l1", "label": "CLARO", "tx_bytes": l1[0], "tx_drops": l1[1]},
        {"name": "l2", "label": "WOM", "tx_bytes": l2[0], "tx_drops": l2[1]}]})


def snap(d, sndbuf, sndbuf6, drops, lower, qd, eng):
    put(f"{d}/snmp", snmp(sndbuf))
    put(f"{d}/snmp6", f"Udp6InDatagrams   5\nUdp6SndbufErrors  {sndbuf6}\n")
    for dev, n in drops.items():
        put(f"{d}/stats-{dev}.txt", stats(n))
    put(f"{d}/stats-eth0.txt", stats(lower))
    for dev, n in qd.items():
        put(f"{d}/tc-{dev}.txt", tc(n))
    put(f"{d}/status.json", status(*eng))
    put(f"{d}/time", "0\n")


def make(root):
    put(f"{root}/plan", "modes=solo all tunnel\nsecs=10\nstreams=4\ntunnel=10.79.0.1\ndevs=l1 eth0 l2 \n"
        "version=cengarde 0.6.3 (protocol 4)\nLinux router 6.6\niperf 3.17\n")
    put(f"{root}/links", "l1 CLARO 192.0.2.1\nl2 WOM 192.0.2.1\n")
    put(f"{root}/idle/ping-l1.txt", busybox_ping([40, 41, 42, 43, 44], 5))
    put(f"{root}/idle/ping-l2.txt", busybox_ping([50, 51, 52, 53, 54], 5))
    put(f"{root}/idle/ping-tunnel.txt", iputils_ping([45.5, 46.5, 47.5, 48.5, 49.5], 5))

    # 01: CLARO alone, upload: 35 Mbit/s, 120 retransmissions, two streams
    # of (60, 300) and (80, 500) ms; ping 12 sent, 10 back; 7 UDP send
    # buffer errors (5 over IPv4, 2 over IPv6), 3 device drops on l1, 4 on
    # the trunk, 9 in l1's queue; the engine sent 1.25 MB on l1 (1 Mbit/s
    # over 10 s) and dropped 0.
    t = f"{root}/01-solo-up-CLARO"
    put(f"{t}/meta", "mode=solo\ndir=up\nsecs=10\nstreams=4\nstart=1000\nlink=CLARO dev=l1 host=192.0.2.1\nend=1010\n")
    put(f"{t}/iperf-l1.json", iperf_json(35, 120, [(60, 300), (80, 500)]))
    put(f"{t}/ping-l1.txt", busybox_ping([100, 200, 300, 400, 500, 600, 700, 800, 900, 1000], 12))
    snap(f"{t}/before", 10, 1, {"l1": 0, "l2": 0}, 100, {"l1": 1, "l2": 0}, ((0, 0), (0, 0)))
    snap(f"{t}/after", 15, 3, {"l1": 3, "l2": 0}, 104, {"l1": 10, "l2": 0}, ((1250000, 0), (0, 0)))

    # 02: both at once, download: CLARO 80, WOM's iperf3 failed (no JSON,
    # its error in .err); WOM's ping never answered; the engine dropped 12
    # sends on l2.
    t = f"{root}/02-all-down"
    put(f"{t}/meta", "mode=all\ndir=down\nsecs=10\nstreams=4\nstart=2000\nlink=CLARO dev=l1 host=192.0.2.1\n"
        "link=WOM dev=l2 host=192.0.2.1\nend=2010\n")
    put(f"{t}/iperf-l1.json", iperf_json(80, 30))
    put(f"{t}/iperf-l2.json", "")
    put(f"{t}/iperf-l2.err", "iperf3: error - unable to connect to server: Connection refused\n")
    put(f"{t}/ping-l1.txt", busybox_ping([60, 70, 80], 3))
    put(f"{t}/ping-l2.txt", busybox_ping([], 12))
    snap(f"{t}/before", 15, 3, {"l1": 3, "l2": 0}, 104, {}, ((0, 0), (0, 5)))
    snap(f"{t}/after", 15, 3, {"l1": 3, "l2": 0}, 104, {}, ((0, 0), (0, 17)))

    # 03: the tunnel, upload: 50 Mbit/s through it, its ping in iputils'
    # words; the engine sent 62.5 MB on l1 and 31.25 MB on l2 (50 and 25
    # Mbit/s over 10 s), dropped 40 on l1.
    t = f"{root}/03-tunnel-up"
    put(f"{t}/meta", "mode=tunnel\ndir=up\nsecs=10\nstreams=4\nstart=3000\nlink=CLARO dev=l1 host=192.0.2.1\n"
        "link=WOM dev=l2 host=192.0.2.1\nend=3010\n")
    put(f"{t}/iperf-tunnel.json", iperf_json(50, 7, [(90, 200)]))
    put(f"{t}/ping-tunnel.txt", iputils_ping([150, 160, 170], 3))
    put(f"{t}/ping-l1.txt", busybox_ping([120, 130], 2))
    put(f"{t}/ping-l2.txt", busybox_ping([55], 1))
    snap(f"{t}/before", 15, 3, {"l1": 3, "l2": 0}, 104, {}, ((1000, 100), (2000, 17)))
    snap(f"{t}/after", 16, 3, {"l1": 3, "l2": 1}, 106, {}, ((62501000, 140), (31252000, 17)))
    # not a test directory
    put(f"{root}/marcas.txt", "1000 2026-10-13 05:00:00 solo-up-CLARO\n")


def test_parts(root):
    p = fieldperf.ping(f"{root}/01-solo-up-CLARO/ping-l1.txt")
    check(p["sent"] == 12 and p["lost"] == 2, f"ping: 12 sent, 2 lost: {p}")
    check(p["p50"] == 600 and p["p95"] == 1000 and p["max"] == 1000, f"ping: p50 600, p95 and max 1000: {p}")
    p = fieldperf.ping(f"{root}/idle/ping-tunnel.txt")
    check(p["sent"] == 5 and p["lost"] == 0 and p["p50"] == 47.5, f"ping in iputils' words: {p}")
    p = fieldperf.ping(f"{root}/02-all-down/ping-l2.txt")
    check(p["p50"] is None and p["lost"] == 12, f"a ping with no answer: {p}")
    check(fieldperf.ping(f"{root}/nothing.txt") is None, "no ping file: None")

    i = fieldperf.iperf(f"{root}/01-solo-up-CLARO/iperf-l1.json")
    check(approx(i["mbps"], 35) and i["retr"] == 120, f"iperf: 35 Mbit/s received, 120 retransmissions: {i}")
    check(approx(i["rtt_mean"], 70) and approx(i["rtt_max"], 500), f"iperf: RTT mean of means 70, max 500: {i}")
    i = fieldperf.iperf(f"{root}/02-all-down/iperf-l2.json")
    check(i["error"].startswith("iperf3: error - unable to connect"), f"iperf: no JSON, the .err line: {i}")

    t = f"{root}/01-solo-up-CLARO"
    check(fieldperf.snmp(f"{t}/before") == 11 and fieldperf.snmp(f"{t}/after") == 18,
          "snmp: Udp and Udp6 SndbufErrors added, UdpLite's left out")
    check(fieldperf.qdisc(f"{t}/after", "l1") == 10, "tc: the root qdisc's drops, not its children's")
    check(fieldperf.qdisc(f"{t}/after", "l2") == 0 and fieldperf.qdisc(f"{root}/02-all-down/after", "l1") is None,
          "tc: 0 is 0, no file is None")
    check(fieldperf.stats(f"{t}/after", "eth0")["tx_dropped"] == 104, "sysfs stats")


def test_rows(root):
    a = fieldperf.analyse(root)
    rows = a["rows"]
    by = {(r["test"], r["link"]): r for r in rows}
    check(len(rows) == 1 + 3 + 3, f"rows: solo 1, all and tunnel 1 + 2 each: {[(r['test'], r['link']) for r in rows]}")
    check([l["label"] for l in a["links"]] == ["CLARO", "WOM"], "links")
    check(a["idle"]["l2"]["p50"] == 52, "idle p50 of WOM")

    r = by[("01-solo-up-CLARO", "CLARO")]
    check(approx(r["mbps"], 35) and r["retr"] == 120 and approx(r["rtt_mean"], 70), f"solo row: iperf: {r}")
    check(r["ping"]["p50"] == 600 and r["idle_p50"] == 42, "solo row: ping under load and idle")
    check(r["sndbuf"] == 7, f"solo row: 7 UDP send buffer errors: {r['sndbuf']}")
    check(r["dev_drops"] == 3 and r["lower_drops"] == 4 and r["qdisc_drops"] == 9,
          f"solo row: device 3, trunk 4, queue 9: {r}")
    check(approx(r["engine_mbps"], 1.0) and r["sock_drops"] == 0, f"solo row: the engine's 1 Mbit/s, 0 drops: {r}")

    w = by[("02-all-down", "*")]
    check(approx(w["mbps"], 80), f"all: the whole is the links that answered: {w['mbps']}")
    check(w["failed"] == 1 and w["error"] is None, "all: the whole says a link failed")
    check(w["sndbuf"] == 0 and w["sock_drops"] == 12 and w["lower_drops"] == 0, f"all: counters of the whole: {w}")
    r = by[("02-all-down", "WOM")]
    check(r["mbps"] is None and r["error"] and r["sock_drops"] == 12, f"all: WOM failed, 12 socket drops: {r}")
    check(r["ping"]["lost"] == 12 and r["sndbuf"] is None and r["lower_drops"] is None,
          "all: per link, no router-wide counters")
    check(by[("02-all-down", "CLARO")]["qdisc_drops"] is None, "all: no tc, no queue drops")

    w = by[("03-tunnel-up", "tunnel")]
    check(approx(w["mbps"], 50) and w["retr"] == 7 and approx(w["rtt_max"], 200), f"tunnel: its iperf3: {w}")
    check(w["ping"]["p50"] == 160 and w["idle_p50"] == 47.5, "tunnel: its ping, and the idle one")
    check(approx(w["engine_mbps"], 75) and w["sock_drops"] == 40, f"tunnel: the engine sent 75 Mbit/s, 40 drops: {w}")
    check(w["sndbuf"] == 1 and w["dev_drops"] == 1 and w["lower_drops"] == 2, f"tunnel: router counters: {w}")
    r = by[("03-tunnel-up", "CLARO")]
    check(r["mbps"] is None and approx(r["engine_mbps"], 50) and r["ping"]["p50"] == 130,
          f"tunnel: CLARO carried 50 Mbit/s of it: {r}")
    check(approx(by[("03-tunnel-up", "WOM")]["engine_mbps"], 25), "tunnel: WOM 25 Mbit/s")


def test_output(root, d):
    out = io.StringIO()
    a = fieldperf.analyse(root)
    fieldperf.markdown(a, out)
    md = out.getvalue()
    check("| solo up | CLARO | 35.0 | 120 | 70.0/500.0 | 600/1000/1000, 2 lost | 42.0 | 1.0 | 0 | 3/4 | 9 | 7 |" in md,
          "markdown: the solo row")
    check("| all down | * | 80.0 (1 failed) |" in md, "markdown: the whole of all, with the failed link counted")
    check("|  | WOM | error: iperf3: error - unable to connect" in md and "all lost (12)" in md, "markdown: WOM's row")
    check("| tunnel up | tunnel | 50.0 | 7 | 90.0/200.0 | 160/170/170 | 47.5 | 75.0 | 40 | 1/2 |" in md,
          "markdown: the tunnel row")
    check("engine cengarde 0.6.3 (protocol 4)" in md, "markdown: the engine's version")
    csvf, jsonf = os.path.join(d, "t.csv"), os.path.join(d, "t.json")
    with contextlib.redirect_stdout(io.StringIO()):
        fieldperf.main([root, "--csv", csvf, "--json", jsonf])
    with open(csvf, encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    check(len(rows) == 7 and list(rows[0]) == fieldperf.FIELDS, "csv: 7 rows, the columns")
    check(rows[0]["ping_lost"] == "2" and rows[0]["sndbuf"] == "7", f"csv: flat values: {rows[0]}")
    with open(jsonf, encoding="utf-8") as f:
        j = json.load(f)
    check(len(j) == 7 and j[-1]["link"] == "WOM" and approx(j[-1]["engine_mbps"], 25), "json: the same rows")


def main():
    with tempfile.TemporaryDirectory() as d:
        root = os.path.join(d, "fieldperf-test")
        make(root)
        test_parts(root)
        test_rows(root)
        test_output(root, d)
    print(f"fieldperf_test: {checks - fails}/{checks} ok" if not fails else f"fieldperf_test: {fails} of {checks} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
