#!/usr/bin/env python3
"""A queue of bonding-lab runs (bench/lab.d/bond.sh) spread over several labs at
once (bench/lab.sh with LAB_ID), to compare engine builds ("arms").

  labq.py plan  [PLAN] [options]   the jobs it would run, in order, and the time
  labq.py build [PLAN] [options]   build each arm's revision once (cache)
  labq.py run   [PLAN] [options]   build, then run what is not done yet

PLAN is a JSON file:

  {"arms": [{"name": "base", "rev": "origin/master"},
            {"name": "idle50", "rev": "HEAD", "client_extra": "reorder_idle_ms = 50"}],
   "grid": {"cases": ["equal", "het"], "dirs": ["down", "up"], "cc": ["cubic"],
            "reps": 4, "secs": 30, "seed": 1},
   "env": {"BOND_NETEM_INFLIGHT": "1"}}

An arm has a name and either a git revision ("rev", built once per commit into
the cache: a detached git worktree, make, the binary kept) or a prebuilt engine
("bin"); "client_extra" and "server_extra" ("key = value;..."), "variant" (a
bond.sh variant, "bonded" by default) and "env" (more variables for its jobs)
are optional. The lab's scripts are always this tree's; only the engine changes.
The same can be said without a file: --arm NAME=REV (or NAME=/path/to/cengarde),
--client NAME='k = v;...', --server NAME='...', and --cases, --dirs, --cc, --reps,
--secs, --seed, --env K=V, which also override the file's.

A job is one case, direction, cc and repetition of one arm: one call of
"lab.sh bond" with BOND_N=1, BOND_REP0=rep-1 and BOND_SEED=seed (ltespikerand
draws the same jumps for every arm at the same repetition, and the same as one
BOND_N run would). Jobs go repetition by repetition, then case, direction and
cc, the arms side by side and their order rotated at each repetition, so drift
over the hours hits every arm alike. Up to -j labs run at once, LAB_ID 1..j
(run as root, or through sudo -n); --pin gives each lab its own CPU cores
(taskset), whole cores split evenly.

Output in --out DIR: ARM.jsonl per arm and all.jsonl, bond.sh's JSON lines with
"variant" set to the arm's name ("bond_variant" keeps bond.sh's), "arm", "sha"
and "slot" added: "bondtab.py table DIR/all.jsonl --ref base" compares the arms.
logs/ holds each job's output, plan.json the plan with each arm's commit, and
failed.txt the jobs that failed. Run again with the same --out to go on: done
jobs are skipped, failed ones tried again; an arm whose revision now resolves to
another commit is refused (use another --out).
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
GRID_DEFAULTS = {"cases": ["equal", "het"], "dirs": ["down", "up"], "cc": ["cubic"], "reps": 1, "secs": 10, "seed": 1}
KEY = ("arm", "case", "dir", "cc", "rep")


def die(msg):
    sys.exit("labq: " + msg)


def words(v):
    """A list from a list or a string of words separated by spaces or commas."""
    return [str(x) for x in v] if isinstance(v, list) else str(v).replace(",", " ").split()


# ---- the plan -------------------------------------------------------------------


def load_plan(a):
    plan = {"arms": [], "grid": {}, "env": {}}
    if a.plan:
        with open(a.plan) as f:
            plan.update(json.load(f))
    arms = {x["name"]: dict(x) for x in plan.get("arms", [])}
    order = [x["name"] for x in plan.get("arms", [])]
    for spec in a.arm or []:
        name, _, src = spec.partition("=")
        if not src:
            die("--arm NAME=REV or NAME=/path/to/cengarde: " + spec)
        if name not in arms:
            arms[name], order = {"name": name}, order + [name]
        arms[name].pop("rev", None), arms[name].pop("bin", None)
        arms[name]["bin" if os.sep in src and os.path.exists(src) else "rev"] = src
    for opt, field in ((a.client, "client_extra"), (a.server, "server_extra")):
        for spec in opt or []:
            name, _, val = spec.partition("=")
            if name not in arms:
                die("--client/--server for an arm that does not exist: " + name)
            arms[name][field] = val
    grid = dict(GRID_DEFAULTS, **plan.get("grid", {}))
    for k in ("cases", "dirs", "cc", "reps", "secs", "seed"):
        if getattr(a, k) is not None:
            grid[k] = getattr(a, k)
    for k in ("cases", "dirs", "cc"):
        grid[k] = words(grid[k])
    for k in ("reps", "secs", "seed"):
        grid[k] = int(grid[k])
    env = {k: str(v) for k, v in plan.get("env", {}).items()}
    for kv in a.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    out = [arms[n] for n in order]
    if not out:
        die("no arms (a plan file, or --arm NAME=REV)")
    for arm in out:
        if not NAME_RE.match(arm["name"]):
            die("an arm's name: letters, digits, . _ + -: " + arm["name"])
        if ("rev" in arm) == ("bin" in arm):
            die("arm %s: either rev or bin" % arm["name"])
        arm.setdefault("variant", "bonded")
    return {"arms": out, "grid": grid, "env": env}


def jobs(plan):
    """Every job, in the order they run: repetition, case, direction, cc, and the
    arms side by side, their order rotated at each repetition."""
    g, arms, out = plan["grid"], plan["arms"], []
    for rep in range(1, g["reps"] + 1):
        k = (rep - 1) % len(arms)
        rot = arms[k:] + arms[:k]
        for case in g["cases"]:
            for d in g["dirs"]:
                for cc in g["cc"]:
                    for arm in rot:
                        out.append({"arm": arm["name"], "case": case, "dir": d, "cc": cc, "rep": rep})
    return out


def job_id(j):
    return "{arm}-{case}-{dir}-{cc}-{rep}".format(**j)


def job_secs(plan, j):
    """About how long a job takes: its seconds, about 4 more per run and 8 to
    build and tear down its lab (bond_plan's rule, plus the checks)."""
    s = plan["grid"]["secs"]
    if j["case"].startswith("starlink15"):
        s = max(s, 31)
    return s + 12


# ---- builds ---------------------------------------------------------------------


def git(*args, cwd=REPO):
    return subprocess.run(["git", "-C", cwd] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def resolve(arm):
    if "bin" in arm:
        return os.path.realpath(arm["bin"]), "bin:" + os.path.realpath(arm["bin"])
    try:
        sha = git("rev-parse", "--verify", arm["rev"] + "^{commit}")
    except subprocess.CalledProcessError:
        die("arm %s: unknown revision %s" % (arm["name"], arm["rev"]))
    return None, sha


def build(arm, cache, jobs_make):
    """The engine of arm (its binary's path, built when not in the cache)."""
    if "bin" in arm:
        b = os.path.realpath(arm["bin"])
        if not os.access(b, os.X_OK):
            die("arm %s: %s is not an executable" % (arm["name"], b))
        return b
    sha = arm["sha"]
    out = os.path.join(cache, sha[:12])
    b = os.path.join(out, "cengarde")
    if os.access(b, os.X_OK):
        return b
    os.makedirs(out, exist_ok=True)
    src = os.path.join(out, "src")
    print("labq: building %s (%s, %s)" % (arm["name"], arm["rev"], sha[:12]), flush=True)
    if os.path.exists(src):
        subprocess.run(["git", "-C", REPO, "worktree", "remove", "--force", src], capture_output=True)
    git("worktree", "add", "--detach", src, sha)
    try:
        r = subprocess.run(["make", "-s", "-j%d" % jobs_make, "-C", os.path.join(src, "engine"), "cengarde"],
                           capture_output=True, text=True)
        if r.returncode:
            die("arm %s: the build failed:\n%s" % (arm["name"], (r.stdout + r.stderr)[-2000:]))
        os.replace(os.path.join(src, "engine", "cengarde"), b + ".tmp")
        os.replace(b + ".tmp", b)
    finally:
        subprocess.run(["git", "-C", REPO, "worktree", "remove", "--force", src], capture_output=True)
    return b


# ---- CPUs -----------------------------------------------------------------------


def cpu_sets(n):
    """n CPU lists, whole cores (with their hyperthreads) split evenly; None when
    there are fewer cores than labs."""
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        try:
            with open("/sys/devices/system/cpu/cpu%d/topology/core_id" % cpu) as f:
                core = (f.read().strip(),)
            with open("/sys/devices/system/cpu/cpu%d/topology/physical_package_id" % cpu) as f:
                core += (f.read().strip(),)
        except OSError:
            core = (str(cpu),)
        cores.setdefault(core, []).append(cpu)
    groups = list(cores.values())
    if len(groups) < n:
        return None
    per = len(groups) // n
    return [",".join(str(c) for g in groups[i * per:(i + 1) * per] for c in g) for i in range(n)]


# ---- running --------------------------------------------------------------------


def read_done(out):
    """The keys of the jobs in all.jsonl, and the commit each arm ran with."""
    done, shas = set(), {}
    try:
        with open(os.path.join(out, "all.jsonl")) as f:
            for ln in f:
                if ln.strip():
                    d = json.loads(ln)
                    done.add(tuple(d[k] for k in KEY))
                    shas.setdefault(d["arm"], d.get("sha"))
    except FileNotFoundError:
        pass
    return done, shas


def job_env(plan, arm, j, slot):
    g = plan["grid"]
    env = dict(plan["env"])
    env.update({k: str(v) for k, v in arm.get("env", {}).items()})
    env.update({"LAB_ID": str(slot), "RUN": os.path.join(HERE, "run-%d" % slot), "CENGARDE_BIN": arm["path"],
                "BOND_CASES": j["case"], "BOND_DIRS": j["dir"], "BOND_CC": j["cc"], "BOND_VARIANTS": arm["variant"],
                "BOND_N": "1", "BOND_REP0": str(j["rep"] - 1), "BOND_SEED": str(g["seed"]), "BOND_S": str(g["secs"]),
                "CLIENT_EXTRA": arm.get("client_extra", ""), "SERVER_EXTRA": arm.get("server_extra", "")})
    return env


class Runner:
    def __init__(self, plan, out, todo, nslots, pins):
        self.plan, self.out, self.todo, self.pins = plan, out, list(todo), pins
        self.arms = {a["name"]: a for a in plan["arms"]}
        self.lock = threading.Lock()
        self.stop = False
        self.n, self.ok, self.failed, self.t0 = len(todo), 0, [], time.monotonic()
        self.slots = list(range(1, nslots + 1))

    def next_job(self):
        with self.lock:
            if self.stop or not self.todo:
                return None
            return self.todo.pop(0)

    def worker(self, slot):
        while (j := self.next_job()) is not None:
            self.run_job(j, slot)

    def run_job(self, j, slot):
        arm = self.arms[j["arm"]]
        env = job_env(self.plan, arm, j, slot)
        cmd = ([] if os.geteuid() == 0 else ["sudo", "-n"])
        if self.pins:
            cmd += ["taskset", "-c", self.pins[slot - 1]]
        cmd += ["env"] + ["%s=%s" % kv for kv in sorted(env.items())] + [os.path.join(HERE, "lab.sh"), "bond"]
        log = os.path.join(self.out, "logs", job_id(j) + ".log")
        t = time.monotonic()
        with open(log, "w") as f:
            f.write("# " + " ".join(shlex.quote(c) for c in cmd) + "\n")
            f.flush()
            rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL).returncode
        lines = []
        try:
            with open(os.path.join(env["RUN"], "bond.jsonl")) as f:
                lines = [json.loads(ln) for ln in f if ln.strip()]
        except (OSError, ValueError):
            pass
        took = time.monotonic() - t
        with self.lock:
            if rc != 0 or len(lines) != 1:
                self.failed.append(j)
                with open(os.path.join(self.out, "failed.txt"), "a") as f:
                    f.write("%s rc=%d lines=%d log=%s\n" % (job_id(j), rc, len(lines), log))
                print("labq: [%d/%d] %s slot %d: FAILED (rc %d), see %s" % (self.ok + len(self.failed), self.n,
                      job_id(j), slot, rc, log), flush=True)
                return
            d = lines[0]
            d["bond_variant"], d["variant"] = d.get("variant"), arm["name"]
            d.update(arm=arm["name"], sha=arm["sha"], slot=slot, rep=j["rep"])
            row = json.dumps(d) + "\n"
            for name in (arm["name"] + ".jsonl", "all.jsonl"):
                with open(os.path.join(self.out, name), "a") as f:
                    f.write(row)
            self.ok += 1
            print("labq: [%d/%d] %-40s slot %d %6.1f Mbit/s retrans %s rtt p50 %s p99 %s (%.0f s)" % (
                self.ok + len(self.failed), self.n, job_id(j), slot, d.get("mbps", 0), d.get("retrans"),
                d.get("rtt_p50"), d.get("rtt_p99"), took), flush=True)

    def run(self):
        threads = []
        for slot in self.slots:
            th = threading.Thread(target=self.worker, args=(slot,), daemon=True)
            th.start()
            threads.append(th)
            time.sleep(1)  # the labs' set-ups a little apart
        try:
            for th in threads:
                while th.is_alive():
                    th.join(0.5)
        except KeyboardInterrupt:
            # lab.sh gets the terminal's SIGINT too and tears its lab down
            with self.lock:
                self.stop = True
            print("labq: interrupted, waiting for the running jobs", flush=True)
            for th in threads:
                th.join()
            return 130
        print("labq: %d done, %d failed, %.0f min" % (self.ok, len(self.failed), (time.monotonic() - self.t0) / 60))
        return 1 if self.failed else 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Runs of bench/lab.sh bond spread over several labs (see the docstring).")
    p.add_argument("cmd", choices=("plan", "build", "run"))
    p.add_argument("plan", nargs="?", help="a JSON plan")
    p.add_argument("--arm", action="append", help="NAME=REV or NAME=/path/to/cengarde")
    p.add_argument("--client", action="append", help="NAME='key = value;...' (CLIENT_EXTRA of that arm)")
    p.add_argument("--server", action="append", help="NAME='key = value;...' (SERVER_EXTRA of that arm)")
    p.add_argument("--env", action="append", help="K=V for every job (BOND_NETEM_INFLIGHT=1, BOND_P=4...)")
    p.add_argument("--cases"), p.add_argument("--dirs"), p.add_argument("--cc")
    p.add_argument("--reps", type=int), p.add_argument("--secs", type=int), p.add_argument("--seed", type=int)
    p.add_argument("-j", type=int, default=1, help="labs at once (LAB_ID 1..j)")
    p.add_argument("--pin", action="store_true", help="each lab on its own cores (taskset)")
    p.add_argument("--out", help="the output directory (needed by run)")
    p.add_argument("--cache", default=os.path.join(HERE, "labq-cache"), help="where builds go (bench/labq-cache)")
    a = p.parse_args(argv)
    plan = load_plan(a)
    for arm in plan["arms"]:
        arm["path"], arm["sha"] = resolve(arm)
    all_jobs = jobs(plan)
    done, shas = read_done(a.out) if a.out else (set(), {})
    todo = [j for j in all_jobs if tuple(j[k] for k in KEY) not in done]
    if a.cmd == "plan":
        for j in todo:
            print(job_id(j))
        secs = sum(job_secs(plan, j) for j in todo)
        print("labq: %d jobs (%d done), about %.0f min with -j %d" % (len(todo), len(all_jobs) - len(todo),
              secs / 60 / max(a.j, 1), a.j))
        return 0
    os.makedirs(a.cache, exist_ok=True)
    for arm in plan["arms"]:
        arm["path"] = build(arm, a.cache, os.cpu_count() or 4)
    if a.cmd == "build":
        for arm in plan["arms"]:
            print("%s %s %s" % (arm["name"], arm["sha"], arm["path"]))
        return 0
    if not a.out:
        die("run needs --out DIR")
    for arm in plan["arms"]:
        if arm["name"] in shas and shas[arm["name"]] != arm["sha"]:
            die("arm %s ran as %s in %s, now it is %s: use another --out" % (arm["name"], shas[arm["name"]], a.out,
                arm["sha"]))
    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
    with open(os.path.join(a.out, "plan.json"), "w") as f:
        json.dump(plan, f, indent=1)
    pins = None
    if a.pin:
        pins = cpu_sets(a.j)
        if pins is None:
            die("--pin: fewer cores than labs")
    print("labq: %d jobs to run (%d done) on %d lab(s)%s" % (len(todo), len(all_jobs) - len(todo), a.j,
          " pinned to " + " | ".join(pins) if pins else ""), flush=True)
    if not todo:
        return 0
    return Runner(plan, a.out, todo, a.j, pins).run()


if __name__ == "__main__":
    sys.exit(main())
