#!/usr/bin/env python3
"""Watches link health during `bench/lab.sh health` (see there).

usage: health.py up|down RUN_DIR SECONDS BAD_AT GOOD_AT

Link 3 gets a 500 ms queue (tbf at 5 Mbit/s) at BAD_AT seconds and loses it
at GOOD_AT; its state is read from the status JSON of the sending end five
times a second. Passes when the link is muted within 5 s of the queue
appearing, is active at the end, and is not muted again on what it measures
after the queue is gone (a mute decided within SETTLE of GOOD_AT still counts
the queue).
"""
import json
import subprocess
import sys
import time

SETTLE = 3.0  # mute_settle_ms plus the delay filter


def link_state(run, direction):
    try:
        if direction == "up":
            with open(f"{run}/client.json") as f:
                links = json.load(f)["links"]
            prefix = "upload"
        else:
            with open(f"{run}/server.json") as f:
                links = json.load(f)["sessions"][0]["links"]
            prefix = "download"
        link = next(x for x in links if x["id"] == 2)
        return link[prefix], link[prefix + "_mutes"], link[prefix + "_behind_ms"]
    except (OSError, ValueError, LookupError, StopIteration):
        return None


def tc(ns, *args):
    subprocess.run(["ip", "netns", "exec", ns, "tc", "qdisc", *args], check=True)


def main():
    direction, run = sys.argv[1], sys.argv[2]
    secs, bad_at, good_at = (float(a) for a in sys.argv[3:6])
    ns, dev = ("cli", "l3") if direction == "up" else ("srv", "s3")
    samples, shaped, recovered = [], False, False
    t0 = time.monotonic()
    while (t := time.monotonic() - t0) < secs:
        if not shaped and t >= bad_at:
            tc(ns, "replace", "dev", dev, "root", "tbf", "rate", "5mbit", "burst", "32kb", "latency", "500ms")
            shaped = True
        if not recovered and t >= good_at:
            tc(ns, "del", "dev", dev, "root")
            recovered = True
        s = link_state(run, direction)
        if s:
            samples.append((t, *s))
        time.sleep(0.2)

    muted_at = next((t for t, st, _, _ in samples if t >= bad_at and st == "muted"), None)
    behind = [b for t, _, _, b in samples if bad_at <= t < good_at and b is not None]
    mutes_bad = max((m for t, _, m, _ in samples if t < good_at + SETTLE), default=0)
    mutes_after = samples[-1][2] - mutes_bad if samples else 0
    last_muted = max((t for t, st, _, _ in samples if st == "muted"), default=None)
    final = samples[-1][1] if samples else "unknown"
    out = [f"{direction}: queue from {bad_at:.0f} to {good_at:.0f} s"]
    out.append(f"muted {muted_at - bad_at:.1f} s after it formed" if muted_at else "never muted")
    if behind:
        out.append(f"up to {max(behind):.0f} ms behind")
    out.append(f"{mutes_bad} mutes for it")
    if last_muted is not None and last_muted >= good_at:
        out.append(f"active for good {last_muted + 0.2 - good_at:.1f} s after it went")
    out.append(f"mutes after it: {mutes_after}")
    out.append(f"final state {final}")
    print(", ".join(out))
    ok = muted_at is not None and muted_at - bad_at <= 5 and final == "active" and mutes_after == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
