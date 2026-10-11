# shellcheck shell=bash disable=SC2034 # ENGINE and NLINKS are read by lab.sh
# The bonding lab, step 0 of story 012: "bench/lab.sh bond".
#
# Real WireGuard through cengarde, real TCP inside: iperf3 (Cubic and BBR, up
# and down) between the two ends of the tunnel while ping measures the RTT
# under load, over links shaped with netem (one-way delay, rate, queue,
# events). Next to TCP goes a QUIC sender, bench/quicperf.py (aioquic): same
# links, same direction and ping, cc "quic". It measures what the router's
# users would see, so that the bonding of Fase 5 has a baseline to beat:
# today's redundancy, and each case's best link alone. It judges nothing
# about speed; a run fails only when the plumbing does (no handshake, no
# traffic).
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
#   starlink15sw  not in the default list: starlink15 with the capacities swapped
#               (link 1 100 Mbit/s, link 2 50 Mbit/s), the same delays and
#               cuts. What a cut costs when the link left has room for the
#               flow, to set against starlink15 (story 012: the retransmissions
#               of starlink15 come from the cuts, not from the redundancy)
#   ltespike    link 1 (50 Mbit/s, 25 ms) jumps by 300 ms for 1 s every 5 s;
#               link 2 steady (50 Mbit/s, 30 ms)
#   deepq       link 1 at 20 Mbit/s behind 500 ms of queue, link 2 at
#               50 Mbit/s with 100 ms; both 25 ms
#
# Knobs: BOND_S (seconds per run, 10; starlink15 takes at least 31), BOND_N
# (repetitions, 1; the table then gives the median and the spread), BOND_CC
# ("cubic bbr quic"; quic only when aioquic imports, else a note),
# BOND_NEED_QUIC (1: no aioquic is a failure, the CI), BOND_QUIC_CC (cubic or
# reno, what the QUIC sender runs) and BOND_QUIC_MTU (QUIC datagram, 1350: the
# tunnel is 1380), BOND_DIRS ("up down"), BOND_OUT (a file to append the JSON
# lines to). Needs iperf3, wg (wireguard-tools), ping and netem; WireGuard
# from the kernel, or wireguard-go named by WG_GO when the kernel has none.
# Without netem the links only get a rate (tbf) and the delays are skipped,
# which is said; BOND_NEED_NETEM=1 (the CI) makes that a failure instead.
# In the JSON lines, "retrans" is TCP's retransmissions or, for quic, the
# packets the sender declared lost ("spurious": the ones that arrived after all).

BOND_S=${BOND_S:-10}
BOND_N=${BOND_N:-1}
BOND_CC=${BOND_CC:-} # empty: cubic bbr quic (bond_ccs)
BOND_QUIC_CC=${BOND_QUIC_CC:-cubic}
BOND_QUIC_MTU=${BOND_QUIC_MTU:-1350}
BOND_DIRS=${BOND_DIRS:-up down}
BOND_CASES=${BOND_CASES:-equal het reorder50 reorder150 linkdeath starlink15 ltespike deepq}
BOND_OUT=${BOND_OUT:-}
WG_GO=${WG_GO:-}
BOND_NEED_NETEM=${BOND_NEED_NETEM:-0}
BOND_NEED_QUIC=${BOND_NEED_QUIC:-0}

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
	for f in "$RUN"/wgc.pid "$RUN"/wgs.pid "$RUN"/quic.pid; do
		[ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
		rm -f "$f"
	done
}

# bond_quic_ok: whether bench/quicperf.py can run here (aioquic imports).
bond_quic_ok() { python3 -I -c 'import aioquic, cryptography' 2>/dev/null; }

# bond_ccs: settles BOND_CC. Empty means cubic, bbr and quic; quic goes, with a
# note, when aioquic does not import, and that is a failure with BOND_NEED_QUIC=1.
bond_ccs() {
	local cc out="" quic=0
	bond_quic_ok && quic=1
	if [ "$BOND_NEED_QUIC" = 1 ] && [ "$quic" = 0 ]; then
		echo "FAIL: no aioquic (BOND_NEED_QUIC=1): pip install aioquic==1.2.0"
		return 1
	fi
	for cc in ${BOND_CC:-cubic bbr quic}; do
		if [ "$cc" = quic ] && [ "$quic" = 0 ]; then
			echo "   note: no aioquic here (pip install aioquic==1.2.0): no quic runs"
			continue
		fi
		out="$out $cc"
	done
	BOND_CC=${out# }
	[ -n "$BOND_CC" ] || { echo "FAIL: no congestion control to run"; return 1; }
}

# bond_quic_server: the QUIC server on the tunnel's server side, up before
# the first run (it prints "listening" once it is bound).
bond_quic_server() {
	local i
	ip netns exec srv python3 -I "$LAB/quicperf.py" server --bind 10.79.0.1 \
		--cc "$BOND_QUIC_CC" --mtu "$BOND_QUIC_MTU" >"$RUN/quic-server.log" 2>&1 &
	echo $! >"$RUN/quic.pid"
	for i in $(seq 1 50); do
		grep -q listening "$RUN/quic-server.log" 2>/dev/null && return 0
		sleep 0.1
	done
	echo "FAIL: the QUIC server did not start ($(head -c 200 "$RUN/quic-server.log"))"
	return 1
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
	local c=$1 s=$2 t r2=100
	bond_netem_ok || return 0
	case $c in
	linkdeath)
		sleep "$(awk -v s="$s" 'BEGIN { print s / 3 }')"
		bond_link 2 25 40 100 100
		sleep "$(awk -v s="$s" 'BEGIN { print s / 3 }')"
		bond_link 2 25 40
		;;
	starlink15 | starlink15sw)
		[ "$c" = starlink15sw ] && r2=50
		for t in $(seq 15 15 "$s"); do
			sleep 13.5
			bond_link 2 20 "$r2" 100 100
			sleep 1.5
			bond_link 2 "$([ $((t / 15 % 2)) = 1 ] && echo 50 || echo 20)" "$r2"
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
	starlink15sw) bond_link 1 25 100 && bond_link 2 20 50 ;;
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

# bond_run CASE VARIANT DIR CC REP SECS: one run (iperf3, or quicperf for cc
# quic) with ping alongside; appends a JSON line to $RUN/bond.jsonl and prints
# a short one.
bond_run() {
	local c=$1 v=$2 dir=$3 cc=$4 rep=$5 s=$6 cp sp c0 s0 c1 s1 pg ev res err rc=0
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec cli ping -n -i 0.1 -w $((s + 1)) 10.79.0.1 >"$RUN/ping.out" 2>&1 &
	pg=$!
	bond_events "$c" "$s" &
	ev=$!
	if [ "$cc" = quic ]; then
		res=$RUN/quic.json err=$RUN/quic.err
		ip netns exec cli python3 -I "$LAB/quicperf.py" client 10.79.0.1 --dir "$dir" --secs "$s" \
			--cc "$BOND_QUIC_CC" --mtu "$BOND_QUIC_MTU" >"$res" 2>"$err" || rc=1
	else
		res=$RUN/iperf.json err=$RUN/iperf.err
		# shellcheck disable=SC2046 # -R or nothing
		ip netns exec cli iperf3 -c 10.79.0.1 -t "$s" -C "$cc" -J $([ "$dir" = down ] && echo -R) >"$res" 2>"$err" || rc=1
	fi
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	# Not a bare wait: the wireguard-go processes are children too.
	wait "$pg" "$ev"
	python3 - "$res" "$RUN/ping.out" "$c" "$v" "$dir" "$cc" "$rep" "$s" $((c1 - c0)) $((s1 - s0)) \
		"$(getconf CLK_TCK)" "$BOND_NETEM" >>"$RUN/bond.jsonl" <<'EOF' || rc=1
import json, re, sys
resf, pingf, case, var, dirn, cc, rep, secs, ct, st, hz, netem = sys.argv[1:]
# What does not apply to a kind of run stays null: TCP does not say how many
# packets it sent, and QUIC has no "retransmits" but the packets it declared lost.
extra = {"pkts_sent": None, "spurious": None, "srtt_ms": None, "min_rtt_ms": None, "quic_cc": None}
try:
    j = json.load(open(resf))
    if cc == "quic":
        mbps, retrans = j["mbps"], j["lost"]
        extra = {"pkts_sent": j["sent"], "spurious": j["spurious"], "srtt_ms": j["srtt_ms"],
                 "min_rtt_ms": j["min_rtt_ms"], "quic_cc": j["cc"]}
    else:
        end = j["end"]
        mbps = end["sum_received"]["bits_per_second"] / 1e6
        retrans = end["sum_sent"].get("retransmits")
        snd = end["streams"][0]["sender"]  # the sender's TCP_INFO RTTs, in microseconds
        us = lambda k: round(snd[k] / 1000, 2) if snd.get(k) else None
        extra.update(srtt_ms=us("mean_rtt"), min_rtt_ms=us("min_rtt"))
except Exception as e:
    mbps, retrans = 0.0, None
rtts = [float(m) for m in re.findall(r"time=([0-9.]+) ms", open(pingf).read())]
rtts.sort()
q = lambda p: rtts[min(len(rtts) - 1, int(p * len(rtts)))] if rtts else None
print(json.dumps({"case": case, "variant": var, "dir": dirn, "cc": cc, "rep": int(rep), "secs": int(secs),
                  "mbps": round(mbps, 2), "retrans": retrans, **extra, "rtt_p50": q(0.5), "rtt_p99": q(0.99),
                  "pings": len(rtts), "cpu_client_pct": round(100 * int(ct) / int(hz) / int(secs), 1),
                  "cpu_server_pct": round(100 * int(st) / int(hz) / int(secs), 1), "netem": netem == "yes"}))
sys.exit(0 if mbps > 0.5 and rtts else 1)
EOF
	tail -1 "$RUN/bond.jsonl" | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
sp = "" if d["spurious"] is None else " (spurious {})".format(d["spurious"])
print("   {case:<10} {variant:<6} {dir:<4} {cc:<5} #{rep} {mbps:7.1f} Mbit/s retrans {retrans}".format(**d) + sp +
      " rtt p50 {rtt_p50} p99 {rtt_p99} ms cpu {cpu_client_pct}/{cpu_server_pct} %".format(**d))'
	[ "$rc" = 0 ] || echo "FAIL: $c $v $dir $cc #$rep: no traffic or no ping through the tunnel ($(head -c 200 "$err"))"
	return "$rc"
}

# bond_case CASE VARIANT: a fresh lab for one case (VARIANT solo: the first
# link alone), every direction, congestion control and repetition.
bond_case() {
	local c=$1 v=$2 s=$BOND_S dir cc rep fail=0
	case $c in starlink15*) [ "$s" -lt 31 ] && s=31 ;; esac
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
	case " $BOND_CC " in *" quic "*) bond_quic_server || return 1 ;; esac
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

# bond_table: per case, variant, direction and congestion control. With one
# repetition, the value; with more, the median and, in parentheses, the p25 to
# p75 range (the minimum to the maximum when there are fewer than 4). The
# spurious column appears when some run (QUIC) counted them.
bond_table() {
	python3 - "$RUN/bond.jsonl" <<'EOF'
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
keys = []
for r in rows:
    k = (r["case"], r["variant"], r["dir"], r["cc"])
    if k not in keys:
        keys.append(k)
spur = any(r.get("spurious") is not None for r in rows)
many = any(sum(1 for r in rows if (r["case"], r["variant"], r["dir"], r["cc"]) == k) > 1 for k in keys)

def cell(xs, p=1, spread=True):
    """The median and, with several values, (p25-p75), or (min-max) with fewer than 4."""
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return "-"
    num = lambda x: f"{x:.{p}f}"
    if len(xs) == 1 or not spread:
        return num(statistics.median(xs))
    if len(xs) < 4:
        lo, hi = xs[0], xs[-1]
    else:
        q = statistics.quantiles(xs, n=4, method="inclusive")
        lo, hi = q[0], q[2]
    return f"{num(statistics.median(xs))} ({num(lo)}–{num(hi)})"

head = ["case", "variant", "dir", "cc", "n", "Mbit/s", "retrans"] + (["spurious"] if spur else []) + ["RTT p50", "RTT p99", "CPU cli/srv %"]
print("| " + " | ".join(head) + " |")
print("| " + " | ".join(["---"] * 4 + ["---:"] * (len(head) - 4)) + " |")
for k in keys:
    rs = [r for r in rows if (r["case"], r["variant"], r["dir"], r["cc"]) == k]
    col = lambda name, p=1: cell([r.get(name) for r in rs], p)
    med = lambda name: cell([r.get(name) for r in rs], spread=False)
    cells = [k[0], k[1], k[2], k[3], str(len(rs)), col("mbps"), col("retrans", 0)] + ([col("spurious", 0)] if spur else []) + \
        [col("rtt_p50"), col("rtt_p99"), f"{med('cpu_client_pct')}/{med('cpu_server_pct')}"]
    print("| " + " | ".join(cells) + " |")
if many or spur:
    print()
    notes = []
    if many:
        notes.append("n > 1: median (p25–p75), or (min–max) with n < 4.")
    if spur:
        notes.append("retrans of quic: packets declared lost; spurious: of those, the ones acknowledged afterwards.")
    print(" ".join(notes))
EOF
}

bond() {
	local c v fail=0 failed=""
	for tool in iperf3 wg ping; do
		command -v "$tool" >/dev/null || { echo "bond: needs $tool"; return 1; }
	done
	bond_ccs || return 1
	trap 'bond_untunnel; teardown' EXIT
	trap 'bond_untunnel; teardown; exit 130' INT TERM
	mkdir -p "$RUN"
	: >"$RUN/bond.jsonl"
	echo "bond: $BOND_CC, $BOND_N repetition(s) of $BOND_S s"
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
