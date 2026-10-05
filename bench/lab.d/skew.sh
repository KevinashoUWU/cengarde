# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# First arrivals on identical links: "bench/lab.sh skew" (gate C5).
#
# An end sends a batch path after path, so on three identical links the
# path sent first delivers first. When it was always path 0, link 1 won
# 90-100 % of the first arrivals (multithreading study, 5.3), a bias in
# rx_first, the "wins" of the link statistics. The server now rotates the
# first path from batch to batch.
#
# 3 links, 2000 and 40 000 pps, 5 s each way. Download (the client's
# rx_first): no link above SKEW_MAX % (60) of the first arrivals at either
# rate. Upload (the server's rx_first) is reported: the router still sends
# link after link until its send threads (design PR 3c).
LAB_CI=1

SKEW_MAX=${SKEW_MAX:-60}
SKEW_S=${SKEW_S:-5}

# skew_firsts END: the rx_first of every link of END (client or server),
# as "name=count ...".
skew_firsts() {
	python3 - "$RUN/$1.sock" "$1" <<'EOF'
import json, socket, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2)
s.connect(sys.argv[1])
s.sendall(b"status\n")
buf = b""
while chunk := s.recv(65536):
    buf += chunk
d = json.loads(buf)
if sys.argv[2] == "client":
    firsts = {l["name"]: l["rx_first"] for l in d["links"]}
else:
    firsts = {f"l{p['id'] + 1}": p["rx_first"] for sess in d["sessions"] if sess["newest"] for p in sess["links"]}
print(" ".join(f"{k}={v}" for k, v in sorted(firsts.items())))
EOF
}

# skew_share BEFORE AFTER: each link's share of the first arrivals between
# two skew_firsts lines; exit 1 when one is above SKEW_MAX %.
skew_share() {
	python3 - "$1" "$2" "$SKEW_MAX" <<'EOF'
import sys

before = dict(kv.split("=") for kv in sys.argv[1].split())
after = dict(kv.split("=") for kv in sys.argv[2].split())
got = {k: int(v) - int(before.get(k, 0)) for k, v in after.items()}
total = sum(got.values()) or 1
share = {k: 100 * v / total for k, v in got.items()}
print("first arrivals: " + " ".join(f"{k} {share[k]:.1f} %" for k in sorted(share)) + f" (of {total})")
sys.exit(0 if max(share.values()) <= float(sys.argv[3]) else 1)
EOF
}

skew() {
	local fail=0 dir end rate before out
	if [ "$(printf %s "$RUN/client.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/client.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "skew: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup && start || return 1
	for dir in down up; do
		end=$([ "$dir" = down ] && echo client || echo server)
		for rate in 2000 40000; do
			before=$(skew_firsts "$end")
			"$dir" "$rate" "$SKEW_S" >"$RUN/traffic.txt"
			if out=$(skew_share "$before" "$(skew_firsts "$end")"); then
				echo "   $dir $rate pps, at the $end: $out"
			elif [ "$dir" = down ]; then
				echo "   $dir $rate pps, at the $end: $out"
				echo "FAIL: a link took more than $SKEW_MAX % of the first arrivals down"
				fail=1
			else
				echo "   $dir $rate pps, at the $end: $out (reported: the router sends link after link)"
			fi
		done
	done
	stop
	teardown
	[ "$fail" = 0 ] && echo "skew: ok" || echo "skew: FAILED"
	return "$fail"
}
