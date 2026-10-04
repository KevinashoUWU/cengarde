# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# Server restart under traffic: "bench/lab.sh restart [REF]".
#
# A restarted server opens the session again with a new random sequence. In
# about half of the restarts it lands more than the anti-replay window behind
# what the client saw last, and before the fix every packet down was "too
# old" until the client restarted too. The client now resets its window on
# an old probe reply that answers one of its recent probes, once nothing new
# has come for 2 x probe_idle_ms (engine/src/epoch.h).
#
# 2000 pps flow down (20 pps up keep the probes fast), and link 3 sits behind
# an 800 ms queue kept full (tbf at 5 Mbit/s on the client side, plus a
# 7.8 Mbit/s filler, as health keeps its 500 ms one full), so that its
# replies answer probes 8 or more back.
#
# - The server restarts RESTARTS times (10). Each time every link must be
#   live within RESTART_LIMIT seconds (4) of the new server's start, with a
#   packet verified after it, and the download must flow again. A restart
#   left without that for WEDGE_S seconds (10) counts as wedged, and the
#   client is restarted to go on.
# - Then with l1 and l2 paused, so that only l3's late replies can show the
#   restart, until one restart lands behind the window (at most 8; each has
#   an even chance), with the same checks.
#
# With REF, it measures the engine of that git commit instead (e.g. the one
# before the fix, to count the wedges).
LAB_CI=1

RESTARTS=${RESTARTS:-10}
RESTART_LIMIT=${RESTART_LIMIT:-4}
WEDGE_S=${WEDGE_S:-10}

# restart_watch T0 LIMIT: waits until every link of the client that is not
# paused is live with a packet verified after T0 (epoch seconds), then checks
# for one second that the download flows. Prints one line; exit 0 when it
# recovered, 1 when not within LIMIT seconds, 2 when the download did not
# flow afterwards, 3 when the client never answered on its control socket.
restart_watch() {
	python3 - "$RUN/client.sock" "$1" "$2" 2000 <<'EOF'
import json, socket, sys, time

path, t0, limit, pps = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])


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


def behind(d, since_ms):
    """Links not live yet, or without a packet verified in the last since_ms."""
    return [l["name"] for l in d["links"] if l["state"] != "paused" and
            (l["state"] != "live" or l["last_rx_ms_ago"] >= since_ms)]


d, late, seen = None, ["?"], False
while time.time() - t0 < limit:
    d = status()
    if d:
        seen = True
        late = behind(d, (time.time() - t0) * 1000)
        if not late:
            break
    time.sleep(0.05)
if not seen:
    print(f"no status from {path} in {limit:.0f} s: the client is down or has no control socket")
    sys.exit(3)
if late:
    print(f"wedged: {' '.join(late)} without a packet verified since the restart after {limit:.0f} s")
    sys.exit(1)
took, p0 = time.time() - t0, d["download"]["packets"]
time.sleep(1)
d = status() or d
flow = d["download"]["packets"] - p0
resets = d["download"].get("window_resets", "-")
print(f"every link live after {took:.1f} s, then {flow} packets down in 1 s, window_resets {resets}")
sys.exit(0 if flow >= pps // 2 and not behind(d, 1000) else 2)
EOF
}

# restart_proc client|server: stops that end and starts it again.
restart_proc() {
	local pid ns=cli i
	[ "$1" = server ] && ns=srv
	pid=$(cat "$RUN/$1.pid")
	kill "$pid" 2>/dev/null
	for i in $(seq 1 30); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.1
		[ "$i" = 30 ] && kill -9 "$pid" 2>/dev/null
	done
	ip netns exec "$ns" "$CENGARDE_BIN" -c "$RUN/$1.conf" >>"$RUN/$1.log" 2>&1 &
	echo $! >"$RUN/$1.pid"
}

# restart_once LABEL: restarts the server and judges how the client comes
# back. Returns 0 when every link is live within RESTART_LIMIT s and the
# download flows, 1 when wedged (the client is then restarted, as an operator
# had to), 2 when the download did not flow afterwards, 3 when it took longer
# than RESTART_LIMIT, 4 when the client did not answer at all (nothing to
# judge: it is down, not wedged).
restart_once() {
	local out rc took
	restart_proc server
	out=$(restart_watch "$(date +%s.%N)" "$WEDGE_S")
	rc=$?
	echo "   $1: $out"
	case $rc in
	0)
		took=$(echo "$out" | sed -n 's/.*after \([0-9.]*\) s.*/\1/p')
		awk -v t="$took" -v l="$RESTART_LIMIT" 'BEGIN { exit !(t > l) }' && return 3
		return 0
		;;
	1)
		restart_proc client
		restart_watch "$(date +%s.%N)" "$WEDGE_S" >/dev/null || echo "   (no recovery after restarting the client either)"
		return 1
		;;
	3) return 4 ;;
	esac
	return 2
}

# window_resets: the client's counter right now ("-" before the fix, "?" when
# the client does not answer).
window_resets() {
	"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/client-now.json" 2>/dev/null &&
		jget "$RUN/client-now.json" 'd["download"].get("window_resets", "-")' 2>/dev/null || echo "?"
}

# not_running: names each end that is not running, with the end of its log
# (e.g. a setting it refused).
not_running() {
	local end
	for end in server client; do
		[ -f "$RUN/$end.pid" ] && ! kill -0 "$(cat "$RUN/$end.pid")" 2>/dev/null || continue
		echo "FAIL: the $end is not running"
		tail -n 5 "$RUN/$end.log" | sed 's/^/   /'
	done
}

restart() {
	local ref=${1:-} tmp="" fail=0 ready=1 n=0 wedged=0 slow=0 noflow=0 secs i j rc rtt out r0 alone=""
	local ctl="$CENGARDE_BIN ctl -s $RUN/client.sock"
	if [ "$(printf %s "$RUN/client.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/client.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "restart: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	if [ -n "$ref" ]; then
		tmp=$RUN/engine-$ref
		rm -rf "$tmp" && mkdir -p "$tmp"
		if ! git -C "$REPO" archive "$ref" engine | tar -x -C "$tmp" ||
			! make -s -C "$tmp/engine" cengarde VERSION="$ref" >/dev/null; then
			echo "FAIL: cannot build the engine at $ref"
			rm -rf "$tmp"
			echo "restart: FAILED"
			return 1
		fi
		CENGARDE_BIN=$tmp/engine/cengarde
		ctl="$CENGARDE_BIN ctl -s $RUN/client.sock"
		echo "   engine at $ref"
	fi
	if ! { setup && start; }; then
		echo "FAIL: the lab did not start"
		fail=1 ready=0
	else
		secs=$(((RESTARTS + 8) * (WEDGE_S + 10) + 10)) # killed by teardown long before
		ip netns exec cli tc qdisc replace dev l3 root tbf rate 5mbit burst 32kb latency 800ms
		ip netns exec srv "$BIN/udpgen" -b 10.0.3.2:7000 -d "$secs" -g 0 >/dev/null &
		ip netns exec cli "$BIN/udpgen" -b 10.0.3.1:7000 -p 10.0.3.2:7000 -r 700 -s 1400 -d "$secs" -g 0 >/dev/null &
		ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r 2000 -s "$SIZE" -d "$secs" -g 1 >/dev/null &
		ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r 20 -s 64 -d "$secs" -g 1 >/dev/null &
		sleep 4
		if ! out=$(restart_watch "$(date +%s.%N)" 5); then
			echo "FAIL: before any restart: $out"
			fail=1 ready=0
		else
			rtt=$(jget "$RUN/client.json" '[round(l["rtt_ms"]) for l in d["links"] if l["name"] == "l3"][0]' 2>/dev/null)
			echo "   before the restarts: $out; l3 RTT ${rtt:-?} ms"
			# Not fatal: with a shorter RTT the restarts still tell something.
			[ "${rtt:-0}" -ge 600 ] || { echo "FAIL: the l3 queue is not full (RTT ${rtt:-?} ms, 800 wanted)"; fail=1; }
		fi
	fi
	if [ "$ready" = 0 ]; then
		# Restarting would only wait WEDGE_S at a time and blame a wedge:
		# name what is wrong instead.
		echo "FAIL: no restarts, the lab is not ready"
		not_running
	else
		for i in $(seq 1 "$RESTARTS") alone; do
			if [ "$i" = alone ]; then
				# Only l3 left: a restart is seen from replies to probes
				# sent 800 ms before, 8 or more back in the ring.
				[ "$($ctl link l1 off)" = ok ] && [ "$($ctl link l2 off)" = ok ] ||
					{ echo "FAIL: cannot pause l1 and l2"; fail=1; break; }
				sleep 2
				for j in $(seq 1 8); do
					r0=$(window_resets)
					n=$((n + 1))
					restart_once "l3 alone, restart $j"
					rc=$?
					[ "$rc" = 0 ] || break
					[ "$(window_resets)" != "$r0" ] && { alone=reset; break; }
					sleep 1
				done
				[ "$rc" = 0 ] && [ -z "$alone" ] && echo "   (no restart with l3 alone landed behind the window: 1 chance in 256)"
			else
				n=$((n + 1))
				restart_once "restart $i"
				rc=$?
			fi
			case $rc in
			1) wedged=$((wedged + 1)) ;;
			2) noflow=$((noflow + 1)) && echo "FAIL: the download did not flow again" ;;
			3) slow=$((slow + 1)) && echo "FAIL: longer than $RESTART_LIMIT s" ;;
			4) echo "FAIL: no status from the client"; not_running; fail=1; break ;;
			esac
			sleep 1
		done
		[ "$wedged" = 0 ] || echo "FAIL: $wedged restarts wedged the download"
		[ "$wedged$slow$noflow" = 000 ] || fail=1
		echo "   $n restarts: $wedged wedged the download, $slow took more than $RESTART_LIMIT s," \
			"$noflow without download afterwards; window resets: $(window_resets)"
		grep -h "started over" "$RUN/client.log" | sed 's/^/   /'
	fi
	stop
	teardown
	[ -n "$tmp" ] && rm -rf "$tmp"
	[ "$fail" = 0 ] && echo "restart: ok" || echo "restart: FAILED"
	return "$fail"
}
