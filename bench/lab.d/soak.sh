# shellcheck shell=bash disable=SC2034 # ENGINE and NLINKS are read by lab.sh
# Soak of the client's link_threads modes: "bench/lab.sh soak" (by hand;
# PR 3b, gate C0 with off, then on; docs/historias/011).
#
# For SOAK_S (3600) seconds per mode of SOAK_MODES ("off on"), traffic both
# ways in segments of SOAK_SEG (60) seconds, each at a rate and size taken
# in turn from 2000, 10000, 20000 and 40000 pps and 80, 400 and 1400 bytes,
# while:
# - every 2 minutes l3 changes its loss and delay (netem when the kernel has
#   it, else a tbf rate);
# - every 5 minutes the client reloads its configuration (ctl reload);
# - every 10 minutes l2 goes down for 10 s and comes back.
# Each segment prints what each direction lost and the drops counted for
# that direction while it ran, which can explain a loss:
# - drops of whole packets: the kernel's receive drops (full queues, or a
#   port with no socket; /proc/net/snmp and /proc/net/udp) at that
#   direction's sockets, what the engines could not hand to WireGuard, and
#   the sending fake WireGuard's send errors;
# - drops of copies on the way out, per link: what the engine could not
#   send, what the veth or l3's qdisc dropped after it. A packet is lost
#   only if its copies on two links or more are, so the copies of every
#   link but the one that dropped most count: l3, shaped on purpose, or l2
#   while it is down, do not explain a loss on their own.
#
# Passes (design D.4: delivered = sent minus counted drops) when no segment
# lost more packets in a direction than the drops counted for it, the
# client never warns of a stalled thread nor of a socket it could not
# poll, every reload answers "ok", and the client's RSS after the first 5
# minutes grows by less than SOAK_RSS_KB (1024) kB. CLIENT_EXTRA and
# SERVER_EXTRA go first, so the soak's own settings win.
SOAK_S=${SOAK_S:-3600}
SOAK_MODES=${SOAK_MODES:-off on}
SOAK_SEG=${SOAK_SEG:-60}
SOAK_RSS_KB=${SOAK_RSS_KB:-1024}

soak_rss() { awk '/^VmRSS:/ { print $2 }' "/proc/$(cat "$RUN/client.pid")/status" 2>/dev/null || echo 0; }

# soak_events SECS: l3's loss and delay, reloads, l2 down and up. What was
# done goes to soak-done.txt, a reload that did not answer "ok" to
# soak-events.txt.
soak_events() {
	local t=0 netem=1 i=0 out
	local -a shapes=("loss 2% delay 20ms" "delay 60ms 10ms" "loss 10%" "delay 5ms")
	local -a rates=(5mbit 20mbit 2mbit 50mbit)
	ip netns exec cli tc qdisc replace dev l3 root netem delay 1ms 2>/dev/null || netem=0
	while [ "$t" -lt "$1" ]; do
		sleep 10
		t=$((t + 10))
		if [ $((t % 120)) = 0 ]; then
			if [ "$netem" = 1 ]; then
				# shellcheck disable=SC2086 # the shape is several words
				ip netns exec cli tc qdisc replace dev l3 root netem ${shapes[i % 4]}
			else
				ip netns exec cli tc qdisc replace dev l3 root tbf rate "${rates[i % 4]}" burst 32kb limit 3mb
			fi
			i=$((i + 1))
			echo "$t shape" >>"$RUN/soak-done.txt"
		fi
		# The reload first: the l2 down below moves t past the multiple of 300.
		if [ $((t % 300)) = 0 ]; then
			out=$("$CENGARDE_BIN" ctl -s "$RUN/client.sock" reload 2>&1)
			[ "$out" = ok ] || echo "reload at $t s said: $out" >>"$RUN/soak-events.txt"
			echo "$t reload" >>"$RUN/soak-done.txt"
		fi
		if [ $((t % 600)) = 0 ]; then
			ip -n cli link set l2 down
			sleep 10
			t=$((t + 10))
			ip -n cli link set l2 up
			echo "$t l2" >>"$RUN/soak-done.txt"
		fi
	done
}

# soak_counts: the drops counted so far, "kup kdown wup wdown up1 up2 up3
# down1 down2 down3": the kernels' receive drops of each direction (in the
# server's namespace all but those of its WireGuard socket are upload's,
# in the client's all but those of its WireGuard socket are download's; a
# port with no socket: upload's in the server's namespace, download's in
# the client's); what the server (upload) and the client (download) could
# not hand to WireGuard; and the copies each link lost on the way out, up
# (the client's sends, l1-l3 and their qdiscs) and down (the server's
# sends, s1-s3).
soak_counts() {
	"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/soak-client.json" 2>/dev/null
	"$CENGARDE_BIN" ctl -s "$RUN/server.sock" status >"$RUN/soak-server.json" 2>/dev/null
	python3 - "$RUN" <<'EOF'
import json, subprocess, sys

def out(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout

def parse(text, empty):
    try:
        return json.loads(text)
    except ValueError:
        return empty

def load(path):
    try:
        with open(path) as f:
            return parse(f.read(), {})
    except OSError:
        return {}

def snmp(ns):
    rows = [l.split()[1:] for l in out('ip', 'netns', 'exec', ns, 'cat', '/proc/net/snmp').splitlines()
            if l.startswith('Udp:')]
    udp = dict(zip(rows[0], map(int, rows[1]))) if len(rows) == 2 else {}
    return udp.get('InErrors', 0), udp.get('NoPorts', 0)

def wg_socket(ns, field, addr):
    # The drops of the UDP socket whose local (field 1) or remote (field 2)
    # address is addr, 127.0.0.1:port as /proc/net/udp writes it.
    want = '0100007F:%04X' % addr
    return sum(int(r.split()[-1]) for r in out('ip', 'netns', 'exec', ns, 'cat', '/proc/net/udp').splitlines()[1:]
               if r.split()[field] == want)

def veth(ns, dev):
    links = parse(out('ip', '-n', ns, '-s', '-j', 'link', 'show', 'dev', dev), [])
    return links[0].get('stats64', {}).get('tx', {}).get('dropped', 0) if links else 0

def qdisc(ns, dev):
    return sum(q.get('drops', 0) for q in parse(out('tc', '-n', ns, '-s', '-j', 'qdisc', 'show', 'dev', dev), []))

run = sys.argv[1]
c, s = load(run + '/soak-client.json'), load(run + '/soak-server.json')
(ce, cn), (se, sn) = snmp('cli'), snmp('srv')
cwg, swg = wg_socket('cli', 1, 59401), wg_socket('srv', 2, 59301)
up, down = [], []
for i in (1, 2, 3):
    up.append(sum(l['tx_drops'] + l['tx_errors'] for l in c.get('links', []) if l.get('name') == 'l%d' % i) +
              veth('cli', 'l%d' % i) + qdisc('cli', 'l%d' % i))
    down.append(sum(l['tx_drops'] for x in s.get('sessions', []) for l in x.get('links', [])
                    if l.get('address', '').startswith('10.0.%d.1:' % i)) + veth('srv', 's%d' % i))
print(se - swg + sn + cwg, ce - cwg + cn + swg, sum(x.get('wireguard_drops', 0) for x in s.get('sessions', [])),
      c.get('download', {}).get('wireguard_drops', 0), *up, *down)
EOF
}

# soak_grew A B: B - A, or 0 when a counter went back (the server forgets a
# path that expired, and its drops with it; tc replaces l3's qdisc).
soak_grew() { echo $(($2 > $1 ? $2 - $1 : 0)); }

# soak_spare A B C: A + B + C without the largest of them.
soak_spare() {
	local m=$1
	[ "$2" -gt "$m" ] && m=$2
	[ "$3" -gt "$m" ] && m=$3
	echo $(($1 + $2 + $3 - m))
}

soak_mode() {
	local mode=$1 fail=0 seg=0 end rate size gs gc ev rss0="" rss d from to sent uniq lost line i
	local -a rates=(2000 10000 20000 40000) sizes=(80 400 1400) a b g
	local su sd budget budget_up budget_down
	CLIENT_EXTRA="${soak_cextra:+$soak_cextra;}link_threads = $mode;control_socket = $RUN/client.sock"
	SERVER_EXTRA="${soak_sextra:+$soak_sextra;}control_socket = $RUN/server.sock"
	setup && start || return 1
	: >"$RUN/soak-events.txt"
	: >"$RUN/soak-done.txt"
	soak_events "$SOAK_S" &
	ev=$!
	end=$((SECONDS + SOAK_S))
	while [ $((SECONDS + SOAK_SEG)) -le "$end" ]; do
		rate=${rates[seg % 4]} size=${sizes[seg / 4 % 3]}
		read -ra a < <(soak_counts)
		ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r "$rate" -s "$size" -d "$SOAK_SEG" -g 2 >"$RUN/srv.out" &
		gs=$!
		sleep 0.3 # as in up(): else the server hands the first packets to an unbound port
		ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r "$rate" -s "$size" -d "$SOAK_SEG" \
			-g 2 >"$RUN/cli.out" &
		gc=$!
		wait "$gs" "$gc"
		read -ra b < <(soak_counts)
		seg=$((seg + 1))
		g=()
		for i in 0 1 2 3 4 5 6 7 8 9; do
			g+=("$(soak_grew "${a[i]:-0}" "${b[i]:-0}")")
		done
		# Each sender's send errors: its direction's.
		su=$(sed -n 's/.*send_err=\([0-9]*\).*/\1/p' "$RUN/cli.out")
		sd=$(sed -n 's/.*send_err=\([0-9]*\).*/\1/p' "$RUN/srv.out")
		budget_up=$((g[0] + g[2] + ${su:-0} + $(soak_spare "${g[4]}" "${g[5]}" "${g[6]}")))
		budget_down=$((g[1] + g[3] + ${sd:-0} + $(soak_spare "${g[7]}" "${g[8]}" "${g[9]}")))
		line="   $mode seg $seg ($rate pps, $size B):"
		# down: sent by srv, received by cli; up: the other way.
		for d in down up; do
			from=$RUN/srv.out to=$RUN/cli.out budget=$budget_down
			[ "$d" = up ] && from=$RUN/cli.out to=$RUN/srv.out budget=$budget_up
			sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$from")
			uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$to")
			if [ -z "$sent" ] || [ -z "$uniq" ]; then
				echo "FAIL: $mode segment $seg ($rate pps, $size B) $d: no result"
				fail=1
				continue
			fi
			lost=$((sent - uniq))
			line="$line $d lost $lost of $sent (drops $budget),"
			if [ "$lost" -gt "$budget" ]; then
				echo "FAIL: $mode segment $seg ($rate pps, $size B) $d: lost $lost, more than the $budget drops counted"
				fail=1
			fi
		done
		echo "$line kernel up ${g[0]} down ${g[1]}, WireGuard up ${g[2]} down ${g[3]}, senders up ${su:-0}" \
			"down ${sd:-0}, copies up ${g[4]}/${g[5]}/${g[6]} down ${g[7]}/${g[8]}/${g[9]}"
		rss=$(soak_rss)
		[ -z "$rss0" ] && [ "$SECONDS" -ge $((end - SOAK_S + 300)) ] && rss0=$rss
	done
	kill "$ev" 2>/dev/null
	wait "$ev" 2>/dev/null
	rss=$(soak_rss)
	echo "   $mode: $seg segments, $(grep -c ' reload$' "$RUN/soak-done.txt") reloads," \
		"$(grep -c ' l2$' "$RUN/soak-done.txt") times l2 down and up, $(grep -c ' shape$' "$RUN/soak-done.txt")" \
		"changes of l3, RSS ${rss0:-?} kB after 5 min, $rss kB at the end"
	if [ -n "$rss0" ] && [ $((rss - rss0)) -gt "$SOAK_RSS_KB" ]; then
		echo "FAIL: $mode: RSS grew by $((rss - rss0)) kB"
		fail=1
	fi
	if grep -E "has not run for|could not be polled" "$RUN/client.log"; then
		echo "FAIL: $mode: a stalled thread or a socket it could not poll"
		fail=1
	fi
	if [ -s "$RUN/soak-events.txt" ]; then
		cat "$RUN/soak-events.txt"
		fail=1
	fi
	stop
	return "$fail"
}

soak() {
	local fail=0 mode soak_cextra=${CLIENT_EXTRA:-} soak_sextra=${SERVER_EXTRA:-}
	ENGINE=c NLINKS=3
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	for mode in $SOAK_MODES; do
		soak_mode "$mode" || fail=1
	done
	teardown
	[ "$fail" = 0 ] && echo "soak: ok" || echo "soak: FAILED"
	return "$fail"
}
