#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# cengarde-nat with real traffic, in network namespaces, the real iptables
# and conntrack; veth pairs stand in for the WireGuard interfaces:
#
#   inet [ext 10.1.0.2 (.3), ext2 10.2.0.2, 198.51.100.1 on lo]
#     -- [pub 10.1.0.1, pub2 10.2.0.1] vps [cg-alpha, cg-bravo: 10.79.0.1]
#     -- alpha [10.79.0.2] and bravo [10.79.12.34]
#
# The routers and the server answer every TCP connection and UDP datagram
# with who they are, the port and the source they saw. The forwarding comes
# from a forward table of protocol 4 (FORWARD_FILE): alpha has tcp and udp
# 9000 (to its port 22) and tcp 7000-7010; bravo udp 5000-5010 (to
# 15000-15010) and the whole range; at the end, from protocol 3's IP pass
# file, the whole range for alpha, the router of the first slot. Checks:
# - each rule reaches the right router and port, the shifted range too,
#   with the source intact; the whole range reaches its holder, but not
#   the ports carved out of it; reserved ports (SSH, cengarde, a service of
#   the server itself on 8123) stay on the server; a skipped source and a
#   broadcast are not forwarded;
# - the routers do not reach each other's tunnel addresses; hairpin: bravo
#   reaches alpha's published port at the server's public address, as does
#   alpha itself (from 10.79.0.1), while 10.79.0.1 and port 22 stay local;
# - interface independence: rules applied before there is a default route,
#   the route then added and moved to another device: the routers' Internet
#   (MASQUERADE) and the forwarding keep working with no new apply;
# - live flows: a UDP stream of 10 datagrams a second, from a fixed port,
#   keeps reaching the old router after a port changes hands when conntrack
#   is missing (the problem), and lands on the new one within 2 s of the
#   sync with it: the whole range moving, an explicit rule moving, a router
#   removed, a port the server answered itself given to a router (while the
#   server's own flow from a port of that range to a peer stays its own),
#   and a restart while another rule keeps conntrack on: after down nothing
#   reaches the router, and after apply the stream does again;
# - protocol 3: IP pass on reaches alpha; off, the stream leaves it within
#   0.5 s; on again, with the stream still running, it reaches alpha again
#   within 2 s;
# - apply twice gives the same rules, two applies at once one jump each;
#   check puts back a flushed chain and a deleted jump, warns about an
#   nftables chain that drops forwarded traffic, and does nothing while
#   down; sync does nothing while down; starting from cengarde 0.4's rules,
#   apply removes them; down leaves the firewall as it was.
#
#   sudo sh contrib/vps/test/netns.sh
#
# Needs root, iproute2, iptables, python3 and conntrack (conntrack-tools);
# nft for the nftables warning. Run it under the lab's lock (flock
# /tmp/cengarde-netns.lock) next to other network namespace tests.
# CENGARDE_NAT names another cengarde-nat.
#
# SPDX-License-Identifier: GPL-2.0-only
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
NAT=${CENGARDE_NAT:-$HERE/../cengarde-nat}
INET=cgnn-inet VPS=cgnn-vps A=cgnn-alpha B=cgnn-bravo
TMP=
fails=0

say() { echo "netns: $*"; }
ok() { echo "netns: ok: $*"; }
bad() {
	echo "netns: FAIL: $*"
	fails=$((fails + 1))
}
check() {
	what=$1
	shift
	if "$@"; then ok "$what"; else bad "$what"; fi
}
not() { ! "$@"; }
nsexec() { ip netns exec "$@"; }

cleanup() {
	for ns in "$INET" "$VPS" "$A" "$B"; do
		pids=$(ip netns pids "$ns" 2>/dev/null)
		# shellcheck disable=SC2086 # one PID per word
		[ -z "$pids" ] || kill $pids 2>/dev/null
		ip netns del "$ns" 2>/dev/null
	done
	[ -z "$TMP" ] || rm -rf "$TMP"
}

[ "$(id -u)" -eq 0 ] || {
	echo "netns.sh: run it as root" >&2
	exit 2
}
for cmd in ip iptables iptables-save python3 conntrack; do
	command -v "$cmd" >/dev/null || {
		echo "netns.sh: $cmd is missing" >&2
		exit 2
	}
done
cleanup
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
[ -z "${RUN:-}" ] || mkdir -p "$RUN"
TMP=$(mktemp -d "${RUN:-/tmp}/netns.XXXXXX")
R=$TMP/root
mkdir -p "$R/etc/cengarde/clients" "$R/var/lib/cengarde"
chmod 0700 "$R/etc/cengarde/clients"
printf 'ENGINE_USER=nobody\nFORWARD_FILE=%s\nFORWARD_SKIP_SRC="10.1.0.3/32"\n' "$R/var/lib/cengarde/forward" >"$R/etc/cengarde/nat.conf"
router() { # NAME ADDR SLOT
	printf 'SECRET=x\nSLOT=%s\nTUNNEL_ADDR=%s\nFORWARD=yes\n' "$3" "$2" >"$R/etc/cengarde/clients/$1"
	chmod 0600 "$R/etc/cengarde/clients/$1"
}
router alpha 10.79.0.2 0
router bravo 10.79.12.34 1
# table LINES: the engine's forward table.
table() {
	printf '# cengarde forward table 1, written by the engine: data only\n%s\n' "$1" >"$R/var/lib/cengarde/forward.tmp"
	chown nobody "$R/var/lib/cengarde/forward.tmp"
	mv "$R/var/lib/cengarde/forward.tmp" "$R/var/lib/cengarde/forward"
}
T1='rule alpha tcp 9000 9000 22
rule alpha udp 9000 9000 22
rule alpha tcp 7000 7010 7000
rule bravo udp 5000 5010 15000
pass bravo'
table "$T1"
# nat ARGS...: cengarde-nat in the server's namespace; its output in OUT.
nat() {
	OUT=$(nsexec "$VPS" env CENGARDE_ROOT="$R" sh "$NAT" "$@" 2>&1)
	rc=$?
	printf '%s\n' "$OUT" | sed 's/^/    /'
	return "$rc"
}
# nat_noct ARGS...: as nat, as if conntrack were not installed.
nat_noct() {
	OUT=$(nsexec "$VPS" env CENGARDE_ROOT="$R" CENGARDE_CONNTRACK=/nonexistent/conntrack sh "$NAT" "$@" 2>&1)
	rc=$?
	printf '%s\n' "$OUT" | sed 's/^/    /'
	return "$rc"
}

cat >"$TMP/t.py" <<'EOF'
import select, socket, sys, time
A = socket.AF_INET
def serve(name, log, tcp, udp):
    socks = {}
    for p in [int(x) for x in tcp.split(",") if x]:
        s = socket.socket(A, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", p)); s.listen(64); socks[s] = ("tcp", p)
    for p in [int(x) for x in udp.split(",") if x]:
        s = socket.socket(A, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", p)); socks[s] = ("udp", p)
    open(log + ".ready", "w").close()
    while True:
        for s in select.select(list(socks), [], [])[0]:
            kind, p = socks[s]
            if kind == "tcp":
                c, a = s.accept()
                c.sendall(("%s:%d:%s\n" % (name, p, a[0])).encode()); c.close()
            else:
                d, a = s.recvfrom(2048)
                with open(log, "a") as f:
                    f.write("%.3f %d %s %s\n" % (time.time(), p, a[0], d.decode(errors="replace")))
                s.sendto(("%s:%d:%s\n" % (name, p, a[0])).encode(), a)
def tcp(addr, port, src=""):
    s = socket.socket(A, socket.SOCK_STREAM); s.settimeout(1.5)
    if src: s.bind((src, 0))
    try:
        s.connect((addr, int(port))); print(s.recv(256).decode().strip())
    except Exception:
        print("none")
def udp(addr, port, payload, src="", bcast=""):
    s = socket.socket(A, socket.SOCK_DGRAM); s.settimeout(1.5)
    if src: s.bind((src, 0))
    if bcast: s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.sendto(payload.encode(), (addr, int(port)))
    try:
        print(s.recv(256).decode().strip())
    except Exception:
        print("none")
def stream(addr, port, sport, seconds):
    s = socket.socket(A, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", int(sport)))
    end, n = time.time() + float(seconds), 0
    while time.time() < end:
        s.sendto(("stream%s %d" % (sport, n)).encode(), (addr, int(port))); n += 1
        time.sleep(0.1)
def peer(bind, port, addr, dport, every, seconds, log):
    s = socket.socket(A, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, int(port)))
    end, nxt, n = time.time() + float(seconds), 0, 0
    while time.time() < end:
        if time.time() >= nxt:
            try:
                s.sendto(("peer%s %d" % (port, n)).encode(), (addr, int(dport)))
            except OSError:
                pass
            n += 1; nxt = time.time() + float(every)
        if select.select([s], [], [], max(0, min(nxt, end) - time.time()))[0]:
            try:
                d, a = s.recvfrom(2048)
            except OSError:
                continue
            with open(log, "a") as f:
                f.write("%.3f %s %s %s\n" % (time.time(), port, a[0], d.decode(errors="replace")))
{"serve": serve, "tcp": tcp, "udp": udp, "stream": stream, "peer": peer}[sys.argv[1]](*sys.argv[2:])
EOF
py() {
	ns=$1
	shift
	nsexec "$ns" python3 "$TMP/t.py" "$@"
}
# is WANT COMMAND...: the command prints WANT.
is() {
	want=$1
	shift
	got=$("$@")
	[ "$got" = "$want" ] || {
		say "  got: $got"
		return 1
	}
}
# saved: the server's iptables, one rule per line. iptables -S rather than
# iptables-save, which leaves out the tables nobody has used yet.
saved() {
	for t in filter nat; do
		nsexec "$VPS" iptables -t "$t" -S | sed "s/^/$t /"
	done
}
jumps() { nsexec "$VPS" iptables-save | grep -c -- "-j $1\$"; }

say "namespaces: $INET -- $VPS -- $A, $B"
set -e
for ns in "$INET" "$VPS" "$A" "$B"; do
	ip netns add "$ns"
	ip -n "$ns" link set lo up
done
ip link add pub netns "$VPS" type veth peer name ext netns "$INET"
ip link add pub2 netns "$VPS" type veth peer name ext2 netns "$INET"
ip -n "$VPS" addr add 10.1.0.1/24 dev pub
ip -n "$VPS" addr add 10.2.0.1/24 dev pub2
ip -n "$INET" addr add 10.1.0.2/24 dev ext
ip -n "$INET" addr add 10.1.0.3/24 dev ext
ip -n "$INET" addr add 10.2.0.2/24 dev ext2
ip -n "$INET" addr add 198.51.100.1/32 dev lo
for l in pub pub2; do ip -n "$VPS" link set "$l" up; done
for l in ext ext2; do ip -n "$INET" link set "$l" up; done
tunnel() { # NS NAME ADDR: a router's tunnel, as wg-quick with Table=auto sets it up
	ip link add "cg-$2" netns "$VPS" type veth peer name wg netns "$1"
	ip -n "$VPS" addr add 10.79.0.1/32 dev "cg-$2"
	ip -n "$VPS" link set "cg-$2" up
	ip -n "$VPS" route add "$3/32" dev "cg-$2"
	ip -n "$1" addr add "$3/32" dev wg
	ip -n "$1" link set wg up
	ip -n "$1" route add 10.79.0.1/32 dev wg
	ip -n "$1" route add default via 10.79.0.1 dev wg onlink
}
tunnel "$A" alpha 10.79.0.2
tunnel "$B" bravo 10.79.12.34
nsexec "$VPS" sysctl -qw net.ipv4.ip_forward=1
set +e

py "$INET" serve inet "$TMP/inet.log" 80 "" &
py "$VPS" serve vps "$TMP/vps.log" 22,8123 65500 &
py "$A" serve alpha "$TMP/alpha.log" 22,7005,20000 22,20000,30000 &
py "$B" serve bravo "$TMP/bravo.log" 22,7005,9000,20000 15005,20000,9000,30000 &
n=0
while [ "$(find "$TMP" -maxdepth 1 -name '*.ready' | wc -l)" -lt 4 ] && [ "$n" -lt 50 ]; do
	sleep 0.1
	n=$((n + 1))
done
before=$(saved)

say "from cengarde 0.4's rules, before there is a default route"
old04() {
	x() { nsexec "$VPS" iptables "$@"; }
	x -I INPUT -i pub -p udp --dport 65500 -j ACCEPT
	x -I INPUT ! -i lo -p udp --dport 65501 -j DROP
	x -I FORWARD -i wg0 -j ACCEPT
	x -I FORWARD -i wg0 -d 169.254.0.0/16 -j REJECT --reject-with icmp-net-prohibited
	x -I FORWARD -o wg0 -j ACCEPT
	x -t nat -A POSTROUTING -s 10.79.0.0/30 -o pub -j MASQUERADE
	for r in "udp 65500" "udp 65501" "tcp 22"; do
		# shellcheck disable=SC2086 # protocol and port
		set -- $r
		x -t nat -A PREROUTING -i pub -p "$1" --dport "$2" -j RETURN
	done
	for p in tcp udp; do
		x -t nat -A PREROUTING -i pub -p "$p" --dport 1024:65000 -j DNAT --to-destination 10.79.0.2
	done
}
old04
check "sync while down does nothing" sh -c 'nsexec() { ip netns exec "$@"; }; [ "$(ip netns exec "$1" iptables-save -t nat | grep -c CG_)" -eq 0 ] &&
	ip netns exec "$1" env CENGARDE_ROOT="$2" sh "$3" sync | grep -q "down: nothing to sync" &&
	[ "$(ip netns exec "$1" iptables-save -t nat | grep -c CG_)" -eq 0 ]' sh "$VPS" "$R" "$NAT"
check "cengarde-nat apply" nat apply
check "cengarde 0.4's rules are gone" not sh -c 'ip netns exec "$1" iptables-save | grep -Eq "wg0|10\.79\.0\.0/30|-i pub "' sh "$VPS"
check "one jump into each chain" [ "$(jumps CG_IN)$(jumps CG_FWD)$(jumps CG_PRE)$(jumps CG_POST)" = 1111 ]
once=$(saved)
check "apply again" nat apply
check "the same rules" [ "$(saved)" = "$once" ]
nsexec "$VPS" env CENGARDE_ROOT="$R" sh "$NAT" apply >/dev/null 2>&1 &
p1=$!
nsexec "$VPS" env CENGARDE_ROOT="$R" sh "$NAT" apply >/dev/null 2>&1 &
p2=$!
wait "$p1" "$p2"
check "two applies at once: still the same rules, one jump each" [ "$(saved)" = "$once" ]

say "forwarding from the Internet"
check "tcp 9000 reaches alpha's port 22, source intact" is "alpha:22:10.1.0.2" py "$INET" tcp 10.1.0.1 9000
check "udp 9000 reaches alpha's port 22" is "alpha:22:10.1.0.2" py "$INET" udp 10.1.0.1 9000 probe
check "udp 5005 reaches bravo's 15005 (a shifted range)" is "bravo:15005:10.1.0.2" py "$INET" udp 10.1.0.1 5005 probe
check "tcp 7005 reaches alpha (carved out of bravo's whole range)" is "alpha:7005:10.1.0.2" py "$INET" tcp 10.1.0.1 7005
check "tcp 20000 reaches bravo, the whole range's holder" is "bravo:20000:10.1.0.2" py "$INET" tcp 10.1.0.1 20000
check "udp 20000 too" is "bravo:20000:10.1.0.2" py "$INET" udp 10.1.0.1 20000 probe
check "tcp 22 stays on the server (reserved)" is "vps:22:10.1.0.2" py "$INET" tcp 10.1.0.1 22
check "udp 65500 stays on the server (cengarde)" is "vps:65500:10.1.0.2" py "$INET" udp 10.1.0.1 65500 probe
check "tcp 8123, a service of the server inside the whole range, stays on it" is "vps:8123:10.1.0.2" py "$INET" tcp 10.1.0.1 8123
check "a skipped source (FORWARD_SKIP_SRC) is not forwarded" is none py "$INET" tcp 10.1.0.1 20000 10.1.0.3
py "$INET" udp 10.1.0.255 20000 bcast 10.1.0.2 yes >/dev/null
check "a broadcast to the server is not forwarded" not grep -q " 20000 .* bcast" "$TMP/bravo.log"

say "between the routers"
check "alpha does not reach bravo's tunnel address" is none py "$A" tcp 10.79.12.34 20000
check "bravo does not reach alpha's" is none py "$B" tcp 10.79.0.2 22
check "hairpin: bravo reaches alpha's published port at the public address, from 10.79.0.1" \
	is "alpha:22:10.79.0.1" py "$B" tcp 10.1.0.1 9000
check "hairpin over UDP too" is "alpha:22:10.79.0.1" py "$B" udp 10.1.0.1 9000 probe
check "alpha reaches its own published port" is "alpha:22:10.79.0.1" py "$A" tcp 10.1.0.1 9000
check "alpha reaches bravo's whole range at the public address" is "bravo:20000:10.79.0.1" py "$A" tcp 10.1.0.1 20000
check "10.79.0.1 stays local (port 22)" is "vps:22:10.79.0.2" py "$A" tcp 10.79.0.1 22
check "port 22 of the public address stays local, for the routers too" is "vps:22:10.79.0.2" py "$A" tcp 10.1.0.1 22

say "interface independence: a default route after apply, then moved"
check "no default route yet: alpha cannot reach the Internet" is none py "$A" tcp 198.51.100.1 80
nsexec "$VPS" ip route add default via 10.1.0.2
check "default route via pub: alpha reaches the Internet, masqueraded" is "inet:80:10.1.0.1" py "$A" tcp 198.51.100.1 80
nsexec "$VPS" ip route replace default via 10.2.0.2
check "moved to pub2: masqueraded there, with no new apply" is "inet:80:10.2.0.1" py "$A" tcp 198.51.100.1 80
check "and the forwarding works on pub2's address" is "bravo:20000:10.2.0.2" py "$INET" tcp 10.2.0.1 20000 10.2.0.2
check "the rules did not change" [ "$(saved)" = "$once" ]

say "check"
check "check with nothing to do" nat check
check "it does not apply again" not sh -c 'printf "%s\n" "$1" | grep -q "applied again"' sh "$OUT"
nsexec "$VPS" iptables -t nat -F CG_PRE
check "CG_PRE flushed by someone: check" nat check
check "it says so and applies again" sh -c 'printf "%s\n" "$1" | grep -q "rules were missing or changed"' sh "$OUT"
check "the same rules again" [ "$(saved)" = "$once" ]
nsexec "$VPS" iptables -t filter -D FORWARD -j CG_FWD
check "a jump deleted: check" nat check
check "the same rules again" [ "$(saved)" = "$once" ]
if command -v nft >/dev/null; then
	nsexec "$VPS" nft -f - <<-'EOF'
		table inet strict {
			chain forward { type filter hook forward priority 0; policy drop; }
		}
	EOF
	check "an nftables chain dropping forwarded traffic: check" nat check
	check "it says which lines to add" sh -c 'printf "%s\n" "$1" | grep -q "chain inet strict forward drops" &&
		printf "%s\n" "$1" | grep -qF "oifname \"cg-*\" ct status dnat accept"' sh "$OUT"
	nsexec "$VPS" nft add rule inet strict forward iifname '"cg-*"' accept
	nsexec "$VPS" nft add rule inet strict forward oifname '"cg-*"' ct state established,related accept
	nsexec "$VPS" nft add rule inet strict forward oifname '"cg-*"' ct status dnat accept
	check "with them added, check is quiet about it" nat check
	check "no warning" not sh -c 'printf "%s\n" "$1" | grep -q "strict"' sh "$OUT"
	check "and the forwarding works through it" is "bravo:20000:10.1.0.2" py "$INET" tcp 10.1.0.1 20000
	nsexec "$VPS" nft delete table inet strict
else
	say "note: no nft here; the nftables warning is not checked"
fi
check "status" nat status
check "status says what is forwarded" sh -c 'printf "%s\n" "$1" | grep -q "tcp 9000 alpha -> 10.79.0.2 (22)"' sh "$OUT"

# stream PORT SPORT: a UDP stream from the Internet to the server's port
# PORT, 10 datagrams a second for 12 s, from the fixed port SPORT.
stream() {
	py "$INET" stream 10.1.0.1 "$1" "$2" 12 &
	STREAM=$!
}
# heard LOG WORD [SINCE [UNTIL]]: datagrams in LOG whose payload starts with
# WORD, between two times.
heard() {
	awk -v s="$2" -v a="${3:-0}" -v b="${4:-9999999999}" '$4 == s && $1 >= a && $1 <= b { n++ } END { exit !(n > 0) }' "$1"
}
# got LOG SPORT [SINCE [UNTIL]]: datagrams of the stream from SPORT in LOG.
got() { heard "$1" "stream$2" "${3:-0}" "${4:-9999999999}"; }
# later T SECONDS: T plus SECONDS, to the millisecond (print would round
# the time to six digits).
later() { awk -v t="$1" -v s="$2" 'BEGIN { printf "%.3f", t + s }'; }
# moved FROM TO SPORT T: the stream left FROM by T + 0.5 s and reached TO
# by T + 2 s.
moved() {
	got "$TMP/$2.log" "$3" "$4" "$(later "$4" 2)" &&
		not got "$TMP/$1.log" "$3" "$(later "$4" 0.5)"
}
now() { date +%s.%N | cut -c1-14; }

say "live flows: the whole range changes hands"
table 'pass alpha'
check "the whole range to alpha" nat sync
stream 30000 41000
sleep 2
check "the stream reaches alpha" got "$TMP/alpha.log" 41000
table 'pass bravo'
t=$(now)
check "the whole range to bravo, with no conntrack" nat_noct sync
check "it warns that live flows stay" sh -c 'printf "%s\n" "$1" | grep -q "conntrack is not installed"' sh "$OUT"
sleep 2
check "the stream still reaches alpha (the problem conntrack solves)" got "$TMP/alpha.log" 41000 "$(later "$t" 1.5)"
table 'pass alpha'
check "back to alpha, with no conntrack" nat_noct sync
sleep 1
table 'pass bravo'
t=$(now)
check "the whole range to bravo, with conntrack" nat sync
check "it moves the flow" sh -c 'printf "%s\n" "$1" | grep -q "moved [1-9][0-9]* live flows"' sh "$OUT"
sleep 2.5
check "the stream left alpha and reached bravo within 2 s" moved alpha bravo 41000 "$t"
wait "$STREAM"

say "live flows: a port changes hands"
table "$T1"
check "udp 9000 to alpha" nat sync
stream 9000 41001
sleep 2
check "the stream reaches alpha" got "$TMP/alpha.log" 41001
table "$(printf '%s\n' "$T1" | sed 's/^rule alpha udp 9000 9000 22$/rule bravo udp 9000 9000 9000/')"
t=$(now)
check "udp 9000 to bravo" nat sync
sleep 2.5
check "the stream left alpha and reached bravo within 2 s" moved alpha bravo 41001 "$t"
wait "$STREAM"

say "live flows: a router removed"
table "$T1"
check "udp 9000 to alpha" nat sync
stream 9000 41002
sleep 2
check "the stream reaches alpha" got "$TMP/alpha.log" 41002
rm -f "$R/etc/cengarde/clients/alpha"
t=$(now)
check "alpha removed" nat sync
sleep 2.5
check "the stream left alpha and reached bravo, the whole range's holder, within 2 s" moved alpha bravo 41002 "$t"
wait "$STREAM"
router alpha 10.79.0.2 0

say "live flows: a port the server answered itself goes to a router"
table 'rule alpha tcp 7000 7010 7000'
check "udp 9000 and the range not forwarded" nat sync
stream 9000 41003
# The server's own flow, from its port 42000 (inside the range the next
# sync gives bravo) to a peer at 198.51.100.1: it must stay the server's.
py "$VPS" peer 0.0.0.0 42000 198.51.100.1 30000 2 10 "$TMP/vps-peer.log" &
P1=$!
sleep 0.5
py "$INET" peer 198.51.100.1 30000 10.2.0.1 42000 0.1 9 "$TMP/inet-peer.log" &
P2=$!
sleep 2
check "the server hears its peer" heard "$TMP/vps-peer.log" peer30000
table "$T1"
t=$(now)
check "udp 9000 to alpha, the whole range to bravo" nat sync
check "it moves the flow the server answered" sh -c 'printf "%s\n" "$1" | grep -q "moved [1-9][0-9]* live flows"' sh "$OUT"
sleep 2.5
check "the stream reaches alpha within 2 s" got "$TMP/alpha.log" 41003 "$t" "$(later "$t" 2)"
check "the server's own flow, on a port bravo now holds, still reaches the server" \
	heard "$TMP/vps-peer.log" peer30000 "$(later "$t" 0.5)" "$(later "$t" 2.5)"
wait "$STREAM" "$P1" "$P2"

say "live flows: cengarde-nat restarted (down, apply) while another rule keeps conntrack on"
# As Docker's rules do: then the stream is tracked, answered by the server,
# between down and apply, and apply has no applied state to compare with.
nsexec "$VPS" iptables -A FORWARD -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
stream 30000 41004
sleep 2
check "the stream reaches bravo, the whole range's holder" got "$TMP/bravo.log" 41004
check "down" nat down
t=$(now)
sleep 2
check "after down nothing reaches bravo" not got "$TMP/bravo.log" 41004 "$(later "$t" 0.3)"
t=$(now)
check "apply" nat apply
sleep 2.5
check "the stream reaches bravo again within 2 s" got "$TMP/bravo.log" 41004 "$t" "$(later "$t" 2)"
wait "$STREAM"
nsexec "$VPS" iptables -D FORWARD -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

say "protocol 3: the engine's IP pass file, read as the whole range"
# pass on|off: the IP pass file as the engine writes it: its user's, a
# temporary file renamed over the old one.
pass() {
	echo "$1" >"$R/var/lib/cengarde/passthrough.tmp"
	chown nobody "$R/var/lib/cengarde/passthrough.tmp"
	mv "$R/var/lib/cengarde/passthrough.tmp" "$R/var/lib/cengarde/passthrough"
}
printf 'ENGINE_USER=nobody\nFORWARD_SKIP_SRC="10.1.0.3/32"\n' >"$R/etc/cengarde/nat.conf"
pass on
check "IP pass on" nat sync
check "to alpha, the router of the first slot" sh -c 'printf "%s\n" "$1" | grep -q "IP pass on (router), to alpha (10.79.0.2)"' sh "$OUT"
check "tcp 20000 reaches alpha" is "alpha:20000:10.1.0.2" py "$INET" tcp 10.1.0.1 20000
check "tcp 22 stays on the server" is "vps:22:10.1.0.2" py "$INET" tcp 10.1.0.1 22
stream 30000 41005
sleep 2
check "a stream reaches alpha" got "$TMP/alpha.log" 41005
pass off
t=$(now)
check "IP pass off" nat sync
sleep 2.5
check "the stream left alpha within 0.5 s" not got "$TMP/alpha.log" 41005 "$(later "$t" 0.5)"
check "tcp 20000 stays on the server now" is none py "$INET" tcp 10.1.0.1 20000
pass on
t=$(now)
check "IP pass on again, the stream still running" nat sync
sleep 2.5
check "the stream reaches alpha again within 2 s" got "$TMP/alpha.log" 41005 "$t" "$(later "$t" 2)"
wait "$STREAM"

say "down"
check "cengarde-nat down" nat down
check "the firewall is as it was before" [ "$(saved)" = "$before" ]
check "check while down does nothing" nat check
check "it says so" sh -c 'printf "%s\n" "$1" | grep -q "down: nothing to check"' sh "$OUT"
check "still as before" [ "$(saved)" = "$before" ]

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
