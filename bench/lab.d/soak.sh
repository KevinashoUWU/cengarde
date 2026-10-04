# shellcheck shell=bash disable=SC2034 # ENGINE and NLINKS are read by lab.sh
# Soak of the client's link_threads modes: "bench/lab.sh soak" (by hand;
# PR 3b, gate C0 with off, then on; docs/historias/011).
#
# For SOAK_S (3600) seconds per mode of SOAK_MODES ("off on"), traffic both
# ways in segments of SOAK_SEG (60) seconds, each at a rate and size taken
# in turn from 2000, 10000 and 20000 pps and 80, 400 and 1400 bytes, while:
# - every 2 minutes l3 changes its loss and delay (netem when the kernel has
#   it, else a tbf rate);
# - every 10 minutes l2 goes down for 10 s and comes back;
# - every 5 minutes the client reloads its configuration (ctl reload).
# Passes when every segment delivers 99.9 % of what was sent in each
# direction (one link at a time is lossy or down: the other two carry
# everything), the client never warns of a stalled thread nor of a socket
# it could not poll, every reload answers "ok", and the client's RSS after
# the first 5 minutes grows by less than SOAK_RSS_KB (1024) kB.
SOAK_S=${SOAK_S:-3600}
SOAK_MODES=${SOAK_MODES:-off on}
SOAK_SEG=${SOAK_SEG:-60}
SOAK_RSS_KB=${SOAK_RSS_KB:-1024}

soak_rss() { awk '/^VmRSS:/ { print $2 }' "/proc/$(cat "$RUN/client.pid")/status" 2>/dev/null || echo 0; }

# soak_events SECS: l3's loss and delay, l2 down and up, reloads.
soak_events() {
	local t=0 netem=1 i=0 out
	local -a shapes=("loss 2% delay 20ms" "delay 60ms 10ms" "loss 10%" "delay 5ms")
	local -a rates=(5mbit 20mbit 2mbit 50mbit)
	ip netns exec cli tc qdisc replace dev l3 root netem delay 1ms 2>/dev/null || netem=0
	while [ "$t" -lt "$1" ]; do
		sleep 10
		t=$((t + 10))
		if [ $((t % 120)) = 0 ]; then
			if [ "$netem" = 1 ]; then
				# shellcheck disable=SC2086 # the shape is several words
				ip netns exec cli tc qdisc replace dev l3 root netem ${shapes[i % 4]}
			else
				ip netns exec cli tc qdisc replace dev l3 root tbf rate "${rates[i % 4]}" burst 32kb limit 3mb
			fi
			i=$((i + 1))
		fi
		if [ $((t % 600)) = 0 ]; then
			ip -n cli link set l2 down
			sleep 10
			t=$((t + 10))
			ip -n cli link set l2 up
		fi
		if [ $((t % 300)) = 0 ]; then
			out=$("$CENGARDE_BIN" ctl -s "$RUN/client.sock" reload 2>&1)
			[ "$out" = ok ] || echo "reload said: $out" >>"$RUN/soak-events.txt"
		fi
	done
}

soak_mode() {
	local mode=$1 fail=0 seg=0 end rate size sent uniq gs gc ev rss0="" rss d from to
	local -a rates=(2000 10000 20000) sizes=(80 400 1400)
	CLIENT_EXTRA="link_threads = $mode;control_socket = $RUN/client.sock"
	setup && start || return 1
	: >"$RUN/soak-events.txt"
	soak_events "$SOAK_S" &
	ev=$!
	end=$((SECONDS + SOAK_S))
	while [ $((SECONDS + SOAK_SEG)) -le "$end" ]; do
		rate=${rates[seg % 3]} size=${sizes[seg / 3 % 3]}
		ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r "$rate" -s "$size" -d "$SOAK_SEG" -g 2 >"$RUN/srv.out" &
		gs=$!
		ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r "$rate" -s "$size" -d "$SOAK_SEG" \
			-g 2 >"$RUN/cli.out" &
		gc=$!
		wait "$gs" "$gc"
		seg=$((seg + 1))
		# down: sent by srv, received by cli; up: the other way.
		for d in down up; do
			from=$RUN/srv.out to=$RUN/cli.out
			[ "$d" = up ] && from=$RUN/cli.out to=$RUN/srv.out
			sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$from")
			uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$to")
			if [ -z "$sent" ] || [ -z "$uniq" ] || [ $((uniq * 1000)) -lt $((sent * 999)) ]; then
				echo "FAIL: $mode segment $seg ($rate pps, $size B) $d: sent ${sent:-?} delivered ${uniq:-?}"
				fail=1
			fi
		done
		rss=$(soak_rss)
		[ -z "$rss0" ] && [ "$SECONDS" -ge $((end - SOAK_S + 300)) ] && rss0=$rss
	done
	kill "$ev" 2>/dev/null
	wait "$ev" 2>/dev/null
	rss=$(soak_rss)
	echo "   $mode: $seg segments, RSS ${rss0:-?} kB after 5 min, $rss kB at the end"
	if [ -n "$rss0" ] && [ $((rss - rss0)) -gt "$SOAK_RSS_KB" ]; then
		echo "FAIL: $mode: RSS grew by $((rss - rss0)) kB"
		fail=1
	fi
	if grep -E "has not run for|could not be polled" "$RUN/client.log"; then
		echo "FAIL: $mode: a stalled thread or a socket it could not poll"
		fail=1
	fi
	if [ -s "$RUN/soak-events.txt" ]; then
		cat "$RUN/soak-events.txt"
		fail=1
	fi
	stop
	return "$fail"
}

soak() {
	local fail=0 mode
	ENGINE=c NLINKS=3
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	for mode in $SOAK_MODES; do
		soak_mode "$mode" || fail=1
	done
	teardown
	[ "$fail" = 0 ] && echo "soak: ok" || echo "soak: FAILED"
	return "$fail"
}
