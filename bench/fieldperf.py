#!/usr/bin/env python3
"""The tables of a bench/fieldperf.sh run (story 012): what each uplink
carried alone and all at once outside the tunnel, and the tunnel on top of
them, with the ping each link had under that load and where the router
dropped what it could not send.

    python3 bench/fieldperf.py DIR [--csv FILE] [--json FILE]

DIR is the directory fieldperf.sh wrote (or the one its .tgz unpacks to).
Prints Markdown; --csv writes one row per test and link (link "*": the
test's whole), for R or pandas; --json the same rows as a list.

Each column, per test and link:
- Mbit/s: the receiver's goodput (iperf3's sum_received), the sum of the
  links on a test's "*" row;
- retr.: TCP retransmissions (the sender's; on a download, the VPS's);
- TCP RTT: the sender's smoothed RTT, mean and max over the streams (only
  on uploads: on a download the sender is the VPS);
- ping: p50/p95/max under the load, and how many were lost; "idle" is the
  p50 of the 5 pings before any test;
- engine out: what the engine sent on the link (its tx_bytes), in Mbit/s:
  on a tunnel test, how the tunnel's load fell on each link;
- sock. drops: the engine's tx_drops on the link (sendmmsg refused with
  EAGAIN or ENOBUFS: the socket's buffer or the device's queue was full);
- dev drops: the device's tx_dropped (and its lower device's, a VLAN's
  parent, after the slash);
- qdisc drops: the root queue's drops (tc, when it was installed);
- UDP sndbuf: SndbufErrors of the whole router (IPv4 and IPv6), on the "*"
  row only.

The raw files hold the addresses of the VPS and of the modems: they stay
with whoever ran the test, and so does this output.

SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import csv
import glob
import json
import os
import re
import sys

PING_TIME = re.compile(r"time[=<]([0-9.]+) ?ms")
PING_SUM = re.compile(r"(\d+) packets transmitted, (\d+) (?:packets )?received")
TC_SENT = re.compile(r"Sent \d+ bytes \d+ pkt \(dropped (\d+),")


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def kv(text):
    """key=value words, as fieldperf.sh writes meta and plan; "link=" lines
    are the links of a test, in order."""
    out, links = {}, []
    for line in (text or "").splitlines():
        words = dict(w.split("=", 1) for w in line.split() if "=" in w)
        if "link" in words:
            links.append(words)
        elif "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out, links


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else None


def ping(path):
    """{p50, p95, max, sent, lost} of a ping's output, or None without one."""
    text = read(path)
    if text is None:
        return None
    times = [float(t) for t in PING_TIME.findall(text)]
    m = PING_SUM.search(text)
    sent, got = (int(m.group(1)), int(m.group(2))) if m else (None, len(times))
    return {
        "p50": pct(times, 0.5),
        "p95": pct(times, 0.95),
        "max": max(times) if times else None,
        "sent": sent,
        "lost": sent - got if sent is not None else None,
    }


def iperf(path):
    """{mbps, retr, rtt_mean, rtt_max, error} of an iperf3 -J output."""
    text = read(path)
    if text is None:
        return None
    try:
        j = json.loads(text)
    except ValueError:
        err = (read(path[: -len(".json")] + ".err") or "").strip().splitlines()
        return {"error": err[0] if err else "no JSON"}
    end = j.get("end") or {}
    recv, sent = end.get("sum_received") or {}, end.get("sum_sent") or {}
    rtts = [s.get("sender", {}) for s in end.get("streams", [])]
    means = [r["mean_rtt"] for r in rtts if r.get("mean_rtt")]
    maxes = [r["max_rtt"] for r in rtts if r.get("max_rtt")]
    out = {
        "mbps": recv["bits_per_second"] / 1e6 if "bits_per_second" in recv else None,
        "retr": sent.get("retransmits"),
        "rtt_mean": sum(means) / len(means) / 1000 if means else None,
        "rtt_max": max(maxes) / 1000 if maxes else None,
        "error": j.get("error"),
    }
    return out


def snmp(dirname):
    """SndbufErrors of UDP over IPv4 and IPv6, or None."""
    total, seen = 0, False
    text = read(os.path.join(dirname, "snmp")) or ""
    lines = [line.split() for line in text.splitlines()]
    for a, b in zip(lines, lines[1:]):
        if a and b and a[0] == "Udp:" and b[0] == "Udp:" and not b[1].isalpha() and "SndbufErrors" in a:
            total += int(b[a.index("SndbufErrors")])
            seen = True
    for line in (read(os.path.join(dirname, "snmp6")) or "").splitlines():
        w = line.split()
        if len(w) == 2 and w[0] == "Udp6SndbufErrors":
            total += int(w[1])
            seen = True
    return total if seen else None


def stats(dirname, dev):
    out = {}
    for line in (read(os.path.join(dirname, f"stats-{dev}.txt")) or "").splitlines():
        w = line.split()
        if len(w) == 2 and w[1].isdigit():
            out[w[0]] = int(w[1])
    return out


def qdisc(dirname, dev):
    """The root qdisc's drops (the first one tc lists), or None."""
    text = read(os.path.join(dirname, f"tc-{dev}.txt"))
    m = TC_SENT.search(text or "")
    return int(m.group(1)) if m else None


def engine(dirname):
    """The engine's links by device: tx_bytes and tx_drops."""
    try:
        with open(os.path.join(dirname, "status.json"), encoding="utf-8") as f:
            st = json.load(f)
    except (OSError, ValueError):
        return {}
    return {l.get("name"): l for l in st.get("links", []) if l.get("name")}


def delta(a, b):
    return b - a if a is not None and b is not None else None


def analyse(root):
    plan, _ = kv(read(os.path.join(root, "plan")))
    links = []
    for line in (read(os.path.join(root, "links")) or "").splitlines():
        w = line.split()
        if len(w) == 3:
            links.append({"dev": w[0], "label": w[1], "host": w[2]})
    idle = {l["dev"]: ping(os.path.join(root, "idle", f"ping-{l['dev']}.txt")) for l in links}
    idle["tunnel"] = ping(os.path.join(root, "idle", "ping-tunnel.txt"))
    # The lower devices: what fieldperf.sh counted that is not a link (the
    # VLANs' parent, the trunk). Shared by the links, so only on a test's
    # whole (and on a solo test's one row).
    devs = plan.get("devs", "").split()
    link_devs = {l["dev"] for l in links}
    lowers = [d for d in devs if d not in link_devs]
    rows = []
    for tdir in sorted(glob.glob(os.path.join(root, "[0-9][0-9]-*"))):
        meta, tlinks = kv(read(os.path.join(tdir, "meta")))
        before, after = os.path.join(tdir, "before"), os.path.join(tdir, "after")
        try:
            secs = max(1, int(meta.get("end", 0)) - int(meta.get("start", 0)))
        except ValueError:
            secs = None
        eb, ea = engine(before), engine(after)
        test = os.path.basename(tdir)
        mode, way = meta.get("mode", "?"), meta.get("dir", "?")
        whole = {
            "test": test, "mode": mode, "dir": way, "link": "*", "dev": "",
            "mbps": None, "retr": None, "rtt_mean": None, "rtt_max": None,
            "ping": None, "idle_p50": None, "engine_mbps": None, "sock_drops": None,
            "dev_drops": None, "lower_drops": None, "qdisc_drops": None,
            "sndbuf": delta(snmp(before), snmp(after)), "error": None, "failed": 0,
        }
        per = []
        for tl in tlinks:
            dev = tl.get("dev", "")
            ip = iperf(os.path.join(tdir, f"iperf-{dev}.json")) if mode != "tunnel" else None
            sb, sa = stats(before, dev), stats(after, dev)
            e0, e1 = eb.get(dev, {}), ea.get(dev, {})
            ebytes = delta(e0.get("tx_bytes"), e1.get("tx_bytes"))
            r = dict(whole)
            r.update({
                "link": tl.get("link", dev), "dev": dev,
                "mbps": ip.get("mbps") if ip else None,
                "retr": ip.get("retr") if ip else None,
                "rtt_mean": ip.get("rtt_mean") if ip else None,
                "rtt_max": ip.get("rtt_max") if ip else None,
                "error": ip.get("error") if ip else None,
                "ping": ping(os.path.join(tdir, f"ping-{dev}.txt")),
                "idle_p50": (idle.get(dev) or {}).get("p50"),
                "engine_mbps": ebytes * 8 / secs / 1e6 if ebytes is not None and secs else None,
                "sock_drops": delta(e0.get("tx_drops"), e1.get("tx_drops")),
                "dev_drops": delta(sb.get("tx_dropped"), sa.get("tx_dropped")),
                "lower_drops": None,
                "qdisc_drops": delta(qdisc(before, dev), qdisc(after, dev)),
                "sndbuf": None,
            })
            per.append(r)
        if mode == "tunnel":
            ip = iperf(os.path.join(tdir, "iperf-tunnel.json")) or {}
            whole.update({k: ip.get(k) for k in ("mbps", "retr", "rtt_mean", "rtt_max", "error")})
            whole["link"] = "tunnel"
            whole["ping"] = ping(os.path.join(tdir, "ping-tunnel.txt"))
            whole["idle_p50"] = (idle.get("tunnel") or {}).get("p50")
        else:
            got = [r["mbps"] for r in per if r["mbps"] is not None]
            whole["mbps"] = sum(got) if got else None
            whole["failed"] = sum(1 for r in per if r["error"])
        for k in ("sock_drops", "dev_drops", "qdisc_drops", "engine_mbps"):
            got = [r[k] for r in per if r[k] is not None]
            whole[k] = sum(got) if got else None
        got = [delta(stats(before, lo).get("tx_dropped"), stats(after, lo).get("tx_dropped")) for lo in lowers]
        got = [g for g in got if g is not None]
        whole["lower_drops"] = sum(got) if got else None
        if mode == "solo" and len(per) == 1:
            per[0]["sndbuf"] = whole["sndbuf"]
            per[0]["lower_drops"] = whole["lower_drops"]
            rows.append(per[0])
        else:
            rows.append(whole)
            rows.extend(per)
    return {"plan": plan, "links": links, "idle": idle, "rows": rows}


def f1(v):
    return "" if v is None else f"{v:.1f}"


def n0(v):
    return "" if v is None else str(v)


def ms(v):
    return f"{v:.0f}" if v >= 10 else f"{v:.1f}"


def ping_cell(p):
    if not p or p["p50"] is None:
        return "" if not p else f"all lost ({p['sent']})"
    lost = f", {p['lost']} lost" if p.get("lost") else ""
    return f"{ms(p['p50'])}/{ms(p['p95'])}/{ms(p['max'])}{lost}"


def markdown(a, out):
    w = out.write
    w("# Field throughput (bench/fieldperf.sh)\n\n")
    p = a["plan"]
    w(f"{p.get('secs', '?')} s per test, {p.get('streams', '?')} TCP streams; tunnel to {p.get('tunnel', '?')}; "
      f"engine {p.get('version') or '?'}.\n\n")
    w("## Links, idle\n\n| link | device | ping idle p50/p95/max ms |\n| --- | --- | --- |\n")
    for l in a["links"]:
        w(f"| {l['label']} | {l['dev']} | {ping_cell(a['idle'].get(l['dev']))} |\n")
    if a["idle"].get("tunnel"):
        w(f"| tunnel | | {ping_cell(a['idle']['tunnel'])} |\n")
    w("\n## Tests\n\n")
    w("| test | link | Mbit/s | retr. | TCP RTT avg/max ms | ping p50/p95/max ms | idle p50 | engine out Mbit/s "
      "| sock. drops | dev drops | qdisc drops | UDP sndbuf |\n")
    w("| --- | --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for r in a["rows"]:
        rtt = f"{f1(r['rtt_mean'])}/{f1(r['rtt_max'])}" if r["rtt_mean"] is not None else ""
        dev_drops = n0(r["dev_drops"])
        if r["lower_drops"] is not None:
            dev_drops = f"{dev_drops}/{r['lower_drops']}"
        name = f"{r['mode']} {r['dir']}" if r["link"] in ("*", "tunnel") or r["mode"] == "solo" else ""
        mbps = f1(r["mbps"]) if not r["error"] else f"error: {r['error']}"
        if r.get("failed"):
            mbps = f"{mbps} ({r['failed']} failed)"
        w(f"| {name} | {r['link']} | {mbps} | {n0(r['retr'])} | {rtt} | {ping_cell(r['ping'])} | {f1(r['idle_p50'])} "
          f"| {f1(r['engine_mbps'])} | {n0(r['sock_drops'])} | {dev_drops} | {n0(r['qdisc_drops'])} | {n0(r['sndbuf'])} |\n")
    w("\n")


FIELDS = ["test", "mode", "dir", "link", "dev", "mbps", "failed", "retr", "rtt_mean", "rtt_max", "ping_p50", "ping_p95",
          "ping_max", "ping_lost", "idle_p50", "engine_mbps", "sock_drops", "dev_drops", "lower_drops", "qdisc_drops",
          "sndbuf", "error"]


def flat(r):
    p = r["ping"] or {}
    out = {k: r.get(k) for k in FIELDS if k in r}
    out.update({"ping_p50": p.get("p50"), "ping_p95": p.get("p95"), "ping_max": p.get("max"),
                "ping_lost": p.get("lost")})
    return {k: out.get(k) for k in FIELDS}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--csv")
    ap.add_argument("--json")
    args = ap.parse_args(argv)
    if not os.path.isdir(args.dir):
        sys.exit(f"fieldperf: no directory {args.dir}")
    a = analyse(args.dir)
    if not a["rows"]:
        sys.exit(f"fieldperf: no test in {args.dir}")
    markdown(a, sys.stdout)
    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            wr = csv.DictWriter(f, FIELDS)
            wr.writeheader()
            for r in a["rows"]:
                wr.writerow(flat(r))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump([flat(r) for r in a["rows"]], f, indent=1)


if __name__ == "__main__":
    main()
