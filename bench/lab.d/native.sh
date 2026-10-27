# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The native tunnel with real TCP: "bench/lab.sh native".
#
# Client and server own a TUN device each (cg0: 10.79.0.2/16 and
# 10.79.0.1/16, as in every lab, and fd00:79::2/64 and fd00:79::1/64 where
# the kernel has IPv6), over 2 links shaped to
# NATIVE_RATE each way, with split = on at the client (the server splits
# the download when the client asks, its default). It pings across the
# TUN, then runs iperf3 TCP up and down for NATIVE_S seconds each, and
# checks that both directions were split, that nothing was refused at
# the TUN (not IP, or a source not the client's) and that the tunnel
# carried more than one link could (NATIVE_MIN_X x NATIVE_RATE).
#
# Then two routers on one server, each a [client NAME] with its own
# tun_route: alpha (l1, TUN cga, 10.79.0.2) and bravo (l2, cgb,
# 10.79.0.3), both in the cli namespace. Each pings the server through its
# own TUN, and the server's replies find their router by destination; a
# packet bravo sends with alpha's source address is dropped (spoofed).
# Needs iperf3 and ping.
LAB_CI=1

NATIVE_S=${NATIVE_S:-5}
NATIVE_RATE=${NATIVE_RATE:-40} # Mbit/s per link and direction
NATIVE_MIN_X=${NATIVE_MIN_X:-1.2}

native_stop_iperf() {
	[ -f "$RUN/iperf3.pid" ] && kill "$(cat "$RUN/iperf3.pid")" 2>/dev/null
	rm -f "$RUN/iperf3.pid"
}

# native_iperf ARGS...: Mbit/s received by the other end, from iperf3's JSON.
native_iperf() {
	ip netns exec "$NS_CLI" iperf3 -c 10.79.0.1 -t "$NATIVE_S" -J "$@" >"$RUN/iperf3.json" 2>&1 || return 1
	python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print("%.1f" % (d["end"]["sum_received"]["bits_per_second"] / 1e6))' \
		"$RUN/iperf3.json"
}

native() {
	local fail=0 tool i v6="" loss up down min v extra=$CLIENT_EXTRA
	for tool in iperf3 ping; do
		command -v "$tool" >/dev/null || { echo "native: needs $tool"; return 1; }
	done
	NLINKS=2
	[ -d /proc/sys/net/ipv6 ] && v6=1
	CLIENT_EXTRA="tun_addr = 10.79.0.2/16${v6:+ fd00:79::2/64};split = on${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="tun_addr = 10.79.0.1/16${v6:+ fd00:79::1/64}${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap 'native_stop_iperf; teardown' EXIT
	trap 'native_stop_iperf; teardown; exit 130' INT TERM
	setup && start || return 1
	for i in 1 2; do
		ip netns exec "$NS_CLI" tc qdisc replace dev "l$i" root tbf rate "${NATIVE_RATE}mbit" burst 32kb latency 50ms || return 1
		ip netns exec "$NS_SRV" tc qdisc replace dev "s$i" root tbf rate "${NATIVE_RATE}mbit" burst 32kb latency 50ms || return 1
	done

	loss=$(ip netns exec "$NS_CLI" ping -c 20 -i 0.05 -W 1 10.79.0.1 | sed -n 's/.* \([0-9.]*\)% packet loss.*/\1/p')
	echo "   ping 10.79.0.1 through the TUN: ${loss:-?}% loss"
	[ "$loss" = 0 ] || { echo "FAIL: ping over IPv4 lost packets"; fail=1; }
	if [ -n "$v6" ]; then
		loss=$(ip netns exec "$NS_CLI" ping -6 -c 20 -i 0.05 -W 1 fd00:79::1 | sed -n 's/.* \([0-9.]*\)% packet loss.*/\1/p')
		echo "   ping fd00:79::1 through the TUN: ${loss:-?}% loss"
		[ "$loss" = 0 ] || { echo "FAIL: ping over IPv6 lost packets"; fail=1; }
	fi

	ip netns exec "$NS_SRV" iperf3 -s -B 10.79.0.1 -D -I "$RUN/iperf3.pid" || return 1
	sleep 0.3
	up=$(native_iperf) || { echo "FAIL: iperf3 up: $(tail -3 "$RUN/iperf3.json")"; fail=1; }
	down=$(native_iperf -R) || { echo "FAIL: iperf3 down: $(tail -3 "$RUN/iperf3.json")"; fail=1; }
	native_stop_iperf
	echo "   iperf3 TCP over 2 x ${NATIVE_RATE} Mbit/s, split on: up ${up:-?} Mbit/s, down ${down:-?} Mbit/s"
	min=$(python3 -c "print($NATIVE_MIN_X * $NATIVE_RATE)")
	for v in "$up" "$down"; do
		if [ -z "$v" ] || ! python3 -c "import sys; sys.exit(0 if $v > $min else 1)"; then
			echo "FAIL: ${v:-?} Mbit/s is not more than $min (one link carries $NATIVE_RATE)"
			fail=1
		fi
	done

	sleep 1.2 # the status files, once a second
	v=$(jget "$RUN/client.json" 'd["upload"]["split"]')
	echo "   client: upload split $v packets, not IP written to the TUN: $(jget "$RUN/client.json" 'd["download"]["not_ip"]')"
	[ "${v:-0}" -gt 0 ] || { echo "FAIL: the upload was not split"; fail=1; }
	v=$(jget "$RUN/server.json" '[s for s in d["sessions"] if s["newest"]][0]["download_split"]')
	echo "   server: download split $v packets, $(jget "$RUN/server.json" 'd["tun"]')"
	[ "${v:-0}" -gt 0 ] || { echo "FAIL: the download was not split"; fail=1; }
	v=$(jget "$RUN/server.json" 'sum(s["not_ip"] + s["spoofed"] for s in d["sessions"]) + d["tun"]["not_ip"] + d["tun"]["no_route"]')
	[ "$v" = 0 ] || { echo "FAIL: $v packets refused at the server's TUN"; fail=1; }
	v=$(jget "$RUN/client.json" 'd["download"]["not_ip"]')
	[ "$v" = 0 ] || { echo "FAIL: $v packets refused at the client's TUN"; fail=1; }

	stop
	[ "$fail" = 0 ] || { echo "--- client log"; cat "$RUN/client.log"; echo "--- server log"; cat "$RUN/server.log"; }
	teardown
	native_multi "$extra" || fail=1
	[ "$fail" = 0 ] && echo "native: ok" || echo "native: FAILED"
	return "$fail"
}

# native_ping TUN DST: % lost of 10 pings through TUN.
native_ping() {
	ip netns exec "$NS_CLI" ping -c 10 -i 0.05 -W 1 -I "$1" "$2" 2>/dev/null | sed -n 's/.* \([0-9.]*\)% packet loss.*/\1/p'
}

# native_multi EXTRA: the routers get EXTRA too (CLIENT_EXTRA as given).
native_multi() {
	local fail=0 name n loss v
	NLINKS=2
	CLIENT_EXTRA="" SERVER_EXTRA=""
	setup || return 1
	cat >"$RUN/server.conf" <<-EOF
		mode = server
		listen = *:59402
		tunnel = native
		tun_addr = 10.79.0.1/16
		status_file = $RUN/server.json
		status_interval_ms = 200
		[client alpha]
		key = $KEY
		tun_route = 10.79.0.2/32
		[client bravo]
		key = ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4Q=
		tun_route = 10.79.0.3/32
	EOF
	for name in alpha bravo; do
		n=$([ "$name" = alpha ] && echo 1 || echo 2)
		{
			printf 'mode = client\nkey = %s\nserver = 10.0.%s.2:59402\ninterfaces = l%s\n' \
				"$([ "$name" = alpha ] && echo "$KEY" || echo ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4Q=)" "$n" "$n"
			printf 'tunnel = native\ntun = cg%s\ntun_addr = 10.79.0.%s/16\n' "${name:0:1}" $((n + 1))
			printf 'status_file = %s/%s.json\nstatus_interval_ms = 200\n' "$RUN" "$name"
			[ -n "$1" ] && echo "$1" | tr ';' '\n'
		} >"$RUN/$name.conf"
	done
	ip netns exec "$NS_SRV" "$CENGARDE_BIN" -c "$RUN/server.conf" >"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	for name in alpha bravo; do
		ip netns exec "$NS_CLI" "$CENGARDE_BIN" -c "$RUN/$name.conf" >"$RUN/$name.log" 2>&1 &
		echo $! >"$RUN/$name.pid"
	done
	sleep 1.5
	for name in alpha bravo; do
		loss=$(native_ping "cg${name:0:1}" 10.79.0.1)
		echo "   $name: ping 10.79.0.1 through cg${name:0:1}: ${loss:-?}% loss"
		[ "$loss" = 0 ] || { echo "FAIL: $name's ping lost packets"; fail=1; }
	done
	# bravo's TUN, alpha's source: the server drops it.
	ip -n "$NS_CLI" route add 10.79.0.1/32 dev cgb src 10.79.0.2
	loss=$(ip netns exec "$NS_CLI" ping -c 5 -i 0.05 -W 1 10.79.0.1 2>/dev/null | sed -n 's/.* \([0-9.]*\)% packet loss.*/\1/p')
	sleep 0.5
	v=$(jget "$RUN/server.json" 'sum(s["spoofed"] for s in d["sessions"] if s["client"] == "bravo")')
	echo "   bravo with alpha's address: ${loss:-?}% loss, $v dropped as spoofed at the server"
	[ "$loss" = 100 ] && [ "${v:-0}" -ge 5 ] || { echo "FAIL: the server took bravo's packets with alpha's address"; fail=1; }
	for name in alpha bravo; do
		[ -f "$RUN/$name.pid" ] && kill "$(cat "$RUN/$name.pid")" 2>/dev/null
		rm -f "$RUN/$name.pid"
	done
	stop
	[ "$fail" = 0 ] || for name in alpha bravo server; do echo "--- $name log"; cat "$RUN/$name.log"; done
	teardown
	return "$fail"
}
