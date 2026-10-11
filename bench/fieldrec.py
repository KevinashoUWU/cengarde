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
  per hour of the day. Link ids come from links[].id; an id of 8 or more only
  shows in the engine's "other" counter and is not analyzed;
- lost on every link (download): the tunnel's loss, overall and per hour;
- WireGuard sizes, per direction: the share of data packets and of bytes at or
  under each edge, and the handshake counts.

Usage: bench/fieldrec.py [--utc-offset H] [--json OUT] DIR|FILE...
"""

import argparse
import bisect
import collections
import glob
import gzip
import json
import math
import os
import statistics
import sys

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
                 starts=[], steps=[], hours=collections.defaultdict(lambda: [0, 0, [], 0.0, 0]))
         for n in links}
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
        state = {n: dict(out=False, start=None, last=None) for n in links}
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
                if l.get("state") == "paused":
                    continue
                d = L[n]
                present.append(n)
                out = link_out(l, b.probe)
                d["time"] += dt
                st = state[n]
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
                segments=len(segs), samples=sum(len(s) for s in segs),
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
        w(f"- {j['other']} ({fp(j['other_pct'])} % of the packets) had a link of id 8 or more: outside the engine's "
          "histograms, so those links and those packets are not analyzed.")
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+", help="cengarde-rec directories or files (.jsonl, .jsonl.gz)")
    ap.add_argument("--utc-offset", type=float, default=0.0, help="hours added to UTC for the hourly table (Chile: -3 or -4)")
    ap.add_argument("--json", help="also write the summary as JSON here")
    o = ap.parse_args(argv)
    notes = []
    files = files_of(o.paths)
    if not files:
        print("no recordings found", file=sys.stderr)
        return 1
    segs = segments(read_lines(files, notes), notes)
    if not segs or sum(len(s) for s in segs) < 2:
        print("fewer than two samples", file=sys.stderr)
        return 1
    s = summarize(analyze(segs, o.utc_offset))
    report(s, notes, o.utc_offset)
    if o.json:
        with open(o.json, "w") as fh:
            json.dump(s, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
