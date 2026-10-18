# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# A source address with no route through its own link: "bench/lab.sh srcroute".
#
# netifd puts a prefix's source route ("default from PREFIX") on one
# interface. When two uplinks get the same prefix, its addresses on the
# first one route out through the other only, and a link socket bound to
# its interface (SO_BINDTODEVICE) cannot connect from them: "Network
# unreachable". The OpenWrt e2e hit it on 2026-10-11 (story 010): eth1's
# newest address, the engine's pick, was of a prefix eth3 also got, so eth1
# fell back to the server's IPv4 address and stayed there.
#
# Here l1 has two IPv6 addresses, A (2001:db8:1::20, the newest, so the
# pick) and B (2001:db8:1::10), and two source routes to the server's
# 2001:db8::4: from A through l2, from B through l1. Its list is
# "[2001:db8::4]:59402 10.0.1.2:59402".
# 1. l1 is live on [2001:db8::4] from B, on a detour (not from its pick),
#    and the log says there is no route from A; 2000 pps go up and down
#    exactly once.
# 2. Restarted with B's route through l2 too, no IPv6 source works: l1 goes
#    live on 10.0.1.2, on a detour. With A's route back through l1, l1 is
#    back on [2001:db8::4] from A within two RECONCILE_MS (5 s), no longer
#    on a detour.
# A host without IPv6, or without source routes (IPV6_SUBTREES), skips it.
LAB_CI=1

SR_A=2001:db8:1::20
SR_B=2001:db8:1::10
SR_S=2001:db8::4

# sr_wait EXPR SECS: waits until EXPR (Python, on l: the status of l1 from
# the client's control socket) holds. Prints how long it took, or l1 as it
# was when SECS ran out (exit 1).
sr_wait() {
	python3 - "$RUN/client.sock" "$1" "$2" <<'EOF'
import json, socket, sys, time

path, expr, limit = sys.argv[1], sys.argv[2], float(sys.argv[3])


def status():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1)
    try:
        s.connect(path)
        s.sendall(b"status\n")
        buf = b""
        while chunk := s.recv(65536):
            buf += chunk
        return json.loads(buf)
    except (OSError, ValueError):
        return None
    finally:
        s.close()


t0, l = time.time(), None
while time.time() - t0 < limit:
    d = status()
    l = next((x for x in d["links"] if x["name"] == "l1"), None) if d else None
    if l is not None and eval("(" + expr + ")"):
        print(f"{time.time() - t0:.1f} s")
        sys.exit(0)
    time.sleep(0.1)
keys = ("state", "reason", "local", "remote", "candidate", "detour", "failovers")
print("not within %.0f s: %s" % (limit, {k: l.get(k) for k in keys} if l else "no l1"))
sys.exit(1)
EOF
}

# sr_traffic: 2000 pps for 3 s each way, every packet once.
sr_traffic() {
	local dir sent uniq dup fail=0
	for dir in up down; do
		"$dir" 2000 3 >/dev/null
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		if [ -z "$sent" ] || [ -z "$uniq" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
			echo "FAIL: $dir: sent=$sent uniq=$uniq dup=$dup"
			fail=1
		fi
	done
	return "$fail"
}

srcroute() {
	local fail=0 out
	local on_b="l['state'] == 'live' and l['remote'] == '[$SR_S]:59402' and l['local'].startswith('[$SR_B]:')"
	local on_a="l['state'] == 'live' and l['remote'] == '[$SR_S]:59402' and l['local'].startswith('[$SR_A]:')"
	ENGINE=c NLINKS=1
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup || { echo "srcroute: FAILED"; return 1; }
	if ! ip netns exec "$NS_CLI" test -d /proc/sys/net/ipv6; then
		teardown
		echo "srcroute: skip (no IPv6 in this kernel)"
		return 0
	fi
	ip -n "$NS_SRV" addr add 2001:db8:1::1/64 dev s1 nodad
	ip -n "$NS_SRV" addr add "$SR_S/128" dev s1 nodad
	ip -n "$NS_CLI" addr add "$SR_B/64" dev l1 nodad noprefixroute
	ip -n "$NS_CLI" addr add "$SR_A/64" dev l1 nodad noprefixroute
	ip -n "$NS_CLI" addr add 2001:db8:2::10/64 dev l2 nodad
	if ! ip -n "$NS_CLI" -6 route add "$SR_S/128" from "$SR_A/128" dev l2 2>/dev/null; then
		teardown
		echo "srcroute: skip (no IPv6 source routes in this kernel)"
		return 0
	fi
	ip -n "$NS_CLI" -6 route add "$SR_S/128" from "$SR_B/128" dev l1
	sed -i "/^\[link l1\]\$/{n;s/^server = .*/server = [$SR_S]:59402 10.0.1.2:59402/}" "$RUN/client.conf"

	echo "   1. A routes out through l2 only, B through l1"
	start
	out=$(sr_wait "$on_b and l['candidate'] == 0 and l['detour']" 5) ||
		{ echo "FAIL: l1 not live on [$SR_S] from B: $out"; fail=1; }
	echo "      l1 live on [$SR_S] from $SR_B after $out"
	grep -q "link l1: no route from \[$SR_A\]:0 through it to \[$SR_S\]:59402, sending from \[$SR_B\]" "$RUN/client.log" ||
		{ echo "FAIL: no log line about the route from A"; fail=1; }
	sr_traffic || fail=1
	stop

	echo "   2. B through l2 too: IPv4 until A routes through l1 again"
	ip -n "$NS_CLI" -6 route del "$SR_S/128" from "$SR_B/128" dev l1
	ip -n "$NS_CLI" -6 route add "$SR_S/128" from "$SR_B/128" dev l2
	start
	out=$(sr_wait "l['state'] == 'live' and l['remote'] == '10.0.1.2:59402' and l['candidate'] == 1 and l['detour']" 5) ||
		{ echo "FAIL: l1 not live on 10.0.1.2 on a detour: $out"; fail=1; }
	echo "      l1 live on 10.0.1.2 after $out"
	grep -q "link l1: cannot use \[$SR_S\]:59402 (connect \[$SR_S\]:59402: " "$RUN/client.log" ||
		{ echo "FAIL: no log line about the IPv6 address it could not use"; fail=1; }
	ip -n "$NS_CLI" -6 route replace "$SR_S/128" from "$SR_A/128" dev l1
	out=$(sr_wait "$on_a and l['candidate'] == 0 and not l['detour']" 12) ||
		{ echo "FAIL: l1 not back on [$SR_S] from A: $out"; fail=1; }
	echo "      route from A through l1: l1 back on [$SR_S] from $SR_A after $out"
	grep -q "link l1: a socket to \[$SR_S\]:59402 opens from \[$SR_A\]:0 now, back there from 10.0.1.1" \
		"$RUN/client.log" || { echo "FAIL: no log line about going back"; fail=1; }
	sr_traffic || fail=1

	grep -hE "link l1" "$RUN/client.log" | sed 's/^/   /'
	stop
	teardown
	[ "$fail" = 0 ] && echo "srcroute: ok" || echo "srcroute: FAILED"
	return "$fail"
}
