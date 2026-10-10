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


class Sample:
    __slots__ = ("up", "wall", "session", "probe", "down", "links", "synced")

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
        for l in r.get("links") or []:
            name = l.get("name") or str(l.get("id"))
            if l.get("label"):
                name = f"{name} ({l['label']})"
            self.links[name] = l
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
                segments=len(segs), samples=sum(len(s) for s in segs))


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
        steps = [t for t, s in d["steps"] if s >= max(5.0, 4 * (pct([x for _, x in d["steps"]], 50) or 0))]
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
