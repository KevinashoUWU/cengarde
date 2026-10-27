# shellcheck shell=bash disable=SC2034 # NLINKS is read by lab.sh
# The server's CPU with several routers on FEC v3 (story 026): "bench/lab.sh fec3cpu".
#
# FC_ROUTERS client engines (1 to 3) in the cli namespace, each a router with
# two links of its own (l1-l2, l3-l4, l5-l6) losing FC_LOSS % at random each
# way (netem), and one server with a [client NAME] section each. Router N
# owns the TUN cgN with 10.79.N.2/24 and the server's cg0 has 10.79.N.1/24
# (as in multiclient). Every router sends FC_PPS datagrams a second of
# FC_SIZE bytes (udpgen, through its TUN) up, or the server down (FC_DIR),
# for FC_S seconds, split over its two
# links; then the same with the next arm (FC_ARMS): off (fec = no on both
# ends), xor (fec = yes) or rlcN (fec_code = rlc, fec_pct = N, at the end
# that sends; fec = yes at both). Per arm, a JSON line on stdout and in
# $RUN/fec3cpu.jsonl: the server's and the routers' CPU (% of one core over
# the traffic, from /proc), what arrived and what the receivers rebuilt.
# Not part of "lab.sh ci" (a measurement, not a check).

FC_ROUTERS=${FC_ROUTERS:-3}
FC_PPS=${FC_PPS:-3000}
FC_SIZE=${FC_SIZE:-1392}
FC_LOSS=${FC_LOSS:-1}
FC_S=${FC_S:-20}
FC_DIR=${FC_DIR:-up}
FC_ARMS=${FC_ARMS:-off rlc10 rlc25}
FC_KEYS="r1=$KEY r2=ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4Q= r3=AlbCIRHlyMZZ7TuRdu05c8OkgeGanpqawJ15fifXF0o="

fc_key() { echo "$FC_KEYS" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

# fc_arm ARM END: the arm's settings at END (client or server).
fc_arm() {
	case $1 in
	off) echo "fec = no" ;;
	xor) echo "fec = yes" ;;
	rlc*)
		if { [ "$2" = client ] && [ "$FC_DIR" = up ]; } || { [ "$2" = server ] && [ "$FC_DIR" = down ]; }; then
			printf 'fec = yes\nfec_code = rlc\nfec_pct = %s\nfec_single = yes\n' "${1#rlc}"
		else
			echo "fec = yes"
		fi
		;;
	esac
}

fc_conf() {
	local n=$1 arm=$2 i
	cat <<-EOF
		mode = client
		key = $(fc_key "r$n")
		tunnel = native
		tun = cg$n
		tun_addr = 10.79.$n.2/24
		server = 10.0.$((2 * n - 1)).2:59402
		interfaces = none
		status_file = $RUN/r$n.json
		control_socket = $RUN/r$n.sock
		split = force
		mute_behind_ms = 0
		$(fc_arm "$arm" client)
	EOF
	for i in 1 2 3 4 5 6; do
		printf '[link l%s]\nserver = 10.0.%s.2:59402\nenabled = %s\n' "$i" "$i" \
			"$([ "$i" = $((2 * n - 1)) ] || [ "$i" = $((2 * n)) ] && echo yes || echo no)"
	done
}

fc_server_conf() {
	local n
	cat <<-EOF
		mode = server
		listen = *:59402
		status_file = $RUN/server.json
		control_socket = $RUN/server.sock
		split = force
		mute_behind_ms = 0
		tunnel = native
		tun_addr = 10.79.1.1/24 10.79.2.1/24 10.79.3.1/24
		$(fc_arm "$1" server)
	EOF
	for n in $(seq 1 "$FC_ROUTERS"); do
		printf '[client r%s]\nkey = %s\ntun_route = 10.79.%s.2/32\n' "$n" "$(fc_key "r$n")" "$n"
	done
}

fc_stop() {
	local n
	for n in 1 2 3; do
		[ -f "$RUN/r$n.pid" ] && kill "$(cat "$RUN/r$n.pid")" 2>/dev/null
		rm -f "$RUN/r$n.pid"
	done
	stop
}

fc_teardown() {
	fc_stop
	teardown
}

# fc_status END: END's status from its control socket (JSON).
fc_status() { "$CENGARDE_BIN" ctl -s "$RUN/$1.sock" status 2>/dev/null; }

fec3cpu() {
	local arm n i pids s0 s1 c0 c1 t0 t1 hz line
	NLINKS=6
	hz=$(getconf CLK_TCK)
	trap fc_teardown EXIT
	trap 'fc_teardown; exit 130' INT TERM
	setup || return 1
	for i in $(seq 1 6); do
		ip netns exec "$NS_CLI" tc qdisc replace dev "l$i" root netem delay 20ms loss "${FC_LOSS}%" limit 100000
		ip netns exec "$NS_SRV" tc qdisc replace dev "s$i" root netem delay 20ms loss "${FC_LOSS}%" limit 100000
	done
	: >"$RUN/fec3cpu.jsonl"
	for arm in $FC_ARMS; do
		fc_server_conf "$arm" >"$RUN/server.conf"
		ip netns exec "$NS_SRV" "$CENGARDE_BIN" -c "$RUN/server.conf" >"$RUN/server.log" 2>&1 &
		echo $! >"$RUN/server.pid"
		for n in $(seq 1 "$FC_ROUTERS"); do
			fc_conf "$n" "$arm" >"$RUN/r$n.conf"
			ip netns exec "$NS_CLI" "$CENGARDE_BIN" -c "$RUN/r$n.conf" >"$RUN/r$n.log" 2>&1 &
			echo $! >"$RUN/r$n.pid"
		done
		sleep 3
		pids=
		for n in $(seq 1 "$FC_ROUTERS"); do
			if [ "$FC_DIR" = up ]; then
				ip netns exec "$NS_SRV" "$BIN/udpgen" -b "10.79.$n.1:59301" -d $((FC_S + 3)) -g 3 >"$RUN/rx-$n.out" &
			else
				ip netns exec "$NS_CLI" "$BIN/udpgen" -b "10.79.$n.2:50000" -p "10.79.$n.1:59301" -r 20 -s 64 \
					-d $((FC_S + 3)) -g 2 >"$RUN/rx-$n.out" &
			fi
			pids="$pids $!"
		done
		sleep 1
		s0=$(cpu_ticks "$(cat "$RUN/server.pid")")
		c0=0
		for n in $(seq 1 "$FC_ROUTERS"); do c0=$((c0 + $(cpu_ticks "$(cat "$RUN/r$n.pid")"))); done
		t0=$(date +%s.%N)
		for n in $(seq 1 "$FC_ROUTERS"); do
			if [ "$FC_DIR" = up ]; then
				ip netns exec "$NS_CLI" "$BIN/udpgen" -b "10.79.$n.2:50000" -p "10.79.$n.1:59301" -r "$FC_PPS" \
					-s "$FC_SIZE" -d "$FC_S" -g 1 >"$RUN/tx-$n.out" &
			else
				ip netns exec "$NS_SRV" "$BIN/udpgen" -b "10.79.$n.1:59301" -l -r "$FC_PPS" -s "$FC_SIZE" -d "$FC_S" \
					-g 1 >"$RUN/tx-$n.out" &
			fi
			pids="$pids $!"
		done
		# shellcheck disable=SC2086 # one pid each
		wait $pids
		t1=$(date +%s.%N)
		s1=$(cpu_ticks "$(cat "$RUN/server.pid")")
		c1=0
		for n in $(seq 1 "$FC_ROUTERS"); do c1=$((c1 + $(cpu_ticks "$(cat "$RUN/r$n.pid")"))); done
		fc_status server >"$RUN/server-status.json"
		for n in $(seq 1 "$FC_ROUTERS"); do fc_status "r$n" >"$RUN/r$n-status.json"; done
		line=$(python3 -I - "$RUN" "$arm" "$FC_ROUTERS" "$FC_PPS" "$FC_SIZE" "$FC_LOSS" "$FC_DIR" \
			"$(awk -v a="$s0" -v b="$s1" -v t0="$t0" -v t1="$t1" -v hz="$hz" 'BEGIN { printf "%.2f", (b - a) / hz / (t1 - t0) * 100 }')" \
			"$(awk -v a="$c0" -v b="$c1" -v t0="$t0" -v t1="$t1" -v hz="$hz" 'BEGIN { printf "%.2f", (b - a) / hz / (t1 - t0) * 100 }')" <<'EOF'
import json, re, sys
run, arm, nr, pps, size, loss, d, scpu, ccpu = sys.argv[1:]
def j(p):
    try:
        return json.load(open(p))
    except (OSError, ValueError):
        return {}
def num(path):
    try:
        return int(re.search(r"uniq=(\d+)", open(path).read()).group(1))
    except (OSError, AttributeError):
        return None
srv = j(run + "/server-status.json")
rec = 0
for s in srv.get("sessions", []):
    rec += (s.get("repair", {}).get("rlc") or {}).get("recovered", 0) + (s.get("fec") or {}).get("recovered", 0)
for n in range(1, int(nr) + 1):
    rec += (j("%s/r%d-status.json" % (run, n)).get("download_fec") or {}).get("rlc_recovered", 0)
got = [num("%s/rx-%d.out" % (run, n)) for n in range(1, int(nr) + 1)]
print(json.dumps({"arm": arm, "routers": int(nr), "pps": int(pps), "size": int(size), "loss_pct": float(loss), "dir": d,
                  "server_cpu_pct": float(scpu), "routers_cpu_pct": float(ccpu), "received": got, "rebuilt": rec}))
EOF
		)
		echo "$line" | tee -a "$RUN/fec3cpu.jsonl"
		fc_stop
		sleep 1
	done
	fc_teardown
	echo "fec3cpu: ok"
}
