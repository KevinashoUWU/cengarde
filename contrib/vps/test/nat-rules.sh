#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# The rules cengarde-nat writes, against golden files (fixtures/rules), with
# fake ss and iptables (-S DOCKER) in PATH, so that nothing of this machine
# leaks in:
# - no router; one router with IP pass off, on, from nat.conf until the
#   router asks, and with forwarding off;
# - the forward table of protocol 4 (fixtures/forward-two-routers): a port
#   to another port, a range to itself, a shifted range, both protocols,
#   and the whole range of another router with the explicit rules carved
#   out of it;
# - the reserved set: below 1024 with FORWARD_ALLOW_LOW, this server's own
#   listeners and Docker's ports (fixtures/ss-*.txt, docker-nat.txt),
#   FORWARD_UNRESERVE, FORWARD_RESERVED; "reserved -v" with the reasons;
# - FORWARD_SKIP_SRC; the jumps and the MASQUERADE without and with PUB_IF;
#   the hairpin accept before the tunnels' DROP, and its SNAT; IPv6;
# - a forward table with bad lines (fixtures/forward-bad): each skipped and
#   logged by its number and reason, never its content; a table or IP pass
#   file that is a symlink, a FIFO, someone else's or over 64 KiB: refused
#   whole; a router's file writable by others, or its directory: left out;
#   a nat.conf that is not root's: refused;
# - every output loaded by iptables-restore --test --noflush (and
#   ip6tables-restore), in a network namespace of its own.
#
#   sudo sh contrib/vps/test/nat-rules.sh       (UPDATE=1 rewrites the golden files)
#
# Needs root, iproute2 and iptables. CENGARDE_NAT names another cengarde-nat.
# Run it under the lab's lock (flock /tmp/cengarde-netns.lock) next to
# other network namespace tests.
#
# SPDX-License-Identifier: GPL-2.0-only
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
NAT=${CENGARDE_NAT:-$HERE/../cengarde-nat}
FIX=$HERE/fixtures
GOLD=$FIX/rules
NS=cgnr-test
TMP=
fails=0

say() { echo "nat-rules: $*"; }
ok() { echo "nat-rules: ok: $*"; }
bad() {
	echo "nat-rules: FAIL: $*"
	fails=$((fails + 1))
}
check() {
	what=$1
	shift
	if "$@"; then ok "$what"; else bad "$what"; fi
}
not() { ! "$@"; }
cleanup() {
	ip netns del "$NS" 2>/dev/null
	[ -z "$TMP" ] || rm -rf "$TMP"
}

[ "$(id -u)" -eq 0 ] || {
	echo "nat-rules.sh: run it as root" >&2
	exit 2
}
for cmd in ip iptables-restore; do
	command -v "$cmd" >/dev/null || {
		echo "nat-rules.sh: $cmd is missing" >&2
		exit 2
	}
done
cleanup
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
[ -z "${RUN:-}" ] || mkdir -p "$RUN"
TMP=$(mktemp -d "${RUN:-/tmp}/nat-rules.XXXXXX")
ip netns add "$NS" || exit 2

BIN=$TMP/bin
mkdir -p "$BIN"
cat >"$BIN/ss" <<'EOF'
#!/bin/sh
case $1 in
-Hlntp) cat "$FIX/ss-tcp.txt" ;;
-Hlnup) cat "$FIX/ss-udp.txt" ;;
*) exit 1 ;;
esac
EOF
cat >"$BIN/iptables" <<'EOF'
#!/bin/sh
[ "$*" = "-w -t nat -S DOCKER" ] || exit 1
cat "$FIX/docker-nat.txt"
EOF
chmod +x "$BIN/ss" "$BIN/iptables"
export FIX

R=
# root NAME [NAT.CONF LINES]: a server's files: nat.conf (auto-reserve off
# unless set), the routers alpha (10.79.0.2) and bravo (10.79.12.34).
root() {
	R=$TMP/$1
	mkdir -p "$R/etc/cengarde/clients" "$R/var/lib/cengarde" "$R/proc/sys/net/ipv4"
	chmod 0700 "$R/etc/cengarde/clients"
	echo "32768 60999" >"$R/proc/sys/net/ipv4/ip_local_port_range"
	printf 'ENGINE_USER=nobody\nFORWARD_AUTO_RESERVE=no\n%s\n' "${2:-}" >"$R/etc/cengarde/nat.conf"
	router alpha 10.79.0.2 0 yes
	router bravo 10.79.12.34 1 yes
}
router() { # NAME ADDR SLOT FORWARD
	printf 'SECRET=x\nSLOT=%s\nTUNNEL_ADDR=%s\nFORWARD=%s\n' "$3" "$2" "$4" >"$R/etc/cengarde/clients/$1"
	chmod 0600 "$R/etc/cengarde/clients/$1"
}
# engine FILE CONTENT: a file as the engine (user nobody) writes it.
engine() {
	printf '%s\n' "$2" >"$1"
	chown nobody "$1"
}
# nat ARGS...: cengarde-nat on R; stdout in OUT, stderr in ERR.
nat() {
	env CENGARDE_ROOT="$R" PATH="$BIN:$PATH" sh "$NAT" "$@" >"$TMP/out" 2>"$TMP/err"
	RC=$?
	OUT=$(cat "$TMP/out") ERR=$(cat "$TMP/err")
	return "$RC"
}
# golden NAME: OUT is fixtures/rules/NAME (UPDATE=1 writes it).
golden() {
	if [ "${UPDATE:-}" = 1 ]; then
		printf '%s\n' "$OUT" >"$GOLD/$1"
		return 0
	fi
	if printf '%s\n' "$OUT" | cmp -s - "$GOLD/$1"; then
		return 0
	fi
	printf '%s\n' "$OUT" | diff -u "$GOLD/$1" - | sed 's/^/    /'
	return 1
}
# loads 4|6: iptables-restore --test --noflush takes OUT, in NS.
loads() {
	if [ "$1" = 4 ]; then
		printf '%s\n' "$OUT" | ip netns exec "$NS" iptables-restore --test --noflush
	else
		printf '%s\n' "$OUT" | ip netns exec "$NS" ip6tables-restore --test --noflush
	fi
}
err_has() { printf '%s\n' "$ERR" | grep -qF -- "$1"; }
out_has() { printf '%s\n' "$OUT" | grep -qxF -- "$1"; }
# rules4 NAME WHAT: rules 4 against the golden NAME.4, and loadable.
rules4() {
	if nat rules 4; then
		check "$2: as $1.4" golden "$1.4"
		check "$2: iptables-restore --test takes it" loads 4
	else
		bad "$2: cengarde-nat rules 4 failed: $ERR"
	fi
}

say "no router"
root none
rm -f "$R/etc/cengarde/clients/"*
rules4 none "no router, nothing forwarded"
check "the hairpin's accept comes before the tunnels' DROP" \
	sh -c 'printf "%s\n" "$1" | grep -n "" | grep -E "ctstate DNAT -j ACCEPT|-i cg-\+ -o cg-\+ -j DROP" | cut -d: -f1 | tr "\n" " " | awk "{ exit !(\$1 < \$2) }"' sh "$OUT"
check "and the metadata REJECT before both" \
	sh -c 'printf "%s\n" "$1" | grep "^-A CG_FWD" | head -n 1 | grep -q "169.254.0.0/16 -j REJECT"' sh "$OUT"
check "hairpin SNAT to 10.79.0.1, MASQUERADE out of anything but a tunnel" \
	sh -c 'printf "%s\n" "$1" | grep -qx -- "-A CG_POST -s 10.79.0.0/16 -o cg-+ -m conntrack --ctstate DNAT -j SNAT --to-source 10.79.0.1" &&
		printf "%s\n" "$1" | grep -qx -- "-A CG_POST -s 10.79.0.0/16 ! -o cg-+ -j MASQUERADE"' sh "$OUT"

say "one router (protocol 3)"
root v3
rm -f "$R/etc/cengarde/clients/bravo"
engine "$R/var/lib/cengarde/passthrough" off
rules4 v3-off "IP pass off"
engine "$R/var/lib/cengarde/passthrough" on
rules4 v3-on "IP pass on: the whole range to 10.79.0.2"
rm -f "$R/var/lib/cengarde/passthrough"
rules4 v3-off "no IP pass file, PASSTHROUGH=no"
printf 'PASSTHROUGH=yes\n' >>"$R/etc/cengarde/nat.conf"
rules4 v3-on "no IP pass file, PASSTHROUGH=yes in nat.conf"
engine "$R/var/lib/cengarde/passthrough" off
rules4 v3-off "the router's off wins over PASSTHROUGH=yes"
engine "$R/var/lib/cengarde/passthrough" on
router alpha 10.79.0.2 0 no
rules4 v3-off "forwarding off for the router (forward off)"
router alpha 10.79.0.2 0 yes
engine "$R/var/lib/cengarde/passthrough" maybe
rules4 v3-off "an IP pass file that says neither on nor off"
check "logged by line, not content" sh -c 'printf "%s\n" "$1" | grep -q "line 1 skipped: not on or off" && ! printf "%s\n" "$1" | grep -q maybe' sh "$ERR"

say "the forward table (protocol 4)"
root table "FORWARD_FILE=$TMP/table/var/lib/cengarde/forward"
cp "$FIX/forward-two-routers" "$R/var/lib/cengarde/forward"
chown nobody "$R/var/lib/cengarde/forward"
rules4 table "explicit rules of two routers, the whole range of one"
check "a port to another port" out_has "-A CG_PRE -p tcp --dport 9000 -j DNAT --to-destination 10.79.0.2:22"
check "a range to itself" out_has "-A CG_PRE -p tcp --dport 7000:7010 -j DNAT --to-destination 10.79.0.2"
check "a shifted range" out_has "-A CG_PRE -p udp --dport 5000:5010 -j DNAT --to-destination 10.79.12.34:15000-15010/5000"
check "the whole range last, after every explicit rule" \
	sh -c 'printf "%s\n" "$1" | grep -- "-j DNAT" | tail -n 2 | grep -c "1024:65000 -j DNAT --to-destination 10.79.12.34$" | grep -qx 2' sh "$OUT"
check "nothing logged" [ -z "$ERR" ]

say "a forward table with bad lines"
root badtable "FORWARD_FILE=$TMP/badtable/var/lib/cengarde/forward"
router bravo 10.79.12.34 1 no
cp "$FIX/forward-bad" "$R/var/lib/cengarde/forward"
chown nobody "$R/var/lib/cengarde/forward"
rules4 badtable "only the good lines"
for l in "3 expected: rule NAME" "4 no such router" "5 protocol not tcp or udp" "6 expected: rule NAME" \
	"7 a port is not 1 to 65535" "8 first port after the last" "9 a port is not 1 to 65535" \
	"10 the target range goes past 65535" "11 a reserved port" "12 a reserved port" "13 overlaps an earlier rule" \
	"14 forwarding is off for that router" "16 a second holder of the whole range" "17 unknown line" \
	"18 a port is not 1 to 65535"; do
	check "line ${l%% *} skipped: ${l#* }" err_has "line ${l%% *} skipped: ${l#* }"
done
check "15 lines logged, no more" [ "$(printf '%s\n' "$ERR" | grep -c 'skipped')" -eq 15 ]
check "never their content" not err_has SECRETMARK
check "never a path from them" not err_has "../"

say "the engine's file refused whole"
root refuse "FORWARD_FILE=$TMP/refuse/var/lib/cengarde/forward"
F=$R/var/lib/cengarde/forward
ln -s "$FIX/forward-two-routers" "$F"
rules4 refused "a symlink"
check "it says so" err_has "is a symlink: refused"
rm -f "$F"
cp "$FIX/forward-two-routers" "$F"
rules4 refused "a file owned by root, not the engine's user"
check "it says so" err_has "is not owned by nobody: refused"
chown nobody "$F"
head -c 70000 /dev/zero | tr '\0' '#' >>"$F"
rules4 refused "over 64 KiB"
check "it says so" err_has "larger than 64 KiB: refused"
rm -f "$F"
mkdir "$F"
chown nobody "$F"
rules4 refused "a directory"
check "it says so" err_has "is not a regular file: refused"
rmdir "$F"
mkfifo "$F"
chown nobody "$F"
rules4 refused "a FIFO (it would block the open)"
check "it says so" err_has "refused"
rm -f "$F"
cp "$FIX/forward-two-routers" "$F"
chown nobody "$F"
sed -i '1s/table 1,/table 2,/' "$F"
chown nobody "$F"
rules4 refused "a table of another version (2)"
check "it says so" err_has "written by another version of the engine (table 2): refused"
printf 'ENGINE_USER=no-such-user-here\n' >>"$R/etc/cengarde/nat.conf"
rules4 refused "no user to own it"
check "it says so" err_has "there is no user no-such-user-here"
root refuse3
rm -f "$R/etc/cengarde/clients/bravo"
engine "$TMP/on" on
ln -s "$TMP/on" "$R/var/lib/cengarde/passthrough"
rules4 v3-off "an IP pass file that is a symlink: IP pass off"
check "it says so" err_has "is a symlink: refused"

say "the routers' files"
root clients "FORWARD_FILE=$TMP/clients/var/lib/cengarde/forward"
cp "$FIX/forward-two-routers" "$R/var/lib/cengarde/forward"
chown nobody "$R/var/lib/cengarde/forward"
chmod 0606 "$R/etc/cengarde/clients/bravo"
rules4 alpha-only "bravo's file writable by others: bravo left out"
check "it says so" err_has "router bravo ignored"
chmod 0600 "$R/etc/cengarde/clients/bravo"
chown nobody "$R/etc/cengarde/clients/bravo"
rules4 alpha-only "bravo's file not root's: bravo left out"
chmod 0777 "$R/etc/cengarde/clients"
rules4 no-router-table "the directory writable by others: no router served"
check "it says so" err_has "no router served"
chmod 0700 "$R/etc/cengarde/clients"
ln -s "$R/etc/cengarde/clients/alpha" "$R/etc/cengarde/clients/charlie"
nat rules 4
check "a router's file that is a symlink: left out" err_has "router charlie ignored"

say "the reserved set"
root reserved 'FORWARD_AUTO_RESERVE=yes
FORWARD_ALLOW_LOW="tcp:80 tcp:443 udp:2000"
FORWARD_UNRESERVE="tcp:8123"
FORWARD_RESERVED="udp:5000-5010 bogus"'
rm -f "$R/etc/cengarde/clients/bravo"
engine "$R/var/lib/cengarde/passthrough" on
rules4 reserved "below 1024 but tcp 80 and 443, listeners, Docker, FORWARD_RESERVED, the whole range"
check "a bad token and a port above 1023 in FORWARD_ALLOW_LOW: said, ignored" \
	sh -c 'printf "%s\n" "$1" | grep -q "FORWARD_RESERVED: bogus is not" && printf "%s\n" "$1" | grep -q "FORWARD_ALLOW_LOW: udp:2000: only ports below 1024"' sh "$ERR"
nat reserved -v
check "reserved -v: the reasons" golden reserved-v.txt
check "loopback listeners, a UDP socket in the ephemeral range and FORWARD_UNRESERVE are not reserved" \
	sh -c '! printf "%s\n" "$1" | grep -Eq "^(tcp:631|udp:323|udp:40000|tcp:8123) "' sh "$OUT"
nat reserved
check "reserved: merged" golden reserved.txt

say "sources skipped, PUB_IF, IPv6"
root skip 'FORWARD_SKIP_SRC="192.168.1.0/24 10.0.0.5 300.1.1.1/24 192.168.2.0/40"
PUB_IF=eth0'
rm -f "$R/etc/cengarde/clients/bravo"
engine "$R/var/lib/cengarde/passthrough" on
rules4 skip-pubif "FORWARD_SKIP_SRC and PUB_IF"
check "bad CIDRs said and ignored" sh -c 'printf "%s\n" "$1" | grep -q "300.1.1.1/24 is not" && printf "%s\n" "$1" | grep -q "192.168.2.0/40 is not"' sh "$ERR"
nat jumps 4
check "jumps with PUB_IF: from it and from the tunnels" golden jumps-pubif.4
root jumps
nat jumps 4
check "jumps without PUB_IF: no interface" golden jumps.4
nat jumps 6
check "IPv6 jump" golden jumps.6
nat rules 6
check "IPv6 rules: cengarde open, WireGuard only on lo" golden rules.6
if ip netns exec "$NS" ip6tables -w -n -L INPUT >/dev/null 2>&1; then
	check "ip6tables-restore --test takes them" loads 6
else
	say "note: no ip6tables in this kernel; the IPv6 rules are not loaded here (vps.yml does)"
fi
printf 'PUB_IF="eth0;reboot"\n' >>"$R/etc/cengarde/nat.conf"
check "a PUB_IF that is no interface name is refused" not nat rules 4

say "nat.conf"
root conf
chown nobody "$R/etc/cengarde/nat.conf"
check "not root's: refused" not nat rules 4
check "it says so" err_has "must be a file owned by root"
chown root "$R/etc/cengarde/nat.conf"
chmod 0666 "$R/etc/cengarde/nat.conf"
check "writable by others: refused" not nat rules 4
chmod 0644 "$R/etc/cengarde/nat.conf"
printf 'WG_PORT=65400\nCLIENT_IP=10.79.0.2\n' >>"$R/etc/cengarde/nat.conf"
nat rules 4
check "0.4's WG_PORT is the base of the WireGuard ports" out_has "-A CG_IN ! -i lo -p udp --dport 65400:65431 -j DROP"
printf 'WG_PORT_BASE=65520\n' >>"$R/etc/cengarde/nat.conf"
check "WireGuard ports past 65535: refused" not nat rules 4

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
