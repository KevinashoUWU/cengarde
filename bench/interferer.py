#!/usr/bin/env python3
"""The controlled interferer's sampler, measures and table (lab.d/interferer.sh).

  interferer.py sample RUN                 every 100 ms, both links' netem
                                           counters (and once a second the
                                           client's status) to RUN/if-samples.jsonl
  interferer.py measure RUN CASE REP LABEL RATE DIV EXTRA
                                           the run's JSON line (stdout)
  interferer.py table FILE                 the runs of FILE as a Markdown table

What each measure means is in lab.d/interferer.sh. SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import subprocess
import sys
import time

DELAY = {1: 0.020, 2: 0.030}  # each link's one-way delay up, s (interferer.sh)
SIZE = 1400  # a dropped packet, bytes


def sample(run):
    out = open(run + "/if-samples.jsonl", "w")
    last_st = 0.0
    while True:
        t = time.time()
        rec = {"t": t}
        for n in (1, 2):
            try:
                q = json.loads(subprocess.run(["ip", "netns", "exec", "cli", "tc", "-s", "-j", "qdisc", "show", "dev",
                                               "l%d" % n], capture_output=True, text=True).stdout)[0]
                rec["l%d" % n] = [q.get("bytes", 0), q.get("backlog", 0), q.get("drops", 0)]
            except (ValueError, IndexError, OSError):
                pass
        if t - last_st >= 1:
            last_st = t
            try:
                with open(run + "/client.json") as f:
                    d = json.load(f)
                rec["cap"] = [l.get("capacity_mbit") for l in d["links"]]
                rec["qpred"] = [l.get("queue_pred_ms") for l in d["links"]]
            except (ValueError, KeyError, OSError):
                pass
        out.write(json.dumps(rec) + "\n")
        out.flush()
        time.sleep(max(0.0, 0.1 - (time.time() - t)))


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def measure(run, case, rep, label, rate, div, extra):
    rate, div = float(rate), float(div)
    samples = []
    for line in open(run + "/if-samples.jsonl"):
        try:
            samples.append(json.loads(line))
        except ValueError:
            pass
    marks = [(l.split()[0], float(l.split()[1])) for l in open(run + "/if-marks") if l.strip()]
    # the cuts (cut .. end), and link 1's rate from each mark: cut, IF_RATE /
    # DIV; drain, IF_RATE (case drain: the queue empties); half, IF_RATE / 2;
    # end, IF_RATE
    cuts, steps, a = [], [], None
    for k, t in marks:
        steps.append((t, {"cut": rate / div, "drain": rate, "half": rate / 2, "end": rate}[k]))
        if k == "cut":
            a = t
        elif k == "end" and a is not None:
            cuts.append((a, t))
            a = None
    drains = [t for k, t in marks if k == "drain"]

    def rate_at(n, t):
        r = rate
        if n == 1:
            for ts, v in steps:
                if ts <= t:
                    r = v
        return r

    def series(n):
        """(t, Mbit/s offered over the next 0.5 s) and (t, queue ms)."""
        pts = [(s["t"], s["l%d" % n]) for s in samples if "l%d" % n in s]
        off, q = [], []
        j = 0
        for i, (t, v) in enumerate(pts):
            q.append((t, max(0.0, v[1] * 8 / (rate_at(n, t) * 1e6) - DELAY[n]) * 1000))
            j = max(j, i)
            while j + 1 < len(pts) and pts[j][0] - t < 0.5:
                j += 1
            if j > i:
                t2, w = pts[j]
                o = (w[0] + w[1] + SIZE * w[2]) - (v[0] + v[1] + SIZE * v[2])
                off.append((t, o * 8 / (t2 - t) / 1e6))
        return off, q

    off1, q1 = series(1)
    caps = [(s["t"], s["cap"][0]) for s in samples if s.get("cap") and s["cap"][0] is not None]
    eps = []
    for a, b in cuts:
        b_cut = next((t for k, t in marks if a < t <= b and k in ("drain", "end")), b)
        pre = mean([o for t, o in off1 if a - 4 <= t < a - 0.5])
        precap = mean([c for t, c in caps if a - 4 <= t < a])
        e = {"off_s": None, "back_s": None, "cap_s": None, "cap_max": None, "cap_over": None, "q_ms": None,
             "q100_s": None, "pre_mbit": round(pre, 1) if pre else None,
             "pre_cap": round(precap, 1) if precap else None}
        # the reaction: the first second from which the split offered link 1
        # no more than its new rate
        e["off_s"] = next((round(t - a, 2) for i, (t, o) in enumerate(off1) if a <= t < b_cut and
                           mean([x for u, x in off1[i:] if u < t + 1]) <= rate / div), None)
        if pre:
            e["back_s"] = next((round(t - b, 2) for t, o in off1 if t >= b and o >= 0.8 * pre), None)
        if precap:
            e["cap_s"] = next((round(t - b, 2) for t, c in caps if t >= b and c >= 0.8 * precap), None)
        pk = [c for t, c in caps if b <= t < b + 15]
        e["cap_max"] = round(max(pk), 1) if pk else None
        # case drain: the largest capacity while the cell gives half, over
        # that half (an overshoot from the drain is > 1)
        for d in drains:
            if a < d < b:
                ov = [c for t, c in caps if d + 1 <= t < b]
                e["cap_over"] = round(max(ov) / (rate / 2), 2) if ov else None
        qs = [(t, q) for t, q in q1 if a <= t < b + 5]
        e["q_ms"] = round(max(q for t, q in qs)) if qs else None
        e["q100_s"] = round(sum(1 for t, q in qs if q > 100) * 0.1, 1) if qs else None
        eps.append(e)

    ip = {}
    try:
        d = json.load(open(run + "/if-iperf3.json"))
        st = d["end"]["streams"][0]["sender"]
        ip = {"mbit": round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 1),
              "retrans": d["end"]["sum_sent"].get("retransmits"),
              "rtt_max_ms": round(st.get("max_rtt", 0) / 1000)}
    except (ValueError, KeyError, IndexError, OSError):
        pass
    rtts = sorted(float(l.split("time=")[1].split()[0]) for l in open(run + "/if-ping.txt") if "time=" in l)
    pg = {"p99_ms": round(rtts[max(0, int(len(rtts) * 0.99) - 1)]) if rtts else None,
          "max_ms": round(rtts[-1]) if rtts else None}

    def agg(k):
        v = [e[k] for e in eps if e[k] is not None]
        return max(v) if v else None

    return {"label": label, "case": case, "rep": int(rep), "extra": extra, "episodes": eps,
            "off_s": agg("off_s"), "back_s": agg("back_s"), "cap_s": agg("cap_s"), "cap_max": agg("cap_max"),
            "cap_over": agg("cap_over"), "q_ms": agg("q_ms"),
            "q100_s": round(sum(e["q100_s"] or 0 for e in eps), 1),
            "missed_back": sum(1 for e in eps if e["back_s"] is None),
            "iperf3": ip, "ping": pg}


def table(path):
    rows = {}
    for line in open(path):
        if line.strip():
            d = json.loads(line)
            rows.setdefault((d["label"], d["case"]), []).append(d)

    def med(v):
        v = sorted(x for x in v if x is not None)
        return v[len(v) // 2] if v else None

    def f(x):
        return "-" if x is None else "%g" % x

    out = ["| variant | case | n | off_s | back_s | cap_s | cap_over | q_ms | q100_s | TCP RTT max ms | retrans "
           "| ping p99 ms | Mbit/s |",
           "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for (lab, case), runs in rows.items():
        out.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            lab, case, len(runs), f(med([r["off_s"] for r in runs])),
            f(med([r["back_s"] if not r["missed_back"] else None for r in runs])),
            f(med([r["cap_s"] for r in runs])), f(med([r["cap_over"] for r in runs])),
            f(med([r["q_ms"] for r in runs])), f(med([r["q100_s"] for r in runs])),
            f(med([r["iperf3"].get("rtt_max_ms") for r in runs])),
            f(med([r["iperf3"].get("retrans") for r in runs])), f(med([r["ping"]["p99_ms"] for r in runs])),
            f(med([r["iperf3"].get("mbit") for r in runs]))))
    return "\n".join(out)


def main(argv):
    if len(argv) >= 2 and argv[0] == "sample":
        sample(argv[1])
    elif len(argv) == 8 and argv[0] == "measure":
        print(json.dumps(measure(*argv[1:])))
    elif len(argv) == 2 and argv[0] == "table":
        print(table(argv[1]))
    else:
        print(__doc__, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
