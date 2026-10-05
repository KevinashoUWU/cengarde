# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# A stalled link thread: "bench/lab.sh mtstall" (PR 3b, docs/historias/011).
#
# With link_threads = on each link has a thread that reads its socket, so a
# thread that does not get the CPU fills only its own link's socket: the
# copies of the other links reach WireGuard on time. Here, 2000 pps flow in
# both ways for MTSTALL_S (12) seconds while a SCHED_FIFO busy loop takes
# one CPU 300 ms of every 2 s; the thread that reads l3, found by name with
# "cengarde ctl threads", is moved onto that CPU and everything else (the
# other threads, the server, the fake WireGuards) kept off it. Each stall
# of 300 ms holds about 600 of l3's datagrams, more than its socket keeps
# (rcvbuf = 256 KiB, doubled by the kernel), so l3 drops.
#
# - link_threads = on: no tunnel loss (99.9 %, as the other scenarios) in
#   either direction; no drops in the sockets of l1 and l2; at most
#   MTSTALL_LATE_PM (5) per mille of the tunnel's packets 50 ms late or
#   more (the VM pauses for up to 38 ms on its own; bench/jitter.c, which
#   marks those pauses, comes with the server's lanes).
# - off and legacy: the one loop reads every link, so the busy loop takes
#   the loop itself (the main thread): reported, not judged; the late
#   packets come close to the stall.
#
# A link_threads in CLIENT_EXTRA (each lab job of engine.yml sets one) runs
# that mode alone, the caller's other settings kept; without one, all three
# modes run, each in a subshell of its own with every CPU.
#
# Needs chrt and taskset (util-linux) and 2 CPUs or more.
LAB_CI=1

MTSTALL_S=${MTSTALL_S:-12}
MTSTALL_LATE_PM=${MTSTALL_LATE_PM:-5}

# mt_hog CPU SECS: a SCHED_FIFO 99 busy loop on CPU, 300 ms of every 2 s.
mt_hog() {
	local end=$((SECONDS + $2))
	while [ "$SECONDS" -lt "$end" ]; do
		sleep 1.7
		timeout 0.3 chrt -f 99 taskset -c "$1" sh -c 'while :; do :; done'
	done
}

# mt_tid NAME: the TID of a thread of the client, from "cengarde ctl threads".
mt_tid() { "$CENGARDE_BIN" ctl -s "$RUN/client.sock" threads | awk -v n="$1" '$1 == n { print $2 }'; }

# mt_run MODE: one run; prints one line per direction and the drops per
# link, and returns 1 when MODE is on and a check failed.
mt_run() {
	local mode=$1 cpu rest tid t fail=0 down up d1 d2 d3 gs gc
	cpu=$(($(nproc) - 1))
	rest=0-$((cpu - 1))
	# The caller's settings first: the last value of a key wins.
	CLIENT_EXTRA="${mt_extra:+$mt_extra;}link_threads = $mode;rcvbuf = 262144;control_socket = $RUN/client.sock"
	setup && start || return 1
	# Everything but the stalled thread stays off the busy CPU.
	taskset -a -p -c "$rest" "$(cat "$RUN/server.pid")" >/dev/null
	taskset -a -p -c "$rest" "$(cat "$RUN/client.pid")" >/dev/null
	taskset -p -c "$rest" "$BASHPID" >/dev/null # this subshell: the fake WireGuards inherit it
	t=cg-hub
	[ "$mode" = on ] && t=cg-l3
	tid=$(mt_tid "$t")
	if [ -z "$tid" ]; then
		echo "FAIL: $mode: no thread $t in ctl threads"
		"$CENGARDE_BIN" ctl -s "$RUN/client.sock" threads
		stop
		return 1
	fi
	taskset -p -c "$cpu" "$tid" >/dev/null
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r 2000 -s "$SIZE" -d "$MTSTALL_S" -g 2 >"$RUN/srv.out" &
	gs=$!
	ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r 2000 -s "$SIZE" -d "$MTSTALL_S" -g 2 \
		>"$RUN/cli.out" &
	gc=$!
	sleep 1
	mt_hog "$cpu" $((MTSTALL_S - 2))
	wait "$gs" "$gc"
	"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/client-now.json"
	read -r d1 d2 d3 < <(jget "$RUN/client-now.json" '" ".join(str(l["socket_drops"]) for l in d["links"])')
	# down: sent by srv, received by cli; up: the other way.
	down=$(mt_dir "$RUN/srv.out" "$RUN/cli.out")
	up=$(mt_dir "$RUN/cli.out" "$RUN/srv.out")
	echo "   $mode down: $down"
	echo "   $mode up:   $up"
	echo "   $mode socket drops: l1=$d1 l2=$d2 l3=$d3; $t (TID $tid) on CPU $cpu with the busy loop"
	stop
	[ "$mode" = on ] || return 0
	for t in "down:$down" "up:$up"; do
		case ${t#*:} in
		*" ok") ;;
		*) echo "FAIL: on: ${t%%:*} lost packets or had more than $MTSTALL_LATE_PM per mille late"; fail=1 ;;
		esac
	done
	[ "$d1" = 0 ] && [ "$d2" = 0 ] || { echo "FAIL: on: l1 or l2 dropped in its socket"; fail=1; }
	return "$fail"
}

# mt_dir SENDER RECEIVER: "sent N uniq N late N (x per mille) ok|bad".
mt_dir() {
	local sent uniq late
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$1")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$2")
	late=$(sed -n 's/.*over50ms=\([0-9]*\).*/\1/p' "$2")
	if [ -z "$sent" ] || [ -z "$uniq" ] || [ -z "$late" ]; then
		echo "no result"
		return
	fi
	echo "sent $sent uniq $uniq late $late ($((late * 1000 / (sent > 0 ? sent : 1))) per mille) $(
		[ $((uniq * 1000)) -ge $((sent * 999)) ] && [ $((late * 1000)) -le $((sent * MTSTALL_LATE_PM)) ] &&
			echo ok || echo bad)"
}

mtstall() {
	local fail=0 mode modes="on off legacy" mt_extra=${CLIENT_EXTRA:-}
	ENGINE=c NLINKS=3
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	if [ "$(nproc)" -lt 2 ] || ! command -v chrt >/dev/null || ! command -v taskset >/dev/null; then
		echo "FAIL: needs 2 CPUs or more, chrt and taskset"
		echo "mtstall: FAILED"
		return 1
	fi
	# A link_threads in CLIENT_EXTRA runs that mode alone. Each pass in a
	# subshell: it narrows its own CPUs with taskset, and nproc follows them.
	mode=$(sed -n 's/.*link_threads *= *\([a-z]*\).*/\1/p' <<<"$mt_extra")
	[ -n "$mode" ] && modes=$mode
	for mode in $modes; do
		(mt_run "$mode") || fail=1
	done
	teardown
	[ "$fail" = 0 ] && echo "mtstall: ok" || echo "mtstall: FAILED"
	return "$fail"
}
