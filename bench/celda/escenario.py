#!/usr/bin/env python3
"""escenario.py: two ways to load the virtual cell and watch UE 0 (ours).
One JSON line per case, also appended to $LOGS/escenario.jsonl.

  escenario.py "OURGAIN,..." "OTHERMBIT,..." [FLOOR]
      UE 0 (1 Mb/s up, like cengarde's copies, plus 4 pings a second) at
      each gain while UE 1 (the cell's other users) loads the uplink at
      0..N Mb/s; our RTT p50/p90/max, ping and UDP loss, and the eNB's view
      of each UE (SNR, MCS, BLER). Needs celda.sh with NUE=2. (Defaults
      1,0.3,0.15 and 0,5,10,20; floor 300: the eNB's common noise; a gain
      is UE 0's path loss as an amplitude, 0.3 ~ -10 dB, 0.15 ~ -16 dB.)

  escenario.py tasa "RATE,..." [N]
      With the cell up as a profile (PERFIL=name celda.sh start: its
      channel, its queue and the other users' load already running): per
      repetition, UE 0's capacity (a UDP flood, what the core got once
      the queue was full: the goodput after BLER and headers), then
      for each offered rate (Mb/s, or "x1.2": 1.2 times that capacity) a
      UDP flow of SECS seconds (default 30) with 4 pings a second. RTT
      (p50/p90/max) and loss count only after WARM seconds (default 10,
      for a deep queue to fill): the ping loss and the UDP loss the core
      saw in those seconds. Plus the eNB's view of UE 0.

Needs LOGS pointing at the cell's logs. iperf3 servers are stopped by
their PID (never by name: this PC runs its own iperf3 service)."""
import json
import os
import re
import statistics as st
import subprocess
import sys
import time
import xmlrpc.client

from perfil import iperf_server, kill_pidfile, load, report_since
from perfil import ours as perfil_ours

LOGS = os.environ.get("LOGS", "/tmp/celda")
ch = xmlrpc.client.ServerProxy("http://127.0.0.1:8765")


def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout).stdout


def enb_view(rep):
    ues = []
    for rnti, us in sorted(rep.items()):
        ues.append(dict(rnti=rnti, snr=round(st.median(u["ul_snr"] for u in us), 1),
                        mcs=round(st.median(u["ul_mcs"] for u in us), 1),
                        bler=round(st.median(u["ul_bler"] for u in us), 3),
                        mbit=round(st.median(u["ul_bitrate"] for u in us) / 1e6, 2)))
    return ues


def udp(ns, port, mbit, secs, extra=""):
    return subprocess.Popen(f"sudo -n ip netns exec {ns} iperf3 -c 172.16.0.1 -p {port} -u -b {mbit}M -t {secs} -l 1200 -J {extra}",
                            shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def lost(p):
    try:
        return json.loads(p.communicate(timeout=60)[0])["end"]["sum"].get("lost_percent")
    except (ValueError, KeyError, subprocess.TimeoutExpired):
        return None


def pings(o):
    rtts = sorted(float(x) for x in re.findall(r"time=([\d.]+)", o))
    m = re.search(r"([\d.]+)% packet loss", o)
    return rtts, (float(m.group(1)) if m else 100.0)


def pct(xs, q):
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else None


def gains_vs_others(log):
    ours = [float(x) for x in (sys.argv[1] if len(sys.argv) > 1 else "1,0.3,0.15").split(",")]
    others = [float(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "0,5,10,20").split(",")]
    floor = float(sys.argv[3]) if len(sys.argv) > 3 else 300.0
    secs = 12
    ch.set_ul_noise(floor)
    ch.set_ul_gain(1.0, 1)  # the other users: a good signal
    for our in ours:
        ch.set_ul_gain(our, 0)
        for oth in others:
            time.sleep(3)
            for port in (5231, 5232):
                iperf_server(port, f"{LOGS}/iperf{port}.pid")
            t0 = time.time()
            bg = udp("ue2", 5232, oth, secs + 3) if oth > 0 else None
            time.sleep(1.5)
            fg = udp("ue1", 5231, 1, secs)
            o = sh(f"sudo -n ip netns exec ue1 ping -c {secs * 4} -i 0.25 -W 4 172.16.0.1", timeout=secs + 30)
            fg_loss, bg_loss = lost(fg), (lost(bg) if bg else None)
            for port in (5231, 5232):
                kill_pidfile(f"{LOGS}/iperf{port}.pid")
            rtts, ploss = pings(o)
            row = dict(our_gain=our, floor=floor, other_mbit=oth,
                       rtt=[pct(rtts, .5), pct(rtts, .9), rtts[-1]] if rtts else None,
                       ping_loss=ploss, our_udp_loss=fg_loss, other_udp_loss=bg_loss, enb=enb_view(report_since(t0)))
            print(json.dumps(row), flush=True)
            log.write(json.dumps(row) + "\n")
    ch.set_ul_noise(0)
    ch.set_ul_gain(1.0, 0)


def capacity(secs=20, warm=8):
    """UE 0's uplink capacity now: what the core got from a UDP flood once
    our queue was full (after warm seconds; Mb/s, IP goodput), then a pause
    for the queue to drain."""
    for _ in range(3):
        got = flow(40, secs, warm, ping=False)[3]
        time.sleep(6)
        if got is not None:
            return got
    return None


def rlfs():
    """Failed scheduling requests and radio link failures UE 0 logged so
    far (at the cliff they come now and then: the UE drops and reconnects
    in ~1-2 s)."""
    out = sh(f"sudo -n cat {LOGS}/ue0.out")
    return out.count("Radio-Link Failure") + out.count("Scheduling request failed")


def flow(mbit, secs, warm, ping=True):
    """UE 0 sends UDP at mbit for secs with 4 pings a second; RTT and loss
    after warm seconds."""
    iperf_server(5231, f"{LOGS}/iperf5231.pid")
    t0 = time.time()
    p = udp("ue1", 5231, mbit, secs, "--get-server-output")
    time.sleep(warm)
    o = sh(f"sudo -n ip netns exec ue1 ping -c {int((secs - warm - 2) * 4)} -i 0.25 -W 8 172.16.0.1", timeout=secs + 40) if ping else ""
    try:
        d = json.loads(p.communicate(timeout=secs + 40)[0])
        iv = d["server_output_json"]["intervals"]
        tail = [x["sum"] for x in iv if x["sum"]["start"] >= warm]
        pk = sum(x.get("packets", 0) for x in tail)
        udp_loss = round(100 * sum(x.get("lost_packets", 0) for x in tail) / pk, 1) if pk else None
        got = round(st.mean(x["bits_per_second"] for x in tail) / 1e6, 2) if tail else None
    except (ValueError, KeyError, subprocess.TimeoutExpired):
        udp_loss = got = None
    kill_pidfile(f"{LOGS}/iperf5231.pid")
    rtts, ploss = pings(o)
    return rtts, ploss, udp_loss, got, report_since(t0 + warm)


def rates_sweep(log):
    name = os.environ.get("PERFIL")
    if not name:
        sys.exit("escenario.py tasa: set PERFIL (the profile the cell was started with)")
    prof = load(name)
    rates = (sys.argv[2] if len(sys.argv) > 2 else "x0.8,x1.1,x1.25").split(",")
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    secs = int(os.environ.get("SECS", "30"))
    warm = int(os.environ.get("WARM", "10"))
    for rep in range(1, n + 1):
        cap = capacity()
        for rate in rates:
            mbit = round(float(rate[1:]) * cap, 2) if rate.startswith("x") and cap else float(rate.lstrip("x"))
            # a point whose iperf3 failed (its control connection rides the
            # same full queue, or the UE dropped) is run again, up to 3 times
            r0, tries = rlfs(), 0
            while tries < 3:
                tries += 1
                rtts, ploss, udp_loss, got, rp = flow(mbit, secs, warm)
                if got is not None:
                    break
                time.sleep(8)
            ours = enb_view({0: perfil_ours(rp)}) if perfil_ours(rp) else []
            row = dict(perfil=name, rep=rep, cap_mbit=cap and round(cap, 2), rate=rate, offered_mbit=mbit,
                       load=round(mbit / cap, 2) if cap else None, got_mbit=got,
                       rtt=[pct(rtts, .5), pct(rtts, .9), rtts[-1]] if rtts else None,
                       ping_loss=ploss, udp_loss=udp_loss, enb=ours[0] if ours else None, tries=tries, rlf=rlfs() - r0,
                       queue_bytes=os.environ.get("QUEUE_BYTES") or prof.get("queue_bytes"), secs=secs, warm=warm)
            print(json.dumps(row), flush=True)
            log.write(json.dumps(row) + "\n")
            log.flush()
            # let our queue drain before the next rate
            q0 = (prof.get("queue_bytes") or [0])[0] or 256 * 1200
            time.sleep(max(4.0, q0 * 8 / ((cap or 1) * 1e6) + 2))


def main():
    log = open(os.path.join(LOGS, "escenario.jsonl"), "a")
    if len(sys.argv) > 1 and sys.argv[1] == "tasa":
        rates_sweep(log)
    else:
        gains_vs_others(log)


if __name__ == "__main__":
    main()
