#!/usr/bin/env python3
"""cengarde.py: cengarde over the virtual LTE cell, against its SNR cliff.

The client runs in netns ue1 (UE 0) with two links: "lte" over tun_srsue
(the cell, to the core's 172.16.0.1) and "fijo" over a veth pair to the
root netns (fijo in ue1, 10.80.0.2; fijo-r in root, 10.80.0.1) with netem
on both sides. The server runs in the root netns and listens on both
addresses. Native tunnel: TUN cgcel, 10.81.0.2 (client) and 10.81.0.1
(server); 10.79/16 and port 59402 are left to the field client on this PC.

  cengarde.py up [redund|bond]   veth, configs and both engines (for a look)
  cengarde.py down               engines, veth and iperf3 servers away
  cengarde.py run ["GAIN,..."] ["MODE,..."] [N]
                                 the scenario (defaults 1,0.15,0.1,0.07,0.05,
                                 redund,bond and 1)

Per point (mode, UE 0's uplink gain, repetition): the engines start fresh
with the gain at 1.0 and settle; TCP upload (iperf3, cubic) through the
tunnel and pings inside it start; after PRE seconds the gain drops to the
point's value, and the run goes on for SECS seconds in all. Every 0.5 s it
samples the client's and the server's status (per link: state, upload
active/muted, RTT, capacity, the server's view of it: first and rx) and at
the end reads the eNB's view of UE 0 (ul_snr, ul_mcs, ul_bler) since the
drop. One JSON line per point to stdout and $LOGS/cengarde.jsonl; a table
at the end. A UE that detaches is brought back by restarting the cell.

Needs the cell up (celda.sh start, NUE=1, LOGS as here), iperf3, ping, tc
with netem and sudo -n. Knobs (environment): LOGS (/tmp/celda), PERFIL (the
cell started as a profile of perfiles.json: its floor and UE 0's gain are
the defaults of FLOOR and BASE_GAIN, the gain before the drop, and a
restart brings the profile back), PRB (for a cell restart, 50), FLOOR (the
eNB's noise floor, 300), BASE_GAIN (1), SECS (20), PRE (3),
RET (seconds from the start at which the gain goes back up, the return
phase; 0: none), RET_GAIN (the gain then, BASE_GAIN; the tail is then the TCP up to
RET, and "ret" says what the LTE link did after it: its capacity, share,
RTT, when it carried again, and the TCP), SETTLE (6), ARMS (more modes,
"name=base:client extra|server extra" separated by "/", base one of MODES,
e.g. "redq=redund:min_active_links = 1|min_active_links = 1"),
FIJO_NETEM (netem on each direction of fijo: "delay 15ms rate 10mbit loss
1% limit 100", ~110 ms of queue), FIJO_TYPE (fijo's link type, auto),
CENGARDE_BIN (engine/cengarde), CLIENT_EXTRA and SERVER_EXTRA
("k = v;..." added to both modes)."""
import json
import os
import re
import statistics as st
import subprocess
import sys
import threading
import time
import xmlrpc.client

import perfil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOGS = os.environ.get("LOGS", "/tmp/celda")
RUN = os.path.join(LOGS, "cg")
BIN = os.environ.get("CENGARDE_BIN", os.path.join(REPO, "engine", "cengarde"))
# PERFIL: a profile of perfiles.json the cell was started with (its floor,
# UE 0's gain as the base before the drop, its NUE for a restart)
PERFIL = os.environ.get("PERFIL", "")
PROF = perfil.load(PERFIL) if PERFIL else {}
FLOOR = float(os.environ.get("FLOOR", PROF.get("floor", 300)))
BASE_GAIN = float(os.environ.get("BASE_GAIN", (PROF.get("gain") or [1.0])[0]))
SECS = int(os.environ.get("SECS", "20"))
PRE = float(os.environ.get("PRE", "3"))
RET = float(os.environ.get("RET", "0"))
RET_GAIN = float(os.environ.get("RET_GAIN", BASE_GAIN))
SETTLE = float(os.environ.get("SETTLE", "6"))
FIJO_NETEM = os.environ.get("FIJO_NETEM", "delay 15ms rate 10mbit loss 1% limit 100")
FIJO_TYPE = os.environ.get("FIJO_TYPE", "auto")
KEY = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA="  # lab-only shared key
PORT = 59480
LTE, FIJO = "tun_srsue", "fijo"
IPERF_PID = os.path.join(RUN, "iperf.pid")  # stopped by PID: 5201 is this PC's iperf3 service
NAMES = {LTE: "lte", FIJO: "fijo"}

# What each mode adds at both ends. bond is the field profile (the testnet's
# client and VPS, 2026-10-18); redund is the engine's defaults.
MODES = {
    "redund": ("", ""),
    "bond": ("split = on;split_eta = yes;capacity_weights = yes;split_spread_ms = 250;split_spread_copy = no;"
             "rescue_wait_ms = 60;rescue_kb = 32768;probe_idle_ms = 100;mute_behind_ms = 0;reorder_max_wait_ms = 500",
             "split_eta = yes;capacity_weights = yes;governor = yes;split_spread_ms = 250;split_spread_copy = no;"
             "rescue_wait_ms = 60;rescue_kb = 32768;reorder_margin_ms = 10"),
}
for _a in filter(None, os.environ.get("ARMS", "").split("/")):
    _n, _rest = _a.split("=", 1)
    _base, _ex = _rest.split(":", 1)
    _cx, _sx = (_ex.split("|", 1) + [""])[:2]
    MODES[_n.strip()] = (";".join(x for x in (MODES[_base.strip()][0], _cx) if x),
                         ";".join(x for x in (MODES[_base.strip()][1], _sx) if x))


def sh(cmd, timeout=60, check=False):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode:
        raise RuntimeError(f"{cmd}: {r.stderr.strip()}")
    return r.stdout


def ns(cmd):
    return f"sudo -n ip netns exec ue1 {cmd}"


def conf_lines(extra):
    return "".join(x.strip() + "\n" for x in extra.split(";") if x.strip())


def write_confs(mode):
    cx, sx = MODES[mode]
    os.makedirs(RUN, exist_ok=True)
    with open(os.path.join(RUN, "server.conf"), "w") as f:
        f.write(f"mode = server\nkey = {KEY}\nlisten = *:{PORT}\ntunnel = native\ntun = cgcel\n"
                f"tun_addr = 10.81.0.1/24\nstatus_file = {RUN}/server.json\nstatus_interval_ms = 500\n")
        f.write(conf_lines(sx) + conf_lines(os.environ.get("SERVER_EXTRA", "")))
    with open(os.path.join(RUN, "client.conf"), "w") as f:
        f.write(f"mode = client\nkey = {KEY}\nserver = 172.16.0.1:{PORT}\ntunnel = native\ntun = cgcel\ntun_addr = 10.81.0.2/24\n"
                f"interfaces = none\nstatus_file = {RUN}/client.json\nstatus_interval_ms = 500\n")
        f.write(conf_lines(cx) + conf_lines(os.environ.get("CLIENT_EXTRA", "")))
        f.write(f"[link {LTE}]\nserver = 172.16.0.1:{PORT}\nlabel = lte\ntype = cellular\n")
        f.write(f"[link {FIJO}]\nserver = 10.80.0.1:{PORT}\nlabel = fijo\ntype = {FIJO_TYPE}\n")


def veth_up():
    sh("sudo -n ip -n ue1 link set lo up")
    if "fijo-r" not in sh("ip -br link show fijo-r 2>/dev/null"):
        sh("sudo -n ip link add fijo-r type veth peer name fijo netns ue1", check=True)
        sh("sudo -n ip addr add 10.80.0.1/24 dev fijo-r", check=True)
        sh("sudo -n ip -n ue1 addr add 10.80.0.2/24 dev fijo", check=True)
        sh("sudo -n ip link set fijo-r up", check=True)
        sh("sudo -n ip -n ue1 link set fijo up", check=True)
    sh(f"sudo -n tc qdisc replace dev fijo-r root netem {FIJO_NETEM}", check=True)
    sh(f"sudo -n ip netns exec ue1 tc qdisc replace dev fijo root netem {FIJO_NETEM}", check=True)


def engines_stop():
    for who in ("client", "server"):
        p = os.path.join(RUN, who + ".pid")
        if os.path.exists(p):
            sh(f"sudo -n kill {open(p).read().strip()}")
            os.remove(p)
    time.sleep(0.5)
    sh(f"sudo -n rm -f {RUN}/client.json {RUN}/server.json")


def engines_start(mode):
    engines_stop()
    write_confs(mode)
    for who, pre in (("server", ""), ("client", "ip netns exec ue1 ")):
        # sh writes its own PID and becomes the engine: the PID to kill
        subprocess.Popen(f"sudo -n {pre}sh -c 'echo $$ > {RUN}/{who}.pid; exec {BIN} -c {RUN}/{who}.conf'",
                         shell=True, stdout=open(f"{RUN}/{who}.log", "a"), stderr=subprocess.STDOUT)
        time.sleep(0.3)


def status(who):
    try:
        return json.loads(sh(f"sudo -n cat {RUN}/{who}.json"))
    except ValueError:
        return None


def link_view(c, s):
    """Per link: what the client and the server say of it (the server's
    newest session)."""
    out = {}
    sess = None
    if s:
        ss = [x for x in s.get("sessions", []) if x.get("newest")] or s.get("sessions", [])
        sess = ss[0] if ss else None
    for ln in (c or {}).get("links", []):
        n = NAMES.get(ln.get("name"), ln.get("name"))
        sv = ln.get("server_view") or {}
        cap = ln.get("cap_diag") or {}
        out[n] = dict(state=ln.get("state"), up=ln.get("upload"), mutes=ln.get("upload_mutes"),
                      behind_ms=ln.get("upload_behind_ms"), late_ms=ln.get("upload_late_ms"),
                      expired=cap.get("expired"), restored=cap.get("restore_rest"), full_age=cap.get("full_age_ms"),
                      best=cap.get("best_mbit"),
                      rtt_ms=ln.get("rtt_ms"), cap=ln.get("capacity_mbit"), queue_ms=ln.get("queue_ms"),
                      share=ln.get("split_share"), tx=ln.get("tx_packets"),
                      sv_first=sv.get("first"), sv_rx=sv.get("rx"))
    if sess is not None:
        out["_srv"] = dict(lost_all=sess.get("lost_all"))
    return out


def report_since(t0):
    """The eNB's metrics per RNTI since t0 (perfil.report_since)."""
    return perfil.report_since(t0, LOGS)


def ue_attached():
    # with several UEs, UE 0 is not always 172.16.0.2
    return "172.16.0." in sh("sudo -n ip -n ue1 -br addr show tun_srsue 2>/dev/null")


def cell_restart():
    env = dict(os.environ, LOGS=LOGS, NUE=str(PROF.get("nue", 1)))
    subprocess.run([os.path.join(HERE, "celda.sh"), "stop"], env=env, capture_output=True)
    subprocess.run([os.path.join(HERE, "celda.sh"), "start"], env=env, capture_output=True, timeout=120)
    time.sleep(2)
    ch().set_ul_noise(FLOOR)
    ch().set_ul_gain(BASE_GAIN, 0)


def ch():
    return xmlrpc.client.ServerProxy("http://127.0.0.1:8765")


def pct(xs, q):
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else None


def one(mode, gain, rep):
    if not ue_attached():
        cell_restart()
    ch().set_ul_noise(FLOOR)
    ch().set_ul_gain(BASE_GAIN, 0)
    engines_start(mode)
    time.sleep(SETTLE)
    perfil.iperf_server(5290, IPERF_PID, bind="10.81.0.1")
    t_start = time.time()
    tcp = subprocess.Popen(ns(f"iperf3 -c 10.81.0.1 -p 5290 -t {SECS} -C cubic -J"), shell=True,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    png = subprocess.Popen(ns(f"ping -D -i 0.2 -W 3 -w {SECS} 10.81.0.1"), shell=True, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True)
    samples, t_drop, t_ret = [], None, None
    while time.time() - t_start < SECS + 1:
        t = time.time() - t_start
        if t_drop is None and t >= PRE:
            ch().set_ul_gain(gain, 0)
            t_drop = time.time()
        if RET and t_ret is None and t >= RET:
            ch().set_ul_gain(RET_GAIN, 0)
            t_ret = time.time()
        samples.append((round(t, 1), link_view(status("client"), status("server"))))
        time.sleep(0.5)
    tcp_out = tcp.communicate(timeout=60)[0]
    png_out = png.communicate(timeout=30)[0]
    srv_end = status("server")
    perfil.kill_pidfile(IPERF_PID)
    ch().set_ul_gain(BASE_GAIN, 0)
    engines_stop()
    try:
        d = json.loads(tcp_out)
        mbit = round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 2)
        retr = d["end"]["sum_sent"].get("retransmits")
        ivals = [round(x["sum"]["bits_per_second"] / 1e6, 1) for x in d["intervals"]]
        tail = round(st.mean(ivals[int(PRE) + 3:int(RET) if RET else None]), 2) if len(ivals) > PRE + 3 else None
    except (ValueError, KeyError):
        mbit = retr = tail = None
        ivals = []
    # pings after the drop only, by their -D timestamps
    rtts, sent_after = [], 0
    for m in re.finditer(r"\[([\d.]+)\].*icmp_seq=(\d+).*time=([\d.]+)", png_out):
        if float(m.group(1)) >= t_drop and (t_ret is None or float(m.group(1)) < t_ret):
            rtts.append(float(m.group(3)))
    sent_after = int(((RET or SECS) - PRE) / 0.2)
    rtts.sort()
    rep_e = report_since(t_drop + 1)
    enb = None
    if rep_e:
        us = perfil.ours(rep_e) or max(rep_e.values(), key=len)
        enb = dict(snr=round(st.median(u["ul_snr"] for u in us), 1), mcs=round(st.median(u["ul_mcs"] for u in us), 1),
                   bler=round(st.median(u["ul_bler"] for u in us), 3),
                   mbit=round(st.median(u["ul_bitrate"] for u in us) / 1e6, 2))
    # the LTE link after the drop: when it first left the upload (muted or not
    # live, or out of the split share), how much of the time it was out, and
    # its last capacity and RTT
    after = [(t, v.get("lte", {})) for t, v in samples if t >= PRE and (not RET or t < RET)]
    out_t = [t for t, l in after if l.get("up") == "muted" or l.get("state") != "live" or l.get("share") in ("no", "spread")]
    lte_last = after[-1][1] if after else {}
    # the guard: when the LTE link was first muted, and how often it went
    # from active to muted (flaps) after the drop
    muted_t = [t for t, l in after if l.get("up") == "muted"]
    ups = [l.get("up") for _, l in after]
    flaps = sum(1 for a, b in zip(ups, ups[1:]) if a == "active" and b == "muted")
    m0 = (after[0][1].get("mutes") or 0) if after else 0
    mutes = (after[-1][1].get("mutes") or 0) - m0 if after else 0
    # the return phase: the LTE link's capacity at the restore and its
    # first values after, its share of the sends, its queue, and the TCP
    ret = None
    if RET and t_ret is not None:
        back = [(t, v.get("lte", {})) for t, v in samples if t >= RET]
        rs = []
        for (t0_, a), (t1_, b) in zip(samples, samples[1:]):
            if t1_ >= RET:
                d = {n: (b.get(n, {}).get("tx") or 0) - (a.get(n, {}).get("tx") or 0) for n in ("lte", "fijo")}
                if d["lte"] + d["fijo"] > 0:
                    rs.append((d["lte"], d["lte"] + d["fijo"]))
        ret_iv = ivals[int(RET) + 2:] if len(ivals) > RET + 2 else []
        cap_at = [l.get("cap") for t, l in back]
        rtts_b = [l.get("rtt_ms") for t, l in back if l.get("rtt_ms") is not None]
        live_back = [t for t, l in back if l.get("state") == "live" and l.get("up") == "active"]
        ret = dict(cap=cap_at[:6], cap_end=cap_at[-1] if cap_at else None,
                   expired=(back[0][1].get("expired") if back else None),
                   share=round(sum(x for x, _ in rs) / sum(y for _, y in rs), 2) if rs else None,
                   lte_rtt_max=max(rtts_b) if rtts_b else None,
                   in_after_s=round(live_back[0] - RET, 1) if live_back else None,
                   tcp=round(st.mean(ret_iv), 2) if ret_iv else None)
    first = {n: (samples[-1][1].get(n, {}).get("sv_first") or 0) - (samples[0][1].get(n, {}).get("sv_first") or 0)
             for n in ("lte", "fijo")} if samples else {}
    caps = [l.get("cap") for _, l in after if l.get("cap") is not None]
    # what the client sent on LTE, as a share of both links' sends, per sample
    # after the drop and over all of it
    shares, t_low = [], None
    for (t0_, a), (t1_, b) in zip(samples, samples[1:]):
        d = {n: (b.get(n, {}).get("tx") or 0) - (a.get(n, {}).get("tx") or 0) for n in ("lte", "fijo")}
        tot = d["lte"] + d["fijo"]
        if t1_ >= PRE and tot > 0:
            shares.append((d["lte"], tot))
            if t_low is None and d["lte"] < 0.1 * tot:
                t_low = t1_
    lte_share = round(sum(x for x, _ in shares) / sum(y for _, y in shares), 2) if shares else None
    sess = None
    if srv_end:
        ss = [x for x in srv_end.get("sessions", []) if x.get("newest")]
        sess = ss[0] if ss else None
    row = dict(mode=mode, gain=gain, ret_at=RET, ret_gain=RET_GAIN, floor=FLOOR, rep=rep, secs=SECS, pre=PRE, fijo=FIJO_NETEM,
               tcp_mbit=mbit, tcp_tail_mbit=tail, retrans=retr, tcp_ivals=ivals,
               rtt=[pct(rtts, .5), pct(rtts, .9), rtts[-1] if rtts else None],
               ping_loss=round(100 * (1 - len(rtts) / sent_after), 1) if sent_after else None,
               lte_out_after_s=round(out_t[0] - PRE, 1) if out_t else None,
               lte_muted_after_s=round(muted_t[0] - PRE, 1) if muted_t else None,
               lte_muted_frac=round(len(muted_t) / len(after), 2) if after else None,
               flaps=flaps, mutes=mutes, ret=ret,
               lte_out_frac=round(len(out_t) / len(after), 2) if after else None,
               lte_tx_share=lte_share, lte_low_after_s=round(t_low - PRE, 1) if t_low is not None else None,
               lte_last=lte_last, lte_cap=[min(caps), max(caps), caps[-1]] if caps else None,
               sv_first=first, lost_all=(sess or {}).get("lost_all"), enb=enb,
               ue_detached=not ue_attached(), samples=samples)
    return row


def table(rows):
    """One line per point: the eNB's view of UE 0 after the drop, TCP, the
    pings in the tunnel, and what cengarde did with the LTE link (seconds
    to muted or not live and the time out; its share of the sends and
    seconds to under 10 %; its last capacity; the copies the server took
    first from each link)."""
    print("\nmode    gain   SNR  MCS  BLER  TCP Mb/s (tail) retr  RTT p50/p90 ms  ping%  out s/frac  share low s    cap  first lte/fijo")
    for r in rows:
        e, rt, f = r.get("enb") or {}, r["rtt"], r.get("sv_first") or {}
        print(f"{r['mode']:7s} {r['gain']:5.2f} {e.get('snr', '-')!s:>5} {e.get('mcs', '-')!s:>4} {e.get('bler', '-')!s:>5} "
              f"{r['tcp_mbit']!s:>6} ({r['tcp_tail_mbit']!s:>5}) {r['retrans']!s:>4}  {rt[0]!s:>6}/{rt[1]!s:<6} "
              f"{r['ping_loss']!s:>5}  {r['lte_out_after_s']!s:>4}/{r['lte_out_frac']!s:<5} {r['lte_tx_share']!s:>5} "
              f"{r['lte_low_after_s']!s:>4}  {(r['lte_cap'] or [None] * 3)[2]!s:>5}  {f.get('lte', '-')}/{f.get('fijo', '-')}")


def down():
    engines_stop()
    perfil.kill_pidfile(IPERF_PID)
    sh("sudo -n ip link del fijo-r 2>/dev/null")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd == "down":
        down()
        return
    if not ue_attached():
        sys.exit("cengarde.py: UE 0 is not attached (celda.sh start, LOGS=%s)" % LOGS)
    veth_up()
    if cmd == "up":
        ch().set_ul_noise(FLOOR)
        ch().set_ul_gain(BASE_GAIN, 0)
        engines_start(sys.argv[2] if len(sys.argv) > 2 else "redund")
        return
    gains = [float(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1,0.15,0.1,0.07,0.05").split(",")]
    modes = (sys.argv[3] if len(sys.argv) > 3 else "redund,bond").split(",")
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    rows = []
    log = open(os.path.join(LOGS, "cengarde.jsonl"), "a")
    try:
        for rep in range(1, n + 1):
            for gain in gains:
                for mode in modes:
                    r = one(mode, gain, rep)
                    rows.append(r)
                    log.write(json.dumps(r) + "\n")
                    log.flush()
                    print(json.dumps({k: v for k, v in r.items() if k != "samples"}), flush=True)
    finally:
        try:
            ch().set_ul_gain(BASE_GAIN, 0)
            ch().set_ul_noise(FLOOR if PERFIL else 0)
        except OSError:
            pass
        down()
    table(rows)


if __name__ == "__main__":
    main()
