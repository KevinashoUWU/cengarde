#!/usr/bin/env python3
"""cbr.py: a constant-rate UDP stream with sequence numbers and send times,
like the station's upload (story 020): what is lost end to end, when, and
how late it arrives.

  cbr.py tx ADDR:PORT MBIT SECS [SIZE]   send (SIZE bytes, 1200 by default)
  cbr.py rx ADDR:PORT SECS OUT.json      receive for SECS seconds (plus a
                                         grace of 3 s) and write a summary

Each datagram carries its sequence and CLOCK_MONOTONIC send time, shared by
every network namespace of the host: the receiver measures the one-way
delay. The summary: sent (the highest sequence + 1), unique and duplicate
packets, the lost ones per second of the send time ("lost_s": a list, one
count per second from the first packet) and the one-way delay's p50, p99
and maximum in ms ("owd"), and per second ("owd_s": the p99 of each
second), and "late": the packets that arrived after a later one (rebuilt
from a parity or a repair, or a copy that came first on a slower link;
story 021): how many, and how much later than the median one-way delay
they came (p50, p95, max, ms). The sender stops at SECS; the receiver
should start first."""
import json
import socket
import struct
import sys
import time

HDR = struct.Struct("!QQ")  # sequence, send time (ns)


def addr(s):
    h, p = s.rsplit(":", 1)
    return h, int(p)


def tx(dst, mbit, secs, size=1200):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pps = mbit * 1e6 / 8 / size
    gap = 1.0 / pps
    pad = b"\0" * (size - HDR.size)
    t0 = time.monotonic()
    seq = 0
    while True:
        due = t0 + seq * gap
        now = time.monotonic()
        if now - t0 >= secs:
            break
        if due > now:
            time.sleep(due - now)
        try:
            s.sendto(HDR.pack(seq, time.monotonic_ns()) + pad, dst)
        except OSError:
            pass  # a full socket buffer: that one is lost, as a router's would be
        seq += 1


def pct(xs, q):
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else None


def rx(bind, secs, out):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(bind)
    s.settimeout(0.2)
    end = time.monotonic() + secs + 3
    seen = {}
    dup = 0
    t_first = None
    top = -1
    late = []
    while time.monotonic() < end:
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        now = time.monotonic_ns()
        if len(b) < HDR.size:
            continue
        seq, ts = HDR.unpack_from(b)
        if seq in seen:
            dup += 1
            continue
        seen[seq] = (ts, now)
        if seq < top:
            late.append((ts, (now - ts) / 1e6))
        top = max(top, seq)
        if t_first is None or ts < t_first:
            t_first = ts
    sent = max(seen) + 1 if seen else 0
    owd = sorted((r - t) / 1e6 for t, r in seen.values())
    # the send time of a lost packet: between its neighbours' (interpolated
    # from the first packet's and the rate)
    per = {}
    if seen:
        ts_sorted = sorted((q, t) for q, (t, _) in seen.items())
        (q0, t0), (q1, t1) = ts_sorted[0], ts_sorted[-1]
        ns_per = (t1 - t0) / (q1 - q0) if q1 > q0 else 1e9
        nsec = int((t1 - t_first) / 1e9) + 1
        lost_s = [0] * nsec
        for q in range(sent):
            if q not in seen:
                k = int((t0 + (q - q0) * ns_per - t_first) / 1e9)
                if 0 <= k < nsec:
                    lost_s[k] += 1
        for q, (t, r) in seen.items():
            per.setdefault(int((t - t_first) / 1e9), []).append((r - t) / 1e6)
    else:
        lost_s = []
    owd_s = [round(pct(sorted(per.get(k, [])), .99), 1) if per.get(k) else None for k in range(len(lost_s))]
    res = dict(sent=sent, uniq=len(seen), dup=dup, lost=sent - len(seen),
               lost_pct=round(100.0 * (sent - len(seen)) / sent, 4) if sent else None,
               owd=[round(pct(owd, .5), 1) if owd else None, round(pct(owd, .99), 1) if owd else None,
                    round(owd[-1], 1) if owd else None],
               lost_s=lost_s, owd_s=owd_s)
    if owd:
        med = pct(owd, .5)
        lx = sorted(x - med for _, x in late)
        res["late"] = [len(lx), round(pct(lx, .5), 1) if lx else None, round(pct(lx, .95), 1) if lx else None,
                       round(lx[-1], 1) if lx else None]
        # each late one: its second of the send time and how late (for the
        # caller to split inside and outside an episode)
        res["late_t"] = [[int((t - t_first) / 1e9), round(x - med, 1)] for t, x in late]
    with open(out, "w") as f:
        json.dump(res, f)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("tx", "rx"):
        sys.exit(__doc__)
    if sys.argv[1] == "tx":
        tx(addr(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 1200)
    else:
        rx(addr(sys.argv[2]), float(sys.argv[3]), sys.argv[4])


if __name__ == "__main__":
    main()
