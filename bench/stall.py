#!/usr/bin/env python3
"""Stalls of one link of the bonding lab (bench/lab.d/bond.sh, story 026).

  stall.py plan --secs S --ms T (--every P | --gap LO HI --seed N)    the schedule
  stall.py run  --ns-cli NS --ns-srv NS --link N --secs S --ms T ...  apply it (root)

A stall: link N drops everything for T ms, both ways (netem's loss at 100 %
on the client's lN and the server's sN), then comes back as it was (its
delay, rate and queue: --delay, --rate, --queue, --loss, --inflight, as
bond.sh's bond_link counts netem's limit). What was already in the link's
delay still arrives: a radio that stops taking packets, not one that holds
them (ltespike holds them).

--every P: a stall every P seconds, the first at P. --gap LO HI: a stall
LO to HI seconds after the last one ends, drawn from --seed (bond.sh: BOND_SEED
* 1000 + the repetition, as ltespikerand: the same draw for every arm at the
same repetition).

tc goes through one "tc -force -batch -" per side, fed a line per change:
no fork per change, so a stall of 50 ms lasts 50 ms (a "tc qdisc replace"
per change costs a fork and an exec of ip and tc, ~10 ms). Each stall's
start and end (monotonic, s from the start) go to --log when given.
"""

import argparse
import json
import random
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def plan(secs, ms, every=None, gap=None, seed=1):
    """The stalls' start times (s) within secs."""
    out = []
    if every:
        t = every
        while t + ms / 1000.0 < secs:
            out.append(round(t, 3))
            t += every
        return out
    rng = random.Random(seed)
    t = 0.0
    while True:
        t += rng.uniform(gap[0], gap[1])
        if t + ms / 1000.0 >= secs:
            return out
        out.append(round(t, 3))
        t += ms / 1000.0


def limit(rate, queue_ms, delay_ms, inflight):
    """netem's limit in packets, as bond.sh's bond_link counts it."""
    lim = int(rate * 1000000 * queue_ms / 8000 / 1400) + 10
    if inflight:
        lim += int(rate * 1000000 * int(delay_ms) / 8000 / 1400)
    return lim


def line(dev, a, loss):
    return "qdisc replace dev %s root netem delay %gms rate %gmbit limit %d loss %g%%\n" % (
        dev, a.delay, a.rate, limit(a.rate, a.queue, a.delay, a.inflight), loss)


def run(a, starts):
    sides = []
    for ns, dev in ((a.ns_cli, "l%d" % a.link), (a.ns_srv, "s%d" % a.link)):
        p = subprocess.Popen(["ip", "netns", "exec", ns, "tc", "-force", "-batch", "-"], stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
        sides.append((p, dev))
    log = []
    t0 = time.monotonic()

    def apply(loss):
        for p, dev in sides:
            p.stdin.write(line(dev, a, loss))
            p.stdin.flush()

    try:
        for t in starts:
            wait = t0 + t - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            apply(100)
            s = time.monotonic() - t0
            wait = t0 + t + a.ms / 1000.0 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            apply(a.loss)
            log.append({"start": round(s, 4), "end": round(time.monotonic() - t0, 4)})
    finally:
        apply(a.loss)
        for p, _ in sides:
            p.stdin.close()
            p.wait()
    if a.log:
        with open(a.log, "w") as f:
            json.dump(log, f)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Stalls of one lab link (see the docstring).")
    p.add_argument("cmd", choices=("plan", "run"))
    p.add_argument("--secs", type=float, required=True)
    p.add_argument("--ms", type=int, required=True, help="each stall, ms")
    p.add_argument("--every", type=float, default=None, help="a stall every this many s")
    p.add_argument("--gap", type=float, nargs=2, default=None, metavar=("LO", "HI"),
                   help="a stall LO to HI s after the last, at random")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--ns-cli"), p.add_argument("--ns-srv"), p.add_argument("--link", type=int, default=1)
    p.add_argument("--delay", type=float, default=25, help="the link's one-way delay, ms")
    p.add_argument("--rate", type=float, default=20, help="its rate, Mbit/s")
    p.add_argument("--queue", type=int, default=100, help="its netem queue, ms")
    p.add_argument("--loss", type=float, default=0, help="its loss between stalls, %%")
    p.add_argument("--inflight", type=int, default=0, help="BOND_NETEM_INFLIGHT")
    p.add_argument("--log", default=None, help="where the stalls' times go (JSON)")
    a = p.parse_args(argv)
    if bool(a.every) == bool(a.gap):
        sys.exit("stall: --every or --gap, one of them")
    starts = plan(a.secs, a.ms, a.every, a.gap, a.seed)
    if a.cmd == "plan":
        print(json.dumps(starts))
        return 0
    if not (a.ns_cli and a.ns_srv):
        sys.exit("stall: run needs --ns-cli and --ns-srv")
    return run(a, starts)


if __name__ == "__main__":
    sys.exit(main())
