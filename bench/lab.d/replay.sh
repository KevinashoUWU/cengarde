# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# Cookies and replays, protocol 4: "bench/lab.sh replay".
#
# bench/cgprobe plays a second router with the lab's key, from addresses of
# the cli namespace, next to the real client:
#  1. a probe for a session the server does not have gets a HELLO, and
#     no session; the same probe with the HELLO's cookie gets a reply and
#     a session, with IP pass on, as it asked (saved as p.bin);
#  2. p.bin sent again from another address is a duplicate: no answer, the
#     path stays where it was;
#  3. a fresh probe with that cookie from another address gets a HELLO for
#     the new address and does not move the path; with the new cookie it
#     does (a NAT that changed the router's port);
#  4. DATA for a session the server does not have, a probe with another
#     client's hint, and a probe under another key: no answer, counted;
#  5. the server restarts: p.bin from any address, its own included, gets
#     only a HELLO (the cookie keys are new): no session, and IP pass is not
#     written;
#  6. the real client, all along: its download goes on before the restart,
#     and after it within RESTART_S (5) seconds, its windows never reset.
LAB_CI=1

# replay_probe BIND ARGS...: cgprobe from BIND (10.0.N.1:PORT) in the cli
# namespace, to the server's address on the same link (10.0.N.2).
replay_probe() {
	local bind=$1 srv
	shift
	srv=${bind%.1:*}.2:59402
	ip netns exec cli "$BIN/cgprobe" -k "$KEY" -s "$srv" -b "$bind" "$@"
}

# replay_srv EXPR: a value of the server's status (its file is fresh every 200 ms).
replay_srv() {
	sleep 0.4
	jget "$RUN/server.json" "$1"
}

# replay_eq WHAT GOT WANT: GOT is exactly WANT, or a FAIL line.
replay_eq() {
	if [ "$2" = "$3" ]; then
		echo "   $1: $2"
	else
		echo "FAIL: $1: got '$2', expected '$3'"
		return 1
	fi
}

# replay_expect WHAT GOT PATTERN: GOT matches the glob PATTERN, or a FAIL line.
replay_expect() {
	local what=$1 got=$2 want=$3
	# shellcheck disable=SC2254 # a glob on purpose
	case $got in
	$want) echo "   $what: $got" ;;
	*)
		echo "FAIL: $what: got '$got', expected '$want'"
		return 1
		;;
	esac
}

replay_server_restart() {
	kill "$(cat "$RUN/server.pid")" 2>/dev/null
	sleep 0.5
	rm -f "$RUN/passthrough"
	ip netns exec srv "$CENGARDE_BIN" -c "$RUN/server.conf" >>"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	sleep 0.5
}

replay() {
	local fail=0 out c1 c2 S=c0ffee01 i loss hint b
	ENGINE=c NLINKS=2
	SERVER_EXTRA="status_interval_ms = 200;passthrough_file = $RUN/passthrough${SERVER_EXTRA:+;$SERVER_EXTRA}"
	CLIENT_EXTRA="status_interval_ms = 200${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	if [ ! -x "$BIN/cgprobe" ]; then
		echo "FAIL: $BIN/cgprobe missing: bench/lab.sh build"
		echo "replay: FAILED"
		return 1
	fi
	setup && start || return 1
	sess() { replay_srv "[s['id'] for s in d['sessions']]"; }

	echo "   1. a new session needs the cookie of a HELLO"
	out=$(replay_probe 10.0.1.1:47001 -q 99 probe "$S" 0)
	replay_expect "probe without a cookie" "$out" "hello *" || fail=1
	c1=${out#hello }
	case $(sess) in *"$S"*) echo "FAIL: a session without a cookie"; fail=1 ;; esac
	out=$(replay_probe 10.0.1.1:47001 -q 100 -P -T 1000 -w "$RUN/p.bin" probe "$S" 0 "$c1")
	replay_expect "the same probe with the cookie" "$out" "reply" || fail=1
	case $(sess) in *"$S"*) ;; *) echo "FAIL: no session $S"; fail=1 ;; esac
	replay_expect "IP pass, as the probe asked" "$(sleep 0.3; cat "$RUN/passthrough" 2>/dev/null)" "on" || fail=1

	echo "   2. the same datagram again, from elsewhere"
	out=$(replay_probe 10.0.2.1:47002 raw "$RUN/p.bin")
	replay_expect "answer" "$out" "none" || fail=1
	replay_eq "link 0 of $S" \
		"$(replay_srv "[l['address'] for s in d['sessions'] if s['id'] == '$S' for l in s['links']]")" \
		"['10.0.1.1:47001']" || fail=1

	echo "   3. a fresh probe from another address"
	out=$(replay_probe 10.0.2.1:47002 -q 101 probe "$S" 0 "$c1")
	replay_expect "with the old address's cookie" "$out" "hello *" || fail=1
	c2=${out#hello }
	replay_eq "link 0 of $S" \
		"$(replay_srv "[l['address'] for s in d['sessions'] if s['id'] == '$S' for l in s['links']]")" \
		"['10.0.1.1:47001']" || fail=1
	out=$(replay_probe 10.0.2.1:47002 -q 102 probe "$S" 0 "$c2")
	replay_expect "with its own cookie" "$out" "reply" || fail=1
	replay_eq "link 0 of $S moved" \
		"$(replay_srv "[(l['address'], l['moves']) for s in d['sessions'] if s['id'] == '$S' for l in s['links']]")" \
		"[('10.0.2.1:47002', 1)]" || fail=1

	echo "   4. no answer, counted"
	replay_expect "DATA, no session" "$(replay_probe 10.0.1.1:47004 data c0ffee02 0)" "none" || fail=1
	replay_expect "rx.no_session" "$(replay_srv "d['rx']['no_session']")" "[1-9]*" || fail=1
	hint=$("$BIN/cgprobe" -k "$KEY" hint)
	out=$(replay_probe 10.0.1.1:47004 -H $((hint ^ 1)) probe c0ffee03 0)
	replay_expect "another client's hint" "$out" "none" || fail=1
	replay_expect "rx.other_hint" "$(replay_srv "d['rx']['other_hint']")" "[1-9]*" || fail=1
	out=$(ip netns exec cli "$BIN/cgprobe" -k "$(printf '%032d' 7 | base64 | head -c 44)" -H "$hint" \
		-s 10.0.1.2:59402 -b 10.0.1.1:47004 probe c0ffee03 0)
	replay_expect "another key, our hint" "$out" "none" || fail=1
	replay_expect "rx.auth_failures" "$(replay_srv "d['rx']['auth_failures']")" "[1-9]*" || fail=1

	echo "   6. the real client's download, before the restart"
	loss=$(down 1000 2 | sed -n 's/.*loss= *\([0-9.]*\)%.*/\1/p')
	replay_expect "loss %" "$loss" "0.00" || fail=1

	echo "   5. the server restarts; the datagram again"
	replay_server_restart
	for b in 10.0.2.1:47003 10.0.1.1:47001; do
		out=$(replay_probe "$b" raw "$RUN/p.bin")
		replay_expect "from $b" "$out" "hello *" || fail=1
	done
	case $(sess) in *"$S"*) echo "FAIL: the replay took session $S up"; fail=1 ;; esac
	[ -e "$RUN/passthrough" ] && { echo "FAIL: the replay wrote IP pass: $(cat "$RUN/passthrough")"; fail=1; }

	echo "   6. the real client after the restart"
	for i in $(seq 1 50); do
		[ "$(jget "$RUN/server.json" "len([l for s in d['sessions'] for l in s['links'] if l['state'] == 'live'])" 2>/dev/null)" = "$NLINKS" ] && break
		sleep 0.1
	done
	echo "   its links live after $((i / 10)).$((i % 10)) s"
	[ "$i" -lt 50 ] || { echo "FAIL: the client's links are not live 5 s after the restart"; fail=1; }
	loss=$(down 1000 2 | sed -n 's/.*loss= *\([0-9.]*\)%.*/\1/p')
	replay_expect "loss %" "$loss" "0.00" || fail=1
	replay_expect "client HELLOs" "$(jget "$RUN/client.json" "d['download']['hellos']")" "[1-9]*" || fail=1
	replay_expect "client too_old" "$(jget "$RUN/client.json" "d['download']['too_old']")" "0" || fail=1

	teardown
	if [ "$fail" = 0 ]; then
		echo "replay: ok"
	else
		echo "replay: FAILED"
	fi
	return "$fail"
}
