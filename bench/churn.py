#!/usr/bin/env python3
"""churn: many short TCP connections for story 025's soak (the protocontroller): the
streams' table, buffers and memory under churn, next to live-like streams.

    python3 -I bench/churn.py serve --bind ADDR:PORT --secs S
    python3 -I bench/churn.py run --to ADDR:PORT --secs S [--workers 16]
                                     [--max-kb 256] [--abort-pct 5] [--out FILE]

serve accepts connections and, for each, reads a 12-byte header (the bytes the
client will send and the bytes it wants back, network order), the bytes, and
writes the reply, then closes; it runs S + 10 s.

run keeps WORKERS connections busy for S seconds: each connects, asks for a
random size up to MAX_KB each way, checks the reply's length and pattern, and
closes; ABORT_PCT of them reset the connection halfway instead (SO_LINGER 0).
It prints one JSON object: connections done, aborted on purpose, failed (by
error), bytes each way, and the connect and completion times' percentiles.
"""

import argparse
import json
import random
import socket
import struct
import sys
import threading
import time

HDR = struct.Struct("!IQ")


def addr(s):
    h, _, p = s.rpartition(":")
    return h, int(p)


def pattern(n, seed):
    b = bytes((seed + i) & 0xFF for i in range(256))
    return (b * (n // 256 + 1))[:n]


def handle(c):
    try:
        c.settimeout(60)
        h = b""
        while len(h) < HDR.size:
            d = c.recv(HDR.size - len(h))
            if not d:
                return
            h += d
        n_in, n_out = HDR.unpack(h)
        got = 0
        while got < n_in:
            d = c.recv(min(262144, n_in - got))
            if not d:
                return
            got += len(d)
        c.sendall(pattern(n_out, n_out & 0xFF))
        c.shutdown(socket.SHUT_WR)
        while c.recv(65536):
            pass
    except OSError:
        pass
    finally:
        c.close()


def serve(a):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(addr(a.bind))
    ls.listen(512)
    ls.settimeout(1.0)
    end = time.monotonic() + a.secs + 10
    while time.monotonic() < end:
        try:
            c, _ = ls.accept()
        except socket.timeout:
            continue
        threading.Thread(target=handle, args=(c,), daemon=True).start()
    ls.close()
    return 0


def pct(xs, q):
    if not xs:
        return None
    xs = sorted(xs)
    return round(xs[min(len(xs) - 1, int(q * len(xs)))], 1)


def worker(a, st, lock, end, seed):
    rnd = random.Random(seed)
    while time.monotonic() < end:
        n_in = rnd.randint(1, a.max_kb * 1024)
        n_out = rnd.randint(1, a.max_kb * 1024)
        abort = rnd.random() * 100 < a.abort_pct
        t0 = time.monotonic()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(30)
        ok = False
        try:
            s.connect(addr(a.to))
            t1 = time.monotonic()
            s.sendall(HDR.pack(n_in, n_out))
            if abort:
                s.sendall(pattern(n_in // 2, 1))
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                s.close()
                with lock:
                    st["aborted"] += 1
                continue
            s.sendall(pattern(n_in, 1))
            got = bytearray()
            while len(got) < n_out:
                d = s.recv(262144)
                if not d:
                    break
                got += d
            ok = len(got) == n_out and bytes(got[:256]) == pattern(min(256, n_out), n_out & 0xFF)
            t2 = time.monotonic()
            with lock:
                if ok:
                    st["done"] += 1
                    st["bytes_up"] += n_in
                    st["bytes_down"] += n_out
                    st["connect_ms"].append((t1 - t0) * 1000)
                    st["total_ms"].append((t2 - t0) * 1000)
                else:
                    st["short"] += 1
        except OSError as e:
            with lock:
                st["failed"] += 1
                st["errors"][type(e).__name__] = st["errors"].get(type(e).__name__, 0) + 1
        finally:
            try:
                s.close()
            except OSError:
                pass


def run(a):
    st = {"done": 0, "aborted": 0, "failed": 0, "short": 0, "bytes_up": 0, "bytes_down": 0, "connect_ms": [],
          "total_ms": [], "errors": {}}
    lock = threading.Lock()
    end = time.monotonic() + a.secs
    th = [threading.Thread(target=worker, args=(a, st, lock, end, i + 1), daemon=True) for i in range(a.workers)]
    for t in th:
        t.start()
    for t in th:
        t.join(a.secs + 60)
    out = {k: st[k] for k in ("done", "aborted", "failed", "short", "bytes_up", "bytes_down", "errors")}
    for k in ("connect_ms", "total_ms"):
        out[k] = {"p50": pct(st[k], 0.5), "p99": pct(st[k], 0.99), "max": pct(st[k], 1.0)}
    js = json.dumps(out)
    if a.out:
        with open(a.out, "w") as f:
            f.write(js + "\n")
    else:
        print(js)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog="churn.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("serve")
    p.add_argument("--bind", required=True)
    p.add_argument("--secs", type=float, required=True)
    p = sub.add_parser("run")
    p.add_argument("--to", required=True)
    p.add_argument("--secs", type=float, required=True)
    p.add_argument("--workers", type=int, default=16)
    p.add_argument("--max-kb", type=int, default=256)
    p.add_argument("--abort-pct", type=float, default=5.0)
    p.add_argument("--out")
    a = ap.parse_args(argv)
    return serve(a) if a.cmd == "serve" else run(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
