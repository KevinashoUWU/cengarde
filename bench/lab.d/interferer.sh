# shellcheck shell=bash disable=SC2034 # ENGINE, NLINKS and the EXTRAs are read by lab.sh
# The controlled interferer (story 013, 2026-10-16): "bench/lab.sh interferer".
#
# In the field a phone loading the same cell as one of the links did not
# make it drop packets: the cell queued them (bufferbloat), the link's share
# of the uplink fell to about half or a third, and the queue grew 35-40 ms
# a second until the load stopped, then drained in under 0.2 s. This builds
# that: the native tunnel (cg0, as lab.d/native.sh) over 2 links shaped with
# netem, link 1 (20 ms one way, IF_RATE Mbit/s, a queue of IF_QUEUE_MS: the
# faster link, that the split by expected arrival fills first) and link 2
# (30 ms, IF_RATE2, 200 ms of queue), an upload TCP stream at a constant
# IF_LOAD (iperf3 -b, as an RTMP sender) through the tunnel, a ping through
# it every 100 ms, and link 1's rate up (the client's side only: only a
# load in the same direction hurt) cut to IF_RATE / IF_DIV:
#
#   long    once, for IF_S seconds (10)
#   burst   IF_BURSTS times (3), 2 s each, every 8 s
#   drain   for IF_S seconds, then 1 s at IF_RATE (its queue empties at the
#           cell's full rate), then IF_S seconds at IF_RATE / 2 (the cell
#           still shared): what the capacity takes from the drain
#
# A sampler reads both links' netem counters every 100 ms (what the split
# offered each link, and the queue: the backlog over the rate, less the
# delay) and the client's status once a second (link 1's capacity_mbit).
# Per episode (a cut), it measures (bench/interferer.py):
#
#   off_s    from the cut until the first second over which the split
#            offers link 1 no more than its new rate
#   back_s   from the end of the cut until the split offers link 1 80 % of
#            what it did in the 4 s before the cut (over 0.5 s)
#   cap_s    the same for link 1's capacity_mbit (status, once a second)
#   cap_over drain only: link 1's largest capacity_mbit while the cell
#            gives half, over that half (an overshoot from the drain: > 1)
#   q_ms     link 1's largest queue during the cut and the 5 s after
#   q100_s   how long it was over 100 ms then (summed over the episodes)
#
# and per run iperf3's TCP RTT max and retransmissions and the ping's p99
# and max. One JSON line per run goes to IF_OUT (if given) and a line of
# text to the terminal; IF_LABEL names the run. Compare builds with
# CENGARDE_BIN (lab.sh), engine settings with IF_EXTRA (";"-separated, both
# ends; the base is split = on, capacity_weights = yes, split_eta = yes).
# Knobs: IF_CASES ("long burst drain"), IF_N (repetitions, 1), IF_RATE (40),
# IF_RATE2 (20), IF_DIV (3), IF_LOAD (24M), IF_S (10), IF_BURSTS (3),
# IF_QUEUE_MS (2000),
# IF_WARM (8 s before the first cut), IF_TAIL (20 s after the last),
# IF_JITTER_MS (0: link 1's delay swings by this much, netem's jitter, as a
# cellular link's; it also reorders some of its packets).
# Needs iperf3, ping, netem and python3.

IF_CASES=${IF_CASES:-long burst drain}
IF_N=${IF_N:-1}
IF_RATE=${IF_RATE:-40}
IF_RATE2=${IF_RATE2:-20}
IF_DIV=${IF_DIV:-3}
IF_LOAD=${IF_LOAD:-24M}
IF_S=${IF_S:-10}
IF_BURSTS=${IF_BURSTS:-3}
IF_QUEUE_MS=${IF_QUEUE_MS:-2000}
IF_WARM=${IF_WARM:-8}
IF_TAIL=${IF_TAIL:-20}
IF_JITTER_MS=${IF_JITTER_MS:-0}
IF_EXTRA=${IF_EXTRA:-}
IF_OUT=${IF_OUT:-}
IF_LABEL=${IF_LABEL:-run}

# if_link N DELAY_MS RATE QUEUE_MS VERB: netem on link N, the client's side,
# with room for QUEUE_MS at the link's own rate (VERB change keeps its
# queue: the cut must not empty it).
if_link() {
	local n=$1 d=$2 r=$3 q=$4 verb=${5:-replace} lim base=$IF_RATE jit=""
	[ "$n" = 2 ] && base=$IF_RATE2
	[ "$n" = 1 ] && [ "$IF_JITTER_MS" != 0 ] && jit="${IF_JITTER_MS}ms"
	lim=$((base * 1000000 * (q + d) / 8000 / 1400 + 10))
	# shellcheck disable=SC2086 # jit: empty, or the jitter
	ip netns exec cli tc qdisc "$verb" dev "l$n" root netem delay "${d}ms" $jit rate "${r}mbit" limit "$lim"
}

if_stop() {
	local f
	for f in iperf3 ping sampler; do
		[ -f "$RUN/if-$f.pid" ] && kill "$(cat "$RUN/if-$f.pid")" 2>/dev/null
		rm -f "$RUN/if-$f.pid"
	done
}

# if_events CASE: the cuts on link 1, each marked in if-marks (cut / end).
if_events() {
	local c=$1 i cut=$((IF_RATE / IF_DIV))
	sleep "$IF_WARM"
	case $c in
	long)
		echo "cut $(date +%s.%N)" >>"$RUN/if-marks"
		if_link 1 20 "$cut" "$IF_QUEUE_MS" change
		sleep "$IF_S"
		if_link 1 20 "$IF_RATE" "$IF_QUEUE_MS" change
		echo "end $(date +%s.%N)" >>"$RUN/if-marks"
		;;
	burst)
		for i in $(seq 1 "$IF_BURSTS"); do
			echo "cut $(date +%s.%N)" >>"$RUN/if-marks"
			if_link 1 20 "$cut" "$IF_QUEUE_MS" change
			sleep 2
			if_link 1 20 "$IF_RATE" "$IF_QUEUE_MS" change
			echo "end $(date +%s.%N)" >>"$RUN/if-marks"
			[ "$i" -lt "$IF_BURSTS" ] && sleep 6
		done
		;;
	drain)
		echo "cut $(date +%s.%N)" >>"$RUN/if-marks"
		if_link 1 20 "$cut" "$IF_QUEUE_MS" change
		sleep "$IF_S"
		echo "drain $(date +%s.%N)" >>"$RUN/if-marks"
		if_link 1 20 "$IF_RATE" "$IF_QUEUE_MS" change
		sleep 1
		echo "half $(date +%s.%N)" >>"$RUN/if-marks"
		if_link 1 20 $((IF_RATE / 2)) "$IF_QUEUE_MS" change
		sleep "$IF_S"
		if_link 1 20 "$IF_RATE" "$IF_QUEUE_MS" change
		echo "end $(date +%s.%N)" >>"$RUN/if-marks"
		;;
	esac
}

interferer() {
	local tool c rep secs fail=0 line ev
	for tool in iperf3 ping python3; do
		command -v "$tool" >/dev/null || { echo "interferer: needs $tool"; return 1; }
	done
	ENGINE=c NLINKS=2
	CLIENT_EXTRA="tunnel = native;tun_addr = 10.79.0.2/16;split = on;capacity_weights = yes;split_eta = yes${IF_EXTRA:+;$IF_EXTRA}"
	SERVER_EXTRA="tunnel = native;tun_addr = 10.79.0.1/16;capacity_weights = yes;split_eta = yes${IF_EXTRA:+;$IF_EXTRA}"
	trap 'if_stop; teardown' EXIT
	trap 'if_stop; teardown; exit 130' INT TERM
	for c in $IF_CASES; do
		case $c in
		long) secs=$((IF_WARM + IF_S + IF_TAIL)) ;;
		burst) secs=$((IF_WARM + IF_BURSTS * 8 - 6 + IF_TAIL)) ;;
		drain) secs=$((IF_WARM + 2 * IF_S + 1 + IF_TAIL)) ;;
		*) echo "interferer: no case $c (long, burst, drain)"; return 1 ;;
		esac
		for rep in $(seq 1 "$IF_N"); do
			setup && start || return 1
			if ! if_link 1 20 "$IF_RATE" "$IF_QUEUE_MS"; then
				echo "interferer: needs netem"
				return 1
			fi
			if_link 2 30 "$IF_RATE2" 200
			ip netns exec srv tc qdisc replace dev s1 root netem delay 20ms rate "${IF_RATE}mbit"
			ip netns exec srv tc qdisc replace dev s2 root netem delay 30ms rate "${IF_RATE2}mbit"
			rm -f "$RUN/if-marks" "$RUN/if-ping.txt"
			ip netns exec srv iperf3 -s -B 10.79.0.1 -D -I "$RUN/if-iperf3.pid" || return 1
			python3 "$LAB/interferer.py" sample "$RUN" &
			echo $! >"$RUN/if-sampler.pid"
			ip netns exec cli ping -i 0.1 -W 3 10.79.0.1 >"$RUN/if-ping.txt" 2>&1 &
			echo $! >"$RUN/if-ping.pid"
			sleep 0.5
			if_events "$c" &
			ev=$!
			if ! ip netns exec cli iperf3 -c 10.79.0.1 -b "$IF_LOAD" -t "$secs" -J >"$RUN/if-iperf3.json" 2>&1; then
				echo "FAIL: iperf3: $(tail -3 "$RUN/if-iperf3.json")"
				fail=1
			fi
			wait "$ev"
			if_stop
			line=$(python3 "$LAB/interferer.py" measure "$RUN" "$c" "$rep" "$IF_LABEL" "$IF_RATE" "$IF_DIV" "$IF_EXTRA") ||
				{ echo "FAIL: no measure"; fail=1; }
			[ -n "$IF_OUT" ] && [ -n "$line" ] && echo "$line" >>"$IF_OUT"
			[ -n "$line" ] && python3 -c 'import json, sys; d = json.loads(sys.argv[1]); i = d["iperf3"]; p = d["ping"]
print("   %s %s #%d: off %ss, back %ss (cap %ss, over x%s), queue %s ms (%ss over 100); TCP RTT max %s ms, %s retrans, %s Mbit/s; ping p99 %s ms" % (
    d["label"], d["case"], d["rep"], d["off_s"], d["back_s"], d["cap_s"], d["cap_over"], d["q_ms"], d["q100_s"],
    i.get("rtt_max_ms"), i.get("retrans"), i.get("mbit"), p["p99_ms"]))' "$line"
			teardown
		done
	done
	[ -n "$IF_OUT" ] && python3 "$LAB/interferer.py" table "$IF_OUT"
	if [ "$fail" = 0 ]; then
		echo "interferer: ok"
	else
		echo "interferer: FAILED"
	fi
	return "$fail"
}
