#!/bin/bash
# cengarde benchmark lab: two network namespaces joined by three veth "links".
#
#   netns cli (the Pi)                          netns srv (the VPS)
#   fake WG: udpgen 127.0.0.1:50000             server :59402 (engarde or cengarde)
#   client 127.0.0.1:59401                      fake WG: udpgen 127.0.0.1:59301
#   l1 10.0.1.1 ──────────────────────────────── s1 10.0.1.2
#   l2 10.0.2.1 ──────────────────────────────── s2 10.0.2.2
#   l3 10.0.3.1 ──────────────────────────────── s3 10.0.3.2
#
# Needs root, iproute2 (ip, tc with sch_tbf), gcc, go, curl, python3.
# Usage: sudo bench/lab.sh <function> [args]   (see bench/README.md)
set -u
LAB=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$LAB/.." && pwd)
BIN=$LAB/bin
RUN=$LAB/run
CLIENT_BIN=${CLIENT_BIN:-$BIN/engarde-client}
SERVER_BIN=${SERVER_BIN:-$BIN/engarde-server}
CENGARDE_BIN=${CENGARDE_BIN:-$BIN/cengarde} # e.g. a wrapper that runs an OpenWrt build
NLINKS=${NLINKS:-3}
SIZE=${SIZE:-1400}
WRITE_TIMEOUT=${WRITE_TIMEOUT:-10}
PROTO=${PROTO:-} # empty: Go client; "c": C prototype; "dedup": C prototype + dedup
ENGINE=${ENGINE:-go} # go: engarde (Go) on both ends; c: cengarde (engine/) on both ends
# Extra cengarde settings, "key = value" pairs separated by ";" (global section).
CLIENT_EXTRA=${CLIENT_EXTRA:-}
SERVER_EXTRA=${SERVER_EXTRA:-}
KEY=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA= # lab-only shared key

build() {
	mkdir -p "$BIN"
	gcc -O2 -Wall -Wextra -pthread -o "$BIN/udpgen" "$LAB/udpgen.c" || return 1
	gcc -O2 -Wall -Wextra -o "$BIN/protoclient" "$LAB/protoclient.c" || return 1
	make -s -C "$REPO/engine" cengarde && cp "$REPO/engine/cengarde" "$BIN/cengarde" || return 1
	[ "$ENGINE" = c ] && return 0 # the Go baseline is not needed
	if [ ! -d "$REPO/cmd/engarde-client" ]; then
		echo "no Go sources: set CLIENT_BIN/SERVER_BIN to existing engarde binaries"
		return 0
	fi
	# Build the Go engarde from the working tree with a stub web UI, leaving the
	# real internal/assets/browser (produced by the Angular build) untouched.
	local tmp t
	tmp=$(mktemp -d)
	(cd "$REPO" && git ls-files -z cmd internal vendor go.mod go.sum | tar --null -T - -cf -) | tar -xf - -C "$tmp"
	mkdir -p "$tmp/internal/assets/browser"
	echo '<html>stub</html>' >"$tmp/internal/assets/browser/index.html"
	for t in client server; do
		(cd "$tmp" && go build -o "$BIN/engarde-$t" "./cmd/engarde-$t") || return 1
		(cd "$tmp" && go build -race -o "$BIN/engarde-$t-race" "./cmd/engarde-$t") || return 1
	done
	rm -rf "$tmp"
}

setup() {
	teardown
	mkdir -p "$RUN"
	ip netns add cli || return 1
	ip netns add srv || return 1
	ip -n cli link set lo up
	ip -n srv link set lo up
	local i excl='"lo"'
	for i in 1 2 3; do
		ip link add "l$i" netns cli type veth peer name "s$i" netns srv
		ip -n cli addr add "10.0.$i.1/24" dev "l$i"
		ip -n srv addr add "10.0.$i.2/24" dev "s$i"
		ip -n cli link set "l$i" up
		ip -n srv link set "s$i" up
		[ "$i" -gt "$NLINKS" ] && excl="$excl, \"l$i\""
	done
	cat >"$RUN/client.yml" <<EOF
client:
  listenAddr: "127.0.0.1:59401"
  dstAddr: "10.0.1.2:59402"
  writeTimeout: $WRITE_TIMEOUT
  excludedInterfaces: [$excl]
  dstOverrides:
    - ifName: "l2"
      dstAddr: "10.0.2.2:59402"
    - ifName: "l3"
      dstAddr: "10.0.3.2:59402"
  webManager:
    listenAddr: "127.0.0.1:9001"
EOF
	cat >"$RUN/server.yml" <<EOF
server:
  listenAddr: "0.0.0.0:59402"
  dstAddr: "127.0.0.1:59301"
  clientTimeout: 30
  webManager:
    listenAddr: "127.0.0.1:9002"
EOF
	cat >"$RUN/client.conf" <<EOF
mode = client
key = $KEY
listen = 127.0.0.1:59401
server = 10.0.1.2:59402
interfaces = none
status_file = $RUN/client.json
EOF
	[ -n "$CLIENT_EXTRA" ] && echo "$CLIENT_EXTRA" | tr ';' '\n' >>"$RUN/client.conf"
	for i in 1 2 3; do
		printf '[link l%s]\nserver = 10.0.%s.2:59402\nenabled = %s\n' "$i" "$i" \
			"$([ "$i" -le "$NLINKS" ] && echo yes || echo no)" >>"$RUN/client.conf"
	done
	cat >"$RUN/server.conf" <<EOF
mode = server
key = $KEY
listen = *:59402
wireguard = 127.0.0.1:59301
status_file = $RUN/server.json
EOF
	[ -n "$SERVER_EXTRA" ] && echo "$SERVER_EXTRA" | tr ';' '\n' >>"$RUN/server.conf"
	return 0
}

start() {
	if [ "$ENGINE" = c ]; then
		ip netns exec srv "$CENGARDE_BIN" -c "$RUN/server.conf" >"$RUN/server.log" 2>&1 &
		echo $! >"$RUN/server.pid"
		ip netns exec cli "$CENGARDE_BIN" -c "$RUN/client.conf" >"$RUN/client.log" 2>&1 &
		echo $! >"$RUN/client.pid"
		sleep 1.5 # links come up from netlink at once; give the first probes time
		return
	fi
	ip netns exec srv "$SERVER_BIN" "$RUN/server.yml" >"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	if [ -n "$PROTO" ]; then
		local i links="" dd=""
		[ "$PROTO" = dedup ] && dd="-D"
		for i in 1 2 3; do
			[ "$i" -le "$NLINKS" ] && links="$links -L l$i,10.0.$i.1,10.0.$i.2:59402"
		done
		# shellcheck disable=SC2086
		ip netns exec cli "$BIN/protoclient" -l 127.0.0.1:59401 $dd $links >"$RUN/client.log" 2>&1 &
	else
		ip netns exec cli "$CLIENT_BIN" "$RUN/client.yml" >"$RUN/client.log" 2>&1 &
	fi
	echo $! >"$RUN/client.pid"
	sleep 2.5 # the Go client polls the interfaces once per second
}

stop() {
	local f
	for f in client server; do
		[ -f "$RUN/$f.pid" ] && kill "$(cat "$RUN/$f.pid")" 2>/dev/null
		rm -f "$RUN/$f.pid"
	done
	sleep 0.5
}

teardown() {
	stop 2>/dev/null
	pkill -f "$BIN/udpgen" 2>/dev/null
	ip netns del cli 2>/dev/null
	ip netns del srv 2>/dev/null
	return 0
}

# shape LINK RATE: limit the client side egress of a link (e.g. shape l3 5mbit).
# The queue is local, like a USB modem or a slow WiFi uplink.
shape() { ip netns exec cli tc qdisc replace dev "$1" root tbf rate "$2" burst 32kb limit 3mb; }
unshape() { ip netns exec cli tc qdisc del dev "$1" root; }

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# up PPS [SECS]: fake WG on cli sends through engarde; fake WG on srv counts.
up() {
	local pps=$1 secs=${2:-10} cp sp c0 s0 c1 s1 rx
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -d "$secs" -g 3 >"$RUN/rx.out" &
	rx=$!
	sleep 0.3
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r "$pps" -s "$SIZE" -d "$secs" -g 1 >"$RUN/tx.out"
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	wait $rx
	report "$pps" "$secs" $((c1 - c0)) $((s1 - s0))
}

# down PPS [SECS]: fake WG on srv learns engarde-server's address (like WG
# roaming) and sends; a 20 pps stream from cli keeps every path alive.
down() {
	local pps=$1 secs=${2:-10} cp sp c0 s0 c1 s1 rx
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r 20 -s 64 -d $((secs + 2)) -g 2 >"$RUN/rx.out" &
	rx=$!
	sleep 1
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r "$pps" -s "$SIZE" -d "$secs" -g 1 >"$RUN/tx.out"
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	wait $rx
	report "$pps" "$secs" $((c1 - c0)) $((s1 - s0))
}

# CPU per packet includes the kernel work done in the process context (sends,
# and on veth/loopback part of the peer's receive path), the same for all variants.
report() {
	local pps=$1 secs=$2 cticks=$3 sticks=$4 sent uniq dup p50 p99 p999
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	read -r uniq dup p50 p99 p999 < <(sed -n 's/.*uniq=\([0-9]*\) dup=\([0-9]*\).*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\).*/\1 \2 \3 \4 \5/p' "$RUN/rx.out")
	awk -v pps="$pps" -v secs="$secs" -v sent="$sent" -v uniq="$uniq" -v dup="$dup" -v p50="$p50" \
		-v p99="$p99" -v p999="$p999" -v ct="$cticks" -v st="$sticks" -v hz="$(getconf CLK_TCK)" -v size="$SIZE" 'BEGIN {
		loss = sent > 0 ? 100 * (sent - uniq) / sent : 0
		printf "pps=%-6d (%4.0f Mbit/s) sent=%-7d uniq=%-7d loss=%6.2f%% dup=%-7d p50=%6dus p99=%6dus p99.9=%6dus cpu client=%5.1f%% server=%5.1f%% | us/pkt client=%5.1f server=%5.1f\n",
			pps, pps * size * 8 / 1e6, sent, uniq, loss, dup, p50, p99, p999, 100 * ct / hz / secs, 100 * st / hz / secs,
			(sent > 0 ? 1e6 * ct / hz / sent : 0), (sent > 0 ? 1e6 * st / hz / sent : 0)
	}'
}

# Reproduces the measurements quoted in ROADMAP.md (takes a few minutes).
suite() {
	local n v
	echo "## Go client, download 10k pps, 1/2/3 links"
	for n in 1 2 3; do
		NLINKS=$n
		setup && start
		echo -n "links=$n down "
		down 10000 5
		echo -n "links=$n up   "
		up 10000 5
		stop
	done
	NLINKS=3
	echo "## Client: Go vs C prototype vs C prototype + dedup (3 links, same Go server)"
	for v in "" c dedup; do
		PROTO=$v
		setup && start
		for n in 10000 20000 40000; do
			echo -n "client=${v:-go} down "
			down $n 5
		done
		for n in 10000 30000; do
			echo -n "client=${v:-go} up   "
			up $n 5
		done
		stop
	done
	PROTO=
	echo "## Slow link: l3 shaped to 5 Mbit/s, upload 2000 pps (22 Mbit/s)"
	setup && start
	echo -n "no shaping                    "
	up 2000 8
	shape l3 5mbit
	echo -n "l3 5mbit, writeTimeout 10 ms  "
	up 2000 8
	echo "   l3 sockets re-created: $(grep -c "re-creating socket" "$RUN/client.log")"
	stop
	WRITE_TIMEOUT=-1
	setup && start
	shape l3 5mbit
	echo -n "l3 5mbit, writeTimeout -1     "
	up 2000 8
	stop
	WRITE_TIMEOUT=10
	teardown
}

# One packet from an unknown address makes engarde-server send it the tunnel's
# whole downstream traffic for clientTimeout seconds.
demo_stranger() {
	local traffic
	setup && start
	down 2000 6 >/dev/null &
	traffic=$!
	sleep 2
	echo "stranger 10.0.1.1:7777 sends one packet to the server port, then listens:"
	ip netns exec cli "$BIN/udpgen" -b 10.0.1.1:7777 -p 10.0.1.2:59402 -r 2 -d 1 -g 5 | sed 's/ lat_avg.*//'
	wait "$traffic"
	teardown
}

# The client panics ("http: multiple registrations for /") when the web
# manager port is busy, taking the tunnel down with it.
demo_webpanic() {
	local squatter
	setup
	ip netns exec cli python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",9001)); s.listen(1); time.sleep(5)' &
	squatter=$!
	sleep 0.5
	timeout 4 ip netns exec cli "$CLIENT_BIN" "$RUN/client.yml" >"$RUN/client.log" 2>&1
	echo "engarde-client exit code: $? (124 = still running after 4 s)"
	grep -E "panic|level=error" "$RUN/client.log"
	wait "$squatter"
	teardown
}

# Race-detector builds under traffic plus normal web UI usage.
demo_races() {
	local i traffic
	CLIENT_BIN=$BIN/engarde-client-race SERVER_BIN=$BIN/engarde-server-race
	setup && start
	down 3000 5 >/dev/null &
	traffic=$!
	for i in $(seq 1 10); do
		ip netns exec cli curl -s -m1 http://127.0.0.1:9001/api/v1/get-list >/dev/null
		ip netns exec cli curl -s -m1 -X POST -d '{"interface":"l2"}' http://127.0.0.1:9001/api/v1/swap-exclusion >/dev/null
		ip netns exec srv curl -s -m1 http://127.0.0.1:9002/api/v1/get-list >/dev/null
		sleep 0.6
	done
	wait "$traffic"
	stop
	echo "data races: client=$(grep -c 'WARNING: DATA RACE' "$RUN/client.log") server=$(grep -c 'WARNING: DATA RACE' "$RUN/server.log")"
	teardown
}

# engarde (Go) against cengarde (C), each on both ends, 3 links; then the
# slow-link scenario with cengarde (quoted in docs/historias/005).
compare() {
	local eng r
	for eng in go c; do
		ENGINE=$eng
		NLINKS=3
		setup && start
		for r in 10000 20000 40000; do
			echo -n "$eng down "
			down $r 5
		done
		for r in 10000 30000 60000; do
			echo -n "$eng up   "
			up $r 5
		done
		stop
	done
	ENGINE=c
	setup && start
	shape l3 5mbit
	echo -n "c slow l3 "
	up 2000 8
	echo "   link sockets re-created: $(grep -c 'link .* down' "$RUN/client.log")"
	stop
	teardown
}

# Link health (docs/historias/006): link 3 gets a 500 ms queue (5 Mbit/s,
# socket buffers big enough to fill it) while 2000 pps (22 Mbit/s) flow,
# first upload (queue on the client), then download (queue on the server).
# It must be muted within 5 s, without loss on the tunnel, and come back to
# stay once the queue is gone.
health() {
	local dir fail=0 sent uniq
	ENGINE=c
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
# on both ends, and the Go engarde for reference when it is built.
latency() {
	local b r
	NLINKS=1
	for b in 0 50 200; do
		ENGINE=c CLIENT_EXTRA="busy_poll_us = $b" SERVER_EXTRA="busy_poll_us = $b"
		setup && start
		for r in down up; do
			echo -n "c busy_poll_us=$b $r "
			"$r" 10000 5
		done
		stop
	done
	if [ -x "$CLIENT_BIN" ]; then
		ENGINE=go
		setup && start
		for r in down up; do
			echo -n "go $r "
			"$r" 10000 5
		done
		stop
	fi
	teardown
}

# CI smoke test for cengarde (engine/): every packet arrives exactly once in
# both directions, and an unauthenticated sender gets nothing back.
smoke() {
	local fail=0 dir sent uniq dup got traffic
	ENGINE=c
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
	got=$(ip netns exec cli "$BIN/udpgen" -b 10.0.1.1:7777 -p 10.0.1.2:59402 -r 2 -d 1 -g 2 |
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

if [ $# -eq 0 ]; then
	echo "usage: $0 build|setup|start|stop|teardown|up PPS [S]|down PPS [S]|shape LINK RATE|unshape LINK|suite|compare|smoke|health|latency|demo_stranger|demo_webpanic|demo_races"
	exit 1
fi
"$@"
