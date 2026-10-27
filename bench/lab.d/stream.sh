# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The simulated broadcast end to end: "bench/lab.sh stream" (bench/stream/).
#
# What the owner does from the field, in the bonding lab's namespaces: a
# sender in cli (the PC on the router's LAN) sends a test picture through
# cengarde's tunnel (bond.sh's lab: netem per link) to a receiver in
# srv (the VPS), over RTMP (TCP) and SRT (UDP). send.sh and recv.sh write their
# run directories, cengarde-rec records the client's status file at the same
# time, bench/fieldrec.py --export turns that into CSVs, and report.py lines
# all of it up by wall clock and prints what the stream saw (stalls, TCP
# collapses, SRT statistics) next to what the links did (outages, mutes).
#
# libx264 at a modest size and bitrate, so that it fits a CI runner or a
# container without a GPU. Cases are bond.sh's (equal, linkdeath, starlink15,
# ltespike...) plus "blackout": every link cut for 2 s, a third of the way in,
# which no redundancy can hide, so the kit has a stall to find. The scenario
# judges nothing about quality: it fails when the plumbing does (no stream, no
# packets at the receiver, report.py failing) and, for blackout, when the
# receiver does not see a stall of a second or more.
#
# Knobs: STREAM_CASES ("equal linkdeath blackout"), STREAM_PROTOS ("rtmp srt"),
# STREAM_S (seconds per run, 30), STREAM_SIZE (640x360), STREAM_KBPS (1500),
# STREAM_ENC (libx264), STREAM_KEEP (a directory to copy the run directories
# to), STREAM_QUIET (1: only a line per run, not the whole report). Needs
# ffmpeg (with libsrt for SRT), ss, python3 and netem like bond; bond.sh's
# note on BOND_NEED_NETEM applies. Not in "ci": it takes about a
# minute per case and protocol.
LAB_CI=0

STREAM_CASES=${STREAM_CASES:-equal linkdeath blackout}
STREAM_PROTOS=${STREAM_PROTOS:-rtmp srt}
STREAM_S=${STREAM_S:-30}
STREAM_SIZE=${STREAM_SIZE:-640x360}
STREAM_KBPS=${STREAM_KBPS:-1500}
STREAM_ENC=${STREAM_ENC:-libx264}
STREAM_KEEP=${STREAM_KEEP:-}
STREAM_QUIET=${STREAM_QUIET:-0}

# stream_shape CASE: the links of a case ("blackout" is "equal" with an event)
stream_shape() {
	case $1 in
	blackout) bond_shape equal ;;
	*) bond_shape "$1" ;;
	esac
}

# stream_events CASE SECS: what changes during the run, in the background
stream_events() {
	local c=$1 s=$2 n
	if [ "$c" = blackout ]; then
		bond_netem_ok || return 0
		sleep "$(awk -v s="$s" 'BEGIN { print s / 3 }')"
		for n in $(seq 1 "$(bond_nlinks equal)"); do bond_link "$n" 25 50 100 100; done
		sleep 2
		for n in $(seq 1 "$(bond_nlinks equal)"); do bond_link "$n" 25 50; done
		return 0
	fi
	bond_events "$c" "$s"
}

# stream_wait PID SECS: wait for a process to end, or stop it after SECS
stream_wait() {
	local pid=$1 i
	for i in $(seq 1 $(($2 * 5))); do
		kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; return 0; }
		sleep 0.2
	done
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	return 1
}

# stream_case CASE PROTO: one run in a fresh lab; the run directories go to $OUT
stream_case() {
	local c=$1 proto=$2 s=$STREAM_S dir=$RUN/stream/$1-$2 key=$RUN/stream.key
	local rec rx ev snd_dir rx_dir rc=0 dest port rxopts=() snopts=() summary
	case $c in starlink15*) [ "$s" -lt 31 ] && s=31 ;; esac
	NLINKS=$(bond_nlinks "$c")
	[ "$c" = blackout ] && NLINKS=2
	CLIENT_EXTRA="status_interval_ms = 250;probe_idle_ms = 100${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	rm -rf "$dir"
	mkdir -p "$dir"
	STREAM_PIDS=
	trap 'kill $STREAM_PIDS 2>/dev/null' EXIT # the background jobs of this run, if it is cut short
	setup && start || return 1
	if [ "$BOND_NEED_NETEM" = 1 ] && ! bond_netem_ok; then
		echo "FAIL: no netem (BOND_NEED_NETEM=1)"
		return 1
	fi
	stream_shape "$c" || return 1
	bond_tunnel || return 1
	# The key is the RTMP stream key and the SRT passphrase (10 characters or more).
	(umask 077 && echo "labstreamkey-0123456789" >"$key")
	if [ "$proto" = rtmp ]; then
		port=1935
		dest="rtmp://10.79.0.1:$port/live"
	else
		port=9000
		dest="srt://10.79.0.1:$port"
		rxopts=(-p srt -l 200)
		snopts=(-l 200)
		if command -v srt-live-transmit >/dev/null; then # SRT statistics at both ends
			rxopts+=(-S)
			snopts+=(-S)
		fi
	fi
	# What the engine records meanwhile, the way the router does it.
	sh "$LAB/../openwrt/cengarde/files/cengarde-rec" -s "$RUN/client.json" -d "$dir/rec" -r 3600 -p 0.1 &
	rec=$!
	STREAM_PIDS="$rec"
	ip netns exec "$NS_SRV" "$LAB/stream/recv.sh" -1 -b 10.79.0.1 -P "$port" -k "$key" -o "$dir" -n "$c-$proto" "${rxopts[@]}" >"$dir/recv.out" 2>&1 &
	rx=$!
	STREAM_PIDS="$STREAM_PIDS $rx"
	local i
	for i in $(seq 1 50); do
		grep -q "listening" "$dir/recv.out" 2>/dev/null && break
		sleep 0.1
	done
	sleep 0.5
	stream_events "$c" "$s" &
	ev=$!
	STREAM_PIDS="$STREAM_PIDS $ev"
	ip netns exec "$NS_CLI" "$LAB/stream/send.sh" -e "$STREAM_ENC" -s "$STREAM_SIZE" -b "${STREAM_KBPS}k" -d "$s" -k "$key" -o "$dir" \
		-n "$c-$proto" -c "$RUN/client.json" "${snopts[@]}" "$dest" >"$dir/send.out" 2>&1 || {
		echo "FAIL: $c $proto: send.sh failed: $(tail -n 3 "$dir/send.out" | tr '\n' ' ')"
		rc=1
	}
	wait "$ev"
	stream_wait "$rx" 25 || { echo "FAIL: $c $proto: recv.sh did not end with the stream"; rc=1; }
	sleep 0.5
	kill -TERM "$rec" 2>/dev/null
	wait "$rec" 2>/dev/null
	bond_untunnel
	teardown
	snd_dir=
	rx_dir=
	for i in "$dir"/*-"$c-$proto"; do # the sender's directory and the receiver's (-rx-)
		case $i in *-rx-*) rx_dir=$i ;; *) snd_dir=$i ;; esac
	done
	if [ -z "$snd_dir" ] || [ -z "$rx_dir" ]; then
		echo "FAIL: $c $proto: no run directory (sender '$snd_dir', receiver '$rx_dir')"
		return 1
	fi
	python3 "$LAB/fieldrec.py" --export "$dir/field" "$dir/rec" >"$dir/fieldrec.md" 2>&1 || echo "   note: fieldrec.py could not read the recording ($(tail -n 1 "$dir/fieldrec.md"))"
	local fr=()
	[ -f "$dir/field/events.csv" ] && fr=(--fieldrec "$dir/field")
	python3 "$LAB/stream/report.py" "$snd_dir" --rx "$rx_dir" "${fr[@]}" --json "$dir/report.json" --no-plots >"$dir/report.txt" 2>"$dir/report.err" || {
		echo "FAIL: $c $proto: report.py: $(tail -n 2 "$dir/report.err" | tr '\n' ' ')"
		return 1
	}
	if [ "$STREAM_QUIET" = 1 ]; then
		:
	else
		sed 's/^/   /' "$dir/report.txt"
	fi
	summary=$(python3 - "$dir/report.json" "$c" "$proto" "$s" "$STREAM_KBPS" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
case, proto, secs, kbps = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
rx, tc, sr = r["receiver"], r["tcp"], r["srt"]
fr = r.get("fieldrec") or {}
ev = {}
for row in fr.get("rows", []):
    if row["link"] == "all":
        ev[row["kind"]] = row["events"]
bits = ["video %.2f Mbit/s" % (rx.get("video_mbps") or 0), "stalls %d (longest %s ms)" % (rx.get("stalls", 0), "%.0f" % rx["stall_max_ms"] if rx.get("stall_max_ms") else "-")]
if tc.get("available"):
    bits.append("tcp retrans %s (%.2f %%), collapses %d, RTT over min p95 %.0f ms" % (tc.get("retrans_segments"), tc.get("retrans_pct") or 0, tc.get("collapses", 0), tc.get("rtt_infl_p95_ms") or 0))
elif sr.get("available"):
    bits.append("srt lost %s, retrans %s, dropped %s" % (sr.get("pktSndLoss"), sr.get("pktRetrans"), sr.get("pktSndDrop")))
bits.append("link events %s" % (", ".join("%s %d" % (k, v) for k, v in sorted(ev.items())) or "none"))
print("   %-10s %-4s %s" % (case, proto, "; ".join(bits)))
# the plumbing: packets at the receiver, as many as a stream of this length has (a lot of slack)
ok = rx.get("video_packets", 0) >= secs * 30 * 0.5
if case == "blackout":
    ok = ok and (rx.get("stall_max_ms") or 0) >= 1000
sys.exit(0 if ok else 1)
EOF
	) || {
		echo "$summary"
		if [ "$c" = blackout ]; then
			echo "FAIL: $c $proto: the receiver should have seen a stall of 1 s or more (and at least half the packets)"
		else
			echo "FAIL: $c $proto: fewer than half of the video packets reached the receiver"
		fi
		return 1
	}
	echo "$summary"
	return "$rc"
}

stream() {
	local c p fail=0 failed="" tool
	for tool in ffmpeg ss ping python3; do
		command -v "$tool" >/dev/null || { echo "stream: needs $tool"; return 1; }
	done
	if ! ffmpeg -hide_banner -protocols 2>/dev/null | grep -qw srt; then
		echo "   note: this ffmpeg has no libsrt: no srt runs"
		STREAM_PROTOS=${STREAM_PROTOS//srt/}
	fi
	[ -n "${STREAM_PROTOS// /}" ] || { echo "FAIL: nothing to run"; return 1; }
	# shellcheck disable=SC2317 # run by the traps
	stream_cleanup() {
		local j
		for j in $(jobs -p); do kill "$j" 2>/dev/null; done
		bond_untunnel
		teardown
	}
	trap stream_cleanup EXIT
	trap 'stream_cleanup; exit 130' INT TERM
	mkdir -p "$RUN"
	echo "stream: $STREAM_ENC $STREAM_SIZE at ${STREAM_KBPS} kbit/s, ${STREAM_S} s per run, cases $STREAM_CASES, $STREAM_PROTOS"
	for c in $STREAM_CASES; do
		for p in $STREAM_PROTOS; do
			echo "## $c ($p)"
			if ! (stream_case "$c" "$p"); then
				failed="$failed $c/$p"
				fail=1
			fi
			bond_untunnel
			teardown
		done
	done
	if [ -n "$STREAM_KEEP" ]; then
		mkdir -p "$STREAM_KEEP" && cp -r "$RUN/stream/." "$STREAM_KEEP/" && echo "stream: run directories kept in $STREAM_KEEP"
	else
		echo "stream: run directories in $RUN/stream (report.py DIR --rx DIR --fieldrec DIR/field)"
	fi
	if [ "$fail" = 0 ]; then
		echo "stream: ok"
	else
		echo "stream: FAILED:$failed"
	fi
	return "$fail"
}
