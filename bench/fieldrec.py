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
- per hour of the day, each link's outages, RTT and loss.

A link is out while its state is not "live" or nothing came from it for
3 probe intervals (at least 300 ms); "paused" links (set off by hand) do
not count. Resolution is the sampling interval (status_interval_ms); the
start of an outage is placed at its last reply, which the engine stamps.

The passive instruments of the engine (status fields "joint", "lost_all" and
"sizes", packages 0.5.5) add three sections; a recording without them says
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
- WireGuard sizes, per direction: the share of data packets and of bytes at or
  under each edge, and the handshake counts.

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
time those windows cover), with the counts and a binomial test; the SINR of the
minute before an event against the link's own SINR; and a table per hour.

Usage: bench/fieldrec.py [--utc-offset H] [--json OUT] DIR|FILE...
                         [--rf FILE... [--rf-offset S] [--rf-window S]
                          [--rf-map LEG=LINK] [--rf-precision P]]
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


class Sample:
    __slots__ = ("up", "wall", "session", "probe", "down", "links", "synced",
                 "ids", "idkey", "lost_all", "joint", "sizes")

    def __init__(self, r):
        self.up = int(r["uptime_ms"])
        if "time_ms" in r:
            self.wall = int(r["time_ms"])
        elif "t" in r:
            self.wall = int(float(r["t"]) * 1000)
        else:
            self.wall = None
        self.session = r.get("session")
        self.probe = int(r.get("probe_interval_ms") or 100)
        self.down = int((r.get("download") or {}).get("packets") or 0)
        self.links = {}
        self.ids = {}                  # link id (the bit in the engine's masks) -> name
        for l in r.get("links") or []:
            name = link_name(l)
            self.links[name] = l
            if isinstance(l.get("id"), int):
                self.ids[l["id"]] = name
        self.idkey = tuple(sorted(self.ids.items()))
        self.lost_all = count_or_none(r.get("lost_all"))
        self.joint = parse_joint(r.get("joint"))
        self.sizes = parse_sizes(r.get("sizes"))
        self.synced = True


def segments(lines, notes):
    """Runs of samples from one engine run without gaps; repeated samples
    (same uptime) dropped."""
    segs, cur, wall_s = [], [], False
    for r in lines:
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
    tol = 2000 if wall_s else 1000
    unsynced = 0
    for seg in segs:
        last = next((s for s in reversed(seg) if s.wall is not None), None)
        for s in seg:
            s.synced = last is not None and s.wall is not None and \
                abs((s.wall - s.up) - (last.wall - last.up)) <= tol
            unsynced += not s.synced
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


def analyze(segs, utc_offset_h=0.0):
    links = sorted({n for seg in segs for s in seg for n in s.links})
    L = {n: dict(time=0, out_time=0, events=[], rtt=[], dev=[], behind=[], up_behind=[],
                 loss_num=0.0, loss_den=0, up_tx=0, up_rx=0, mutes=0, muted_time=0,
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
        hist = collections.defaultdict(list)   # rolling RTTs for the median
        # An outage runs from its last reply (stamped by the engine) to
        # halfway between its last sample out and the first one back.
        state = {n: dict(out=False, start=None, last=None, cov=None) for n in links}
        upwin = {n: [0, 0, 0] for n in links}  # tx, rx, ms
        for a, b in zip(seg, seg[1:]):
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
                if l.get("state") == "paused":
                    continue
                d = L[n]
                present.append(n)
                out = link_out(l, b.probe)
                d["time"] += dt
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
                        if b.synced:
                            d["starts"].append(b.wall - ago)
                    st["last"] = b.up
                else:
                    if st["out"]:
                        end = (st["last"] + b.up) / 2
                        d["events"].append(((si, st["start"]), end - st["start"]))
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
                        pa = a.links.get(n)
                        if pa and not link_out(pa, a.probe) and b.synced:
                            ra = float(pa.get("rtt_ms") or 0)
                            if ra > 0:
                                d["steps"].append((b.wall, abs(r - ra)))
                    if l.get("upload_behind_ms") is not None:
                        d["up_behind"].append(float(l["upload_behind_ms"]))
                if l.get("upload") == "muted":
                    d["muted_time"] += dt
                pa = a.links.get(n)
                if pa is not None:
                    d["mutes"] += max(0, (l.get("upload_mutes") or 0) - (pa.get("upload_mutes") or 0))
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
                    got = sum((l.get(k) or 0) - (pa.get(k) or 0) for k in ("rx_first", "rx_duplicate", "rx_late"))
                    if dD >= MIN_PKTS and not l.get("download_muted"):
                        loss = min(1.0, max(0.0, 1 - got / dD))
                        d["loss_num"] += loss * dD
                        d["loss_den"] += dD
                        if hour is not None:
                            d["hours"][hour][3] += loss * dD
                            d["hours"][hour][4] += dD
                    tx = (l.get("tx_packets") or 0) - (pa.get("tx_packets") or 0)
                    rx = ((l.get("server_view") or {}).get("rx", 0) - (pa.get("server_view") or {}).get("rx", 0)) % 2**32
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
        for n, st in state.items():
            if st["out"]:
                L[n]["events"].append(((si, st["start"]), st["last"] - st["start"]))
    return dict(links=links, L=L, simul=simul, pair_t=pair_t, pair_out=pair_out, corr=corr,
                spread_all=spread_all, spread_two=spread_two, total_ms=total_ms, gap_ms=gap_ms,
                segments=len(segs), samples=sum(len(s) for s in segs), meta=meta,
                bond=collect_bonding(segs, utc_offset_h))


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


def collect_bonding(segs, utc_offset_h=0.0):
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
    for seg in segs:
        for a, b in zip(seg, seg[1:]):
            keys = (-1,)
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
                    for w in ("up", "down"):
                        s = sizes[w]
                        for k in ("data", "data_bytes"):
                            s[k] = d[w][k] if s[k] is None else [x + y for x, y in zip(s[k], d[w][k])]
                        for k in HANDSHAKES:
                            s[k] += d[w][k]
    return dict(joint=joint, lost=lost, sizes=sizes)


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


def f(x, p=1):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:.{p}f}"
    return str(x)


def report(s, notes, utc_offset_h):
    w = print
    w("# Field recording\n")
    w(f"{s['hours']} h recorded in {s['segments']} runs ({s['samples']} samples), "
      f"{s['gaps_hours']} h of gaps between them.\n")
    for n in notes:
        w(f"- note: {n}")
    if notes:
        w()
    w("## Links\n")
    w("| link | h | up % | outages | p50/p90/max s | <=1/1-3/3-10/10-60/>60 s | RTT p50/p95/p99 ms | dev p95 | behind best p50/p99 | loss down % | loss up % | mutes | muted % |")
    w("| --- | ---: | ---: | ---: | --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: |")
    for n, d in s["links"].items():
        o, r, b = d["outage_s"], d["rtt_ms"], d["behind_best_ms"]
        w(f"| {n} | {d['hours']} | {f(d['up_pct'], 3)} | {d['outages']} | {f(o['p50'])}/{f(o['p90'])}/{f(o['max'])} | "
          f"{'/'.join(str(v) for v in o['buckets'].values())} | {f(r[50])}/{f(r[95])}/{f(r[99])} | "
          f"{f(d['rtt_dev_ms_p95'])} | {f(b[50])}/{f(b[99])} | {f(d['loss_down_pct'], 3)} | "
          f"{f(d['loss_up_pct'], 3)} | {d['mutes']} | {f(d['muted_pct'], 2)} |")
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


def no_data(x, what):
    """Say why a section is empty; True if it is."""
    if x is None:
        print(f"No data: the recording has no `{what}` field (an engine without the passive instruments of 0.5.5).")
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
    if no_data(z, "sizes"):
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


# --- Radio: the RF metrics of SmoothStream's cell-monitor, crossed with the links ---------------

RF_GAP_K = 3           # a gap longer than this many RF intervals is a hole, not coverage
RF_MIN_WINDOW_S = 20   # the window of the crossing is 2 RF intervals, at least this (N)
RF_PRE_S = 60          # "the minute before" an event
RF_TREND_S = 300       # the minutes before that one, for the trend of the SINR
MIN_EPISODES = 10      # independent events needed before the report reads a test
EPISODE_S = 60         # events closer than this (or than 2N) are one episode for the tests
P_READ = 0.01          # a test is read as a difference below this
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
    r = {"rat": rat, "band": band, "nsa": s.nsa, "up": s.up}
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
    return tl


def changes_of(tl, kind):
    """(times a value changed, [start, end] spans in which a change would have
    shown). Two readings of a value count only if no more than RF_GAP_K
    intervals lie between them: after a longer hole nothing can be said."""
    vals = getattr(tl, kind)
    cap = RF_GAP_K * tl.interval if tl.interval else 0
    times, spans, last_t, last_v = [], [], None, None
    for t, v in zip(tl.t, vals):
        if v is None:
            continue
        if last_t is not None and t - last_t <= cap:
            spans.append((last_t, t))
            if v != last_v:
                times.append(t)
        last_t, last_v = t, v
    return times, spans


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


def cross_row(evs, eps, cs, N, V):
    """One kind of event against one kind of change. cs: the sorted change
    times. An event has one if a change lies within N s of its start; chance
    is the part of V (the time where an event can be judged) within N s of a
    change, which is the probability that an event placed at random would
    have one. The test is on the episodes."""
    n, vlen = len(evs), iv_len(V)
    if not cs:
        return dict(n=n, hits=0, share_pct=0.0 if n else None, chance_pct=0.0, expected=0.0, before=0, after=0,
                    episodes=len(eps), episode_hits=0, p=None, reading="no changes")

    def hit(e, lo, hi):
        i = bisect.bisect_left(cs, e + lo)
        return i < len(cs) and cs[i] <= e + hi

    def chance(lo, hi):
        W = iv_merge([[c - hi, c - lo] for c in cs])
        return iv_len(iv_and(W, V)) / vlen if vlen else 0.0

    q = chance(-N, N)
    hits = sum(hit(e, -N, N) for e in evs)
    eh = sum(hit(e, -N, N) for e in eps)
    p = binom_tail(eh, len(eps), len(eps), q) if eps else None
    if len(eps) < MIN_EPISODES:
        reading = "too few events"
    elif p < P_READ:
        reading = "more than chance"
    elif binom_tail(0, eh, len(eps), q) < P_READ:
        reading = "less than chance"
    else:
        reading = "about chance"
    return dict(n=n, hits=hits, share_pct=100 * hits / n if n else None, chance_pct=100 * q, expected=n * q,
                before=sum(hit(e, -N, -1e-9) for e in evs), after=sum(hit(e, 0, N) for e in evs),
                before_chance_pct=100 * chance(-N, 0), after_chance_pct=100 * chance(0, N),
                episodes=len(eps), episode_hits=eh, p=p, reading=reading)


def sinr_before(eps, S, base_times, need, need_trend):
    """The SINR of the minute before each episode against the link's own: the
    mean over [e-60, e) of each event, and the same mean at every RF sample
    time, so the baseline is the same statistic. The trend is that mean minus
    the mean of the 4 minutes before it."""
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
        if len(vals) < MIN_EPISODES or o.get("p") is None:
            o["reading"] = "too few events"
        elif o["p"] < P_READ:
            low = o["z"] < 0
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
            ts, spans = changes_of(tl, kind)
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
                 changes={c: cross_row(ev, eps, chg[c], N, V) for c in CROSS_KINDS})
        x["sinr_before"] = sinr_before(eps, S, base_t, need, 3) if vals["sinr"] else None
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


def summarize_radio(a, rf, o, utc_offset_h):
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
      "(any: one of the three)? Chance: the share of that time that is within N s of a change, which an event at a "
      "random time would have; in brackets, the hits it implies. Before and after: hits with the change in the N s "
      "before the start, or at or after it. The p-value (binomial, one-sided) and the reading use the episodes, the "
      "events at least max(60 s, 2N) apart, because events in a burst share their window; they need "
      f"{MIN_EPISODES} episodes. Part of an excess can be the hour of the day (see the table by hour): both kinds of "
      "event may be more frequent at the same hours. Judged: the events with RF data N s either side. A mute is the "
      "engine's standby of a slow link (story 006).\n")
    w("| link | event | judged of all | change | within N s | by chance | before / after | episodes | episode hits | p | reading |")
    w("| --- | --- | ---: | --- | --- | --- | --- | ---: | ---: | ---: | --- |")
    for n, d in r["links"].items():
        for kind, label in EVENT_KINDS:
            x = d["events"].get(kind)
            if not x:
                continue
            for c in CROSS_KINDS:
                y = x["changes"][c]
                w(f"| {n} | {label} | {x['in_rf']} of {x['events']} | {c} | {y['hits']} ({f(y['share_pct'], 0)} %) | "
                  f"{f(y['chance_pct'], 1)} % ({f(y['expected'], 1)}) | {y['before']} / {y['after']} | {y['episodes']} | "
                  f"{y['episode_hits']} | {fpv(y['p'])} | {y['reading']} |")
    w("\n### SINR before link events\n")
    w("The mean SINR of the minute before each episode against the same mean at every RF sample time of the link (so "
      "the baseline has the same spread). Level: the mean rank of the episodes in that baseline (50 % is no "
      "difference; low is a lower SINR than usual), and how many fell under the baseline's p10 (the baseline "
      "gives the expected count). Trend: that minute minus the 4 before it; negative is falling. The reading needs "
      f"{MIN_EPISODES} episodes and p under {P_READ}; with several rows, one of them under {P_READ} by chance is "
      "to be expected now and then, so the counts matter more than the word.\n")
    w("| link | event | episodes | minute before p50 dB | baseline p50 | under p10 (expected) | level: rank, p | level | "
      "trend p50 dB | trend: rank, p | trend |")
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
            lv, tr = sb["level"], sb["trend"]
            w(f"| {n} | {label} | {lv['n']} of {x['episodes']} | {f(lv['median'])} | {f(lv['baseline_median'])} | "
              f"{lv.get('under_p10', '-')} ({f(lv.get('expected_under_p10'), 1)}) | "
              f"{f(lv.get('mean_rank_pct'), 0)} %, {fpv(lv.get('p'))} | {lv['reading']} | {f(tr['median'])} | "
              f"{f(tr.get('mean_rank_pct'), 0)} %, {fpv(tr.get('p'))} | {tr['reading']} |")
    w(f"\n### Radio by hour (UTC{utc_offset_h:+g})\n")
    w("RF hours with the recording, the changes seen, the median SINR and the link events that began in them.\n")
    w("| link | hour | RF h | RAT chg | band chg | cell chg | SINR p50 | outages | upload mutes | download mutes |")
    w("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for n, d in r["links"].items():
        for h, v in d["by_hour"].items():
            w(f"| {n} | {int(h):02d} | {f(v['rf_hours'], 2)} | {v['rat']} | {v['band']} | {v['cell']} | {f(v['sinr_p50'])} | "
              f"{v['outage']} | {v['upload_mute']} | {v['download_mute']} |")


class RadioOpts:
    def __init__(self, offset=0.0, window=None, maps=()):
        self.offset, self.window, self.maps = offset, window, list(maps)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", help="cengarde-rec directories or files (.jsonl, .jsonl.gz)")
    ap.add_argument("--utc-offset", type=float, default=0.0, help="hours added to UTC for the hourly table (Chile: -3 or -4)")
    ap.add_argument("--json", help="also write the summary as JSON here")
    ap.add_argument("--rf", nargs="+", action="extend", metavar="FILE",
                    help="cell-monitor's RF metrics from InfluxDB: annotated CSV or line protocol (plain or .gz); "
                         "it takes every name after it, so put the recordings before it")
    ap.add_argument("--rf-offset", type=float, default=0.0, metavar="S",
                    help="seconds added to the RF times to put them on the Pi's clock (InfluxDB's 5 s ahead of the Pi: -5)")
    ap.add_argument("--rf-window", type=float, metavar="S",
                    help="N, the window of the crossing with the link events (default: 2 RF intervals, at least 20 s)")
    ap.add_argument("--rf-map", action="append", default=[], metavar="LEG=LINK",
                    help="a leg (or carrier, or modem_id) tag that maps to a link by hand; repeat for more")
    ap.add_argument("--rf-precision", choices=("auto", "s", "ms", "us", "ns"), default="auto",
                    help="unit of the timestamps of line-protocol lines (default: by size)")
    o = ap.parse_args(argv)
    if not o.paths:
        ap.error("no recordings: name them before --rf, which takes every name after it "
                 "(fieldrec.py DIR --rf rf.csv)")
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
    segs = segments(read_lines(files, notes), notes)
    if not segs or sum(len(s) for s in segs) < 2:
        print("fewer than two samples", file=sys.stderr)
        return 1
    a = analyze(segs, o.utc_offset)
    s = summarize(a)
    if rf is not None:
        s["radio"] = summarize_radio(a, rf, RadioOpts(o.rf_offset, o.rf_window, maps), o.utc_offset)
    report(s, notes, o.utc_offset)
    if o.json:
        with open(o.json, "w") as fh:
            json.dump(s, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
