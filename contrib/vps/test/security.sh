#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# The security rules of cengarde-nat, in network namespaces with the real
# iptables; a veth pair stands in for a router's WireGuard, cg-router:
#
#   inet [ext 10.1.0.2, 169.254.169.254] -- [pub 10.1.0.1] vps
#     [cg-router 10.79.0.1] -- [wg 10.79.0.2] router
#
# inet is the Internet and the provider's network, with a fake metadata
# service on 169.254.169.254 that serves the "user data". On a VPS it sits
# behind the public interface, so the tunnel's traffic to it is forwarded.
# With no firewall, and then behind a ufw-like one, the test checks that:
# - after "apply" the router cannot read the metadata through the tunnel,
#   and is told so at once, while the VPS itself and the router's Internet
#   work;
# - the REJECT deleted by hand, or the tunnel's ACCEPT (a chain changed by
#   someone else): "check" puts the chain back whole, in order, with no
#   rule twice;
# - WireGuard's ports (65501-65532, one per router) are dropped from the
#   Internet and from the tunnel but answer on lo, and cengarde's port is
#   open;
# - with IPv6, ip6tables accepts cengarde's port and drops WireGuard's;
# - "down" leaves the rules as they were before "apply", also starting from
#   the rules of cengarde 0.4, which "apply" replaces, and runs quietly when
#   they are gone.
#
#   sudo sh contrib/vps/test/security.sh
#
# Needs root, iproute2, iptables, python3 and curl. CENGARDE_NAT names
# another cengarde-nat to test. SECURITY_REQUIRE_V6=1 makes a VPS namespace
# without IPv6 a failure instead of a note (vps.yml sets it). Run it under
# the lab's lock (flock /tmp/cengarde-netns.lock) next to other network
# namespace tests.
#
# SPDX-License-Identifier: GPL-2.0-only
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
NAT=${CENGARDE_NAT:-$HERE/../cengarde-nat}
INET=cgsec-inet VPS=cgsec-vps ROUTER=cgsec-router
CG_PORT=65500 WG_PORTS=65501:65532
META=169.254.169.254
SECRET=not-the-real-secret-$$
TMP=
fails=0

say() { echo "security: $*"; }
ok() { echo "security: ok: $*"; }
bad() {
	echo "security: FAIL: $*"
	fails=$((fails + 1))
}
# check WHAT COMMAND...: ok when COMMAND succeeds.
check() {
	what=$1
	shift
	if "$@"; then ok "$what"; else bad "$what"; fi
}
not() { ! "$@"; }
nsexec() { ip netns exec "$@"; }

cleanup() {
	for ns in "$INET" "$VPS" "$ROUTER"; do
		pids=$(ip netns pids "$ns" 2>/dev/null)
		# shellcheck disable=SC2086 # one PID per word
		[ -z "$pids" ] || kill $pids 2>/dev/null
		ip netns del "$ns" 2>/dev/null
	done
	[ -z "$TMP" ] || rm -rf "$TMP"
}

if [ "$(id -u)" -ne 0 ]; then
	echo "security.sh: run it as root" >&2
	exit 2
fi
for cmd in ip iptables python3 curl; do
	command -v "$cmd" >/dev/null || {
		echo "security.sh: $cmd is missing" >&2
		exit 2
	}
done
cleanup
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
[ -z "${RUN:-}" ] || mkdir -p "$RUN"
TMP=$(mktemp -d "${RUN:-/tmp}/security.XXXXXX")

# cengarde-nat as on a server, with its files kept in TMP/root.
mkdir -p "$TMP/root/etc/cengarde"
echo PASSTHROUGH=no >"$TMP/root/etc/cengarde/nat.conf"
nat() { nsexec "$VPS" env CENGARDE_ROOT="$TMP/root" sh "$NAT" "$@"; }

# udp.py listen PORT FILE: FILE.ready once bound, FILE if a datagram comes
# within 1.5 s. udp.py send ADDR PORT: two datagrams.
cat >"$TMP/udp.py" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
if sys.argv[1] == "listen":
    s.bind(("0.0.0.0", int(sys.argv[2])))
    s.settimeout(1.5)
    open(sys.argv[3] + ".ready", "w").close()
    try:
        s.recvfrom(64)
        open(sys.argv[3], "w").close()
    except socket.timeout:
        pass
else:
    for _ in range(2):
        s.sendto(b"x", (sys.argv[2], int(sys.argv[3])))
        time.sleep(0.05)
EOF
# udp LISTEN_NS SEND_NS ADDR PORT: does a datagram sent from SEND_NS to
# ADDR:PORT reach a socket on PORT in LISTEN_NS?
udp() {
	rm -f "$TMP/got" "$TMP/got.ready"
	nsexec "$1" python3 "$TMP/udp.py" listen "$4" "$TMP/got" &
	listener=$!
	n=0
	while [ ! -e "$TMP/got.ready" ] && [ "$n" -lt 50 ]; do
		sleep 0.1
		n=$((n + 1))
	done
	nsexec "$2" python3 "$TMP/udp.py" send "$3" "$4"
	wait "$listener"
	[ -e "$TMP/got" ]
}
# fetch NS ADDR: NS reads the user data at ADDR; CODE is curl's exit status
# (0 read, 7 refused at once, 28 no answer).
fetch() {
	rm -f "$TMP/body"
	nsexec "$1" curl -s -m 3 -o "$TMP/body" "http://$2/user-data"
	CODE=$?
	[ "$CODE" -eq 0 ] && grep -q "$SECRET" "$TMP/body"
}
# rejected NS ADDR: the user data cannot be read, and NS learns it at once.
rejected() {
	if fetch "$1" "$2"; then
		say "$1 read the user data at $2"
		return 1
	fi
	[ "$CODE" -eq 7 ] || say "curl exit $CODE: dropped instead of rejected?"
	[ "$CODE" -eq 7 ]
}
# rules iptables|ip6tables: the VPS's rules, without counters.
rules() {
	for t in filter nat; do
		[ "$1 $t" != "ip6tables nat" ] || continue
		nsexec "$VPS" "$1" -t "$t" -S 2>/dev/null | sed "s/^/$t /"
	done
}
# has iptables|ip6tables CHAIN RULE...: the VPS has RULE (filter table).
has() {
	cmd=$1
	shift
	nsexec "$VPS" "$cmd" -C "$@" 2>/dev/null
}
# dup: a rule twice, IPv4 and IPv6 apart (their policy lines are the same).
dup() {
	rules iptables | sort | uniq -d | grep -q . ||
		{ [ "$V6" = yes ] && rules ip6tables | sort | uniq -d | grep -q .; }
}
quiet_down() {
	out=$(nat down 2>&1) && [ -z "$out" ]
}
v6_checks() {
	if [ "$V6" = no ]; then
		if [ "${SECURITY_REQUIRE_V6:-0}" = 1 ]; then
			bad "IPv6 required (SECURITY_REQUIRE_V6=1), but the VPS namespace has none (no /proc/net/if_inet6)"
		else
			say "note: no IPv6 in this kernel (no /proc/net/if_inet6), so no ip6tables rules and no IPv6 checks here; vps.yml sets SECURITY_REQUIRE_V6=1"
		fi
		return
	fi
	check "IPv6: INPUT jumps to CG_IN" has ip6tables INPUT -j CG_IN
	check "IPv6: cengarde's port accepted" \
		has ip6tables CG_IN -p udp --dport "$CG_PORT" -j ACCEPT
	check "IPv6: WireGuard's ports dropped except on lo" \
		has ip6tables CG_IN ! -i lo -p udp --dport "$WG_PORTS" -j DROP
}

say "namespaces: $INET (Internet, metadata on $META) -- $VPS -- $ROUTER"
set -e # any failure here ends the test, and the trap cleans up
for ns in "$INET" "$VPS" "$ROUTER"; do
	ip netns add "$ns"
	ip -n "$ns" link set lo up
done
ip link add pub netns "$VPS" type veth peer name ext netns "$INET"
ip -n "$VPS" addr add 10.1.0.1/24 dev pub
ip -n "$VPS" link set pub up
ip -n "$INET" addr add 10.1.0.2/24 dev ext
ip -n "$INET" addr add "$META/32" dev ext
ip -n "$INET" link set ext up
# As wg-quick sets cg-router up: 10.79.0.1/32, and a route to the router.
ip link add cg-router netns "$VPS" type veth peer name wg netns "$ROUTER"
ip -n "$VPS" addr add 10.79.0.1/32 dev cg-router
ip -n "$VPS" link set cg-router up
ip -n "$VPS" route add 10.79.0.2/32 dev cg-router
ip -n "$ROUTER" addr add 10.79.0.2/32 dev wg
ip -n "$ROUTER" link set wg up
ip -n "$ROUTER" route add 10.79.0.1/32 dev wg
ip -n "$VPS" route add default via 10.1.0.2
ip -n "$ROUTER" route add default via 10.79.0.1 dev wg onlink
nsexec "$VPS" sysctl -qw net.ipv4.ip_forward=1
set +e
V6=no
nsexec "$VPS" test -e /proc/net/if_inet6 && V6=yes

mkdir "$TMP/www"
echo "$SECRET" >"$TMP/www/user-data"
nsexec "$INET" python3 -m http.server 80 --bind 0.0.0.0 --directory "$TMP/www" >/dev/null 2>&1 &
n=0
until nsexec "$INET" curl -s -m 1 -o /dev/null "http://127.0.0.1/user-data"; do
	n=$((n + 1))
	if [ "$n" -ge 50 ]; then
		echo "security.sh: the fake metadata service did not start" >&2
		exit 1
	fi
	sleep 0.1
done

before4=$(rules iptables)
before6=$(rules ip6tables)
check "without the rules, WireGuard's port is reachable from the Internet (the test sees it)" \
	udp "$VPS" "$INET" 10.1.0.1 65501

say "apply, with no firewall"
check "cengarde-nat apply" nat apply
check "the VPS itself reads the metadata" fetch "$VPS" "$META"
check "the router reaches the Internet through the tunnel" fetch "$ROUTER" 10.1.0.2
check "the router cannot read the metadata through the tunnel, and is told at once" \
	rejected "$ROUTER" "$META"
check "WireGuard's port is dropped from the Internet" not udp "$VPS" "$INET" 10.1.0.1 65501
check "another router's WireGuard port too" not udp "$VPS" "$INET" 10.1.0.1 65532
check "WireGuard's port is dropped from the tunnel" not udp "$VPS" "$ROUTER" 10.79.0.1 65501
check "WireGuard's port answers on lo" udp "$VPS" "$VPS" 127.0.0.1 65501
check "cengarde's port is reachable from the Internet" udp "$VPS" "$INET" 10.1.0.1 "$CG_PORT"
v6_checks

say "the REJECT deleted by hand, then check"
nsexec "$VPS" iptables -D CG_FWD -i cg-+ -d 169.254.0.0/16 -j REJECT --reject-with icmp-net-prohibited
check "without it the router reads the metadata (the test sees the leak)" fetch "$ROUTER" "$META"
check "cengarde-nat check" nat check
check "the REJECT is back above the tunnels' ACCEPT" rejected "$ROUTER" "$META"
check "no rule twice" not dup

say "the tunnels' ACCEPT deleted by hand, then check"
nsexec "$VPS" iptables -D CG_FWD -i cg-+ -j ACCEPT
check "cengarde-nat check" nat check
check "the REJECT is still above the tunnels' ACCEPT" rejected "$ROUTER" "$META"
check "the router reaches the Internet through the tunnel" fetch "$ROUTER" 10.1.0.2
check "no rule twice" not dup

say "down"
check "cengarde-nat down" nat down
check "the IPv4 rules are as before apply" [ "$(rules iptables)" = "$before4" ]
check "the IPv6 rules are as before apply" [ "$(rules ip6tables)" = "$before6" ]
check "down again succeeds, quietly" quiet_down

say "behind a ufw-like firewall: INPUT and FORWARD dropped unless allowed"
nsexec "$VPS" iptables -A INPUT -i lo -j ACCEPT
nsexec "$VPS" iptables -A INPUT -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
nsexec "$VPS" iptables -P INPUT DROP
nsexec "$VPS" iptables -P FORWARD DROP
before4=$(rules iptables)
check "cengarde-nat apply" nat apply
check "cengarde's port is reachable from the Internet" udp "$VPS" "$INET" 10.1.0.1 "$CG_PORT"
check "WireGuard's port answers on lo" udp "$VPS" "$VPS" 127.0.0.1 65501
check "the router reaches the Internet through the tunnel" fetch "$ROUTER" 10.1.0.2
check "the router cannot read the metadata through the tunnel" rejected "$ROUTER" "$META"
check "cengarde-nat down" nat down
check "the firewall is as before apply" [ "$(rules iptables)" = "$before4" ]

say "from the rules of cengarde 0.4 (its wg0, its PUB_IF)"
old04() {
	nsexec "$VPS" iptables -I INPUT -i pub -p udp --dport "$CG_PORT" -j ACCEPT
	nsexec "$VPS" iptables -I INPUT -i pub -p udp --dport 65501 -j DROP
	nsexec "$VPS" iptables -I INPUT ! -i lo -p udp --dport 65501 -j DROP
	nsexec "$VPS" iptables -I FORWARD -i wg0 -j ACCEPT
	nsexec "$VPS" iptables -I FORWARD -i wg0 -d 169.254.0.0/16 -j REJECT --reject-with icmp-net-prohibited
	nsexec "$VPS" iptables -I FORWARD -o wg0 -j ACCEPT
	nsexec "$VPS" iptables -t nat -A POSTROUTING -s 10.79.0.0/30 -o pub -j MASQUERADE
}
old04
check "cengarde-nat down" nat down
check "nothing is left of them" [ "$(rules iptables)" = "$before4" ]
old04
check "cengarde-nat apply over them" nat apply
check "they are gone" not sh -c 'ip netns exec "$1" iptables -S | grep -Eq "wg0|-i pub -p udp"' sh "$VPS"
check "the router cannot read the metadata through the tunnel" rejected "$ROUTER" "$META"
check "no rule twice" not dup
check "cengarde-nat down" nat down
check "nothing is left" [ "$(rules iptables)" = "$before4" ]

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
