# shellcheck shell=bash disable=SC2034 # ENGINE and NLINKS are read by lab.sh
# The bonding lab, step 0 of story 012: "bench/lab.sh bond".
#
# Real WireGuard through cengarde, real TCP inside: iperf3 (Cubic and BBR, up
# and down) between the two ends of the tunnel while ping measures the RTT
# under load, over links shaped with netem (one-way delay, rate, queue,
# events). It measures what the router's users would see, so that the
# bonding of Fase 5 has a baseline to beat: today's redundancy, and each
# case's best link alone. It judges nothing about speed; a run fails only
# when the plumbing does (no handshake, no traffic).
#
#   netns cli: wgc 10.79.0.2/30, WireGuard -> 127.0.0.1:59401 (cengarde client)
#   netns srv: wgs 10.79.0.1/30 on :59301 <- cengarde server (wireguard =)
#
# Cases (BOND_CASES), one-way delays per direction, so the RTT is twice:
#   equal       2 links, 50 Mbit/s, 25 ms; and the first link alone
#   het         3 links: 60 Mbit/s 20 ms, 30 Mbit/s 35 ms, 10 Mbit/s 60 ms;
#               and the first link alone
#   reorder50   2 links of 50 Mbit/s, 20 ms and 70 ms
#   reorder150  2 links of 50 Mbit/s, 20 ms and 170 ms
#   linkdeath   3 links of 40 Mbit/s, 25 ms; link 2 drops everything for the
#               middle third of the run
#   starlink15  link 1 like 5G (50 Mbit/s, 25 ms); link 2 like Starlink
#               (100 Mbit/s) whose delay alternates 20 and 50 ms every 15 s,
#               with 1.5 s of total loss at each switch (a supposed pattern,
#               story 012: the real one is still to be measured)
#   ltespike    link 1 (50 Mbit/s, 25 ms) jumps by 300 ms for 1 s every 5 s;
#               link 2 steady (50 Mbit/s, 30 ms)
#   deepq       link 1 at 20 Mbit/s behind 500 ms of queue, link 2 at
#               50 Mbit/s with 100 ms; both 25 ms
#
# Knobs: BOND_S (seconds per iperf3 run, 10; starlink15 takes at least 31),
# BOND_N (repetitions, 1), BOND_CC ("cubic bbr"), BOND_DIRS ("up down"),
# BOND_OUT (a file to append the JSON lines to). Needs iperf3, wg
# (wireguard-tools), ping and netem; WireGuard from the kernel, or
# wireguard-go named by WG_GO when the kernel has none. Without netem the
# links only get a rate (tbf) and the delays are skipped, which is said;
# BOND_NEED_NETEM=1 (the CI) makes that a failure instead.

BOND_S=${BOND_S:-10}
BOND_N=${BOND_N:-1}
BOND_CC=${BOND_CC:-cubic bbr}
BOND_DIRS=${BOND_DIRS:-up down}
BOND_CASES=${BOND_CASES:-equal het reorder50 reorder150 linkdeath starlink15 ltespike deepq}
BOND_OUT=${BOND_OUT:-}
WG_GO=${WG_GO:-}
BOND_NEED_NETEM=${BOND_NEED_NETEM:-0}

# bond_wg NETNS IF LISTEN ADDR KEYFILE PEERPUB PEERADDR [ENDPOINT]: a
# WireGuard interface in NETNS, from the kernel or from wireguard-go.
bond_wg() {
	local ns=$1 ifc=$2 port=$3 addr=$4 key=$5 pub=$6 peer=$7 ep=${8:-} i
	if ! ip -n "$ns" link add "$ifc" type wireguard 2>/dev/null; then
		if [ -z "$WG_GO" ]; then
			echo "FAIL: no WireGuard in the kernel: set WG_GO to a wireguard-go binary"
			return 1
		fi
		ip netns exec "$ns" env WG_PROCESS_FOREGROUND=1 "$WG_GO" "$ifc" >"$RUN/$ifc.log" 2>&1 &
		echo $! >"$RUN/$ifc.pid"
		for i in $(seq 1 50); do
			ip -n "$ns" link show "$ifc" >/dev/null 2>&1 && break
			sleep 0.1
		done
	fi
	ip netns exec "$ns" wg set "$ifc" listen-port "$port" private-key "$key" \
		peer "$pub" allowed-ips "$peer/32" ${ep:+endpoint "$ep" persistent-keepalive 25} || return 1
	ip -n "$ns" addr add "$addr/30" dev "$ifc"
	ip -n "$ns" link set "$ifc" mtu 1380 up
}

# bond_tunnel: the two WireGuard ends, with the client's peer pointing at the
# cengarde client and the server's learning the cengarde server's address.
bond_tunnel() {
	local i
	wg genkey >"$RUN/wgc.key" && wg genkey >"$RUN/wgs.key" || return 1
	chmod 600 "$RUN/wgc.key" "$RUN/wgs.key"
	bond_wg srv wgs 59301 10.79.0.1 "$RUN/wgs.key" "$(wg pubkey <"$RUN/wgc.key")" 10.79.0.2 || return 1
	bond_wg cli wgc 50000 10.79.0.2 "$RUN/wgc.key" "$(wg pubkey <"$RUN/wgs.key")" 10.79.0.1 127.0.0.1:59401 || return 1
	for i in $(seq 1 50); do
		ip netns exec cli ping -q -c 1 -W 1 10.79.0.1 >/dev/null 2>&1 && return 0
		sleep 0.2
	done
	echo "FAIL: no handshake through cengarde"
	return 1
}

bond_untunnel() {
	local f
	pkill -f "iperf3 -s -B 10.79.0.1" 2>/dev/null
	for f in "$RUN"/wgc.pid "$RUN"/wgs.pid; do
		[ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
		rm -f "$f"
	done
}

BOND_NETEM=
# bond_netem_ok: whether netem works here (once).
bond_netem_ok() {
	if [ -z "$BOND_NETEM" ]; then
		BOND_NETEM=no
		ip netns exec cli tc qdisc replace dev l1 root netem delay 1ms 2>/dev/null && BOND_NETEM=yes
		ip netns exec cli tc qdisc del dev l1 root 2>/dev/null
		[ "$BOND_NETEM" = yes ] || echo "   note: no netem here: links get a rate only, no delay, loss or events"
	fi
	[ "$BOND_NETEM" = yes ]
}

# bond_link N DELAY_MS RATE_MBIT [QUEUE_MS [LOSS_PCT]]: link N, both
# directions: one-way delay, rate, a queue of QUEUE_MS (100) at that rate.
bond_link() {
	local n=$1 d=$2 r=$3 q=${4:-100} loss=${5:-0} lim ns dev
	lim=$((r * 1000000 * q / 8000 / 1400 + 10))
	for ns in cli srv; do
		dev=$([ "$ns" = cli ] && echo "l$n" || echo "s$n")
		if bond_netem_ok; then
			ip netns exec "$ns" tc qdisc replace dev "$dev" root netem delay "${d}ms" rate "${r}mbit" limit "$lim" loss "${loss}%"
		else
			ip netns exec "$ns" tc qdisc replace dev "$dev" root tbf rate "${r}mbit" burst 32kb limit $((lim * 1500))
		fi
	done
}

# bond_events CASE SECS: what changes during a run (in the background).
bond_events() {
	local c=$1 s=$2 t
	bond_netem_ok || return 0
	case $c in
	linkdeath)
		sleep "$(awk -v s="$s" 'BEGIN { print s / 3 }')"
		bond_link 2 25 40 100 100
		sleep "$(awk -v s="$s" 'BEGIN { print s / 3 }')"
		bond_link 2 25 40
		;;
	starlink15)
		for t in $(seq 15 15 "$s"); do
			sleep 13.5
			bond_link 2 20 100 100 100
			sleep 1.5
			bond_link 2 "$([ $((t / 15 % 2)) = 1 ] && echo 50 || echo 20)" 100
		done
		;;
	ltespike)
		for t in $(seq 5 5 "$s"); do
			sleep 4
			bond_link 1 325 50
			sleep 1
			bond_link 1 25 50
		done
		;;
	esac
}

# bond_shape CASE: the links of a case, and its NLINKS (echoed).
bond_shape() {
	case $1 in
	equal) bond_link 1 25 50 && bond_link 2 25 50 ;;
	het) bond_link 1 20 60 && bond_link 2 35 30 && bond_link 3 60 10 ;;
	reorder50) bond_link 1 20 50 && bond_link 2 70 50 ;;
	reorder150) bond_link 1 20 50 && bond_link 2 170 50 ;;
	linkdeath) bond_link 1 25 40 && bond_link 2 25 40 && bond_link 3 25 40 ;;
	starlink15) bond_link 1 25 50 && bond_link 2 20 100 ;;
	ltespike) bond_link 1 25 50 && bond_link 2 30 50 ;;
	deepq) bond_link 1 25 20 500 && bond_link 2 25 50 ;;
	*) return 1 ;;
	esac
}

bond_nlinks() {
	case $1 in
	het | linkdeath) echo 3 ;;
	*) echo 2 ;;
	esac
}

# bond_run CASE VARIANT DIR CC REP SECS: one iperf3 run with ping alongside;
# appends a JSON line to $RUN/bond.jsonl and prints a short one.
bond_run() {
	local c=$1 v=$2 dir=$3 cc=$4 rep=$5 s=$6 cp sp c0 s0 c1 s1 pg ev rc=0
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec cli ping -n -i 0.1 -w $((s + 1)) 10.79.0.1 >"$RUN/ping.out" 2>&1 &
	pg=$!
	bond_events "$c" "$s" &
	ev=$!
	# shellcheck disable=SC2046 # -R or nothing
	ip netns exec cli iperf3 -c 10.79.0.1 -t "$s" -C "$cc" -J $([ "$dir" = down ] && echo -R) >"$RUN/iperf.json" 2>"$RUN/iperf.err" || rc=1
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	# Not a bare wait: the wireguard-go processes are children too.
	wait "$pg" "$ev"
	python3 - "$RUN/iperf.json" "$RUN/ping.out" "$c" "$v" "$dir" "$cc" "$rep" "$s" $((c1 - c0)) $((s1 - s0)) \
		"$(getconf CLK_TCK)" "$BOND_NETEM" >>"$RUN/bond.jsonl" <<'EOF' || rc=1
import json, re, sys
ipf, pingf, case, var, dirn, cc, rep, secs, ct, st, hz, netem = sys.argv[1:]
try:
    j = json.load(open(ipf))
    end = j["end"]
    mbps = end["sum_received"]["bits_per_second"] / 1e6
    retrans = end["sum_sent"].get("retransmits")
except Exception as e:
    mbps, retrans = 0.0, None
rtts = [float(m) for m in re.findall(r"time=([0-9.]+) ms", open(pingf).read())]
rtts.sort()
q = lambda p: rtts[min(len(rtts) - 1, int(p * len(rtts)))] if rtts else None
sent = len(re.findall(r"icmp_seq=", open(pingf).read()))
print(json.dumps({"case": case, "variant": var, "dir": dirn, "cc": cc, "rep": int(rep), "secs": int(secs),
                  "mbps": round(mbps, 2), "retrans": retrans, "rtt_p50": q(0.5), "rtt_p99": q(0.99),
                  "pings": len(rtts), "cpu_client_pct": round(100 * int(ct) / int(hz) / int(secs), 1),
                  "cpu_server_pct": round(100 * int(st) / int(hz) / int(secs), 1), "netem": netem == "yes"}))
sys.exit(0 if mbps > 0.5 and rtts else 1)
EOF
	tail -1 "$RUN/bond.jsonl" | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
print("   {case:<10} {variant:<6} {dir:<4} {cc:<5} #{rep} {mbps:7.1f} Mbit/s retrans {retrans} rtt p50 {rtt_p50} p99 {rtt_p99} ms cpu {cpu_client_pct}/{cpu_server_pct} %".format(**d))'
	[ "$rc" = 0 ] || echo "FAIL: $c $v $dir $cc #$rep: no traffic or no ping through the tunnel ($(head -c 200 "$RUN/iperf.err"))"
	return "$rc"
}

# bond_case CASE VARIANT: a fresh lab for one case (VARIANT solo: the first
# link alone), every direction, congestion control and repetition.
bond_case() {
	local c=$1 v=$2 s=$BOND_S dir cc rep fail=0
	[ "$c" = starlink15 ] && [ "$s" -lt 31 ] && s=31
	ENGINE=c
	NLINKS=$(bond_nlinks "$c")
	[ "$v" = solo ] && NLINKS=1
	setup && start || return 1
	if [ "$BOND_NEED_NETEM" = 1 ] && ! bond_netem_ok; then
		echo "FAIL: no netem (BOND_NEED_NETEM=1)"
		return 1
	fi
	bond_shape "$c" || return 1
	bond_tunnel || return 1
	ip netns exec srv iperf3 -s -B 10.79.0.1 -D || return 1
	sleep 0.5
	for rep in $(seq 1 "$BOND_N"); do
		for dir in $BOND_DIRS; do
			for cc in $BOND_CC; do
				bond_run "$c" "$v" "$dir" "$cc" "$rep" "$s" || fail=1
			done
		done
	done
	bond_untunnel
	teardown
	return "$fail"
}

# bond_table: medians per case, variant, direction and congestion control.
bond_table() {
	python3 - "$RUN/bond.jsonl" <<'EOF'
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
keys = []
for r in rows:
    k = (r["case"], r["variant"], r["dir"], r["cc"])
    if k not in keys:
        keys.append(k)
med = lambda xs: statistics.median(xs) if xs else None
print("| case | variant | dir | cc | n | Mbit/s | retrans | RTT p50 | RTT p99 | CPU cli/srv % |")
print("| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
for k in keys:
    rs = [r for r in rows if (r["case"], r["variant"], r["dir"], r["cc"]) == k]
    f = lambda name: med([r[name] for r in rs if r[name] is not None])
    fmt = lambda x, p=1: "-" if x is None else f"{x:.{p}f}"
    print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {len(rs)} | {fmt(f('mbps'))} | {fmt(f('retrans'), 0)} | "
          f"{fmt(f('rtt_p50'))} | {fmt(f('rtt_p99'))} | {fmt(f('cpu_client_pct'))}/{fmt(f('cpu_server_pct'))} |")
EOF
}

bond() {
	local c v fail=0 failed=""
	for tool in iperf3 wg ping; do
		command -v "$tool" >/dev/null || { echo "bond: needs $tool"; return 1; }
	done
	trap 'bond_untunnel; teardown' EXIT
	trap 'bond_untunnel; teardown; exit 130' INT TERM
	mkdir -p "$RUN"
	: >"$RUN/bond.jsonl"
	for c in $BOND_CASES; do
		for v in bonded solo; do
			case "$v:$c" in solo:equal | solo:het | bonded:*) ;; *) continue ;; esac
			echo "## $c ($v)"
			if ! (bond_case "$c" "$v"); then
				failed="$failed $c/$v"
				fail=1
			fi
			bond_untunnel
			teardown
		done
	done
	bond_table | tee "$RUN/bond.md"
	[ -n "$BOND_OUT" ] && cat "$RUN/bond.jsonl" >>"$BOND_OUT"
	if [ "$fail" = 0 ]; then
		echo "bond: ok"
	else
		echo "bond: FAILED:$failed"
	fi
	return "$fail"
}
