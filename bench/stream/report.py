#!/usr/bin/env python3
"""report.py: reads the run directories of send.sh (and recv.sh) and says how the
stream behaved through the tunnel (bench/stream/, standard library only).

  report.py SENDER_RUN [--rx RECEIVER_RUN] [--fieldrec EXPORT_DIR] [--json FILE]

SENDER_RUN is a directory made by send.sh; --rx is the one recv.sh made on the
server side; --fieldrec is a directory made by bench/fieldrec.py --export (from
cengarde-rec recordings of the router). Everything is aligned by wall clock, so
the PC, the server and the router must agree on the time (NTP): an offset
moves the stalls against the link events by exactly that much (--fieldrec-offset
corrects the router's, if you know it).

What it reports, each in its own section:
  sender   target and achieved bitrate over time, encoder speed under 1 and dropped
           frames, and how long the sender was held back (backpressure)
  tcp      (RTMP) retransmissions, RTT over its minimum, collapses of cwnd (a drop of
           more than half within a second), time limited by the send buffer
  srt      (SRT with send.sh -S / recv.sh -S) loss, retransmissions, drops (packets
           too late), RTT, buffer; the UDP socket's own queue and drops otherwise
  receiver stalls (gaps in the arrival of video packets), frames late against the
           sender's timeline, jumps in pts, out-of-order TCP segments
  link events (with --fieldrec) which stalls, collapses and spikes fall within
           --window seconds of an outage or a mute of a link, against what chance
           would give

Every time is wall clock; "+12.3 s" is seconds from the start of the run.
--json writes all the numbers; plots go to report.png in the run directory when
matplotlib is installed (--no-plots to skip them).
"""

import argparse
import bisect
import csv
import datetime
import gzip
import importlib.util
import json
import math
import os
import sys

sys.dont_write_bytecode = True  # no bench/stream/__pycache__ in the tree

# What counts as what, all of it overridable on the command line.
STALL_MS = 200        # receiver: no video packet for this long
LATE_MS = 500         # receiver: a frame this much later than the floor is late
HOLD_MS = 200         # sender: output time this far behind real time
RTT_SPIKE_MS = 100    # tcp: smoothed RTT this far over its minimum
SKIP_S = 2.0          # start-up left out of the rate and speed statistics
SKIP_END_S = 2.0      # receiver: the end of the stream (the sender stopping) left out too
WINDOW_S = 3.0        # link events: how near is near
COLLAPSE_RATIO = 0.5  # cwnd under this fraction of its last second's peak
COLLAPSE_PEAK_MIN = 10  # ... and that peak at least this many segments


# ---- small helpers ---------------------------------------------------------------------------

def fnum(s):
    """float or None ('', 'N/A', garbage)."""
    if s is None:
        return None
    s = s.strip()
    if not s or s in ("N/A", "nan"):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def pct(xs, p):
    """The p-th percentile (0 to 100) of a list, by linear interpolation; None for an empty list."""
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    lo = int(math.floor(k))
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def interp(ts, vs, x):
    """vs at time x, linear between samples (ts ascending), clamped at the ends."""
    i = bisect.bisect_left(ts, x)
    if i <= 0:
        return vs[0]
    if i >= len(ts):
        return vs[-1]
    t0, t1 = ts[i - 1], ts[i]
    if t1 == t0:
        return vs[i]
    return vs[i - 1] + (vs[i] - vs[i - 1]) * (x - t0) / (t1 - t0)


def hms(t):
    """UTC time of day of an epoch, to the millisecond."""
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    return d.strftime("%H:%M:%S.") + "%03d" % (d.microsecond // 1000)


def rel(t, t0):
    return "%+.1f s" % (t - t0)


def f1(x, nd=1, unit=""):
    return "-" if x is None else ("%.*f%s" % (nd, x, unit))


def fm(x):
    """Milliseconds: two decimals under 10, one above."""
    if x is None:
        return "-"
    return "%.2f" % x if abs(x) < 10 else "%.1f" % x


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", newline="")
    return open(path, "r", newline="")


def find_file(d, name):
    for n in (name, name + ".gz"):
        p = os.path.join(d, n)
        if os.path.isfile(p):
            return p
    return None


class Table:
    """A CSV read into columns: numbers as floats (None where missing), and the raw strings."""

    def __init__(self, path):
        self.names = []
        self.raw = {}
        self.num = {}
        self.n = 0
        with open_text(path) as fh:
            rd = csv.reader(fh)
            try:
                head = next(rd)
            except StopIteration:
                return
            idx = {}
            for i, h in enumerate(head):
                if h not in idx:  # srt-live-transmit has a column named Time twice: the first one wins
                    idx[h] = i
            self.names = list(idx)
            cols = {h: [] for h in idx}
            for row in rd:
                if len(row) < len(head):
                    continue
                for h, i in idx.items():
                    cols[h].append(row[i])
            self.raw = cols
            self.n = len(cols[head[0]]) if head else 0
            self.num = {h: [fnum(v) for v in vs] for h, vs in cols.items()}

    def has(self, name):
        return name in self.num and any(v is not None for v in self.num[name])

    def col(self, name):
        return self.num.get(name, [None] * self.n)


def span(vals):
    """Last minus first of the values a cumulative counter has (a row of a socket that is still connecting has none)."""
    xs = [x for x in vals if x is not None]
    return xs[-1] - xs[0] if len(xs) >= 2 else None


def episodes(ts, flags, pad):
    """Runs of true flags as (start, end, first index, last index); end is the last true sample plus pad."""
    out = []
    start = None
    for i, f in enumerate(flags):
        if f and start is None:
            start = i
        if start is not None and (not f or i == len(flags) - 1):
            last = i if f else i - 1
            out.append((ts[start], ts[last] + pad, start, last))
            start = None
    return out


def merge_intervals(iv, gap):
    """Intervals (a, b, value) sorted by a, merged when the next starts within gap of the end; values add up."""
    out = []
    for a, b, v in sorted(iv):
        if out and a <= out[-1][1] + gap:
            out[-1][1] = max(out[-1][1], b)
            out[-1][2] += v
        else:
            out.append([a, b, v])
    return [tuple(x) for x in out]


# ---- loading ---------------------------------------------------------------------------------

def load_meta(d):
    p = os.path.join(d, "meta.json")
    if os.path.isfile(p):
        try:
            with open(p) as fh:
                return json.load(fh)
        except (OSError, ValueError):
            pass
    return {}


def parse_speed(v):
    v = (v or "").strip().rstrip("x").strip()
    return fnum(v)


def load_progress(d):
    """ffmpeg -progress blocks, each stamped by send.sh/recv.sh with the wall clock of its last line."""
    p = find_file(d, "progress.log")
    out = []
    if not p:
        return out
    cur = {}
    with open_text(p) as fh:
        for line in fh:
            parts = line.strip().split(None, 1)
            if len(parts) < 2 or "=" not in parts[1]:
                continue
            t = fnum(parts[0])
            k, v = parts[1].split("=", 1)
            cur[k] = v.strip()
            if k == "progress" and t is not None:
                out.append({
                    "t": t, "frame": fnum(cur.get("frame")), "size": fnum(cur.get("total_size")),
                    "out_us": fnum(cur.get("out_time_us")), "speed": parse_speed(cur.get("speed")),
                    "drop": fnum(cur.get("drop_frames")), "dup": fnum(cur.get("dup_frames")),
                    "end": v.strip() == "end"})
                cur = {}
    return out


def iso_epoch(s):
    """srt-live-transmit's Timepoint: 2026-10-11T20:22:54.356700+0000."""
    try:
        return datetime.datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%f%z").timestamp()
    except ValueError:
        return None


class Run:
    """One run directory (sender or receiver) with what it recorded."""

    def __init__(self, d):
        self.dir = d
        self.meta = load_meta(d)
        self.kind = self.meta.get("kind") or ("receiver" if find_file(d, "packets.csv") else "sender")
        self.progress = load_progress(d)
        p = find_file(d, "tcp.csv")
        self.tcp = Table(p) if p else None
        p = find_file(d, "udp.csv")
        self.udp = Table(p) if p else None
        p = find_file(d, "srt-stats.csv")
        self.srt = Table(p) if p else None
        if self.srt and self.srt.n:
            self.srt.t = [iso_epoch(s) for s in self.srt.raw.get("Timepoint", [])]
        p = find_file(d, "packets.csv")
        self.packets = Table(p) if p else None
        self.start = self.meta.get("start_epoch")
        self.end = self.meta.get("end_epoch")
        if self.start is None and self.progress:
            self.start = self.progress[0]["t"]
        if self.end is None and self.progress:
            self.end = self.progress[-1]["t"]


# ---- sender analysis -------------------------------------------------------------------------

def analyse_sender(run, a):
    """Bitrate, speed, held-back episodes and the burned clock's offset, from ffmpeg's -progress."""
    res = {"available": False}
    pr = [p for p in run.progress if p["out_us"] is not None and p["size"] is not None and p["t"] is not None]
    if len(pr) < 3:
        return res, []
    res["available"] = True
    t0 = run.start if run.start is not None else pr[0]["t"]
    ts = [p["t"] for p in pr]
    size = [p["size"] for p in pr]
    out_s = [p["out_us"] / 1e6 for p in pr]
    first, last = ts[0], ts[-1]
    target = run.meta.get("target_bitrate_bps")
    res["target_mbps"] = target / 1e6 if target else None
    dur = last - first
    res["seconds"] = dur
    res["mean_mbps"] = (size[-1] - size[0]) * 8 / dur / 1e6 if dur > 0 else None
    # 1 s bins (after the start-up), by the size and the output time at their edges
    b0 = first + a.skip
    nb = int((last - b0) // 1.0)
    bins = []
    for k in range(max(nb, 0)):
        x0, x1 = b0 + k, b0 + k + 1
        mbps = (interp(ts, size, x1) - interp(ts, size, x0)) * 8 / 1e6
        speed = interp(ts, out_s, x1) - interp(ts, out_s, x0)
        bins.append((x0, mbps, speed))
    res["bin_series"] = [(b[0], b[1]) for b in bins]
    mb = [b[1] for b in bins]
    res["bitrate_p5_mbps"] = pct(mb, 5)
    res["bitrate_p50_mbps"] = pct(mb, 50)
    res["bitrate_p95_mbps"] = pct(mb, 95)
    res["bins_below_half_target"] = sum(1 for m in mb if target and m < 0.5 * target / 1e6)
    res["bins"] = len(bins)
    sp = [b[2] for b in bins]
    res["speed_final"] = next((p["speed"] for p in reversed(pr) if p["speed"] is not None), None)
    res["speed_min_1s"] = min(sp) if sp else None
    res["bins_speed_below_0_95"] = sum(1 for s in sp if s < 0.95)
    res["drop_frames"] = int(pr[-1]["drop"] or 0)
    res["dup_frames"] = int(pr[-1]["dup"] or 0)
    # held back: wall time minus output time grows when the sender is blocked and falls when it catches up
    lag = [t - o for t, o in zip(ts, out_s)]
    steady = [l for t, l in zip(ts, lag) if t >= b0]
    holds = []
    if steady:
        base = pct(steady, 5)
        excess = [l - base for l in lag]
        flags = [e * 1000 >= a.hold_ms and t >= b0 for e, t in zip(excess, ts)]
        step = pct([ts[i] - ts[i - 1] for i in range(1, len(ts))], 50) or 0.2
        for s, e, i0, i1 in episodes(ts, flags, step):
            holds.append({"type": "sender_hold", "start": s, "end": e, "value": max(excess[i0:i1 + 1]) * 1000,
                          "detail": "sender held back up to %.0f ms" % (max(excess[i0:i1 + 1]) * 1000)})
        res["lag_baseline_s"] = base
        res["clock_offset_s"] = None
        origin = (run.meta.get("clock") or {}).get("burn_origin_epoch")
        if origin is not None:
            # the least-delayed sample is when the frame left the encoder: the burned CLK trails it by this
            res["clock_offset_s"] = min(lag) - origin
    res["holds"] = len(holds)
    res["hold_seconds"] = sum(h["end"] - h["start"] for h in holds)
    res["hold_peak_ms"] = max((h["value"] for h in holds), default=None)
    res["t0"] = t0
    return res, holds


# ---- tcp -------------------------------------------------------------------------------------

def analyse_tcp(tab, a):
    """The sender's (or the receiver's) ss -tin samples."""
    res = {"available": False}
    if tab is None or tab.n < 3 or "cwnd" not in tab.num:
        return res, []
    t = tab.col("t")
    ok = [i for i in range(tab.n) if t[i] is not None and tab.num["cwnd"][i] is not None]
    if len(ok) < 3:
        return res, []
    res["available"] = True

    def c(name):
        return [tab.num[name][i] if name in tab.num else None for i in ok]

    ts = [t[i] for i in ok]
    cwnd, rtt = c("cwnd"), c("rtt_ms")
    res["samples"] = len(ts)
    dts = [ts[i] - ts[i - 1] for i in range(1, len(ts))]
    res["interval_p50_ms"] = pct(dts, 50) * 1000
    res["interval_p95_ms"] = pct(dts, 95) * 1000
    res["interval_max_ms"] = max(dts) * 1000
    ccs = [tab.raw["cc"][i] for i in ok if "cc" in tab.raw and tab.raw["cc"][i]]
    res["cc"] = ccs[-1] if ccs else None
    res["cwnd_p50"] = pct(cwnd, 50)
    res["cwnd_max"] = max(x for x in cwnd if x is not None)
    inc = []

    # retransmissions
    rt = c("retrans_total")
    if any(x is not None for x in rt):
        rts = [x or 0 for x in rt]
        total = rts[-1] - rts[0]
        dsegs = span(c("data_segs_out"))
        res["retrans_segments"] = int(total)
        res["data_segments"] = int(dsegs) if dsegs else None
        res["retrans_pct"] = 100.0 * total / dsegs if dsegs else None
        brb, bsb = span(c("bytes_retrans")), span(c("bytes_sent"))
        res["retrans_bytes_pct"] = 100.0 * brb / bsb if brb is not None and bsb else None
        iv = [(ts[i - 1], ts[i], rts[i] - rts[i - 1]) for i in range(1, len(ts)) if rts[i] > rts[i - 1]]
        bursts = merge_intervals(iv, 1.0)
        res["retrans_bursts"] = len(bursts)
        res["retrans_burst_max"] = int(max((b[2] for b in bursts), default=0))
        for s, e, v in bursts:
            inc.append({"type": "tcp_retrans", "start": s, "end": e, "value": v, "detail": "%d segments retransmitted" % v})
    lost = c("lost")
    res["lost_max"] = int(max((x for x in lost if x is not None), default=0))
    reo = [x for x in c("reordering") if x is not None]
    res["reordering_max"] = int(max(reo)) if reo else None

    # RTT over its minimum (the kernel's rtt is the smoothed one: spikes show smaller than they were)
    rv = [(ts[i], rtt[i]) for i in range(len(ts)) if rtt[i] is not None and rtt[i] > 0]
    if rv:
        mn = c("minrtt_ms")
        kmin = next((x for x in reversed(mn) if x), None)
        smin = min(v for _, v in rv)
        base = smin
        res["rtt_min_ms"] = base
        res["rtt_kernel_min_ms"] = kmin
        infl = [v - base for _, v in rv]
        res["rtt_p50_ms"] = pct([v for _, v in rv], 50)
        res["rtt_infl_p50_ms"] = pct(infl, 50)
        res["rtt_infl_p95_ms"] = pct(infl, 95)
        res["rtt_infl_p99_ms"] = pct(infl, 99)
        res["rtt_infl_max_ms"] = max(infl)
        step = res["interval_p50_ms"] / 1000.0
        res["rtt_over_100ms_s"] = sum(step for x in infl if x >= a.rtt_spike_ms)
        res["rtt_over_2x_s"] = sum(step for _, v in rv if v >= 2 * base) if base >= 1.0 else None
        rts_t = [x for x, _ in rv]
        flags = [x >= a.rtt_spike_ms for x in infl]
        spikes = episodes(rts_t, flags, step)
        res["rtt_spikes"] = len(spikes)
        for s, e, i0, i1 in spikes:
            pk = max(infl[i0:i1 + 1])
            inc.append({"type": "tcp_rtt_spike", "start": s, "end": e, "value": pk,
                        "detail": "smoothed RTT %.0f ms over its minimum (%s ms)" % (pk, fm(base))})

    # collapses of cwnd: under half of the last second's peak, the peak at least COLLAPSE_PEAK_MIN
    coll = []
    state = None
    j = 0
    for i in range(len(ts)):
        while ts[j] < ts[i] - 1.0:
            j += 1
        peak = max(cwnd[j:i + 1])
        if state is None:
            if peak >= COLLAPSE_PEAK_MIN and cwnd[i] <= COLLAPSE_RATIO * peak:
                state = {"start": ts[i], "from": peak, "min": cwnd[i], "i0": i}
        else:
            state["min"] = min(state["min"], cwnd[i])
            if cwnd[i] >= 0.8 * state["from"] or ts[i] - state["start"] > 30:
                state["end"] = ts[i]
                coll.append(state)
                state = None
    if state is not None:
        state["end"] = ts[-1]
        coll.append(state)
    probe = 0
    for s in coll:
        s["duration"] = s["end"] - s["start"]
        s["probe_rtt_like"] = bool(res["cc"] and "bbr" in res["cc"] and s["duration"] < 0.4 and s["min"] <= 4)
        probe += s["probe_rtt_like"]
    res["collapses"] = len(coll)
    res["collapses_probe_rtt_like"] = probe
    res["collapse_list"] = [{"start": s["start"], "end": s["end"], "from": s["from"], "to": s["min"],
                             "probe_rtt_like": s["probe_rtt_like"]} for s in coll]
    for s in coll:
        if s["probe_rtt_like"]:
            continue  # BBR's ProbeRTT dip: by design, not a loss event
        inc.append({"type": "tcp_collapse", "start": s["start"], "end": max(s["end"], s["start"] + 0.1), "value": s["from"] - s["min"],
                    "detail": "cwnd %d -> %d segments, back in %.1f s" % (s["from"], s["min"], s["duration"])})

    # the time limited by the send buffer, and the backlog (counters in ms, cumulative)
    dur = ts[-1] - ts[0]
    for key, label in (("sndbuf_limited_ms", "sndbuf_limited_s"), ("rwnd_limited_ms", "rwnd_limited_s"), ("busy_ms", "busy_s")):
        v = c(key)
        if any(x is not None for x in v) and tab.has(key):
            res[label] = max(0.0, (v[-1] or 0) - (v[0] or 0)) / 1000.0
            res[label + "_pct"] = 100.0 * res[label] / dur if dur > 0 else None
        else:
            res[label] = None
    ns = c("notsent")
    sq = c("sendq")
    res["notsent_p95_bytes"] = pct(ns, 95)
    res["notsent_max_bytes"] = max((x for x in ns if x is not None), default=None)
    res["backlog_over_64k_s"] = sum(res["interval_p50_ms"] / 1000.0 for x in ns if x is not None and x > 65536)
    res["sendq_max_bytes"] = max((x for x in sq if x is not None), default=None)
    app = c("app_limited")
    dr = [x for x, al in zip(c("delivery_rate_bps"), app) if x and not al]  # an app-limited sender's rate says nothing
    res["delivery_rate_p50_mbps"] = pct(dr, 50) / 1e6 if dr else None
    # the receiver side of the same columns, when this is the receiver's table
    oo = c("rcv_ooopack")
    if any(x for x in oo):
        res["rcv_ooopack"] = int(oo[-1] - oo[0])
        sin = span(c("segs_in"))
        res["segs_in"] = int(sin) if sin is not None else None
        ds = span(c("dsack_dups"))
        res["dsack_dups"] = int(ds) if ds is not None else None
    res["ts"] = ts
    res["cwnd"] = cwnd
    res["rtt"] = rtt
    return res, inc


# ---- srt and udp -----------------------------------------------------------------------------

def srt_delta(tab, name):
    """Growth of a cumulative counter of srt-live-transmit -f over the run (None when absent)."""
    v = tab.num.get(name)
    if not v or all(x is None for x in v):
        return None
    xs = [x for x in v if x is not None]
    return xs[-1] - xs[0]


def analyse_srt(tab, a):
    res = {"available": False}
    if tab is None or tab.n < 3 or not getattr(tab, "t", None):
        return res, []
    res["available"] = True
    ts = tab.t
    res["reports"] = tab.n
    res["seconds"] = ts[-1] - ts[0]
    sent = srt_delta(tab, "pktSentUnique")
    recv = srt_delta(tab, "pktRecvUnique")
    res["packets_sent"] = srt_delta(tab, "pktSent")
    res["packets_sent_unique"] = sent
    res["packets_recv_unique"] = recv
    inc = []
    kinds = (("pktSndLoss", "srt_loss", "packets reported lost by the receiver"),
             ("pktRetrans", None, None),
             ("pktSndDrop", "srt_drop", "packets dropped at the sender (too late to be sent)"),
             ("pktRcvLoss", "srt_rx_loss", "packets lost on the way, seen by the receiver"),
             ("pktRcvRetrans", None, None),
             ("pktRcvDrop", "srt_rx_drop", "packets dropped at the receiver (too late to play)"),
             ("pktRcvBelated", None, None))
    for name, typ, text in kinds:
        d = srt_delta(tab, name)
        res[name] = d
        v = tab.num.get(name)
        if typ and d and v:
            iv = []
            for i in range(1, len(v)):
                if v[i] is not None and v[i - 1] is not None and v[i] > v[i - 1] and ts[i] is not None and ts[i - 1] is not None:
                    iv.append((ts[i - 1], ts[i], v[i] - v[i - 1]))
            for s, e, n in merge_intervals(iv, 1.0):
                inc.append({"type": typ, "start": s, "end": e, "value": n, "detail": "%d %s" % (n, text)})
    for name, key, base in (("pktSndLoss", "loss_pct", sent), ("pktRetrans", "retrans_pct", sent), ("pktSndDrop", "drop_pct", sent),
                            ("pktRcvLoss", "rcv_loss_pct", recv), ("pktRcvRetrans", "rcv_retrans_pct", recv), ("pktRcvDrop", "rcv_drop_pct", recv)):
        res[key] = 100.0 * res[name] / base if res.get(name) is not None and base else None
    # the gauges of the first seconds are not measured yet (the RTT starts at 100 ms, the bandwidth at a guess)
    warm = [i for i in range(tab.n) if ts[i] is not None and ts[i] >= ts[0] + a.skip]
    rtt = [tab.col("msRTT")[i] for i in warm if tab.col("msRTT")[i] is not None]
    if rtt:
        res["rtt_min_ms"] = min(rtt)
        res["rtt_p50_ms"] = pct(rtt, 50)
        res["rtt_p95_ms"] = pct(rtt, 95)
        res["rtt_max_ms"] = max(rtt)
    for name, key in (("msSndBuf", "sndbuf_ms"), ("msRcvBuf", "rcvbuf_ms"), ("msRcvTsbPdDelay", "latency_ms"),
                      ("mbpsBandwidth", "bandwidth_mbps"), ("mbpsSendRate", "send_rate_mbps"), ("mbpsRecvRate", "recv_rate_mbps")):
        v = [tab.col(name)[i] for i in warm if tab.col(name)[i] is not None]
        if v:
            res[key + "_p50"] = pct(v, 50)
            res[key + "_p95"] = pct(v, 95)
            res[key + "_max"] = max(v)
    res["ts"] = ts
    res["rtt"] = tab.col("msRTT")
    res["sndbuf"] = tab.col("msSndBuf")
    res["rcvbuf"] = tab.col("msRcvBuf")
    return res, inc


def analyse_udp(tab):
    res = {"available": False}
    if tab is None or tab.n < 3:
        return res
    res["available"] = True
    sq = [x for x in tab.col("sendq") if x is not None]
    rq = [x for x in tab.col("recvq") if x is not None]
    dr = [x for x in tab.col("drops") if x is not None]
    res["sendq_max_bytes"] = max(sq) if sq else None
    res["recvq_max_bytes"] = max(rq) if rq else None
    res["drops"] = int(dr[-1] - dr[0]) if dr else None
    return res


# ---- receiver --------------------------------------------------------------------------------

def startup_burst(a, p):
    """How many leading packets arrived in a lump: ffmpeg probes the input before it hands anything over."""
    n = 0
    while n < len(a) and a[n] - a[0] < 0.1:
        n += 1
    if n >= 3 and p[n - 1] is not None and p[0] is not None and p[n - 1] - p[0] > 0.25:
        return n - 1
    return 0


def analyse_receiver(run, a):
    res = {"available": False}
    tab = run.packets
    if tab is None or tab.n < 10:
        return res, []
    t = tab.col("t")
    pts = tab.col("pts")
    dts = tab.col("dts")
    size = tab.col("size")
    rows = [(t[i], pts[i] if pts[i] is not None else dts[i], size[i] or 0, tab.raw["flags"][i]) for i in range(tab.n) if t[i] is not None]
    if len(rows) < 10:
        return res, []
    res["available"] = True
    arr = [r[0] for r in rows]
    pt = [r[1] for r in rows]
    res["video_packets"] = len(rows)
    res["first_packet"] = arr[0]
    res["last_packet"] = arr[-1]
    res["seconds"] = arr[-1] - arr[0]
    res["keyframes"] = sum(1 for r in rows if "K" in r[3])
    n0 = startup_burst(arr, pt)
    res["startup_burst_packets"] = n0
    arr, pt, rows = arr[n0:], pt[n0:], rows[n0:]
    inc = []
    # bitrate per second of arrival
    total_bytes = sum(r[2] for r in rows)
    span = arr[-1] - arr[0]
    res["video_mbps"] = total_bytes * 8 / span / 1e6 if span > 0 else None
    cum = []
    acc = 0
    for r in rows:
        acc += r[2]
        cum.append(acc)
    b0 = arr[0] + a.skip
    mb = []
    nb = int((arr[-1] - a.skip_end - b0) // 1.0)
    for k in range(max(nb, 0)):
        x0, x1 = b0 + k, b0 + k + 1
        mb.append((interp(arr, cum, x1) - interp(arr, cum, x0)) * 8 / 1e6)
    res["video_p5_mbps"] = pct(mb, 5)
    res["video_p50_mbps"] = pct(mb, 50)
    res["video_p95_mbps"] = pct(mb, 95)
    res["bin_series"] = [(b0 + k, v) for k, v in enumerate(mb)]
    # the frame interval, and jumps in pts
    pd = [pt[i] - pt[i - 1] for i in range(1, len(pt)) if pt[i] is not None and pt[i - 1] is not None]
    fi = pct([x for x in pd if x > 0], 50) or 0.033
    res["frame_interval_ms"] = fi * 1000
    jumps, missing = [], 0
    for i in range(1, len(pt)):
        if pt[i] is None or pt[i - 1] is None:
            continue
        d = pt[i] - pt[i - 1]
        if d < -0.001 or d > max(0.25, 4 * fi):
            jumps.append((arr[i], d))
            if d > 0:
                missing += max(0, int(round(d / fi)) - 1)
    res["pts_jumps"] = len(jumps)
    res["pts_jump_list"] = [{"t": x, "jump_s": d} for x, d in jumps[:20]]
    res["frames_missing"] = missing
    # stalls: no video packet for STALL_MS
    tail = arr[-1] - a.skip_end  # what comes after this is the stream ending: the sender stops, the muxer flushes
    # No start-up skip here: with the burst gone, the log starts in real time, and a stall 1 s in is a stall.
    gaps = [(arr[i - 1], arr[i]) for i in range(1, len(arr)) if arr[i] <= tail and arr[i] - arr[i - 1] >= a.stall_ms / 1000.0]
    res["stalls"] = len(gaps)
    gd = [(e - s) * 1000 for s, e in gaps]
    res["stall_total_s"] = sum(gd) / 1000.0
    res["stall_p50_ms"] = pct(gd, 50)
    res["stall_p95_ms"] = pct(gd, 95)
    res["stall_max_ms"] = max(gd) if gd else None
    res["stall_list"] = [{"start": s, "end": e, "ms": (e - s) * 1000} for s, e in gaps]
    for s, e in gaps:
        inc.append({"type": "rx_stall", "start": s, "end": e, "value": (e - s) * 1000, "detail": "no video packet for %.0f ms" % ((e - s) * 1000)})
    # late frames: arrival minus pts, over its floor; the sender's timeline against the receiver's clock
    pairs = [(arr[i], pt[i]) for i in range(len(arr)) if pt[i] is not None and arr[i] <= tail]
    if len(pairs) >= 10:
        d = [x - p for x, p in pairs]
        floor = pct(d, 1)
        late = [(x - p - floor) * 1000 for x, p in pairs]
        res["delay_floor_s"] = floor
        res["delay_p50_ms"] = pct(late, 50)
        res["delay_p95_ms"] = pct(late, 95)
        res["delay_p99_ms"] = pct(late, 99)
        res["delay_max_ms"] = max(late)
        lt = [x for x, _ in pairs]
        flags = [v >= a.late_ms for v in late]
        eps = episodes(lt, flags, fi)
        res["late_episodes"] = len(eps)
        res["late_frames"] = sum(flags)
        res["late_list"] = [{"start": s, "end": e, "peak_ms": max(late[i0:i1 + 1]), "frames": i1 - i0 + 1} for s, e, i0, i1 in eps]
        for s, e, i0, i1 in eps:
            pk = max(late[i0:i1 + 1])
            inc.append({"type": "rx_late", "start": s, "end": e, "value": pk, "detail": "%d frames up to %.0f ms late" % (i1 - i0 + 1, pk)})
        res["lag_series"] = list(zip(lt, late))
    return res, inc


# ---- fieldrec --------------------------------------------------------------------------------

def load_fieldrec(d, offset):
    """events.csv, and the span of the recording, from a bench/fieldrec.py --export directory."""
    res = {"events": [], "span": None, "lost_all": []}
    p = find_file(d, "events.csv")
    if not p:
        return None
    tab = Table(p)
    for i in range(tab.n):
        kind = tab.raw["kind"][i]
        s = tab.num["start_epoch"][i]
        e = tab.num["end_epoch"][i]
        if s is None:
            continue
        res["events"].append({"kind": kind, "link": tab.raw["link"][i], "start": s + offset,
                              "end": (e if e is not None else s) + offset, "known_end": e is not None,
                              "ends": tab.raw["ends"][i] if "ends" in tab.raw else ""})
    p = find_file(d, "tunnel.csv")
    if p:
        tt = Table(p)
        te = tt.col("time_epoch")
        ia = tt.col("interval_ms")
        la = tt.col("lost_all_delta")
        for i in range(tt.n):
            if te[i] is None:
                continue
            t1 = te[i] + offset
            t0 = t1 - (ia[i] or 0) / 1000.0
            res["lost_all"].append((t0, t1, la[i] or 0))
        if res["lost_all"]:
            res["span"] = (res["lost_all"][0][0], res["lost_all"][-1][1])
    if res["span"] is None:
        p = find_file(d, "samples.csv")
        if p:
            st = Table(p)
            te = [x for x in st.col("time_epoch") if x is not None]
            if te:
                res["span"] = (min(te) + offset, max(te) + offset)
    return res


def overlaps(a0, a1, b0, b1, pad):
    return a0 <= b1 + pad and a1 >= b0 - pad


def covered(intervals, lo, hi, pad):
    """Seconds of [lo, hi] inside the union of the intervals padded by pad."""
    iv = sorted((max(a - pad, lo), min(b + pad, hi)) for a, b in intervals if b + pad > lo and a - pad < hi)
    tot, cur = 0.0, None
    for a, b in iv:
        if b <= a:
            continue
        if cur is None or a > cur[1]:
            if cur:
                tot += cur[1] - cur[0]
            cur = [a, b]
        else:
            cur[1] = max(cur[1], b)
    if cur:
        tot += cur[1] - cur[0]
    return tot


def cross(fr, incidents, lo, hi, window, with_loss=True):
    """Which incidents fall within `window` s of a link event, and how many that would be by chance."""
    ev = [e for e in fr["events"] if e["kind"] in ("outage", "upload_mute", "download_mute") and e["end"] >= lo - window and e["start"] <= hi + window]
    out = {"window_s": window, "events": len(ev), "rows": [], "incidents": [], "span": fr["span"]}
    types = sorted({i["type"] for i in incidents})
    out["types"] = types
    run_len = hi - lo
    groups = {}
    for e in ev:
        groups.setdefault((e["kind"], None), []).append(e)
        groups.setdefault((e["kind"], e["link"]), []).append(e)
    for (kind, link), es in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1] or "")):
        hit = 0
        for e in es:
            if any(overlaps(i["start"], i["end"], e["start"], e["end"], window) for i in incidents):
                hit += 1
        frac = covered([(e["start"], e["end"]) for e in es], lo, hi, window) / run_len if run_len > 0 else 0
        row = {"kind": kind, "link": link or "all", "events": len(es), "events_with_incident": hit,
               "seconds": sum(e["end"] - e["start"] for e in es), "covered_frac": frac, "by_type": {}}
        for ty in types:
            tot = [i for i in incidents if i["type"] == ty]
            near = [i for i in tot if any(overlaps(i["start"], i["end"], e["start"], e["end"], window) for e in es)]
            row["by_type"][ty] = {"near": len(near), "total": len(tot), "expected": len(tot) * frac}
        out["rows"].append(row)
    for i in incidents:
        near = []
        for e in ev:
            if overlaps(i["start"], i["end"], e["start"], e["end"], window):
                if i["end"] < e["start"]:
                    where = "%.1f s before it" % (e["start"] - i["end"])
                elif i["start"] > e["end"]:
                    where = "%.1f s after it" % (i["start"] - e["end"])
                else:
                    where = "during it"
                near.append({"kind": e["kind"], "link": e["link"], "where": where})
        out["incidents"].append({**{k: i[k] for k in ("type", "start", "end", "detail")}, "near": near})
    # packets lost on every link (the engine's lost_all), against the TCP retransmission bursts
    la = [x for x in fr["lost_all"] if x[0] < hi and x[1] > lo]
    if la:
        out["lost_all_packets"] = int(sum(x[2] for x in la))
        out["lost_all_intervals"] = sum(1 for x in la if x[2] > 0)
        bad = [(x[0], x[1]) for x in la if x[2] > 0]
        rb = [i for i in incidents if i["type"] == "tcp_retrans"]
        out["retrans_bursts"] = len(rb)
        out["retrans_bursts_with_lost_all"] = sum(1 for i in rb if any(overlaps(i["start"], i["end"], a, b, 1.0) for a, b in bad))
    return out


# ---- text ------------------------------------------------------------------------------------

def render(r, out):
    w = out.append
    m = r["meta"]
    t0 = r["t0"]
    w("# Stream run %s" % (m.get("run_id") or r["dir"]))
    bits = []
    d = m.get("dest") or {}
    if d:
        bits.append("%s to %s:%s" % (d.get("scheme"), d.get("host"), d.get("port")))
    e = m.get("encoder") or {}
    if e:
        bits.append("%s in %s" % (e.get("used"), e.get("container")))
    if m.get("target_bitrate_bps"):
        bits.append("target %.2f Mbit/s" % (m["target_bitrate_bps"] / 1e6))
    v = m.get("video") or {}
    if v:
        bits.append("%s@%s, GOP %s s" % (v.get("size"), v.get("fps"), v.get("gop_s")))
    if (m.get("kernel") or {}).get("tcp_congestion_control") and d.get("transport") == "tcp":
        bits.append("cc %s" % m["kernel"]["tcp_congestion_control"])
    if bits:
        w(", ".join(bits))
    if t0 is not None and r["t1"] is not None:
        w("%s UTC, %.1f s%s" % (hms(t0), r["t1"] - t0, ("; stopped by " + str(m["stopped_by"])) if m.get("stopped_by") else ""))
    w("")
    s = r["sender"]
    if s.get("available"):
        w("## Sender")
        w("")
        tgt = s.get("target_mbps")
        w("- bitrate at the muxer (audio and container overhead included, so a little over the video target), 1 s bins (first %.0f s left out): target %s Mbit/s; mean %s; p5 %s, p50 %s, p95 %s; "
          "%d of %d seconds under half the target" % (r["params"]["skip_s"], f1(tgt, 2), f1(s["mean_mbps"], 2), f1(s["bitrate_p5_mbps"], 2),
                                                      f1(s["bitrate_p50_mbps"], 2), f1(s["bitrate_p95_mbps"], 2), s["bins_below_half_target"], s["bins"]))
        w("- encoder speed: %sx at the end; slowest second %sx; %d seconds under 0.95x; dropped frames %d, duplicated %d"
          % (f1(s["speed_final"], 2), f1(s["speed_min_1s"], 2), s["bins_speed_below_0_95"], s["drop_frames"], s["dup_frames"]))
        w("- sender held back (output time behind real time by %d ms or more): episodes %d, %.1f s in all, worst %s ms"
          % (r["params"]["hold_ms"], s["holds"], s["hold_seconds"], f1(s["hold_peak_ms"], 0)))
        if s.get("clock_offset_s") is not None:
            w("- burned-in CLK against the wall clock: it is %.2f s %s the real time at which the frame left the encoder (the start-up time the lead "
              "did not cover; an estimate, good to a frame or two of encoder delay)" % (abs(s["clock_offset_s"]), "behind" if s["clock_offset_s"] > 0 else "ahead of"))
        w("")
    tc = r["tcp"]
    if tc.get("available"):
        w("## TCP (ss -tin of the stream's socket, %d samples every %.0f ms, p95 %.0f ms, longest gap %.0f ms)" % (
            tc["samples"], tc["interval_p50_ms"], tc["interval_p95_ms"], tc["interval_max_ms"]))
        w("")
        if tc.get("retrans_segments") is not None:
            w("- retransmissions: %d segments of %s (%s %%), %s %% of the bytes; %d bursts, the largest %d segments; kernel `lost` peaked at %d, reordering at %s"
              % (tc["retrans_segments"], tc.get("data_segments") or "?", f1(tc.get("retrans_pct"), 2), f1(tc.get("retrans_bytes_pct"), 2),
                 tc["retrans_bursts"], tc["retrans_burst_max"], tc["lost_max"], tc.get("reordering_max") if tc.get("reordering_max") is not None else "-"))
        if tc.get("rtt_min_ms") is not None:
            x2 = ("%.1f s over 2x the minimum" % tc["rtt_over_2x_s"]) if tc["rtt_over_2x_s"] is not None else "2x the minimum not judged (the minimum is under 1 ms)"
            w("- RTT (the kernel's smoothed one): minimum %s ms, p50 %s; over the minimum p50 %s, p95 %s, p99 %s, max %s ms; "
              "%.1f s over +%d ms (spikes: %d), %s" % (
                  fm(tc["rtt_min_ms"]), fm(tc["rtt_p50_ms"]), fm(tc["rtt_infl_p50_ms"]), fm(tc["rtt_infl_p95_ms"]), fm(tc["rtt_infl_p99_ms"]),
                  fm(tc["rtt_infl_max_ms"]), tc["rtt_over_100ms_s"], r["params"]["rtt_spike_ms"], tc["rtt_spikes"], x2))
        extra = ""
        if tc["collapses_probe_rtt_like"]:
            extra = " (%d of them ProbeRTT-like dips of BBR, not counted as incidents)" % tc["collapses_probe_rtt_like"]
        w("- cwnd (%s): p50 %s, max %d segments; collapses under %d %% of the last second's peak: %d%s"
          % (tc.get("cc") or "?", f1(tc["cwnd_p50"], 0), tc["cwnd_max"], int(COLLAPSE_RATIO * 100), tc["collapses"], extra))
        for c in tc["collapse_list"][:12]:
            w("  - %s (%s): %d -> %d segments%s" % (hms(c["start"]), rel(c["start"], t0), c["from"], c["to"], ", ProbeRTT-like" if c["probe_rtt_like"] else ""))
        if len(tc["collapse_list"]) > 12:
            w("  - ... %d more" % (len(tc["collapse_list"]) - 12))
        if tc.get("sndbuf_limited_s") is not None:
            w("- limited by the send buffer: %.1f s (%.1f %% of the run); by the receiver's window %.1f s; busy %.1f s"
              % (tc["sndbuf_limited_s"], tc["sndbuf_limited_s_pct"] or 0, tc.get("rwnd_limited_s") or 0, tc.get("busy_s") or 0))
        else:
            w("- limited by the send buffer: no counter in this kernel's ss output")
        w("- send backlog (bytes not yet sent): p95 %s, max %s; %.1f s over 64 KiB; delivery rate p50 %s Mbit/s"
          % (f1(tc["notsent_p95_bytes"], 0), f1(tc["notsent_max_bytes"], 0), tc["backlog_over_64k_s"], f1(tc.get("delivery_rate_p50_mbps"), 1)))
        w("")
    sr, sx = r["srt"], r["srt_rx"]
    if sr.get("available") or sx.get("available"):
        w("## SRT (srt-live-transmit statistics: %s)" % ", ".join(
            "%s %d reports over %.1f s" % (n, x["reports"], x["seconds"]) for n, x in (("sender", sr), ("receiver", sx)) if x.get("available")))
        w("")
        if sr.get("available"):
            w("- sender: %s packets sent (%s unique); lost %s (%s %%), retransmitted %s (%s %%), dropped too late %s (%s %%)" % (
                f1(sr.get("packets_sent"), 0), f1(sr.get("packets_sent_unique"), 0), f1(sr.get("pktSndLoss"), 0), f1(sr.get("loss_pct"), 2),
                f1(sr.get("pktRetrans"), 0), f1(sr.get("retrans_pct"), 2), f1(sr.get("pktSndDrop"), 0), f1(sr.get("drop_pct"), 2)))
            if sr.get("rtt_min_ms") is not None:
                w("- RTT: min %.1f, p50 %.1f, p95 %.1f, max %.1f ms" % (sr["rtt_min_ms"], sr["rtt_p50_ms"], sr["rtt_p95_ms"], sr["rtt_max_ms"]))
            if sr.get("sndbuf_ms_p50") is not None:
                w("- sender buffer: p50 %.0f, p95 %.0f, max %.0f ms; estimated bandwidth p50 %s Mbit/s" % (
                    sr["sndbuf_ms_p50"], sr["sndbuf_ms_p95"], sr["sndbuf_ms_max"], f1(sr.get("bandwidth_mbps_p50"), 1)))
        if sx.get("available"):
            w("- receiver: %s packets received; lost %s (%s %%), retransmissions received %s, dropped too late %s (%s %%), belated %s" % (
                f1(sx.get("packets_recv_unique"), 0), f1(sx.get("pktRcvLoss"), 0), f1(sx.get("rcv_loss_pct"), 2), f1(sx.get("pktRcvRetrans"), 0),
                f1(sx.get("pktRcvDrop"), 0), f1(sx.get("rcv_drop_pct"), 2), f1(sx.get("pktRcvBelated"), 0)))
            if sx.get("rcvbuf_ms_p50") is not None:
                w("- receiver buffer: p50 %.0f, p95 %.0f, max %.0f ms (SRT latency %s ms)" % (
                    sx["rcvbuf_ms_p50"], sx["rcvbuf_ms_p95"], sx["rcvbuf_ms_max"], f1(sx.get("latency_ms_p50"), 0)))
        w("")
    elif r["sender"].get("available") and (m.get("dest") or {}).get("scheme") == "srt":
        w("## SRT")
        w("")
        w("- no SRT statistics: ffmpeg's libsrt prints none. Run send.sh -S (and recv.sh -S) to get them from srt-live-transmit (package srt-tools).")
        w("")
    ud = r["udp"]
    if ud.get("available"):
        w("- UDP socket of the stream: send queue up to %s bytes, receive queue up to %s bytes, drops %s" % (
            f1(ud.get("sendq_max_bytes"), 0), f1(ud.get("recvq_max_bytes"), 0), f1(ud.get("drops"), 0)))
        w("")
    rx = r["receiver"]
    if rx.get("available"):
        w("## Receiver")
        w("")
        w("- %d video packets (%d key frames) over %.1f s, %s Mbit/s on average; per second p5 %s, p50 %s, p95 %s; frame interval %.1f ms; "
          "the start-up burst (%d packets) is left out" % (
              rx["video_packets"], rx["keyframes"], rx["seconds"], f1(rx["video_mbps"], 2), f1(rx["video_p5_mbps"], 2), f1(rx["video_p50_mbps"], 2),
              f1(rx["video_p95_mbps"], 2), rx["frame_interval_ms"], rx["startup_burst_packets"]))
        w("- stalls (no video packet for %d ms or more, the last %g s left out): %d, %.1f s in all; p50 %s ms, p95 %s ms, longest %s ms" % (
            r["params"]["stall_ms"], r["params"]["skip_end_s"], rx["stalls"], rx["stall_total_s"], f1(rx["stall_p50_ms"], 0), f1(rx["stall_p95_ms"], 0), f1(rx["stall_max_ms"], 0)))
        for st in rx["stall_list"][:10]:
            w("  - %s (%s): %.0f ms" % (hms(st["start"]), rel(st["start"], t0), st["ms"]))
        if len(rx["stall_list"]) > 10:
            w("  - ... %d more" % (len(rx["stall_list"]) - 10))
        if rx.get("delay_p50_ms") is not None:
            w("- frames late against the sender's timeline (arrival minus pts, over its floor, which is the two clocks' offset plus the path's "
              "least delay): p50 %.0f, p95 %.0f, p99 %.0f, max %.0f ms; episodes over %d ms: %d (%d frames)" % (
                  rx["delay_p50_ms"], rx["delay_p95_ms"], rx["delay_p99_ms"], rx["delay_max_ms"],
                  r["params"]["late_ms"], rx["late_episodes"], rx["late_frames"]))
        w("- jumps in pts: %d (frames missing from them: %d)" % (rx["pts_jumps"], rx["frames_missing"]))
        rt = r.get("rx_tcp") or {}
        if rt.get("available") and rt.get("rcv_ooopack") is not None:
            w("- the server's TCP socket: %d out-of-order segments of %s received, %s DSACKs sent" % (rt["rcv_ooopack"], rt.get("segs_in") or "?", rt.get("dsack_dups")))
        elif rt.get("available"):
            w("- the server's TCP socket: no out-of-order segments seen")
        w("")
    fr = r.get("fieldrec")
    if fr:
        w("## Link events (cengarde-rec, within %.1f s)" % fr["window_s"])
        w("")
        if fr.get("span"):
            w("The recording covers %s to %s UTC." % (hms(fr["span"][0]), hms(fr["span"][1])))
            if fr["span"][1] < r["t0"] or fr["span"][0] > r["t1"]:
                w("")
                w("WARNING: the recording does not overlap the run: check the clocks (NTP on the PC and the router), or use --fieldrec-offset.")
        if not fr["events"]:
            w("No link outages or mutes during the run (or the recording has none).")
        else:
            w("%d link events overlap the run." % fr["events"])
            w("")
            types = fr["types"]
            head = ["event", "link", "events", "seconds", "with an incident near"] + ["%s near/total (chance)" % t for t in types]
            w("| " + " | ".join(head) + " |")
            w("| --- | --- | ---: | ---: | ---: |" + " ---: |" * len(types))
            for row in fr["rows"]:
                cells = [row["kind"], row["link"], str(row["events"]), "%.1f" % row["seconds"], "%d of %d" % (row["events_with_incident"], row["events"])]
                for t in types:
                    b = row["by_type"][t]
                    cells.append("%d/%d (%.1f)" % (b["near"], b["total"], b["expected"]))
                w("| " + " | ".join(cells) + " |")
            w("")
            w("Incidents: stalls and late frames at the receiver, held-back episodes at the sender, cwnd collapses, RTT spikes and "
              "retransmission bursts of the TCP socket, SRT losses and drops. Near means within the window, by overlap of the intervals. "
              "Chance is the incidents of that type times the share of the run covered by the windows of those events.")
        if "lost_all_packets" in fr:
            w("")
            w("- packets lost on every link (the engine's lost_all) during the run: %d, in %d intervals; %d of %d TCP retransmission bursts fall in one of them (1 s)"
              % (fr["lost_all_packets"], fr["lost_all_intervals"], fr["retrans_bursts_with_lost_all"], fr["retrans_bursts"]))
        shown = [i for i in fr["incidents"] if i["near"]]
        if shown:
            w("")
            w("Incidents near a link event:")
            w("")
            for i in shown[:25]:
                w("- %s (%s) %s: %s; %s" % (hms(i["start"]), rel(i["start"], t0), i["type"], i["detail"],
                                           ", ".join("%s %s %s" % (n["kind"], n["link"], n["where"]) for n in i["near"][:3])))
            if len(shown) > 25:
                w("- ... %d more" % (len(shown) - 25))
        w("")
    w("## Incidents")
    w("")
    cnt = {}
    for i in r["incidents"]:
        cnt[i["type"]] = cnt.get(i["type"], 0) + 1
    w("- " + (", ".join("%s %d" % (k, v) for k, v in sorted(cnt.items())) if cnt else "none"))
    if r.get("plot"):
        w("- plot: %s" % r["plot"])
    elif r.get("plot_note"):
        w("- plot: %s" % r["plot_note"])
    w("")


# ---- plots -----------------------------------------------------------------------------------

def make_plot(r, path):
    """report.png: stacked panels on one time axis, one scale per panel (no dual axes), link events as bands."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # The reference palette of the dataviz skill (validated light, blue and orange are adjacent slots);
    # the status colors mark states only: outage, mute.
    SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1"
    BLUE, ORANGE = "#2a78d6", "#eb6834"
    CRIT, WARN, SERIOUS = "#d03b3b", "#fab219", "#ec835a"
    t0 = r["t0"]
    panels = []
    s, tc, sr, rx = r["sender"], r["tcp"], r["srt"], r["receiver"]
    if s.get("available") or rx.get("available"):
        panels.append("rate")
    if tc.get("available"):
        panels += ["cwnd", "rtt"]
    elif sr.get("available") or r["srt_rx"].get("available"):
        panels += ["srt"]
    if rx.get("available") and rx.get("lag_series"):
        panels.append("late")
    if not panels:
        raise RuntimeError("nothing to plot")
    fig, axes = plt.subplots(len(panels), 1, sharex=True, figsize=(12, 2.4 * len(panels) + 0.8), facecolor=SURFACE)
    if len(panels) == 1:
        axes = [axes]
    ev = (r.get("fieldrec") or {}).get("events_in_run") or []
    band = {"outage": (CRIT, 0.16), "upload_mute": (WARN, 0.28), "download_mute": (SERIOUS, 0.26)}
    for ax, name in zip(axes, panels):
        ax.set_facecolor(SURFACE)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        for sp in ("left", "bottom"):
            ax.spines[sp].set_color(INK2)
        ax.tick_params(colors=INK2, labelsize=9)
        ax.grid(True, color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)
        for e in ev:
            col, al = band[e["kind"]]
            ax.axvspan(e["start"] - t0, max(e["end"], e["start"] + 0.1) - t0, color=col, alpha=al, linewidth=0, zorder=0)

        def title(txt):
            ax.set_title(txt, loc="left", fontsize=10, color=INK, pad=4)

        def legend(**kw):
            lg = ax.legend(loc="upper right", fontsize=8, frameon=False, ncol=3, **kw)
            for t in lg.get_texts():
                t.set_color(INK2)

        if name == "rate":
            title("Bitrate, Mbit/s (1 s bins)")
            if s.get("available") and s.get("bin_series"):
                ax.plot([b[0] - t0 for b in s["bin_series"]], [b[1] for b in s["bin_series"]], color=BLUE, linewidth=1.6, label="sender, muxer output")
            if rx.get("available") and rx.get("bin_series"):
                ax.plot([b[0] - t0 for b in rx["bin_series"]], [b[1] for b in rx["bin_series"]], color=ORANGE, linewidth=1.6, label="receiver, video")
            if s.get("target_mbps"):
                ax.axhline(s["target_mbps"], color=INK2, linewidth=1.0, linestyle=(0, (4, 3)), label="target")
            ax.set_ylim(bottom=0)
            legend()
        elif name == "cwnd":
            title("TCP congestion window, segments (%s)" % (tc.get("cc") or "?"))
            ax.plot([x - t0 for x in tc["ts"]], tc["cwnd"], color=BLUE, linewidth=1.4)
            for c in tc["collapse_list"]:
                ax.plot([c["start"] - t0], [c["to"]], marker="v", color=CRIT if not c["probe_rtt_like"] else INK2, markersize=7, linestyle="none", markeredgecolor=SURFACE)
            ax.set_ylim(bottom=0)
        elif name == "rtt":
            title("TCP smoothed RTT, ms (dashed: minimum)")
            pts = [(x - t0, v) for x, v in zip(tc["ts"], tc["rtt"]) if v is not None]
            ax.plot([p[0] for p in pts], [p[1] for p in pts], color=BLUE, linewidth=1.4)
            if tc.get("rtt_min_ms") is not None:
                ax.axhline(tc["rtt_min_ms"], color=INK2, linewidth=1.0, linestyle=(0, (4, 3)))
            ax.set_ylim(bottom=0)
        elif name == "srt":
            sr = sr if sr.get("available") else r["srt_rx"]
            title("SRT, ms (RTT and buffer)")
            ts = [x - t0 for x in sr["ts"] if x is not None]
            rt = [v for x, v in zip(sr["ts"], sr["rtt"]) if x is not None]
            ax.plot(ts, [v if v is not None else float("nan") for v in rt], color=BLUE, linewidth=1.4, label="RTT")
            sb = [v for x, v in zip(sr["ts"], sr["sndbuf"] if any(v is not None for v in sr["sndbuf"]) else sr["rcvbuf"]) if x is not None]
            ax.plot(ts, [v if v is not None else float("nan") for v in sb], color=ORANGE, linewidth=1.4, label="buffer")
            for i in r["incidents"]:
                if i["type"].startswith("srt_"):
                    ax.plot([i["start"] - t0], [0], marker="^", color=CRIT, markersize=7, linestyle="none", markeredgecolor=SURFACE)
            ax.set_ylim(bottom=0)
            legend()
        elif name == "late":
            title("Receiver: frames late over the floor, ms (red ticks: stalls)")
            xs, ys = [], []
            for x, v in rx["lag_series"]:
                if xs and x - (xs[-1] + t0) > 0.3:  # no line across a stall: nothing arrived
                    xs.append(float("nan"))
                    ys.append(float("nan"))
                xs.append(x - t0)
                ys.append(v)
            ax.plot(xs, ys, color=BLUE, linewidth=1.2, marker=".", markersize=2)
            for st in rx["stall_list"]:
                ax.plot([st["start"] - t0, st["end"] - t0], [0, 0], color=CRIT, linewidth=4, solid_capstyle="butt")
            ax.set_ylim(bottom=-20)
    axes[-1].set_xlabel("seconds from the start of the run (%s UTC)" % hms(t0), color=INK2, fontsize=9)
    # the link events, said once, at the top of the figure
    if ev:
        import matplotlib.patches as mp
        kinds = sorted({e["kind"] for e in ev})
        hs = [mp.Patch(facecolor=band[k][0], alpha=band[k][1] + 0.2, label="link " + k.replace("_", " ")) for k in kinds]
        lg = fig.legend(handles=hs, loc="upper right", fontsize=8, frameon=False, ncol=len(hs))
        for t in lg.get_texts():
            t.set_color(INK2)
    fig.suptitle(r["meta"].get("run_id") or r["dir"], x=0.01, ha="left", fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=110, facecolor=SURFACE)
    plt.close(fig)


# ---- main ------------------------------------------------------------------------------------

def build(a, snd, rx_run):
    """Everything the report says, as one dict: the numbers, the incidents, the series for the plots."""
    base = snd or rx_run
    r = {"dir": a.run, "meta": base.meta, "t0": base.start, "t1": base.end,
         "params": {"skip_s": a.skip, "skip_end_s": a.skip_end, "stall_ms": a.stall_ms, "late_ms": a.late_ms, "hold_ms": a.hold_ms,
                    "rtt_spike_ms": a.rtt_spike_ms, "window_s": a.window}}
    none = {"available": False}
    r["sender"], r["tcp"], r["srt"], r["srt_rx"], r["udp"], r["receiver"], r["rx_tcp"] = none, none, none, none, none, none, none
    incidents = []
    if snd:
        r["sender"], inc = analyse_sender(snd, a)
        incidents += inc
        if r["sender"].get("available"):
            r["t0"] = r["sender"]["t0"]
        r["tcp"], inc = analyse_tcp(snd.tcp, a)
        incidents += inc
        r["srt"], inc = analyse_srt(snd.srt, a)
        incidents += inc
        r["udp"] = analyse_udp(snd.udp)
    if rx_run:
        r["rx_meta"] = rx_run.meta
        r["receiver"], inc = analyse_receiver(rx_run, a)
        incidents += inc
        r["rx_tcp"], _ = analyse_tcp(rx_run.tcp, a)  # the server's socket: out-of-order segments, no incidents of its own
        r["srt_rx"], inc = analyse_srt(rx_run.srt, a)
        incidents += inc
        if not r["udp"].get("available"):
            r["udp"] = analyse_udp(rx_run.udp)
        if r["t0"] is None and r["receiver"].get("available"):
            r["t0"] = r["receiver"]["first_packet"]
        if r["t1"] is None and r["receiver"].get("available"):
            r["t1"] = r["receiver"]["last_packet"]
    incidents.sort(key=lambda i: i["start"])
    r["incidents"] = incidents
    if a.fieldrec:
        fr = load_fieldrec(a.fieldrec, a.fieldrec_offset)
        if fr is None:
            r["fieldrec_error"] = "no events.csv in %s: is it a fieldrec.py --export directory?" % a.fieldrec
        elif r["t0"] is not None and r["t1"] is not None:
            c = cross(fr, incidents, r["t0"], r["t1"], a.window)
            c["events_in_run"] = [e for e in fr["events"] if e["kind"] in ("outage", "upload_mute", "download_mute")
                                  and e["end"] >= r["t0"] and e["start"] <= r["t1"]]
            r["fieldrec"] = c
    return r


def jsonable(x):
    """The result without the long series (they are for the plots), and with plain floats."""
    drop = {"ts", "cwnd", "rtt", "sndbuf", "rcvbuf", "lag_series", "bin_series", "events_in_run"}
    if isinstance(x, dict):
        return {k: jsonable(v) for k, v in x.items() if k not in drop}
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return None
    return x


def main(argv=None):
    ap = argparse.ArgumentParser(description="Report on a run of bench/stream/send.sh (and recv.sh); see the top of this file.")
    ap.add_argument("run", help="a run directory made by send.sh (or recv.sh)")
    ap.add_argument("--rx", metavar="DIR", help="the run directory recv.sh made on the server side")
    ap.add_argument("--fieldrec", metavar="DIR", help="a bench/fieldrec.py --export directory (events.csv)")
    ap.add_argument("--fieldrec-offset", type=float, default=0.0, metavar="S", help="seconds added to the router's times (its clock minus the PC's, negated)")
    ap.add_argument("--window", type=float, default=WINDOW_S, metavar="S", help="near a link event means within S seconds (%g)" % WINDOW_S)
    ap.add_argument("--stall-ms", type=float, default=STALL_MS, help="a receiver stall is a gap this long in the arrival of video (%g)" % STALL_MS)
    ap.add_argument("--late-ms", type=float, default=LATE_MS, help="a frame this much over its floor is late (%g)" % LATE_MS)
    ap.add_argument("--hold-ms", type=float, default=HOLD_MS, help="the sender is held back when its output is this far behind (%g)" % HOLD_MS)
    ap.add_argument("--rtt-spike-ms", type=float, default=RTT_SPIKE_MS, help="a TCP RTT spike is this far over the minimum (%g)" % RTT_SPIKE_MS)
    ap.add_argument("--skip", type=float, default=SKIP_S, metavar="S", help="start-up left out of the bitrate and encoder speed statistics (%g)" % SKIP_S)
    ap.add_argument("--skip-end", type=float, default=SKIP_END_S, metavar="S",
                    help="the last S seconds of the receiver's log are left out of its statistics: the stream ending (%g)" % SKIP_END_S)
    ap.add_argument("--json", metavar="FILE", help="write all the numbers as JSON ('-' for stdout)")
    ap.add_argument("--no-plots", action="store_true", help="do not draw report.png even if matplotlib is installed")
    ap.add_argument("--plots", metavar="DIR", help="where to put report.png (default: the run directory)")
    a = ap.parse_args(argv)
    for d in (a.run, a.rx, a.fieldrec):
        if d and not os.path.isdir(d):
            ap.error("not a directory: %s" % d)
    snd, rx_run = Run(a.run), Run(a.rx) if a.rx else None
    if snd.kind == "receiver" and rx_run is None:
        snd, rx_run = None, snd
    r = build(a, snd, rx_run)
    r["plot"] = None
    r["plot_note"] = None
    if not a.no_plots:
        if importlib.util.find_spec("matplotlib") is None:
            r["plot_note"] = "no plot (matplotlib is not installed: apt install python3-matplotlib)" if a.plots is not None else None
        else:
            path = os.path.join(a.plots or a.run, "report.png")
            try:
                os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
                make_plot(r, path)
                r["plot"] = path
            except (RuntimeError, OSError, ValueError) as e:
                r["plot_note"] = "no plot: %s" % e
    lines = []
    if "fieldrec_error" in r:
        lines.append("WARNING: " + r["fieldrec_error"])
        lines.append("")
    render(r, lines)
    text = "\n".join(lines)
    print(text)
    if a.json:
        doc = json.dumps(jsonable(r), indent=1, sort_keys=True, default=str)
        if a.json == "-":
            print(doc)
        else:
            with open(a.json, "w") as fh:
                fh.write(doc + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
