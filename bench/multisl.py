#!/usr/bin/env python3
"""Several Starlink dishes in time for the bonding lab (bench/lab.d/bond.sh, the
msl cases; story 022): each dish a link like starsat's (starslot.py), with
what makes their losses independent or shared.

  multisl.py plan --secs S --seed N --dishes K [model]          the schedule, JSON lines
  multisl.py run  --ns-cli NS --ns-srv NS --links "1 2" [model]  apply it with tc (root)
  multisl.py joint --secs S --seed N --dishes K [model]          the schedule's loss, alone and joint

Each dish, both directions (the client's lN and the server's sN):

- its slots as on nostalgica (starslot.py, story 015): a base delay drawn per
  15 s slot (its own draw per dish: each dish has its own satellite) with a
  slow drift, its rate drawn per slot from --up and --down;
- independent random loss, --ind % per packet (netem's loss): the part of
  Starlink's loss without a queue (story 017: 0.3-1.3 %) that no other dish
  shares (model a);
- loss episodes, --ep % of the time lost on each dish in blackouts of
  --ep-ms (10 to 60 ms, uniform), all of the dish's packets in one lost: a
  congested hop past the dish. Of each dish's episodes a share --rho hits
  every dish at once (the same PoP, the same path to the VPS: the copies of
  a packet cross it within a few ms of each other), the rest that dish alone,
  so each dish loses the same at any --rho and only the overlap changes
  (model b: rho 0 independent, 1 every episode shared);
- the slot edges, on the wall clock (:12, :27, :42, :57, as the engine's slot
  clock expects): at each edge each dish stops with probability --edge, drawn
  per dish (model c; 1: every dish at every edge, all aligned), the field's
  stop: 25 ms lost and 20 ms held 55 ms more (starslot.py);
- a microcut (model d, story 017: 20-40 s at 50-77 % lost): --cut one (the
  first dish) or all, --cut-loss % for the middle third of the run.

The draw comes from --seed (bond.sh: BOND_SEED * 1000 + the repetition), the
same for every arm at the same repetition; dish k's own draws from seed and k.
Outages of a single dish (starslot's 1.45 an hour) are left out: they would add
noise to what the episodes and the cut say.

The changes go to two tc processes in batch mode (one per netns), so that a
blackout starts on every dish within a millisecond or so.
"""

import argparse
import json
import random
import subprocess
import sys
import time

sys.dont_write_bytecode = True

SLOT_S = 15.0
EDGE_S = 12.0
STEP_S = 0.5
# starslot.py's field model (nostalgica, 2026-10-19), one-way ms
LEVEL_MS = [-2.5, -2.0, -1.3, -0.9, -0.6, -0.4, -0.3, -0.1, 0.1, 0.2, 0.5, 0.6, 0.9, 1.2, 1.9, 2.9]
SLOPE_MS_S = [-0.20, -0.12, -0.08, -0.04, -0.03, -0.01, -0.01, 0.0, 0.0, 0.01, 0.02, 0.03, 0.05, 0.08, 0.12, 0.21]
CUT_LOSS_MS = 25
CUT_HOLD_MS = 20
HOLD_MS = 55


def episodes(rng, secs, rate, lo_ms, hi_ms):
    """A Poisson process of blackouts: [(from, to)] in s."""
    out = []
    if rate <= 0:
        return out
    t = rng.expovariate(rate)
    while t < secs:
        d = rng.uniform(lo_ms, hi_ms) / 1000
        out.append((t, t + d))
        t += d + rng.expovariate(rate)
    return out


def model(a, k):
    """Dish k's pieces: slots [(t0, level, slope, up, down)], its blackouts,
    its edges [(t_loss, t_hold, t_end)], the shared blackouts apart."""
    rng = random.Random(a.seed * 101 + k)
    nslots = int(a.secs // SLOT_S) + 1
    delays = a.delays
    base = delays[k] if k < len(delays) else delays[-1]
    slots = []
    for s in range(nslots):
        i = rng.randrange(len(a.up_r))
        slots.append((s * SLOT_S, base + rng.choice(LEVEL_MS), rng.choice(SLOPE_MS_S), a.up_r[i], a.down_r[i]))
    erng = random.Random(a.seed * 101 + k + 50)
    edges = []
    for s in range(1, nslots + 1):
        if a.edge > 0 and erng.random() < a.edge:
            te = s * SLOT_S
            edges.append((te - (CUT_LOSS_MS + CUT_HOLD_MS) / 1000, te - CUT_HOLD_MS / 1000, te))
    return slots, edges


def blackouts(a):
    """Each dish's blackouts: its own and the shared ones."""
    mean_s = (a.ep_lo + a.ep_hi) / 2000
    lam = a.ep / 100 / mean_s if a.ep > 0 else 0
    shared = episodes(random.Random(a.seed * 101 + 99), a.secs, lam * a.rho, a.ep_lo, a.ep_hi)
    out = []
    for k in range(a.dishes):
        own = episodes(random.Random(a.seed * 101 + k + 200), a.secs, lam * (1 - a.rho), a.ep_lo, a.ep_hi)
        out.append(sorted(own + shared))
    return out


def inside(spans, t):
    return any(x <= t < y for x, y in spans)


def state(a, k, t, slots, edges, black):
    """Dish k at time t: (delay ms, up, down, loss %)."""
    s = min(int(t // SLOT_S), len(slots) - 1)
    t0, lev, slope, up, down = slots[s]
    i = int((t - t0) // STEP_S)
    d = max(lev + slope * (i * STEP_S - SLOT_S / 2), 0.1)
    keep = 1 - a.ind / 100
    for tl, th, te in edges:
        if tl <= t < th:
            keep = 0
        elif th <= t < te:
            d += HOLD_MS
    if inside(black[k], t):
        keep = 0
    if a.cut != "none" and (a.cut == "all" or k == 0) and a.cut_t[0] <= t < a.cut_t[1]:
        keep *= 1 - a.cut_loss / 100
    return round(d, 1), up, down, round(100 * (1 - keep), 3)


def plan(a):
    """[(t, k, state)]: each dish's state at each change, in time order."""
    pieces = [model(a, k) for k in range(a.dishes)]
    black = blackouts(a)
    times = set()
    for k in range(a.dishes):
        slots, edges = pieces[k]
        t = 0.0
        while t < a.secs:
            times.add(t)
            t += STEP_S
        for e in edges:
            times.update(e)
        for x, y in black[k]:
            times.update((x, y))
    if a.cut != "none":
        times.update(a.cut_t)
    out, last = [], {}
    for t in sorted(x for x in times if 0 <= x < a.secs):
        for k in range(a.dishes):
            st = state(a, k, t, pieces[k][0], pieces[k][1], black)
            if last.get(k) != st:
                last[k] = st
                out.append((t, k, st))
    return out


def limit(rate, queue_ms, delay_ms, inflight):
    """netem's limit in packets, as bond.sh's bond_link counts it."""
    lim = int(rate * 1000000 * queue_ms / 8000 / 1400) + 10
    if inflight:
        lim += int(rate * 1000000 * int(delay_ms) / 8000 / 1400)
    return lim


def tc_line(dev, delay, rate, queue_ms, loss, inflight):
    return "qdisc replace dev %s root netem delay %.1fms rate %smbit limit %d loss %s%%\n" % (
        dev, delay, rate, limit(rate, queue_ms, delay, inflight), ("%.3f" % loss).rstrip("0").rstrip("."))


def joint(a):
    """What the schedule loses over time (the plan is piecewise constant): each
    dish alone, every dish at once, and the product of each dish's mean (what
    independence would give)."""
    ev = plan(a)
    cur = [0.0] * a.dishes
    alone = [0.0] * a.dishes
    both = 0.0
    k = 0
    while k < len(ev):
        t = ev[k][0]
        while k < len(ev) and ev[k][0] == t:
            cur[ev[k][1]] = ev[k][2][3] / 100
            k += 1
        t1 = ev[k][0] if k < len(ev) else a.secs
        p = 1.0
        for d in range(a.dishes):
            alone[d] += cur[d] * (t1 - t)
            p *= cur[d]
        both += p * (t1 - t)
    prod = 1.0
    for x in alone:
        prod *= x / a.secs
    return {"dishes": a.dishes, "alone_pct": [round(100 * x / a.secs, 4) for x in alone],
            "joint_pct": round(100 * both / a.secs, 5), "independent_pct": round(100 * prod, 5)}


def run(a, ev):
    t0 = time.monotonic() - a.off
    if a.off:
        now = time.monotonic() - t0
        latest = {}
        for e in ev:
            if e[0] <= now:
                latest[e[1]] = e
        ev = sorted(latest.values()) + [e for e in ev if e[0] > now]
    procs = {}
    for ns in (a.ns_cli, a.ns_srv):
        procs[ns] = subprocess.Popen(["ip", "netns", "exec", ns, "tc", "-force", "-batch", "-"], stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    try:
        i = 0
        while i < len(ev):
            t = ev[i][0]
            wait = t0 + t - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            while i < len(ev) and ev[i][0] == t:
                _, k, (d, up, down, loss) = ev[i]
                n = a.links[k]
                procs[a.ns_cli].stdin.write(tc_line("l%d" % n, d, up, a.queue, loss, a.inflight))
                procs[a.ns_srv].stdin.write(tc_line("s%d" % n, d, down, a.queue, loss, a.inflight))
                i += 1
            for p in procs.values():
                p.stdin.flush()
    except BrokenPipeError:
        pass
    finally:
        for p in procs.values():
            try:
                p.stdin.close()
            except BrokenPipeError:
                pass
            p.wait()
    return 0


def rates(s):
    return [float(x) if "." in x else int(x) for x in s.replace(",", " ").split()]


def parse(argv):
    p = argparse.ArgumentParser(description="Several Starlink dishes in time for bond.sh (see the docstring).")
    p.add_argument("cmd", choices=("plan", "run", "joint"))
    p.add_argument("--secs", type=float, required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--dishes", type=int, default=None, help="default: as many as --links")
    p.add_argument("--links", default="1", help="the lab links of the dishes, in order")
    p.add_argument("--delay", default="15", help="each dish's one-way ms of its median slot (the last for the rest)")
    p.add_argument("--up", default="15 25 35", help="rates to draw from per slot, Mbit/s, the client's side")
    p.add_argument("--down", default="100 150 200", help="the same for the server's side")
    p.add_argument("--queue", type=int, default=15, help="netem queue, ms")
    p.add_argument("--ind", type=float, default=0.0, help="independent random loss per dish, %%")
    p.add_argument("--ep", type=float, default=0.0, help="each dish's time in blackouts, %%")
    p.add_argument("--ep-ms", default="10 60", help="a blackout's length, ms (uniform between the two)")
    p.add_argument("--rho", type=float, default=0.0, help="the share of each dish's blackouts that hit every dish")
    p.add_argument("--edge", type=float, default=1.0, help="each dish's probability of stopping at an edge")
    p.add_argument("--cut", choices=("none", "one", "all"), default="none")
    p.add_argument("--cut-loss", type=float, default=60.0, help="%% lost in the microcut, the run's middle third")
    p.add_argument("--inflight", type=int, default=0, help="BOND_NETEM_INFLIGHT")
    p.add_argument("--free", action="store_true", help="slot 0 starts now, not at the wall clock's last edge")
    p.add_argument("--ns-cli"), p.add_argument("--ns-srv")
    a = p.parse_args(argv)
    a.links = [int(x) for x in a.links.replace(",", " ").split()]
    if a.dishes is None:
        a.dishes = len(a.links)
    if a.dishes > len(a.links) and a.cmd == "run":
        sys.exit("multisl: more dishes than --links")
    a.delays = [float(x) for x in a.delay.replace(",", " ").split()]
    a.up_r, a.down_r = rates(a.up), rates(a.down)
    if len(a.up_r) != len(a.down_r):
        sys.exit("multisl: --up and --down need as many rates")
    a.ep_lo, a.ep_hi = (float(x) for x in a.ep_ms.replace(",", " ").split())
    if not 0 <= a.rho <= 1 or not 0 <= a.edge <= 1:
        sys.exit("multisl: --rho and --edge go from 0 to 1")
    return a


def main(argv=None):
    a = parse(argv)
    # aligned on the wall clock, plan time 0 is the last edge (starslot.py) and
    # the run starts off seconds into the plan; the cut is in the run's time
    a.off = (time.time() - EDGE_S) % SLOT_S if a.cmd == "run" and not a.free else 0.0
    a.cut_t = (a.off + a.secs / 3, a.off + 2 * a.secs / 3)
    if a.cmd == "joint":
        print(json.dumps(joint(a)))
        return 0
    a.secs += a.off
    ev = plan(a)
    if a.cmd == "plan":
        for t, k, (d, up, down, loss) in ev:
            print(json.dumps({"t": round(t, 4), "dish": k, "delay": d, "up": up, "down": down, "loss": loss}))
        return 0
    if not a.ns_cli or not a.ns_srv:
        sys.exit("multisl: run needs --ns-cli and --ns-srv")
    return run(a, ev)


if __name__ == "__main__":
    sys.exit(main())
