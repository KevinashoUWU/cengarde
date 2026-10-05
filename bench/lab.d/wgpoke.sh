# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# A router whose clock went back: "bench/lab.sh wgpoke".
#
# WireGuard on the VPS ignores a handshake initiation older than the last
# one it took. A router without a battery-backed clock that restarts comes
# back behind, and its tunnel stays down until its clock catches up
# (docs/historias/002). The server pokes WireGuard into starting a handshake
# of its own (engine/src/wgwatch.h). bench/fakewg.py plays both WireGuards:
# the VPS's one ignores every initiation and starts one when poked, the
# router's one answers it.
#
# 1. Data flows: WireGuard learns the client's session; nothing is poked.
# 2. The client restarts and knocks: within KNOCK_S seconds (6) WireGuard is
#    poked, its initiation (to the old session) goes down the new one, the
#    router answers, and the new session is WireGuard's endpoint.
# 3. Again, but WireGuard starts its handshake 7 s after the poke (as it does
#    15 s later when its session is still valid): by then the old session
#    timed out, and the knocking one took over its port, so the initiation
#    still reaches the router, without a redirect.
# 4. The client stops for longer than session_timeout_ms (5 s here) and
#    knocks again: its new session takes the old one's port, so WireGuard's
#    initiation reaches it directly.
# 5. With wireguard_poke = none (a reload), a client that knocks after a
#    restart is not poked for.
LAB_CI=1

KNOCK_S=${KNOCK_S:-6}

# wgpoke_wait FILE FROM PATTERN SECS: a line after line FROM of FILE matches
# within SECS seconds.
wgpoke_wait() {
	local i
	for i in $(seq 1 $(($4 * 10))); do
		tail -n +$(($2 + 1)) "$1" | grep -q "$3" && return 0
		sleep 0.1
	done
	return 1
}

# wgpoke_lines FILE: how many lines it has now.
wgpoke_lines() { wc -l <"$1"; }

# wgpoke_router MODE: the router's fake WireGuard, started again in MODE.
wgpoke_router() {
	pkill -f "$LAB/fakewg.py router" 2>/dev/null
	sleep 0.2
	[ "$1" = stop ] && return
	ip netns exec cli python3 "$LAB/fakewg.py" router 127.0.0.1:50000 127.0.0.1:59401 "$1" >>"$RUN/router-wg.log" 2>&1 &
}

# wgpoke_client stop|start|restart: the client engine (a new session each start).
wgpoke_client() {
	local pid i
	if [ "$1" != start ]; then
		pid=$(cat "$RUN/client.pid")
		kill "$pid" 2>/dev/null
		for i in $(seq 1 30); do
			kill -0 "$pid" 2>/dev/null || break
			sleep 0.1
		done
	fi
	[ "$1" = stop ] && return
	ip netns exec cli "$CENGARDE_BIN" -c "$RUN/client.conf" >>"$RUN/client.log" 2>&1 &
	echo $! >"$RUN/client.pid"
}

# wgpoke_server EXPR: a value of the server's status file, refreshed.
wgpoke_server() {
	sleep 1.2 # status_interval_ms
	jget "$RUN/server.json" "$1" 2>/dev/null
}

wgpoke() {
	local fail=0 v r n rn port_b='' ports='' delay
	ENGINE=c NLINKS=3
	SERVER_EXTRA="wireguard_poke = 127.0.0.1:59309;session_timeout_ms = 5000${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap 'pkill -f "$LAB/fakewg.py"; teardown' EXIT
	trap 'pkill -f "$LAB/fakewg.py"; teardown; exit 130' INT TERM
	if ! { setup && start; }; then
		echo "FAIL: the lab did not start"
		echo "wgpoke: FAILED"
		return 1
	fi
	v=$RUN/vps-wg.log r=$RUN/router-wg.log delay=$RUN/vps-wg.delay
	: >"$v"
	: >"$r"
	echo 0 >"$delay"
	ip netns exec srv python3 "$LAB/fakewg.py" vps 127.0.0.1:59301 127.0.0.1:59309 "$delay" >>"$v" 2>&1 &

	echo "   1. data flows"
	wgpoke_router data
	if ! wgpoke_wait "$v" 0 '^endpoint' 5; then
		echo "FAIL: no data reached WireGuard"
		fail=1
	fi
	sleep 2
	grep -q '^poked' "$v" && { echo "FAIL: WireGuard was poked while the data flowed"; fail=1; }

	echo "   2. the client restarts and knocks"
	n=$(wgpoke_lines "$v")
	wgpoke_client restart
	wgpoke_router knock
	if wgpoke_wait "$r" 0 '^got-initiation' "$KNOCK_S"; then
		echo "   WireGuard's initiation reached the router: $(tail -n +$((n + 1)) "$v" | grep -c '^ignored') initiations ignored before"
	else
		echo "FAIL: no initiation from WireGuard reached the router in $KNOCK_S s"
		fail=1
	fi
	tail -n +$((n + 1)) "$v" | grep -q '^poked' || { echo "FAIL: WireGuard was not poked"; fail=1; }
	[ "$(wgpoke_server 'd["wireguard"]["redirected_handshakes"]')" -ge 1 ] 2>/dev/null ||
		{ echo "FAIL: the initiation did not go down the new session (redirected_handshakes)"; fail=1; }
	if wgpoke_wait "$v" "$n" '^endpoint' 3; then
		port_b=$(tail -n +$((n + 1)) "$v" | sed -n 's/^endpoint //p' | tail -1)
		echo "   the router's response made the new session WireGuard's endpoint (port $port_b)"
	else
		echo "FAIL: WireGuard's endpoint did not move to the new session"
		fail=1
	fi

	echo "   3. the client restarts and knocks; WireGuard answers the poke 7 s late"
	echo 7 >"$delay"
	rn=$(wgpoke_lines "$r")
	wgpoke_client restart
	wgpoke_router knock
	if wgpoke_wait "$r" "$rn" '^got-initiation' 14; then
		echo "   WireGuard's initiation reached the router after the old session closed"
	else
		echo "FAIL: no initiation from WireGuard reached the router in 14 s"
		fail=1
	fi
	grep -q 'takes over the port' "$RUN/server.log" || { echo "FAIL: the new session did not take over the port"; fail=1; }
	[ "$(wgpoke_server 'd["wireguard"]["redirected_handshakes"]')" = 1 ] ||
		{ echo "FAIL: a redirect where none was needed"; fail=1; }
	echo 0 >"$delay"
	sleep 1

	echo "   4. the client stops for 8 s (session_timeout_ms 5 s), then knocks"
	wgpoke_router stop
	wgpoke_client stop
	sleep 8
	[ "$(wgpoke_server 'len(d["sessions"])')" = 0 ] || { echo "FAIL: the old session is still there"; fail=1; }
	n=$(wgpoke_lines "$v")
	rn=$(wgpoke_lines "$r")
	wgpoke_client start
	wgpoke_router knock
	if wgpoke_wait "$r" "$rn" '^got-initiation' "$KNOCK_S"; then
		echo "   WireGuard's initiation reached the router again"
	else
		echo "FAIL: no initiation from WireGuard reached the router in $KNOCK_S s"
		fail=1
	fi
	ports=$(tail -n +$((n + 1)) "$v" | sed -n 's/^ignored //p' | sort -u | tr '\n' ' ')
	if [ -n "$port_b" ] && [ "${ports% }" = "$port_b" ]; then
		echo "   the new session took the old one's port ($port_b)"
	else
		echo "FAIL: the new session knocked from port(s) ${ports:-none}, not $port_b"
		fail=1
	fi
	[ "$(wgpoke_server 'd["wireguard"]["redirected_handshakes"]')" = 1 ] ||
		{ echo "FAIL: a redirect where none was needed"; fail=1; }

	echo "   5. wireguard_poke = none"
	sed -i 's/^wireguard_poke = .*/wireguard_poke = none/' "$RUN/server.conf"
	kill -HUP "$(cat "$RUN/server.pid")"
	sleep 1
	n=$(wgpoke_lines "$v")
	wgpoke_client restart
	wgpoke_router knock
	sleep 4
	tail -n +$((n + 1)) "$v" | grep -q '^poked' && { echo "FAIL: WireGuard was poked with wireguard_poke = none"; fail=1; }
	tail -n +$((n + 1)) "$v" | grep -q '^ignored' || { echo "FAIL: the client did not knock"; fail=1; }

	echo "   server: pokes $(wgpoke_server 'd["wireguard"]["pokes"]'), $(grep -c 'poking it' "$RUN/server.log") logged"
	pkill -f "$LAB/fakewg.py"
	stop
	teardown
	[ "$fail" = 0 ] && echo "wgpoke: ok" || echo "wgpoke: FAILED"
	return "$fail"
}
