#!/usr/bin/env python3
"""proteger.py: a lossy primary (Starlink-like, netem on "fijo") protected by
a choked secondary (the virtual cell's LTE past its knee), in redundancy
(story 020): full copies, cut copies, partial copies or parity.

  proteger.py run "ARM,..." "udp,tcp" [N]   each arm, traffic kind and
                                            repetition (arms rotate)
  proteger.py arms                          the arms it knows

The setup is cengarde.py's (client in ue1 with "lte" over the cell and
"fijo" over a veth with netem; server in the root netns, native tunnel).
Here fijo is the primary: shorter delay, more capacity, and it loses
(FIJO_NETEM, by default "delay 5ms rate 30mbit loss 0.7% limit 150":
the cell's uplink takes ~10-20 ms one way, its 35 ms of path are on the
downlink, and the lossy link must be the faster one, as Starlink was); the
cell is started with a profile (PERFIL=wom or entel_b2: limited capacity,
deep queue). Upload only. Per run:

  udp  a constant-rate stream (cbr.py, RATE Mbit/s, 1200 B) through the
       tunnel: what is lost end to end (overall, inside the episode and
       outside it), its one-way delay, what the server counted lost on
       every link (lost_all) and rebuilt from parities (fec.recovered)
  tcp  iperf3 (cubic): throughput and retransmissions

and in both, pings inside the tunnel (p50/p99), the LTE link's probe RTT
and queue as the client sees it (its queue: what the copies cost), and
what each link carried (lte bytes per fijo byte: the data cost; fijo
carries every packet once). The episode (EP_AT s from the start, EP_SECS
long, 0: none) is a microcut: fijo's upload loses EP_LOSS % (story 017:
20-40 s at 50-77 %).

Knobs (environment): PERFIL (the cell's profile, as started), LOGS
(/tmp/celda), SECS (40), SETTLE (8), RATE (7, Mbit/s of the stream),
FIJO_NETEM, EP_AT (15), EP_SECS (10), EP_LOSS (60), FIJO_TYPE (auto),
CENGARDE_BIN, OUT (a JSONL to append to, LOGS/proteger.jsonl), and
ARMS_EXTRA ("name=client|server" separated by "/", more arms)."""
import json
import os
import re
import statistics as st
import subprocess
import sys
import time

os.environ.setdefault("FIJO_NETEM", "delay 5ms rate 30mbit loss 0.7% limit 150")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cengarde as cg  # noqa: E402  (reads FIJO_NETEM, PERFIL, LOGS)
import perfil  # noqa: E402

SECS = int(os.environ.get("SECS", "40"))
SETTLE = float(os.environ.get("SETTLE", "8"))
RATE = float(os.environ.get("RATE", "7"))
EP_AT = float(os.environ.get("EP_AT", "15"))
EP_SECS = float(os.environ.get("EP_SECS", "10"))
EP_LOSS = float(os.environ.get("EP_LOSS", "60"))
OUT = os.environ.get("OUT", os.path.join(cg.LOGS, "proteger.jsonl"))
CBR_PORT = 5295
CBR = os.path.join(HERE, "cbr.py")

# Each arm: what the client and the server add (redundancy, the engine's
# defaults otherwise). The window needs a pace: redund_pace_ms on every link
# (the starlink preset's pace_peers_ms is the same window, from Starlink's
# side; fijo stays auto here so that only the window differs).
ARMS = {
    "all": ("", ""),                                    # every copy, no window
    "full": ("redund_pace_ms = 250", ""),               # A: the field's 250
    "cut": ("redund_pace_ms = 60", ""),                 # B: the preset's 60
    "fec": ("fec = yes", "fec = yes"),                  # C: FEC v7 as it is
    "pcopy": ("redund_spare = copies", "fec = yes"),    # D: partial copies in the spare
    "parity": ("redund_spare = parity", "fec = yes"),   # E: parity in the spare
    "mix": ("redund_spare = mix", "fec = yes"),         # F: the mix
    "rs": ("redund_spare = rs", "fec = yes"),           # G: Reed-Solomon (Leopard) in the spare (story 021)
    "rlc": ("redund_spare = rlc", "fec = yes"),         # H: the sliding-window RLC in the spare (story 021)
}
for _a in filter(None, os.environ.get("ARMS_EXTRA", "").split("/")):
    _n, _rest = _a.split("=", 1)
    _c, _s = (_rest.split("|", 1) + [""])[:2]
    ARMS[_n.strip()] = (_c.strip(), _s.strip())


def status(who):
    try:
        return json.loads(cg.sh(f"sudo -n cat {cg.RUN}/{who}.json"))
    except ValueError:
        return None


def sample(t):
    c, s = status("client"), status("server")
    out = dict(t=round(t, 1))
    for ln in (c or {}).get("links", []):
        n = cg.NAMES.get(ln.get("name"), ln.get("name"))
        out[n] = dict(rtt=ln.get("rtt_ms"), q=ln.get("queue_ms"), cap=ln.get("capacity_mbit"),
                      txb=(ln.get("tx_bytes") or 0) + (ln.get("tx_fec_bytes") or 0), txp=ln.get("tx_packets"),
                      up=ln.get("upload"),
                      spare=(ln.get("spare") or {}).get("mbit"))
    if c:
        up = c.get("upload") or {}
        out["cfec"] = (up.get("fec") or c.get("fec") or {}).get("parities")
        out["spare"] = up.get("spare")
    if s:
        ss = [x for x in s.get("sessions", []) if x.get("newest")] or s.get("sessions", [])
        if ss:
            f = ss[0].get("fec") or {}
            rp = ss[0].get("repair") or {}
            out["srv"] = dict(lost_all=ss[0].get("lost_all"),
                              rec=(f.get("recovered") or 0) + (rp.get("recovered") or 0), par=f.get("parities"),
                              unrec=f.get("unrecovered"), there=f.get("already_there"), rep=rp)
    return out


def netem_fijo_client(extra):
    """fijo's upload (the client's side, in ue1) with FIJO_NETEM and extra."""
    base = re.sub(r"\bloss \S+", "", cg.FIJO_NETEM).strip()
    cg.sh(f"sudo -n ip netns exec ue1 tc qdisc replace dev fijo root netem {base} {extra}", check=True)


def base_loss():
    m = re.search(r"\bloss (\S+)", cg.FIJO_NETEM)
    return f"loss {m.group(1)}" if m else ""


def pct(xs, q):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else None


def one(arm, kind, rep):
    if not cg.ue_attached():
        cg.cell_restart()
    cg.MODES[arm] = ARMS[arm]
    cg.veth_up()
    cg.engines_start(arm)
    time.sleep(SETTLE)
    rxo = os.path.join(cg.RUN, "cbr.json")
    procs = []
    if kind == "udp":
        cg.sh(f"rm -f {rxo}")
        procs.append(subprocess.Popen(["python3", "-I", CBR, "rx", f"10.81.0.1:{CBR_PORT}", str(SECS), rxo]))
        time.sleep(0.3)
        procs.append(subprocess.Popen(cg.ns(f"python3 -I {CBR} tx 10.81.0.1:{CBR_PORT} {RATE} {SECS}"), shell=True))
        tcp = None
    else:
        perfil.iperf_server(5290, cg.IPERF_PID, bind="10.81.0.1")
        tcp = subprocess.Popen(cg.ns(f"iperf3 -c 10.81.0.1 -p 5290 -t {SECS} -C cubic -J"), shell=True,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    png = subprocess.Popen(cg.ns(f"ping -D -i 0.2 -W 3 -w {SECS} 10.81.0.1"), shell=True, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True)
    t0 = time.time()
    samples, ep_on, ep_off = [], None, None
    while time.time() - t0 < SECS + 1:
        t = time.time() - t0
        if EP_SECS and ep_on is None and t >= EP_AT:
            netem_fijo_client(f"loss {EP_LOSS}%")
            ep_on = time.time()
        if EP_SECS and ep_on is not None and ep_off is None and t >= EP_AT + EP_SECS:
            netem_fijo_client(base_loss())
            ep_off = time.time()
        samples.append(sample(t))
        time.sleep(0.5)
    if ep_on is not None and ep_off is None:
        netem_fijo_client(base_loss())
    png_out = png.communicate(timeout=30)[0]
    row = dict(arm=arm, kind=kind, rep=rep, perfil=cg.PERFIL, rate=RATE, secs=SECS, fijo=cg.FIJO_NETEM,
               ep=[EP_AT, EP_SECS, EP_LOSS] if EP_SECS else None, client=ARMS[arm][0], server=ARMS[arm][1])
    if tcp is not None:
        out = tcp.communicate(timeout=60)[0]
        perfil.kill_pidfile(cg.IPERF_PID)
        try:
            d = json.loads(out)
            row["tcp_mbit"] = round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 2)
            row["retrans"] = d["end"]["sum_sent"].get("retransmits")
            row["tcp_ivals"] = [round(x["sum"]["bits_per_second"] / 1e6, 1) for x in d["intervals"]]
            iv = row["tcp_ivals"]
            if EP_SECS:
                row["tcp_ep_mbit"] = round(st.mean(iv[int(EP_AT):int(EP_AT + EP_SECS)]), 2)
                row["tcp_out_mbit"] = round(st.mean(iv[2:int(EP_AT)] + iv[int(EP_AT + EP_SECS) + 2:]), 2)
        except (ValueError, KeyError, st.StatisticsError):
            row["tcp_mbit"] = row["retrans"] = None
    else:
        for p in procs:
            p.wait(timeout=60)
        try:
            r = json.load(open(rxo))
        except (OSError, ValueError):
            r = None
        if r:
            ls = r["lost_s"]
            pps = RATE * 1e6 / 8 / 1200
            row.update(sent=r["sent"], lost=r["lost"], lost_pct=r["lost_pct"], owd=r["owd"], dup=r["dup"],
                       late=r.get("late"))
            if not EP_SECS:
                lo = sorted(x for _, x in r.get("late_t", []))
                row["late_out"] = [len(lo), pct(lo, .5), pct(lo, .95), lo[-1] if lo else None]
            if EP_SECS:
                # the episode by the stream's send time, with a second each
                # side (the netem change and the stream's start are not to
                # the second) and the queue's drain after it
                a, b = max(0, int(EP_AT) - 1), int(EP_AT + EP_SECS) + 2
                ep = sum(ls[a:b])
                row["lost_ep"] = ep
                row["lost_ep_pct"] = round(100.0 * ep / (pps * EP_SECS), 3)
                rest = r["lost"] - ep
                row["lost_out_pct"] = round(100.0 * rest / max(1, r["sent"] - pps * (b - a)), 4)
                row["owd_ep_p99"] = pct(r["owd_s"][a:b], .5)
                # the late ones (rebuilt, or copies that came first on a
                # slower link) outside the episode: how much later (ms)
                lo = sorted(x for s, x in r.get("late_t", []) if not a <= s < b)
                row["late_out"] = [len(lo), pct(lo, .5), pct(lo, .95), lo[-1] if lo else None]
            row["lost_s"] = ls
            row["owd_s"] = r["owd_s"]
    rtts = sorted(float(m.group(1)) for m in re.finditer(r"time=([\d.]+)", png_out))
    sent_p = int(SECS / 0.2)
    row["ping"] = [pct(rtts, .5), pct(rtts, .99)]
    row["ping95"] = pct(rtts, .95)
    row["ping_loss"] = round(100.0 * (1 - len(rtts) / sent_p), 1)
    lte = [s.get("lte", {}) for s in samples if s.get("lte")]
    row["lte_rtt"] = [pct([x.get("rtt") for x in lte], .5), pct([x.get("rtt") for x in lte], .95),
                      max((x.get("rtt") or 0) for x in lte) if lte else None]
    row["lte_q"] = [pct([x.get("q") for x in lte], .5), max((x.get("q") or 0) for x in lte) if lte else None]
    row["lte_cap"] = pct([x.get("cap") for x in lte], .5)
    if len(samples) > 2:
        a, b = samples[1], samples[-1]

        def d(n, k):
            return ((b.get(n) or {}).get(k) or 0) - ((a.get(n) or {}).get(k) or 0)

        row["lte_bytes"], row["fijo_bytes"] = d("lte", "txb"), d("fijo", "txb")
        row["cost"] = round(row["lte_bytes"] / row["fijo_bytes"], 3) if row["fijo_bytes"] else None
        sv = (b.get("srv") or {})
        row["lost_all"] = sv.get("lost_all")
        row["fec_rec"], row["fec_par"], row["fec_unrec"] = sv.get("rec"), sv.get("par"), sv.get("unrec")
        row["client_parities"] = b.get("cfec")
        row["repair"] = sv.get("rep")
        row["spare_end"] = b.get("spare")
    cg.engines_stop()
    row["samples"] = samples
    return row


def table(rows):
    print("\narm     kind rep  lost%   ep%    out%   owd p50/p99  TCP  retr  ping p50/p99  lteRTT p50/p95/max  cost  rec/par")
    for r in rows:
        print(f"{r['arm']:7s} {r['kind']:4s} {r['rep']:3d} {r.get('lost_pct')!s:>6} {r.get('lost_ep_pct')!s:>6} "
              f"{r.get('lost_out_pct')!s:>7} {str((r.get('owd') or [None, None])[:2]):>12} {r.get('tcp_mbit')!s:>5} "
              f"{r.get('retrans')!s:>5} {str(r.get('ping')):>14} {str(r.get('lte_rtt')):>20} {r.get('cost')!s:>5} "
              f"{r.get('fec_rec')}/{r.get('fec_par')}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd == "arms":
        for k, (c, s) in ARMS.items():
            print(f"{k:8s} client: {c or '-'}  server: {s or '-'}")
        return
    if cmd == "down":
        cg.down()
        return
    if not cg.ue_attached():
        sys.exit("proteger.py: UE 0 is not attached (PERFIL=... celda.sh start)")
    arms = (sys.argv[2] if len(sys.argv) > 2 else "full,cut").split(",")
    kinds = (sys.argv[3] if len(sys.argv) > 3 else "udp").split(",")
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    for a in arms:
        if a not in ARMS:
            sys.exit(f"proteger.py: no arm {a!r}")
    rows = []
    log = open(OUT, "a")
    try:
        for rep in range(1, n + 1):
            k = (rep - 1) % len(arms)
            order = arms[k:] + arms[:k]  # rotated: drift hits every arm alike
            for kind in kinds:
                for a in order:
                    r = one(a, kind, rep)
                    rows.append(r)
                    log.write(json.dumps(r) + "\n")
                    log.flush()
                    print(json.dumps({x: v for x, v in r.items() if x not in ("samples", "lost_s", "owd_s", "tcp_ivals")}),
                          flush=True)
    finally:
        try:
            netem_fijo_client(base_loss())
        except RuntimeError:
            pass
        cg.down()
    table(rows)


if __name__ == "__main__":
    main()
