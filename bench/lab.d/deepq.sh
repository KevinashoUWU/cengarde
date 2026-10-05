# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# One path with a deep local queue on the server: "bench/lab.sh deepq".
#
# s3, the server's side of link 3, sends at 5 Mbit/s behind a 20 MB queue
# (tbf limit 20mb) while 20 000 pps (224 Mbit/s) flow down for 10 s. The
# queue is deeper than a socket's send buffer, so the socket path 3 sends
# from fills up. With one listen socket for every path (lanes = 1) that
# buffer was everybody's: the healthy paths dropped 13.6 % of their copies,
# the tunnel lost as much, and the full socket also stopped their probe
# replies (multithreading study, E4m). With lanes, path 3 leaves from its
# own lane: the tunnel loses nothing and the healthy paths drop nothing;
# only path 3 drops (or stops, once its replies time out).
LAB_CI=1

DEEPQ_PPS=${DEEPQ_PPS:-20000}
DEEPQ_S=${DEEPQ_S:-10}

# deepq_drops: the server's tx_drops per path ("link id: drops ...") and
# whether the healthy paths (links 0 and 1) dropped nothing (exit 0).
deepq_drops() {
	python3 - "$RUN/server.sock" <<'EOF'
import json, socket, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2)
s.connect(sys.argv[1])
s.sendall(b"status\n")
buf = b""
while chunk := s.recv(65536):
    buf += chunk
d = json.loads(buf)
links = {p["id"]: p for sess in d["sessions"] if sess["newest"] for p in sess["links"]}
print("server tx_drops by path: " + " ".join(f"{i}={links[i]['tx_drops']} ({links[i]['download']})"
                                             for i in sorted(links)) +
      " | lanes " + " ".join(f"{l['index']}:drops={l['drops']}" for l in d["lanes"][:3]))
sys.exit(0 if all(i in links and links[i]["tx_drops"] == 0 for i in (0, 1)) else 1)
EOF
}

deepq() {
	local fail=0 sent uniq out
	if [ "$(printf %s "$RUN/server.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/server.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "deepq: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup && start || return 1
	ip netns exec srv tc qdisc replace dev s3 root tbf rate 5mbit burst 32kb limit 20mb || return 1
	down "$DEEPQ_PPS" "$DEEPQ_S" >"$RUN/traffic.txt"
	echo "   s3 at 5mbit, 20 MB queue: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	if [ -z "$sent" ] || [ "$uniq" != "$sent" ]; then
		echo "FAIL: the tunnel lost packets: sent=$sent uniq=$uniq"
		fail=1
	fi
	out=$(deepq_drops) || { echo "FAIL: a healthy path dropped copies"; fail=1; }
	echo "   $out"
	grep -h "muted" "$RUN/server.log" | sed 's/^/   /'
	stop
	teardown
	[ "$fail" = 0 ] && echo "deepq: ok" || echo "deepq: FAILED"
	return "$fail"
}
