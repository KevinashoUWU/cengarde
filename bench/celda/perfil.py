#!/usr/bin/env python3
"""perfil.py: the virtual cell's calibrated profiles (perfiles.json) and the
helpers the other scripts share.

  perfil.py env NAME      shell defaults for celda.sh (PRB, EARFCN, NUE,
                          QUEUE_BYTES), as ": ${X:=v}" lines to eval
  perfil.py aplicar NAME  the channel (the eNB's floor, each UE's gain) and
                          the other users' load (UEs 1.., UDP to the core,
                          steady or on/off), with the cell up
  perfil.py quitar        stops the other users' load and its iperf3
                          servers, by their PID files in $LOGS
  perfil.py lista         the profiles and what each one stands for

A profile: prb, earfcn, nue, queue_bytes (per UE, 0: srsRAN's own 256 SDUs),
floor (the eNB's noise), gain (per UE), otros (per other UE: mbit, and
on_s/off_s for an on/off load with exponential periods; 0: steady) and
camino_ms (a fixed delay on the core's side: the path to the VPS). Only
this PC: the cell is single-instance (fixed ZMQ ports)."""
import json
import os
import random
import signal
import subprocess
import sys
import time
import xmlrpc.client

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.environ.get("LOGS", "/tmp/celda")
CORE = "172.16.0.1"
OTROS_PORT = 5240  # other UE k (k >= 1) sends to port 5240 + k
# our own servers use 5231-5232 (escenario.py, calibrar.py) and 5290
# (cengarde.py): 5201 is the iperf3 service of this PC, listening on all
# addresses


def load(name):
    with open(os.path.join(HERE, "perfiles.json")) as f:
        prof = json.load(f)
    if name not in prof:
        sys.exit(f"perfil: no profile {name!r} (have: {', '.join(k for k in prof if not k.startswith('_'))})")
    return prof[name]


def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout).stdout


def iperf_server(port, pidfile, once=True, root=False, bind=CORE):
    """An iperf3 server on bind:port, daemonized, its PID in pidfile (so
    that it is stopped by PID, never by name: this PC runs its own iperf3
    service)."""
    kill_pidfile(pidfile, root)
    sh(f"{'sudo -n ' if root else ''}iperf3 -s -J -B {bind} -p {port} -D {'-1 ' if once else ''}-I {pidfile}")
    for _ in range(20):
        time.sleep(0.1)
        if os.path.exists(pidfile):
            return
    raise RuntimeError(f"iperf3 server on {bind}:{port} did not start (port taken?)")


def kill_pidfile(pidfile, root=False):
    """Stops the process whose PID is in pidfile (if it is still there)."""
    try:
        pid = int(open(pidfile).read().strip())
    except (OSError, ValueError):
        return
    sh(f"{'sudo -n ' if root else ''}kill {pid} 2>/dev/null")
    for _ in range(20):
        if not os.path.exists(f"/proc/{pid}"):
            break
        time.sleep(0.1)
    sh(f"{'sudo -n ' if root else ''}rm -f {pidfile}")


def report_since(t0, logs=None):
    """The eNB's metrics per RNTI written after t0: its JSON report is a
    stream of objects, with -inf/nan for a UE that sends nothing (not JSON:
    read as Python's floats; a broken object is skipped, not the end)."""
    txt = sh(f"sudo -n cat {logs or LOGS}/enb_report.json")
    txt = txt.replace(": -inf", ": -Infinity").replace(": inf", ": Infinity").replace(": nan", ": NaN").replace(": -nan", ": NaN")
    dec, i, out = json.JSONDecoder(), 0, {}
    while True:
        j = txt.find("{\n  \"type\"", i)
        if j < 0:
            break
        try:
            obj, i = dec.raw_decode(txt, j)
        except ValueError:
            i = j + 1
            continue
        if obj.get("type") == "metrics" and obj.get("timestamp", 0) >= t0:
            for c in obj.get("cell_list", []):
                for u in c["cell_container"].get("ue_list", []):
                    u = u["ue_container"]
                    out.setdefault(u["ue_rnti"], []).append(u)
    return out


def ours(rep):
    """UE 0's metrics in a report_since() result. The report has RNTIs, not
    IMSIs: UE 0 is taken as the active UE with the lowest uplink SNR (the
    profiles and the scenarios give it the weakest signal; the other users
    have gain 1). [] if no UE sent anything."""
    act = {k: v for k, v in rep.items() if any(u.get("ul_bitrate", 0) > 0 for u in v)}
    if not act:
        return []
    return act[min(act, key=lambda k: sorted(u["ul_snr"] for u in act[k])[len(act[k]) // 2])]


def channel(p):
    ch = xmlrpc.client.ServerProxy("http://127.0.0.1:8765")
    ch.set_ul_noise(p.get("floor", 300))
    for k, g in enumerate(p.get("gain", [1.0])):
        ch.set_ul_gain(g, k)
    return ch


def camino(ms):
    """The path from the core to the VPS: a fixed delay (netem, no loss, no
    rate) on the core's side of the cell (srs_spgw_sgi, toward the UEs), so
    that an idle RTT is the field's (~50 ms) and not the cell's (~15-20)."""
    if ms > 0:
        sh(f"sudo -n tc qdisc replace dev srs_spgw_sgi root netem delay {ms}ms limit 100000")
    else:
        sh("sudo -n tc qdisc del dev srs_spgw_sgi root 2>/dev/null")


def aplicar(name):
    p = load(name)
    channel(p)
    camino(p.get("camino_ms", 0))
    quitar()
    for k, o in enumerate(p.get("otros", []), start=1):
        if not o or not o.get("mbit"):
            continue
        port = OTROS_PORT + k
        iperf_server(port, f"{LOGS}/otros{k}-srv.pid", once=False)
        # the load runs as root in the UE's netns; sh writes its PID and
        # becomes the runner, so it is stopped by PID
        pid = f"{LOGS}/otros{k}.pid"
        subprocess.Popen(f"sudo -n ip netns exec ue{k + 1} sh -c 'echo $$ > {pid}; exec python3 {HERE}/perfil.py _otros "
                         f"{port} {o['mbit']} {o.get('on_s', 0)} {o.get('off_s', 0)}'", shell=True,
                         stdout=open(f"{LOGS}/otros{k}.log", "a"), stderr=subprocess.STDOUT)
    time.sleep(1)
    print(f"perfil {name}: floor {p.get('floor')}, gains {p.get('gain')}, others {p.get('otros', [])}, "
          f"path {p.get('camino_ms', 0)} ms")


def quitar():
    k = 1
    while k < 16:
        kill_pidfile(f"{LOGS}/otros{k}.pid", root=True)
        kill_pidfile(f"{LOGS}/otros{k}-srv.pid")
        k += 1


def otros(port, mbit, on_s, off_s):
    """Runs inside the other UE's netns: UDP to the core at mbit, steady or
    on/off (exponential periods of mean on_s and off_s), until SIGTERM."""
    child = [None]

    def stop(*_):
        if child[0] and child[0].poll() is None:
            child[0].terminate()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    rnd = random.Random(port)
    while True:
        secs = max(1, round(rnd.expovariate(1 / on_s))) if on_s > 0 else 3600
        child[0] = subprocess.Popen(["iperf3", "-c", CORE, "-p", str(port), "-u", "-b", f"{mbit}M", "-l", "1200",
                                     "-t", str(secs)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        child[0].wait()
        if off_s > 0:
            time.sleep(rnd.expovariate(1 / off_s))
        else:
            time.sleep(0.5)  # the server needs a moment between tests


def env(name):
    p = load(name)
    q = ",".join(str(x) for x in p.get("queue_bytes", [0]))
    for k, v in (("PRB", p.get("prb", 50)), ("EARFCN", p.get("earfcn", 3350)), ("NUE", p.get("nue", 1)),
                 ("QUEUE_BYTES", q), ("FLOOR", p.get("floor", 300))):
        print(f": ${{{k}:={v}}}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "lista"
    if cmd == "env":
        env(sys.argv[2])
    elif cmd == "aplicar":
        aplicar(sys.argv[2])
    elif cmd == "quitar":
        quitar()
    elif cmd == "_otros":
        otros(int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]))
    elif cmd == "lista":
        with open(os.path.join(HERE, "perfiles.json")) as f:
            for k, v in json.load(f).items():
                if not k.startswith("_"):
                    print(f"{k}: {v.get('descripcion', '')}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
