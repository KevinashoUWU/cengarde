# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# Server address lists and failover: "bench/lab.sh fallback".
#
# Each link sends to one address of its server list and moves to the next
# one after server_failover_ms without a verified reply, sticking to the one
# that answers (engine/src/srvpick.h). Here with 4 s of failover (10 s by
# default):
#
# - l1, "server = 10.0.1.99:59402 10.0.1.2:59402": nothing answers at the
#   first address, so l1 goes live on the second within the failover time,
#   and stays there. Then two reloads that leave it where it is: one adds
#   an IPv6 entry to its list, which l1 has no IPv6 for, and one swaps its
#   two addresses, so the new first is where l1 already is. Neither closes
#   its socket.
# - l2, "server = [2001:db8::2]:59402 10.0.2.2:59402": the link has no IPv6
#   address (nor does the lab), so the IPv6 entry is no candidate for it and
#   l2 goes live on 10.0.2.2 at once.
# - l3, "server = 10.0.3.2:59402 10.0.3.20:59402", both the server:
#   10.0.3.20 reaches it through a DNAT in the srv netns, so the scenario
#   needs a single server address. With 10.0.3.2 blocked, l3 moves to
#   10.0.3.20, and stays there once 10.0.3.2 works again. Then an outage:
#   the server side of l3 loses its addresses for 25 s, while l3 keeps its
#   own (a modem that keeps its lease). l3 goes through both addresses
#   without a reply, a dead round, and the first reply after the outage
#   sends it back to 10.0.3.2, the first.
# - Path MTU: l2's MTU drops to 1400 on the client side while 1400-byte
#   datagrams go up. The status shows path_mtu 1400 on l2, and the log asks
#   for a WireGuard MTU of 1316 on l2 only.
#
# Needs iptables (the DNAT and the block), besides what lab.sh needs.
LAB_CI=1

FAILOVER_MS=${FAILOVER_MS:-4000}
OUTAGE_S=${OUTAGE_S:-25}

# fb_wait LINK EXPR SECS: waits until EXPR (Python, on l, the status of LINK
# from the control socket) holds. Prints how long it took, or the link as it
# was when SECS ran out (exit 1).
fb_wait() {
	python3 - "$RUN/client.sock" "$1" "$2" "$3" <<'EOF'
import json, socket, sys, time

path, name, expr, limit = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])


def status():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1)
    try:
        s.connect(path)
        s.sendall(b"status\n")
        buf = b""
        while chunk := s.recv(65536):
            buf += chunk
        return json.loads(buf)
    except (OSError, ValueError):
        return None
    finally:
        s.close()


t0, l = time.time(), None
while time.time() - t0 < limit:
    d = status()
    l = next((x for x in d["links"] if x["name"] == name), None) if d else None
    if l is not None and eval("(" + expr + ")"):
        print(f"{time.time() - t0:.1f} s")
        sys.exit(0)
    time.sleep(0.1)
keys = ("state", "reason", "remote", "family", "candidate", "candidates", "failovers", "path_mtu")
print("not within %.0f s: %s" % (limit, {k: l.get(k) for k in keys} if l else "no such link"))
sys.exit(1)
EOF
}

# fb_link LINK KEY: one value from the client's status, now.
fb_link() {
	"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/client-now.json" 2>/dev/null &&
		jget "$RUN/client-now.json" "[l[\"$2\"] for l in d[\"links\"] if l[\"name\"] == \"$1\"][0]" 2>/dev/null ||
		echo "?"
}

# fb_list LINK LIST: the server list of [link LINK] in the client's config.
fb_list() { sed -i "/^\[link $1\]\$/{n;s/^server = .*/server = $2/}" "$RUN/client.conf"; }

# fb_reload LINK LIST: fb_list, then a reload of the client.
fb_reload() {
	local out
	fb_list "$1" "$2"
	out=$("$CENGARDE_BIN" ctl -s "$RUN/client.sock" reload 2>&1)
	[ "$out" = ok ] || { echo "FAIL: ctl reload said: $out"; return 1; }
}

fallback() {
	local fail=0 out moves1 moves3 downs sent uniq secs=$(((FAILOVER_MS + 1999) / 1000 + 1))
	local on1='l["state"] == "live" and l["remote"] == "10.0.1.2:59402" and l["candidate"] == 1'
	local on3a='l["state"] == "live" and l["remote"] == "10.0.3.2:59402" and l["candidate"] == 0'
	local on3b='l["state"] == "live" and l["remote"] == "10.0.3.20:59402" and l["candidate"] == 1'
	if ! command -v iptables >/dev/null; then
		echo "FAIL: fallback needs iptables"
		echo "fallback: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $RUN/client.sock;server_failover_ms = $FAILOVER_MS${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup || { echo "fallback: FAILED"; return 1; }
	fb_list l1 "10.0.1.99:59402 10.0.1.2:59402"
	fb_list l2 "[2001:db8::2]:59402 10.0.2.2:59402"
	fb_list l3 "10.0.3.2:59402 10.0.3.20:59402"
	ip -n srv addr add 10.0.3.20/32 dev s3
	ip netns exec srv iptables -t nat -A PREROUTING -i s3 -d 10.0.3.20 -p udp --dport 59402 \
		-j DNAT --to-destination 10.0.3.2 || { echo "FAIL: iptables DNAT in the srv netns"; fail=1; }
	start

	# l2: the IPv6 entry is skipped, so no failover is needed.
	out=$(fb_wait l2 'l["state"] == "live" and l["remote"] == "10.0.2.2:59402" and l["candidate"] == 0 and
		l["candidates"] == 1 and l["family"] == "ipv4" and l["failovers"] == 0' 3) ||
		{ echo "FAIL: l2 did not skip the IPv6 entry: $out"; fail=1; }
	echo "   l2, IPv6 entry first on an IPv4-only link: live on 10.0.2.2 after $out"
	# l1: the first address leads nowhere.
	out=$(fb_wait l1 "$on1" "$secs") || { echo "FAIL: l1 did not move to its second address: $out"; fail=1; }
	echo "   l1, first address unreachable: live on the second after $out (failover $FAILOVER_MS ms)"
	# l3: its first address blocked, then working again.
	out=$(fb_wait l3 "$on3a" 3) || { echo "FAIL: l3 not live on its first address: $out"; fail=1; }
	ip netns exec srv iptables -t raw -I PREROUTING -i s3 -d 10.0.3.2 -j DROP
	out=$(fb_wait l3 "$on3b" "$((secs + 1))") || { echo "FAIL: l3 did not move to 10.0.3.20: $out"; fail=1; }
	echo "   l3, first address blocked: live on the second after $out"
	ip netns exec srv iptables -t raw -D PREROUTING -i s3 -d 10.0.3.2 -j DROP
	moves1=$(fb_link l1 failovers) moves3=$(fb_link l3 failovers)
	sleep $((2 * FAILOVER_MS / 1000 + 1))
	# Sticky: both stay where replies come, the first address working or not.
	out=$(fb_wait l1 "$on1 and l[\"failovers\"] == $moves1" 1) || { echo "FAIL: l1 moved again: $out"; fail=1; }
	out=$(fb_wait l3 "$on3b and l[\"failovers\"] == $moves3" 1) ||
		{ echo "FAIL: l3 left 10.0.3.20 although it answers: $out"; fail=1; }
	echo "   l1 and l3 stay on their second address (failovers: l1 $moves1, l3 $moves3)"
	# Reloads: an IPv6 entry added to l1's list is none of l1's (no IPv6
	# on it), so l1 stays on its second address; its two addresses swapped
	# are the operator's new order, whose first is where l1 already is.
	downs=$(grep -c "link l1 down" "$RUN/client.log")
	fb_reload l1 "10.0.1.99:59402 10.0.1.2:59402 [2001:db8::9]:59402" || fail=1
	sleep "$secs"
	out=$(fb_wait l1 "$on1 and l[\"failovers\"] == $moves1" 1) ||
		{ echo "FAIL: l1 moved on a reload that added an IPv6 entry: $out"; fail=1; }
	fb_reload l1 "10.0.1.2:59402 10.0.1.99:59402" || fail=1
	out=$(fb_wait l1 "l[\"state\"] == \"live\" and l[\"remote\"] == \"10.0.1.2:59402\" and l[\"candidate\"] == 0" 2) ||
		{ echo "FAIL: l1 not on the new first address after a reload that swapped its list: $out"; fail=1; }
	[ "$(grep -c "link l1 down" "$RUN/client.log")" = "$downs" ] && [ "$(fb_link l1 failovers)" = "$moves1" ] ||
		{ echo "FAIL: a reload that kept l1's addresses closed its socket"; fail=1; }
	echo "   l1, reloads with an IPv6 entry added and its two addresses swapped: stays on 10.0.1.2 without reopening"

	# An outage of l3: every address of its server side gone for a while.
	ip -n srv addr flush dev s3
	sleep "$OUTAGE_S"
	ip -n srv addr add 10.0.3.2/24 dev s3
	ip -n srv addr add 10.0.3.20/32 dev s3
	out=$(fb_wait l3 "$on3a" "$((secs + 3))") ||
		{ echo "FAIL: l3 not back on its first address after the outage: $out"; fail=1; }
	echo "   l3, after $OUTAGE_S s with no server address: back on the first after $out," \
		"failovers $(fb_link l3 failovers)"
	grep -q "link l3: the server answers again after a round of its addresses without replies, back to 10.0.3.2:59402" \
		"$RUN/client.log" || { echo "FAIL: l3 did not come back through a dead round"; fail=1; }

	# Path MTU: l2 at 1400 bytes while 1400-byte datagrams go up.
	ip -n cli link set l2 mtu 1400
	up 2000 7 >"$RUN/traffic.txt"
	echo "   tunnel: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	[ -n "$sent" ] && [ $((uniq * 1000)) -ge $((sent * 995)) ] || { echo "FAIL: packets lost"; fail=1; }
	[ "$(fb_link l2 path_mtu)" = 1400 ] || { echo "FAIL: l2 path_mtu is $(fb_link l2 path_mtu), 1400 wanted"; fail=1; }
	[ "$(fb_link l1 path_mtu)" = 1500 ] || { echo "FAIL: l1 path_mtu is $(fb_link l1 path_mtu), 1500 wanted"; fail=1; }
	grep -q "link l2: WireGuard datagrams of 1400 bytes .* lower the WireGuard MTU to 1316" "$RUN/client.log" ||
		{ echo "FAIL: no path MTU warning for l2"; fail=1; }
	grep -q "link l[13]: WireGuard datagrams" "$RUN/client.log" && { echo "FAIL: a path MTU warning for l1 or l3"; fail=1; }

	grep -hE "no reply from|cannot use|back to|path MTU" "$RUN/client.log" | sed 's/^/   /'
	stop
	teardown
	[ "$fail" = 0 ] && echo "fallback: ok" || echo "fallback: FAILED"
	return "$fail"
}
