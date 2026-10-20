#!/usr/bin/env python3
"""Tests for bench/labq.py without a lab: the plan from a file and from the
command line, the order of the jobs (the arms side by side, rotated at each
repetition), what a job hands lab.sh, resuming from all.jsonl and the CPU split.
Run: python3 bench/labq_test.py
"""

import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import labq  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + what)


def args(*argv):
    p = labq.argparse.Namespace(plan=None, arm=None, client=None, server=None, env=None, cases=None, dirs=None,
                                cc=None, reps=None, secs=None, seed=None)
    it = iter(argv)
    for k in it:
        v = next(it)
        if k in ("arm", "client", "server", "env"):
            setattr(p, k, (getattr(p, k) or []) + [v])
        else:
            setattr(p, k, v)
    return p


with tempfile.TemporaryDirectory() as tmp:
    f = os.path.join(tmp, "plan.json")
    with open(f, "w") as fh:
        json.dump({"arms": [{"name": "a", "rev": "HEAD"}, {"name": "b", "rev": "HEAD", "client_extra": "x = 1"}],
                   "grid": {"cases": "equal het", "dirs": ["down"], "reps": 3, "secs": 30},
                   "env": {"BOND_P": 4}}, fh)
    plan = labq.load_plan(args("plan", f, "reps", 2, "server", "b=y = 2", "env", "BOND_WARM=5"))
    check([x["name"] for x in plan["arms"]] == ["a", "b"], "the arms in the file's order")
    check(plan["grid"]["reps"] == 2 and plan["grid"]["cases"] == ["equal", "het"] and plan["grid"]["cc"] == ["cubic"],
          "the command line over the file, words from a string, defaults for the rest")
    check(plan["arms"][1]["client_extra"] == "x = 1" and plan["arms"][1]["server_extra"] == "y = 2",
          "the extras from the file and from --server")
    check(plan["env"] == {"BOND_P": "4", "BOND_WARM": "5"} and plan["arms"][0]["variant"] == "bonded",
          "env from both, variant bonded by default")

plan = labq.load_plan(args("arm", "a=HEAD", "arm", "b=HEAD", "arm", "c=HEAD", "cases", "equal,het", "dirs", "down up",
                           "reps", 3, "seed", 7))
js = labq.jobs(plan)
check(len(js) == 3 * 2 * 2 * 3, "every arm, case, direction and repetition")
check([j["arm"] for j in js[:3]] == ["a", "b", "c"] and js[0]["case"] == "equal" and js[0]["dir"] == "down",
      "the arms side by side for one case and direction")
check([j["arm"] for j in js[12:15]] == ["b", "c", "a"] and [j["arm"] for j in js[24:27]] == ["c", "a", "b"],
      "the arms' order rotated at each repetition")
check([j["rep"] for j in js[:12]] == [1] * 12, "repetition by repetition")

arm = dict(plan["arms"][1], path="/x/cengarde", sha="abc", client_extra="k = v")
env = labq.job_env(plan, arm, {"arm": "b", "case": "het", "dir": "up", "cc": "cubic", "rep": 3}, 2)
check(env["LAB_ID"] == "2" and env["RUN"].endswith("/run-2") and env["CENGARDE_BIN"] == "/x/cengarde",
      "the lab, its run directory and the engine")
check(env["BOND_N"] == "1" and env["BOND_REP0"] == "2" and env["BOND_SEED"] == "7" and env["BOND_CASES"] == "het"
      and env["BOND_DIRS"] == "up" and env["BOND_VARIANTS"] == "bonded" and env["CLIENT_EXTRA"] == "k = v",
      "one repetition, numbered as in a BOND_N run, the same seed for every arm")

with tempfile.TemporaryDirectory() as tmp:
    with open(os.path.join(tmp, "all.jsonl"), "w") as fh:
        fh.write(json.dumps({"arm": "a", "case": "equal", "dir": "down", "cc": "cubic", "rep": 1, "sha": "s1"}) + "\n")
    done, shas = labq.read_done(tmp)
    check(done == {("a", "equal", "down", "cubic", 1)} and shas == {"a": "s1"}, "resume: the done jobs and commits")
    check(labq.read_done(os.path.join(tmp, "none")) == (set(), {}), "resume: nothing done")

local = labq.parse_node("local:2", None)
local["off"] = 4
remote = labq.parse_node("u@h:1", None)
r = labq.Runner(plan, "/tmp", [], [local, remote], None)
check([s for _, s in r.slots] == [5, 6, 1], "--lab-offset: the local labs from N+1, the remote ones as before")

sets = labq.cpu_sets(1)
check(sets is not None and len(sets) == 1, "one lab: all the CPUs")
n = len(os.sched_getaffinity(0))
check(labq.cpu_sets(n + 1) is None, "more labs than cores: no pinning")

print("labq_test: %s" % ("ok" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
