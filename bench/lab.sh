#!/bin/bash
# cengarde benchmark lab: two network namespaces joined by veth "links" (three,
# or NLINKS when more), and the native tunnel between them (a TUN each end).
#
#   netns cli (the router)                      netns srv (the VPS)
#   udpgen 10.79.0.2:50000                      udpgen 10.79.0.1:59301
#   cg0 10.79.0.2/16 (cengarde client)          cg0 10.79.0.1/16 (cengarde server :59402)
#   l1 10.0.1.1 ──────────────────────────────── s1 10.0.1.2
#   l2 10.0.2.1 ──────────────────────────────── s2 10.0.2.2
#   l3 10.0.3.1 ──────────────────────────────── s3 10.0.3.2
#
# Needs root, iproute2 (ip, tc with sch_tbf), gcc, make and python3.
# Usage: sudo bench/lab.sh <function> [args]   (see bench/README.md)
set -u
export LC_NUMERIC=C # awk's and printf's decimals with a point, as the parsers expect
LAB=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$LAB/.." && pwd)
# LAB_ID: several labs side by side on one machine (bench/labq.py runs them).
# Empty, the names of always: netns cli and srv, bench/run. LAB_ID=N names the
# netns cli-N and srv-N and the run directory bench/run-N; teardown then
# touches only those. Everything else (veths, addresses, ports, TUN devices,
# sockets) lives inside the two netns or in RUN, so it keeps its name: l1 in
# cli-1 and l1 in cli-2 are two interfaces.
LAB_ID=${LAB_ID:-}
case $LAB_ID in *[!0-9]*) echo "LAB_ID must be a number" >&2; exit 1 ;; esac
NS_CLI=cli${LAB_ID:+-$LAB_ID}
NS_SRV=srv${LAB_ID:+-$LAB_ID}
export LAB_ID NS_CLI NS_SRV # for the Python helpers (health.py)
BIN=${BIN:-$LAB/bin} # the bench tools and the engine "build" writes
RUN=$(realpath -ms "${RUN:-$LAB/run${LAB_ID:+-$LAB_ID}}") # configs, logs and control sockets (absolute; sun_path: at most 107 bytes)
CENGARDE_BIN=${CENGARDE_BIN:-$BIN/cengarde} # e.g. a wrapper that runs an OpenWrt build, or a build of another revision
NLINKS=${NLINKS:-3}
# udpgen's datagrams: 1392 bytes of UDP payload make a 1420-byte IPv4 packet,
# the largest the TUN (tun_mtu 1420) takes without fragmenting it.
SIZE=${SIZE:-1392}
# The tunnel's ends, the TUN addresses (cg0) the generators talk between.
# The configurations say "tunnel = native" although it is the only mode: an
# engine older than 0.12 (labq.py's arms, "restart REF") defaults to WireGuard.
CLI_TUN=10.79.0.2
SRV_TUN=10.79.0.1
# Extra cengarde settings, "key = value" pairs separated by ";" (global section).
CLIENT_EXTRA=${CLIENT_EXTRA:-}
SERVER_EXTRA=${SERVER_EXTRA:-}
# Each client link's type (story 015, linkmodel.h), in order: "starlink
# cellular cellular" sets l1 starlink and l2, l3 cellular; empty or "-": none
# written (auto). bond.sh takes it as BOND_LINK_TYPES.
LINK_TYPES=${LINK_TYPES:-${BOND_LINK_TYPES:-}}
KEY=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA= # lab-only shared key

build() {
	mkdir -p "$BIN"
	gcc -O2 -Wall -Wextra -pthread -o "$BIN/udpgen" "$LAB/udpgen.c" || return 1
	gcc -O2 -Wall -Wextra -pthread -o "$BIN/mgen" "$LAB/mgen.c" || return 1
	gcc -O2 -Wall -Wextra -pthread -o "$BIN/jitter" "$LAB/jitter.c" || return 1
	gcc -O2 -Wall -Wextra -pthread -I"$REPO/engine/src" -o "$BIN/ringbench" "$LAB/ringbench.c" || return 1
	gcc -O2 -Wall -Wextra -I"$REPO/engine/src" -o "$BIN/robench" "$LAB/robench.c" || return 1
	gcc -O2 -Wall -Wextra -I"$REPO/engine/src" -o "$BIN/cgprobe" "$LAB/cgprobe.c" \
		"$REPO"/engine/src/{proto,siphash,util,log,blake2s,pair}.c || return 1
	gcc -O2 -Wall -Wextra -I"$REPO/engine/src" -o "$BIN/macbench" "$LAB/macbench.c" \
		"$REPO"/engine/src/{proto,siphash}.c || return 1
	make -s -C "$REPO/engine" cengarde && cp "$REPO/engine/cengarde" "$BIN/cengarde" || return 1
	# the erasure codes' cost (story 021), on the engine's Leopard objects
	local leo o objs=
	leo=$(make -s -C "$REPO/engine" leo-objs) || return 1
	for o in $leo; do objs="$objs $REPO/engine/$o"; done
	# shellcheck disable=SC2086 # the object list splits on spaces
	gcc -O2 -Wall -Wextra -I"$REPO/engine/src" -I"$REPO/engine/third_party/leopard" -o "$BIN/fecbench" \
		"$LAB/fecbench.c" "$REPO/engine/src/rs.c" $objs -lpthread
}

setup() {
	teardown
	mkdir -p "$RUN"
	ip netns add "$NS_CLI" || return 1
	ip netns add "$NS_SRV" || return 1
	ip -n "$NS_CLI" link set lo up
	ip -n "$NS_SRV" link set lo up
	local i t nveth=$((NLINKS > 3 ? NLINKS : 3))
	for i in $(seq 1 "$nveth"); do
		ip link add "l$i" netns "$NS_CLI" type veth peer name "s$i" netns "$NS_SRV"
		ip -n "$NS_CLI" addr add "10.0.$i.1/24" dev "l$i"
		ip -n "$NS_SRV" addr add "10.0.$i.2/24" dev "s$i"
		ip -n "$NS_CLI" link set "l$i" up
		ip -n "$NS_SRV" link set "s$i" up
	done
	cat >"$RUN/client.conf" <<EOF
mode = client
key = $KEY
server = 10.0.1.2:59402
interfaces = none
tunnel = native
tun_addr = $CLI_TUN/16
status_file = $RUN/client.json
EOF
	[ -n "$CLIENT_EXTRA" ] && echo "$CLIENT_EXTRA" | tr ';' '\n' >>"$RUN/client.conf"
	for i in $(seq 1 "$nveth"); do
		printf '[link l%s]\nserver = 10.0.%s.2:59402\nenabled = %s\n' "$i" "$i" \
			"$([ "$i" -le "$NLINKS" ] && echo yes || echo no)" >>"$RUN/client.conf"
		t=$(echo "$LINK_TYPES" | awk -v i="$i" '{ print $i }')
		[ -n "$t" ] && [ "$t" != - ] && echo "type = $t" >>"$RUN/client.conf"
	done
	cat >"$RUN/server.conf" <<EOF
mode = server
key = $KEY
listen = *:59402
tunnel = native
tun_addr = $SRV_TUN/16
status_file = $RUN/server.json
EOF
	[ -n "$SERVER_EXTRA" ] && echo "$SERVER_EXTRA" | tr ';' '\n' >>"$RUN/server.conf"
	return 0
}

start() {
	ip netns exec "$NS_SRV" "$CENGARDE_BIN" -c "$RUN/server.conf" >"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	ip netns exec "$NS_CLI" "$CENGARDE_BIN" -c "$RUN/client.conf" >"$RUN/client.log" 2>&1 &
	echo $! >"$RUN/client.pid"
	sleep 1.5 # links come up from netlink at once; give the first probes time
}

stop() {
	local f
	for f in client server; do
		[ -f "$RUN/$f.pid" ] && kill "$(cat "$RUN/$f.pid")" 2>/dev/null
		rm -f "$RUN/$f.pid"
	done
	sleep 0.5
}

# ns_kill NETNS [NAME]: kills what runs in that netns (only the processes
# called NAME, when given); other labs' processes are left alone.
ns_kill() {
	local p
	for p in $(ip netns pids "$1" 2>/dev/null); do
		[ -z "${2:-}" ] || [ "$(cat "/proc/$p/comm" 2>/dev/null)" = "$2" ] || continue
		kill "$p" 2>/dev/null
	done
}

teardown() {
	stop 2>/dev/null
	ns_kill "$NS_CLI"
	ns_kill "$NS_SRV"
	ip netns del "$NS_CLI" 2>/dev/null
	ip netns del "$NS_SRV" 2>/dev/null
	return 0
}

# shape LINK RATE: limit the client side egress of a link (e.g. shape l3 5mbit).
# The queue is local, like a USB modem or a slow WiFi uplink.
shape() { ip netns exec "$NS_CLI" tc qdisc replace dev "$1" root tbf rate "$2" burst 32kb limit 3mb; }
unshape() { ip netns exec "$NS_CLI" tc qdisc del dev "$1" root; }

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# up PPS [SECS]: udpgen on cli sends through the tunnel (into the client's
# TUN), udpgen on srv counts what the server's TUN hands it.
up() {
	local pps=$1 secs=${2:-10} cp sp c0 s0 c1 s1 rx
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	ip netns exec "$NS_SRV" "$BIN/udpgen" -b "$SRV_TUN:59301" -d "$secs" -g 3 >"$RUN/rx.out" &
	rx=$!
	sleep 0.3
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec "$NS_CLI" "$BIN/udpgen" -b "$CLI_TUN:50000" -p "$SRV_TUN:59301" -r "$pps" -s "$SIZE" -d "$secs" -g 1 >"$RUN/tx.out"
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	wait $rx
	report "$pps" "$secs" $((c1 - c0)) $((s1 - s0))
}

# down PPS [SECS]: udpgen on srv learns its peer from a 20 pps stream from
# cli (which also keeps every path alive) and sends down the tunnel.
down() {
	local pps=$1 secs=${2:-10} cp sp c0 s0 c1 s1 rx
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	ip netns exec "$NS_CLI" "$BIN/udpgen" -b "$CLI_TUN:50000" -p "$SRV_TUN:59301" -r 20 -s 64 -d $((secs + 2)) -g 2 >"$RUN/rx.out" &
	rx=$!
	sleep 1
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec "$NS_SRV" "$BIN/udpgen" -b "$SRV_TUN:59301" -l -r "$pps" -s "$SIZE" -d "$secs" -g 1 >"$RUN/tx.out"
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	wait $rx
	report "$pps" "$secs" $((c1 - c0)) $((s1 - s0))
}

# CPU per packet includes the kernel work done in the process context (sends,
# and on veth/loopback part of the peer's receive path), the same for all variants.
report() {
	local pps=$1 secs=$2 cticks=$3 sticks=$4 sent uniq dup p50 p99 p999 size=$SIZE
	[ "$size" = imix ] && size=345 # udpgen -s imix: 345 bytes on average
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	read -r uniq dup p50 p99 p999 < <(sed -n 's/.*uniq=\([0-9]*\) dup=\([0-9]*\).*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\).*/\1 \2 \3 \4 \5/p' "$RUN/rx.out")
	awk -v pps="$pps" -v secs="$secs" -v sent="$sent" -v uniq="$uniq" -v dup="$dup" -v p50="$p50" \
		-v p99="$p99" -v p999="$p999" -v ct="$cticks" -v st="$sticks" -v hz="$(getconf CLK_TCK)" -v size="$size" 'BEGIN {
		loss = sent > 0 ? 100 * (sent - uniq) / sent : 0
		printf "pps=%-6d (%4.0f Mbit/s) sent=%-7d uniq=%-7d loss=%6.2f%% dup=%-7d p50=%6dus p99=%6dus p99.9=%6dus cpu client=%5.1f%% server=%5.1f%% | us/pkt client=%5.1f server=%5.1f\n",
			pps, pps * size * 8 / 1e6, sent, uniq, loss, dup, p50, p99, p999, 100 * ct / hz / secs, 100 * st / hz / secs,
			(sent > 0 ? 1e6 * ct / hz / sent : 0), (sent > 0 ? 1e6 * st / hz / sent : 0)
	}'
}

# One packet from an unknown address: engarde (Go) sent such a sender the
# tunnel's whole downstream traffic for clientTimeout seconds (story 001);
# cengarde sends it nothing.
demo_stranger() {
	local traffic
	setup && start
	down 2000 6 >/dev/null &
	traffic=$!
	sleep 2
	echo "stranger 10.0.1.1:7777 sends one packet to the server port, then listens:"
	ip netns exec "$NS_CLI" "$BIN/udpgen" -b 10.0.1.1:7777 -p 10.0.1.2:59402 -r 2 -d 1 -g 5 | sed 's/ lat_avg.*//'
	wait "$traffic"
	teardown
}

# Link health (docs/historias/006): link 3 gets a 500 ms queue (5 Mbit/s,
# socket buffers big enough to fill it) while 2000 pps (22 Mbit/s) flow,
# first upload (queue on the client), then download (queue on the server).
# It must be muted within 5 s, without loss on the tunnel, and come back to
# stay once the queue is gone.
health() {
	local dir fail=0 sent uniq
	CLIENT_EXTRA="sndbuf = 4194304${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	for dir in up down; do
		setup && start || return 1
		python3 "$LAB/health.py" "$dir" "$RUN" 50 3 25 >"$RUN/health.txt" &
		local watch=$!
		"$dir" 2000 50 >"$RUN/traffic.txt"
		wait "$watch" || fail=1
		cat "$RUN/health.txt"
		echo "   tunnel: $(cat "$RUN/traffic.txt")"
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		[ -n "$sent" ] && [ $((uniq * 1000)) -ge $((sent * 999)) ] || { echo "FAIL: $dir lost packets"; fail=1; }
		grep -h "muted" "$RUN/client.log" "$RUN/server.log" | sed 's/^/   /'
		stop
	done
	teardown
	[ "$fail" = 0 ] && echo "health: ok" || echo "health: FAILED"
	return "$fail"
}

# Wake-up latency and CPU with busy polling (docs/historias/006): one link,
# so there are no copies to race; cengarde with busy_poll_us 0, 50 and 200
# on both ends.
latency() {
	local b r
	NLINKS=1
	for b in 0 50 200; do
		CLIENT_EXTRA="busy_poll_us = $b" SERVER_EXTRA="busy_poll_us = $b"
		setup && start
		for r in down up; do
			echo -n "busy_poll_us=$b $r "
			"$r" 10000 5
		done
		stop
	done
	teardown
}

# jget FILE EXPR: a value from a status JSON (EXPR in Python, on d).
jget() { python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$1" "$2"; }

# Control socket, reload and IP pass (docs/historias/009). While 2000 pps flow
# up: pause link 3 by hand, reload the client by SIGHUP (IP pass off) and by
# "cengarde ctl reload" (IP pass back on), and resume link 3. The tunnel keeps
# its session and loses nothing, and the server writes each IP pass wish to
# its passthrough_file. Then a setting that needs a restart (cpu) restarts
# the client in place: same PID, new session.
control() {
	local fail=0 ctl sess pid traffic sent uniq out
	CLIENT_EXTRA="control_socket = $RUN/client.sock;passthrough = yes${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock;passthrough_file = $RUN/passthrough${SERVER_EXTRA:+;$SERVER_EXTRA}"
	setup && start || return 1
	ctl="$CENGARDE_BIN ctl -s $RUN/client.sock"
	sess=$(jget "$RUN/client.json" 'd["session"]')
	pid=$(cat "$RUN/client.pid")
	[ "$(cat "$RUN/passthrough" 2>/dev/null)" = on ] || { echo "FAIL: IP pass not asked for at start"; fail=1; }
	up 2000 12 >"$RUN/traffic.txt" &
	traffic=$!
	sleep 2
	[ "$($ctl link l3 off)" = ok ] || { echo "FAIL: link l3 off"; fail=1; }
	sleep 1
	$ctl links | grep -Eq '^l3 +[^ ]+ +paused +off ' || { echo "FAIL: l3 not paused"; $ctl links; fail=1; }
	$ctl status >"$RUN/client-now.json"
	[ "$(jget "$RUN/client-now.json" '[l["state"] for l in d["links"] if l["name"] == "l3"][0]')" = paused ] ||
		{ echo "FAIL: the status does not show l3 paused"; fail=1; }
	sed -i 's/^passthrough = yes/passthrough = no/' "$RUN/client.conf"
	kill -HUP "$pid"
	sleep 2
	[ "$(cat "$RUN/passthrough")" = off ] || { echo "FAIL: IP pass off did not reach the server"; fail=1; }
	sed -i 's/^passthrough = no/passthrough = yes/' "$RUN/client.conf"
	out=$($ctl reload)
	[ "$out" = ok ] || { echo "FAIL: ctl reload said: $out"; fail=1; }
	sleep 2
	[ "$(cat "$RUN/passthrough")" = on ] || { echo "FAIL: IP pass on did not reach the server"; fail=1; }
	[ "$($ctl link l3 auto)" = ok ] || { echo "FAIL: link l3 auto"; fail=1; }
	sleep 1
	$ctl links | grep -Eq '^l3 +[^ ]+ +(live|waiting) +- ' || { echo "FAIL: l3 not back"; $ctl links; fail=1; }
	wait "$traffic"
	echo "   tunnel: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	[ -n "$sent" ] && [ $((uniq * 1000)) -ge $((sent * 999)) ] || { echo "FAIL: packets lost"; fail=1; }
	[ "$(jget "$RUN/client.json" 'd["session"]')" = "$sess" ] || { echo "FAIL: the reloads changed the session"; fail=1; }
	[ "$(jget "$RUN/client.json" 'd["passthrough"]["server"]')" = on ] ||
		{ echo "FAIL: the client does not see IP pass on at the server"; fail=1; }
	$CENGARDE_BIN ctl -s "$RUN/server.sock" links | grep -q '^SESSION' || { echo "FAIL: server ctl links"; fail=1; }
	# A setting the loop set up once: the client starts again in place.
	sed -i '1a cpu = 0' "$RUN/client.conf"
	out=$($ctl reload)
	[ "$out" = "ok: cpu changed, restarting" ] || { echo "FAIL: ctl reload said: $out"; fail=1; }
	sleep 2
	kill -0 "$pid" 2>/dev/null || { echo "FAIL: the client is gone"; fail=1; }
	[ "$(jget "$RUN/client.json" 'd["session"]')" != "$sess" ] || { echo "FAIL: no new session after the restart"; fail=1; }
	$ctl links | grep -Eq '^l1 +[^ ]+ +live' || { echo "FAIL: no control socket after the restart"; fail=1; }
	grep -hE "paused|by hand|reload|IP pass|restarting|configuration" "$RUN/client.log" "$RUN/server.log" | sed 's/^/   /'
	stop
	teardown
	[ "$fail" = 0 ] && echo "control: ok" || echo "control: FAILED"
	return "$fail"
}

# CI smoke test for cengarde (engine/): every packet arrives exactly once in
# both directions, and an unauthenticated sender gets nothing back.
smoke() {
	local fail=0 dir sent uniq dup got traffic
	setup && start || return 1
	for dir in down up; do
		"$dir" 2000 3
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
			echo "FAIL: $dir: sent=$sent uniq=$uniq dup=$dup"
			fail=1
		fi
	done
	down 2000 4 >/dev/null &
	traffic=$!
	sleep 1
	got=$(ip netns exec "$NS_CLI" "$BIN/udpgen" -b 10.0.1.1:7777 -p 10.0.1.2:59402 -r 2 -d 1 -g 2 |
		sed -n 's/.*rx=\([0-9]*\).*/\1/p')
	wait "$traffic"
	if [ "$got" != 0 ]; then
		echo "FAIL: an unauthenticated sender received $got packets"
		fail=1
	fi
	grep -q "from 10.0.1.1:7777" "$RUN/server.log" || { echo "FAIL: server did not report the stranger"; fail=1; }
	stop
	[ "$fail" = 0 ] && echo "smoke: ok" || { echo "--- client log"; cat "$RUN/client.log"; echo "--- server log"; cat "$RUN/server.log"; }
	teardown
	return "$fail"
}

# What CI runs (engine.yml): smoke, health, control and every bench/lab.d
# scenario with LAB_CI=1. Each one runs in a subshell, since they set NLINKS
# and the *_EXTRA settings, and from a clean lab; a failure does not stop the
# others, and the last line sums up.
ci() {
	local t failed=""
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	for t in smoke health control "${LAB_D_CI[@]}"; do
		echo "## $t"
		("$t") || failed="$failed $t"
		teardown
	done
	if [ -n "$failed" ]; then
		echo "ci: FAILED:$failed"
		return 1
	fi
	echo "ci: ok (smoke health control ${LAB_D_CI[*]})"
}

# Scenarios in bench/lab.d/NAME.sh: each one defines the function NAME (run
# it as "bench/lab.sh NAME"), uses what is above, and sets LAB_CI=1 to run in
# "bench/lab.sh ci". A new scenario is a new file; this one does not change.
LAB_D=() LAB_D_CI=()
for f in "$LAB"/lab.d/*.sh; do
	[ -e "$f" ] || continue
	LAB_CI=0
	# shellcheck source=/dev/null
	. "$f"
	f=$(basename "$f" .sh)
	LAB_D+=("$f")
	[ "$LAB_CI" = 1 ] && LAB_D_CI+=("$f")
done
unset f LAB_CI

if [ $# -eq 0 ]; then
	f=$(IFS='|' && echo "${LAB_D[*]}")
	echo "usage: $0 build|setup|start|stop|teardown|up PPS [S]|down PPS [S]|shape LINK RATE|unshape LINK|smoke|health|control|latency|ci|demo_stranger${f:+|$f}"
	exit 1
fi
"$@"
