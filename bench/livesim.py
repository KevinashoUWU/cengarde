#!/usr/bin/env python3
"""livesim: a live-video-like TCP stream (RTMP from OBS) for the bonding lab.

Story 024 (PEP mode A): what an RTMP push sees. The sender makes records at a
constant bitrate, as an encoder makes frames, keeps the ones the socket does
not take in its own queue, and drops a record that waited there longer than
--drop-ms (OBS's "dropped frames" threshold is 700 ms by default); the
receiver timestamps every record as it completes. Both ends run on the same
machine (the lab's network namespaces), so the CLOCK_MONOTONIC the sender
writes into each record is the receiver's too: the delay of a record is its
arrival minus its making, the whole path (the sender's queue, its socket
buffer, any proxy's buffers, the network).

    python3 -I bench/livesim.py recv --bind ADDR:PORT --secs S [--out FILE]
    python3 -I bench/livesim.py send --to ADDR:PORT --secs S [--mbit 8]
                                     [--drop-ms 700] [--out FILE]
    python3 -I bench/livesim.py merge PREFIX

merge prints {"far": ..., "near": ...} from PREFIX-far-recv.json and
PREFIX-far-send.json (and the same for near, when there): each the receiver's
object with the sender's under "send" and the record size under "size".

A record is --size bytes (1250: 800 a second at 8 Mbit/s), made in batches every
10 ms; its first 24 bytes are the sequence number, the time it was made and
the time the stream started (nanoseconds, network order).

recv prints (or writes to --out) one JSON object, over the window from the
stream's start to S seconds later (the receiver keeps reading S + 5 s at most):

    mbps        the goodput over the window (record bytes, by arrival)
    sec_mbps    each second's goodput, [1, S) (the first second is the ramp)
    sec_p5, sec_p10, sec_p50, sec_min   their percentiles
    stalls      gaps between two arrivals longer than --stall-ms (500), and
                stall_s their total, max_gap_ms the longest (inside the window,
                the end of the window counting as an arrival)
    delay_p50, delay_p95, delay_p99, delay_max   ms, every record received
    delivered   unique records received; due_pct the share received of those
                made before S - 3 s (the last 3 s may still be on the way)

send prints its own: records made, sent (whole, into the socket), dropped
(--drop-ms), max_queue_ms (the oldest record waiting, at worst), and from the
socket's TCP_INFO the retransmissions and the smoothed RTT at the end.
"""

import argparse
import json
import select
import socket
import struct
import sys
import time
from collections import deque

HDR = struct.Struct("!QQQ")
TCP_INFO_FMT = "B" * 8 + "I" * 24  # tcpi_state .. tcpi_total_retrans (linux/tcp.h)


def addr(s):
    h, _, p = s.rpartition(":")
    return h, int(p)


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def tcp_info(sock):
    try:
        raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, struct.calcsize(TCP_INFO_FMT))
        v = struct.unpack(TCP_INFO_FMT, raw[:struct.calcsize(TCP_INFO_FMT)])
        return {"total_retrans": v[8 + 23], "srtt_ms": round(v[8 + 15] / 1000, 1), "cwnd": v[8 + 18]}
    except (OSError, struct.error):
        return {}


def send(a):
    rps = a.mbit * 1e6 / 8 / a.size
    pad = bytes(a.size - HDR.size)
    s = socket.create_connection(addr(a.to), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.setblocking(False)
    t0 = time.monotonic_ns()
    q = deque()  # (gen_ns, record bytes) waiting, whole
    head = b""   # the record being written, its unsent rest
    made = sent = dropped = 0
    max_q = 0.0
    end = t0 + int(a.secs * 1e9)
    drop_ns = int(a.drop_ms * 1e6)
    while True:
        now = time.monotonic_ns()
        if now >= end:
            break
        due = int((now - t0) / 1e9 * rps) + 1
        while made < due:
            q.append((now, HDR.pack(made, now, t0) + pad))
            made += 1
        # the encoder's queue: whole records older than the threshold go
        while q and now - q[0][0] > drop_ns:
            q.popleft()
            dropped += 1
        if q:
            max_q = max(max_q, (now - q[0][0]) / 1e6)
        while True:
            if not head:
                if not q:
                    break
                head = q.popleft()[1]
            try:
                n = s.send(head)
            except BlockingIOError:
                break
            except OSError:
                head, q = b"", deque()
                break
            head = head[n:]
            if not head:
                sent += 1
        wait = min(0.01, max(0.0, (end - time.monotonic_ns()) / 1e9))
        if head or q:
            select.select([], [s], [], wait)
        else:
            time.sleep(wait)
    info = tcp_info(s)
    s.close()
    out = {"made": made, "sent": sent, "dropped": dropped, "drop_pct": round(100.0 * dropped / max(made, 1), 3),
           "max_queue_ms": round(max_q, 1), "mbit": a.mbit, "size": a.size, **info}
    return out


def recv(a):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(addr(a.bind))
    ls.listen(1)
    ls.settimeout(a.secs + 30)
    c, _ = ls.accept()
    c.settimeout(0.2)
    buf = bytearray()
    arr = []  # (arrival_ns, seq, gen_ns)
    t0 = None
    stop = None
    seen = set()
    while True:
        now = time.monotonic_ns()
        if stop is not None and now > stop:
            break
        try:
            d = c.recv(262144)
        except socket.timeout:
            continue
        except OSError:
            break
        if not d:
            break
        now = time.monotonic_ns()
        buf += d
        n = len(buf) // a.size
        for i in range(n):
            seq, gen, st = HDR.unpack_from(buf, i * a.size)
            if t0 is None:
                t0, stop = st, st + int((a.secs + 5) * 1e9)
            if seq not in seen:
                seen.add(seq)
                arr.append((now, seq, gen))
        del buf[:n * a.size]
    c.close()
    ls.close()
    if t0 is None:
        return {"delivered": 0, "mbps": 0.0}
    secs = int(a.secs)
    end = t0 + secs * 10**9
    bins = [0] * secs
    for t, _, _ in arr:
        k = (t - t0) // 10**9
        if 0 <= k < secs:
            bins[k] += a.size
    sec = [round(b * 8 / 1e6, 3) for b in bins]
    steady = sec[1:]
    times = [t for t, _, _ in arr if t <= end]
    gaps = []
    prev = t0
    for t in times + [end]:
        gaps.append((t - prev) / 1e6)
        prev = t
    stall = [g for g in gaps if g > a.stall_ms]
    delays = [(t - g) / 1e6 for t, _, g in arr]
    rps = a.mbit * 1e6 / 8 / a.size
    due_n = int(max(0, a.secs - 3) * rps)
    got_due = sum(1 for _, s, _ in arr if s < due_n)
    r = lambda x: None if x is None else round(x, 1)
    return {"mbps": round(sum(bins) * 8 / 1e6 / secs, 3), "sec_mbps": steady,
            "sec_p5": pct(steady, 0.05), "sec_p10": pct(steady, 0.10), "sec_p50": pct(steady, 0.5),
            "sec_min": min(steady) if steady else None,
            "stalls": len(stall), "stall_s": round(sum(stall) / 1000, 3), "max_gap_ms": r(max(gaps)),
            "delay_p50": r(pct(delays, 0.5)), "delay_p95": r(pct(delays, 0.95)), "delay_p99": r(pct(delays, 0.99)),
            "delay_max": r(max(delays) if delays else None), "delivered": len(arr),
            "due_pct": round(100.0 * got_due / due_n, 3) if due_n else None}


def merge(prefix):
    out = {}
    for k in ("far", "near"):
        try:
            r = json.load(open("%s-%s-recv.json" % (prefix, k)))
        except (OSError, ValueError):
            continue
        try:
            s = json.load(open("%s-%s-send.json" % (prefix, k)))
        except (OSError, ValueError):
            s = {}
        r["send"], r["size"] = s, s.get("size", 1250)
        out[k] = r
    return out


def main(argv):
    if argv[:1] == ["merge"] and len(argv) == 2:
        print(json.dumps(merge(argv[1])))
        return 0
    ap = argparse.ArgumentParser(prog="livesim.py")
    sp = ap.add_subparsers(dest="cmd", required=True)
    for name in ("send", "recv"):
        p = sp.add_parser(name)
        p.add_argument("--secs", type=float, required=True)
        p.add_argument("--mbit", type=float, default=8.0)
        p.add_argument("--size", type=int, default=1250)
        p.add_argument("--out")
        if name == "send":
            p.add_argument("--to", required=True)
            p.add_argument("--drop-ms", type=float, default=700.0)
        else:
            p.add_argument("--bind", required=True)
            p.add_argument("--stall-ms", type=float, default=500.0)
    a = ap.parse_args(argv)
    if a.size < HDR.size:
        sys.exit("livesim: --size at least %d" % HDR.size)
    out = send(a) if a.cmd == "send" else recv(a)
    text = json.dumps(out)
    if a.out:
        with open(a.out, "w") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
