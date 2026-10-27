# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The field throughput kit end to end: "bench/lab.sh fieldperf" (story 012).
#
# First bench/fieldperf_test.py: the analysis on a made-up run whose answers
# are known. Then the real thing on 2 links: iperf3 servers in srv on two
# ports, bench/fieldperf.sh (under sh) in cli with the links and each one's
# VPS address taken from the engine's status, "solo" and "all" for 2 s each
# way (its "tunnel" mode is not run here), and bench/fieldperf.py on
# what it wrote. Passes when "plan" prints the estimate for the three modes
# and runs nothing, every test has its iperf3 with traffic and its ping,
# and the counters were read (the UDP sockets', each device's, the
# engine's).
LAB_CI=1

fieldperf() {
	local fail=0 out="$RUN/fieldperf" p plan
	NLINKS=2
	if ! p=$(python3 "$LAB/fieldperf_test.py"); then
		echo "$p" | grep -v '^ok'
		echo "FAIL: bench/fieldperf_test.py"
		fail=1
	fi
	if ! command -v iperf3 >/dev/null; then
		echo "fieldperf: the end-to-end half skipped, no iperf3 here (apt-get install iperf3)"
		return "$fail"
	fi
	trap 'fieldperf_stop; teardown' EXIT
	trap 'fieldperf_stop; teardown; exit 130' INT TERM
	setup && start || return 1
	for p in 5201 5202; do
		ip netns exec "$NS_SRV" iperf3 -s -p "$p" -D -I "$RUN/iperf3-$p.pid" || { echo "FAIL: iperf3 server on $p"; return 1; }
	done
	rm -rf "$out"
	plan=$(ip netns exec "$NS_CLI" sh "$LAB/fieldperf.sh" plan -S "$RUN/client.json" -t 2 -P 1 -o "$out" 2>&1)
	printf '   %s\n' "${plan//$'\n'/$'\n'   }"
	if ! echo "$plan" | grep -q '^Total: about 210 MB' || [ -e "$out" ]; then
		echo "FAIL: plan: expected 210 MB (3 modes x 2 links x 140 Mbit/s x 2 s) and nothing run"
		fail=1
	fi
	if ! ip netns exec "$NS_CLI" sh "$LAB/fieldperf.sh" run -m "solo all" -S "$RUN/client.json" -t 2 -P 1 -o "$out" -y \
		>"$RUN/fieldperf.log" 2>&1; then
		cat "$RUN/fieldperf.log"
		echo "FAIL: fieldperf.sh run"
		fail=1
	fi
	grep -E '^  (solo|all)-' "$RUN/fieldperf.log" | sed 's/^/ /'
	python3 "$LAB/fieldperf.py" --json "$RUN/fieldperf.json" "$out" >"$RUN/fieldperf.md" || fail=1
	grep '^|' "$RUN/fieldperf.md" | sed -n '/^| test/,$p'
	python3 - "$RUN/fieldperf.json" <<'EOF' || fail=1
import json, sys
rows = json.load(open(sys.argv[1]))
errs = []
want = [("solo", "up", "l1"), ("solo", "down", "l1"), ("solo", "up", "l2"), ("solo", "down", "l2"),
        ("all", "up", "*"), ("all", "up", "l1"), ("all", "up", "l2"),
        ("all", "down", "*"), ("all", "down", "l1"), ("all", "down", "l2")]
got = [(r["mode"], r["dir"], r["link"]) for r in rows]
if got != want:
    errs.append(f"rows: expected {want}, got {got}")
for r in rows:
    what = f"{r['mode']} {r['dir']} {r['link']}"
    if r["error"] or not (r["mbps"] or 0) > 1:
        errs.append(f"{what}: no traffic ({r['mbps']}, {r['error']})")
    if r["link"] != "*" and (r["ping_p50"] is None or r["dev_drops"] is None or r["sock_drops"] is None):
        errs.append(f"{what}: no ping or counters: {r}")
    if (r["link"] == "*" or r["mode"] == "solo") and r["sndbuf"] is None:
        errs.append(f"{what}: no UDP counters")
for e in errs:
    print("FAIL: " + e)
sys.exit(1 if errs else 0)
EOF
	fieldperf_stop
	teardown
	if [ "$fail" = 0 ]; then
		echo "fieldperf: ok"
	else
		echo "fieldperf: FAILED"
	fi
	return "$fail"
}

fieldperf_stop() {
	local f
	for f in "$RUN"/iperf3-*.pid; do
		[ -e "$f" ] && kill "$(cat "$f")" 2>/dev/null
		rm -f "$f"
	done
}
