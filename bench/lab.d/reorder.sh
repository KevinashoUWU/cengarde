# shellcheck shell=bash disable=SC2034 # LAB_CI, NLINKS and SIZE are read by lab.sh
# Reordering at the receiver and the split (story 012, Fase 5 step 1):
# "bench/lab.sh reorder" (or reorder_imix, reorder_hol on their own).
#
# Written against the settings and the status of the step 1 plan (split,
# reorder_*, the "reorder" object of the status), so an engine without them
# says "skip: engine without reorder" and passes: it needs the integration.
# Both scenarios run with split = force on both ends and muting off
# (mute_behind_ms = 0), over the shaping of reorder150 or reorder50 (bond.sh).
#
# imix: IMIX packets (udpgen -s imix: 7 x 64, 4 x 576, 1 x 1392 bytes of UDP payload)
# over two links 150 ms apart, up and down, at two rates. The window is 4096
# sequences: 20 kpps (205 ms) fits the spread, 30 kpps (136 ms) does not and
# overflows. Always: every packet arrives once (unique == sent), nothing is
# dropped as too old or at the TUN, the pool never runs dry. Below
# the window nothing is given up for the overflow and the big packets arrive in
# order (IMIX_BELOW_ORD_MAX_PERMILLE, 5 per thousand); above it the overflow
# happens (lost.overflow > 0) and the share of big packets that arrive behind a
# later one is reported. The late copies of forced gaps come out of order, so
# it is not near 0 there; IMIX_ORD_MAX_PCT, if set, bounds it.
#
# hol: head of line. The real thing (iperf3 and QUIC through the TUN, as in bond)
# with a flow of small packets alongside: HOL_PPS (50) of HOL_SIZE (160
# bytes of payload: a 188-byte IPv4 packet in the tunnel, above the default bypass
# and below the split size) through the tunnel while the transfers go, with
# reorder_bypass 0, 128 and 256 (HOL_BYPASS). What the small flow waits for
# behind the held big packets (p50 and p99 of its delay) and what the
# transfers see (retransmissions, spurious losses) come out in the bond table.
# Fails only when something is dropped as too old or at the TUN.
# It runs with BOND_NETEM_INFLIGHT=1: a split has no second copy to cover what
# netem drops when the packets in its delay fill its limit (bond_link).
#
# Not in ci yet (LAB_CI=0). Needs netem for the delays: without it, "skip: no
# netem here", or a failure with REORDER_NEED_NETEM=1.
LAB_CI=0

IMIX_S=${IMIX_S:-8}
IMIX_BELOW_PPS=${IMIX_BELOW_PPS:-20000}
IMIX_OVER_PPS=${IMIX_OVER_PPS:-30000}
IMIX_BELOW_ORD_MAX_PERMILLE=${IMIX_BELOW_ORD_MAX_PERMILLE:-5}
IMIX_ORD_MAX_PCT=${IMIX_ORD_MAX_PCT:-}
HOL_PPS=${HOL_PPS:-50}
HOL_SIZE=${HOL_SIZE:-160}
HOL_BYPASS=${HOL_BYPASS:-0 128 256}
HOL_CASE=${HOL_CASE:-reorder50}
HOL_DIRS=${HOL_DIRS:-up down}
HOL_CC=${HOL_CC:-cubic quic}
HOL_N=${HOL_N:-1}
REORDER_NEED_NETEM=${REORDER_NEED_NETEM:-0}

# reorder_ready: 0 when the engine reports the reorder status (client and, once
# it has a session, server) and netem works; 1 (after saying "skip: ...") when
# not; 2 when the lab itself did not come up.
reorder_ready() {
	local have=0
	[ "${REORDER_READY:-}" = 1 ] && return 0 # reorder asked already
	NLINKS=2
	setup && start || {
		echo "FAIL: the lab did not start"
		teardown
		return 2
	}
	jget "$RUN/client.json" 'd["reorder"]["held_total"]' >/dev/null 2>&1 && have=1
	if [ "$have" = 1 ] && [ "$(jget "$RUN/server.json" 'len(d["sessions"])' 2>/dev/null)" -gt 0 ] 2>/dev/null; then
		jget "$RUN/server.json" 'd["sessions"][0]["reorder"]["held_total"]' >/dev/null 2>&1 || have=0
	fi
	if [ "$have" = 0 ]; then
		echo "reorder: skip: engine without reorder (no \"reorder\" object in the status)"
		teardown
		return 1
	fi
	if ! bond_netem_ok; then
		teardown
		if [ "$REORDER_NEED_NETEM" = 1 ]; then
			echo "FAIL: no netem (REORDER_NEED_NETEM=1)"
			return 2
		fi
		echo "reorder: skip: no netem here"
		return 1
	fi
	teardown
	return 0
}

# reorder_link N DELAY_MS: link N with a one-way delay and a queue that holds
# what a delay of that length holds at tens of kpps (netem counts the packets
# in the delay against its limit).
reorder_link() {
	local ns dev
	for ns in "$NS_CLI" "$NS_SRV"; do
		dev=$([ "$ns" = "$NS_CLI" ] && echo "l$1" || echo "s$1")
		ip netns exec "$ns" tc qdisc replace dev "$dev" root netem delay "${2}ms" limit 60000 || return 1
	done
}

# reorder_get SIDE NAME: a counter of the last delta ($RUN/delta.json).
reorder_get() { jget "$RUN/delta.json" "d['eng']['$1']['$2']"; }

# reorder_imix_run PHASE PPS DIR: one run of IMIX; 0 when it passes.
reorder_imix_run() {
	local phase=$1 pps=$2 dir=$3 side fail=0 sent uniq big bigord ovf late_old held_max pool old_c old_s drops_c drops_s pct
	side=$([ "$dir" = up ] && echo srv || echo cli)
	sleep 1.2 # the engine writes its status every second
	bond_snap "$RUN/snap-before"
	"$dir" "$pps" "$IMIX_S" >"$RUN/traffic.txt"
	sleep 1.2
	bond_snap "$RUN/snap-after"
	python3 -I "$LAB/bondtab.py" delta "$RUN/snap-before" "$RUN/snap-after" >"$RUN/delta.json"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	big=$(sed -n 's/.*big=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	bigord=$(sed -n 's/.*bigord=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	ovf=$(reorder_get "$side" lost_overflow)
	late_old=$(reorder_get "$side" late_old)
	held_max=$(reorder_get "$side" held_max)
	pool=$(reorder_get "$side" pool_full)
	old_c=$(reorder_get cli too_old) old_s=$(reorder_get srv too_old)
	drops_c=$(reorder_get cli tun_drops) drops_s=$(reorder_get srv tun_drops)
	pct=$(awk -v a="${bigord:-0}" -v b="${big:-0}" 'BEGIN { printf "%.1f", (b > 0 ? 100 * a / b : 0) }')
	echo "   $dir $phase $pps pps at the $side: sent=$sent uniq=$uniq big=$big bigord=$bigord ($pct %)" \
		"lost.overflow=$ovf late.old=$late_old held_max=$held_max pool_full=$pool" \
		"too_old=$old_c/$old_s tun_drops=$drops_c/$drops_s"
	[ -n "$sent" ] && [ "$sent" -gt 0 ] && [ "$uniq" = "$sent" ] || { echo "FAIL: $dir $phase: unique ($uniq) is not sent ($sent)"; fail=1; }
	[ "$old_c" = 0 ] && [ "$old_s" = 0 ] || { echo "FAIL: $dir $phase: packets dropped as too old ($old_c/$old_s)"; fail=1; }
	[ "$drops_c" = 0 ] && [ "$drops_s" = 0 ] || { echo "FAIL: $dir $phase: packets dropped at the TUN ($drops_c/$drops_s)"; fail=1; }
	[ "$pool" = 0 ] || { echo "FAIL: $dir $phase: the pool ran dry ($pool)"; fail=1; }
	if [ "$phase" = below ]; then
		[ "$ovf" = 0 ] || { echo "FAIL: $dir $phase: the window overflowed ($ovf) below its rate"; fail=1; }
		[ $((bigord * 1000)) -le $((big * IMIX_BELOW_ORD_MAX_PERMILLE)) ] ||
			{ echo "FAIL: $dir $phase: $bigord of $big big packets out of order (over $IMIX_BELOW_ORD_MAX_PERMILLE per thousand)"; fail=1; }
	else
		{ [ "$ovf" != None ] && [ "$ovf" -gt 0 ]; } 2>/dev/null || { echo "FAIL: $dir $phase: no overflow (lost.overflow $ovf): the scenario did not reach the window"; fail=1; }
		if [ -n "$IMIX_ORD_MAX_PCT" ] && [ $((bigord * 100)) -gt $((big * IMIX_ORD_MAX_PCT)) ]; then
			echo "FAIL: $dir $phase: $bigord of $big big packets out of order (over $IMIX_ORD_MAX_PCT %)"
			fail=1
		fi
	fi
	return "$fail"
}

reorder_imix() {
	local fail=0 ready phase pps dir
	reorder_ready
	ready=$?
	[ "$ready" = 1 ] && return 0
	[ "$ready" = 0 ] || return 1
	NLINKS=2 SIZE=imix
	CLIENT_EXTRA=$(bond_join "split = force;mute_behind_ms = 0;reorder_max_wait_ms = 400" "$CLIENT_EXTRA")
	SERVER_EXTRA=$(bond_join "split = force;mute_behind_ms = 0;reorder_max_wait_ms = 400" "$SERVER_EXTRA")
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup && start || return 1
	reorder_link 1 20 && reorder_link 2 170 || return 1
	for phase in "below $IMIX_BELOW_PPS" "overflow $IMIX_OVER_PPS"; do
		pps=${phase#* }
		phase=${phase% *}
		for dir in up down; do
			reorder_imix_run "$phase" "$pps" "$dir" || fail=1
		done
	done
	stop
	teardown
	[ "$fail" = 0 ] && echo "reorder_imix: ok" || echo "reorder_imix: FAILED"
	return "$fail"
}

reorder_hol() {
	local fail=0 ready v bp
	reorder_ready
	ready=$?
	[ "$ready" = 1 ] && return 0
	[ "$ready" = 0 ] || return 1
	v=
	for bp in $HOL_BYPASS; do v="$v split-bp$bp"; done
	BOND_CASES=$HOL_CASE BOND_VARIANTS=${HOL_VARIANTS:-${v# }} BOND_SMALL=$HOL_PPS BOND_SMALL_SIZE=$HOL_SIZE \
		BOND_DIRS=$HOL_DIRS BOND_CC=$HOL_CC BOND_N=$HOL_N BOND_NEED_NETEM=1 BOND_NETEM_INFLIGHT=1 bond || fail=1
	python3 -I "$LAB/bondtab.py" zero "$RUN/bond.jsonl" too_old tun_drops || {
		echo "FAIL: packets dropped as too old or at the TUN (above)"
		fail=1
	}
	[ "$fail" = 0 ] && echo "reorder_hol: ok" || echo "reorder_hol: FAILED"
	return "$fail"
}

reorder() {
	local fail=0 ready
	reorder_ready
	ready=$?
	[ "$ready" = 1 ] && return 0
	[ "$ready" = 0 ] || return 1
	export REORDER_READY=1 # the two scenarios need not ask again
	(reorder_imix) || fail=1
	(reorder_hol) || fail=1
	[ "$fail" = 0 ] && echo "reorder: ok" || echo "reorder: FAILED"
	return "$fail"
}
