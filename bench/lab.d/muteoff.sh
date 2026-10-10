# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# Muting turned off by a reload: "bench/lab.sh muteoff".
#
# 2000 pps flow one way over 3 links. Link 3 gets a 500 ms queue (tbf at
# 5 Mbit/s, as in health) and is muted. A reload (SIGHUP) then sets
# mute_behind_ms = 0: the link carries again within MUTEOFF_MAX_MS (1000)
# although its queue is still there, says why in the log, and is not muted
# again while muting is off. A second reload takes the setting out: the link
# is muted again. Upload (the client mutes) and download (the server). The
# tunnel keeps its session and loses nothing.
LAB_CI=1

MUTEOFF_MAX_MS=${MUTEOFF_MAX_MS:-1000}

# muteoff_state DIR: the sending end's state of link 3, "muted" or "active".
muteoff_state() {
	if [ "$1" = up ]; then
		jget "$RUN/client.json" "[l['upload'] for l in d['links'] if l['id'] == 2][0]" 2>/dev/null
	else
		jget "$RUN/server.json" "[l['download'] for l in d['sessions'][0]['links'] if l['id'] == 2][0]" 2>/dev/null
	fi
}

# muteoff_wait DIR STATE SECONDS: 0 once link 3 is in STATE, 1 if it is not
# within SECONDS.
muteoff_wait() {
	local end=$(($(date +%s) + $3))
	while [ "$(date +%s)" -le "$end" ]; do
		[ "$(muteoff_state "$1")" = "$2" ] && return 0
		sleep 0.05
	done
	return 1
}

muteoff() {
	local fail=0 dir end ns dev pid sess traffic t0 ms sent uniq
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="sndbuf = 4194304;status_interval_ms = 100${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="status_interval_ms = 100${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	for dir in up down; do
		if [ "$dir" = up ]; then
			end=client ns=cli dev=l3
		else
			end=server ns=srv dev=s3
		fi
		setup && start || return 1
		pid=$(cat "$RUN/$end.pid")
		sess=$(jget "$RUN/client.json" 'd["session"]')
		"$dir" 2000 18 >"$RUN/traffic.txt" &
		traffic=$!
		sleep 1
		ip netns exec "$ns" tc qdisc replace dev "$dev" root tbf rate 5mbit burst 32kb latency 500ms
		if ! muteoff_wait "$dir" muted 8; then
			echo "FAIL: $dir: link 3 not muted with its queue"
			fail=1
		else
			sed -i '1a mute_behind_ms = 0' "$RUN/$end.conf" # global, before any [link]
			t0=$(date +%s%N)
			kill -HUP "$pid"
			if muteoff_wait "$dir" active 3; then
				ms=$((($(date +%s%N) - t0) / 1000000))
				echo "   $dir: link 3 active $ms ms after the reload, with its 5 Mbit/s queue still on"
				[ "$ms" -le "$MUTEOFF_MAX_MS" ] || { echo "FAIL: $dir: more than $MUTEOFF_MAX_MS ms"; fail=1; }
			else
				echo "FAIL: $dir: link 3 still muted 3 s after the reload that turned muting off"
				fail=1
			fi
			sleep 3
			[ "$(muteoff_state "$dir")" = active ] || { echo "FAIL: $dir: muted again with muting off"; fail=1; }
			sed -i '/^mute_behind_ms = 0$/d' "$RUN/$end.conf"
			kill -HUP "$pid"
			muteoff_wait "$dir" muted 8 || { echo "FAIL: $dir: not muted again once muting is back"; fail=1; }
		fi
		wait "$traffic"
		ip netns exec "$ns" tc qdisc del dev "$dev" root 2>/dev/null
		echo "   tunnel: $(cat "$RUN/traffic.txt")"
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 999)) ]; then
			echo "FAIL: $dir lost packets"
			fail=1
		fi
		[ "$(jget "$RUN/client.json" 'd["session"]')" = "$sess" ] || { echo "FAIL: $dir: the reloads changed the session"; fail=1; }
		grep -q "unmuted, muting is off" "$RUN/$end.log" || { echo "FAIL: $dir: the log does not say why"; fail=1; }
		if grep -q "unknown key" "$RUN/$end.log"; then
			echo "FAIL: $dir: a reload did not take the setting"
			fail=1
		fi
		grep -hE "muted|reload" "$RUN/$end.log" | sed 's/^/   /'
		stop
	done
	teardown
	if [ "$fail" = 0 ]; then
		echo "muteoff: ok"
	else
		echo "muteoff: FAILED"
	fi
	return "$fail"
}
