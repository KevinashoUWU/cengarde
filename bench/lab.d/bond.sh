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
#   ltespikerand  not in the default list: ltespike with the jumps 2 to 10 s
#               apart at random (the same draw for every variant at the same
#               repetition, from BOND_SEED and the repetition): a radio's
#               stalls do not keep time, and every 5 s exactly an engine whose
#               own cycle fell in step with it looked better than it was (the
#               old empty split of G4 copied everything just when each jump
#               came; 2026-10-17)
#   deepq       link 1 at 20 Mbit/s behind 500 ms of queue, link 2 at
#               50 Mbit/s with 100 ms; both 25 ms
#   spikeloss   not in the default list: 2 links of 50 Mbit/s, 25 and 30 ms,
#               each losing 1 % at random, link 1 jumping by 80 ms for 0.5 s
#               every 2 s: a modem whose radio loses and retransmits (HARQ),
#               its queue jumping and draining, as Entel on its lone B2; the
#               rescue should take it for lossy, not full (the experiment of
#               PR #51, kept in history)
#   lossy       not in the default list: 3 links (60 Mbit/s 20 ms, 30 Mbit/s
#               35 ms, 50 Mbit/s 25 ms), each losing 1 % at random in both
#               directions: the loss on every link of a split packet that the
#               rescue (rescue.h) answers; run it with CLIENT_EXTRA
#               "rescue_wait_ms = 60" and SERVER_EXTRA "rescue_kb = 32768"
#   bloat       bufferbloat as on the Pi's 5G modems (story 012, 2026-10-13):
#               3 links of similar delay (20, 20 and 22 ms) and unequal rate
#               (30, 20 and 8 Mbit/s) behind deep queues (1, 1 and 2 s). A
#               split with equal weights fills the 8 Mbit/s queue first; what
#               the delay guard does then is what this case is for
#
# Variants (BOND_VARIANTS): what runs in each case. The engine settings of a
# variant go to both ends through CLIENT_EXTRA and SERVER_EXTRA (yours, if any,
# come after them).
#   bonded       today's redundancy with the engine's defaults, what CI runs:
#                the same as redund, the name the step 1 tables use
#   solo         the first link alone (NLINKS 1), in the cases of BOND_SOLO_CASES
#   redund       the defaults
#   redund-ro0   reorder = no
#   redund-dw20  reorder_dup_wait_ms = 20
#   split        split = force, mute_behind_ms = 0, reorder_max_wait_ms = 400
#                (muting would take the slow link out of reorder150 and hide
#                the spread); the engine's delay guard (split_spread_ms, 30
#                by default) still takes it out of the share in reorder50
#                and reorder150, as in the field: split-sp0 is without it
#   splitnoro    split, with reorder = no: the control of split
#   NAME-bpN     any of them with reorder_bypass = N
#   NAME-spN     any of them with split_spread_ms = N; split-sp0 is the split
#                without the delay guard, the raw round-robin (the engine's
#                default, 30, keeps a link 50 ms behind out of the share)
#   NAME-gov     any of them with governor = yes: each link's weight from its
#                own queue (govern.h) at both ends (split-sp250-gov against
#                split-sp250)
#   NAME-nc      any of them with split_spread_copy = no: a link the delay
#                guard keeps out carries no copies, and one link left in the
#                share takes it all (split-sp30-nc against split-sp30)
# Suffixes go in any order, each once (split-sp0-bp64, split-sp250-nc).
# BOND_VARIANTS empty is "bonded solo", solo in equal and het only: exactly the
# runs from before the variants (the CI's time estimate counts them). With
# BOND_VARIANTS given, solo also runs in reorder50 and reorder150, the
# baselines of split; BOND_SOLO_CASES says it by hand.
#
# Knobs: BOND_S (seconds per run, 10; starlink15 takes at least 31), BOND_N
# (repetitions, 1; the table then gives the median and the spread), BOND_REP0
# (0: the first repetition's number minus one, for ltespikerand's draw and the
# lines' rep; bench/labq.py runs repetition r alone as BOND_N=1 BOND_REP0=r-1), BOND_P
# (TCP flows in parallel, 1; speedtest opens many, and many flows starting
# at once fill the links' queues as one does not; story 013), BOND_CC
# ("cubic bbr quic"; quic only when aioquic imports, else a note),
# BOND_NEED_QUIC (1: no aioquic is a failure, the CI), BOND_QUIC_CC (cubic or
# reno, what the QUIC sender runs) and BOND_QUIC_MTU (QUIC datagram, 1350: the
# tunnel is 1380), BOND_DIRS ("up down"), BOND_OUT (a file to append the JSON
# lines to), BOND_SMALL (pps of a flow of datagrams of BOND_SMALL_SIZE bytes,
# 160, through the tunnel alongside the transfer, 0: none) and
# BOND_NETEM_INFLIGHT (1: netem's limit also covers the packets in the delay,
# see bond_link; 0 is the step 0 baseline), BOND_WARM and BOND_REST (the start
# from rest, story 013: before each run, BOND_WARM seconds of the same transfer
# and BOND_REST of nothing, so the run starts on links measured and then left
# idle; 0: none) and BOND_KEEP (a directory to keep each run's iperf3 JSON in,
# with intervals of 0.5 s: the ramp of its first seconds). Needs iperf3, wg
# (wireguard-tools), ping and netem; WireGuard from the kernel, or
# wireguard-go named by WG_GO when the kernel has none.
# Without netem the links only get a rate (tbf) and the delays are skipped,
# which is said; BOND_NEED_NETEM=1 (the CI) makes that a failure instead.
#
# Each run takes the engine's status files and the WireGuard interfaces' counters
# before and after (the engine writes them every second: a wait of 1.1 s per run;
# the after of a run is the before of the next), and its JSON line carries the
# differences: "eng" for cli and srv (the reorder counters, too_old,
# wireguard_drops and lost_all; null from an engine without them; and what
# the end sent: data packets, their sends over all its links and the sends a
# full queue refused, which the table shows as sends per packet and send
# drops at the end that sent the bulk) and "wg_rx_dropped". A second ping, of 1200-byte packets (the ones a reorderer
# holds: the 84-byte one bypasses it), gives rtt_big_p50 and rtt_big_p99.
# "retrans" is TCP's retransmissions or, for quic, the packets the sender
# declared lost ("spurious": the ones that arrived after all); retrans_pct is
# its rate, over the segments the sender wrote or the packets it sent. The
# tables and their rules are bondtab.py's (bondtab_test.py tests them).

BOND_S=${BOND_S:-10}
BOND_N=${BOND_N:-1}
BOND_REP0=${BOND_REP0:-0} # the repetitions are numbered from BOND_REP0 + 1 (labq.py runs one at a time)
BOND_P=${BOND_P:-1}
BOND_CC=${BOND_CC:-} # empty: cubic bbr quic (bond_ccs)
BOND_QUIC_CC=${BOND_QUIC_CC:-cubic}
BOND_QUIC_MTU=${BOND_QUIC_MTU:-1350}
BOND_DIRS=${BOND_DIRS:-up down}
BOND_CASES=${BOND_CASES:-equal het reorder50 reorder150 linkdeath starlink15 ltespike deepq bloat}
BOND_VARIANTS=${BOND_VARIANTS:-} # empty: bonded solo (bond_settle)
BOND_SOLO_CASES=${BOND_SOLO_CASES:-} # empty: equal het (bond_settle)
BOND_SMALL=${BOND_SMALL:-0}
BOND_SMALL_SIZE=${BOND_SMALL_SIZE:-160}
BOND_NETEM_INFLIGHT=${BOND_NETEM_INFLIGHT:-0}
BOND_OUT=${BOND_OUT:-}
BOND_WARM=${BOND_WARM:-0}
BOND_REST=${BOND_REST:-0}
BOND_KEEP=${BOND_KEEP:-}
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
	bond_wg "$NS_SRV" wgs 59301 10.79.0.1 "$RUN/wgs.key" "$(wg pubkey <"$RUN/wgc.key")" 10.79.0.2 || return 1
	bond_wg "$NS_CLI" wgc 50000 10.79.0.2 "$RUN/wgc.key" "$(wg pubkey <"$RUN/wgs.key")" 10.79.0.1 127.0.0.1:59401 || return 1
	for i in $(seq 1 50); do
		ip netns exec "$NS_CLI" ping -q -c 1 -W 1 10.79.0.1 >/dev/null 2>&1 && return 0
		sleep 0.2
	done
	echo "FAIL: no handshake through cengarde"
	return 1
}

bond_untunnel() {
	local f
	ns_kill "$NS_SRV" iperf3
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

# bond_settle: fills BOND_VARIANTS and BOND_SOLO_CASES when they are empty.
bond_settle() {
	if [ -z "$BOND_VARIANTS" ]; then
		BOND_VARIANTS="bonded solo"
		BOND_SOLO_CASES=${BOND_SOLO_CASES:-equal het}
	else
		BOND_SOLO_CASES=${BOND_SOLO_CASES:-equal het reorder50 reorder150}
	fi
}

# bond_variant NAME: the settings of a variant: BV_CLIENT and BV_SERVER
# ("key = value;..."), BV_SOLO (1: the first link alone). 1 for a name it does not know.
bond_variant() {
	local v=$1 bp='' sp='' nc='' gov='' s
	BV_CLIENT='' BV_SERVER='' BV_SOLO=0
	# The suffixes, from the last: -bpN, -spN, -nc and -gov, each once.
	while :; do
		s=${v##*-}
		case $s in
		bp[0-9]* | sp[0-9]*)
			case ${s#??} in *[!0-9]*) return 1 ;; esac
			case $s in
			bp*) [ -z "$bp" ] || return 1; bp=${s#bp} ;;
			*) [ -z "$sp" ] || return 1; sp=${s#sp} ;;
			esac
			;;
		nc) [ -z "$nc" ] || return 1; nc=1 ;;
		gov) [ -z "$gov" ] || return 1; gov=1 ;;
		*) break ;;
		esac
		v=${v%-*}
	done
	case $v in
	bonded | redund) ;;
	redund-ro0) BV_CLIENT="reorder = no" ;;
	redund-dw20) BV_CLIENT="reorder_dup_wait_ms = 20" ;;
	split) BV_CLIENT="split = force;mute_behind_ms = 0;reorder_max_wait_ms = 400" ;;
	splitnoro) BV_CLIENT="split = force;mute_behind_ms = 0;reorder_max_wait_ms = 400;reorder = no" ;;
	solo) BV_SOLO=1 ;;
	*) return 1 ;;
	esac
	[ -n "$bp" ] && BV_CLIENT="${BV_CLIENT:+$BV_CLIENT;}reorder_bypass = $bp"
	[ -n "$sp" ] && BV_CLIENT="${BV_CLIENT:+$BV_CLIENT;}split_spread_ms = $sp"
	[ -n "$nc" ] && BV_CLIENT="${BV_CLIENT:+$BV_CLIENT;}split_spread_copy = no"
	[ -n "$gov" ] && BV_CLIENT="${BV_CLIENT:+$BV_CLIENT;}governor = yes"
	BV_SERVER=$BV_CLIENT
	return 0
}

# bond_applies CASE VARIANT: whether the variant runs in the case (solo, in the
# cases of BOND_SOLO_CASES only).
bond_applies() {
	[ "${2%%-*}" = solo ] || return 0
	case " $BOND_SOLO_CASES " in *" $1 "*) return 0 ;; esac
	return 1
}

# bond_variants_check: every variant is known, and the engine takes its settings
# ("cengarde -t" on a config like the lab's: a typo would otherwise cost a whole run).
bond_variants_check() {
	local v end f
	mkdir -p "$RUN"
	for v in $BOND_VARIANTS; do
		bond_variant "$v" || {
			echo "FAIL: unknown variant $v (bonded solo redund redund-ro0 redund-dw20 split splitnoro, and any with -bpN, -spN, -nc or -gov)"
			return 1
		}
		[ -x "$CENGARDE_BIN" ] || continue
		for end in client server; do
			f=$RUN/check-$end.conf
			if [ "$end" = client ]; then
				printf 'mode = client\nkey = %s\nlisten = 127.0.0.1:59401\nserver = 10.0.1.2:59402\ninterfaces = none\n%s\n' \
					"$KEY" "$(echo "$BV_CLIENT" | tr ';' '\n')" >"$f"
			else
				printf 'mode = server\nkey = %s\nlisten = *:59402\nwireguard = 127.0.0.1:59301\n%s\n' \
					"$KEY" "$(echo "$BV_SERVER" | tr ';' '\n')" >"$f"
			fi
			"$CENGARDE_BIN" -t -c "$f" >"$RUN/check.out" 2>&1 || {
				echo "FAIL: the engine does not take the settings of $v at the $end: $(head -c 200 "$RUN/check.out")"
				return 1
			}
		done
	done
	rm -f "$RUN"/check-*.conf "$RUN/check.out"
}

# bond_join A B: A;B, or whichever is not empty.
bond_join() { echo "${1:+$1${2:+;}}$2"; }

# bond_quic_server: the QUIC server on the tunnel's server side, up before
# the first run (it prints "listening" once it is bound).
bond_quic_server() {
	local i
	ip netns exec "$NS_SRV" python3 -I "$LAB/quicperf.py" server --bind 10.79.0.1 \
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
		ip netns exec "$NS_CLI" tc qdisc replace dev l1 root netem delay 1ms 2>/dev/null && BOND_NETEM=yes
		ip netns exec "$NS_CLI" tc qdisc del dev l1 root 2>/dev/null
		[ "$BOND_NETEM" = yes ] || echo "   note: no netem here: links get a rate only, no delay, loss or events"
	fi
	[ "$BOND_NETEM" = yes ]
}

# bond_link N DELAY_MS RATE_MBIT [QUEUE_MS [LOSS_PCT]]: link N, both
# directions: one-way delay, rate, a queue of QUEUE_MS (100) at that rate.
# netem counts the packets in its delay against its limit: a link of 170 ms and
# 50 Mbit/s holds 760 packets of 1400 bytes in flight, more than the 456 of the
# limit for a 100 ms queue, and drops what does not fit (about 30 Mbit/s get
# through, the queue being what is left of the limit). The step 0 tables are
# like that, the redundancy hiding it behind the other link; a split has no
# other copy. BOND_NETEM_INFLIGHT=1 adds to the limit what the delay holds, so
# that QUEUE_MS is the queue beyond it (it changes the step 0 baseline).
bond_link() {
	local n=$1 d=$2 r=$3 q=${4:-100} loss=${5:-0} lim ns dev inflight=0
	lim=$((r * 1000000 * q / 8000 / 1400 + 10))
	[ "${BOND_NETEM_INFLIGHT:-0}" = 1 ] && inflight=$((r * 1000000 * d / 8000 / 1400))
	for ns in "$NS_CLI" "$NS_SRV"; do
		dev=$([ "$ns" = "$NS_CLI" ] && echo "l$n" || echo "s$n")
		if bond_netem_ok; then
			ip netns exec "$ns" tc qdisc replace dev "$dev" root netem delay "${d}ms" rate "${r}mbit" limit $((lim + inflight)) loss "${loss}%"
		else
			ip netns exec "$ns" tc qdisc replace dev "$dev" root tbf rate "${r}mbit" burst 32kb limit $((lim * 1500))
		fi
	done
}

# bond_events CASE SECS [REP]: what changes during a run (in the background).
bond_events() {
	local c=$1 s=$2 rep=${3:-1} t r2=100 gap
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
	ltespikerand)
		RANDOM=$((${BOND_SEED:-1} * 1000 + rep))
		t=0
		while :; do
			gap=$((2 + RANDOM % 9)) # 2 to 10 s between jumps
			t=$((t + gap))
			[ "$t" -lt "$s" ] || break
			sleep $((gap - 1))
			bond_link 1 325 50
			sleep 1
			bond_link 1 25 50
		done
		;;
	spikeloss)
		for t in $(seq 2 2 "$s"); do
			sleep 1.5
			bond_link 1 105 50 100 1
			sleep 0.5
			bond_link 1 25 50 100 1
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
	ltespike | ltespikerand) bond_link 1 25 50 && bond_link 2 30 50 ;;
	spikeloss) bond_link 1 25 50 100 1 && bond_link 2 30 50 100 1 ;;
	deepq) bond_link 1 25 20 500 && bond_link 2 25 50 ;;
	bloat) bond_link 1 20 30 1000 && bond_link 2 20 20 1000 && bond_link 3 22 8 2000 ;;
	lossy) bond_link 1 20 60 100 1 && bond_link 2 35 30 100 1 && bond_link 3 25 50 100 1 ;;
	*) return 1 ;;
	esac
}

bond_nlinks() {
	case $1 in
	het | linkdeath | bloat | lossy) echo 3 ;;
	*) echo 2 ;;
	esac
}

# bond_snap PREFIX: the engine's status files and the WireGuard interfaces'
# counters now, as bondtab.py reads them (PREFIX.client.json, .server.json,
# .wgc.json and .wgs.json; an empty file for what could not be read).
bond_snap() {
	cp "$RUN/client.json" "$1.client.json" 2>/dev/null || : >"$1.client.json"
	cp "$RUN/server.json" "$1.server.json" 2>/dev/null || : >"$1.server.json"
	ip -n "$NS_CLI" -s -j link show dev wgc >"$1.wgc.json" 2>/dev/null || : >"$1.wgc.json"
	ip -n "$NS_SRV" -s -j link show dev wgs >"$1.wgs.json" 2>/dev/null || : >"$1.wgs.json"
}

# bond_snap_roll: the after of a run is the before of the next.
bond_snap_roll() {
	local f
	for f in client server wgc wgs; do
		mv -f "$RUN/snap-after.$f.json" "$RUN/snap-before.$f.json"
	done
}

# bond_small_start DIR SECS: BOND_SMALL pps of BOND_SMALL_SIZE-byte datagrams
# (udpgen) through the tunnel, towards where DIR sends; their pids go to BOND_BG.
bond_small_start() {
	local tx rx txa rxa
	if [ "$1" = up ]; then
		tx=$NS_CLI rx=$NS_SRV txa=10.79.0.2 rxa=10.79.0.1
	else
		tx=$NS_SRV rx=$NS_CLI txa=10.79.0.1 rxa=10.79.0.2
	fi
	rm -f "$RUN/small-tx.out" "$RUN/small-rx.out"
	ip netns exec "$rx" "$BIN/udpgen" -b "$rxa:7002" -d $(($2 + 1)) -g 1 >"$RUN/small-rx.out" &
	BOND_BG="$BOND_BG $!"
	sleep 0.2
	ip netns exec "$tx" "$BIN/udpgen" -b "$txa:7001" -p "$rxa:7002" -r "$BOND_SMALL" -s "$BOND_SMALL_SIZE" \
		-d "$2" -g 0 >"$RUN/small-tx.out" &
	BOND_BG="$BOND_BG $!"
}

# bond_run CASE VARIANT DIR CC REP SECS: one run (iperf3, or quicperf for cc
# quic) with two pings alongside (84 and 1200 bytes); appends a JSON line to
# $RUN/bond.jsonl and prints a short one.
bond_run() {
	local c=$1 v=$2 dir=$3 cc=$4 rep=$5 s=$6 cp sp c0 s0 c1 s1 res err rc=0 small=()
	cp=$(cat "$RUN/client.pid") sp=$(cat "$RUN/server.pid")
	if [ "$BOND_REST" -gt 0 ] && [ "$cc" != quic ]; then
		# shellcheck disable=SC2046 # -R or nothing
		[ "$BOND_WARM" -gt 0 ] && ip netns exec "$NS_CLI" iperf3 -c 10.79.0.1 -t "$BOND_WARM" -C "$cc" -P "$BOND_P" \
			$([ "$dir" = down ] && echo -R) >/dev/null 2>&1
		sleep "$BOND_REST"
		bond_snap "$RUN/snap-before"
		[ -n "$BOND_KEEP" ] && mkdir -p "$BOND_KEEP" && cp "$RUN/server.json" "$BOND_KEEP/$c-$v-$dir-$cc-$rep.rest-server.json"
	fi
	BOND_BG=
	c0=$(cpu_ticks "$cp") s0=$(cpu_ticks "$sp")
	ip netns exec "$NS_CLI" ping -n -i 0.1 -w $((s + 1)) 10.79.0.1 >"$RUN/ping.out" 2>&1 &
	BOND_BG="$BOND_BG $!"
	ip netns exec "$NS_CLI" ping -n -i 0.1 -s 1200 -w $((s + 1)) 10.79.0.1 >"$RUN/pingbig.out" 2>&1 &
	BOND_BG="$BOND_BG $!"
	bond_events "$c" "$s" "$rep" &
	BOND_BG="$BOND_BG $!"
	if [ "$BOND_SMALL" -gt 0 ]; then
		bond_small_start "$dir" "$s"
		small=(--small-tx "$RUN/small-tx.out" --small-rx "$RUN/small-rx.out")
	fi
	if [ "$cc" = quic ]; then
		res=$RUN/quic.json err=$RUN/quic.err
		ip netns exec "$NS_CLI" python3 -I "$LAB/quicperf.py" client 10.79.0.1 --dir "$dir" --secs "$s" \
			--cc "$BOND_QUIC_CC" --mtu "$BOND_QUIC_MTU" >"$res" 2>"$err" || rc=1
	else
		res=$RUN/iperf.json err=$RUN/iperf.err
		# shellcheck disable=SC2046 # -R or nothing
		ip netns exec "$NS_CLI" iperf3 -c 10.79.0.1 -t "$s" -C "$cc" -P "$BOND_P" -J ${BOND_KEEP:+-i 0.5} \
			$([ "$dir" = down ] && echo -R) >"$res" 2>"$err" || rc=1
		[ -n "$BOND_KEEP" ] && mkdir -p "$BOND_KEEP" && cp "$res" "$BOND_KEEP/$c-$v-$dir-$cc-$rep.json"
	fi
	c1=$(cpu_ticks "$cp") s1=$(cpu_ticks "$sp")
	# The engine writes its status every second: what it counted until now is in
	# the file after this wait (which overlaps the end of the pings).
	sleep 1.1
	bond_snap "$RUN/snap-after"
	# Not a bare wait: the wireguard-go processes are children too.
	# shellcheck disable=SC2086 # a list of pids
	wait $BOND_BG
	python3 -I "$LAB/bondtab.py" line --result "$res" --ping "$RUN/ping.out" --ping-big "$RUN/pingbig.out" \
		--case "$c" --variant "$v" --dir "$dir" --cc "$cc" --rep "$rep" --secs "$s" \
		--cpu-client $((c1 - c0)) --cpu-server $((s1 - s0)) --hz "$(getconf CLK_TCK)" --netem "$BOND_NETEM" \
		--before "$RUN/snap-before" --after "$RUN/snap-after" ${small[@]+"${small[@]}"} >>"$RUN/bond.jsonl" || rc=1
	bond_snap_roll
	python3 -I "$LAB/bondtab.py" show "$RUN/bond.jsonl"
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
	bond_variant "$v" || return 1
	[ "$BV_SOLO" = 1 ] && NLINKS=1
	CLIENT_EXTRA=$(bond_join "$BV_CLIENT" "$CLIENT_EXTRA")
	SERVER_EXTRA=$(bond_join "$BV_SERVER" "$SERVER_EXTRA")
	setup && start || return 1
	if [ "$BOND_NEED_NETEM" = 1 ] && ! bond_netem_ok; then
		echo "FAIL: no netem (BOND_NEED_NETEM=1)"
		return 1
	fi
	bond_shape "$c" || return 1
	bond_tunnel || return 1
	ip netns exec "$NS_SRV" iperf3 -s -B 10.79.0.1 -D || return 1
	case " $BOND_CC " in *" quic "*) bond_quic_server || return 1 ;; esac
	sleep 1.7 # and the status files hold the handshake: the first before
	bond_snap "$RUN/snap-before"
	for rep in $(seq $((BOND_REP0 + 1)) $((BOND_REP0 + BOND_N))); do
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

# bond_table: the summary tables of the run, in Markdown. The rules (median and
# p25-p75, or min-max, two modes, the engine's counters, the comparison of each
# variant with the references of its family) are bondtab.py's.
bond_table() { python3 -I "$LAB/bondtab.py" table "$RUN/bond.jsonl"; }

# bond_plan: how long the runs would take, from BOND_CASES, BOND_VARIANTS, BOND_N,
# BOND_S, BOND_DIRS and BOND_CC (all three when empty): what bond.yml checks
# before it starts. 1 when it is more than BOND_MAX_MIN (180) minutes.
bond_plan() {
	local c v w dirs=0 ccs=0 dur secs=0 runs=0 pairs=0 min
	bond_settle
	for v in $BOND_VARIANTS; do
		bond_variant "$v" || {
			echo "bond: unknown variant $v"
			return 1
		}
	done
	for w in $BOND_DIRS; do dirs=$((dirs + 1)); done
	for w in ${BOND_CC:-cubic bbr quic}; do ccs=$((ccs + 1)); done
	for c in $BOND_CASES; do
		case $c in
		equal | het | reorder50 | reorder150 | linkdeath | starlink15 | starlink15sw | ltespike | ltespikerand | deepq | bloat | lossy | spikeloss) ;;
		*)
			echo "bond: unknown case $c"
			return 1
			;;
		esac
		dur=$BOND_S
		case $c in starlink15*) [ "$dur" -lt 31 ] && dur=31 ;; esac
		for v in $BOND_VARIANTS; do
			bond_applies "$c" "$v" || continue
			pairs=$((pairs + 1))
			runs=$((runs + dirs * ccs))
			# a run takes its seconds and about 4 more (starting, draining, the
			# wait for the engine's status); a case, 6 to build its lab
			secs=$((secs + dirs * ccs * (dur + 4) + 6))
		done
	done
	min=$(((secs * BOND_N + 59) / 60 + 3))
	echo "bond: $pairs case/variant pairs, $runs runs x $BOND_N repetition(s): about $min minutes, 3 of them installing"
	[ "$min" -le "${BOND_MAX_MIN:-180}" ] || {
		echo "Too long (more than ${BOND_MAX_MIN:-180}): pick fewer cases, variants, repetitions or seconds"
		return 1
	}
}

bond() {
	local c v fail=0 failed="" tool
	for tool in iperf3 wg ping; do
		command -v "$tool" >/dev/null || { echo "bond: needs $tool"; return 1; }
	done
	if [ "$BOND_SMALL" -gt 0 ] && [ ! -x "$BIN/udpgen" ]; then
		echo "bond: needs $BIN/udpgen (bench/lab.sh build)"
		return 1
	fi
	bond_ccs || return 1
	bond_settle
	mkdir -p "$RUN"
	bond_variants_check || return 1
	trap 'bond_untunnel; teardown' EXIT
	trap 'bond_untunnel; teardown; exit 130' INT TERM
	: >"$RUN/bond.jsonl"
	echo "bond: $BOND_CC; $BOND_VARIANTS; $BOND_N repetition(s) of $BOND_S s"
	for c in $BOND_CASES; do
		for v in $BOND_VARIANTS; do
			bond_applies "$c" "$v" || continue
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
