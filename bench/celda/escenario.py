#!/usr/bin/env python3
"""escenario.py: UE 0 (ours: 1 Mb/s up, like cengarde's copies, plus 4
pings a second) at a good and a bad SNR, while UE 1 (the cell's other
users) loads the uplink at 0..N Mb/s. Per case: our RTT p50/p90/max, ping
and UDP loss, and the eNB's view of each UE (SNR, MCS, BLER). Needs
celda.sh with NUE=2 and LOGS set. One JSON line per case, also appended to
$LOGS/escenario.jsonl.

  escenario.py "OURGAIN,..." "OTHERMBIT,..." [FLOOR]
  (defaults 1,0.3,0.15 and 0,5,10,20; floor 300: the eNB's common noise;
  a gain is UE 0's path loss as an amplitude, 0.3 ~ -10 dB, 0.15 ~ -16 dB)"""
import json
import os
import re
import statistics as st
import subprocess
import sys
import time
import xmlrpc.client

LOGS = os.environ.get("LOGS", "/tmp/celda")
ch = xmlrpc.client.ServerProxy("http://127.0.0.1:8765")
ours = [float(x) for x in (sys.argv[1] if len(sys.argv) > 1 else "1,0.3,0.15").split(",")]
others = [float(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "0,5,10,20").split(",")]
FLOOR = float(sys.argv[3]) if len(sys.argv) > 3 else 300.0
SECS = 12


def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout).stdout


def report_since(t0):
    txt = sh(f"sudo -n cat {LOGS}/enb_report.json")
    dec, i, out = json.JSONDecoder(), 0, {}
    while True:
        j = txt.find("{", i)
        if j < 0:
            break
        try:
            obj, i = dec.raw_decode(txt, j)
        except ValueError:
            break
        if obj.get("type") == "metrics" and obj.get("timestamp", 0) >= t0:
            for c in obj.get("cell_list", []):
                for u in c["cell_container"].get("ue_list", []):
                    u = u["ue_container"]
                    out.setdefault(u["ue_rnti"], []).append(u)
    return out


def udp(ns, port, mbit, secs):
    return subprocess.Popen(f"sudo -n ip netns exec {ns} iperf3 -c 172.16.0.1 -p {port} -u -b {mbit}M -t {secs} -l 1200 -J",
                            shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def lost(p):
    try:
        return json.loads(p.communicate(timeout=60)[0])["end"]["sum"].get("lost_percent")
    except (ValueError, KeyError, subprocess.TimeoutExpired):
        return None


log = open(os.path.join(LOGS, "escenario.jsonl"), "a")
ch.set_ul_noise(FLOOR)
ch.set_ul_gain(1.0, 1)  # the other users: a good signal
for our in ours:
    ch.set_ul_gain(our, 0)
    for oth in others:
        time.sleep(3)
        for port in (5201, 5202):
            sh(f"iperf3 -s -B 172.16.0.1 -p {port} -D -1")
        time.sleep(0.5)
        t0 = time.time()
        bg = udp("ue2", 5202, oth, SECS + 3) if oth > 0 else None
        time.sleep(1.5)
        fg = udp("ue1", 5201, 1, SECS)
        o = sh(f"sudo -n ip netns exec ue1 ping -c {SECS * 4} -i 0.25 -W 4 172.16.0.1", timeout=SECS + 30)
        fg_loss, bg_loss = lost(fg), (lost(bg) if bg else None)
        rtts = sorted(float(x) for x in re.findall(r"time=([\d.]+)", o))
        m = re.search(r"([\d.]+)% packet loss", o)
        rep = report_since(t0)
        ues = []
        for rnti, us in sorted(rep.items()):
            ues.append(dict(rnti=rnti, snr=round(st.median(u["ul_snr"] for u in us), 1),
                            mcs=round(st.median(u["ul_mcs"] for u in us), 1),
                            bler=round(st.median(u["ul_bler"] for u in us), 3),
                            mbit=round(st.median(u["ul_bitrate"] for u in us) / 1e6, 2)))
        row = dict(our_gain=our, floor=FLOOR, other_mbit=oth,
                   rtt=[rtts[len(rtts) // 2], rtts[int(len(rtts) * .9)], rtts[-1]] if rtts else None,
                   ping_loss=float(m.group(1)) if m else 100.0, our_udp_loss=fg_loss, other_udp_loss=bg_loss, enb=ues)
        print(json.dumps(row), flush=True)
        log.write(json.dumps(row) + "\n")
ch.set_ul_noise(0)
ch.set_ul_gain(1.0, 0)
