# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The field recorder end to end: "bench/lab.sh fieldrec" (story 012).
#
# First bench/fieldrec_test.py: the analysis on a made-up recording whose
# answers are known. Then the real thing: 3 links, status_interval_ms 250
# and probe_idle_ms 100, cengarde-rec (openwrt/cengarde/files, under sh)
# recording the client's status file in 4 s files, 1000 pps down for 12 s
# while s2, the server's end of link 2, goes down for 3 s from the 4th
# second. Passes when the recorder stops clean on SIGTERM leaving only
# compressed files, every engine sample at most once and with time_ms, and
# bench/fieldrec.py finds one outage of 2 to 5 s on l2 with download loss,
# none on l1 and l3, and time with 2 of 3 links live.
LAB_CI=1

fieldrec() {
	local fail=0 rec cut dir="$RUN/rec" out
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="status_interval_ms = 250;probe_idle_ms = 100${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	if ! out=$(python3 "$LAB/fieldrec_test.py"); then
		echo "$out" | grep -v '^ok'
		echo "FAIL: bench/fieldrec_test.py"
		fail=1
	fi
	setup && start || return 1
	rm -rf "$dir"
	sh "$LAB/../openwrt/cengarde/files/cengarde-rec" -s "$RUN/client.json" -d "$dir" -r 4 -p 0.05 &
	rec=$!
	(
		sleep 4
		ip netns exec srv ip link set s2 down
		sleep 3
		ip netns exec srv ip link set s2 up
	) &
	cut=$!
	down 1000 12 >"$RUN/traffic.txt"
	wait "$cut"
	sleep 1
	kill -TERM "$rec"
	wait "$rec" || { echo "FAIL: cengarde-rec exited with $? on SIGTERM"; fail=1; }
	ls "$dir"
	if [ -e "$dir/current.jsonl" ]; then
		echo "FAIL: cengarde-rec left current.jsonl uncompressed"
		fail=1
	fi
	python3 - "$dir" <<'EOF' || fail=1
import glob, gzip, json, sys
files = sorted(glob.glob(sys.argv[1] + "/rec-*.jsonl.gz"))
rows = [json.loads(l) for f in files for l in gzip.open(f, "rt")]
ups = [r["uptime_ms"] for r in rows]
print(f"   {len(files)} files, {len(rows)} samples")
ok = len(files) >= 3 and len(rows) >= 40 and ups == sorted(set(ups)) and all("time_ms" in r for r in rows)
if not ok:
    print("FAIL: expected 3 or more files, 40 or more samples, each once, in order, with time_ms")
sys.exit(0 if ok else 1)
EOF
	python3 "$LAB/fieldrec.py" --json "$RUN/fieldrec.json" "$dir" >"$RUN/fieldrec.md" || fail=1
	sed -n '/^## Links$/,/^## Pairs/p' "$RUN/fieldrec.md" | grep '^|'
	python3 - "$RUN/fieldrec.json" <<'EOF' || fail=1
import json, sys
s = json.load(open(sys.argv[1]))
L = s["links"]
l2 = L["l2"]
errs = []
if l2["outages"] != 1 or not 2 <= (l2["outage_s"]["max"] or 0) <= 5:
    errs.append(f"l2: expected one outage of 2-5 s, got {l2['outages']} ({l2['outage_s']})")
if not (l2["loss_down_pct"] or 0) > 0:
    errs.append(f"l2: expected some download loss, got {l2['loss_down_pct']}")
for n in ("l1", "l3"):
    if L[n]["outages"]:
        errs.append(f"{n}: expected no outage, got {L[n]['outages']}")
if not s["simultaneous"].get("2 of 3 live"):
    errs.append(f"no time with 2 of 3 links live: {s['simultaneous']}")
for e in errs:
    print("FAIL: " + e)
sys.exit(1 if errs else 0)
EOF
	teardown
	if [ "$fail" = 0 ]; then
		echo "fieldrec: ok"
	else
		echo "fieldrec: FAILED"
	fi
	return "$fail"
}
