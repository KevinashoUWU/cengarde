#!/usr/bin/env python3
"""Analysis of cengarde-rec field recordings (story 012).

cengarde-rec keeps every version of the client's status file as a JSON line
(hourly .jsonl.gz files). This reads them, in order, and reports what the
bonding of Fase 5 needs from the field:

- per link: time up, outages (count, durations), RTT (p50/p95/p99), the
  delay variation, download loss when there is traffic, upload loss, mutes;
- how many links are out at once, and each pair's correlation: P(B out | A
  out), the lift over independence, and how their RTTs move together;
- the RTT spread between the live links, and between the best two;
- per link, outage starts and RTT steps folded on the wall clock, modulo 15 s
  and per second of the minute: Starlink reconfigures its links every 15 s
  (story 012 assumes 1.5 s of loss at each, still to be measured);
- per hour of the day, each link's outages, RTT and loss;
- while the client split its upload, what each link did in the split
  (links[].split_share: a share, out for its delay with split_spread_ms,
  with copies or without as split_spread_copy says, waiting for a delay...), how often the delay guard took
  it out and how far behind the fastest it saw it.

A link is out while its state is not "live" or nothing came from it for
3 probe intervals (at least 300 ms); "paused" links (set off by hand) do
not count. Resolution is the sampling interval (status_interval_ms); the
start of an outage is placed at its last reply, which the engine stamps.

The passive instruments of the engine (status fields "joint", "lost_all" and
"sizes", packages 0.5.5; "sizes" counted WireGuard's messages and went with
the WireGuard mode in 0.12) add three sections; a recording without them says
so and keeps the rest. Their counters are cumulative since the engine
started: they are differenced between consecutive samples of one run (a
restart, a gap or a counter that went down starts over, as for the others).

- joint lateness (download): for each deadline after the first copy of a
  packet (50, 100 and 150 ms) and for every set S of 2 or more links, P(all
  of S late or lost) over the packets sent on all of S, the same under
  independence (the product of each link's own share), and the lift over it;
  the best pair by joint lateness against the best pair by each link's own
  lateness and by its RTT (story 012, "Para decidir k-de-N"); all of it also
  per hour of the day. Link ids come from links[].id. The engine's "other"
  counter holds what it could not judge, which is not analyzed: packets
  with an expected link of id 8 or more, and packets whose slot came round
  again before the longest deadline (about 27 kpps and up);
- lost on every link (download): the tunnel's loss, overall and per hour;
- WireGuard sizes, per direction (recordings of 0.5.5 to 0.11): the share of
  data packets and of bytes at or under each edge, and the handshake counts.

The receiver's reorderer (status field "reorder", step 1 of the bonding) adds a
fourth section, with the same rules: its counters are differenced between
samples and added up overall and per hour of the day; the client's is the
top-level field, and a server's status, recorded the same way (cengarde-rec
-s), has one per session, so its sessions are reported each, with the pool.
Reported: the packets delivered and the share that was held, those handed on
at once for being small, the gaps it gave up on (every link past the packet,
the deadline, no room, a flush), the packets that came late after that (by what
gave it up), the hold times and lateness histograms with their p50/p90/p99, the
extremes of the gauges and the worst intervals. An interval with reordering off,
or where a counter went down or the layout changed, is not added (counted
apart). A recording without the field says so.

Radio (optional, --rf FILE...): the RF metrics that SmoothStream's cell-monitor
posts to InfluxDB for each 5G CPE (RSRP, RSRQ, SINR, band, RAT, cell), crossed
with the links. Two formats, told apart by their content, plain or gzipped:
InfluxDB annotated CSV (`influx query --raw`, or the UI's CSV download; several
tables per file, one row per field or pivoted) and line protocol. Everything
else in the files (other measurements, other fields) is ignored. A series
belongs to the link whose label (else name) is its `leg` tag, case blind and
after the sanitizing cell-monitor does to tags; with no `leg`, to the link
whose label has its `carrier`; what maps to none is reported. cell-monitor
posts no timestamps: the times are InfluxDB's own clock when each POST came
in, in whole seconds, so that clock has to agree with the Pi's (--rf-offset
moves it). The section reports, per link and over the hours where both the
recording and the RF data exist: the time share of each RAT and band;
SINR, RSRQ and RSRP p10/p50/p90; RAT, band and serving-cell changes (per hour);
for each kind of link event (outage, upload mute, download mute) the share of
event starts with a RAT, band or cell change within +-N s (N = 2 RF intervals,
at least 20 s), against the share a random time would have (the part of the
time those windows cover). That chance is worked out twice: over all the time
(binomial test) and, for each event, over the time within L s of it (L =
30 min, --rf-local; Poisson-binomial test), so that hours in which outages and
radio changes both come thick (a moving car, a crowded evening) do not read as
one announcing the other; the reading is on the local one. The same for the SINR
of the minute before an event against the link's own: the whole link, and the
neighbourhood of the event without [e-60 s, e+N]. And a table per hour.

Export (--export DIR, --export-gzip): tidy CSVs for R and pandas, written while
the analysis goes through the recording: samples.csv (a row per link per status
sample, with what fieldrec derives), events.csv (outages, upload and download
mutes, gaps and restarts), joint.csv (late packets per interval, deadline and
set of links), tunnel.csv (lost_all and, before 0.12, WireGuard sizes per interval), reorder.csv
(the reorderer per interval in long format: the growth of each counter, the
gauges and the histogram buckets; only if the recording has it) and, with
--rf, rf.csv and rf_changes.csv; README.txt in DIR lists their columns, units
and how the clocks relate. The columns are the schemas *_COLS below.

Usage: bench/fieldrec.py [--utc-offset H] [--json OUT] DIR|FILE...
                         [--export DIR [--export-gzip]]
                         [--rf FILE... [--rf-offset S] [--rf-window S]
                          [--rf-local S] [--rf-map LEG=LINK] [--rf-precision P]]
"""

import argparse
import bisect
import calendar
import collections
import csv
import glob
import gzip
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

GAP_MS = 5000          # longer between two samples: a gap, not an interval
MIN_PKTS = 20          # download packets in an interval to judge its loss
UP_WIN_MS = 10000      # upload loss over windows this long
RTT_MED_N = 60         # samples in the rolling median for RTT deviations
EVENT_SLOP_MS = 2000   # A's outage overlaps B's if within this
JOINT_MIN_PKTS = 1000  # packets sent on a set of links to give its probability
JOINT_MAX_LINKS = 8    # the engine's histograms hold link ids 0..7
MD_SUBSETS = 40        # sets per deadline in the report (the JSON has them all)
MD_PAIRS = 10          # pair columns in the hourly table of the report


def files_of(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            out += sorted(glob.glob(os.path.join(p, "rec-*.jsonl*")))
            cur = os.path.join(p, "current.jsonl")
            if os.path.exists(cur):
                out.append(cur)
        else:
            out.append(p)
    return out


def read_lines(files, notes):
    """JSON objects from the files, in order; a file cut by a power loss
    gives what it has."""
    bad = 0
    for f in files:
        try:
            fh = gzip.open(f, "rt") if f.endswith(".gz") else open(f)
            with fh:
                for line in fh:
                    try:
                        yield json.loads(line)
                    except ValueError:
                        bad += 1
        except (EOFError, OSError) as e:
            notes.append(f"{os.path.basename(f)}: cut short ({e.__class__.__name__}), kept what it had")
    if bad:
        notes.append(f"{bad} lines that are not JSON (cut by a power loss?) skipped")


def link_name(l):
    name = l.get("name") or str(l.get("id"))
    if l.get("label"):
        name = f"{name} ({l['label']})"
    return name


def count_or_none(x):
    return x if isinstance(x, int) and not isinstance(x, bool) and x >= 0 else None


def buckets(d):
    """{"7": 120000, ...} -> {7: 120000, ...}, the nonzero ones."""
    out = {}
    for k, v in d.items():
        k, v = int(k), int(v)
        if k < 0 or v < 0:
            raise ValueError("negative bucket")
        if v:
            out[k] = v
    return out


def parse_joint(j):
    """The status field "joint", or None if absent or not as expected."""
    if not isinstance(j, dict):
        return None
    try:
        dl = tuple(int(x) for x in j["deadlines_ms"])
        late = [buckets(x) for x in j["late"]]
        if not dl or len(late) != len(dl):
            return None
        return dict(dl=dl, packets=int(j["packets"]), other=int(j.get("other") or 0),
                    expect=buckets(j["expect"]), late=late)
    except (KeyError, TypeError, ValueError, AttributeError):
        return None


HANDSHAKES = ("init", "response", "cookie", "other")


def parse_sizes(z):
    """The status field "sizes", or None if absent or not as expected."""
    if not isinstance(z, dict):
        return None
    try:
        edges = tuple(int(x) for x in z["edges"])
        out = dict(edges=edges)
        for w in ("up", "down"):
            d = z[w]
            data = [int(x) for x in d["data"]]
            nbytes = [int(x) for x in d["data_bytes"]]
            if len(data) != len(edges) + 1 or len(nbytes) != len(data) or min(data + nbytes) < 0:
                return None
            out[w] = dict(data=data, data_bytes=nbytes, **{k: int(d.get(k) or 0) for k in HANDSHAKES})
        return out
    except (KeyError, TypeError, ValueError, AttributeError):
        return None


# The receiver's reorderer of the bonding step 1: the status field "reorder", the client's top-level one or the one of each
# session of a server. Its counters are cumulative since the engine started, the gauges are not.
RO_COUNTS = ("in_order", "held_total", "bypassed", "split_rx", "far", "resyncs", "pool_full",
             "lost_rack", "lost_cap", "lost_overflow", "lost_flush",
             "late_after_rack", "late_after_cap", "late_after_overflow", "late_old")
RO_LOST = ("rack", "cap", "overflow", "flush")             # reorder.lost.*
RO_LATE = ("after_rack", "after_cap", "after_overflow", "old")   # reorder.late.*
RO_GAUGES = ("held", "held_max", "pool_buffers", "pool_free", "max_burst")   # held and pool_free now; held_max and max_burst since the start
RO_HISTS = ("wait", "late_by")
RO_WORST = 10          # intervals listed as the worst ones of a section


def whole(x):
    """A count of the status: a whole number from 0 up (not a boolean), or ValueError."""
    if isinstance(x, bool) or not isinstance(x, int) or x < 0:
        raise ValueError("not a count")
    return x


class Reorder:
    __slots__ = ("enabled", "edges", "gauges", "counts", "wait", "late_by")


def parse_reorder(o):
    """The status field "reorder", or None if absent or not as expected. A
    counter the engine does not report is 0; one that is there but is not a
    count makes the whole field count as absent. The pool gauges only come
    from a client, and the histograms need edges_ms (one more bucket than
    edges)."""
    if not isinstance(o, dict):
        return None
    try:
        r = Reorder()
        r.enabled = o.get("enabled", True)
        lost, late = o.get("lost", {}), o.get("late", {})
        if not isinstance(r.enabled, bool) or not isinstance(lost, dict) or not isinstance(late, dict):
            return None
        if r.enabled and not all(k in o for k in ("in_order", "held_total", "lost", "late")):
            return None
        r.edges = tuple(whole(x) for x in o.get("edges_ms") or ())

        def count(d, k):
            return 0 if d.get(k) is None else whole(d[k])
        r.counts = (tuple(count(o, k) for k in RO_COUNTS[:7]) + tuple(count(lost, k) for k in RO_LOST)
                    + tuple(count(late, k) for k in RO_LATE))
        r.gauges = tuple(None if o.get(k) is None else whole(o[k]) for k in RO_GAUGES)
        for k in RO_HISTS:
            h = o.get(k)
            h = None if h is None else tuple(whole(x) for x in h)
            if h is not None and len(h) != len(r.edges) + 1:
                return None
            setattr(r, k, h)
        return r
    except (TypeError, ValueError, AttributeError):
        return None


def parse_pool(p):
    """A server's "reorder_pool" as (buffers, free, sessions_holding, full), or None."""
    if not isinstance(p, dict):
        return None
    try:
        return tuple(whole(p[k]) for k in ("buffers", "free", "sessions_holding", "full"))
    except (KeyError, TypeError, ValueError):
        return None


# A week at 1 Hz is 600 000 samples, which are all kept: of each link only what is read is kept (a full link is 35 keys
# and 2.5 KB parsed), and equal strings are one object. A key read anywhere below has to be here.
LINK_KEYS = ("id", "name", "label", "state", "last_rx_ms_ago", "rtt_ms", "upload", "upload_behind_ms", "upload_mutes",
             "upload_state_ms", "download_muted", "tx_packets", "rx_first", "rx_duplicate", "rx_late", "split_share",
             "split_behind_ms")
# What a link does in the upload split (links[].split_share, step 1's guard G4), in the report's order.
SPLIT_SHARES = ("yes", "spread", "waiting", "weight 0", "no")
SHARED = {}


def slim_link(l):
    out = {}
    for k in LINK_KEYS:
        v = l.get(k)
        if v is not None:
            out[k] = SHARED.setdefault(v, v) if isinstance(v, str) else v
    sv = l.get("server_view")
    if isinstance(sv, dict) and "rx" in sv:
        out["server_view"] = {"rx": sv["rx"]}
    return out


class Sample:
    __slots__ = ("up", "wall", "session", "probe", "down", "links", "synced",
                 "ids", "idkey", "lost_all", "joint", "sizes", "reorder")

    def __init__(self, r):
        self.up = int(r["uptime_ms"])
        if "time_ms" in r:
            self.wall = int(r["time_ms"])
        elif "t" in r:
            self.wall = int(float(r["t"]) * 1000)
        else:
            self.wall = None
        self.session = r.get("session")
        if isinstance(self.session, str):
            self.session = SHARED.setdefault(self.session, self.session)
        self.probe = int(r.get("probe_interval_ms") or 100)
        self.down = int((r.get("download") or {}).get("packets") or 0)
        self.links = {}
        self.ids = {}                  # link id (the bit in the engine's masks) -> name
        for l in r.get("links") or []:
            l = slim_link(l)
            name = link_name(l)
            name = SHARED.setdefault(name, name)
            self.links[name] = l
            if isinstance(l.get("id"), int):
                self.ids[l["id"]] = name
        self.idkey = tuple(sorted(self.ids.items()))
        self.lost_all = count_or_none(r.get("lost_all"))
        self.joint = parse_joint(r.get("joint"))
        self.sizes = parse_sizes(r.get("sizes"))
        self.reorder = parse_reorder(r.get("reorder"))
        self.synced = True


class ServerSample:
    """The status of a server (mode "server"), of which only the reordering of
    each session is read: sessions maps a session's id to (its Reorder, the
    packets it delivered so far, its client's name or None);
    pool is parse_pool's tuple."""
    __slots__ = ("up", "wall", "coarse", "synced", "sessions", "pool")

    def __init__(self, r):
        self.up = int(r["uptime_ms"])
        self.coarse = "time_ms" not in r and "t" in r
        if "time_ms" in r:
            self.wall = int(r["time_ms"])
        elif "t" in r:
            self.wall = int(float(r["t"]) * 1000)
        else:
            self.wall = None
        self.sessions = {}
        for x in r.get("sessions") or []:
            if not isinstance(x, dict) or not isinstance(x.get("id"), str):
                continue
            ro = parse_reorder(x.get("reorder"))
            if ro is not None:
                sid = SHARED.setdefault(x["id"], x["id"])
                client = x.get("client") if isinstance(x.get("client"), str) else None
                self.sessions[sid] = (ro, count_or_none(x.get("upload_packets")), SHARED.setdefault(client, client))
        self.pool = parse_pool(r.get("reorder_pool"))
        self.synced = True


def mark_synced(segs, tol):
    """Sets .synced on the samples of the runs: a router without an RTC has
    the wrong wall time until NTP sets it, and within a run the offset
    between wall time and uptime is fixed, so the samples whose offset
    differs from the run's last one predate the correction. Returns how many
    do (or have no wall time at all)."""
    unsynced = 0
    for seg in segs:
        last = next((s for s in reversed(seg) if s.wall is not None), None)
        for s in seg:
            s.synced = last is not None and s.wall is not None and \
                abs((s.wall - s.up) - (last.wall - last.up)) <= tol
            unsynced += not s.synced
    return unsynced


def server_segments(samples, notes):
    """Runs of a server's samples without a gap or a restart (its uptime
    going back); repeated samples (same uptime) dropped."""
    segs, cur = [], []
    for s in samples:
        if cur:
            p = cur[-1]
            if s.up == p.up:
                continue
            if s.up < p.up or s.up - p.up > GAP_MS:
                segs.append(cur)
                cur = []
        cur.append(s)
    if cur:
        segs.append(cur)
    unsynced = mark_synced(segs, 2000 if any(s.coarse for s in samples) else 1000)
    if unsynced:
        notes.append(f"{unsynced} samples of the server before its clock was set (or without one): out of the hours")
    return segs


def segments(lines, notes, server=None):
    """Runs of samples from one engine run without gaps; repeated samples
    (same uptime) dropped. With a list as server, the samples of a server's
    recording (mode "server") are put in it, for server_segments."""
    segs, cur, wall_s = [], [], False
    for r in lines:
        if server is not None and isinstance(r, dict) and r.get("mode") == "server" and "uptime_ms" in r:
            server.append(ServerSample(r))
            continue
        if not isinstance(r, dict) or r.get("mode") != "client" or "uptime_ms" not in r:
            continue
        s = Sample(r)
        wall_s = wall_s or ("time_ms" not in r and "t" in r)
        if cur:
            p = cur[-1]
            if s.up == p.up and s.session == p.session:
                continue
            if s.up < p.up or s.session != p.session or s.up - p.up > GAP_MS:
                segs.append(cur)
                cur = []
        cur.append(s)
    if cur:
        segs.append(cur)
    if wall_s:
        notes.append("an engine without time_ms: wall clock from the recorder, to the second")
    # A router without an RTC: before NTP sets the clock, its wall time is
    # wrong. Within a run the offset to uptime is fixed, so samples whose
    # offset differs from the run's last one predate the correction.
    unsynced = mark_synced(segs, 2000 if wall_s else 1000)
    if unsynced:
        notes.append(f"{unsynced} samples before the clock was set (or without one): out of the wall-clock folds")
    return segs


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]


def link_out(l, probe_ms):
    if l.get("state") != "live":
        return True
    return (l.get("last_rx_ms_ago") or 0) > max(3 * probe_ms, 300)


def analyze(segs, utc_offset_h=0.0, ex=None, server=()):
    """segs: the client's runs (segments); server: those of a server's
    recording (server_segments), of which only the reordering is read."""
    links = sorted({n for seg in segs for s in seg for n in s.links})
    L = {n: dict(time=0, out_time=0, events=[], rtt=[], dev=[], behind=[], up_behind=[],
                 loss_num=0.0, loss_den=0, up_tx=0, up_rx=0, mutes=0, muted_time=0,
                 split=collections.Counter(), split_leaves=0, split_behind=[],
                 starts=[], steps=[], hours=collections.defaultdict(lambda: [0, 0, [], 0.0, 0]),
                 # For the radio section: where the link was observed (wall ms, as
                 # [start, end] runs) and the wall ms at which its upload and
                 # download mutes began.
                 cover=[], mute_starts=[], dmute_starts=[])
         for n in links}
    meta = {}                          # link name -> (name, label) of the status

    simul = collections.Counter()      # (links present, links out) -> ms
    pair_t = collections.Counter()     # (a, b) -> ms both present
    pair_out = collections.Counter()   # (a, b, which) -> ms out: 'a', 'b', 'ab'
    spread_all, spread_two = [], []
    corr = collections.defaultdict(lambda: [0, 0.0, 0.0, 0.0, 0.0, 0.0])  # (a, b) -> n, sx, sy, sxx, syy, sxy
    total_ms = gap_ms = 0
    for si, seg in enumerate(segs):
        if si:
            prev = segs[si - 1][-1]
            if prev.wall is not None and seg[0].wall is not None and seg[0].wall > prev.wall:
                gap_ms += seg[0].wall - prev.wall
        if ex is not None:
            ex.segment_begin(si, seg, segs[si - 1][-1] if si else None)
        hist = collections.defaultdict(list)   # rolling RTTs for the median
        # An outage runs from its last reply (stamped by the engine) to
        # halfway between its last sample out and the first one back.
        state = {n: dict(out=False, start=None, last=None, cov=None, wall0=None) for n in links}
        upwin = {n: [0, 0, 0] for n in links}  # tx, rx, ms
        for idx, (a, b) in enumerate(zip(seg, seg[1:]), 1):
            rows = {}                  # what is derived for each link, for the export
            dt = b.up - a.up
            total_ms += dt
            tick_dev = {}
            hour = None
            if b.synced:
                hour = int(((b.wall / 3600000.0) + utc_offset_h) % 24)
            present, outs, rtts = [], set(), {}
            dD = b.down - a.down
            for n, l in b.links.items():
                if n not in meta:
                    meta[n] = (l.get("name") or "", l.get("label") or "")
                xr = rows[n] = {}
                if l.get("state") == "paused":
                    continue
                d = L[n]
                present.append(n)
                out = xr["out"] = link_out(l, b.probe)
                d["time"] += dt
                # The upload split, while the client splits (split_share null: it does not).
                sh = l.get("split_share")
                if isinstance(sh, str):
                    d["split"][sh] += dt
                    pa = a.links.get(n)
                    if sh == "spread" and (pa is None or pa.get("split_share") != "spread"):
                        d["split_leaves"] += 1
                    if num(l.get("split_behind_ms")) is not None:
                        d["split_behind"].append(float(l["split_behind_ms"]))
                st = state[n]
                if b.synced:
                    cv = d["cover"]
                    if cv and st["cov"] == a.up:
                        cv[-1][1] = b.wall
                    else:
                        cv.append([b.wall - dt, b.wall])
                    st["cov"] = b.up
                if out:
                    outs.add(n)
                    d["out_time"] += dt
                    if not st["out"]:
                        ago = l.get("last_rx_ms_ago") or 0
                        st["out"], st["start"] = True, b.up - min(ago, dt)
                        st["wall0"] = b.wall - ago if b.synced else None
                        if b.synced:
                            d["starts"].append(b.wall - ago)
                    st["last"] = b.up
                else:
                    if st["out"]:
                        end = (st["last"] + b.up) / 2
                        d["events"].append(((si, st["start"]), end - st["start"]))
                        if ex is not None:
                            ex.outage(n, si, st["start"], end, st["wall0"], "recovered")
                        st["out"] = False
                    r = float(l.get("rtt_ms") or 0)
                    if r > 0:
                        rtts[n] = r
                        d["rtt"].append(r)
                        h = hist[n]
                        h.append(r)
                        if len(h) > RTT_MED_N:
                            h.pop(0)
                        if len(h) >= RTT_MED_N // 2:
                            dv = r - statistics.median(h)
                            d["dev"].append(abs(dv))
                            tick_dev[n] = dv
                            xr["dev"] = round(dv, 3)
                        pa = a.links.get(n)
                        if pa and not link_out(pa, a.probe):
                            ra = float(pa.get("rtt_ms") or 0)
                            if ra > 0:
                                xr["step"] = round(abs(r - ra), 3)
                                if b.synced:
                                    d["steps"].append((b.wall, abs(r - ra)))
                    if l.get("upload_behind_ms") is not None:
                        d["up_behind"].append(float(l["upload_behind_ms"]))
                if l.get("upload") == "muted":
                    d["muted_time"] += dt
                pa = a.links.get(n)
                if pa is not None:
                    xr["mdelta"] = max(0, (l.get("upload_mutes") or 0) - (pa.get("upload_mutes") or 0))
                    d["mutes"] += xr["mdelta"]
                    if b.synced:
                        # A mute begins where the counter grew or the state turned
                        # to muted; the engine stamps how long the state has lasted.
                        um = max((l.get("upload_mutes") or 0) - (pa.get("upload_mutes") or 0),
                                 int(pa.get("upload") != "muted" and l.get("upload") == "muted"))
                        age = l.get("upload_state_ms") if l.get("upload") == "muted" else None
                        if um > 0:
                            d["mute_starts"] += [b.wall - (min(age, dt) if isinstance(age, (int, float)) and age >= 0 else 0)] * um
                        if l.get("download_muted") and not pa.get("download_muted"):
                            d["dmute_starts"].append(b.wall)
                    got = sum((l.get(c) or 0) - (pa.get(c) or 0) for c in ("rx_first", "rx_duplicate", "rx_late"))
                    xr["got"], xr["rx_first"] = got, (l.get("rx_first") or 0) - (pa.get("rx_first") or 0)
                    if dD >= MIN_PKTS and not l.get("download_muted"):
                        loss = min(1.0, max(0.0, 1 - got / dD))
                        xr["loss"] = round(loss, 6)
                        d["loss_num"] += loss * dD
                        d["loss_den"] += dD
                        if hour is not None:
                            d["hours"][hour][3] += loss * dD
                            d["hours"][hour][4] += dD
                    tx = (l.get("tx_packets") or 0) - (pa.get("tx_packets") or 0)
                    rx = ((l.get("server_view") or {}).get("rx", 0) - (pa.get("server_view") or {}).get("rx", 0)) % 2**32
                    xr["tx"], xr["srv_rx"] = tx, rx
                    w = upwin[n]
                    w[0] += tx
                    w[1] += rx
                    w[2] += dt
                    # The server's count comes with its last probe reply: over
                    # whole windows its lag cancels out (the sums telescope).
                    if w[2] >= UP_WIN_MS:
                        if w[0] >= 50:
                            d["up_tx"] += w[0]
                            d["up_rx"] += w[1]
                        upwin[n] = [0, 0, 0]
                if hour is not None:
                    hh = d["hours"][hour]
                    hh[0] += dt
                    hh[1] += dt if out else 0
                    if n in rtts:
                        hh[2].append(rtts[n])
            simul[(len(present), len(outs))] += dt
            for i, x in enumerate(present):
                for y in present[i + 1:]:
                    k = tuple(sorted((x, y)))
                    pair_t[k] += dt
                    if k[0] in outs:
                        pair_out[k + ("a",)] += dt
                    if k[1] in outs:
                        pair_out[k + ("b",)] += dt
                    if x in outs and y in outs:
                        pair_out[k + ("ab",)] += dt
            dvs = sorted(tick_dev.items())
            for i, (x, dx) in enumerate(dvs):
                for y, dy in dvs[i + 1:]:
                    c = corr[(x, y)]
                    c[0] += 1
                    c[1] += dx
                    c[2] += dy
                    c[3] += dx * dx
                    c[4] += dy * dy
                    c[5] += dx * dy
            if len(rtts) >= 2:
                v = sorted(rtts.values())
                spread_all.append(v[-1] - v[0])
                spread_two.append(v[1] - v[0])
                for n, r in rtts.items():
                    L[n]["behind"].append(r - v[0])
                    rows[n]["behind"] = round(r - v[0], 3)
            if ex is not None:
                ex.interval(si, idx, a, b, dt, rows)
        for n, st in state.items():
            if st["out"]:
                L[n]["events"].append(((si, st["start"]), st["last"] - st["start"]))
                if ex is not None:
                    ex.outage(n, si, st["start"], st["last"], st["wall0"], "segment_end")
        if ex is not None:
            ex.segment_end(si)
    bond = collect_bonding(segs, utc_offset_h, ex)
    bond["reorder"] = collect_reorder(segs, server, utc_offset_h, ex)
    return dict(links=links, L=L, simul=simul, pair_t=pair_t, pair_out=pair_out, corr=corr,
                spread_all=spread_all, spread_two=spread_two, total_ms=total_ms, gap_ms=gap_ms,
                segments=len(segs), samples=sum(len(s) for s in segs), meta=meta, bond=bond)


def delta_buckets(b, a):
    """Growth of each bucket between two cumulative histograms; None if one
    of them went down."""
    for k, v in a.items():
        if b.get(k, 0) < v:
            return None
    return {k: v - a.get(k, 0) for k, v in b.items() if v != a.get(k, 0)}


def delta_sizes(b, a):
    """Growth of the "sizes" counters between two samples; None if one went down."""
    out = {}
    for w in ("up", "down"):
        d = {}
        for k in ("data", "data_bytes"):
            d[k] = [x - y for x, y in zip(b[w][k], a[w][k])]
        for k in HANDSHAKES:
            d[k] = b[w][k] - a[w][k]
        if min(d["data"] + d["data_bytes"] + [d[k] for k in HANDSHAKES]) < 0:
            return None
        out[w] = d
    return out


def merge_ids(ka, kb):
    """Link ids of two consecutive samples: the later one's names win."""
    return tuple(sorted({**dict(ka), **dict(kb)}.items()))


class JointAcc:
    __slots__ = ("packets", "other", "expect", "late")

    def __init__(self, nd):
        self.packets = self.other = 0
        self.expect = collections.Counter()
        self.late = [collections.Counter() for _ in range(nd)]


def collect_bonding(segs, utc_offset_h=0.0, ex=None):
    """Differences of the cumulative counters "joint", "lost_all" and "sizes"
    between consecutive samples of a run, added up overall (key -1) and per
    hour of the day (the hour of the later sample, as for the links). An
    interval where a counter went down, or the layout changed, is skipped and
    counted. The masks stay in link ids, per id->name table, until summarize()."""
    joint = dict(dl=None, acc={}, intervals=0, skipped=0)
    lost = dict(acc=collections.defaultdict(lambda: [0, 0]), intervals=0, skipped=0)
    sizes = dict(edges=None, intervals=0, skipped=0)
    for w in ("up", "down"):
        sizes[w] = dict(data=None, data_bytes=None, **{k: 0 for k in HANDSHAKES})
    for si, seg in enumerate(segs):
        for a, b in zip(seg, seg[1:]):
            keys = (-1,)
            lost_d = size_d = None             # what this interval gives the export
            if b.synced:
                keys = (-1, int(((b.wall / 3600000.0) + utc_offset_h) % 24))
            if a.joint is not None and b.joint is not None:
                ja, jb = a.joint, b.joint
                if joint["dl"] is None:
                    joint["dl"] = jb["dl"]
                de = dls = None
                if ja["dl"] == jb["dl"] == joint["dl"] and jb["packets"] >= ja["packets"] and jb["other"] >= ja["other"]:
                    de = delta_buckets(jb["expect"], ja["expect"])
                    dls = [delta_buckets(x, y) for x, y in zip(jb["late"], ja["late"])]
                if de is None or any(x is None for x in dls):
                    joint["skipped"] += 1
                else:
                    joint["intervals"] += 1
                    ek = b.idkey if a.idkey == b.idkey else merge_ids(a.idkey, b.idkey)
                    if ex is not None:
                        ex.joint(si, a, b, jb["dl"], de, dls, ek)
                    for h in keys:
                        acc = joint["acc"].get((ek, h))
                        if acc is None:
                            acc = joint["acc"][(ek, h)] = JointAcc(len(jb["dl"]))
                        acc.packets += jb["packets"] - ja["packets"]
                        acc.other += jb["other"] - ja["other"]
                        acc.expect.update(de)
                        for hist, d in zip(acc.late, dls):
                            hist.update(d)
            if a.lost_all is not None and b.lost_all is not None:
                dlost, ddown = b.lost_all - a.lost_all, b.down - a.down
                if dlost < 0 or ddown < 0:
                    lost["skipped"] += 1
                else:
                    lost["intervals"] += 1
                    lost_d = dlost
                    for h in keys:
                        lost["acc"][h][0] += dlost
                        lost["acc"][h][1] += ddown
            if a.sizes is not None and b.sizes is not None:
                if sizes["edges"] is None:
                    sizes["edges"] = b.sizes["edges"]
                d = delta_sizes(b.sizes, a.sizes) if a.sizes["edges"] == b.sizes["edges"] == sizes["edges"] else None
                if d is None:
                    sizes["skipped"] += 1
                else:
                    sizes["intervals"] += 1
                    size_d = d
                    for w in ("up", "down"):
                        s = sizes[w]
                        for k in ("data", "data_bytes"):
                            s[k] = d[w][k] if s[k] is None else [x + y for x, y in zip(s[k], d[w][k])]
                        for k in HANDSHAKES:
                            s[k] += d[w][k]
            if ex is not None:
                ex.tunnel(si, a, b, lost_d, size_d)
    return dict(joint=joint, lost=lost, sizes=sizes)


RO_I = {k: i for i, k in enumerate(RO_COUNTS)}
RO_FAILED = ("lost_cap", "lost_overflow") + tuple("late_" + k for k in RO_LATE)   # where the wait did not do its job


def delta_reorder(b, a):
    """Growth between two samples of a Reorder's counters and histograms as
    (counts, wait, late_by), or None if one went down or the layout (the
    edges, which histograms there are) changed."""
    if a.edges != b.edges:
        return None
    out = [tuple(y - x for x, y in zip(a.counts, b.counts))]
    for k in RO_HISTS:
        x, y = getattr(a, k), getattr(b, k)
        if (x is None) != (y is None):
            return None
        out.append(None if x is None else tuple(q - p for p, q in zip(x, y)))
    if any(h is not None and min(h) < 0 for h in out):
        return None
    return tuple(out)


class ROAcc:
    """What a reorderer did over some intervals: the growth of its counters
    added up, and the extremes of its gauges."""
    __slots__ = ("intervals", "delivered", "counts", "wait", "late_by", "held_seen", "held_max", "max_burst",
                 "pool_buffers", "pool_free_min")

    def __init__(self):
        self.intervals = self.delivered = self.held_seen = self.held_max = self.max_burst = 0
        self.counts = [0] * len(RO_COUNTS)
        self.wait = self.late_by = self.pool_buffers = self.pool_free_min = None

    def add(self, d, delivered, rb):
        self.intervals += 1
        self.delivered += delivered
        self.counts = [x + y for x, y in zip(self.counts, d[0])]
        for k, h in zip(RO_HISTS, d[1:]):
            if h is not None:
                cur = getattr(self, k)
                setattr(self, k, list(h) if cur is None else [x + y for x, y in zip(cur, h)])
        held, held_max, bufs, free, burst = rb.gauges
        self.held_seen = max(self.held_seen, held or 0)
        self.held_max = max(self.held_max, held_max or 0)
        self.max_burst = max(self.max_burst, burst or 0)
        if bufs is not None:
            self.pool_buffers = max(self.pool_buffers or 0, bufs)
        if free is not None:
            self.pool_free_min = free if self.pool_free_min is None else min(self.pool_free_min, free)


def ro_owner():
    """What is kept of one reorderer (the client's, or one session's): its
    ROAcc overall (key -1) and per hour, the intervals counted, those where it
    was off, those skipped, and its worst intervals."""
    return dict(acc={}, intervals=0, off=0, skipped=0, edges=None, client=None, worst=[])


def collect_reorder(segs, server=(), utc_offset_h=0.0, ex=None):
    """The reorderer's counters (the status field "reorder": the client's, and
    each session's of a server's recording) differenced between consecutive
    samples of a run and added up, overall (key -1) and per hour of the day
    (the hour of the later sample), as the other instruments are. An interval
    where reordering was off at either end is counted apart, and one where a
    counter went down or the layout changed is skipped and counted: neither is
    added, and neither is exported. Returns the client's owner, the sessions'
    by id, and the server's pool."""
    client, sessions = ro_owner(), {}
    pool = dict(acc={}, intervals=0, skipped=0)

    def hours(b):
        if b.synced and b.wall is not None:
            return (-1, int(((b.wall / 3600000.0) + utc_offset_h) % 24))
        return (-1,)

    def step(o, ra, rb, delivered, keys, b, si, dt):
        """One interval of one reorderer; the delta it added, or None."""
        if not ra.enabled or not rb.enabled:
            o["off"] += 1
            return None
        d = delta_reorder(rb, ra)
        if d is not None and o["edges"] not in (None, rb.edges):     # another layout than the first interval's: not added
            d = None
        if d is None:
            o["skipped"] += 1
            return None
        o["edges"] = rb.edges
        o["intervals"] += 1
        for h in keys:
            acc = o["acc"].get(h)
            if acc is None:
                acc = o["acc"][h] = ROAcc()
            acc.add(d, delivered, rb)
        failed = sum(d[0][RO_I[k]] for k in RO_FAILED)
        if failed:
            o["worst"].append((-failed, si, b.up, dict(
                segment=si, time_utc=iso(b.wall / 1000.0) if b.synced and b.wall is not None else None, uptime_ms=b.up,
                interval_ms=dt, delivered=delivered, held_total=d[0][RO_I["held_total"]], lost_cap=d[0][RO_I["lost_cap"]],
                lost_overflow=d[0][RO_I["lost_overflow"]], late=sum(d[0][RO_I["late_" + k]] for k in RO_LATE), held=rb.gauges[0])))
            if len(o["worst"]) > 4 * RO_WORST:
                o["worst"].sort(key=lambda x: x[:3])
                del o["worst"][RO_WORST:]
        return d

    for si, seg in enumerate(segs):
        for a, b in zip(seg, seg[1:]):
            if a.reorder is None or b.reorder is None:
                continue
            delivered = max(0, b.down - a.down)
            d = step(client, a.reorder, b.reorder, delivered, hours(b), b, si, b.up - a.up)
            if d is not None and ex is not None:
                ex.reorder("client", si, b, b.session, "", delivered, d, b.reorder, b.up - a.up)
    for si, seg in enumerate(server):
        for a, b in zip(seg, seg[1:]):
            keys = hours(b)
            for sid, (rb, pk_b, name) in b.sessions.items():
                before = a.sessions.get(sid)
                if before is None:                    # a session of its own since this sample: it has no interval yet
                    continue
                ra, pk_a, _ = before
                o = sessions.setdefault(sid, ro_owner())
                o["client"] = name or o["client"]
                delivered = max(0, pk_b - pk_a) if pk_a is not None and pk_b is not None else 0
                d = step(o, ra, rb, delivered, keys, b, si, b.up - a.up)
                if d is not None and ex is not None:
                    ex.reorder("server", si, b, sid, name or "", delivered, d, rb, b.up - a.up)
            if a.pool is not None and b.pool is not None:
                full = b.pool[3] - a.pool[3]
                if full < 0:
                    pool["skipped"] += 1
                    continue
                pool["intervals"] += 1
                for h in keys:
                    p = pool["acc"].setdefault(h, dict(buffers=0, free_min=None, holding_max=0, full=0))
                    p["buffers"] = max(p["buffers"], b.pool[0])
                    p["free_min"] = b.pool[1] if p["free_min"] is None else min(p["free_min"], b.pool[1])
                    p["holding_max"] = max(p["holding_max"], b.pool[2])
                    p["full"] += full
    return dict(client=client, sessions=sessions, pool=pool)


def pearson(c):
    n, sx, sy, sxx, syy, sxy = c
    if n < 30:
        return None
    vx, vy = n * sxx - sx * sx, n * syy - sy * sy
    if vx <= 0 or vy <= 0:
        return None
    return (n * sxy - sx * sy) / math.sqrt(vx * vy)


def overlap_share(ev_a, ev_b):
    """Share of A's outages with one of B's within EVENT_SLOP_MS. Events are
    ((run, start uptime), duration): only the same run's compare."""
    if not ev_a:
        return None
    hits = 0
    starts_b = sorted(ev_b)
    for (run, s), d in ev_a:
        lo, hi = s - EVENT_SLOP_MS, s + d + EVENT_SLOP_MS
        i = bisect.bisect_left(starts_b, ((run, lo - 86400000), 0))
        for (rb, sb), db in starts_b[i:]:
            if rb != run or sb > hi:
                break
            if sb + db >= lo:
                hits += 1
                break
    return hits / len(ev_a)


def fold(times_ms):
    """Second-of-minute histogram (to the nearest second) and the 15 s fold:
    the peak phase, and the share of events within a second of it (uniform:
    3/15 = 20 %)."""
    if not times_ms:
        return None
    som = collections.Counter(int(round(t / 1000)) % 60 for t in times_ms)
    f15 = [0] * 15
    for s, c in som.items():
        f15[s % 15] += c
    n = len(times_ms)
    best = max(range(15), key=lambda p: (f15[(p - 1) % 15] + f15[p] + f15[(p + 1) % 15], f15[p]))
    share = (f15[(best - 1) % 15] + f15[best] + f15[(best + 1) % 15]) / n
    return dict(n=n, peak=best, share=share, seconds=[best + 15 * k for k in range(4)],
                fold15=f15, minute=[som.get(s, 0) for s in range(60)])


def split_summary(d):
    """A link's time in the upload split by what it did there (links[].split_share), the times the delay guard
    took it out, and its delay behind the fastest as the guard saw it; None when the client never split."""
    t = sum(d["split"].values())
    if not t:
        return None
    return dict(hours=round(t / 3600000, 3),
                pct={k: round(100 * d["split"][k] / t, 2) for k in list(SPLIT_SHARES) + sorted(set(d["split"]) - set(SPLIT_SHARES))},
                leaves=d["split_leaves"],
                behind_ms={p: pct(d["split_behind"], p) for p in (50, 99)})


def summarize(a):
    L, out = a["L"], {"links": {}, "pairs": [], "simultaneous": {}}
    for n in a["links"]:
        d = L[n]
        durs = [x[1] / 1000 for x in d["events"]]
        buckets = collections.Counter()
        for x in durs:
            buckets["<=1s" if x <= 1 else "1-3s" if x <= 3 else "3-10s" if x <= 10 else "10-60s" if x <= 60 else ">60s"] += 1
        step_med = pct([x for _, x in d["steps"]], 50) or 0
        steps = [t for t, s in d["steps"] if s >= max(5.0, 4 * step_med)]
        out["links"][n] = dict(
            hours=round(d["time"] / 3600000, 2),
            up_pct=round(100 * (1 - d["out_time"] / d["time"]), 3) if d["time"] else None,
            outages=len(durs),
            outage_s=dict(p50=pct(durs, 50), p90=pct(durs, 90), max=max(durs) if durs else None,
                          buckets={k: buckets[k] for k in ("<=1s", "1-3s", "3-10s", "10-60s", ">60s")}),
            rtt_ms={p: pct(d["rtt"], p) for p in (50, 95, 99)},
            rtt_dev_ms_p95=pct(d["dev"], 95),
            behind_best_ms={p: pct(d["behind"], p) for p in (50, 99)},
            upload_behind_ms_p99=pct(d["up_behind"], 99),
            loss_down_pct=round(100 * d["loss_num"] / d["loss_den"], 3) if d["loss_den"] else None,
            loss_up_pct=round(100 * max(0.0, 1 - d["up_rx"] / d["up_tx"]), 3) if d["up_tx"] else None,
            mutes=d["mutes"],
            muted_pct=round(100 * d["muted_time"] / d["time"], 3) if d["time"] else None,
            split=split_summary(d),
            starlink_outages=fold(d["starts"]),
            starlink_steps=fold(steps),
            by_hour={h: dict(out_pct=round(100 * v[1] / v[0], 2) if v[0] else None,
                             rtt_p50=pct(v[2], 50),
                             loss_down_pct=round(100 * v[3] / v[4], 2) if v[4] else None)
                     for h, v in sorted(d["hours"].items())},
        )
    tot = sum(a["simul"].values())
    for (n, k), ms in sorted(a["simul"].items()):
        out["simultaneous"][f"{n - k} of {n} live"] = round(100 * ms / tot, 4) if tot else None
    links = a["links"]
    for i, x in enumerate(links):
        for y in links[i + 1:]:
            t = a["pair_t"][(x, y)]
            if not t:
                continue
            pa, pb, pab = (a["pair_out"][(x, y, w)] / t for w in ("a", "b", "ab"))
            out["pairs"].append(dict(
                a=x, b=y, out_a_pct=round(100 * pa, 3), out_b_pct=round(100 * pb, 3),
                both_pct=round(100 * pab, 4),
                b_given_a=round(pab / pa, 3) if pa else None,
                a_given_b=round(pab / pb, 3) if pb else None,
                lift=round(pab / (pa * pb), 1) if pa and pb else None,
                a_events_with_b=overlap_share(L[x]["events"], L[y]["events"]),
                rtt_corr=pearson(a["corr"][(x, y)])))
    out["spread_ms"] = {p: pct(a["spread_all"], p) for p in (50, 90, 99)}
    out["spread_best_two_ms"] = {p: pct(a["spread_two"], p) for p in (50, 90, 99)}
    out["hours"] = round(a["total_ms"] / 3600000, 2)
    out["gaps_hours"] = round(a["gap_ms"] / 3600000, 2)
    out["segments"] = a["segments"]
    out["samples"] = a["samples"]
    out["joint"] = summarize_joint(a["bond"]["joint"], a)
    out["lost_all"] = summarize_lost(a["bond"]["lost"])
    out["sizes"] = summarize_sizes(a["bond"]["sizes"])
    out["reorder"] = summarize_reorder(a["bond"]["reorder"])
    return out


def sig(x, n=6):
    return None if x is None else float(f"{x:.{n}g}")


def pp(p):
    """A probability as a percentage, 6 significant digits."""
    return None if p is None else sig(100 * p)


def ratio(num, den):
    """num / den: 1 when they are equal (also both 0), infinity when only
    den is 0, None when either is unknown."""
    if num is None or den is None:
        return None
    if num == den:
        return 1.0
    return math.inf if den == 0 else num / den


def finite(x):
    return None if x is None or math.isinf(x) else sig(x, 4)


def ones(m):
    return bin(m).count("1")


def mask_names(m, idm):
    """The link names of a mask over link ids (a name "idN" if unknown)."""
    out, i = [], 0
    while m:
        if m & 1:
            out.append(idm.get(i, f"id{i}"))
        m >>= 1
        i += 1
    return frozenset(out)


def superset_sums(hist, nb):
    """f[S] = sum of hist[m] over the masks m that contain S (all of S's bits)."""
    f = [0] * (1 << nb)
    for m, c in hist.items():
        f[m] += c
    for b in range(nb):
        bit = 1 << b
        for m in range(1 << nb):
            if not m & bit:
                f[m] += f[m | bit]
    return f


def joint_view(names, dl, expect, late, packets, other, rtt):
    """P(all of S late or lost) for each deadline and each set S of 2 or more
    links, over the packets sent on every link of S. expect and late[i] are
    histograms over masks of names' indexes (late[i]: the links late at
    deadline i); the single-link figure is over the packets sent on that
    link. independent = the product of the singles; late_if_indep = what that
    would give in packets. Best pair: lowest P(both late) among all pairs, by
    each link's own late share (the two lowest) and by the RTT p50 (rtt)."""
    nb = len(names)
    E = superset_sums(expect, nb)
    sets = sorted((m for m in range(1, 1 << nb) if ones(m) >= 2),
                  key=lambda m: (ones(m), [i for i in range(nb) if m >> i & 1]))
    full = (1 << nb) - 1
    rr = sorted((i for i in range(nb) if rtt.get(names[i])), key=lambda i: (rtt[names[i]], i))
    by_rtt = (1 << rr[0]) | (1 << rr[1]) if len(rr) >= 2 else None
    views = []
    for di, d in enumerate(dl):
        Lt = superset_sums(late[di], nb)
        p = [None] * (1 << nb)
        for m in range(1, 1 << nb):
            if E[m] >= JOINT_MIN_PKTS:
                p[m] = Lt[m] / E[m]
        single = {names[i]: dict(packets=E[1 << i], late=Lt[1 << i], late_pct=pp(p[1 << i])) for i in range(nb)}
        subs = []
        for m in sets:
            ids = [i for i in range(nb) if m >> i & 1]
            ind = math.prod(p[1 << i] for i in ids) if all(p[1 << i] is not None for i in ids) else None
            subs.append(dict(set=[names[i] for i in ids], packets=E[m], late=Lt[m], late_pct=pp(p[m]),
                             indep_pct=pp(ind), late_if_indep=sig(ind * E[m]) if ind is not None else None,
                             lift=finite(ratio(p[m], ind)) if ind else None))
        pairs = [m for m in sets if ones(m) == 2 and p[m] is not None]
        by_joint = min(pairs, key=lambda m: (p[m], -E[m], m)) if pairs else None
        rk = sorted((i for i in range(nb) if p[1 << i] is not None), key=lambda i: (p[1 << i], -E[1 << i], i))
        by_single = (1 << rk[0]) | (1 << rk[1]) if len(rk) >= 2 else None
        allm = full if nb >= 2 else None

        def ref(m):
            return None if m is None else dict(set=[names[i] for i in range(nb) if m >> i & 1], late_pct=pp(p[m]))

        pj, ps, pr, pa = (p[m] if m is not None else None for m in (by_joint, by_single, by_rtt, allm))
        views.append(dict(deadline_ms=d, single=single, subsets=subs,
                          best=dict(by_joint=ref(by_joint), by_single=ref(by_single), by_rtt=ref(by_rtt),
                                    all=ref(allm), ratio_single=finite(ratio(ps, pj)),
                                    ratio_rtt=finite(ratio(pr, pj)), pair_over_all=finite(ratio(pj, pa)))))
    return dict(packets=packets, other=other, deadlines=views)


def summarize_joint(J, a):
    if not J["acc"] and not J["skipped"]:
        return None
    dl = list(J["dl"] or [])
    out = dict(deadlines_ms=dl, intervals=J["intervals"], skipped_intervals=J["skipped"],
               packets=0, other=0, other_pct=None, links=[], link_packets={}, left_out=[],
               rtt_p50_ms={}, overall=None, by_hour={})
    if not J["acc"]:
        return out
    # Link ids to names, with the table of the interval's own run.
    agg = {}
    for (ek, h), v in J["acc"].items():
        idm, cache = dict(ek), {}

        def nm(m):
            if m not in cache:
                cache[m] = mask_names(m, idm)
            return cache[m]

        g = agg.setdefault(h, dict(packets=0, other=0, expect=collections.Counter(),
                                   late=[collections.Counter() for _ in dl]))
        g["packets"] += v.packets
        g["other"] += v.other
        for m, n in v.expect.items():
            g["expect"][nm(m)] += n
        for hist, src in zip(g["late"], v.late):
            for m, n in src.items():
                hist[nm(m)] += n
    tot = agg[-1]
    per_link = collections.Counter()
    for fs, n in tot["expect"].items():
        for x in fs:
            per_link[x] += n
    ranked = sorted(per_link, key=lambda x: (-per_link[x], x))
    names = sorted([x for x in ranked if per_link[x] >= JOINT_MIN_PKTS][:JOINT_MAX_LINKS])
    ix = {n: i for i, n in enumerate(names)}

    def proj(hist):
        o = collections.Counter()
        for fs, n in hist.items():
            o[sum(1 << ix[x] for x in fs if x in ix)] += n
        return o

    rtt = {n: pct(a["L"][n]["rtt"], 50) for n in names if n in a["L"]}
    rtt_h = {}
    for n in names:
        for h, v in (a["L"][n]["hours"].items() if n in a["L"] else ()):
            rtt_h.setdefault(h, {})[n] = pct(v[2], 50)

    def view(g, r):
        return joint_view(names, dl, proj(g["expect"]), [proj(x) for x in g["late"]], g["packets"], g["other"], r)

    out.update(packets=tot["packets"], other=tot["other"],
               other_pct=round(100 * tot["other"] / tot["packets"], 3) if tot["packets"] else None,
               links=names, link_packets={x: per_link[x] for x in ranked},
               left_out=[x for x in ranked if x not in ix],
               rtt_p50_ms={n: r for n, r in rtt.items() if r is not None})
    out["overall"] = view(tot, rtt)
    out["by_hour"] = {h: view(g, rtt_h.get(h, {})) for h, g in sorted(agg.items()) if h >= 0}
    return out


def summarize_lost(B):
    if not B["acc"] and not B["skipped"]:
        return None

    def row(v):
        return dict(lost=v[0], delivered=v[1], loss_pct=sig(100 * v[0] / (v[0] + v[1])) if v[0] + v[1] else None)

    out = dict(intervals=B["intervals"], skipped_intervals=B["skipped"], overall=None, by_hour={})
    if B["acc"]:
        out["overall"] = row(B["acc"][-1])
        out["by_hour"] = {h: row(v) for h, v in sorted(B["acc"].items()) if h >= 0}
    return out


def summarize_sizes(B):
    if B["edges"] is None and not B["skipped"]:
        return None
    out = dict(edges=list(B["edges"] or []), intervals=B["intervals"], skipped_intervals=B["skipped"])
    if B["up"]["data"] is None:
        return out
    for w in ("up", "down"):
        s = B[w]
        n, nb = sum(s["data"]), sum(s["data_bytes"])
        rows, cn, cb = [], 0, 0
        for i, (c, by) in enumerate(zip(s["data"], s["data_bytes"])):
            cn += c
            cb += by
            rows.append(dict(up_to=out["edges"][i] if i < len(out["edges"]) else None, packets=c,
                             packets_pct=round(100 * c / n, 3) if n else None,
                             cum_packets_pct=round(100 * cn / n, 3) if n else None, bytes=by,
                             bytes_pct=round(100 * by / nb, 3) if nb else None,
                             cum_bytes_pct=round(100 * cb / nb, 3) if nb else None))
        out[w] = dict(data_packets=n, data_bytes=nb, mean_bytes=round(nb / n, 1) if n else None, buckets=rows,
                      handshakes={k: s[k] for k in HANDSHAKES})
    return out


def ro_hist(counts, edges):
    """A histogram of the reorderer (hold time of each held packet, or how
    late a packet came after its gap was given up) as its buckets and the
    smallest edge that holds a share of the packets: None for the last
    bucket, which has no edge, or for no packets."""
    n = sum(counts)

    def le(pct):                       # whole numbers: no rounding at the edge, where a count is exactly pct % of the total
        cum = 0
        for e, c in zip(edges, counts):
            cum += c
            if n and cum * 100 >= pct * n:
                return e
        return None
    return dict(edges_ms=list(edges), counts=list(counts), total=n,
                pct=[round(100 * c / n, 3) if n else None for c in counts],
                p50_le_ms=le(50), p90_le_ms=le(90), p99_le_ms=le(99))


def ro_view(acc, edges):
    c = dict(zip(RO_COUNTS, acc.counts))
    late = {k: c["late_" + k] for k in RO_LATE}
    return dict(intervals=acc.intervals, delivered=acc.delivered, in_order=c["in_order"], held_total=c["held_total"],
                held_pct=sig(100 * c["held_total"] / acc.delivered) if acc.delivered else None,
                bypassed=c["bypassed"], split_rx=c["split_rx"], lost={k: c["lost_" + k] for k in RO_LOST}, late=late,
                late_total=sum(late.values()), far=c["far"], resyncs=c["resyncs"], pool_full=c["pool_full"],
                held_seen_max=acc.held_seen, held_max=acc.held_max, max_burst=acc.max_burst,
                pool_buffers=acc.pool_buffers, pool_free_min=acc.pool_free_min,
                wait=None if acc.wait is None else ro_hist(acc.wait, edges),
                late_by=None if acc.late_by is None else ro_hist(acc.late_by, edges))


def summarize_reorder(B):
    """The "reorder" section: the client's reorderer, and each session of a
    server's, each with its totals and its hours; None if the recording has no
    reorder field at all."""
    def section(o):
        if not (o["acc"] or o["off"] or o["skipped"]):
            return None
        edges = o["edges"] or ()
        out = dict(intervals=o["intervals"], off_intervals=o["off"], skipped_intervals=o["skipped"], edges_ms=list(edges),
                   overall=None, by_hour={}, worst_intervals=[])
        if o["acc"]:
            out["overall"] = ro_view(o["acc"][-1], edges)
            out["by_hour"] = {h: ro_view(v, edges) for h, v in sorted(o["acc"].items()) if h >= 0}
            out["worst_intervals"] = [x[3] for x in sorted(o["worst"], key=lambda x: x[:3])[:RO_WORST]]
        return out

    out = dict(client=section(B["client"]), server=None)
    sess = {}
    for sid, o in sorted(B["sessions"].items()):
        sec = section(o)
        if sec:
            sess[sid] = dict(sec, client=o["client"])
    p = B["pool"]
    if sess or p["acc"] or p["skipped"]:
        out["server"] = dict(sessions=sess, pool=dict(
            intervals=p["intervals"], skipped_intervals=p["skipped"],
            overall=p["acc"].get(-1), by_hour={h: v for h, v in sorted(p["acc"].items()) if h >= 0}))
    return out if out["client"] or out["server"] else None


def f(x, p=1):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:.{p}f}"
    return str(x)


def report(s, notes, utc_offset_h):
    w = print
    w("# Field recording\n")
    if s["samples"] < 2:       # the recording of a server: its reorderer is all there is to analyze
        w("No client samples: this is the recording of a server, of which only the reordering of its sessions is analyzed.\n")
    else:
        w(f"{s['hours']} h recorded in {s['segments']} runs ({s['samples']} samples), "
          f"{s['gaps_hours']} h of gaps between them.\n")
    for n in notes:
        w(f"- note: {n}")
    if notes:
        w()
    if s["samples"] < 2:
        report_reorder(s.get("reorder"), utc_offset_h)
        return
    w("## Links\n")
    w("| link | h | up % | outages | p50/p90/max s | <=1/1-3/3-10/10-60/>60 s | RTT p50/p95/p99 ms | dev p95 | behind best p50/p99 | loss down % | loss up % | mutes | muted % |")
    w("| --- | ---: | ---: | ---: | --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: |")
    for n, d in s["links"].items():
        o, r, b = d["outage_s"], d["rtt_ms"], d["behind_best_ms"]
        w(f"| {n} | {d['hours']} | {f(d['up_pct'], 3)} | {d['outages']} | {f(o['p50'])}/{f(o['p90'])}/{f(o['max'])} | "
          f"{'/'.join(str(v) for v in o['buckets'].values())} | {f(r[50])}/{f(r[95])}/{f(r[99])} | "
          f"{f(d['rtt_dev_ms_p95'])} | {f(b[50])}/{f(b[99])} | {f(d['loss_down_pct'], 3)} | "
          f"{f(d['loss_up_pct'], 3)} | {d['mutes']} | {f(d['muted_pct'], 2)} |")
    split = {n: d["split"] for n, d in s["links"].items() if d.get("split")}
    if split:
        w("\n## The upload split\n")
        w("While the client split its upload: what each link did there (links[].split_share; yes: a share; spread: "
          "out for its delay, split_spread_ms, with copies unless split_spread_copy is off; waiting: no delay measured "
          "yet), how often the delay guard "
          "took it out, and how far behind the fastest the guard saw it.\n")
        w("| link | h | yes % | spread % | waiting % | weight 0 % | no % | taken out | behind p50/p99 ms |")
        w("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
        for n, x in split.items():
            p, b = x["pct"], x["behind_ms"]
            w(f"| {n} | {x['hours']} | " + " | ".join(f(p.get(k, 0.0), 2) for k in SPLIT_SHARES) +
              f" | {x['leaves']} | {f(b[50])}/{f(b[99])} |")
    w("\n## Links live at once\n")
    w("| live | % of time |")
    w("| --- | ---: |")
    for k, v in s["simultaneous"].items():
        w(f"| {k} | {f(v, 4)} |")
    w("\n## Pairs\n")
    w("P(B out | A out) against P(B out): a lift of 1 is independence. Events: the share of A's outages with one "
      "of B's within 2 s. RTT corr.: Pearson of the deviations from each link's 60-sample median.\n")
    w("| A | B | out A % | out B % | both % | P(B\\|A) | P(A\\|B) | lift | A's outages with B | RTT corr. |")
    w("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for p in s["pairs"]:
        w(f"| {p['a']} | {p['b']} | {f(p['out_a_pct'], 3)} | {f(p['out_b_pct'], 3)} | {f(p['both_pct'], 4)} | "
          f"{f(p['b_given_a'], 3)} | {f(p['a_given_b'], 3)} | {f(p['lift'])} | {f(p['a_events_with_b'], 2)} | "
          f"{f(p['rtt_corr'], 2)} |")
    sp, s2 = s["spread_ms"], s["spread_best_two_ms"]
    w("\n## RTT spread between live links\n")
    w(f"- all live links: p50 {f(sp[50])} ms, p90 {f(sp[90])} ms, p99 {f(sp[99])} ms")
    w(f"- the best two: p50 {f(s2[50])} ms, p90 {f(s2[90])} ms, p99 {f(s2[99])} ms")
    w("\n## Wall-clock folds (Starlink's 15 s)\n")
    w("Events folded modulo 15 s: the peak phase and the share within a second of it (uniform: 20 %). "
      "A pattern needs a share well above that and enough events.\n")
    w("| link | what | events | peak s (of the minute) | share ±1 s |")
    w("| --- | --- | ---: | --- | ---: |")
    for n, d in s["links"].items():
        for what, k in (("outage starts", "starlink_outages"), ("RTT steps", "starlink_steps")):
            x = d[k]
            if x:
                w(f"| {n} | {what} | {x['n']} | {','.join(str(v) for v in x['seconds'])} | {f(100 * x['share'])} % |")
    w(f"\n## By hour (UTC{utc_offset_h:+g})\n")
    w("| link | hour | out % | RTT p50 | loss down % |")
    w("| --- | ---: | ---: | ---: | ---: |")
    for n, d in s["links"].items():
        for h, v in d["by_hour"].items():
            w(f"| {n} | {h:02d} | {f(v['out_pct'], 2)} | {f(v['rtt_p50'])} | {f(v['loss_down_pct'], 2)} |")
    if s.get("radio"):
        report_radio(s["radio"], utc_offset_h)
    report_joint(s.get("joint"), utc_offset_h)
    report_lost(s.get("lost_all"), utc_offset_h)
    report_sizes(s.get("sizes"))
    report_reorder(s.get("reorder"), utc_offset_h)


def fp(x):
    """A percentage with 3 significant digits."""
    return "-" if x is None else f"{x:.3g}"


def fn(x):
    """A count that may be fractional (an expectation): whole above 100."""
    return "-" if x is None else f"{x:.0f}" if x >= 100 else f"{x:.3g}"


def fx(num, den):
    r = ratio(num, den)
    return "-" if r is None else "inf" if math.isinf(r) else f"{r:.3g}"


def fpair(ref):
    return ("-", "-") if ref is None else (" + ".join(ref["set"]), fp(ref["late_pct"]))


def no_data(x, what, why="an engine without the passive instruments of 0.5.5"):
    """Say why a section is empty; True if it is."""
    if x is None:
        print(f"No data: the recording has no `{what}` field ({why}).")
    elif not x["intervals"]:
        print(f"No data: {x['skipped_intervals']} intervals skipped (a counter went down or the layout changed).")
    else:
        return False
    return True


def pick_rows(subs):
    """The sets to print: all of them, or with many links the pairs, the set
    of all and the larger ones that were late together most often."""
    if len(subs) <= MD_SUBSETS:
        return subs
    top = max(len(x["set"]) for x in subs)
    keep = [x for x in subs if len(x["set"]) in (2, top)]
    more = sorted((x for x in subs if 2 < len(x["set"]) < top), key=lambda x: -x["late"])
    keep += more[:max(0, MD_SUBSETS - len(keep))]
    return sorted(keep, key=lambda x: (len(x["set"]), x["set"]))


def report_joint(j, utc_offset_h):
    w = print
    w("\n## Joint lateness\n")
    if no_data(j, "joint"):
        return
    dl = j["deadlines_ms"]
    w("Download. Every packet goes out on every live link and the first copy wins. A link is late when its copy came "
      "more than the deadline after the first one, or never.\n")
    w("- P: the share of the packets sent on every link of a set for which all of them were late.")
    w("- independent: the product of each link's own late share (over the packets sent on that link).")
    w("- lift: P over independent; 1 is independence, more is trouble that comes together.")
    w("- if indep.: the late packets independence would give. A lift means little when that and the late count are tiny.")
    w("- The counters move when a slot of the engine's ring is reused, so they trail the traffic: at low rates an "
      "hour's figures hold the end of the one before.\n")
    w(f"{j['packets']} packets evaluated in {j['intervals']} intervals"
      + (f", {j['skipped_intervals']} intervals skipped (a counter went down)" if j["skipped_intervals"] else "") + ".")
    if j["other"]:
        w(f"- {j['other']} ({fp(j['other_pct'])} % of the packets) the engine could not judge (a link of id 8 or more, "
          "or more than about 27 kpps): not analyzed.")
    if j["left_out"]:
        w(f"- left out, fewer than {JOINT_MIN_PKTS} packets sent on them: {', '.join(j['left_out'])}.")
    ov = j["overall"]
    if not j["links"]:
        w(f"\nNo link has {JOINT_MIN_PKTS} packets sent on it.")
        return
    w("\n### Each link alone\n")
    w("| link | packets | " + " / ".join(str(d) for d in dl) + " ms late % | RTT p50 ms |")
    w("| --- | ---: | ---: | ---: |")
    for n in j["links"]:
        w(f"| {n} | {ov['deadlines'][0]['single'][n]['packets']} | "
          + " / ".join(fp(v["single"][n]["late_pct"]) for v in ov["deadlines"]) + f" | {f(j['rtt_p50_ms'].get(n))} |")
    for v in ov["deadlines"]:
        subs = v["subsets"]
        if not subs:
            continue
        rows = pick_rows(subs)
        w(f"\n### Sets of links, {v['deadline_ms']} ms after the first copy\n")
        if len(rows) < len(subs):
            w(f"{len(rows)} of {len(subs)} sets (the JSON has them all).\n")
        w("| links | packets | late together | P % | independent % | lift | if indep. |")
        w("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
        for x in rows:
            w(f"| {' + '.join(x['set'])} | {x['packets']} | {x['late']} | {fp(x['late_pct'])} | "
              f"{fp(x['indep_pct'])} | {fp(x['lift'])} | {fn(x['late_if_indep'])} |")
    if len(j["links"]) < 2:
        return
    w("\n### Best pair: by joint lateness, by each link's own lateness and by RTT\n")
    w("P is the share of packets with both links of the pair late; x is that over the best pair by joint lateness "
      "(1: choosing by single figures costs nothing). All: every link of the analysis late at once; pair/all is "
      "the best pair's P over it (story 012, step 4: 2 or less to adopt k-of-N). inf: the denominator was never "
      "seen, too few events to judge.\n")
    best_header(w, ["deadline ms"])
    for v in ov["deadlines"]:
        best_row(w, str(v["deadline_ms"]), v["best"])
    w(f"\n### By hour (UTC{utc_offset_h:+g})\n")
    w("The same comparison per hour of the day and deadline; the pairs are picked within each hour.\n")
    best_header(w, ["hour", "deadline ms", "packets"])
    for h, hv in j["by_hour"].items():
        for v in hv["deadlines"]:
            best_row(w, f"{h:02d} | {v['deadline_ms']} | {hv['packets']}", v["best"])
    pairs_by_hour(w, j, utc_offset_h)


def pairs_by_hour(w, j, utc_offset_h):
    """P(both late) % and the lift of each pair, per hour and deadline; with
    many pairs the MD_PAIRS with the lowest P at the first deadline (the sets
    of three or more are in the JSON only)."""
    ov = j["overall"]["deadlines"]
    pairs = [tuple(x["set"]) for x in ov[0]["subsets"] if len(x["set"]) == 2]
    if len(pairs) > MD_PAIRS:
        p0 = {tuple(x["set"]): x["late_pct"] for x in ov[0]["subsets"]}
        pairs = sorted(sorted(pairs, key=lambda k: (p0[k] is None, p0[k] or 0))[:MD_PAIRS])
    w(f"\n### Pairs by hour (UTC{utc_offset_h:+g})\n")
    w("Each cell is P % (lift) for the pair that column names"
      + (f", the {MD_PAIRS} with the lowest P overall" if len(pairs) == MD_PAIRS else "") + ".")
    for di, v in enumerate(ov):
        w(f"\n#### {v['deadline_ms']} ms\n")
        w("| hour | " + " | ".join(" + ".join(k) for k in pairs) + " |")
        w("| --- | " + " | ".join(["---:"] * len(pairs)) + " |")
        for h, hv in j["by_hour"].items():
            cell = {tuple(x["set"]): x for x in hv["deadlines"][di]["subsets"]}
            w(f"| {h:02d} | " + " | ".join(
                "-" if cell[k]["late_pct"] is None else f"{fp(cell[k]['late_pct'])} ({fp(cell[k]['lift'])})"
                for k in pairs) + " |")


def best_header(w, lead):
    w("| " + " | ".join(lead + ["by joint", "P %", "by single", "P %", "x", "by RTT", "P %", "x", "all late %", "pair/all"]) + " |")
    w("| " + " | ".join(["---"] * len(lead) + ["---", "---:", "---", "---:", "---:", "---", "---:", "---:", "---:", "---:"]) + " |")


def best_row(w, lead, b):
    (nj, pj), (ns, ps), (nr, pr), (na, pa) = (fpair(b[k]) for k in ("by_joint", "by_single", "by_rtt", "all"))
    pj_, ps_, pr_, pa_ = (b[k]["late_pct"] if b[k] else None for k in ("by_joint", "by_single", "by_rtt", "all"))
    w(f"| {lead} | {nj} | {pj} | {ns} | {ps} | {fx(ps_, pj_)} | {nr} | {pr} | {fx(pr_, pj_)} | {pa} | {fx(pj_, pa_)} |")


def report_lost(x, utc_offset_h):
    w = print
    w("\n## Lost on every link\n")
    if no_data(x, "lost_all"):
        return
    w("Download. Sequences that left the replay window without a copy on any link: the tunnel lost them. "
      "The count trails the traffic by the window (8128 sequences). Loss is lost over lost plus delivered.\n")
    w("| hour | delivered | lost on every link | loss % |")
    w("| --- | ---: | ---: | ---: |")
    rows = [("all", x["overall"])] + [(f"{h:02d} (UTC{utc_offset_h:+g})", v) for h, v in x["by_hour"].items()]
    for h, v in rows:
        w(f"| {h} | {v['delivered']} | {v['lost']} | {fp(v['loss_pct'])} |")


def report_sizes(z):
    w = print
    w("\n## WireGuard sizes\n")
    if no_data(z, "sizes", "an engine before 0.5.5, or of 0.12 or later: it counted WireGuard's messages and went "
                           "with the WireGuard mode"):
        return
    w("Datagram sizes by the engine's edges: the share of data packets and of bytes at or under each one. "
      "Up is what the client read from WireGuard, down what it delivered to it.")
    tag = {32: " (keepalive)", 96: " (empty ACK)"}
    for k in ("up", "down"):
        d = z[k]
        w(f"\n### {k}: {d['data_packets']} data packets, {d['data_bytes']} bytes, mean "
          f"{f(d['mean_bytes'])} B\n")
        w("| size, bytes | packets | % | cum. % | bytes % | cum. bytes % |")
        w("| --- | ---: | ---: | ---: | ---: | ---: |")
        for r in d["buckets"]:
            lab = f"<= {r['up_to']}{tag.get(r['up_to'], '')}" if r["up_to"] is not None else f"> {z['edges'][-1]}"
            w(f"| {lab} | {r['packets']} | {f(r['packets_pct'], 2)} | {f(r['cum_packets_pct'], 2)} | "
              f"{f(r['bytes_pct'], 2)} | {f(r['cum_bytes_pct'], 2)} |")
        w("\nHandshakes: " + ", ".join(f"{n} {d['handshakes'][n]}" for n in HANDSHAKES) + ".")


def ro_row(label, v):
    lost, late = v["lost"], v["late"]
    return (f"| {label} | {v['delivered']} | {v['held_total']} | {fp(v['held_pct'])} | {v['bypassed']} | "
            f"{lost['rack']} / {lost['cap']} / {lost['overflow']} / {lost['flush']} | "
            f"{late['after_rack']} / {late['after_cap']} / {late['after_overflow']} / {late['old']} | "
            f"{v['far']} / {v['resyncs']} / {v['pool_full']} | {v['split_rx']} | {v['held_max']} | {v['max_burst']} |")


def ro_q(h):
    """p50 / p90 / p99 of a reorderer histogram, as the edge each is at or under."""
    if not h["total"] or not h["edges_ms"]:
        return "-"
    return " / ".join(f"> {h['edges_ms'][-1]}" if h[k] is None else str(h[k]) for k in ("p50_le_ms", "p90_le_ms", "p99_le_ms"))


def report_ro_section(title, sec, utc_offset_h):
    w = print
    w(f"\n### {title}\n")
    w(f"{sec['intervals']} intervals counted, {sec['off_intervals']} with reordering off, "
      f"{sec['skipped_intervals']} skipped (a counter went down or the layout changed).\n")
    if sec["overall"] is None:
        w("No data: nothing was counted" + (" (reordering was off)." if sec["off_intervals"] else "."))
        return
    w("| hour | delivered | held | held % | bypassed | lost: rack / cap / overflow / flush | "
      "late after: rack / cap / overflow / old | far / resyncs / pool full | from split | most held | longest burst |")
    w("| --- | ---: | ---: | ---: | ---: | --- | --- | --- | ---: | ---: | ---: |")
    w(ro_row("all", sec["overall"]))
    for h, v in sec["by_hour"].items():
        w(ro_row(f"{h:02d} (UTC{utc_offset_h:+g})", v))
    ov = sec["overall"]
    if ov["pool_buffers"] is not None:
        w(f"\nPool: {ov['pool_buffers']} buffers, the fewest free {ov['pool_free_min']}.")
    edges = sec["edges_ms"]
    hs = [(n, ov[k]) for n, k in (("hold time of the held packets", "wait"), ("lateness of the late ones", "late_by")) if ov[k]]
    if hs and edges:
        w("\nHistograms over all the intervals, in ms (the lateness is how long after its gap was given up on a packet came):\n")
        w("| | " + " | ".join(f"<= {e}" for e in edges) + f" | > {edges[-1]} | total | p50 / p90 / p99 at or under |")
        w("| --- |" + " ---: |" * (len(edges) + 2) + " --- |")
        for n, h in hs:
            w(f"| {n} | " + " | ".join(str(c) for c in h["counts"]) + f" | {h['total']} | {ro_q(h)} |")
    if sec["worst_intervals"]:
        w(f"\nThe {len(sec['worst_intervals'])} worst intervals, by packets given up on at the deadline or for lack of room plus late ones "
          "(UTC; the wall time is empty before the clock was set):\n")
        w("| run | time (UTC) | uptime ms | interval ms | delivered | held | lost cap | lost overflow | late | held at the end |")
        w("| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for x in sec["worst_intervals"]:
            w(f"| {x['segment']} | {x['time_utc'] or '-'} | {x['uptime_ms']} | {x['interval_ms']} | {x['delivered']} | {x['held_total']} | "
              f"{x['lost_cap']} | {x['lost_overflow']} | {x['late']} | {f(x['held'])} |")


def report_reorder(x, utc_offset_h):
    w = print
    w("\n## Reordering\n")
    if x is None:
        w("No data: no `reorder` object in the recording (an engine without the receiver's reorderer of story 012, step 1).")
        return
    w("The receiver's reorderer (the status field `reorder`, counters since the engine started, differenced between samples). "
      "Delivered: packets handed on (to the TUN; to WireGuard before 0.12). Held: packets that waited for an earlier one; bypassed: handed on at once for being small. "
      "Lost: gaps it gave up on, because every link had gone past the packet (rack), at its deadline (cap), for lack of room "
      "(overflow) or at a flush. Late after: packets that came after their gap was given up on, by what gave it up (old: too long ago to tell). "
      "Far, resyncs and pool full: packets passed through at once, and the times it picked up the sequence again. "
      "Late after rack means the links do not deliver in order, or the links it waits for are the wrong ones; late after cap, "
      "that the longest wait is too short.")
    if x["client"]:
        report_ro_section("Client (download)", x["client"], utc_offset_h)
    srv = x["server"]
    if srv:
        for sid, sec in srv["sessions"].items():
            report_ro_section(f"Server, session {sid}" + (f" (client {sec['client']})" if sec["client"] else "") + " (upload)", sec, utc_offset_h)
        p = srv["pool"]
        if p["overall"] is not None:
            o = p["overall"]
            w(f"\n### Server pool\n\n{p['intervals']} intervals ({p['skipped_intervals']} skipped): {o['buffers']} buffers, the fewest free "
              f"{o['free_min']}, most sessions holding at once {o['holding_max']}, times a session found it empty {o['full']}.")


# --- Radio: the RF metrics of SmoothStream's cell-monitor, crossed with the links ---------------

RF_GAP_K = 3           # a gap longer than this many RF intervals is a hole, not coverage
RF_MIN_WINDOW_S = 20   # the window of the crossing is 2 RF intervals, at least this (N)
RF_PRE_S = 60          # "the minute before" an event
RF_TREND_S = 300       # the minutes before that one, for the trend of the SINR
MIN_EPISODES = 10      # independent events needed before the report reads a test
EPISODE_S = 60         # events closer than this (or than 2N) are one episode for the tests
P_READ = 0.01          # a test is read as a difference below this
RF_LOCAL_S = 1800      # L: the neighbourhood (+-) of the local chance and the local SINR baseline
MIN_LOCAL_BASE = 10    # RF samples a local SINR baseline needs
PB_CELLS = 3000000     # episodes x hits the exact Poisson-binomial tail may cost; above it, a normal approximation
CROSS_KINDS = ("rat", "band", "cell", "any")
EVENT_KINDS = (("outage", "outage"), ("upload_mute", "upload mute"), ("download_mute", "download mute"))

# The measurements and fields of cell-monitor.sh that the radio section uses
# (None: the tags are enough). Everything else is skipped: the file may hold
# the whole bucket.
RF_FIELDS = {
    "cell_nwinfo": None,
    "cell_serving_lte": {"cellid", "pci", "earfcn", "rsrp", "rsrq", "sinr"},
    "cell_serving_nr": {"cellid", "pci", "arfcn", "rsrp", "rsrq", "sinr"},
    "cell_ca": {"pci", "arfcn", "rsrp", "rsrq", "sinr"},
    "cell_rxpath": {f"{m}{i}" for m in ("rsrp", "rsrq", "sinr") for i in range(1, 5)},
    "cell_monitor": {"up"},
}
RF_KNOWN_UNUSED = ("cell_signal", "cell_temp", "cell_data", "cell_net")
# Tag columns of cell-monitor's measurements: in a pivoted CSV they tell the
# tags from the fields when the annotations are not there.
RF_TAGS = ("site", "cpe_id", "host", "modem_id", "leg", "carrier", "rat", "band", "state", "duplex",
           "mode", "cc_type", "cc_seq", "sensor", "operator")
CSV_RESERVED = ("", "result", "table", "_start", "_stop", "_time", "_value", "_field", "_measurement")


def san_tag(s):
    """What cell-monitor.sh's san_tag does to a tag value: quotes and CR out,
    lower case, runs of white space to _, only a-z 0-9 _ . - kept. A link's
    label is compared with the leg tag in this form."""
    s = (s or "").replace('"', "").replace("\r", "").lower()
    return re.sub(r"[^a-z0-9_.\-]", "", re.sub(r"\s+", "_", s))


# -- line protocol

def split_esc(s, sep, quotes=False):
    """s cut at the sep that is not escaped by a backslash (nor, with quotes,
    inside a double-quoted string)."""
    out, buf, esc, inq = [], [], False, False
    for ch in s:
        if esc:
            buf.append(ch)
            esc = False
        elif ch == "\\":
            buf.append(ch)
            esc = True
        elif quotes and ch == '"':
            inq = not inq
            buf.append(ch)
        elif ch == sep and not inq:
            out.append("".join(buf))
            buf = []
        else:
            buf.append(ch)
    out.append("".join(buf))
    return out


def lp_sections(line):
    """The 2 or 3 sections of a line: measurement and tags, fields, timestamp.
    The cuts are the spaces that are not escaped nor inside a string field."""
    parts, buf, esc, inq = [], [], False, False
    for ch in line:
        if esc:
            buf.append(ch)
            esc = False
        elif ch == "\\":
            buf.append(ch)
            esc = True
        elif ch == '"' and len(parts) == 1:
            inq = not inq
            buf.append(ch)
        elif ch == " " and not inq:
            if buf:
                parts.append("".join(buf))
                buf = []
        else:
            buf.append(ch)
    if buf:
        parts.append("".join(buf))
    return parts


def lp_unescape(s):
    return re.sub(r"\\([ ,=])", r"\1", s)


def lp_value(v):
    """A field value: 12i and 12u integers, floats, t/f booleans, "strings"."""
    if v.startswith('"'):
        return re.sub(r'\\(["\\])', r"\1", v[1:-1] if v.endswith('"') and len(v) > 1 else v[1:])
    if v[-1:] in ("i", "u") and v[:-1].lstrip("+-").isdigit():
        return int(v[:-1])
    if v in ("t", "T", "true", "True", "TRUE"):
        return True
    if v in ("f", "F", "false", "False", "FALSE"):
        return False
    return float(v)


# A line with no escapes and strings without a backslash or a quote inside (all
# that cell-monitor writes: its tags are sanitized) is read by these two
# expressions, much faster than the general reading below.
LP_FAST = re.compile(r'([^\s,\\"=]+)((?:,[^\s,=\\"]+=[^\s,\\"]*)*) '
                     r'((?:[^\s,=\\"]+=(?:"[^"\\]*"|[^\s,\\"]*))(?:,[^\s,=\\"]+=(?:"[^"\\]*"|[^\s,\\"]*))*)(?: (-?\d+))?')
LP_FIELD = re.compile(r'([^\s,=\\"]+)=("[^"\\]*"|[^\s,\\"]*)')
LP_MEAS = re.compile(r'[^,\s\\]+')


def lp_parse(line):
    """(measurement, tags, fields, timestamp or None) of a line-protocol line;
    ValueError if it is not one."""
    m = LP_FAST.fullmatch(line)
    if m:
        meas, tg, fl, ts = m.groups()
        fields = LP_FIELD.findall(fl)
        if all(v for _, v in fields):
            tags = {}
            for item in tg[1:].split(",") if tg else ():
                k, _, v = item.partition("=")
                if v:
                    tags[k] = v
            return meas, tags, {k: lp_value(v) for k, v in fields}, (int(ts) if ts else None)
    return lp_parse_full(line)


def lp_parse_full(line):
    """The same for any line, escapes and quoted strings included."""
    parts = lp_sections(line)
    if len(parts) not in (2, 3):
        raise ValueError("sections")
    head = split_esc(parts[0], ",")
    tags = {}
    for item in head[1:]:
        k, _, v = item.partition("=")
        if not k:
            raise ValueError("tag")
        if v:
            tags[lp_unescape(k)] = lp_unescape(v)
    fields = {}
    for item in split_esc(parts[1], ",", True):
        k, _, v = item.partition("=")
        if not k or not v:
            raise ValueError("field")
        fields[lp_unescape(k)] = lp_value(v)
    if not head[0] or not fields:
        raise ValueError("empty")
    return lp_unescape(head[0]), tags, fields, (int(parts[2]) if len(parts) == 3 else None)


def lp_seconds(x, precision):
    """A line-protocol timestamp in seconds. Auto: by size, which tells apart
    s, ms, us and ns for any date from 2001 to 2286."""
    if precision == "auto":
        ax = abs(x)
        div = 1e9 if ax >= 1e17 else 1e6 if ax >= 1e14 else 1e3 if ax >= 1e11 else 1
    else:
        div = {"s": 1, "ms": 1e3, "us": 1e6, "ns": 1e9}[precision]
    return x / div


# -- annotated CSV

TIME_RE = re.compile(r"^(\d{4})-(\d\d)-(\d\d)[T ](\d\d):(\d\d):(\d\d)(\.\d+)?(Z|[+-]\d\d:?\d\d)?$")


def csv_time(v):
    """Seconds since the epoch of an RFC 3339 time (a missing zone is UTC), or
    of a bare number; None if it is neither."""
    m = TIME_RE.match(v.strip())
    if not m:
        try:
            return float(v)
        except ValueError:
            return None
    y, mo, d, h, mi, s = (int(x) for x in m.groups()[:6])
    t = calendar.timegm((y, mo, d, h, mi, s)) + (float(m.group(7)) if m.group(7) else 0.0)
    z = m.group(8)
    if z and z != "Z":
        t -= (1 if z[0] == "+" else -1) * (int(z[1:3]) * 3600 + int(z[-2:]) * 60)
    return t


def csv_value(v):
    try:
        return float(v)
    except ValueError:
        return v


# A sample holds what the posts of one second said, in fixed places: a week is
# 300 000 of them, and dicts of dicts take 2.5 KB each, this under 1 KB.
# Serving cell and primary carrier lines: [band, cellid, pci, frequency, rsrp,
# rsrq, sinr, rat]; rx paths: rsrp1-4, rsrq1-4, sinr1-4 by RAT.
SERVING_AT = {"cellid": 1, "pci": 2, "earfcn": 3, "arfcn": 3, "rsrp": 4, "rsrq": 5, "sinr": 6}
RX_AT = {f"{m}{i + 1}": 4 * j + i for j, m in enumerate(("rsrp", "rsrq", "sinr")) for i in range(4)}


class RFSample:
    __slots__ = ("nw", "lte", "sa", "nsa", "pcc", "rx", "up")

    def __init__(self):
        self.nw = self.lte = self.sa = self.pcc = self.rx = self.up = None
        self.nsa = False


class RFSeries:
    """The samples of one modem (cpe_id, modem_id, leg, carrier): second ->
    what the posts of that second held."""

    def __init__(self, key):
        self.key = key
        self.samples = {}


class RF:
    def __init__(self):
        self.series = {}
        self.files = []
        self.records = self.no_time = self.bad = self.no_cols = 0
        self.unknown = collections.Counter()
        self.unused = collections.Counter()
        self.share = {}                # equal values and tags are one object: a week is 300 000 samples

    def wants(self, meas, field=None):
        """Whether anything of this measurement (or this field of it) is used;
        if not, it is counted as skipped."""
        want = RF_FIELDS.get(meas, False)
        if want is False:
            (self.unused if meas in RF_KNOWN_UNUSED else self.unknown)[meas] += 1
            return False
        if field is not None and want is not None and field not in want:
            self.unused[meas] += 1
            return False
        return True

    def add(self, t, meas, tags, fields):
        """One point (or, from a CSV with a row per field, one field of one)."""
        if not self.wants(meas):
            return
        want = RF_FIELDS[meas]
        if want is not None:
            fields = {k: v for k, v in fields.items() if k in want}
            if not fields:
                self.unused[meas] += 1
                return
        sh = self.share
        tags = {k: sh.setdefault(v, v) for k, v in tags.items()}
        fields = {k: sh.setdefault(v, v) for k, v in fields.items()}
        key = (tags.get("cpe_id") or tags.get("host") or "", tags.get("modem_id") or "",
               tags.get("leg") or "", tags.get("carrier") or "")
        ser = self.series.get(key)
        if ser is None:
            ser = self.series[key] = RFSeries(key)
        sec = int(math.floor(t + 0.5))
        s = ser.samples.get(sec)
        if s is None:
            s = ser.samples[sec] = RFSample()
        if meas == "cell_nwinfo":
            s.nw = (tags.get("rat", ""), tags.get("band", ""))
        elif meas == "cell_serving_lte":
            s.lte = serving_put(s.lte, tags, fields)
        elif meas == "cell_serving_nr":
            if (tags.get("mode") or ("sa" if "cellid" in fields else "nsa")).lower() == "sa":
                s.sa = serving_put(s.sa, tags, fields)
            else:
                s.nsa = True
        elif meas == "cell_ca":
            cct = tags.get("cc_type")
            if cct == "pcc" or (not cct and tags.get("cc_seq") == "1"):
                s.pcc = serving_put(s.pcc, tags, fields)
                s.pcc[7] = tags.get("rat", "")
        elif meas == "cell_rxpath":
            rx = s.rx
            if rx is None:
                rx = s.rx = {}
            lst = rx.get((tags.get("rat") or "").lower())
            if lst is None:
                lst = rx[(tags.get("rat") or "").lower()] = [None] * 12
            for k, v in fields.items():
                lst[RX_AT[k]] = v
        elif meas == "cell_monitor":
            s.up = fields["up"]


def serving_put(cur, tags, fields):
    """The fields of a serving cell or carrier line into its 8 places."""
    if cur is None:
        cur = [None] * 8
    if tags.get("band"):
        cur[0] = tags["band"]
    for k, v in fields.items():
        i = SERVING_AT.get(k)
        if i is not None:
            cur[i] = v
    return cur


def read_rf_lp(fh, rf, precision):
    for line in fh:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        rf.records += 1
        m = LP_MEAS.match(line)
        if m and m.group() in RF_KNOWN_UNUSED:         # a third of the lines, none of them used
            rf.unused[m.group()] += 1
            continue
        try:
            meas, tags, fields, ts = lp_parse(line)
        except (ValueError, OverflowError):
            rf.bad += 1
            continue
        if ts is None:
            rf.no_time += 1
            continue
        rf.add(lp_seconds(ts, precision), meas, tags, fields)


def csv_block(head, ann):
    """How to read the rows under a header: the columns of the time, the
    measurement, the tags and the fields; None if it lacks the time or the
    measurement. Tags are the columns named like cell-monitor's tags and those
    in the group key (#group true); of the rest, a pivoted table has a field."""
    idx = {c: i for i, c in enumerate(head) if c}
    if "_time" not in idx or "_measurement" not in idx:
        return None
    grp = ann.get("#group") or []
    tag_cols = [(c, i) for c, i in idx.items() if c not in CSV_RESERVED
                and (c in RF_TAGS or (i < len(grp) and grp[i] == "true"))]
    tagset = {c for c, _ in tag_cols}
    long = "_field" in idx and "_value" in idx
    return dict(head=head, dflt=ann.get("#default") or [], ti=idx["_time"], mi=idx["_measurement"], tags=tag_cols,
                long=long, fi=idx.get("_field"), vi=idx.get("_value"),
                fields=[] if long else [(c, i) for c, i in idx.items() if c not in CSV_RESERVED and c not in tagset])


def csv_cell(row, i, dflt):
    v = row[i] if i < len(row) else ""
    return dflt[i] if v == "" and i < len(dflt) else v


def read_rf_csv(fh, rf):
    """Annotated CSV as InfluxDB writes it: blocks of annotation rows (#group,
    #datatype, #default) and a header row, then the rows of one table or more;
    a blank line ends a block. A row is one field (columns _field and _value)
    or, pivoted, a whole point (a column per field)."""
    ann, blk, head = {}, None, None
    for row in csv.reader(fh):
        if not row or not any(row):
            ann, head = {}, None
            continue
        if row[0].startswith("#"):
            if head is not None:          # annotations again: a new block, even with no blank line before
                ann = {}
            ann[row[0]] = row
            head = None
            continue
        if head is None:
            head = [c.strip() for c in row]
            blk = csv_block(head, ann)
            continue
        if row == head:
            continue
        if blk is None:
            rf.no_cols += 1
            continue
        d = blk["dflt"]
        rf.records += 1
        meas = csv_cell(row, blk["mi"], d)
        if blk["long"]:
            fld = csv_cell(row, blk["fi"], d)
            if not rf.wants(meas, fld):
                continue
        t = csv_time(csv_cell(row, blk["ti"], d))
        if t is None:
            rf.bad += 1
            continue
        tags = {c: v for c, i in blk["tags"] for v in (csv_cell(row, i, d),) if v != ""}
        if blk["long"]:
            fields = {fld: csv_value(csv_cell(row, blk["vi"], d))}
        else:
            fields = {c: csv_value(v) for c, i in blk["fields"] for v in (csv_cell(row, i, d),) if v != ""}
        rf.add(t, meas, tags, fields)


def open_text(p):
    if p.endswith(".gz"):
        return gzip.open(p, "rt", encoding="utf-8-sig", newline="")
    return open(p, encoding="utf-8-sig", newline="")


def load_rf(paths, precision="auto"):
    """The RF data of the files, plain or gzipped, each told apart by its
    content; OSError for one that cannot be read."""
    rf = RF()
    for p in paths:
        with open_text(p) as fh:
            first = next((ln.strip() for ln in fh if ln.strip()), "")
        cols = first.split(",")
        is_csv = first.startswith(("#datatype", "#group", "#default")) or "_time" in cols or "_measurement" in cols
        before = rf.records
        with open_text(p) as fh:
            if is_csv:
                read_rf_csv(fh, rf)
            else:
                read_rf_lp(fh, rf, precision)
        rf.files.append(dict(file=os.path.basename(p), format="annotated CSV" if is_csv else "line protocol",
                             unit="rows" if is_csv else "lines", records=rf.records - before))
    return rf


# -- time intervals: lists of [start, end] in seconds, sorted and disjoint

def iv_merge(iv):
    out = []
    for a, b in sorted(iv):
        if b <= a:
            continue
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def iv_and(A, B):
    out, i, j = [], 0, 0
    while i < len(A) and j < len(B):
        a, b = max(A[i][0], B[j][0]), min(A[i][1], B[j][1])
        if a < b:
            out.append([a, b])
        if A[i][1] < B[j][1]:
            i += 1
        else:
            j += 1
    return out


def iv_len(A):
    return sum(b - a for a, b in A)


def iv_erode(A, n):
    """Each interval n shorter at both ends (gone if that leaves nothing)."""
    return [[a + n, b - n] for a, b in A if b - a > 2 * n]


def iv_has(A, starts, t):
    i = bisect.bisect_right(starts, t) - 1
    return i >= 0 and t <= A[i][1]


def iv_clip(a, b, A, starts):
    """Length of [a, b] inside A."""
    tot = 0.0
    i = max(0, bisect.bisect_right(starts, a) - 1)
    while i < len(A) and A[i][0] < b:
        tot += max(0.0, min(b, A[i][1]) - max(a, A[i][0]))
        i += 1
    return tot


# -- what a sample says

def norm_rat(x):
    """cell-monitor's rat tags ("fdd_lte", "tdd_nr5g", "nr5g-sa", ...) to lte,
    nr5g (stand-alone) or nr5g-nsa; anything else as it comes."""
    x = (x or "").lower().strip()
    for p in ("fdd_", "tdd_"):
        if x.startswith(p):
            x = x[len(p):]
    if not x:
        return None
    if x.startswith("nr5g") or x in ("nr", "5g"):
        return "nr5g-nsa" if "nsa" in x else "nr5g"
    if x.startswith("lte") or x == "eutran":
        return "lte"
    return x


def norm_band(x):
    """"lte_band_7" and "b7" to b7, "nr5g_band_78" and "n78" to n78; another
    technology's band as it comes; None for none ("b" alone is what
    cell-monitor writes when the modem gave no band)."""
    x = (x or "").lower().strip()
    if re.fullmatch(r"[bn]\d+", x):
        return x
    m = re.fullmatch(r"(lte|eutran)\D*(\d+)", x)
    if m:
        return "b" + m.group(2)
    m = re.fullmatch(r"(nr5g|nr|5g)\D*(\d+)", x)
    if m:
        return "n" + m.group(2)
    return None if x in ("", "b", "n") else x


def num(x):
    return float(x) if isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) else None


def mean_of(v, base):
    """The mean of the up to 4 paths of a metric (None where a path is inactive)."""
    v = [num(x) for x in v[base:base + 4]]
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def derive(s):
    """What one sample says: the RAT, the band, three ways to name the serving
    cell, and RSRP, RSRQ and SINR from three places (the serving cell line,
    the rx paths, the primary carrier)."""
    nw, lte, sa, pcc, rx = s.nw, s.lte, s.sa, s.pcc, s.rx
    rat = norm_rat(nw[0]) if nw else None
    if rat is None:
        rat = "nr5g" if sa else "lte" if lte else norm_rat(pcc[7]) if pcc else None
    band = norm_band(nw[1]) if nw else None
    if band is None:
        band = next((b for b in (norm_band(d[0]) for d in (lte, sa, pcc) if d) if b), None)
    srv = sa if (rat == "nr5g" and sa) else (lte or sa)    # in NSA, the LTE anchor
    r = {"rat": rat, "band": band, "nsa": s.nsa, "up": s.up,
         "srv_meas": None if not srv else "cell_serving_nr" if srv is sa else "cell_serving_lte"}
    r["cells"] = {}
    if srv and num(srv[1]) is not None:
        r["cells"]["cellid"] = int(srv[1])
    if srv and num(srv[2]) is not None:
        r["cells"]["pci"] = (int(srv[2]), num(srv[3]))
    if pcc and num(pcc[2]) is not None:
        r["cells"]["pcc"] = (int(pcc[2]), num(pcc[3]))
    r["m"] = {"serving": tuple(num(srv[i]) for i in (4, 5, 6)) if srv else (None,) * 3,
              "ca": tuple(num(pcc[i]) for i in (4, 5, 6)) if pcc else (None,) * 3}
    path = None
    if rx:
        path = rx.get("nr5g" if rat == "nr5g" else "lte") or (next(iter(rx.values())) if len(rx) == 1 else None)
    r["m"]["rxpath"] = tuple(mean_of(path, i) for i in (0, 4, 8)) if path else (None,) * 3
    return r


class Timeline:
    """One modem's samples in the engine's clock: for each, how long it holds
    (until the next sample, at most RF_GAP_K intervals), and the values."""


def build_timeline(ser, off):
    ts = sorted(ser.samples)
    tl = Timeline()
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    tl.interval = float(statistics.median(gaps)) if gaps else None
    cap = RF_GAP_K * tl.interval if tl.interval else 0
    tl.t = [t + off for t in ts]
    tl.w = [min(g, cap) for g in gaps] + [tl.interval or 0]
    tl.cover = iv_merge([[t, t + w] for t, w in zip(tl.t, tl.w)])
    rows = [derive(ser.samples[t]) for t in ts]
    tl.rat = [r["rat"] for r in rows]
    tl.band = [r["band"] for r in rows]
    tl.nsa = [r["nsa"] for r in rows]
    tl.down = sum(1 for r in rows if r["up"] is not None and r["up"] == 0)
    tl.up = [r["up"] for r in rows]
    # One source for the metrics and one way to name the cell for the whole
    # series, so a post that lacks a line does not make the values jump
    # between sources: the first of the order that has at least half as many
    # samples as the one with most.
    order = ("serving", "rxpath", "ca")
    have = {k: sum(1 for r in rows if any(v is not None for v in r["m"][k])) for k in order}
    tl.source = next((k for k in order if have[k] and have[k] * 2 >= max(have.values())), None)
    tl.rsrp, tl.rsrq, tl.sinr = ([r["m"][tl.source][i] if tl.source else None for r in rows] for i in range(3))
    korder = ("cellid", "pci", "pcc")
    kc = {k: sum(1 for r in rows if k in r["cells"]) for k in korder}
    tl.cell_kind = next((k for k in korder if kc[k] and kc[k] * 2 >= max(kc.values())), None)
    tl.cell = [r["cells"].get(tl.cell_kind) for r in rows]
    tl.meas = [{"serving": r["srv_meas"], "rxpath": "cell_rxpath", "ca": "cell_ca"}[tl.source]
               if tl.source and any(v is not None for v in r["m"][tl.source]) else None for r in rows]
    return tl


def changes_of(tl, kind):
    """(times a value changed, [start, end] spans in which a change would have
    shown, [(time, from, to)] of the changes). Two readings of a value count
    only if no more than RF_GAP_K intervals lie between them: after a longer
    hole nothing can be said."""
    vals = getattr(tl, kind)
    cap = RF_GAP_K * tl.interval if tl.interval else 0
    times, spans, flips, last_t, last_v = [], [], [], None, None
    for t, v in zip(tl.t, vals):
        if v is None:
            continue
        if last_t is not None and t - last_t <= cap:
            spans.append((last_t, t))
            if v != last_v:
                times.append(t)
                flips.append((t, last_v, v))
        last_t, last_v = t, v
    return times, spans, flips


# -- series to links

def map_series(key, meta, maps):
    """(link, how, reason): the link a series belongs to by its leg tag (a
    label, else a name), by its carrier when it has no leg, or by --rf-map."""
    cpe, modem, leg, car = key

    def by(i, s):
        return [n for n, m in meta.items() if m[i] and san_tag(m[i]) == s]

    mine = {san_tag(x) for x in (leg, car, modem) if x}
    for k, v in maps:
        if k in mine:
            hit = by(1, v) or by(0, v) or [n for n in meta if san_tag(n) == v]
            if len(hit) == 1:
                return hit[0], "map", ""
            return None, "", f"--rf-map {k}={v}: " + ("no link has that label or name" if not hit else f"{len(hit)} links have it")
    if leg:
        s = san_tag(leg)
        for i, what in ((1, "label"), (0, "name")):
            hit = by(i, s)
            if len(hit) == 1:
                return hit[0], what, ""
            if hit:
                return None, "", f"leg {leg!r} is the {what} of {len(hit)} links ({', '.join(hit)})"
        return None, "", f"leg {leg!r} is no link's label or name"
    if car:
        s = san_tag(car)
        hit = [n for n, m in meta.items() if m[1] and s in san_tag(m[1])]
        if len(hit) == 1:
            return hit[0], "carrier", ""
        if hit:
            return None, "", f"no leg, and carrier {car!r} is in the label of {len(hit)} links ({', '.join(hit)})"
        return None, "", f"no leg, and carrier {car!r} is in no link's label"
    return None, "", "neither a leg nor a carrier tag"


def pairing_notes(rows, meta):
    """A series whose carrier tag is not in its link's label while another
    carrier's name is: the leg pairing of cell-monitor is probably crossed."""
    carriers = {san_tag(r["carrier"]) for r in rows if r["carrier"]}
    notes = []
    for r in rows:
        if not r["link"] or not r["carrier"] or not meta[r["link"]][1]:
            continue
        lab, mine = san_tag(meta[r["link"]][1]), san_tag(r["carrier"])
        other = sorted(c for c in carriers if c != mine and c in lab)
        if mine not in lab and other:
            notes.append(f"{r['cpe_id']}/{r['modem_id']} has leg {r['leg']!r} (link {r['link']}) but carrier {r['carrier']!r}, "
                         f"and that link's label names {other[0]!r}: MODEMn_LEG and MODEMn_CARRIER of the CPE may be crossed")
    return notes


# -- statistics

def binom_logs(ks, n, q):
    lq, lp, lg = math.log(q), math.log1p(-q), math.lgamma
    return [lg(n + 1) - lg(k + 1) - lg(n - k + 1) + k * lq + (n - k) * lp for k in ks]


def binom_tail(lo, hi, n, q):
    """P(lo <= X <= hi), X ~ Binomial(n, q)."""
    lo, hi = max(lo, 0), min(hi, n)
    if lo > hi:
        return 0.0
    if q <= 0:
        return 1.0 if lo == 0 else 0.0
    if q >= 1:
        return 1.0 if hi == n else 0.0
    t = binom_logs(range(lo, hi + 1), n, q)
    m = max(t)
    return min(1.0, math.exp(m) * sum(math.exp(x - m) for x in t))


def thin(times, gap):
    """The events at least gap seconds after the last one kept: episodes
    whose windows do not share time, which is what the tests assume."""
    out = []
    for e in sorted(times):
        if not out or e - out[-1] >= gap:
            out.append(e)
    return out


def rank_of(x, base):
    """Share of the sorted baseline below x (half of the ties)."""
    return (bisect.bisect_left(base, x) + bisect.bisect_right(base, x)) / 2 / len(base)


def rank_test(vals, base):
    """Mean rank of vals in base (0.5: no difference) and the two-sided p of
    its z; each rank is uniform if the values come from the same
    distribution, with variance 1/12 (ties only make that larger)."""
    if not vals or not base:
        return None, None, None
    mr = sum(rank_of(v, base) for v in vals) / len(vals)
    z = (mr - 0.5) * math.sqrt(12 * len(vals))
    return mr, z, math.erfc(abs(z) / math.sqrt(2))


class Means:
    """(time, value) pairs with prefix sums, for the mean over a span."""

    def __init__(self, pairs):
        pairs = sorted(pairs)
        self.t = [p[0] for p in pairs]
        self.c = [0.0]
        for _, v in pairs:
            self.c.append(self.c[-1] + v)

    def mean(self, a, b, need):
        i, j = bisect.bisect_left(self.t, a), bisect.bisect_left(self.t, b)
        return (self.c[j] - self.c[i]) / (j - i) if j - i >= need else None


def hour_of(t, utc_offset_h):
    return int(((t / 3600.0) + utc_offset_h) % 24)


class IvCum:
    """The length of a list of intervals (sorted, disjoint) inside any span,
    in O(log n): prefix sums of the lengths."""

    def __init__(self, A):
        self.A = A
        self.starts = [a for a, _ in A]
        self.pre = [0.0]
        for a, b in A:
            self.pre.append(self.pre[-1] + b - a)

    def upto(self, t):
        i = bisect.bisect_right(self.starts, t)
        if i == 0:
            return 0.0
        a, b = self.A[i - 1]
        return self.pre[i - 1] + min(t, b) - a

    def between(self, a, b):
        return self.upto(b) - self.upto(a)


def local_shares(eps, cs, N, V, L):
    """For each episode e, the share of V within L s of it that lies within N s
    of a change (cs, sorted): the chance that an event placed at random in
    that neighbourhood has a change within N s. None where V has nothing
    there. The global chance is this over the whole of V."""
    W = iv_merge([[c - N, c + N] for c in cs])
    VW, VV = IvCum(iv_and(W, V)), IvCum(V)
    out = []
    for e in eps:
        den = VV.between(e - L, e + L)
        out.append(VW.between(e - L, e + L) / den if den > 0 else None)
    return out


def pb_tail(qs, k, upper):
    """(P(X >= k) if upper else P(X <= k), method) for X the number of
    successes of independent trials with probabilities qs (Poisson-binomial).
    Exact by a dynamic programme that keeps the counts up to k (for the upper
    tail the rest is one absorbing cell, so nothing is ever subtracted from 1)
    while n x (k + 1) fits PB_CELLS; above it, a normal approximation with
    continuity correction."""
    n = len(qs)
    if upper and k <= 0 or not upper and k >= n:
        return 1.0, "exact"
    if upper and k > n or not upper and k < 0:
        return 0.0, "exact"
    if n * (k + 1) > PB_CELLS:
        mu, var = sum(qs), sum(q * (1 - q) for q in qs)
        if var <= 0:
            return float(k <= mu if upper else k >= mu), "normal"
        z = ((k - 0.5 - mu) if upper else (k + 0.5 - mu)) / math.sqrt(var)
        return 0.5 * math.erfc((z if upper else -z) / math.sqrt(2)), "normal"
    dp = [1.0] + [0.0] * k
    for q in qs:
        if q <= 0:
            continue
        prev = dp
        dp = [x * (1 - q) + y * q for x, y in zip(prev, [0.0] + prev[:-1])]
        if upper:
            dp[k] += prev[k] * q
    return (dp[k] if upper else sum(dp)), "exact"


def cross_row(evs, eps, cs, N, V, L=0):
    """One kind of event against one kind of change. cs: the sorted change
    times. An event has one if a change lies within N s of its start. Chance
    is the part of V (the time where an event can be judged) within N s of a
    change: the probability that an event placed at random in V would have
    one (global). The local chance does it for each episode over the part of V
    within L s of it, so that hours in which outages and changes both come
    thick (a moving car, a crowded evening) do not count as one announcing the
    other: the expected hits are the sum of those shares and the tail is the
    Poisson-binomial one. The reading is on the episodes, the local one when
    L > 0."""
    n, vlen = len(evs), iv_len(V)
    if not cs:
        return dict(n=n, hits=0, share_pct=0.0 if n else None, chance_pct=0.0, expected=0.0, before=0, after=0,
                    episodes=len(eps), episode_hits=0, p=None, p_local=None, local_chance_pct=None, local_expected=None,
                    expected_episodes=0.0, basis="", reading="no changes")

    def hit(e, lo, hi):
        i = bisect.bisect_left(cs, e + lo)
        return i < len(cs) and cs[i] <= e + hi

    def chance(lo, hi):
        W = iv_merge([[c - hi, c - lo] for c in cs])
        return iv_len(iv_and(W, V)) / vlen if vlen else 0.0

    q = chance(-N, N)
    hits = sum(hit(e, -N, N) for e in evs)
    eh = sum(hit(e, -N, N) for e in eps)
    ne = len(eps)
    p = binom_tail(eh, ne, ne, q) if eps else None
    qs = [x if x is not None else q for x in local_shares(eps, cs, N, V, L)] if L > 0 and eps else None
    p_local, method = pb_tail(qs, eh, True) if qs else (None, "")
    if ne < MIN_EPISODES:
        reading, basis = "too few events", ""
    else:
        basis = "local" if qs else "global"
        up = p_local if qs else p
        if up < P_READ:
            reading = "more than chance"
        elif (pb_tail(qs, eh, False)[0] if qs else binom_tail(0, eh, ne, q)) < P_READ:
            reading = "less than chance"
        else:
            reading = "about chance"
    return dict(n=n, hits=hits, share_pct=100 * hits / n if n else None, chance_pct=100 * q, expected=n * q,
                before=sum(hit(e, -N, -1e-9) for e in evs), after=sum(hit(e, 0, N) for e in evs),
                before_chance_pct=100 * chance(-N, 0), after_chance_pct=100 * chance(0, N),
                episodes=ne, episode_hits=eh, p=p, expected_episodes=ne * q,
                local_chance_pct=100 * sum(qs) / ne if qs else None, local_expected=sum(qs) if qs else None,
                p_local=p_local, p_method=method, basis=basis, reading=reading)


def sinr_before(eps, S, base_times, need, need_trend, N=0, L=0):
    """The SINR of the minute before each episode against the link's own: the
    mean over [e-60, e) of each event, and the same mean at every RF sample
    time, so the baseline is the same statistic. The trend is that mean minus
    the mean of the 4 minutes before it. With L > 0 the level also has a local
    baseline: for each episode, those means at the RF sample times within L s
    of it, leaving out the times in [e-60, e+N] (the minute itself, the outage
    and the changes around it); each episode's rank in its own baseline is
    uniform if the SINR before outages is as at any time near them, which a
    long bad stretch (rain, a crowd) cannot fake. The reading is on the local
    rank when at least MIN_EPISODES episodes have a baseline."""
    def level(e):
        return S.mean(e - RF_PRE_S, e, need)

    def trend(e):
        a, b = S.mean(e - RF_PRE_S, e, need), S.mean(e - RF_TREND_S - RF_PRE_S, e - RF_PRE_S, need_trend)
        return None if a is None or b is None else a - b

    out = {}
    for what, fn in (("level", level), ("trend", trend)):
        base = sorted(x for x in (fn(t) for t in base_times) if x is not None)
        vals = sorted(x for x in (fn(e) for e in eps) if x is not None)
        o = dict(n=len(vals), median=pct(vals, 50), baseline_median=pct(base, 50), baseline_n=len(base))
        if vals and base:
            mr, z, p = rank_test(vals, base)
            o.update(mean_rank_pct=100 * mr, z=z, p=p)
            if what == "level":
                thr = pct(base, 10)
                o.update(baseline_p10=thr, under_p10=sum(1 for v in vals if v < thr),
                         expected_under_p10=len(vals) * bisect.bisect_left(base, thr) / len(base))
        p, z, o["reading_basis"] = o.get("p"), o.get("z"), "global"
        if what == "level" and L > 0:
            pairs = [(t, m) for t in base_times for m in (level(t),) if m is not None]
            ts, ranks, under, expect, meds = [t for t, _ in pairs], [], 0, 0.0, []
            for e in eps:
                v = level(e)
                lo, hi = bisect.bisect_left(ts, e - L), bisect.bisect_right(ts, e + L)
                b = sorted(m for t, m in pairs[lo:hi] if not e - RF_PRE_S <= t <= e + N)
                if v is None or len(b) < MIN_LOCAL_BASE:
                    continue
                thr = pct(b, 10)
                ranks.append(rank_of(v, b))
                under += v < thr
                expect += bisect.bisect_left(b, thr) / len(b)
                meds.append(pct(b, 50))
            if ranks:
                mr = sum(ranks) / len(ranks)
                zl = (mr - 0.5) * math.sqrt(12 * len(ranks))
                o["local"] = dict(n=len(ranks), mean_rank_pct=100 * mr, z=zl, p=math.erfc(abs(zl) / math.sqrt(2)),
                                  under_p10=under, expected_under_p10=expect, baseline_median=pct(meds, 50))
                if len(ranks) >= MIN_EPISODES:
                    p, z, o["reading_basis"] = o["local"]["p"], zl, "local"
        if len(vals) < MIN_EPISODES or p is None:
            o["reading"] = "too few events"
        elif p < P_READ:
            low = z < 0
            o["reading"] = ("lower than usual" if low else "higher than usual") if what == "level" else ("falls" if low else "rises")
        else:
            o["reading"] = "no clear difference" if what == "level" else "no clear trend"
        out[what] = o
    return out


def radio_link(name, tls, cen, events, o, utc_offset_h):
    """All the radio figures of one link. cen: the intervals (s) where the
    recording watched it; events: kind -> start times (s) in the engine's clock."""
    ints = [tl.interval for tl in tls if tl.interval]
    interval = float(statistics.median(ints)) if ints else None
    N = o.window or max(RF_MIN_WINDOW_S, 2 * (interval or 0))
    rfcov = iv_merge([iv for tl in tls for iv in tl.cover])
    D = iv_and(cen, rfcov)                       # recorded and with RF data
    V = iv_and(cen, iv_erode(rfcov, N))          # also N s of RF data either side
    Ds, Vs = [x[0] for x in D], [x[0] for x in V]
    r = dict(window_s=N, interval_s=interval, rf_hours=round(iv_len(rfcov) / 3600, 3),
             recorded_hours=round(iv_len(cen) / 3600, 3), hours=round(iv_len(D) / 3600, 3),
             series=len(tls), sources=sorted({tl.source for tl in tls if tl.source}),
             cell_key=sorted({tl.cell_kind for tl in tls if tl.cell_kind}), modem_down_samples=sum(tl.down for tl in tls))
    secs = {"rat": collections.Counter(), "band": collections.Counter()}
    nsa_s = tot_s = 0.0
    vals = {"sinr": [], "rsrq": [], "rsrp": []}
    hours = collections.defaultdict(lambda: dict(rf=0.0, sinr=[], rat=0, band=0, cell=0, outage=0, upload_mute=0, download_mute=0))
    sinr_pairs = []
    for tl in tls:
        for i, t in enumerate(tl.t):
            if tl.sinr[i] is not None:
                sinr_pairs.append((t, tl.sinr[i]))
            c = iv_clip(t, t + tl.w[i], D, Ds)
            if c <= 0:
                continue
            tot_s += c
            secs["rat"][tl.rat[i]] += c
            secs["band"][tl.band[i]] += c
            nsa_s += c if tl.nsa[i] else 0.0
            h = hours[hour_of(t, utc_offset_h)]
            h["rf"] += c
            if iv_has(D, Ds, t):
                for m in vals:
                    v = getattr(tl, m)[i]
                    if v is not None:
                        vals[m].append(v)
                        if m == "sinr":
                            h["sinr"].append(v)
    r["rat_pct"] = {k or "unknown": 100 * v / tot_s for k, v in secs["rat"].items()} if tot_s else {}
    r["band_pct"] = {k or "unknown": 100 * v / tot_s for k, v in secs["band"].items()} if tot_s else {}
    r["nr_nsa_pct"] = 100 * nsa_s / tot_s if tot_s else None
    for m, v in vals.items():
        r[m] = dict(n=len(v), **{f"p{p}": pct(v, p) for p in (10, 50, 90)})
    # Changes, per kind: the times (all of them, for the windows), how many
    # fell in D, and the hours in which one could have been seen.
    chg, r["changes"] = {}, {}
    for kind in ("rat", "band", "cell"):
        times, expo = [], 0.0
        for tl in tls:
            ts, spans, _ = changes_of(tl, kind)
            times += ts
            expo += sum(iv_clip(a, b, D, Ds) for a, b in spans)
        chg[kind] = sorted(times)
        inside = [c for c in times if iv_has(D, Ds, c)]
        for c in inside:
            hours[hour_of(c, utc_offset_h)][kind] += 1
        r["changes"][kind] = dict(n=len(inside), observed_hours=round(expo / 3600, 3),
                                  per_hour=len(inside) / (expo / 3600) if expo else None)
    chg["any"] = sorted({c for k in ("rat", "band", "cell") for c in chg[k]})
    r["changes"]["any"] = dict(n=len({c for c in chg["any"] if iv_has(D, Ds, c)}))
    # Events: outages and mutes against the changes, and the SINR before them.
    gap = max(EPISODE_S, 2 * N)
    Lw = max(o.local, 2 * N) if o.local > 0 else 0       # a neighbourhood smaller than the windows would be nonsense
    r["local_window_s"] = Lw
    need = 1 if (interval or 0) > 30 else 2
    S = Means(sinr_pairs)
    base_t = [t for t in S.t if iv_has(V, Vs, t)]
    r["events"] = {}
    for kind, _ in EVENT_KINDS:
        evs = sorted(events.get(kind, []))
        for e in evs:
            if iv_has(D, Ds, e):
                hours[hour_of(e, utc_offset_h)][kind] += 1
        ev = sorted(e for e in evs if iv_has(V, Vs, e))
        if not evs:
            continue
        eps = thin(ev, gap)
        x = dict(events=len(evs), in_rf=len(ev), episodes=len(eps), episode_gap_s=gap,
                 changes={c: cross_row(ev, eps, chg[c], N, V, Lw) for c in CROSS_KINDS})
        x["sinr_before"] = sinr_before(eps, S, base_t, need, 3, N, Lw) if vals["sinr"] else None
        r["events"][kind] = x
    r["by_hour"] = {h: dict(rf_hours=round(v["rf"] / 3600, 3), sinr_p50=pct(v["sinr"], 50),
                            **{k: v[k] for k in ("rat", "band", "cell", "outage", "upload_mute", "download_mute")})
                    for h, v in sorted(hours.items()) if v["rf"] > 0}
    return r


def jsonable(x):
    """Strict JSON: no NaN or infinity (None instead), keys as text."""
    if isinstance(x, dict):
        return {str(k): jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    if isinstance(x, float):
        return None if not math.isfinite(x) else sig(x, 6)
    return x


def iso(t):
    return None if t is None else time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t))


def summarize_radio(a, rf, o, utc_offset_h, ex=None):
    """The radio section: o has offset (s added to the RF times), window (N, or
    None), maps ([(leg or carrier, link)]) and the files read."""
    meta, L = a["meta"], a["L"]
    notes = []
    out = dict(files=rf.files, offset_s=o.offset, records=rf.records, series=[], unmapped=[], links={},
               links_without_rf=[], notes=notes)
    if rf.no_time:
        notes.append(f"{rf.no_time} line-protocol lines of the measurements used without a timestamp skipped (cell-monitor posts none: "
                     "export from InfluxDB, which stamps them, not the output of `cell-monitor -n`)")
    if rf.bad:
        notes.append(f"{rf.bad} lines or rows that could not be read skipped")
    if rf.no_cols:
        notes.append(f"{rf.no_cols} CSV rows under a header without _time or _measurement skipped "
                     "(a pivot() or keep() that dropped them?)")
    if rf.unknown:
        notes.append("measurements this does not know, skipped: " + ", ".join(f"{k} ({v})" for k, v in sorted(rf.unknown.items())))
    rows, tls = [], {}
    for key in sorted(rf.series):
        ser = rf.series[key]
        link, how, why = map_series(key, meta, o.maps)
        tl = build_timeline(ser, o.offset)
        tls[key] = tl
        row = dict(cpe_id=key[0], modem_id=key[1], leg=key[2], carrier=key[3], samples=len(ser.samples),
                   interval_s=tl.interval, first=iso(tl.t[0]) if tl.t else None, last=iso(tl.t[-1]) if tl.t else None,
                   modem_down_samples=tl.down, source=tl.source, cell_key=tl.cell_kind, link=link, how=how, reason=why)
        rows.append(row)
        if tl.sinr and any(v is not None and v > 50 for v in tl.sinr):
            notes.append(f"{key[0]}/{key[1]}: SINR up to {max(v for v in tl.sinr if v is not None):g}: not dB? "
                         "some Quectel firmware reports it in 1/5 dB from -20 dB (0 to 250); this reads it as dB")
        if tl.interval is None:
            notes.append(f"{key[0]}/{key[1]}: a single sample, no coverage")
    out["series"] = rows
    out["unmapped"] = [r for r in rows if not r["link"]]
    notes += pairing_notes(rows, meta)
    by_link = collections.defaultdict(list)
    for key, row in zip(sorted(rf.series), rows):
        if row["link"]:
            by_link[row["link"]].append(key)
    ceng_all = {}
    for n in a["links"]:
        ceng_all[n] = iv_merge([[x / 1000 for x in iv] for iv in L[n]["cover"]])
    for n in a["links"]:
        keys = by_link.get(n)
        if not keys:
            out["links_without_rf"].append(n)
            continue
        group = [tls[k] for k in keys]
        if len(keys) > 1:
            cv = [iv_and(ceng_all[n], t.cover) for t in group]
            for i in range(len(cv)):
                for j in range(i + 1, len(cv)):
                    both = iv_len(iv_and(cv[i], cv[j]))
                    if both > 0.5 * max(1.0, min(iv_len(cv[i]), iv_len(cv[j]))):
                        notes.append(f"link {n}: two RF series cover the same time ({'/'.join(keys[i][:2])} and "
                                     f"{'/'.join(keys[j][:2])}): two modems with the same leg?")
        ev = {"outage": [x / 1000 for x in L[n]["starts"]],
              "upload_mute": [x / 1000 for x in L[n]["mute_starts"]],
              "download_mute": [x / 1000 for x in L[n]["dmute_starts"]]}
        out["links"][n] = radio_link(n, group, ceng_all[n], ev, o, utc_offset_h)
        out["links"][n]["rf_series"] = ["/".join(k[:2]) for k in keys]
    # How the two clocks overlap, for the reader to check before trusting anything.
    allrf = iv_merge([iv for tl in tls.values() for iv in tl.cover])
    allc = iv_merge([iv for v in ceng_all.values() for iv in v])
    out["overlap_hours"] = round(iv_len(iv_and(allrf, allc)) / 3600, 3)
    out["rf_range"] = [iso(allrf[0][0]), iso(allrf[-1][1])] if allrf else None
    out["recording_range"] = [iso(allc[0][0]), iso(allc[-1][1])] if allc else None
    if ex is not None:
        ex.rf([(key, row, tls[key]) for key, row in zip(sorted(rf.series), rows) if row["link"]], meta, ceng_all, o.offset)
    if not rf.series:
        notes.append("no RF series in the files: nothing of the measurements cell-monitor writes could be read from them")
    elif not out["overlap_hours"]:
        notes.append("the RF data and the recording share no time: check the clocks (--rf-offset) and the time zone of the export")
    return jsonable(out)


# -- the report

def trio(d):
    return "/".join(f(d.get(f"p{p}")) for p in (10, 50, 90)) if d and d.get("n") else "-"


def shares(d, top=4):
    if not d:
        return "-"
    items = sorted(d.items(), key=lambda kv: (-kv[1], kv[0]))
    s = " ".join(f"{k} {v:.0f} %" for k, v in items[:top] if v >= 0.5)
    rest = sum(v for k, v in items[top:]) + sum(v for k, v in items[:top] if v < 0.5)
    return s + (f" other {rest:.0f} %" if rest >= 0.5 else "")


def fpv(p):
    return "-" if p is None else "<0.001" if p < 0.001 else f"{p:.2g}"


def report_radio(r, utc_offset_h):
    w = print
    w("\n## Radio\n")
    w("The RF metrics of cell-monitor against the links. The times of the RF data are InfluxDB's clock when each "
      "POST came in (cell-monitor sends none), in whole seconds"
      + (f", moved by {r['offset_s']:g} s" if r["offset_s"] else "") + "; they have to agree with the Pi's. "
      "Everything below is over the time where the recording watched the link and there is RF data for it.\n")
    for fl in r["files"]:
        w(f"- {fl['file']}: {fl['format']}, {fl['records']} {fl['unit']}")
    w(f"- RF data from {(r['rf_range'] or ['-', '-'])[0]} to {(r['rf_range'] or ['-', '-'])[1]} UTC; "
      f"recording from {(r['recording_range'] or ['-', '-'])[0]} to {(r['recording_range'] or ['-', '-'])[1]}; "
      f"{r['overlap_hours']} h in common.")
    for n in r["notes"]:
        w(f"- note: {n}")
    w("\n### RF series and links\n")
    w("| RF series (cpe/modem) | leg | carrier | link | by | samples | interval s | from | to | modem down |")
    w("| --- | --- | --- | --- | --- | ---: | ---: | --- | --- | ---: |")
    for s in r["series"]:
        w(f"| {s['cpe_id']}/{s['modem_id']} | {s['leg'] or '-'} | {s['carrier'] or '-'} | {s['link'] or 'NOT MAPPED'} | "
          f"{s['how'] or '-'} | {s['samples']} | {f(s['interval_s'])} | {s['first']} | {s['last']} | {s['modem_down_samples']} |")
    if r["unmapped"] or r["links_without_rf"]:
        w()
    for s in r["unmapped"]:
        w(f"- not mapped: {s['cpe_id']}/{s['modem_id']} (leg {s['leg'] or '-'}, carrier {s['carrier'] or '-'}): {s['reason']}")
    if r["links_without_rf"]:
        w(f"- links without RF data: {', '.join(r['links_without_rf'])}")
    if not r["links"]:
        w("\nNo link has RF data in common with the recording.")
        return
    w("\n### Radio per link\n")
    w("Time share of each RAT and band; p10/p50/p90 of the samples (SINR and RSRQ in dB, RSRP in dBm) from the source "
      "named (serving: the serving cell line, in NSA the LTE anchor's; rxpath: the mean of the rx paths; ca: the primary "
      "carrier). NSA: time with an NR leg next to LTE.\n")
    w("| link | h | RAT | band | NSA % | SINR p10/p50/p90 | RSRQ | RSRP | n | source |")
    w("| --- | ---: | --- | --- | ---: | --- | --- | --- | ---: | --- |")
    for n, d in r["links"].items():
        w(f"| {n} | {f(d['hours'], 2)} | {shares(d['rat_pct'])} | {shares(d['band_pct'])} | {f(d['nr_nsa_pct'], 0)} | "
          f"{trio(d['sinr'])} | {trio(d['rsrq'])} | {trio(d['rsrp'])} | {d['sinr']['n']} | {', '.join(d['sources']) or '-'} |")
    w("\n### Changes\n")
    w("Counts and rate per hour in which one could be seen. A change is a new value against the last one read; across a "
      "hole of more than 3 RF intervals nothing is counted. The cell is named by the cell id, else the PCI and "
      "frequency, else the primary carrier's.\n")
    w("| link | N s | RAT | band | cell | cell named by | RF series |")
    w("| --- | ---: | --- | --- | --- | --- | --- |")
    for n, d in r["links"].items():
        c = d["changes"]
        w(f"| {n} | {f(d['window_s'], 0)} | " + " | ".join(f"{c[k]['n']} ({f(c[k]['per_hour'], 1)}/h)" for k in ("rat", "band", "cell"))
          + f" | {', '.join(d['cell_key']) or '-'} | {', '.join(d['rf_series'])} |")
    w("\n### Radio changes and link events\n")
    w("For each event start in the recording, with RF data N s either side: has the RAT, band or cell changed within N s "
      "(any: one of the three)? Global chance: the share of that time that is within N s of a change, which an event at a "
      "random time would have; in brackets, the hits it implies. Before and after: hits with the change in the N s "
      "before the start, or at or after it. Local chance: the same share worked out for each episode over the time within "
      "L s of it (L in the table), so that hours in which both come thick (a moving car, a crowded evening, rain) do not "
      "count as one announcing the other; the expected hits are their sum. The p-values (one-sided; binomial for the global "
      "chance, Poisson-binomial for the local one, exact unless it is too big) and the reading are on the episodes, the "
      "events at least max(60 s, 2N) apart, because events in a burst share their window; they need "
      f"{MIN_EPISODES} episodes. The reading says which chance it used: the local one when there is one. Judged: the events "
      "with RF data N s either side. A mute is the engine's standby of a slow link (story 006).\n")
    w("| link | event | judged of all | change | within N s | global chance | before / after | episodes | episode hits | "
      "expected global / local | p global / local | reading |")
    w("| --- | --- | ---: | --- | --- | --- | --- | ---: | ---: | --- | --- | --- |")
    for n, d in r["links"].items():
        for kind, label in EVENT_KINDS:
            x = d["events"].get(kind)
            if not x:
                continue
            for c in CROSS_KINDS:
                y = x["changes"][c]
                rd = y["reading"] + (f" ({y['basis']})" if y["basis"] else "")
                w(f"| {n} | {label} | {x['in_rf']} of {x['events']} | {c} | {y['hits']} ({f(y['share_pct'], 0)} %) | "
                  f"{f(y['chance_pct'], 1)} % ({f(y['expected'], 1)}) | {y['before']} / {y['after']} | {y['episodes']} | "
                  f"{y['episode_hits']} | {f(y['expected_episodes'], 1)} / {f(y['local_expected'], 1)} | "
                  f"{fpv(y['p'])} / {fpv(y['p_local'])} | {rd} |")
    w("\n### SINR before link events\n")
    w("The mean SINR of the minute before each episode against the same mean at every RF sample time of the link (so "
      "the baseline has the same spread). Level: the mean rank of the episodes in that baseline (50 % is no "
      "difference; low is a lower SINR than usual), and how many fell under the baseline's p10 (the baseline "
      "gives the expected count). Global: the baseline is the whole link. Local: for each episode, only the means at sample "
      f"times within {f(next(iter(r['links'].values()))['local_window_s'], 0)} s of it, leaving out [e-60 s, e+N], so a long bad stretch "
      "does not read as a precursor; the reading is on the local rank when there is one. Trend: that minute minus the 4 "
      f"before it; negative is falling. The reading needs {MIN_EPISODES} episodes and p under {P_READ}; with several rows, "
      f"one of them under {P_READ} by chance is to be expected now and then, so the counts matter more than the word.\n")
    w("| link | event | episodes | minute before p50 dB | global baseline p50 | global: rank, p, under p10 (exp.) | "
      "local: rank, p, under p10 (exp.) | level | trend p50 dB | trend: rank, p | trend |")
    w("| --- | --- | ---: | ---: | ---: | --- | --- | --- | ---: | --- | --- |")
    for n, d in r["links"].items():
        for kind, label in EVENT_KINDS:
            x = d["events"].get(kind)
            if not x:
                continue
            sb = x["sinr_before"]
            if not sb:
                w(f"| {n} | {label} | {x['episodes']} | - | - | - | - | no SINR in the RF data | - | - | - |")
                continue
            lv, tr, lc = sb["level"], sb["trend"], sb["level"].get("local")
            loc = "-" if not lc else (f"{f(lc['mean_rank_pct'], 0)} %, {fpv(lc['p'])}, {lc['under_p10']} ({f(lc['expected_under_p10'], 1)})"
                                      f" of {lc['n']}")
            w(f"| {n} | {label} | {lv['n']} of {x['episodes']} | {f(lv['median'])} | {f(lv['baseline_median'])} | "
              f"{f(lv.get('mean_rank_pct'), 0)} %, {fpv(lv.get('p'))}, {lv.get('under_p10', '-')} ({f(lv.get('expected_under_p10'), 1)}) | "
              f"{loc} | {lv['reading']}{'' if lv['reading'] == 'too few events' else ' (' + lv['reading_basis'] + ')'} | {f(tr['median'])} | "
              f"{f(tr.get('mean_rank_pct'), 0)} %, {fpv(tr.get('p'))} | {tr['reading']} |")
    w(f"\n### Radio by hour (UTC{utc_offset_h:+g})\n")
    w("RF hours with the recording, the changes seen, the median SINR and the link events that began in them.\n")
    w("| link | hour | RF h | RAT chg | band chg | cell chg | SINR p50 | outages | upload mutes | download mutes |")
    w("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for n, d in r["links"].items():
        for h, v in d["by_hour"].items():
            w(f"| {n} | {int(h):02d} | {f(v['rf_hours'], 2)} | {v['rat']} | {v['band']} | {v['cell']} | {f(v['sinr_p50'])} | "
              f"{v['outage']} | {v['upload_mute']} | {v['download_mute']} |")


# --- Export: tidy CSVs for R and pandas -----------------------------------------------------

SEGMENT_COL = ("segment", "index", "run of samples with no gap or restart between them, from 0; the same in every file")
TIME_COLS = [
    ("time_utc", "ISO 8601, UTC", "the engine's clock (time_ms of the status) to the ms; empty if the status had none"),
    ("time_epoch", "s", "the same as seconds since 1970-01-01 UTC, to the ms"),
    ("uptime_ms", "ms", "engine uptime: steady inside a segment, starts over at a restart"),
    ("clock_synced", "0/1", "0: recorded before NTP set the Pi's clock, so time_utc and time_epoch are wrong"),
    ("interval_ms", "ms", "uptime since the previous sample of the segment; empty for the first"),
]
SAMPLES_COLS = [SEGMENT_COL,
                ("session", "text", "the engine's session id for the run"),
                ("sample", "index", "position in the segment, from 0 (the first has no interval: it is context)")] + TIME_COLS + [
    ("link", "text", "links[].name"),
    ("label", "text", "links[].label"),
    ("link_id", "int", "links[].id: the bit of this link in the masks of joint.csv"),
    ("state", "text", "links[].state (live, stalled, paused, ...)"),
    ("counted", "0/1", "1 if fieldrec counts this link at this sample (not paused, not the first of a segment)"),
    ("out", "0/1", "out by fieldrec's rule: state not live, or nothing received for 3 probe intervals (300 ms at least); empty if paused"),
    ("last_rx_ms_ago", "ms", "links[].last_rx_ms_ago"),
    ("rtt_ms", "ms", "links[].rtt_ms"),
    ("rtt_dev_ms", "ms", "rtt minus the median of the link's last 60 RTTs (signed); empty until 30 are known, and while out"),
    ("rtt_step_ms", "ms", "absolute change of rtt since the previous sample, when both were live"),
    ("rtt_behind_best_ms", "ms", "rtt minus the lowest rtt among the live links at this sample (2 or more)"),
    ("upload", "text", "links[].upload (active or muted)"),
    ("upload_muted", "0/1", "upload is muted: the engine's standby of a slow link"),
    ("upload_mutes", "count", "links[].upload_mutes, the engine's cumulative counter"),
    ("upload_mutes_delta", "count", "its growth since the previous sample (a counter that went down counts as 0)"),
    ("upload_state_ms", "ms", "links[].upload_state_ms: how long the upload state has lasted"),
    ("upload_behind_ms", "ms", "links[].upload_behind_ms as the engine reports it"),
    ("download_muted", "0/1", "links[].download_muted: the server has this link muted for download"),
    ("tx_packets_delta", "packets", "growth of links[].tx_packets since the previous sample"),
    ("server_rx_delta", "packets", "growth of the server's count of what it received on this link (server_view.rx, modulo 2^32)"),
    ("rx_first_delta", "packets", "growth of links[].rx_first: copies that arrived first"),
    ("rx_copies_delta", "packets", "growth of rx_first + rx_duplicate + rx_late: every copy that arrived on this link"),
    ("loss_down", "fraction", "1 - rx_copies_delta / packets the tunnel delivered in the interval, from 0 to 1; "
                              "empty unless 20 or more were delivered and the link was not muted for download"),
]
EVENTS_COLS = [
    ("kind", "text", "outage, upload_mute, download_mute, gap (the recorder stopped but the engine kept running) or restart (the engine restarted)"),
    ("link", "text", "links[].name; empty for gap and restart"),
    ("label", "text", "links[].label"),
    ("segment", "index", "the segment it began in (for gap and restart, the one that follows it)"),
    ("start_utc", "ISO 8601, UTC", "engine clock at the start; an outage starts at the link's last reply, a mute where the engine stamps it"),
    ("start_epoch", "s", "the same, seconds since 1970"),
    ("end_utc", "ISO 8601, UTC", "engine clock at the end; empty if it is not known"),
    ("end_epoch", "s", "the same"),
    ("duration_s", "s", "end minus start, by the engine's uptime where it can; empty if not known"),
    ("ends", "text", "recovered (halfway between the last sample out and the first one back), segment_end (the run stopped first, so "
                     "the end is the last sample out), within_interval (a mute that began and ended between two samples: no end), "
                     "remuted, paused; empty for gap and restart"),
]
JOINT_COLS = [SEGMENT_COL] + TIME_COLS + [
    ("deadline_ms", "ms", "the deadline after the first copy of a packet that late is judged at"),
    ("mask", "int", "a set of links as the engine's bit mask (bit i is link_id i)"),
    ("set", "text", "the same, as link names joined by +"),
    ("expect", "packets", "packets sent in the interval on exactly the links of the mask (the same at every deadline)"),
    ("late", "packets", "packets that were late or lost at the deadline on exactly the links of the mask (mask 0: none late). "
                        "P(set S late) = sum of late over the masks that contain S / the same of expect"),
]
REORDER_COLS = [SEGMENT_COL] + TIME_COLS + [
    ("source", "text", "client (the reorderer of the client, download) or server (one session's, upload); a server's runs are numbered apart "
                       "from the client's, from 0"),
    ("session", "text", "client: the engine's session id for the run, as in samples.csv; server: the session's id"),
    ("client", "text", "server: the name of the session's client, empty with a single one; empty for a client"),
    ("kind", "text", "delta (growth in the interval), gauge (the value at the end of the interval; no row when 0) or hist (growth of one bucket "
                     "of a histogram)"),
    ("metric", "text", "delta: delivered (packets handed on, to the TUN or before 0.12 to WireGuard: download.packets for a client, the session's upload_packets for a "
                       "server; a row in every interval counted) and each counter that grew: in_order, held_total, bypassed, split_rx, far, "
                       "resyncs, pool_full, lost_rack, lost_cap, lost_overflow, lost_flush, late_after_rack, late_after_cap, "
                       "late_after_overflow, late_old. gauge: held, held_max, pool_buffers, pool_free, max_burst. hist: wait (hold time of "
                       "each held packet) or late_by (how long after its gap was given up on a late packet came)"),
    ("bucket", "text", "hist: le_N (up to N ms; the engine's edges_ms) or gt_N (over the last edge); empty otherwise"),
    ("value", "count", "the number the kind and metric say"),
]
RF_COLS = [
    ("time_utc", "ISO 8601, UTC", "RF sample time after --rf-offset: on the engine's clock"),
    ("time_epoch", "s", "the same, seconds since 1970"),
    ("rf_time_epoch", "s", "the time as exported by InfluxDB, before --rf-offset (cell-monitor posts none: it is the server's clock at the POST)"),
    ("link", "text", "links[].name of the link the series maps to"),
    ("label", "text", "links[].label"),
    ("cpe_id", "text", "tag cpe_id"),
    ("modem_id", "text", "tag modem_id"),
    ("leg", "text", "tag leg, as cell-monitor sanitizes it"),
    ("carrier", "text", "tag carrier"),
    ("recorded", "0/1", "1 if the engine's recording watched the link at this time"),
    ("modem_up", "0/1", "cell_monitor up: 0 when the modem's port was gone; empty if not exported"),
    ("rat", "text", "lte, nr5g (stand-alone), nr5g-nsa or as the modem says; empty if unknown"),
    ("band", "text", "b7, n78, ...; empty if unknown"),
    ("cell_kind", "text", "how the cell is named in the series: cellid, pci (pci@frequency) or pcc (primary carrier)"),
    ("cell", "text", "the serving cell"),
    ("nsa", "0/1", "an NR leg next to LTE (cell_serving_nr mode nsa)"),
    ("sinr", "dB", "SINR from the source named in source"),
    ("rsrq", "dB", "RSRQ"),
    ("rsrp", "dBm", "RSRP"),
    ("source", "text", "measurement of sinr, rsrq and rsrp: cell_serving_lte, cell_serving_nr, cell_rxpath (mean of the rx paths) or cell_ca (primary carrier)"),
]
RF_CHANGES_COLS = [
    ("time_utc", "ISO 8601, UTC", "RF sample time (after --rf-offset) at which the new value was first read"),
    ("time_epoch", "s", "the same, seconds since 1970"),
    ("link", "text", "links[].name"),
    ("label", "text", "links[].label"),
    ("cpe_id", "text", "tag cpe_id"),
    ("modem_id", "text", "tag modem_id"),
    ("kind", "text", "rat, band or cell"),
    ("from", "text", "the value before"),
    ("to", "text", "the value after"),
    ("recorded", "0/1", "1 if the engine's recording watched the link then (the report counts those)"),
]
HS_NAMES = ("init", "response", "cookie", "other")
EXPORT_NAMES = ("samples.csv", "events.csv", "joint.csv", "tunnel.csv", "reorder.csv", "rf.csv", "rf_changes.csv", "README.txt")


def tunnel_cols(edges):
    """The columns of tunnel.csv: the tunnel's lost_all and, for a recording of 0.5.5 to 0.11, WireGuard sizes per interval;
    the sizes one column per bucket edge."""
    cols = [SEGMENT_COL] + TIME_COLS + [
        ("down_packets_delta", "packets", "growth of the packets the tunnel delivered (download.packets)"),
        ("lost_all_delta", "packets", "growth of lost_all: sequences that left the replay window without a copy on any link; empty if the counter "
                                      "went down or the status has none")]
    if edges:
        names = [f"le_{e}" for e in edges] + [f"gt_{edges[-1]}"]
        for w, what in (("up", "read from WireGuard by the client"), ("down", "delivered to WireGuard")):
            for k, unit in (("pkts", "packets"), ("bytes", "bytes")):
                cols += [(f"{w}_{k}_{n}", unit, f"{w}: data {k} {what} in the interval, at or under the edge (le) or over the last (gt)")
                         for n in names]
            cols += [(f"{w}_hs_{h}", "packets", f"{w}: {h} handshake packets in the interval") for h in HS_NAMES]
    return cols


def cell_str(v):
    if v is None:
        return ""
    if isinstance(v, tuple):
        return str(v[0]) if v[1] is None else f"{v[0]}@{int(v[1])}"
    return str(v)


class Exporter:
    """Writes the tidy CSVs of --export while the analysis goes through the
    recording, a row at a time (events are few: they are kept and sorted at
    the end). The headers come from the schemas above, which also make
    README.txt."""

    def __init__(self, dirn, gz, segs, server=()):
        self.dir, self.gz = dirn, gz
        os.makedirs(dirn, exist_ok=True)
        for name in EXPORT_NAMES:            # no file of an earlier export, in the other form, left beside the new ones
            for suffix in ("", ".gz"):
                try:
                    os.remove(os.path.join(dirn, name + suffix))
                except FileNotFoundError:
                    pass
        self.files = {}                  # name -> [csv writer, handle, rows]
        self.titles = {}
        self.events = []
        self.names = {}                  # the report's name of a link -> (name, label)
        self.mute = {}                   # (kind, link) -> the mute in progress
        self.sec, self.prefix = None, ""
        self.tkey, self.tval = None, None
        self.idn = {}
        self.maskstr = {}
        self.info = {}
        self.edges = next((s.sizes["edges"] for seg in segs for s in seg if s.sizes), None)
        self.open("samples.csv", SAMPLES_COLS, "one row per link per status sample the recorder kept")
        self.open("joint.csv", JOINT_COLS, "download: packets late on each set of links, per interval and deadline (from the passive instrument "
                                           "'joint'); only masks that grew")
        self.open("tunnel.csv", tunnel_cols(self.edges), "the tunnel per interval: packets delivered, lost on every link and, before 0.12, WireGuard sizes")
        # Only for a recording that has the field, so that an older one exports what it always did.
        self.server_reorder = any(s.sessions for seg in server for s in seg)
        if self.server_reorder or any(s.reorder for seg in segs for s in seg):
            self.open("reorder.csv", REORDER_COLS, "the receiver's reorderer per interval, long format: one row per counter that grew, gauge "
                                                   "and histogram bucket that grew, with the packets delivered in every interval counted")

    def open(self, name, cols, title):
        path = os.path.join(self.dir, name + (".gz" if self.gz else ""))
        fh = gzip.open(path, "wt", encoding="utf-8", newline="", compresslevel=6) if self.gz else open(path, "w", encoding="utf-8", newline="")
        w = csv.writer(fh, lineterminator="\n")
        w.writerow([c[0] for c in cols])
        self.files[name] = [w, fh, 0, cols]
        self.titles[name] = title

    def row(self, name, values):
        f = self.files[name]
        f[0].writerow(values)
        f[2] += 1

    # -- times

    def stamp(self, ms):
        """(ISO 8601 UTC, epoch seconds) of a time in ms since 1970, or two empty cells."""
        if ms is None:
            return "", ""
        ms = int(round(ms))
        sec, frac = divmod(ms, 1000)
        if sec != self.sec:
            self.sec, self.prefix = sec, time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(sec))
        return f"{self.prefix}.{frac:03d}Z", f"{sec}.{frac:03d}"

    def tcols(self, si, b, dt):
        """[segment, time_utc, time_epoch, uptime_ms, clock_synced, interval_ms] of a sample (the same list for all of its rows)."""
        if self.tkey is not b:
            iso_, ep = self.stamp(b.wall)
            self.tkey, self.tval = b, [si, iso_, ep, b.up, int(b.synced and b.wall is not None), dt]
        return self.tval

    # -- events

    def add_event(self, kind, n, si, up0, up1, wall0, ends, sort):
        name, label = self.names.get(n, (n, "")) if n else ("", "")
        dur = None if up1 is None else max(0.0, up1 - up0)
        wall1 = None if wall0 is None or dur is None else wall0 + dur
        s0, s1 = self.stamp(wall0), self.stamp(wall1)
        self.events.append(((si, sort), [kind, name, label, si, s0[0], s0[1], s1[0], s1[1],
                                         "" if dur is None else round(dur / 1000, 3), ends]))

    def outage(self, n, si, up0, up1, wall0, ends):
        self.add_event("outage", n, si, up0, up1, wall0, ends, wall0 if wall0 is not None else up0)

    def segment_begin(self, si, seg, prev):
        self.mute = {}
        first = seg[0]
        if prev is not None:
            restart = first.session != prev.session or first.up < prev.up
            dur = None
            if prev.wall is not None and first.wall is not None:
                dur = first.wall - prev.wall
            elif not restart:
                dur = first.up - prev.up
            s0, s1 = self.stamp(prev.wall), self.stamp(first.wall)
            self.events.append(((si, -1), ["restart" if restart else "gap", "", "", si, s0[0], s0[1], s1[0], s1[1],
                                           "" if dur is None else round(dur / 1000, 3), ""]))
        tc = self.tcols(si, first, None)
        for n, l in first.links.items():
            self.names[n] = (l.get("name") or str(l.get("id")), l.get("label") or "")
            paused = l.get("state") == "paused"
            self.row("samples.csv", self.sample_row(tc, first, 0, l, {}, 0, None if paused else int(link_out(l, first.probe))))

    def segment_end(self, si):
        for (kind, n), run in list(self.mute.items()):
            self.add_event(kind, n, si, run["up"], run["last"], run["wall"], "segment_end", run["wall"] if run["wall"] is not None else run["up"])
        self.mute = {}

    # -- samples and mutes

    def sample_row(self, tc, s, idx, l, x, counted, out):
        up, dm = l.get("upload"), l.get("download_muted")
        return [tc[0], s.session, idx] + tc[1:] + [
            l.get("name") or str(l.get("id")), l.get("label") or "", l.get("id"), l.get("state"), counted, None if out is None else int(out),
            l.get("last_rx_ms_ago"), l.get("rtt_ms"), x.get("dev"), x.get("step"), x.get("behind"),
            up, None if up is None else int(up == "muted"), l.get("upload_mutes"), x.get("mdelta"), l.get("upload_state_ms"),
            l.get("upload_behind_ms"), None if dm is None else int(bool(dm)),
            x.get("tx"), x.get("srv_rx"), x.get("rx_first"), x.get("got"), x.get("loss")]

    def interval(self, si, idx, a, b, dt, rows):
        tc = self.tcols(si, b, dt)
        for n, l in b.links.items():
            self.names[n] = (l.get("name") or str(l.get("id")), l.get("label") or "")
            x = rows.get(n) or {}
            paused = l.get("state") == "paused"
            self.row("samples.csv", self.sample_row(tc, b, idx, l, x, 0 if paused else 1, x.get("out")))
            pa = a.links.get(n)
            if paused:
                for kind in ("upload_mute", "download_mute"):
                    self.end_mute(kind, n, si, "paused")
            elif pa is not None:
                self.track_upload(n, si, a, b, dt, pa, l)
                self.track_download(n, si, a, b, pa, l)

    def end_mute(self, kind, n, si, ends, up1=None):
        run = self.mute.pop((kind, n), None)
        if run is not None:
            self.add_event(kind, n, si, run["up"], run["last"] if up1 is None else up1, run["wall"], ends,
                           run["wall"] if run["wall"] is not None else run["up"])

    def track_upload(self, n, si, a, b, dt, pa, l):
        """Upload mutes as episodes: the starts are fieldrec's (the counter grew, or the state turned to muted, the engine stamping how long
        it has lasted); an end is halfway between the last sample muted and the first one back."""
        prev_m, cur_m = pa.get("upload") == "muted", l.get("upload") == "muted"
        um = max((l.get("upload_mutes") or 0) - (pa.get("upload_mutes") or 0), int(not prev_m and cur_m))
        age = l.get("upload_state_ms") if cur_m else None
        lag = min(age, dt) if isinstance(age, (int, float)) and age >= 0 else 0
        up0, wall0 = b.up - lag, (b.wall - lag if b.wall is not None and b.synced else None)
        run = self.mute.get(("upload_mute", n))
        if run is not None and not cur_m:
            self.end_mute("upload_mute", n, si, "recovered", (run["last"] + b.up) / 2)
        elif run is not None and um > 0:
            self.end_mute("upload_mute", n, si, "remuted", up0)
        for _ in range(um - (1 if cur_m else 0)):                    # began and ended between two samples
            self.add_event("upload_mute", n, si, b.up, None, b.wall if b.synced else None, "within_interval", b.wall if b.synced and b.wall is not None else b.up)
        if um > 0 and cur_m:
            self.mute[("upload_mute", n)] = run = dict(up=up0, wall=wall0, last=b.up)
        if run is not None and cur_m:
            run["last"] = b.up

    def track_download(self, n, si, a, b, pa, l):
        cur, prev = bool(l.get("download_muted")), bool(pa.get("download_muted"))
        run = self.mute.get(("download_mute", n))
        if run is not None and not cur:
            self.end_mute("download_mute", n, si, "recovered", (run["last"] + b.up) / 2)
        elif cur and not prev:
            self.mute[("download_mute", n)] = run = dict(up=b.up, wall=b.wall if b.synced else None, last=b.up)
        if run is not None and cur:
            run["last"] = b.up

    # -- the tunnel

    def joint(self, si, a, b, deadlines, de, dls, ek):
        key = (a.idkey, b.idkey)
        names = self.idn.get(key)
        if names is None:
            names = {}
            for s_ in (a, b):
                for l in s_.links.values():
                    if isinstance(l.get("id"), int):
                        names[l["id"]] = l.get("name") or str(l["id"])
            self.idn[key] = names
        tc = self.tcols(si, b, b.up - a.up)
        for di, d in enumerate(deadlines):
            for m in sorted(set(de) | set(dls[di])):
                ms = self.maskstr.get((key, m))
                if ms is None:
                    ms = self.maskstr[(key, m)] = "+".join(names.get(i, f"id{i}") for i in range(m.bit_length()) if m >> i & 1)
                self.row("joint.csv", tc + [d, m, ms, de.get(m, 0), dls[di].get(m, 0)])

    def tunnel(self, si, a, b, lost, sizes):
        row = self.tcols(si, b, b.up - a.up) + [b.down - a.down, lost]
        if self.edges:
            for w in ("up", "down"):
                if sizes is None:
                    row += [None] * (2 * (len(self.edges) + 1) + len(HS_NAMES))
                else:
                    z = sizes[w]
                    row += z["data"] + z["data_bytes"] + [z[h] for h in HS_NAMES]
        self.row("tunnel.csv", row)

    def reorder(self, source, si, b, session, client, delivered, d, rb, dt):
        """The rows of one interval of one reorderer (delta_reorder's d; rb is its Reorder at the end of the interval): the packets
        delivered, each counter that grew, the gauges, each histogram bucket that grew."""
        lead = self.tcols(si, b, dt) + [source, session or "", client]

        def put(kind, metric, bucket, value):
            self.row("reorder.csv", lead + [kind, metric, bucket, value])
        put("delta", "delivered", "", delivered)
        for k, v in zip(RO_COUNTS, d[0]):
            if v:
                put("delta", k, "", v)
        for k, v in zip(RO_GAUGES, rb.gauges):
            if v is not None and (v or k == "pool_free"):
                put("gauge", k, "", v)
        names = [f"le_{e}" for e in rb.edges] + [f"gt_{rb.edges[-1]}" if rb.edges else "gt"]
        for k, h in zip(RO_HISTS, d[1:]):
            for n, v in zip(names, h or ()):
                if v:
                    put("hist", k, n, v)

    # -- radio

    def rf(self, entries, meta, cover, offset):
        """entries: (series key, its row of the summary, its timeline) of the series that map to a link; cover: link -> where the
        recording watched it, in seconds on the engine's clock."""
        self.open("rf.csv", RF_COLS, "one row per RF sample (a cell-monitor post) of each series that maps to a link, in time order per series")
        self.open("rf_changes.csv", RF_CHANGES_COLS, "each change of RAT, band or serving cell read in the RF data (not across a hole of more than 3 intervals)")
        for key, row, tl in entries:
            name, label = meta[row["link"]]
            starts = [x[0] for x in cover[row["link"]]]
            rec = lambda t: int(iv_has(cover[row["link"]], starts, t))      # noqa: E731
            for i, t in enumerate(tl.t):
                iso_, ep = self.stamp(t * 1000)
                self.row("rf.csv", [iso_, ep, f"{t - offset:.3f}", name, label, key[0], key[1], key[2], key[3], rec(t),
                                    None if tl.up[i] is None else int(tl.up[i] != 0), tl.rat[i], tl.band[i], tl.cell_kind, cell_str(tl.cell[i]),
                                    int(bool(tl.nsa[i])), tl.sinr[i], tl.rsrq[i], tl.rsrp[i], tl.meas[i]])
            for kind in ("rat", "band", "cell"):
                for t, old, new in changes_of(tl, kind)[2]:
                    iso_, ep = self.stamp(t * 1000)
                    self.row("rf_changes.csv", [iso_, ep, name, label, key[0], key[1], kind,
                                                cell_str(old) if kind == "cell" else old, cell_str(new) if kind == "cell" else new, rec(t)])

    # -- the end

    def close(self):
        self.open("events.csv", EVENTS_COLS, "outages, mutes (standby) and the gaps and restarts of the recording")
        for _, r in sorted(self.events, key=lambda e: e[0]):
            self.row("events.csv", r)
        for f in self.files.values():
            f[1].close()
        with open(os.path.join(self.dir, "README.txt"), "w", encoding="utf-8") as fh:
            fh.write(export_readme(self))


def fieldrec_version():
    """The short commit of the checkout this runs from, if git is there and it is one; otherwise 'unknown'."""
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        r = subprocess.run(["git", "-C", here, "rev-parse", "--short", "HEAD"], capture_output=True, text=True, timeout=3)
        v = r.stdout.strip() if r.returncode == 0 else ""
        if v:
            dirty = subprocess.run(["git", "-C", here, "status", "--porcelain", "--", "fieldrec.py"], capture_output=True, text=True, timeout=3)
            return v + (" (fieldrec.py modified since)" if dirty.stdout.strip() else "")
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def export_readme(ex):
    gz = ".gz" if ex.gz else ""
    out = ["fieldrec export", "================", "",
           f"Made by bench/fieldrec.py, commit {fieldrec_version()}, on {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())} UTC.",
           "Command: " + " ".join(ex.info.get("argv", [])), "",
           "Plain CSV, UTF-8, one header row, comma-separated, LF line ends" + (", each file gzipped (.csv.gz)" if ex.gz else "") + ".",
           "An empty cell is a missing value. Booleans are 0/1. Names are the engine's (links[].name and links[].label).", "",
           "TIMES", "-----",
           "- " + ", ".join(n for n in ("samples.csv", "events.csv", "joint.csv", "tunnel.csv", "reorder.csv") if n in ex.files)
           + ": time_utc and time_epoch are the engine's wall clock (time_ms of the status), the Pi's",
           "  clock, in UTC. Before NTP corrects it (the Pi has no RTC) the value is wrong: clock_synced is 0 there. Intervals inside a",
           "  segment should use uptime_ms, which is steady; uptime_ms starts over when the engine restarts, so it means nothing across segments.",
           "- A segment is a run of samples with no restart and no gap of more than 5 s; fieldrec starts over at each (events.csv has the gap",
           "  and restart rows). Cumulative counters are only differenced inside a segment.",
           "- rf.csv and rf_changes.csv (with --rf): the radio times are InfluxDB's clock when it received each cell-monitor POST (cell-monitor",
           "  sends no timestamp), in whole seconds. time_utc and time_epoch are those plus the offset given with --rf-offset"
           f" (here {ex.info.get('rf_offset', 0):g} s) to put them on the engine's clock; rf_time_epoch is the time as exported."
           " They agree only if the Pi's and the InfluxDB server's clocks do.", ""]
    if ex.server_reorder:
        out[-1:-1] = ["- reorder.csv rows with source server come from a server's recording: their times are that server's clock, and their",
                      "  segments are numbered apart from the client's (events.csv has none of them)."]
    for name in ("samples.csv", "events.csv", "joint.csv", "tunnel.csv", "reorder.csv", "rf.csv", "rf_changes.csv"):
        f = ex.files.get(name)
        if f is None:
            continue
        title = f"{name}{gz}: {ex.titles[name]}"
        out += [title, "-" * len(title), f"{f[2]} rows. Columns:"]
        w = max(len(c[0]) for c in f[3])
        out += [f"  {c[0]:<{w}}  [{c[1]}]  {c[2]}" for c in f[3]]
        out.append("")
    out += ["Reading them", "------------",
            "  R:       s <- readr::read_csv('samples.csv'); aggregate(rtt_ms ~ link, s, mean)    (data.table::fread reads .csv; .csv.gz needs R.utils)",
            "  pandas:  import pandas as pd; s = pd.read_csv('samples.csv', parse_dates=['time_utc']); s.groupby('link').rtt_ms.mean()",
            "  (readr and pandas read .csv.gz as it is.)", ""]
    return "\n".join(out)


class RadioOpts:
    def __init__(self, offset=0.0, window=None, maps=(), local=RF_LOCAL_S):
        self.offset, self.window, self.maps, self.local = offset, window, list(maps), local


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", help="cengarde-rec directories or files (.jsonl, .jsonl.gz)")
    ap.add_argument("--utc-offset", type=float, default=0.0, help="hours added to UTC for the hourly table (Chile: -3 or -4)")
    ap.add_argument("--json", help="also write the summary as JSON here")
    ap.add_argument("--export", metavar="DIR", help="write tidy CSVs for R and pandas into DIR (samples, events, joint, tunnel, reorder "
                    "if the recording has it and, with --rf, rf and rf_changes) and a README.txt that says what is in them")
    ap.add_argument("--export-gzip", action="store_true", help="with --export: write .csv.gz")
    ap.add_argument("--rf", nargs="+", action="extend", metavar="FILE",
                    help="cell-monitor's RF metrics from InfluxDB: annotated CSV or line protocol (plain or .gz); "
                         "it takes every name after it, so put the recordings before it")
    ap.add_argument("--rf-offset", type=float, default=0.0, metavar="S",
                    help="seconds added to the RF times to put them on the Pi's clock (InfluxDB's 5 s ahead of the Pi: -5)")
    ap.add_argument("--rf-window", type=float, metavar="S",
                    help="N, the window of the crossing with the link events (default: 2 RF intervals, at least 20 s)")
    ap.add_argument("--rf-local", type=float, default=RF_LOCAL_S, metavar="S",
                    help="L, the neighbourhood (+-) of the local chance and the local SINR baseline (default 1800; 0: global only)")
    ap.add_argument("--rf-map", action="append", default=[], metavar="LEG=LINK",
                    help="a leg (or carrier, or modem_id) tag that maps to a link by hand; repeat for more")
    ap.add_argument("--rf-precision", choices=("auto", "s", "ms", "us", "ns"), default="auto",
                    help="unit of the timestamps of line-protocol lines (default: by size)")
    o = ap.parse_args(argv)
    if not o.paths:
        ap.error("no recordings: name them before --rf, which takes every name after it "
                 "(fieldrec.py DIR --rf rf.csv)")
    if o.export_gzip and not o.export:
        ap.error("--export-gzip needs --export DIR")
    maps = []
    for m in o.rf_map:
        k, eq, v = m.partition("=")
        if not eq or not san_tag(k) or not san_tag(v):
            ap.error(f"--rf-map {m!r}: expected LEG=LINK")
        maps.append((san_tag(k), san_tag(v)))
    notes = []
    files = files_of(o.paths)
    if not files:
        print("no recordings found", file=sys.stderr)
        return 1
    rf = None
    if o.rf:
        try:
            rf = load_rf(o.rf, o.rf_precision)
        except (OSError, EOFError, UnicodeDecodeError, csv.Error) as e:
            print(f"cannot read the RF data: {e}", file=sys.stderr)
            return 1
    srv = []
    segs = segments(read_lines(files, notes), notes, srv)
    sevs = server_segments(srv, notes)
    if sum(len(s) for s in segs) < 2 and sum(len(s) for s in sevs) < 2:
        print("fewer than two samples", file=sys.stderr)
        return 1
    ex = Exporter(o.export, o.export_gzip, segs, sevs) if o.export else None
    try:
        a = analyze(segs, o.utc_offset, ex, sevs)
        s = summarize(a)
        if rf is not None:
            s["radio"] = summarize_radio(a, rf, RadioOpts(o.rf_offset, o.rf_window, maps, o.rf_local), o.utc_offset, ex)
    finally:
        if ex is not None:
            ex.info = dict(argv=[os.path.basename(sys.argv[0])] + (list(argv) if argv is not None else sys.argv[1:]), rf_offset=o.rf_offset)
            ex.close()
    report(s, notes, o.utc_offset)
    if o.json:
        with open(o.json, "w") as fh:
            json.dump(s, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
