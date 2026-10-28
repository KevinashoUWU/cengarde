#!/bin/bash
# The ceiling capture test (story 029), from the PC of the testnet: plain-text
# numbered records (bench/techo.c) from the PC to the VPS and back, over each
# modem at once, then bonded by GRIT, then as UDP at fixed rates, with every
# packet captured at both ends, each end's log of every record, TCP's and the
# engine's state and the radio of each modem, on one wall clock (chrony).
#
#   bench/techo.sh [-n NAME] [-d up|down|both] [-p tcp,grit,udp] [-T SECS]
#                  [-S STEP_SECS] [-u 50,80,100,120] [-r SIZE] [-k]
#
#   -n  the run's name (a directory in ~/campo/DATE/techo/), DATE-TIME by default
#   -d  directions: up (PC to VPS), down, or both (default)
#   -p  phases, in order (default tcp,grit,udp):
#       tcp   one TCP connection per modem (vlan10..vlan40, the PC's table per
#             modem), BBR, SECS seconds: each modem's ceiling, a bonding ideal
#       grit  one TCP connection from the netns nucc behind the NUC: the
#             protocontroller hands it to GRIT over the four uplinks
#       udp   one UDP flow per modem at each share of that modem's TCP ceiling
#             (-u, in %), STEP_SECS each: short queues to deep ones
#   -T  seconds of tcp and grit (30), -S of each udp step (20)
#   -r  record size (1300: one record per TCP segment on every path)
#   -k  keep the offloads off and the VPS's server up at the end (another run)
#
# What it touches, and puts back on exit: TSO/GSO/GRO off on the PC's eno2
# and vlan10..40 and on the VPS's NIC (one wire packet per record in the
# captures), a ufw rule for TCP 5310 and UDP 5320 on the VPS, tcpdump on the
# NUC (installed if missing). The NUC's tunnel and the OMR VM keep running:
# their own traffic on the modems is small, but it is there.
#
# Files (DIR = ~/campo/DATE/techo/NAME):
#   PHASE-DIR/pc-vlanNN.pcap, vps.pcap     direct: headers and the record's
#                                          first bytes (snaplen 128)
#   PHASE-DIR/nuc-DEV.pcap, vps.pcap       grit: the tunnel's packets whole
#   PHASE-DIR/pc-LINK*.log, vps-*.log      techo's logs (see techo.c)
#   PHASE-DIR/ss-pc.txt, ss-vps.txt        ss -tin at 20 Hz (direct tcp)
#   PHASE-DIR/ctl-nuc.jsonl, ctl-vps.jsonl cengarde ctl status at 10 Hz (grit)
#   radio.jsonl                            evm.at answers of each modem, 1 Hz
#   clock-*.txt                            chronyc tracking at both ends,
#                                          before and after
#   rates.txt                              each modem's ceiling, Mbit/s
#   README.md                              what ran, and the data it used
#
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck disable=SC2024,SC2029 # files written by this shell; remote words expanded here on purpose
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
VPS_IP=64.176.14.91
VPS=vps # ssh alias (root)
NUC=(ssh -o BatchMode=yes -o HostKeyAlias=10.79.173.140 root@192.168.50.1)
TPORT=5310
UPORT=5320
LINKS=(1 2 3 4)
DEVS=(vlan10 vlan20 vlan30 vlan40)
NAMES=(ENTEL WOM MOVISTAR CLARO)
NUC_DEVS=(ENTEL WOM MOVISTAR CLARO)
# The modems as evm.at knows them: CPE alias and modem name.
RADIOS=("cpe1 entel" "cpe1 wom" "cpe2 movistar" "cpe3 claro")
NETNS=nucc

name=$(date +%F-%H%M) dirs=both phases=tcp,grit,udp secs=30 step=20 shares=50,80,100,120 rec=1300 keep=0
while getopts "n:d:p:T:S:u:r:k" o; do
	case $o in
	n) name=$OPTARG ;;
	d) dirs=$OPTARG ;;
	p) phases=$OPTARG ;;
	T) secs=$OPTARG ;;
	S) step=$OPTARG ;;
	u) shares=$OPTARG ;;
	r) rec=$OPTARG ;;
	k) keep=1 ;;
	*) exit 2 ;;
	esac
done
case $dirs in up) dirs="up" ;; down) dirs="down" ;; both) dirs="up down" ;; *) echo "techo.sh: -d up|down|both" >&2; exit 2 ;; esac

DIR=~/campo/$(date +%F)/techo/$name
VDIR=/root/techo/$name
BIN=$DIR/bin/techo
mkdir -p "$DIR/bin"
PIDS=()

say() { echo "techo.sh: $*" | tee -a "$DIR/run.log"; }

# cap_local NAME IFACE SNAP FILTER: tcpdump on the PC into DIR/NAME.pcap
# (written by the shell: tcpdump's own profile may not write here).
cap_local() {
	sudo -n sh -c "echo \$\$ > '$DIR/.cap-$1.pid'; exec tcpdump -i '$2' -s '$3' -U -w - '$4' 2>/dev/null" >"$DIR/$1.pcap" &
}
cap_local_stop() { sudo -n kill "$(cat "$DIR/.cap-$1.pid")" 2>/dev/null; rm -f "$DIR/.cap-$1.pid"; }

# cap_vps NAME SNAP FILTER: tcpdump on the VPS's NIC into VDIR/NAME.pcap.
cap_vps() {
	ssh "$VPS" "cd $VDIR && nohup sh -c 'echo \$\$ > .cap-$1.pid; exec tcpdump -i $VIF -s $2 -U -w - \"$3\" > $1.pcap 2>/dev/null' >/dev/null 2>&1 &"
}
cap_vps_stop() { ssh "$VPS" "cd $VDIR && kill \$(cat .cap-$1.pid) 2>/dev/null; rm -f .cap-$1.pid"; }

# cap_nuc DEV FILE: the NUC's uplink DEV, streamed to the PC over the LAN.
cap_nuc() {
	"${NUC[@]}" "echo \$\$ > /tmp/techo-cap-$1.pid; exec tcpdump -i $1 -s 0 -U -w - udp port 65500 2>/dev/null" >"$2" &
}
cap_nuc_stop() { "${NUC[@]}" "kill \$(cat /tmp/techo-cap-$1.pid) 2>/dev/null; rm -f /tmp/techo-cap-$1.pid"; }

clock() { # TAG: chronyc tracking at each end
	{ date +%s.%N; chronyc tracking; } >"$DIR/clock-pc-$1.txt" 2>&1
	ssh "$VPS" 'date +%s.%N; chronyc tracking' >"$DIR/clock-vps-$1.txt" 2>&1
	"${NUC[@]}" 'date +%s; chronyc tracking' >"$DIR/clock-nuc-$1.txt" 2>&1
}

offloads() { # on|off, PC and VPS
	local d
	for d in eno2 "${DEVS[@]}"; do
		sudo -n /usr/sbin/ethtool -K "$d" tso "$1" gso "$1" gro "$1" 2>/dev/null
	done
	ssh "$VPS" "ethtool -K $VIF tso $1 gso $1 gro $1"
}

# Each modem's serving cell and carriers every second, through evm.at (the
# daemon that owns the AT ports), one JSON line per answer.
radio_start() {
	local r h m
	for r in "${RADIOS[@]}"; do
		read -r h m <<<"$r"
		ssh -o BatchMode=yes "$h" "M=$m sh -s" >>"$DIR/radio.jsonl" 2>/dev/null <<-'EOF' &
			while :; do
				for c in 'AT+QENG=\"servingcell\"' 'AT+QCAINFO'; do
					a=$(ubus call evm.at send "{\"modem\":\"$M\",\"cmd\":\"$c\"}" | tr -d '\n\t')
					echo "{\"t\":$(date +%s),\"modem\":\"$M\",\"answer\":${a:-null}}"
				done
				sleep 1
			done
		EOF
		PIDS+=($!)
	done
}

# ss_start / ss_stop PHASE-DIR: ss -tin at 20 Hz on the PC and the VPS.
ss_start() {
	sudo -n sh -c "echo \$\$ > '$DIR/.ss.pid'; while :; do date +%s.%N; ss -tinH '( dport = :$TPORT or sport = :$TPORT )'; sleep 0.05; done" >"$DIR/$1/ss-pc.txt" 2>/dev/null &
	ssh "$VPS" "cd $VDIR/$1 && nohup sh -c 'echo \$\$ > .ss.pid; while :; do date +%s.%N; ss -tinH \"( sport = :$TPORT or dport = :$TPORT )\"; sleep 0.05; done > ss-vps.txt' >/dev/null 2>&1 &"
}
ss_stop() {
	sudo -n kill "$(cat "$DIR/.ss.pid")" 2>/dev/null
	ssh "$VPS" "cd $VDIR/$1 && kill \$(cat .ss.pid) 2>/dev/null; rm -f .ss.pid"
}

# ctl_start / ctl_stop PHASE-DIR: cengarde ctl status at 10 Hz, the NUC fed
# from here (its busybox sleeps whole seconds), the VPS on its own.
ctl_start() {
	(while :; do echo; sleep 0.1; done | "${NUC[@]}" 'while read -r x; do cengarde ctl status; done' >"$DIR/$1/ctl-nuc.jsonl") &
	CTL_NUC=$!
	ssh "$VPS" "cd $VDIR/$1 && nohup sh -c 'echo \$\$ > .ctl.pid; while :; do cengarde ctl status; sleep 0.1; done > ctl-vps.jsonl' >/dev/null 2>&1 &"
}
ctl_stop() {
	kill "$CTL_NUC" 2>/dev/null
	pkill -P "$CTL_NUC" 2>/dev/null
	ssh "$VPS" "cd $VDIR/$1 && kill \$(cat .ctl.pid) 2>/dev/null; rm -f .ctl.pid"
}

prepare() {
	VIF=$(ssh "$VPS" "ip route show default | awk '{print \$5; exit}'")
	[ -n "$VIF" ] || { echo "techo.sh: no VPS" >&2; exit 1; }
	say "run $name in $DIR: directions $dirs, phases $phases, $secs s, udp steps $step s at $shares %, records of $rec bytes"
	gcc -O2 -Wall -Wextra -pthread -o "$BIN" "$HERE/techo.c" || exit 1
	ssh "$VPS" "mkdir -p $VDIR" && scp -q "$HERE/techo.c" "$VPS:$VDIR/" &&
		ssh "$VPS" "gcc -O2 -pthread -o $VDIR/techo $VDIR/techo.c" || exit 1
	ssh "$VPS" "ufw insert 1 allow $TPORT/tcp comment techo >/dev/null; ufw insert 1 allow $UPORT/udp comment techo >/dev/null"
	ssh "$VPS" "cd $VDIR && nohup sh -c 'echo \$\$ > .srv.pid; exec ./techo serve -t $TPORT -u $UPORT -r $rec -o srv 2> srv.err' >/dev/null 2>&1 &"
	"${NUC[@]}" 'command -v tcpdump >/dev/null || apk add tcpdump >/dev/null 2>&1 || opkg install tcpdump >/dev/null 2>&1; command -v tcpdump' >/dev/null ||
		say "no tcpdump on the NUC: grit runs without its captures"
	offloads off
	clock before
	radio_start
	ssh "$VPS" "cengarde version" >"$DIR/versions.txt"
	"${NUC[@]}" "cengarde version; uci show cengarde.main | grep -v secret" >>"$DIR/versions.txt"
}

finish() {
	local p
	trap - EXIT INT TERM
	for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done # the CPEs' loops end with their ssh
	clock after
	if [ "$keep" = 0 ]; then
		offloads on
		ssh "$VPS" "cd $VDIR && kill \$(cat .srv.pid) 2>/dev/null; rm -f .srv.pid; ufw delete allow $TPORT/tcp >/dev/null; ufw delete allow $UPORT/udp >/dev/null"
	fi
	say "pulling the VPS's files"
	rsync -a --remove-source-files "$VPS:$VDIR/" "$DIR/vps/" 2>/dev/null || scp -rq "$VPS:$VDIR/" "$DIR/vps/"
	report
}

# Mbit/s of records in a techo log (32-byte entries) over secs.
mbits() { python3 -c 'import os,sys; print("%.1f" % (os.path.getsize(sys.argv[1]) / 32 * int(sys.argv[2]) * 8 / float(sys.argv[3]) / 1e6))' "$1" "$rec" "$2"; }

phase_tcp() { # up|down
	local d=$1 p="tcp-$1" i
	mkdir -p "$DIR/$p"
	ssh "$VPS" "mkdir -p $VDIR/$p"
	for i in "${!DEVS[@]}"; do cap_local "$p/pc-${DEVS[$i]}" "${DEVS[$i]}" 128 "tcp port $TPORT"; done
	cap_vps "$p/vps" 128 "tcp port $TPORT"
	ss_start "$p"
	sleep 1
	say "$p: four TCP BBR connections, one per modem, $secs s"
	for i in "${!DEVS[@]}"; do
		sudo -n "$BIN" "tcp-$d" -s "$VPS_IP" -t "$TPORT" -l "${LINKS[$i]}" -i "${DEVS[$i]}" -r "$rec" -T "$secs" \
			-o "$DIR/$p/pc-L${LINKS[$i]}.log" 2>>"$DIR/$p/pc.err" &
		CLIENTS+=($!)
	done
	wait_clients
	sleep 2
	ss_stop "$p"
	for i in "${!DEVS[@]}"; do cap_local_stop "$p/pc-${DEVS[$i]}"; done
	cap_vps_stop "$p/vps"
	for i in "${!DEVS[@]}"; do
		RATE[i]=$(mbits "$DIR/$p/pc-L${LINKS[$i]}.log" "$secs")
		echo "$p ${NAMES[$i]} ${RATE[$i]}" >>"$DIR/rates.txt"
	done
	say "$p: ${NAMES[*]} = ${RATE[*]} Mbit/s"
}

phase_grit() { # up|down
	local d=$1 p="grit-$1" i
	mkdir -p "$DIR/$p"
	ssh "$VPS" "mkdir -p $VDIR/$p"
	for i in "${!NUC_DEVS[@]}"; do cap_nuc "${NUC_DEVS[$i]}" "$DIR/$p/nuc-${NUC_DEVS[$i]}.pcap"; done
	cap_vps "$p/vps" 0 "udp port 65500"
	ctl_start "$p"
	sleep 1
	say "$p: one TCP connection from $NETNS, GRIT over the four uplinks, $secs s"
	sudo -n ip netns exec "$NETNS" "$BIN" "tcp-$d" -s "$VPS_IP" -t "$TPORT" -l 0 -r "$rec" -T "$secs" \
		-o "$DIR/$p/pc-L0.log" 2>>"$DIR/$p/pc.err" &
	CLIENTS+=($!)
	wait_clients
	sleep 2
	ctl_stop "$p"
	for i in "${!NUC_DEVS[@]}"; do cap_nuc_stop "${NUC_DEVS[$i]}"; done
	cap_vps_stop "$p/vps"
	echo "$p bonded $(mbits "$DIR/$p/pc-L0.log" "$secs")" >>"$DIR/rates.txt"
	say "$p: $(mbits "$DIR/$p/pc-L0.log" "$secs") Mbit/s"
}

phase_udp() { # up|down
	local d=$1 p="udp-$1" i s r
	mkdir -p "$DIR/$p"
	ssh "$VPS" "mkdir -p $VDIR/$p"
	for i in "${!DEVS[@]}"; do cap_local "$p/pc-${DEVS[$i]}" "${DEVS[$i]}" 128 "udp port $UPORT"; done
	cap_vps "$p/vps" 128 "udp port $UPORT"
	sleep 1
	for s in ${shares//,/ }; do
		say "$p: $s % of each modem's TCP ceiling, $step s"
		for i in "${!DEVS[@]}"; do
			r=$(awk -v c="$(awk -v p="tcp-$d" -v n="${NAMES[$i]}" '$1 == p && $2 == n { print $3 }' "$DIR/rates.txt" | tail -n 1)" \
				-v s="$s" 'BEGIN { r = c * s / 100; print (r > 1 ? r : 1) }')
			sudo -n "$BIN" "udp-$d" -s "$VPS_IP" -u "$UPORT" -l "${LINKS[$i]}" -i "${DEVS[$i]}" -r "$rec" -R "$r" -T "$step" \
				-o "$DIR/$p/pc-L${LINKS[$i]}-$s.log" 2>>"$DIR/$p/pc.err" &
			CLIENTS+=($!)
		done
		wait_clients
		sleep 2
	done
	for i in "${!DEVS[@]}"; do cap_local_stop "$p/pc-${DEVS[$i]}"; done
	cap_vps_stop "$p/vps"
}

wait_clients() { # the techo clients started since the last wait
	local j
	for j in "${CLIENTS[@]}"; do wait "$j" 2>/dev/null; done
	CLIENTS=()
}

report() {
	local used
	used=$(du -cb "$DIR"/*/pc-L*.log 2>/dev/null | tail -n 1 | awk -v r="$rec" '{ printf "%.2f", $1 / 32 * r / 1e9 }')
	{
		echo "# techo $name"
		echo
		echo "- Directions: $dirs; phases: $phases; $secs s per phase, udp steps of $step s at $shares %."
		echo "- Records of $rec bytes; versions: $(head -n 2 "$DIR/versions.txt" | tr '\n' ' ')"
		echo "- Data on the modems (records only, the PC's logs): ~$used GB"
		echo
		echo '```'
		cat "$DIR/rates.txt" 2>/dev/null
		echo '```'
	} >"$DIR/README.md"
	say "done: $DIR (~$used GB of records)"
}

CTL_NUC=0 VIF=
CLIENTS=()
declare -a RATE
trap finish EXIT
trap 'exit 1' INT TERM
prepare
for d in $dirs; do
	for ph in ${phases//,/ }; do
		case $ph in
		tcp) phase_tcp "$d" ;;
		grit) phase_grit "$d" ;;
		udp) phase_udp "$d" ;;
		*) say "phase $ph: not one (tcp, grit, udp)" ;;
		esac
	done
done
