# shellcheck shell=bash disable=SC2034 # NLINKS is read by lab.sh
# TCP over the native tunnel, tcp_aware off and on: "bench/lab.sh tcpaware".
#
# The field finding of story 014 (2026-10-16): with tcp_aware, the VPS's TCP
# made 135 times the fast retransmissions, many of them spurious (DSACK). A
# retransmission marked free went ahead of what the receiver's reorderer
# held behind a gap, sent before it, and TCP's RACK took all of that for
# lost and sent it again. This measures it: iperf3 -R (the server's TCP
# sends, the client's reorderer holds) with TCPA_P flows for TCPA_S
# seconds over links shaped with netem, and the sender's kernel counters
# (nstat in the srv namespace) next to the reorderer's (the client's
# status file).
#
# TCPA_LINKS: one "DELAY_MS:RATE_MBIT:LOSS_PCT" per link (one-way, both
# directions, 100 ms of queue); TCPA_SPIKE "N:EXTRA_MS:ON_S:EVERY_S": link N
# EXTRA_MS slower for ON_S seconds every EVERY_S. TCPA_VARIANTS: off
# (tcp_aware = no), on (tcp_aware = yes at both ends); any other word W runs
# with the settings in the variable TCPA_CONF_W ("key = value;..."), e.g.
# TCPA_CONF_m20="tcp_aware = yes;reorder_margin_ms = 20".
# TCPA_CONF goes to both ends in every variant (reorder_max_wait_ms = 500, as
# on the testnet), TCPA_CONF_CLIENT to the client only (split = on: the
# server then splits the download too). TCPA_N repetitions, each variant in
# turn. CENGARDE_BIN as everywhere (a build of another commit for a
# before). Needs iperf3, nstat, ping and netem.
#
# One line per run: Mbit/s, the sender's TcpExt counters (fast
# retransmissions, DSACKs received, lost retransmissions, reordering seen by
# timestamps) and its RetransSegs, the median RTT of a 1200-byte ping under
# load, and the client reorderer's gaps given up (rack, cap, retx), late
# packets, packets bypassed and its wait histogram (reorder.h's edges).

TCPA_S=${TCPA_S:-20}
TCPA_P=${TCPA_P:-4}
TCPA_N=${TCPA_N:-1}
TCPA_LINKS=${TCPA_LINKS:-20:50:0.5 30:50:0.5}
TCPA_VARIANTS=${TCPA_VARIANTS:-off on}
TCPA_CONF=${TCPA_CONF:-reorder_max_wait_ms = 500}
TCPA_CONF_CLIENT=${TCPA_CONF_CLIENT:-split = on}
TCPA_SPIKE=${TCPA_SPIKE:-}

# tcpa_link N DELAY_MS RATE_MBIT LOSS_PCT: link N, both directions, with
# 100 ms of queue beyond what its delay holds.
tcpa_link() {
	local n=$1 d=$2 r=$3 loss=$4 lim ns dev
	lim=$((r * 1000000 * (100 + d) / 8000 / 1400 + 10))
	for ns in "$NS_CLI" "$NS_SRV"; do
		dev=$([ "$ns" = "$NS_CLI" ] && echo "l$n" || echo "s$n")
		ip netns exec "$ns" tc qdisc replace dev "$dev" root netem delay "${d}ms" rate "${r}mbit" \
			limit "$lim" loss "${loss}%" || return 1
	done
}

tcpa_links() {
	local n=0 l d r loss
	for l in $TCPA_LINKS; do
		n=$((n + 1))
		IFS=: read -r d r loss <<<"$l"
		tcpa_link "$n" "$d" "$r" "$loss" || return 1
	done
}

# tcpa_spikes: with TCPA_SPIKE "N:EXTRA_MS:ON_S:EVERY_S", link N's delay
# grows by EXTRA_MS for ON_S seconds every EVERY_S (a modem whose queue
# jumps, as bond's ltespike), for TCPA_S seconds; in the background.
tcpa_spikes() {
	local n x on every d r loss t=0
	[ -n "$TCPA_SPIKE" ] || return 0
	IFS=: read -r n x on every <<<"$TCPA_SPIKE"
	IFS=: read -r d r loss <<<"$(echo "$TCPA_LINKS" | tr ' ' '\n' | sed -n "${n}p")"
	while [ "$(awk -v t="$t" -v e="$every" -v s="$TCPA_S" 'BEGIN { print (t + e <= s) }')" = 1 ]; do
		sleep "$(awk -v e="$every" -v o="$on" 'BEGIN { print e - o }')"
		tcpa_link "$n" $((d + x)) "$r" "$loss"
		sleep "$on"
		tcpa_link "$n" "$d" "$r" "$loss"
		t=$(awk -v t="$t" -v e="$every" 'BEGIN { print t + e }')
	done
}

# tcpa_run VARIANT REP: one run, one line.
tcpa_run() {
	local v=$1 rep=$2 conf mbit nst ping spikes p50
	case $v in
	off) conf="tcp_aware = no" ;;
	on) conf="tcp_aware = yes" ;;
	*)
		conf=TCPA_CONF_$v
		conf=${!conf:-}
		[ -n "$conf" ] || { echo "FAIL: variant $v: no TCPA_CONF_$v"; return 1; }
		;;
	esac
	CLIENT_EXTRA="status_interval_ms = 200;$TCPA_CONF;$TCPA_CONF_CLIENT;$conf"
	SERVER_EXTRA="status_interval_ms = 200;$TCPA_CONF;$conf"
	setup && start || return 1
	tcpa_links || return 1
	ip netns exec "$NS_SRV" iperf3 -s -B 10.79.0.1 -D -I "$RUN/iperf3.pid" || return 1
	sleep 0.5
	nst=$RUN/nstat.srv
	rm -f "$nst"
	ip netns exec "$NS_SRV" env NSTAT_HISTORY="$nst" nstat -n
	ip netns exec "$NS_CLI" ping -q -s 1200 -i 0.1 -w "$TCPA_S" 10.79.0.1 >"$RUN/ping.out" 2>&1 &
	ping=$!
	tcpa_spikes &
	spikes=$!
	timeout $((TCPA_S + 15)) ip netns exec "$NS_CLI" iperf3 -c 10.79.0.1 -R -P "$TCPA_P" -t "$TCPA_S" \
		--connect-timeout 3000 -J >"$RUN/iperf3.json" 2>&1
	wait "$ping" "$spikes"
	ip netns exec "$NS_SRV" env NSTAT_HISTORY="$nst" nstat -az >"$RUN/nstat.out"
	sleep 0.5 # the status file, every 200 ms
	[ -f "$RUN/iperf3.pid" ] && kill "$(cat "$RUN/iperf3.pid")" 2>/dev/null
	rm -f "$RUN/iperf3.pid"
	mbit=$(python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print("%.1f" % (d["end"]["sum_received"]["bits_per_second"] / 1e6))' \
		"$RUN/iperf3.json" 2>/dev/null)
	p50=$(sed -n 's/.*= [0-9.]*\/\([0-9.]*\)\/.*/\1/p' "$RUN/ping.out")
	[ -n "$mbit" ] || { echo "FAIL: $v #$rep: no iperf3 result: $(tail -c 300 "$RUN/iperf3.json")"; tail -3 "$RUN/server.log"; teardown; return 1; }
	python3 - "$v" "$rep" "$mbit" "${p50:-?}" "$RUN/nstat.out" "$RUN/client.json" <<-'EOF'
		import json, sys
		v, rep, mbit, p50, nf, cf = sys.argv[1:]
		n = {}
		for line in open(nf):
		    f = line.split()
		    if len(f) >= 2 and f[1].isdigit():
		        n[f[0]] = int(f[1])
		r = json.load(open(cf))["reorder"]
		lost = r["lost"]
		print("%-6s #%s %7s Mbit/s  fastre %6d  dsack %5d  lostre %5d  tsreord %5d  retrans %6d  rtt_p50 %6s ms"
		      "  | lost rack %d cap %d retx %s  late %d  bypassed %d  wait %s"
		      % (v, rep, mbit, n.get("TcpExtTCPFastRetrans", 0), n.get("TcpExtTCPDSACKRecv", 0),
		         n.get("TcpExtTCPLostRetransmit", 0), n.get("TcpExtTCPTSReorder", 0), n.get("TcpRetransSegs", 0),
		         p50, lost["rack"], lost["cap"], lost.get("retx", "-"),
		         sum(r["late"].values()), r["bypassed"], "/".join(str(x) for x in r["wait"])))
	EOF
	teardown
}

tcpaware() {
	local tool rep v fail=0
	for tool in iperf3 nstat ping python3; do
		command -v "$tool" >/dev/null || { echo "tcpaware: needs $tool"; return 1; }
	done
	NLINKS=$(echo "$TCPA_LINKS" | wc -w)
	trap 'pkill -f "iperf3 -s -B 10.79.0.1" 2>/dev/null; teardown' EXIT
	trap 'pkill -f "iperf3 -s -B 10.79.0.1" 2>/dev/null; teardown; exit 130' INT TERM
	echo "   links $TCPA_LINKS (delay ms:Mbit/s:loss %)${TCPA_SPIKE:+, spike $TCPA_SPIKE}, iperf3 -R -P $TCPA_P -t $TCPA_S, $TCPA_CONF, client $TCPA_CONF_CLIENT, $CENGARDE_BIN"
	for rep in $(seq 1 "$TCPA_N"); do
		for v in $TCPA_VARIANTS; do
			tcpa_run "$v" "$rep" || fail=1
		done
	done
	[ "$fail" = 0 ] && echo "tcpaware: ok" || echo "tcpaware: FAILED"
	return "$fail"
}
