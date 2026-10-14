#!/bin/sh
# What the uplinks of a cengarde router carry, measured with iperf3 (story
# 012): each link alone and all at once outside the tunnel, and the tunnel
# on top of them. The router runs the tests (OpenWrt's busybox is enough,
# with iperf3 from the package feed: apk add iperf3, or opkg install iperf3);
# the VPS runs the iperf3 servers ("vps on" there, as root). While each
# test runs it pings every link it loads, and it keeps the counters that
# say where a packet the router could not send was dropped: the UDP
# sockets' send buffer errors, each interface's queue (tc) and its own
# drops, and the engine's tx_drops. bench/fieldperf.py turns the directory
# into tables.
#
# Mobile data costs money: "plan" prints the tests and an estimate of the
# data they move, and "run" asks before it starts. Do not run it while the
# links carry something that matters (a broadcast): it fills them on purpose.
#
#   sh fieldperf.sh plan [OPTIONS]          what would run, and the data
#   sh fieldperf.sh run [OPTIONS]           runs it into a directory
#   sh fieldperf.sh vps on|off|status [-p PORT] [-n SERVERS]
#                                           on the VPS, as root: the iperf3
#                                           servers, the hole in ufw and
#                                           cengarde-nat's reserve for them
# OPTIONS:
#   -m "solo all tunnel"  what runs: each link alone, all of them at once
#                         (both outside the tunnel), through the tunnel
#   -l "LINK ..."         the links, by label or device (every link the
#                         engine's status lists, by default)
#   -s ADDR               the VPS outside the tunnel (by default each link's
#                         own, from the engine's status)
#   -T ADDR               the VPS inside the tunnel (10.79.0.1)
#   -S FILE               the engine's status (/var/run/cengarde.json)
#   -t SECS               each test (10); -P STREAMS: TCP streams (4)
#   -d "up down"          the directions of each test (both)
#   -p PORT               the first iperf3 port (5201): "all" takes one per
#                         link, PORT to PORT+n-1
#   -e UP:DOWN            Mbit/s per link for the estimate (40:100)
#   -o DIR                where the results go (/tmp/fieldperf-DATE)
#   -y                    no question before it starts
#
# The results hold the addresses of the VPS and of the modems: send the
# directory (tar czf) to whoever reads it, never to a public place.
#
# SPDX-License-Identifier: GPL-2.0-only

ME=fieldperf
MODES="solo all tunnel"
LINKS=
VPS=
TADDR=10.79.0.1
STATUS=/var/run/cengarde.json
SECS=10
STREAMS=4
DIRS="up down"
PORT=5201
EST=40:100
OUT=
YES=
SERVERS=4

die() {
	echo "$ME: $*" >&2
	exit 1
}

num() {
	case $1 in '' | *[!0-9]*) die "$2: expected a number, got '$1'" ;; esac
}

# ---- the VPS ----

ports() {
	echo "$PORT" | awk -v n="$SERVERS" '{ for (i = 0; i < n; i++) print $1 + i }'
}

ufw_on() {
	command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q '^Status: active'
}

vps() {
	local p last
	[ "$(id -u)" = 0 ] || die "vps $1: as root (sudo)"
	last=$((PORT + SERVERS - 1))
	case $1 in
	on)
		if ! command -v iperf3 >/dev/null; then
			DEBIAN_FRONTEND=noninteractive apt-get install -y iperf3 >/dev/null || die "apt-get install iperf3"
		fi
		for p in $(ports); do
			systemctl is-active -q "$ME-$p" && continue
			systemd-run -q --unit="$ME-$p" -p DynamicUser=yes iperf3 -s -p "$p" || die "iperf3 on $p"
		done
		if ufw_on; then
			ufw allow "$PORT:$last/tcp" >/dev/null || die "ufw allow $PORT:$last/tcp"
		fi
		# With IP pass, every port from 1024 goes to the router but those
		# this box listens on: cengarde-nat reserves them when it syncs.
		# Its check then rewrites the engine's configuration with the new
		# reserved set (cengarde-vps-setup refresh): now, not at its next
		# 5-minute timer in the middle of a test, where it also undid hand
		# edits to that configuration.
		sleep 1
		if command -v cengarde-nat >/dev/null; then
			cengarde-nat sync >/dev/null || die "cengarde-nat sync"
			cengarde-nat check >/dev/null || die "cengarde-nat check"
		fi
		vps status
		echo "If the provider has a firewall of its own (Vultr's firewall groups), open TCP $PORT-$last there too."
		;;
	off)
		for p in $(ports); do
			systemctl stop "$ME-$p" 2>/dev/null
		done
		if ufw_on; then
			ufw delete allow "$PORT:$last/tcp" >/dev/null 2>&1
		fi
		if command -v cengarde-nat >/dev/null; then
			cengarde-nat sync >/dev/null || die "cengarde-nat sync"
			cengarde-nat check >/dev/null || die "cengarde-nat check"
		fi
		vps status
		;;
	status)
		for p in $(ports); do
			if systemctl is-active -q "$ME-$p"; then
				echo "iperf3 on $p: running"
			else
				echo "iperf3 on $p: stopped"
			fi
		done
		if ufw_on; then
			ufw status | grep -q "^$PORT:$last/tcp" && echo "ufw: $PORT:$last/tcp open" || echo "ufw: $PORT:$last/tcp closed"
		fi
		if command -v cengarde-nat >/dev/null; then
			for p in $(ports); do
				# "PROTO:FIRST" or "PROTO:FIRST-LAST", one per line
				cengarde-nat reserved | awk -F '[:-]' -v p="$p" '
					$1 == "tcp" { hi = $3 == "" ? $2 : $3
						if (p + 0 >= $2 + 0 && p + 0 <= hi + 0) found = 1 }
					END { printf "cengarde-nat: tcp %s %s\n", p,
						found ? "reserved (never forwarded to a router)" : "NOT reserved: with IP pass it goes to the router" }'
			done
		fi
		;;
	*) die "vps on|off|status" ;;
	esac
}

# ---- the links ----

# links_from_status: "DEVICE LABEL HOST STATE" per link of the engine's
# status, HOST the VPS address the link talks to (its "remote" without
# the port).
links_from_status() {
	[ -r "$STATUS" ] || die "no engine status in $STATUS (-S FILE, or -l and -s)"
	if command -v jsonfilter >/dev/null; then
		local i=0 n name label remote state
		n=$(jsonfilter -i "$STATUS" -e '@.links[*].id' 2>/dev/null | wc -l)
		while [ "$i" -lt "$n" ]; do
			name='' label='' remote='' state=''
			eval "$(jsonfilter -i "$STATUS" -e "name=@.links[$i].name" -e "label=@.links[$i].label" \
				-e "remote=@.links[$i].remote" -e "state=@.links[$i].state" 2>/dev/null)"
			echo "$name ${label:-$name} $(host_of "$remote") ${state:-?}"
			i=$((i + 1))
		done
	elif command -v python3 >/dev/null; then
		python3 -I -c '
import json, sys
for l in json.load(open(sys.argv[1])).get("links", []):
    print(l.get("name"), l.get("label") or l.get("name"), l.get("remote") or "-", l.get("state") or "?")
' "$STATUS" | while read -r name label remote state; do
			echo "$name $label $(host_of "$remote") $state"
		done
	else
		die "neither jsonfilter nor python3 to read $STATUS: give -l and -s"
	fi
}

# host_of ADDR:PORT or [ADDR]:PORT: the address alone ("-" without one).
host_of() {
	case $1 in
	'' | -) echo - ;;
	\[*)
		h=${1#\[}
		echo "${h%%\]*}"
		;;
	*:*:*) echo "$1" ;;
	*:*) echo "${1%:*}" ;;
	*) echo "$1" ;;
	esac
}

# pick_links: the links to test, as "DEVICE LABEL HOST" lines in $WORK/links.
pick_links() {
	local all want dev label host state hit
	if [ -r "$STATUS" ] || [ -z "$LINKS" ] || [ -z "$VPS" ]; then
		all=$(links_from_status) || exit 1
	fi
	: >"$WORK/links"
	if [ -z "$LINKS" ]; then
		echo "$all" | while read -r dev label host state; do
			[ -n "$dev" ] || continue
			[ "$state" = live ] || echo "$ME: $label ($dev) is $state, left out" >&2
			[ "$state" = live ] && echo "$dev $label ${VPS:-$host}" >>"$WORK/links"
		done
	else
		for want in $LINKS; do
			hit=
			if [ -n "$all" ]; then
				hit=$(echo "$all" | awk -v w="$want" '$1 == w || $2 == w { print $1, $2, $3; exit }')
			fi
			[ -n "$hit" ] || hit="$want $want -"
			# shellcheck disable=SC2086 # hit is split into its three words
			set -- $hit
			dev=$1 label=$2 host=${VPS:-$3}
			[ "$host" != - ] || die "$want: no VPS address for it (-s ADDR)"
			[ -n "$all" ] || ip link show dev "$dev" >/dev/null 2>&1 || die "$want: no such link"
			echo "$dev $label $host" >>"$WORK/links"
		done
	fi
	[ -s "$WORK/links" ] || die "no link to test"
	while read -r dev label host; do
		case $host in -) die "$label: no VPS address for it (-s ADDR)" ;; esac
	done <"$WORK/links"
	NLINKS=$(wc -l <"$WORK/links")
	[ "$NLINKS" -le "$SERVERS" ] || SERVERS=$NLINKS
}

# lower DEV: the device a VLAN (or any stacked device) sits on, if any here
# (a veth's peer in another namespace shows as "ifN").
lower() {
	local l
	l=$(ip -o link show dev "$1" 2>/dev/null | sed -n 's/^[0-9]*: [^@:]*@\([^:]*\):.*/\1/p')
	[ -n "$l" ] && [ -e "/sys/class/net/$l" ] && echo "$l"
}

# ---- the plan ----

plan() {
	local up="${EST%%:*}" down="${EST#*:}" mb m
	num "$up" "-e UP" && num "$down" "-e DOWN"
	echo "Links ($NLINKS):"
	while read -r dev label host; do
		echo "  $label ($dev) to $host"
	done <"$WORK/links"
	echo "Each test $SECS s, $STREAMS TCP streams, $DIRS; ping on every link it loads."
	echo "Estimate with $up Mbit/s up and $down down per link (an estimate, not a measure):"
	mb=0
	for m in $MODES; do
		case $m in
		solo) w="$NLINKS links one after another" n=$NLINKS ;;
		all) w="$NLINKS links at once (ports $PORT-$((PORT + NLINKS - 1)) on the VPS)" n=$NLINKS ;;
		# copying, each link carries the whole tunnel: the worst case
		tunnel) w="through the tunnel to $TADDR, as the engine is set (estimated as copying: every link carries it all)" n=$NLINKS ;;
		esac
		case " $DIRS " in *" up "*) ;; *) up=0 ;; esac
		case " $DIRS " in *" down "*) ;; *) down=0 ;; esac
		v=$(awk -v n="$n" -v u="$up" -v d="$down" -v s="$SECS" 'BEGIN { printf "%d", n * (u + d) * s / 8 }')
		echo "  $m: $w, about $v MB"
		mb=$((mb + v))
	done
	echo "  idle pings first, 5 s per link: next to nothing"
	tests=0
	for m in $MODES; do
		for w in $DIRS; do
			case $m in solo) tests=$((tests + NLINKS)) ;; *) tests=$((tests + 1)) ;; esac
		done
	done
	echo "Total: about $mb MB of mobile data, in about $((5 + tests * (SECS + 5))) s."
	echo "The VPS first: sudo sh fieldperf.sh vps on -p $PORT -n $SERVERS (vps off afterwards)."
	TOTAL_MB=$mb
}

# ---- the run ----

marca() {
	echo "$(date +%s) $(date '+%Y-%m-%d %H:%M:%S') $1" >>"$OUT/marcas.txt"
}

# snapshot DIR: the counters that say where a send was dropped: the UDP
# sockets' (snmp), each device's own (sysfs) and its queue's (tc, when
# installed: OpenWrt's tc-tiny).
snapshot() {
	local dev f
	mkdir -p "$1"
	cat /proc/net/snmp >"$1/snmp" 2>/dev/null
	[ -r /proc/net/snmp6 ] && cat /proc/net/snmp6 >"$1/snmp6"
	for dev in $DEVS; do
		for f in tx_packets tx_bytes tx_dropped tx_errors; do
			echo "$f $(cat "/sys/class/net/$dev/statistics/$f" 2>/dev/null)"
		done >"$1/stats-$dev.txt"
		command -v tc >/dev/null && tc -s qdisc show dev "$dev" >"$1/tc-$dev.txt" 2>&1
	done
	[ -r "$STATUS" ] && cp "$STATUS" "$1/status.json"
	date +%s >"$1/time"
}

# pinger DIR DEV HOST: pings HOST through DEV (no DEV: by the routes) for
# the length of a test.
pinger() {
	local six=
	case $3 in *:*) six=-6 ;; esac
	if [ -n "$2" ]; then
		ping $six -c $((SECS + 2)) -I "$2" "$3" >"$1/ping-$2.txt" 2>&1 </dev/null &
	else
		ping $six -c $((SECS + 2)) "$3" >"$1/ping-tunnel.txt" 2>&1 </dev/null &
	fi
}

# iperf DIR DEV HOST PORT up|down: one iperf3 client, JSON out.
iperf() {
	local rev='' bind=''
	[ "$5" = down ] && rev=-R
	[ -n "$2" ] && bind="--bind-dev $2"
	# shellcheck disable=SC2086 # rev and bind are empty or whole options
	iperf3 -c "$3" -p "$4" -t "$SECS" -P "$STREAMS" -J $rev $bind >"$1/iperf-${2:-tunnel}.json" \
		2>"$1/iperf-${2:-tunnel}.err" </dev/null &
}

# test MODE DIR [DEV LABEL HOST]: one test; without DEV every link.
run_test() {
	local mode="$1" way="$2" d dev label host i=0 tag
	N=$((N + 1))
	tag=$mode-$way${4:+-$4}
	d=$OUT/$(printf %02d "$N")-$tag
	mkdir -p "$d"
	snapshot "$d/before"
	{
		echo "mode=$mode"
		echo "dir=$way"
		echo "secs=$SECS"
		echo "streams=$STREAMS"
		echo "start=$(date +%s)"
	} >"$d/meta"
	marca "$tag"
	case $mode in
	solo)
		echo "link=$4 dev=$3 host=$5" >>"$d/meta"
		pinger "$d" "$3" "$5"
		iperf "$d" "$3" "$5" "$PORT" "$way"
		;;
	all)
		while read -r dev label host; do
			echo "link=$label dev=$dev host=$host" >>"$d/meta"
			pinger "$d" "$dev" "$host"
			iperf "$d" "$dev" "$host" $((PORT + i)) "$way"
			i=$((i + 1))
		done <"$WORK/links"
		;;
	tunnel)
		while read -r dev label host; do
			echo "link=$label dev=$dev host=$host" >>"$d/meta"
			pinger "$d" "$dev" "$host"
		done <"$WORK/links"
		pinger "$d" "" "$TADDR"
		iperf "$d" "" "$TADDR" "$PORT" "$way"
		;;
	esac
	wait
	echo "end=$(date +%s)" >>"$d/meta"
	snapshot "$d/after"
	printf '  %s:' "$tag"
	for f in "$d"/iperf-*.json; do
		awk '/"sum_received"/ { r = 1 } r && /"bits_per_second"/ { gsub(/[^0-9.e+]/, "", $2); printf " %.1f", $2 / 1e6; exit }' "$f"
	done
	echo " Mbit/s"
	sleep 3 # the queues drain before the next one
}

run() {
	local dev label host m
	command -v iperf3 >/dev/null || die "no iperf3: apk add iperf3 (OpenWrt 25.12) or opkg install iperf3"
	iperf3 --help 2>&1 | grep -q -- --bind-dev || die "this iperf3 has no --bind-dev (3.10 or later)"
	plan
	if [ -z "$YES" ]; then
		printf 'About %s MB of mobile data. Go on? [y/N] ' "$TOTAL_MB"
		read -r a
		case $a in y | Y | yes | s | S | si | sí) ;; *) die "nothing run" ;; esac
	fi
	mkdir -p "$OUT" || die "cannot write $OUT"
	cp "$WORK/links" "$OUT/links"
	DEVS=
	while read -r dev label host; do
		DEVS="$DEVS $dev $(lower "$dev")"
	done <"$WORK/links"
	DEVS=$(echo "$DEVS" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ')
	{
		echo "modes=$MODES"
		echo "secs=$SECS"
		echo "streams=$STREAMS"
		echo "tunnel=$TADDR"
		echo "devs=$DEVS"
		echo "version=$(cengarde version 2>/dev/null | head -n 1)"
		uname -a
		iperf3 --version 2>&1 | head -n 1
	} >"$OUT/plan"
	N=0
	echo "Idle pings (5 s)..."
	mkdir -p "$OUT/idle"
	while read -r dev label host; do
		case $host in *:*) ping -6 -c 5 -I "$dev" "$host" >"$OUT/idle/ping-$dev.txt" 2>&1 </dev/null & ;;
		*) ping -c 5 -I "$dev" "$host" >"$OUT/idle/ping-$dev.txt" 2>&1 </dev/null & ;;
		esac
	done <"$WORK/links"
	case " $MODES " in *" tunnel "*) ping -c 5 "$TADDR" >"$OUT/idle/ping-tunnel.txt" 2>&1 </dev/null & ;; esac
	wait
	for m in $MODES; do
		case $m in
		solo)
			while read -r dev label host; do
				for w in $DIRS; do
					run_test solo "$w" "$dev" "$label" "$host"
				done
			done <"$WORK/links"
			;;
		all | tunnel)
			for w in $DIRS; do
				run_test "$m" "$w"
			done
			;;
		esac
	done
	marca end
	echo "Done: $OUT"
	echo "To send it: tar czf $OUT.tgz -C $(dirname "$OUT") $(basename "$OUT")"
	echo "(it holds the addresses of the VPS and the modems)"
}

# ---- main ----

cmd=$1
[ $# -gt 0 ] && shift
if [ "$cmd" = vps ]; then
	sub=$1
	[ $# -gt 0 ] && shift
	while getopts p:n: o; do
		case $o in
		p) PORT=$OPTARG ;;
		n) SERVERS=$OPTARG ;;
		*) die "vps on|off|status [-p PORT] [-n SERVERS]" ;;
		esac
	done
	num "$PORT" -p
	num "$SERVERS" -n
	vps "$sub"
	exit 0
fi
case $cmd in
plan | run) ;;
*) sed -n '2,41s/^# \{0,1\}//p' "$0" >&2 && exit 2 ;;
esac
while getopts m:l:s:T:S:t:P:p:e:d:o:y o; do
	case $o in
	m) MODES=$OPTARG ;;
	l) LINKS=$OPTARG ;;
	s) VPS=$OPTARG ;;
	T) TADDR=$OPTARG ;;
	S) STATUS=$OPTARG ;;
	t) SECS=$OPTARG ;;
	P) STREAMS=$OPTARG ;;
	p) PORT=$OPTARG ;;
	e) EST=$OPTARG ;;
	d) DIRS=$OPTARG ;;
	o) OUT=$OPTARG ;;
	y) YES=1 ;;
	*) die "see: sh $0" ;;
	esac
done
num "$SECS" -t
num "$STREAMS" -P
num "$PORT" -p
[ -n "$MODES" ] || die "-m: nothing to run"
[ -n "$DIRS" ] || die "-d: no direction"
for w in $DIRS; do
	case $w in up | down) ;; *) die "-d: unknown direction '$w' (up, down)" ;; esac
done
for m in $MODES; do
	case $m in solo | all | tunnel) ;; *) die "-m: unknown mode '$m' (solo, all, tunnel)" ;; esac
done
[ -n "$OUT" ] || OUT=/tmp/$ME-$(date +%Y%m%d-%H%M%S)
WORK=$(mktemp -d) || die "mktemp"
trap 'rm -rf "$WORK"' EXIT
trap 'rm -rf "$WORK"; kill 0 2>/dev/null; exit 130' INT TERM
pick_links
$cmd
