#!/usr/bin/env python3
"""calibrar.py NOISE...: the virtual cell's uplink against noise. For each
noise voltage (canal.py, XML-RPC :8765): the eNB's view (ul_snr, ul_mcs,
ul_bler, ul_bsr from its JSON report), the uplink capacity (UDP flood, what
the core received) and the RTT at a light load (1 Mb/s) and near the top
(80 % of that capacity), with the ping's loss. Needs the cell up
(celda.sh start) and LOGS pointing at its logs. Prints one JSON line per
noise level and writes them to $LOGS/calibracion.jsonl."""
import json
import os
import re
import statistics as st
import subprocess
import sys
import time
import xmlrpc.client

import perfil

LOGS = os.environ.get("LOGS", "/tmp/celda")
PORT, PIDF = 5231, os.path.join(LOGS, "iperf5231.pid")  # 5201 is this PC's iperf3 service
ch = xmlrpc.client.ServerProxy("http://127.0.0.1:8765")


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw).stdout


def report_since(t0):
    """The eNB's per-UE metrics written after t0, all UEs in one list."""
    return [u for us in perfil.report_since(t0, LOGS).values() for u in us]


def udp(rate_mbit, secs):
    """(offered, received Mb/s, loss %) of a UDP flow from the UE to the core."""
    perfil.iperf_server(PORT, PIDF)
    o = sh(f"sudo -n ip netns exec ue1 iperf3 -c 172.16.0.1 -p {PORT} -u -b {rate_mbit}M -t {secs} -l 1200 -J", timeout=secs + 30)
    perfil.kill_pidfile(PIDF)
    try:
        s = json.loads(o)["end"]["sum"]
        lost = s.get("lost_percent", 100.0)
        return s["bits_per_second"] / 1e6, s["bits_per_second"] / 1e6 * (1 - lost / 100), lost
    except (ValueError, KeyError):
        return rate_mbit, 0.0, 100.0


def ping_during(rate_mbit, secs=8):
    """RTT (p50, p90, max) and loss % of 4 pings a second while a UDP flow
    of rate_mbit runs (0: no flow)."""
    flow = None
    if rate_mbit > 0:
        perfil.iperf_server(PORT, PIDF)
        flow = subprocess.Popen(f"sudo -n ip netns exec ue1 iperf3 -c 172.16.0.1 -p {PORT} -u -b {rate_mbit}M -t {secs + 2} -l 1200",
                                shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
    o = sh(f"sudo -n ip netns exec ue1 ping -c {secs * 4} -i 0.25 -W 3 172.16.0.1", timeout=secs + 20)
    if flow:
        flow.wait(timeout=30)
        perfil.kill_pidfile(PIDF)
    rtts = sorted(float(x) for x in re.findall(r"time=([\d.]+)", o))
    m = re.search(r"([\d.]+)% packet loss", o)
    loss = float(m.group(1)) if m else 100.0
    if not rtts:
        return None, None, None, loss
    return rtts[len(rtts) // 2], rtts[int(len(rtts) * .9)], rtts[-1], loss


out = open(os.path.join(LOGS, "calibracion.jsonl"), "a")
for v in [float(x) for x in sys.argv[1:]]:
    ch.set_ul_noise(v)
    time.sleep(4)
    t0 = time.time()
    off, cap, caploss = udp(40, 8)
    rep = report_since(t0)
    snr = st.median([u["ul_snr"] for u in rep]) if rep else None
    mcs = st.median([u["ul_mcs"] for u in rep]) if rep else None
    bler = st.median([u["ul_bler"] for u in rep]) if rep else None
    light = ping_during(1.0)
    heavy = ping_during(round(max(0.5, 0.8 * cap), 1)) if cap > 0.2 else (None, None, None, 100.0)
    t1 = time.time()
    rep2 = report_since(t1 - 10)
    bsr = max((u.get("ul_bsr", 0) for u in rep2), default=None)
    row = dict(noise=v, ul_snr_db=snr and round(snr, 1), ul_mcs=mcs and round(mcs, 1), ul_bler=bler and round(bler, 3),
               cap_mbit=round(cap, 2), cap_loss=round(caploss, 1),
               rtt_light=light[:3], loss_light=light[3], load_heavy=round(0.8 * cap, 1), rtt_heavy=heavy[:3],
               loss_heavy=heavy[3], max_bsr=bsr)
    print(json.dumps(row), flush=True)
    out.write(json.dumps(row) + "\n")
ch.set_ul_noise(0)
