#!/usr/bin/env python3
"""flvrx: the receiving end of an FLV stream over TCP (what RTMP carries,
without its handshake), for the lab's BOND_FLV (story 031, the burst
credit): it reads the tags as they come and says how late each one arrived.

The sender is ffmpeg -re: it writes each frame when its time comes, so a
tag's timestamp is when it was written, give or take a constant. A tag's
lateness is (its arrival less its timestamp) less the 5th percentile of
that over the stream: what the path and its queues added on top of the
fast tags. The first 3 s are left out (the encoder's start writes faster
than real time). A keyframe is many times a frame's size and leaves in a burst: its
lateness is what a pacer that holds bursts adds.

  flvrx.py --listen ADDR:PORT --secs N > out.json    (the far end, upload)
  flvrx.py --connect ADDR:PORT --secs N > out.json   (the LAN, download: the
                                                      sender listens)

SPDX-License-Identifier: GPL-3.0-or-later"""
import argparse
import json
import socket
import sys
import time


def pct(v, q):
    if not v:
        return None
    v = sorted(v)
    return round(v[min(len(v) - 1, int(q * len(v)))], 1)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--listen")
    g.add_argument("--connect")
    ap.add_argument("--secs", type=float, default=30)
    a = ap.parse_args()
    if a.listen:
        h, p = a.listen.rsplit(":", 1)
        ls = socket.socket()
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((h, int(p)))
        ls.listen(1)
        ls.settimeout(a.secs + 20)
        s, _ = ls.accept()
    else:
        h, p = a.connect.rsplit(":", 1)
        s = None
        for _ in range(100):  # the sender may not listen yet
            try:
                s = socket.create_connection((h, int(p)), timeout=5)
                break
            except OSError:
                time.sleep(0.1)
        if s is None:
            print(json.dumps({"error": "no connection"}))
            return 1
    s.settimeout(5)
    buf = bytearray()
    tags = []  # (arrival s, ts ms, size, kind: k keyframe, v video, a audio)
    start = time.monotonic()
    hdr_done = False
    total = 0
    try:
        while time.monotonic() - start < a.secs + 10:
            d = s.recv(262144)
            if not d:
                break
            now = time.monotonic()
            total += len(d)
            buf += d
            if not hdr_done:
                if len(buf) < 13:
                    continue
                del buf[:13]  # the FLV header (9) and the first previous tag size (4)
                hdr_done = True
            while len(buf) >= 11:
                size = buf[1] << 16 | buf[2] << 8 | buf[3]
                if len(buf) < 11 + size + 4:
                    break
                ts = buf[4] << 16 | buf[5] << 8 | buf[6] | buf[7] << 24
                kind = "a"
                if buf[0] == 9:
                    kind = "k" if size and buf[11] >> 4 == 1 else "v"
                elif buf[0] != 8:
                    kind = "m"
                tags.append((now, ts, size, kind))
                del buf[:11 + size + 4]
    except socket.timeout:
        pass
    vid = [t for t in tags if t[3] in "kv" and t[1] >= 3000]
    out = {"bytes": total, "tags": len(tags), "frames": sum(1 for t in tags if t[3] in "kv"),
           "keyframes": sum(1 for t in vid if t[3] == "k")}
    if vid:
        off = pct([t[0] * 1000 - t[1] for t in vid], 0.05)
        late = [(t[0] * 1000 - t[1] - off, t) for t in vid]
        key = [x for x, t in late if t[3] == "k"]
        oth = [x for x, t in late if t[3] == "v"]
        span = vid[-1][0] - vid[0][0]
        out.update({
            "mbps": round(sum(t[2] for t in vid) * 8 / span / 1e6, 2) if span > 0 else None,
            "key_bytes": round(sum(t[2] for t in vid if t[3] == "k") / max(1, out["keyframes"])),
            "key_late_p50_ms": pct(key, 0.5), "key_late_p99_ms": pct(key, 0.99), "key_late_max_ms": pct(key, 1.0),
            "frame_late_p50_ms": pct(oth, 0.5), "frame_late_p99_ms": pct(oth, 0.99),
            "frame_late_max_ms": pct(oth, 1.0),
        })
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
