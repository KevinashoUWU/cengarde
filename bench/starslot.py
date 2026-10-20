#!/usr/bin/env python3
"""A Starlink link in time for the bonding lab (bench/lab.d/bond.sh): its 15 s
slots, as measured on the field router "nostalgica" (2026-10-19, story 015).

  starslot.py plan --secs S --seed N [model options]     the schedule, JSON lines
  starslot.py run  --ns-cli NS --ns-srv NS --link N ...  apply it with tc (root)

Starlink reconfigures its links every 15 s, at :12, :27, :42 and :57 of each
minute (UTC). What the field showed (1.7 h of idle probes at 10 Hz, 477
slots, and the dish's own history), and what this models:

- each slot has its own base delay: the floor of its RTT is drawn anew
  (p5 16.1, p50 20.9, p95 26.1 ms, lag-1 autocorrelation 0.22: nearly
  independent), so the step at a switch goes to an intermediate value, not
  to the lowest one (|step| p50 2.3, p90 6.5 ms of RTT): LEVEL_MS, one-way;
- within a slot the delay drifts little and either way (slope p10 -0.23,
  p50 0, p90 +0.22 ms/s of RTT; over the slot p50 1 ms, p90 4.6 ms):
  SLOPE_MS_S, one-way;
- at every switch the link stops for ~45 ms: what reaches it in the 25 ms
  before is lost, what comes in the last 20 ms waits ~55 ms (CUT_LOSS_MS,
  CUT_HOLD_MS, HOLD_MS);
- outages (the dish's history, 7.6 h): 1.45 an hour, 0.24 to 0.90 s
  (NO_DOWNLINK at a switch, OBSTRUCTED, NO_PINGS): OUTAGE_PER_SLOT, OUTAGE_S.

Not measured (the field data were idle): the rate per slot. The case gives a
list of rates per direction (--up, --down, Mbit/s), one drawn per slot with
the same index for both (starsat's 45, 60 and 75 of old).

--model ramp keeps the lab's old guess instead (a rise of 10 to 25 ms within
each slot, starts at 20, 27 or 34 ms; starlink15ramp before 2026-10-19), for
comparison. The draw comes from --seed (bond.sh: BOND_SEED * 1000 + the
repetition, as ltespikerand), the same for every arm at the same repetition.
"""

import argparse
import json
import random
import subprocess
import sys
import time

sys.dont_write_bytecode = True

SLOT_S = 15.0
STEP_S = 0.5  # how often the delay moves within a slot
# One-way, ms, relative to the median slot: the 16 quantiles (1/32, 3/32, ...)
# of the slots' RTT floor over 2, nostalgica 2026-10-19 (477 slots).
LEVEL_MS = [-2.5, -2.0, -1.3, -0.9, -0.6, -0.4, -0.3, -0.1, 0.1, 0.2, 0.5, 0.6, 0.9, 1.2, 1.9, 2.9]
# One-way, ms per s: the 16 quantiles of the slots' drift over 2.
SLOPE_MS_S = [-0.20, -0.12, -0.08, -0.04, -0.03, -0.01, -0.01, 0.0, 0.0, 0.01, 0.02, 0.03, 0.05, 0.08, 0.12, 0.21]
# The switch: probes sent 45..20 ms before it were lost (8 of ~100 came
# back), those sent in the last 20 ms took 56 ms more (p90 70) on the way up.
CUT_LOSS_MS = 25
CUT_HOLD_MS = 20
HOLD_MS = 55
# The dish's outages after boot over 7.6 h (2026-10-18/19): 11, so 1.45 an
# hour, 0.006 per slot; their lengths.
OUTAGE_PER_SLOT = 1.45 / 240
OUTAGE_S = [0.24, 0.46, 0.48, 0.50, 0.68, 0.72, 0.72, 0.78, 0.80, 0.84, 0.90]


def plan(secs, seed, delay, up, down, model="field", cut_ms=None, outages=True):
    """The schedule: a list of events {t, delay, up, down, loss} (s, ms, Mbit/s,
    Mbit/s, %), each holding until the next one. delay is the link's base one-way
    delay (its median slot); up and down the rates to draw from, per slot."""
    rng = random.Random(seed)
    ev = []
    cuts = []  # spans of total loss, outages and switches: (from, to)
    nslots = int(secs // SLOT_S) + 1
    for s in range(nslots):
        t0 = s * SLOT_S
        k = rng.randrange(len(up))
        ru, rd = up[k], down[k]
        if model == "ramp":
            lev = delay if s == 0 else delay + rng.choice([0, 7, 14])
            rise = 10 + rng.randrange(16)
            slope = rise / (SLOT_S - STEP_S)
            mid = 0.0
        else:
            lev = delay + rng.choice(LEVEL_MS)
            slope = rng.choice(SLOPE_MS_S)
            mid = SLOT_S / 2
        n = int(SLOT_S / STEP_S)
        for i in range(n):
            t = t0 + i * STEP_S
            d = lev + slope * ((i * STEP_S) - mid)
            ev.append({"t": round(t, 3), "delay": round(max(d, 0.1), 1), "up": ru, "down": rd, "loss": 0})
        if model != "ramp" and outages and rng.random() < OUTAGE_PER_SLOT:
            at = t0 + 1 + rng.random() * (SLOT_S - 3)
            dur = rng.choice(OUTAGE_S)
            cuts.append((round(at, 3), round(at + dur, 3)))
            ev.append({"t": round(at, 3), "delay": None, "up": ru, "down": rd, "loss": 100})
            ev.append({"t": round(at + dur, 3), "delay": None, "up": ru, "down": rd, "loss": 0})
        # the switch, at the end of the slot
        tend = t0 + SLOT_S
        if cut_ms is not None:
            cuts.append((round(tend - cut_ms / 1000, 3), round(tend, 3)))
            ev.append({"t": round(tend - cut_ms / 1000, 3), "delay": None, "up": ru, "down": rd, "loss": 100})
        else:
            ev.append({"t": round(tend - (CUT_LOSS_MS + CUT_HOLD_MS) / 1000, 3), "delay": None, "up": ru, "down": rd,
                       "loss": 100})
            ev.append({"t": round(tend - CUT_HOLD_MS / 1000, 3), "delay": "+%d" % HOLD_MS, "up": ru, "down": rd,
                       "loss": 0})
    ev.sort(key=lambda e: e["t"])
    # a delay of None holds the last numeric one; "+N" adds N to it; a step of
    # the drift inside an outage keeps its loss
    last = None
    out = []
    for e in ev:
        e = dict(e)
        if e["t"] >= secs:
            break
        if e["delay"] is None:
            e["delay"] = last
        elif isinstance(e["delay"], str):
            e["delay"] = round(last + float(e["delay"]), 1)
        else:
            last = e["delay"]
        if any(a <= e["t"] < b for a, b in cuts):
            e["loss"] = 100
        out.append(e)
    return out


def limit(rate, queue_ms, delay_ms, inflight):
    """netem's limit in packets, as bond.sh's bond_link counts it."""
    lim = int(rate * 1000000 * queue_ms / 8000 / 1400) + 10
    if inflight:
        lim += int(rate * 1000000 * int(delay_ms) / 8000 / 1400)
    return lim


def tc_args(ns, dev, delay, rate, queue_ms, loss, inflight):
    return ["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev, "root", "netem", "delay",
            "%.1fms" % delay, "rate", "%smbit" % rate, "limit", str(limit(rate, queue_ms, delay, inflight)),
            "loss", "%d%%" % loss]


def run(a, ev):
    t0 = time.monotonic()
    prev = None
    for e in ev:
        wait = t0 + e["t"] - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        key = (e["delay"], e["up"], e["down"], e["loss"])
        if key == prev:
            continue
        prev = key
        for ns, dev, rate in ((a.ns_cli, "l%d" % a.link, e["up"]), (a.ns_srv, "s%d" % a.link, e["down"])):
            subprocess.run(tc_args(ns, dev, e["delay"], rate, a.queue, e["loss"], a.inflight),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return 0


def rates(s):
    return [float(x) if "." in x else int(x) for x in s.replace(",", " ").split()]


def main(argv=None):
    p = argparse.ArgumentParser(description="A Starlink link in time for bond.sh (see the docstring).")
    p.add_argument("cmd", choices=("plan", "run"))
    p.add_argument("--secs", type=float, required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--delay", type=float, default=20, help="one-way ms of the median slot")
    p.add_argument("--up", default="60", help="rates to draw from per slot, Mbit/s, the client's side")
    p.add_argument("--down", default=None, help="the same for the server's side (default: --up)")
    p.add_argument("--queue", type=int, default=15, help="netem queue, ms")
    p.add_argument("--model", choices=("field", "ramp"), default="field")
    p.add_argument("--cut-ms", type=int, default=None, help="a total cut of this at each switch instead of the field's")
    p.add_argument("--no-outages", action="store_true")
    p.add_argument("--inflight", type=int, default=0, help="BOND_NETEM_INFLIGHT")
    p.add_argument("--ns-cli"), p.add_argument("--ns-srv"), p.add_argument("--link", type=int, default=1)
    a = p.parse_args(argv)
    up = rates(a.up)
    down = rates(a.down) if a.down else up
    if len(up) != len(down):
        sys.exit("starslot: --up and --down need as many rates")
    ev = plan(a.secs, a.seed, a.delay, up, down, a.model, a.cut_ms, not a.no_outages)
    if a.cmd == "plan":
        for e in ev:
            print(json.dumps(e))
        return 0
    if not a.ns_cli or not a.ns_srv:
        sys.exit("starslot: run needs --ns-cli and --ns-srv")
    return run(a, ev)


if __name__ == "__main__":
    sys.exit(main())
