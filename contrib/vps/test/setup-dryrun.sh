#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# cengarde-vps-setup and install.sh without touching this machine: every
# path under a directory of the test's own (CENGARDE_ROOT), and fake
# systemctl, wg, wg-quick, ip, stty, cengarde-nat, make, sysctl,
# systemd-sysusers and userdel in PATH, which log what they are asked. The
# engine is the real one for "keys", "genkey" and "-t"; its control socket
# is a fake that answers ok, restarting, an error, or nothing. Checks:
# - add (the tunnel address the secret gives, a [client NAME] section), add
#   again (nothing changes), a new secret (refused, then --replace: its new
#   address, WireGuard restarted, the engine restarting), --rename, a second
#   router (its own slot, port and address), a tunnel address taken, the
#   33rd router, an engine of protocol 3 and --proto 12 refused (with the
#   upgrade command), WireGuard ports out of range; remove, the last one
#   leaving a key that serves nobody; list and forward with each router's
#   sessions;
# - the secret never on a command line, and only in clients/NAME;
#   on a terminal (script(1)), echo off while it is typed, and back on;
# - a configuration the engine refuses, a reload that fails and one that
#   hangs (cut by the timeout): the previous files back, exit 1; with
#   --replace, the engine's IP pass file too (a router may even be called
#   passthrough);
# - the one-shot import of /etc/cengarde/secret: imported once and deleted,
#   at 10.79.0.2 from a cloud-config before 0.5.2, at ROUTER_NAME and
#   ROUTER_TUNNEL_ADDR from a newer one (as the router's, from its secret,
#   when left as the repository has them); after remove, after --replace
#   and with slot 0 taken, running again leaves clients/ as it was;
# - ipv6 on|off: refused on a server without IPv6; on, each router's
#   WireGuard gets its ULA (::1 here, ::2 the router), TUNNEL_IPV6=yes in
#   nat.conf and cengarde-nat applies it; off takes it back; the import of
#   a cloud-config with TUNNEL_IPV6=yes;
# - protocontroller on|off (native only): protocontroller = b, tcp_bbr at
#   boot, PROTOCONTROLLER in nat.conf;
# - fec on|rlc|off: fec = yes (and fec_code = rlc), FEC in nat.conf (story 026);
# - tunnel native|wireguard: native writes tunnel = native, the TUN's
#   10.79.0.1/16 and each router's address as its tun_route (its ULA too
#   with ipv6 on), a systemd drop-in that allows /dev/net/tun, restarts
#   the engine (a reload would keep the old unit) and only then stops each
#   router's WireGuard; a router added then gets no WireGuard; native again
#   changes nothing; wireguard puts every file back as it was, drop-in gone;
#   a TUNNEL that is neither refused; the import of a cloud-config with
#   TUNNEL=native;
# - forward add|del: one spelling of a rule, in the router's file and its
#   section; refused: a reserved port (with the reason), an overlap with
#   its own rule or another router's, past its limit, a bad rule; forward
#   off comments its rules out;
# - forward off|on|limit|allow|disallow|reserve|unreserve, and their
#   refusals; allow says how a router gets the port, and a
#   PASSTHROUGH_PORTS below 1024, which cengarde 0.4 forwarded whole, is
#   warned about;
# - a 0.4 server: wg0 migrated and its IP pass state removed (the router
#   asks again), by cengarde-vps-setup and by install.sh (firewalld refused
#   before anything changes, a build that fails before wg0 is touched, the
#   old service stopped before systemd-sysusers, the nftables drop-in only
#   when nftables is enabled, run twice), and on a server whose running
#   engine refuses the new configuration (0.5.0 and before: no reload asked
#   of it, the restart reads it); purge writes the secret, and
#   whether the router holds IP pass, back for 0.4's install.sh;
# - what systemd runs when it stops cengarde-nat.service (ExecStop) or 0.4's
#   wg0 (PostDown) takes the admin lock, as PID 1's child, not the caller's:
#   a stand-in for it gets the lock at once, during purge and the migration;
# - a server of your own: no secret (the add command shown, a key that
#   serves nobody), a private main IPv4 (FORWARD_SKIP_SRC and what to open,
#   for a home router or a cloud's security list), carrier-grade NAT.
#
#   sh contrib/vps/test/setup-dryrun.sh
#
# Needs root (files owned by root, as on a server), wg (wireguard-tools),
# script (util-linux), python3 and a C compiler for the engine.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
SETUP=$SRC/contrib/vps/cengarde-vps-setup
INSTALL=$SRC/contrib/vps/install.sh
TMP=
fails=0

say() { echo "setup-dryrun: $*"; }
ok() { echo "setup-dryrun: ok: $*"; }
bad() {
	echo "setup-dryrun: FAIL: $*"
	fails=$((fails + 1))
}
check() {
	what=$1
	shift
	if "$@"; then ok "$what"; else bad "$what"; fi
}
not() { ! "$@"; }
cleanup() { [ -z "$TMP" ] || rm -rf "$TMP"; }

[ "$(id -u)" -eq 0 ] || {
	echo "setup-dryrun.sh: run it as root (the files must be root's, as on a server)" >&2
	exit 2
}
for cmd in wg script python3 make cc timeout flock; do
	command -v "$cmd" >/dev/null || {
		echo "setup-dryrun.sh: $cmd is missing" >&2
		exit 2
	}
done
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
TMP=$(mktemp -d)
make -s -C "$SRC/engine" cengarde >/dev/null || {
	echo "setup-dryrun.sh: the engine does not build" >&2
	exit 2
}
ENGINE=$SRC/engine/cengarde
REAL_WG=$(command -v wg)
BIN=$TMP/bin CALLS=$TMP/calls FAKE_STATE=$TMP/units
mkdir -p "$BIN" "$FAKE_STATE"
export ENGINE REAL_WG CALLS FAKE_STATE

# ---- the fakes ----
cat >"$BIN/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >>"$CALLS"
cmd=$1
shift
[ "${1:-}" = -q ] && shift
now=
[ "${1:-}" = --now ] && now=1 && shift
# stopped UNIT: what systemd runs when an active UNIT stops, started as PID 1
# starts it, not as a child of the caller: cengarde-nat.service's ExecStop
# and 0.4's wg0 PostDown take the admin lock; a stand-in waits 3 s for it.
stopped() {
	case $1 in cengarde-nat | cengarde-nat.service | wg-quick@wg0) ;; *) return 0 ;; esac
	[ -e "$FAKE_STATE/$1" ] || return 0
	mkdir -p "$CENGARDE_ROOT/run/lock"
	if env -u CENGARDE_ADMIN_LOCKED flock -w 3 "$CENGARDE_ROOT/run/lock/cengarde-admin.lock" true 9>&-; then
		echo "stop $1: took the admin lock" >>"$CALLS"
	else
		echo "stop $1: the admin lock is busy" >>"$CALLS"
	fi
}
case $cmd in
is-active)
	[ "$1" = firewalld ] && [ -n "${FAKE_FIREWALLD:-}" ] && exit 0
	[ -e "$FAKE_STATE/$1" ]
	exit
	;;
is-enabled)
	[ -e "$FAKE_STATE/enabled-$1" ]
	exit
	;;
enable) for u; do touch "$FAKE_STATE/enabled-$u"; [ -z "$now" ] || touch "$FAKE_STATE/$u"; done ;;
disable)
	for u; do
		rm -f "$FAKE_STATE/enabled-$u"
		[ -z "$now" ] || { stopped "$u"; rm -f "$FAKE_STATE/$u"; }
	done
	;;
start | restart) for u; do touch "$FAKE_STATE/$u"; done ;;
stop) for u; do stopped "$u"; rm -f "$FAKE_STATE/$u"; done ;;
esac
exit 0
EOF
cat >"$BIN/cengarde" <<'EOF'
#!/bin/sh
echo "cengarde $*" >>"$CALLS"
case $1 in
ctl)
	for a; do last=$a; done
	case $last in
	reload)
		case ${FAKE_CTL:-ok} in
		ok) echo ok ;;
		restart) echo "ok: key changed, restarting" ;;
		error)
			echo "cengarde ctl: line 3: refused by the test" >&2
			exit 1
			;;
		hang) exec sleep 30 ;;
		esac
		;;
	links)
		echo "CLIENT           SESSION   NEWEST LINK  STATE    DOWNLOAD LAST RX  ADDRESS                                         LOCAL"
		echo "home             1a2b3c4d  yes    0     live     active   0.4 s    203.0.113.5:40000                               -"
		echo "home             1a2b3c4d         1     live     muted    2.1 s    203.0.113.6:40000                               -"
		echo "office           5e6f7a8b  yes    0     stalled  active   9.0 s    198.51.100.9:40000                              -"
		echo "office           0a0b0c0d         0     stalled  active   31.5 s   198.51.100.9:40001                              -"
		;;
	*) echo "{}" ;;
	esac
	;;
version)
	if [ -n "${FAKE_PROTO:-}" ]; then echo "cengarde test (protocol $FAKE_PROTO)"; else exec "$ENGINE" version; fi
	;;
-t)
	if [ -n "${FAKE_T_FAIL:-}" ]; then
		echo "line 1: refused by the test" >&2
		exit 1
	fi
	exec "$ENGINE" "$@"
	;;
*) exec "$ENGINE" "$@" ;;
esac
EOF
cat >"$BIN/cengarde-nat" <<'EOF'
#!/bin/sh
echo "cengarde-nat $*" >>"$CALLS"
case $1 in
reserved)
	if [ -n "${FAKE_RESERVED:-}" ]; then
		printf '%s\n' $FAKE_RESERVED
	elif [ "${2:-}" = -v ]; then
		printf 'tcp:1-21 below-1024\ntcp:22 ssh\ntcp:23-1023 below-1024\ntcp:8123 listener:hass\nudp:1-1023 below-1024\nudp:65500 cengarde\nudp:65501-65532 wireguard\n'
	else
		printf 'tcp:1-1023\ntcp:8123\nudp:1-1023\nudp:65500-65532\n'
	fi
	;;
status) echo "cengarde-nat: applied" ;;
esac
exit 0
EOF
cat >"$BIN/ip" <<'EOF'
#!/bin/sh
echo "ip $*" >>"$CALLS"
case "$*" in
"-4 route get"*)
	[ -z "${FAKE_MAIN:-}" ] || echo "192.0.2.1 via 192.0.2.254 dev ${FAKE_MAIN#* } src ${FAKE_MAIN% *} uid 0"
	;;
"-4 route show dev"*) for n in ${FAKE_NETS:-}; do echo "$n proto kernel scope link src ${FAKE_MAIN% *}"; done ;;
"-o addr show scope global") echo "2: eth0    inet 203.0.113.10/24 brd 203.0.113.255 scope global eth0" ;;
esac
exit 0
EOF
# wg show cg-NAME endpoints: NAME's endpoint from FAKE_WG_ENDPOINTS
# ("NAME=PORT ..."), on 127.0.0.1 as the engine's sockets are; none without.
cat >"$BIN/wg" <<'EOF'
#!/bin/sh
echo "wg $*" >>"$CALLS"
case $1 in
pubkey | genkey) exec "$REAL_WG" "$@" ;;
show)
	[ "${3:-}" = endpoints ] || exit 0
	for e in ${FAKE_WG_ENDPOINTS:-}; do
		[ "cg-${e%%=*}" = "$2" ] && printf 'cHViS2V5PQ==\t127.0.0.1:%s\n' "${e#*=}"
	done
	;;
esac
exit 0
EOF
cat >"$BIN/wg-quick" <<'EOF'
#!/bin/sh
echo "wg-quick $*" >>"$CALLS"
[ "$1" = strip ] && grep -Ev '^(Address|MTU|Table) ' "$CENGARDE_ROOT/etc/wireguard/$2.conf"
exit 0
EOF
cat >"$BIN/systemd-sysusers" <<'EOF'
#!/bin/sh
echo "systemd-sysusers $*" >>"$CALLS"
grep -q "^$CENGARDE_ENGINE_USER:" "$CENGARDE_ROOT/etc/passwd" 2>/dev/null ||
	echo "$CENGARDE_ENGINE_USER:x:0:0::/:/usr/sbin/nologin" >>"$CENGARDE_ROOT/etc/passwd"
EOF
for f in stty sysctl userdel systemd-run; do
	printf '#!/bin/sh\necho "%s $*" >>"$CALLS"\n' "$f" >"$BIN/$f"
done
# make: the build fails with FAKE_BUILD_FAIL set (the install does not).
cat >"$BIN/make" <<'EOF'
#!/bin/sh
echo "make $*" >>"$CALLS"
case " $* " in *" install "*) ;; *) [ -z "${FAKE_BUILD_FAIL:-}" ] || exit 2 ;; esac
exit 0
EOF
chmod +x "$BIN"/*

S1=$("$ENGINE" genkey) S2=$("$ENGINE" genkey) S3=$("$ENGINE" genkey) S4=$("$ENGINE" genkey)
key_of() { printf '%s\n' "$1" | "$ENGINE" keys | sed -n "s/^CG_LINK_KEY='\(.*\)'$/\1/p"; }
addr_of() { printf '%s\n' "$1" | "$ENGINE" keys | sed -n "s/^CG_TUNNEL_ADDR='\(.*\)'$/\1/p"; }
ula_of() { printf '%s\n' "$1" | "$ENGINE" keys | sed -n "s/^CG_TUNNEL_ULA='\(.*\)'$/\1/p"; }
K1=$(key_of "$S1") K2=$(key_of "$S2") K3=$(key_of "$S3")
A1=$(addr_of "$S1") A2=$(addr_of "$S2") A3=$(addr_of "$S3")

R=
# fresh: a new root, nat.conf with the test's user, the engine's socket.
fresh() {
	R=$TMP/root.$1
	rm -rf "$R" "$FAKE_STATE"
	mkdir -p "$R/etc/cengarde" "$R/run/cengarde" "$FAKE_STATE"
	printf 'ENGINE_USER=root\n%s' "${2:-}" >"$R/etc/cengarde/nat.conf"
	python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$R/run/cengarde/cengarde.sock"
	touch "$FAKE_STATE/cengarde" # the engine runs
	: >"$CALLS"
}
# setup ARGS...: cengarde-vps-setup in the test's root; output in OUT.
setup() {
	OUT=$(env CENGARDE_ROOT="$R" PATH="$BIN:$PATH" CENGARDE_CTL_TIMEOUT=2 sh "$SETUP" "$@" 2>&1)
	RC=$?
	printf '%s\n' "$OUT" | sed 's/^/    /'
	return "$RC"
}
# add NAME SECRET ARGS...: the secret on standard input, not a terminal.
add() {
	n=$1 s=$2
	shift 2
	setup add "$n" "$@" <<-EOF
		$s
	EOF
}
called() { grep -Eq -- "$1" "$CALLS"; }
said() { printf '%s\n' "$OUT" | grep -qF -- "$1"; }
val() { sed -n "s/^$2=//p" "$1"; }
conf() { cat "$R/etc/cengarde/cengarde.conf" 2>/dev/null; }
# section NAME: router NAME's [client] section of cengarde.conf, without it.
section() { awk -v s="[client $1]" '/^\[/ { on = $0 == s; next } on' "$R/etc/cengarde/cengarde.conf"; }
mode() { stat -c '%a %U' "$1"; }
fingerprint() { find "$R/etc" "$R/var/lib/cengarde" -type f ! -name '.*' -exec md5sum {} + 2>/dev/null | sort; }

say "add a router, with no secret file (a server of your own)"
fresh add
check "add router" add router "$S1"
C=$R/etc/cengarde/clients/router
check "clients/router: 0600 root, in a 0700 directory" [ "$(mode "$C") $(mode "${C%/*}")" = "600 root 700 root" ]
check "it holds the secret, slot 0, the tunnel address of its secret ($A1), forwarding on" \
	[ "$(val "$C" SECRET) $(val "$C" SLOT) $(val "$C" TUNNEL_ADDR) $(val "$C" LEGACY_ADDR) $(val "$C" FORWARD)" = "$S1 0 $A1  yes" ]
W=$R/etc/wireguard/cg-router.conf
check "cg-router.conf: 0600, port 65501, 10.79.0.1/32, the router's /32, no wg-quick hooks" \
	sh -c '[ "$(stat -c %a "$1")" = 600 ] && grep -qx "ListenPort = 65501" "$1" && grep -qx "Address = 10.79.0.1/32" "$1" &&
		grep -qx "AllowedIPs = $2/32" "$1" && grep -qx "Table = auto" "$1" && ! grep -q "^Post" "$1"' sh "$W" "$A1"
check "cengarde.conf: 0640, the forward table and the reserved set, no global key, no IP pass file" \
	sh -c '[ "$(stat -c %a "$1")" = 640 ] && grep -qx "forward_file = /var/lib/cengarde/forward" "$1" &&
		grep -qx "# reserved: tcp:1-1023 tcp:8123 udp:1-1023 udp:65500-65532" "$1" &&
		[ "$(sed "/^\[/q" "$1" | grep -c "^key = ")" = 0 ] && ! grep -q "^passthrough_file" "$1"' sh "$R/etc/cengarde/cengarde.conf"
check "[client router]: its key, its WireGuard, the poke to its tunnel address" \
	sh -c '[ "$(printf "%s\n" "$1" | grep -v "^#" | grep .)" = "key = $2
wireguard = 127.0.0.1:65501
wireguard_poke = $3:9" ]' sh "$(section router)" "$K1" "$A1"
check "the engine checks it, and is asked to reload" sh -c 'grep -q "^cengarde -t -c .*cengarde.conf.new" "$1" && grep -q "^cengarde ctl -s .* reload" "$1"' sh "$CALLS"
check "wg-quick@cg-router enabled and started" called '^systemctl enable --now wg-quick@cg-router$'
check "cengarde-nat apply" called '^cengarde-nat apply$'
check "the engine's answer printed" said "engine: ok"
check "the secret is in no command line" not grep -qF "$S1" "$CALLS"
check "the secret is in clients/router and nowhere else" \
	[ "$(grep -rlF "$S1" "$R" 2>/dev/null)" = "$C" ]

say "add again: nothing changes"
before=$(fingerprint)
: >"$CALLS"
check "add router again" add router "$S1"
check "the files are the same" [ "$(fingerprint)" = "$before" ]
check "no reload, no WireGuard restart" not called 'reload|restart|enable --now|syncconf'

say "the router's secret changes"
before=$(fingerprint)
check "another secret under the same name is refused without --replace" not add router "$S2"
check "it says how" said "--replace"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
: >"$CALLS"
check "add router --replace" env FAKE_CTL=restart sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add router --replace' \
	sh "$S2" "$R" "$BIN:$PATH" "$SETUP"
check "the new key is in cengarde.conf" sh -c 'printf "%s\n" "$1" | grep -qx "key = $2"' sh "$(section router)" "$K2"
check "the tunnel address of the new secret ($A2), in its WireGuard" \
	sh -c '[ "$(sed -n "s/^TUNNEL_ADDR=//p" "$1")" = "$3" ] && grep -qx "AllowedIPs = $3/32" "$2"' sh "$C" "$W" "$A2"
check "WireGuard restarted (a new peer)" called '^systemctl restart wg-quick@cg-router$'

say "the same secret under another name: a rename"
check "refused without --rename" not add home "$S2"
check "it says how" said "--rename router"
: >"$CALLS"
check "add home --rename router" add home "$S2" --rename router --label "Casa de Ana"
check "clients/home, no clients/router" [ -e "$R/etc/cengarde/clients/home" ] && [ ! -e "$C" ]
check "slot, address and label kept or set" \
	[ "$(val "$R/etc/cengarde/clients/home" SLOT) $(val "$R/etc/cengarde/clients/home" TUNNEL_ADDR) $(val "$R/etc/cengarde/clients/home" LABEL)" = "0 $A2 Casa de Ana" ]
check "[client home] with its label, no [client router]" \
	sh -c 'printf "%s\n" "$1" | grep -qx "label = Casa de Ana" && ! grep -q "^\[client router\]" "$2"' sh "$(section home)" "$R/etc/cengarde/cengarde.conf"
check "wg-quick@cg-router stopped and its file removed, cg-home up" \
	sh -c 'grep -q "^systemctl disable --now wg-quick@cg-router$" "$1" && grep -q "^systemctl enable --now wg-quick@cg-home$" "$1" &&
		[ ! -e "$2/etc/wireguard/cg-router.conf" ] && [ -e "$2/etc/wireguard/cg-home.conf" ]' sh "$CALLS" "$R"

say "a second router"
: >"$CALLS"
check "add office" add office "$S3"
O=$R/etc/cengarde/clients/office
check "slot 1, its own address ($A3), port 65502" \
	sh -c '[ "$(sed -n "s/^SLOT=//p; s/^TUNNEL_ADDR=//p" "$1" | tr "\n" " ")" = "1 $3 " ] &&
		grep -qx "ListenPort = 65502" "$2/etc/wireguard/cg-office.conf"' sh "$O" "$R" "$A3"
check "cengarde.conf: both sections, office's with its key, port and poke" \
	sh -c 'printf "%s\n" "$1" | grep -qx "key = $2" && printf "%s\n" "$1" | grep -qx "wireguard = 127.0.0.1:65502" &&
		printf "%s\n" "$1" | grep -qx "wireguard_poke = $3:9" && grep -q "^\[client home\]" "$4"' \
	sh "$(section office)" "$K3" "$A3" "$R/etc/cengarde/cengarde.conf"
check "a reload, cg-home left alone" sh -c 'grep -q "^cengarde ctl -s .* reload" "$1" && ! grep -q "wg-quick@cg-home" "$1"' sh "$CALLS"

say "limits of the routers and of the ports"
before=$(fingerprint)
check "a tunnel address taken is refused" not add shop "$S4" --tunnel-addr "$A2"
check "it says whose" said "router home has the tunnel address $A2"
check "--proto 12 (a newer router) is refused with the upgrade command" not add shop "$S4" --proto 12
check "it prints the upgrade command" said "contrib/vps/install.sh"
check "an engine of protocol 3 is refused" not env FAKE_PROTO=3 sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add shop' \
	sh "$S4" "$R" "$BIN:$PATH" "$SETUP"
check "--tunnel-addr 10.79.0.1 is refused" not add home "$S2" --tunnel-addr 10.79.0.1
check "a name too long for cg-NAME is refused" not add abcdefghijklm "$S4"
check "\"default\" is refused" not add default "$S4"
check "a bad secret is refused" not add home "not-a-secret"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
for i in $(seq 2 31); do
	printf 'SECRET=x\nSLOT=%s\nTUNNEL_ADDR=10.79.1.%s\n' "$i" "$i" >"$R/etc/cengarde/clients/r$i"
	chmod 0600 "$R/etc/cengarde/clients/r$i"
done
check "the 33rd router is refused" not add shop "$S4"
check "it says why" said "this server has 32 routers already"
rm -f "$R"/etc/cengarde/clients/r[0-9]*
check "nothing changed" [ "$(fingerprint)" = "$before" ]
printf 'ENGINE_USER=root\nWG_PORT_BASE=65520\n' >"$R/etc/cengarde/nat.conf"
check "WireGuard ports past 65535 are refused" not add home "$S2"
printf 'ENGINE_USER=root\nCENGARDE_PORT=65510\n' >"$R/etc/cengarde/nat.conf"
check "cengarde's port inside the WireGuard ports is refused" not add home "$S2"
printf 'ENGINE_USER=root\n' >"$R/etc/cengarde/nat.conf"

say "list and forward"
: >"$CALLS"
check "list" setup list
check "list shows each router, its port, address, label and last heard" \
	sh -c 'printf "%s\n" "$1" | grep -Eq "^home +0 +65501 +$2 +yes +0\.4 s ago, 1 session +Casa de Ana$" &&
		printf "%s\n" "$1" | grep -Eq "^office +1 +65502 +$3 +yes +9\.0 s ago, 2 sessions *$"' sh "$OUT" "$A2" "$A3"
check "the engine asked once for both" [ "$(grep -c "^cengarde ctl .*links$" "$CALLS")" = 1 ]
check "forward" setup forward
check "forward shows the reserved ports with their reasons" said "something listening here: hass"
check "and the router's forwarding" said "home: yes"

say "the engine refuses, fails or hangs: the previous files back"
before=$(fingerprint)
check "a configuration the engine refuses: exit 1" not env FAKE_T_FAIL=1 FAKE_RESERVED="tcp:1-1023 udp:9999" sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --label X' \
	sh "$S2" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
check "a reload answered with an error: exit 1" not env FAKE_CTL=error FAKE_RESERVED="tcp:1-1023 udp:9999" sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --label X' \
	sh "$S2" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
t0=$(date +%s)
check "a reload that hangs, cut by the timeout: exit 1" not env FAKE_CTL=hang CENGARDE_CTL_TIMEOUT=2 FAKE_RESERVED="tcp:1-1023 udp:9999" sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --label X' \
	sh "$S2" "$R" "$BIN:$PATH" "$SETUP"
check "within a few seconds" [ $(($(date +%s) - t0)) -lt 8 ]
check "nothing changed" [ "$(fingerprint)" = "$before" ]
before=$(fingerprint)
check "--replace with a new secret, a reload answered with an error: exit 1" not env FAKE_CTL=error sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --replace' \
	sh "$S4" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
check "--replace, a configuration the engine refuses: exit 1" not env FAKE_T_FAIL=1 sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --replace' \
	sh "$S4" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed" [ "$(fingerprint)" = "$before" ]

say "forward add|del: a router's own rules"
H=$R/etc/cengarde/clients/home
check "forward add home tcp:8080=80 udp:5000-5010" setup forward add home tcp:8080=80 udp:5000-5010
check "in its file and in its section" \
	sh -c '[ "$(sed -n "s/^RULES=//p" "$1")" = "tcp:8080=80 udp:5000-5010" ] &&
		printf "%s\n" "$2" | grep -qx "forward = tcp:8080=80 udp:5000-5010"' sh "$H" "$(section home)"
check "and cengarde-nat apply" called '^cengarde-nat apply$'
before=$(fingerprint)
check "the same rules spelled otherwise: unchanged" setup forward add home tcp:08080=080 udp:5000-5010=5000
check "it says so" said "router home's rules unchanged"
check "the same files" [ "$(fingerprint)" = "$before" ]
check "forward add home both:7000-7001=7000" setup forward add home both:7000-7001=7000
check "kept as both:7000-7001" [ "$(val "$H" RULES)" = "tcp:8080=80 udp:5000-5010 both:7000-7001" ]
before=$(fingerprint)
check "a port this server listens on is refused" not setup forward add home tcp:8123
check "with the reason" said "tcp:8123: tcp 8123 is reserved on this server, something listening here: hass"
check "below 1024 is refused" not setup forward add home tcp:443
check "with the way to free it" said "below 1024 (cengarde-vps-setup forward allow tcp:443 frees it)"
check "a port of another router's rule is refused" not setup forward add office udp:5005
check "it says whose" said "udp:5005: overlaps udp:5000-5010, a rule of router home"
check "a port of its own rule is refused (both is tcp too)" not setup forward add home tcp:7001
check "it says so" said "tcp:7001: overlaps both:7000-7001, a rule of this router"
for r in tcp:0 tcp:70000 tcp:9-8 tcp:65000-65535=65500 sctp:80 tcp:80= tcp; do
	check "$r is refused" not setup forward add home "$r"
done
check "forward add nobody is refused" not setup forward add nobody tcp:9000
check "nothing changed" [ "$(fingerprint)" = "$before" ]
check "forward limit office 10" setup forward limit office 10
check "11 ports past it are refused" not setup forward add office tcp:20000-20010
check "it says why" said "router office would forward 11 ports with its own rules, past its limit of 10"
check "10 ports fit" setup forward add office tcp:20000-20009
check "a limit below its rules is refused" not setup forward limit office 5
check "it says why" said "router office's rules take 10 ports already"
check "forward limit office default" setup forward limit office default
check "forward shows the rules and the ports they take" setup forward
check "home's, with the default limit" said "its rules: tcp:8080=80 udp:5000-5010 both:7000-7001 (16 of 1024 ports)"
check "office's" said "its rules: tcp:20000-20009 (10 of 1024 ports)"
check "forward del home udp:5000-5010" setup forward del home udp:5000-5010
check "gone from its file and its section" \
	sh -c '[ "$(sed -n "s/^RULES=//p" "$1")" = "tcp:8080=80 both:7000-7001" ] &&
		printf "%s\n" "$2" | grep -qx "forward = tcp:8080=80 both:7000-7001"' sh "$H" "$(section home)"
check "a rule it does not have is refused" not setup forward del home tcp:9999
check "forward del office all" setup forward del office all
check "office has no rules" sh -c '[ -z "$(sed -n "s/^RULES=//p" "$1")" ] && ! printf "%s\n" "$2" | grep -q "^forward"' sh "$O" "$(section office)"
check "forward off home" setup forward off home
check "its section: no IP pass, its rules commented out" \
	sh -c 'printf "%s\n" "$1" | grep -qx "passthrough = no" && printf "%s\n" "$1" | grep -qx "#forward = tcp:8080=80 both:7000-7001"' sh "$(section home)"
check "forward on home" setup forward on home
check "its rules back" sh -c 'printf "%s\n" "$1" | grep -qx "forward = tcp:8080=80 both:7000-7001" && ! printf "%s\n" "$1" | grep -q "^passthrough"' sh "$(section home)"

say "forward off|on|limit|allow|disallow|reserve|unreserve"
N=$R/etc/cengarde/nat.conf
check "forward off home" setup forward off home
check "FORWARD=no, and cengarde-nat apply" sh -c '[ "$(sed -n "s/^FORWARD=//p" "$1")" = no ] && grep -q "^cengarde-nat apply" "$2"' sh "$H" "$CALLS"
check "forward on home" setup forward on home
check "FORWARD=yes" [ "$(val "$H" FORWARD)" = yes ]
check "forward limit home 2048" setup forward limit home 2048
check "FORWARD_MAX_PORTS=2048" [ "$(val "$H" FORWARD_MAX_PORTS)" = 2048 ]
check "forward limit home default" setup forward limit home default
check "FORWARD_MAX_PORTS empty" [ -z "$(val "$H" FORWARD_MAX_PORTS)" ]
check "forward allow tcp:80 tcp:443" setup forward allow tcp:80 tcp:443
check "FORWARD_ALLOW_LOW=\"tcp:80 tcp:443\"" grep -qx 'FORWARD_ALLOW_LOW="tcp:80 tcp:443"' "$N"
check "it says how a router gets one (outside PASSTHROUGH_PORTS)" \
	said 'tcp:443 is allowed: a router gets it with "cengarde-vps-setup forward add NAME tcp:443"'
check "and with it out of the reserved set, forward add home tcp:443 works" \
	env FAKE_RESERVED="tcp:1-79 tcp:81-442 tcp:444-1023" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" forward add home tcp:443' \
	sh "$R" "$BIN:$PATH" "$SETUP"
check "forward del home tcp:443" setup forward del home tcp:443
check "forward allow tcp:80 again: once" setup forward allow tcp:80
check "still once" grep -qx 'FORWARD_ALLOW_LOW="tcp:80 tcp:443"' "$N"
printf 'PASSTHROUGH_PORTS=1:65000\n' >>"$N"
check "with PASSTHROUGH_PORTS=1:65000, forward allow tcp:80" setup forward allow tcp:80
check "it says IP pass forwards it" said "tcp:80 is allowed, inside PASSTHROUGH_PORTS (1:65000): IP pass forwards it to its router"
check "and not that it needs a rule" not said "a router gets it with"
check "forward, with that range" setup forward
check "it warns that the ports below 1024 that 0.4 forwarded are reserved now" \
	said "PASSTHROUGH_PORTS (1:65000) starts below 1024: cengarde 0.4 forwarded ports 1-1023 too"
sed -i '/^PASSTHROUGH_PORTS=/d' "$N"
check "forward disallow tcp:80" setup forward disallow tcp:80
check "FORWARD_ALLOW_LOW=\"tcp:443\"" grep -qx 'FORWARD_ALLOW_LOW="tcp:443"' "$N"
check "forward allow tcp:2000 is refused (not below 1024)" not setup forward allow tcp:2000
check "forward reserve udp:5000-5010" setup forward reserve udp:5000-5010
check "FORWARD_RESERVED=\"udp:5000-5010\"" grep -qx 'FORWARD_RESERVED="udp:5000-5010"' "$N"
check "forward unreserve udp:5000-5010" setup forward unreserve udp:5000-5010
check "moved to FORWARD_UNRESERVE" sh -c 'grep -qx "FORWARD_RESERVED=\"\"" "$1" && grep -qx "FORWARD_UNRESERVE=\"udp:5000-5010\"" "$1"' sh "$N"
check "forward unreserve tcp:22 is refused (SSH)" not setup forward unreserve tcp:22
check "forward unreserve tcp:80 is refused (below 1024: allow)" not setup forward unreserve tcp:80
check "forward allow bogus is refused" not setup forward allow tcp80
check "nat.conf stays 0644 root" [ "$(mode "$N")" = "644 root" ]
check "forward off nobody is refused" not setup forward off nobody

say "remove"
: >"$CALLS"
check "remove home" setup remove home
check "its files gone, its WireGuard stopped" \
	sh -c '[ ! -e "$1/etc/cengarde/clients/home" ] && [ ! -e "$1/etc/wireguard/cg-home.conf" ] && grep -q "^systemctl disable --now wg-quick@cg-home$" "$2"' sh "$R" "$CALLS"
check "office stays, home's section gone" \
	sh -c 'grep -q "^\[client office\]" "$1" && ! grep -q "^\[client home\]" "$1"' sh "$R/etc/cengarde/cengarde.conf"
check "remove nobody is refused" not setup remove nobody
check "remove office, the last one" setup remove office
check "the engine gets a key that serves nobody, and no forward table" \
	sh -c 'grep -q "^# No router yet" "$1" && ! grep -q "^\[client\|^forward_file\|key = $2" "$1"' sh "$R/etc/cengarde/cengarde.conf" "$K3"

say "the one-shot import of /etc/cengarde/secret (the cloud-config)"
fresh import
SF=$R/etc/cengarde/secret
(umask 077 && echo "$S1" >"$SF")
check "no arguments" setup
C=$R/etc/cengarde/clients/router
check "imported as router: slot 0, 10.79.0.2, IMPORTED=1" \
	[ "$(val "$C" SECRET) $(val "$C" SLOT) $(val "$C" TUNNEL_ADDR) $(val "$C" IMPORTED)" = "$S1 0 10.79.0.2 1" ]
check "the secret file deleted" [ ! -e "$SF" ]
check "wg-quick@cg-router up" called '^systemctl enable --now wg-quick@cg-router$'
before=$(fingerprint)
check "no arguments again: nothing changes" setup
check "the same files" [ "$(fingerprint)" = "$before" ]
(umask 077 && echo "$S1" >"$SF")
check "with the file back, holding a configured router's secret" setup
check "it is just deleted" [ ! -e "$SF" ]
check "and clients/ is the same" [ "$(fingerprint)" = "$before" ]
say "  remove, then run again"
(umask 077 && echo "$S1" >"$SF")
check "remove router" setup remove router
check "remove also deletes the imported router's leftover secret file" [ ! -e "$SF" ]
check "no arguments" setup
check "clients/ stays empty" [ -z "$(ls "$R/etc/cengarde/clients")" ]
say "  --replace, then run again"
(umask 077 && echo "$S1" >"$SF")
check "no arguments: imported" setup
check "add router --replace (another secret)" add router "$S2" --replace
(umask 077 && echo "$S1" >"$SF")
before=$(fingerprint)
check "no arguments" setup
check "the old secret is not imported again" [ "$(val "$C" SECRET)" = "$S2" ]
check "it says why" said "ignoring $SF"
check "clients/ unchanged" [ "$(fingerprint)" = "$before" ]
say "  a cloud-config of 0.5.2: ROUTER_NAME and ROUTER_TUNNEL_ADDR"
fresh import52 'ROUTER_NAME=casa
ROUTER_TUNNEL_ADDR=10.79.5.6
'
SF=$R/etc/cengarde/secret
(umask 077 && echo "$S1" >"$SF")
check "no arguments" setup
check "imported as casa, at 10.79.5.6" \
	[ "$(val "$R/etc/cengarde/clients/casa" TUNNEL_ADDR) $(val "$R/etc/cengarde/clients/casa" LEGACY_ADDR)" = "10.79.5.6 " ]
fresh importrepo 'ROUTER_NAME=REPLACE_ROUTER_NAME
ROUTER_TUNNEL_ADDR=REPLACE_TUNNEL_ADDR
'
SF=$R/etc/cengarde/secret
(umask 077 && echo "$S1" >"$SF")
check "as the repository has them" setup
check "imported as router, at its secret's address ($A1)" [ "$(val "$R/etc/cengarde/clients/router" TUNNEL_ADDR)" = "$A1" ]
check "without a warning" not said "ROUTER_"
fresh importbad 'ROUTER_TUNNEL_ADDR=10.80.0.2
'
SF=$R/etc/cengarde/secret
(umask 077 && echo "$S1" >"$SF")
check "ROUTER_TUNNEL_ADDR out of 10.79.0.0/16" setup
check "imported at its secret's address ($A1)" [ "$(val "$R/etc/cengarde/clients/router" TUNNEL_ADDR)" = "$A1" ]
check "with a warning" said "ROUTER_TUNNEL_ADDR in"
say "  slot 0 taken by a router added by hand"
fresh slot0
SF=$R/etc/cengarde/secret
check "add office" add office "$S3"
(umask 077 && echo "$S1" >"$SF")
before=$(fingerprint)
check "no arguments" setup
check "clients/ unchanged: office only" [ "$(ls "$R/etc/cengarde/clients")" = office ]
check "the same files" [ "$(fingerprint)" = "$before" ]
say "  a router named passthrough, as the IP pass file is, replaced: the reload fails"
fresh pname
check "add passthrough" add passthrough "$S1"
mkdir -p "$R/var/lib/cengarde"
echo off >"$R/var/lib/cengarde/passthrough"
before=$(fingerprint)
check "add passthrough --replace, a reload answered with an error: exit 1" not env FAKE_CTL=error sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add passthrough --replace' \
	sh "$S2" "$R" "$BIN:$PATH" "$SETUP"
check "both files back as they were" [ "$(fingerprint)" = "$before" ]

say "on a terminal"
fresh tty
: >"$CALLS"
printf '%s\n' "$S1" >"$TMP/typed"
script -qec "env CENGARDE_ROOT='$R' PATH='$BIN:$PATH' sh '$SETUP' add router" /dev/null <"$TMP/typed" >"$TMP/tty.out" 2>&1
check "add on a terminal" [ -e "$R/etc/cengarde/clients/router" ]
check "echo turned off for the secret, then back on" \
	sh -c 'grep "^stty" "$1" | tr "\n" " " | grep -q "^stty -echo stty echo"' sh "$CALLS"
check "the prompt says it is not echoed" grep -q "not echoed" "$TMP/tty.out"

say "a router whose name sorts before the others'"
fresh order
check "add zeta" add zeta "$S1"
: >"$CALLS"
check "add alpha" add alpha "$S2"
check "cg-alpha.conf: alpha's port and address, cg-zeta left alone" \
	sh -c 'grep -qx "ListenPort = 65502" "$1/etc/wireguard/cg-alpha.conf" && grep -qx "AllowedIPs = $2/32" "$1/etc/wireguard/cg-alpha.conf" &&
		grep -qx "AllowedIPs = $3/32" "$1/etc/wireguard/cg-zeta.conf" && ! grep -q "wg-quick@cg-zeta" "$4"' sh "$R" "$A2" "$A1" "$CALLS"
check "it says alpha's address" said "router alpha: tunnel address $A2"

say "ipv6 on|off: the routers' IPv6 through the tunnels"
fresh ipv6
U1=$(ula_of "$S1") U2=$(ula_of "$S2")
check "add zeta" add zeta "$S1"
check "add alpha" add alpha "$S2"
before=$(fingerprint)
check "ipv6 on, on a server without IPv6, is refused" not setup ipv6 on
check "it says why" said "this server has no IPv6"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
check "list says it is off" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" list 2>&1 | grep -q "IPv6 through the tunnels: off"' sh "$R" "$BIN:$PATH" "$SETUP"
mkdir -p "$R/proc/net"
: >"$R/proc/net/if_inet6"
: >"$CALLS"
check "ipv6 on" setup ipv6 on
check "TUNNEL_IPV6=\"yes\" in nat.conf" grep -qx 'TUNNEL_IPV6="yes"' "$R/etc/cengarde/nat.conf"
check "each router's WireGuard: its ULA, ::1 here and ::2 its peer" \
	sh -c 'grep -qx "Address = 10.79.0.1/32, $2::1/128" "$1/cg-zeta.conf" && grep -qx "AllowedIPs = $4/32, $2::2/128" "$1/cg-zeta.conf" &&
		grep -qx "Address = 10.79.0.1/32, $3::1/128" "$1/cg-alpha.conf" && grep -qx "AllowedIPs = $5/32, $3::2/128" "$1/cg-alpha.conf"' \
	sh "$R/etc/wireguard" "$U1" "$U2" "$A1" "$A2"
check "both restarted, and cengarde-nat applies it" \
	sh -c 'grep -q "^systemctl restart wg-quick@cg-zeta$" "$1" && grep -q "^systemctl restart wg-quick@cg-alpha$" "$1" && grep -q "^cengarde-nat apply$" "$1"' sh "$CALLS"
check "list says it is on" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" list 2>&1 | grep -q "IPv6 through the tunnels: on"' sh "$R" "$BIN:$PATH" "$SETUP"
check "a router added now gets its ULA too" add omega "$S3"
check "omega's WireGuard has it" grep -qx "AllowedIPs = $A3/32, $(ula_of "$S3")::2/128" "$R/etc/wireguard/cg-omega.conf"
check "ipv6 off" setup ipv6 off
check "TUNNEL_IPV6=\"no\"" grep -qx 'TUNNEL_IPV6="no"' "$R/etc/cengarde/nat.conf"
check "no ULA left in the routers' WireGuard" not grep -q "::" "$R/etc/wireguard/cg-zeta.conf" "$R/etc/wireguard/cg-alpha.conf" "$R/etc/wireguard/cg-omega.conf"
check "ipv6 maybe is refused" not setup ipv6 maybe
say "  the import of a cloud-config with TUNNEL_IPV6=yes"
fresh import6 'TUNNEL_IPV6=yes
ROUTER_TUNNEL_ADDR=REPLACE_TUNNEL_ADDR
'
mkdir -p "$R/proc/net"
: >"$R/proc/net/if_inet6"
(umask 077 && echo "$S1" >"$R/etc/cengarde/secret")
check "no arguments" setup
check "router's WireGuard with its ULA" grep -qx "Address = 10.79.0.1/32, $U1::1/128" "$R/etc/wireguard/cg-router.conf"

say "tunnel native|wireguard: the engine's TUN instead of each router's WireGuard"
fresh native
check "add zeta" add zeta "$S1"
check "add alpha" add alpha "$S2"
wg_before=$(cat "$R/etc/wireguard/cg-zeta.conf" "$R/etc/wireguard/cg-alpha.conf")
conf_before=$(conf)
DROPIN=$R/etc/systemd/system/cengarde.service.d/native.conf
check "no drop-in in WireGuard mode" [ ! -e "$DROPIN" ]
: >"$CALLS"
check "tunnel native" setup tunnel native
check "TUNNEL=\"native\" in nat.conf" grep -qx 'TUNNEL="native"' "$R/etc/cengarde/nat.conf"
check "cengarde.conf: tunnel = native on cg0 at 10.79.0.1/16, no WireGuard port file" \
	sh -c 'grep -qx "tunnel = native" "$1" && grep -qx "tun = cg0" "$1" && grep -qx "tun_addr = 10.79.0.1/16" "$1" &&
		! grep -q "^wireguard" "$1"' sh "$R/etc/cengarde/cengarde.conf"
check "[client zeta]: its key and its address as its tun_route, no WireGuard" \
	sh -c '[ "$(printf "%s\n" "$1" | grep -v "^#" | grep .)" = "key = $2
tun_route = $3/32" ]' sh "$(section zeta)" "$K1" "$A1"
check "[client alpha] too" sh -c 'printf "%s\n" "$1" | grep -qx "tun_route = $2/32"' sh "$(section alpha)" "$A2"
check "the engine checks it" called '^cengarde -t -c .*cengarde.conf.new'
check "the drop-in lets the engine open /dev/net/tun, and nothing else" \
	sh -c 'grep -qx "PrivateDevices=no" "$1" && grep -qx "DevicePolicy=closed" "$1" && grep -qx "DeviceAllow=/dev/net/tun rw" "$1"' sh "$DROPIN"
check "systemd reloaded, the engine restarted rather than reloaded" \
	sh -c 'grep -q "^systemctl daemon-reload$" "$1" && grep -q "^systemctl restart cengarde$" "$1" && ! grep -q "^cengarde ctl .*reload$" "$1"' sh "$CALLS"
check "it says every router saw a cut" said "every router saw a cut"
check "each router's WireGuard stopped and its file removed, after the engine" \
	sh -c 'grep -q "^systemctl disable --now wg-quick@cg-zeta$" "$1" && grep -q "^systemctl disable --now wg-quick@cg-alpha$" "$1" &&
		[ ! -e "$2/etc/wireguard/cg-zeta.conf" ] && [ ! -e "$2/etc/wireguard/cg-alpha.conf" ] &&
		[ "$(grep -n -e "^systemctl restart cengarde$" -e "wg-quick@cg-zeta$" "$1" | head -n 1 | cut -d: -f2-)" = "systemctl restart cengarde" ]' sh "$CALLS" "$R"
check "cengarde-nat applies it" called '^cengarde-nat apply$'
check "list says so" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" list 2>&1 | grep -q "tunnels: native"' sh "$R" "$BIN:$PATH" "$SETUP"
before=$(fingerprint)
: >"$CALLS"
check "tunnel native again" setup tunnel native
check "it says so, nothing changed, no restart" sh -c 'printf "%s\n" "$1" | grep -q "already" && ! grep -q "restart\|reload" "$2"' sh "$OUT" "$CALLS"
check "the files are the same" [ "$(fingerprint)" = "$before" ]
: >"$CALLS"
check "a router added now" add omega "$S3"
check "omega: its tun_route, no WireGuard, a reload (no restart)" \
	sh -c 'printf "%s\n" "$1" | grep -qx "tun_route = $2/32" && [ ! -e "$3/etc/wireguard/cg-omega.conf" ] &&
		! grep -q "wg-quick@cg-omega\|restart cengarde" "$4" && grep -q "^cengarde ctl .*reload$" "$4"' sh "$(section omega)" "$A3" "$R" "$CALLS"
check "it says the router needs native too" said "Tunnel mode: native"
mkdir -p "$R/proc/net"
: >"$R/proc/net/if_inet6"
check "ipv6 on, natively" setup ipv6 on
check "the TUN gets fd79::1/8 (every router's ULA inside), each router its ULA ::2" \
	sh -c 'grep -qx "tun_addr = 10.79.0.1/16 fd79::1/8" "$1" && printf "%s\n" "$2" | grep -qx "tun_route = $3/32 $4::2/128"' \
	sh "$R/etc/cengarde/cengarde.conf" "$(section zeta)" "$A1" "$(ula_of "$S1")"
check "still no WireGuard" [ ! -e "$R/etc/wireguard/cg-zeta.conf" ]
check "ipv6 off" setup ipv6 off
rm -rf "$R/proc"
: >"$CALLS"
check "protocontroller on, natively" setup protocontroller on
check "cengarde.conf: protocontroller = b; PROTOCONTROLLER in nat.conf; tcp_bbr at boot" \
	sh -c 'grep -qx "protocontroller = b" "$1" && grep -qx "PROTOCONTROLLER=\"yes\"" "$2" && grep -qx tcp_bbr "$3"' \
	sh "$R/etc/cengarde/cengarde.conf" "$R/etc/cengarde/nat.conf" "$R/etc/modules-load.d/cengarde-bbr.conf"
check "the engine takes it" called '^cengarde -t -c .*cengarde.conf.new'
check "protocontroller off" setup protocontroller off
check "no protocontroller in cengarde.conf" not grep -q "^protocontroller" "$R/etc/cengarde/cengarde.conf"
: >"$CALLS"
check "fec on" setup fec on
check "cengarde.conf: fec = yes; FEC in nat.conf" \
	sh -c 'grep -qx "fec = yes" "$1" && grep -qx "FEC=\"yes\"" "$2"' sh "$R/etc/cengarde/cengarde.conf" "$R/etc/cengarde/nat.conf"
check "the engine takes it" called '^cengarde -t -c .*cengarde.conf.new'
check "fec on again changes nothing" setup fec on
check "fec rlc" setup fec rlc
check "cengarde.conf: fec = yes and fec_code = rlc; FEC=rlc in nat.conf" \
	sh -c 'grep -qx "fec = yes" "$1" && grep -qx "fec_code = rlc" "$1" && grep -qx "FEC=\"rlc\"" "$2"' \
	sh "$R/etc/cengarde/cengarde.conf" "$R/etc/cengarde/nat.conf"
check "fec off" setup fec off
check "no fec in cengarde.conf" not grep -q "^fec" "$R/etc/cengarde/cengarde.conf"
check "fec maybe is refused" not setup fec maybe
check "remove omega" setup remove omega
: >"$CALLS"
check "tunnel wireguard" setup tunnel wireguard
check "TUNNEL=\"wireguard\"" grep -qx 'TUNNEL="wireguard"' "$R/etc/cengarde/nat.conf"
check "the drop-in gone, systemd reloaded, the engine restarted" \
	sh -c '[ ! -e "$1" ] && grep -q "^systemctl daemon-reload$" "$2" && grep -q "^systemctl restart cengarde$" "$2"' sh "$DROPIN" "$CALLS"
check "each router's WireGuard back as it was" \
	[ "$(cat "$R/etc/wireguard/cg-zeta.conf" "$R/etc/wireguard/cg-alpha.conf" 2>/dev/null)" = "$wg_before" ]
check "and started" sh -c 'grep -q "^systemctl enable --now wg-quick@cg-zeta$" "$1" && grep -q "^systemctl enable --now wg-quick@cg-alpha$" "$1"' sh "$CALLS"
check "cengarde.conf as before native" [ "$(conf)" = "$conf_before" ]
check "tunnel gre is refused" not setup tunnel gre
check "protocontroller on is refused without the native tunnel" not setup protocontroller on
check "a TUNNEL in nat.conf that is neither is refused" \
	not sh -c 'printf "TUNNEL=gre\n" >>"$1/etc/cengarde/nat.conf" && env CENGARDE_ROOT="$1" PATH="$2" sh "$3" list 2>/dev/null' sh "$R" "$BIN:$PATH" "$SETUP"
say "  the import of a cloud-config with TUNNEL=native"
fresh importn 'TUNNEL=native
ROUTER_TUNNEL_ADDR=REPLACE_TUNNEL_ADDR
'
(umask 077 && echo "$S1" >"$R/etc/cengarde/secret")
check "no arguments" setup
check "router through the TUN, at its secret's address, no WireGuard, the drop-in" \
	sh -c 'grep -qx "tunnel = native" "$1/etc/cengarde/cengarde.conf" && printf "%s\n" "$2" | grep -qx "tun_route = $3/32" &&
		[ ! -e "$1/etc/wireguard/cg-router.conf" ] && [ -e "$1/etc/systemd/system/cengarde.service.d/native.conf" ]' sh "$R" "$(section router)" "$A1"
check "no WireGuard started" not called 'wg-quick@'

say "a 0.4 server, migrated by cengarde-vps-setup"
fresh m04 'PASSTHROUGH=yes
'
mkdir -p "$R/etc/wireguard" "$R/var/lib/cengarde-nat"
printf '# Written by cengarde-vps-setup from /etc/cengarde/secret.\n[Interface]\nListenPort = 65501\n' >"$R/etc/wireguard/wg0.conf"
echo off >"$R/var/lib/cengarde-nat/passthrough"
(umask 077 && echo "$S1" >"$R/etc/cengarde/secret")
touch "$FAKE_STATE/wg-quick@wg0"
check "no arguments" setup
check "wg0 stopped and its file removed" sh -c 'grep -q "^systemctl disable --now wg-quick@wg0$" "$1" && [ ! -e "$2/etc/wireguard/wg0.conf" ]' sh "$CALLS" "$R"
check "wg0's PostDown, run by systemd, gets the admin lock at once" called '^stop wg-quick@wg0: took the admin lock$'
check "0.4's IP pass state gone, and nothing in its place: the router asks again" \
	[ ! -e "$R/var/lib/cengarde-nat" ] && [ ! -e "$R/var/lib/cengarde/passthrough" ]
check "it says so" said "the router asks for IP pass again"
check "the router imported with 10.79.0.2 (a cloud-config before 0.5.2) on port 65501" \
	sh -c '[ "$(sed -n "s/^TUNNEL_ADDR=//p" "$1/etc/cengarde/clients/router")" = 10.79.0.2 ] && grep -qx "ListenPort = 65501" "$1/etc/wireguard/cg-router.conf"' sh "$R"

say "purge, for cengarde 0.4"
# The engine's forward table: the router holds IP pass.
mkdir -p "$R/var/lib/cengarde"
printf '# cengarde forward table 1, written by the engine: data only\npass router\nrule router tcp 9000 9000 22\n' >"$R/var/lib/cengarde/forward"
mkdir -p "$R/etc/systemd/system/cengarde.service.d" "$R/etc/systemd/system/nftables.service.d"
for u in cengarde.service cengarde-nat.service cengarde-nat-check.timer cengarde-nat-check.service \
	cengarde-passthrough.path cengarde-passthrough.service cengarde.service.d/vps.conf nftables.service.d/cengarde.conf; do
	: >"$R/etc/systemd/system/$u"
done
: >"$CALLS"
touch "$FAKE_STATE/cengarde-nat.service"
check "purge" setup purge
check "the units gone" [ -z "$(find "$R/etc/systemd/system" -type f)" ]
check "every service disabled, cg-router too, and cengarde-nat down" \
	sh -c 'for u in wg-quick@cg-router cengarde-nat-check.timer cengarde-passthrough.path cengarde.service cengarde-nat.service; do
		grep -q "^systemctl disable --now $u$" "$1" || exit 1; done; grep -q "^cengarde-nat down$" "$1"' sh "$CALLS"
check "cengarde-nat.service's ExecStop, run by systemd, gets the admin lock at once" \
	called '^stop cengarde-nat.service: took the admin lock$'
check "the router's secret written back, 0600" sh -c '[ "$(cat "$1")" = "$2" ] && [ "$(stat -c %a "$1")" = 600 ]' sh "$R/etc/cengarde/secret" "$S1"
check "its IP pass written back for 0.4: on, as it holds the range" [ "$(cat "$R/var/lib/cengarde-nat/passthrough")" = on ]
check "clients/ and cg-router.conf kept" [ -e "$R/etc/cengarde/clients/router" ] && [ -e "$R/etc/wireguard/cg-router.conf" ]
check "purge --all" setup purge --all
check "clients/, cg-router.conf and the state gone" \
	sh -c '[ ! -e "$1/etc/cengarde/clients" ] && [ ! -e "$1/etc/wireguard/cg-router.conf" ] && [ ! -e "$1/var/lib/cengarde" ]' sh "$R"

say "a server of your own: no secret file, a private main IPv4"
fresh home
check "no arguments, no secret" env FAKE_MAIN="192.168.1.10 eth0" FAKE_NETS="192.168.1.0/24" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3"' \
	sh "$R" "$BIN:$PATH" "$SETUP" >"$TMP/home.out" 2>&1
check "it shows the add command" grep -q "cengarde-vps-setup add NAME" "$TMP/home.out"
check "the engine gets a key that serves nobody" grep -q "^# No router yet" "$R/etc/cengarde/cengarde.conf"
check "FORWARD_SKIP_SRC: the server's own network" grep -qx 'FORWARD_SKIP_SRC="192.168.1.0/24"' "$R/etc/cengarde/nat.conf"
check "what to open, for a home router or a cloud" grep -q "in your home router or in the provider's firewall (security list)" "$TMP/home.out"
before=$(fingerprint)
check "again: the same key that serves nobody, nothing changes" env FAKE_MAIN="192.168.1.10 eth0" FAKE_NETS="192.168.1.0/24" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" >/dev/null 2>&1' \
	sh "$R" "$BIN:$PATH" "$SETUP"
check "the same files" [ "$(fingerprint)" = "$before" ]
check "carrier-grade NAT: a warning" env FAKE_MAIN="100.72.1.2 eth0" sh -c 'env CENGARDE_ROOT="$1" PATH="$2" sh "$3" 2>&1 | grep -q "carrier-grade NAT"' \
	sh "$R" "$BIN:$PATH" "$SETUP"
check "a public main IPv4: nothing to open" env FAKE_MAIN="203.0.113.10 eth0" sh -c '! env CENGARDE_ROOT="$1" PATH="$2" sh "$3" 2>&1 | grep -q "is private"' \
	sh "$R" "$BIN:$PATH" "$SETUP"

say "install.sh on a 0.4 server"
# install.sh runs cengarde-vps-setup from PATH: this checkout's.
ln -s "$SETUP" "$BIN/cengarde-vps-setup"
# inst ARGS: install.sh in the test's root.
inst() {
	OUT=$(env CENGARDE_ROOT="$R" CENGARDE_ENGINE_USER=root PATH="$BIN:$PATH" sh "$INSTALL" 2>&1)
	RC=$?
	printf '%s\n' "$OUT" | sed 's/^/    /'
	return "$RC"
}
order() { # FIRST SECOND: FIRST logged before SECOND
	a=$(grep -nE -- "$1" "$CALLS" | head -n 1 | cut -d: -f1)
	b=$(grep -nE -- "$2" "$CALLS" | head -n 1 | cut -d: -f1)
	[ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]
}
fresh inst04 'CENGARDE_PORT=65500
PASSTHROUGH=yes
PASSTHROUGH_PORTS=1024:65000
'
mkdir -p "$R/etc/wireguard" "$R/var/lib/cengarde-nat"
printf '# Written by cengarde-vps-setup from /etc/cengarde/secret.\n[Interface]\nListenPort = 65501\n' >"$R/etc/wireguard/wg0.conf"
echo on >"$R/var/lib/cengarde-nat/passthrough"
(umask 077 && echo "$S1" >"$R/etc/cengarde/secret")
before=$(fingerprint)
check "with firewalld active, install.sh refuses" not env FAKE_FIREWALLD=1 sh -c 'env CENGARDE_ROOT="$1" CENGARDE_ENGINE_USER=root PATH="$2" sh "$3"' \
	sh "$R" "$BIN:$PATH" "$INSTALL"
check "before changing anything" sh -c '[ "$(cat "$CALLS")" = "systemctl is-active -q firewalld" ]'
check "the same files" [ "$(fingerprint)" = "$before" ]
: >"$CALLS"
check "a build that fails: install.sh fails" not env FAKE_BUILD_FAIL=1 sh -c 'env CENGARDE_ROOT="$1" CENGARDE_ENGINE_USER=root PATH="$2" sh "$3" >/dev/null 2>&1' \
	sh "$R" "$BIN:$PATH" "$INSTALL"
check "with 0.4's wg0 untouched, so its router keeps its tunnel" not called 'wg-quick@wg0'
check "the same files" [ "$(fingerprint)" = "$before" ]
: >"$CALLS"
check "install.sh" inst
check "the engine built before wg0 is stopped" order '^make -C [^ ]*/engine$' '^systemctl disable --now wg-quick@wg0$'
check "wg0 stopped before anything new is installed" order '^systemctl disable --now wg-quick@wg0$' '^make -C .*/engine install'
check "the old service stopped before systemd-sysusers (its dynamic user)" order '^systemctl stop cengarde$' '^systemd-sysusers'
check "the units in place, nftables' drop-in left out (not enabled)" \
	sh -c 'for u in cengarde.service cengarde.service.d/vps.conf cengarde-nat.service cengarde-nat-check.timer cengarde-nat-check.service cengarde-passthrough.path cengarde-passthrough.service; do
		[ -e "$1/etc/systemd/system/$u" ] || exit 1; done; [ ! -e "$1/etc/systemd/system/nftables.service.d/cengarde.conf" ]' sh "$R"
check "the sysusers file" [ -e "$R/etc/sysusers.d/cengarde.conf" ]
check "the unit runs as the static user, from the file itself" \
	sh -c 'grep -qx "User=cengarde" "$1" && grep -qx "ExecStart=/usr/local/sbin/cengarde -c /etc/cengarde/cengarde.conf" "$1" && ! grep -q "^DynamicUser\|^LoadCredential" "$1"' \
	sh "$R/etc/systemd/system/cengarde.service"
check "the cloud-config's nat.conf kept" grep -qx 'PASSTHROUGH=yes' "$R/etc/cengarde/nat.conf"
check "the router imported: cg-router on 65501, 10.79.0.2" \
	sh -c '[ "$(sed -n "s/^TUNNEL_ADDR=//p" "$1/etc/cengarde/clients/router")" = 10.79.0.2 ] && grep -qx "ListenPort = 65501" "$1/etc/wireguard/cg-router.conf"' sh "$R"
check "the secret file deleted, 0.4's IP pass state too" \
	sh -c '[ ! -e "$1/etc/cengarde/secret" ] && [ ! -e "$1/var/lib/cengarde-nat" ] && [ ! -e "$1/var/lib/cengarde/passthrough" ]' sh "$R"
check "cengarde-nat apply before cengarde-vps-setup's own" order '^cengarde-nat apply$' '^systemctl enable --now wg-quick@cg-router$'
check "the services enabled, cengarde restarted last" \
	sh -c 'grep -q "^systemctl enable cengarde-nat.service cengarde-nat-check.timer cengarde-passthrough.path cengarde.service$" "$1" &&
		[ "$(tail -n 1 "$1" | sed "s/^cengarde version$//")" = "" ] && grep -q "^systemctl restart cengarde$" "$1"' sh "$CALLS"
before=$(fingerprint)
: >"$CALLS"
check "install.sh again" inst
check "nothing migrated or stopped this time" not called 'wg-quick@wg0|^systemctl stop cengarde$'
check "the same files" [ "$(fingerprint)" = "$before" ]
touch "$FAKE_STATE/enabled-nftables"
check "with nftables.service enabled" inst
check "its drop-in installed" [ -e "$R/etc/systemd/system/nftables.service.d/cengarde.conf" ]
rm -f "$FAKE_STATE/enabled-nftables"
check "nftables.service no longer enabled" inst
check "its drop-in removed" [ ! -e "$R/etc/systemd/system/nftables.service.d/cengarde.conf" ]
say "the ports WireGuard knows the routers by, read from WireGuard until the engine writes them"
fresh wgports
check "add router, WireGuard with no endpoint for it" add router "$S1"
check "the engine's configuration names the port file" \
	grep -qx 'wireguard_port_file = /var/lib/cengarde/wg/ports' "$R/etc/cengarde/cengarde.conf"
check "no port file: nothing to seed it with" [ ! -e "$R/var/lib/cengarde/wg/ports" ]
export FAKE_WG_ENDPOINTS="router=34604 office=40000"
check "add office, WireGuard knowing router by 34604 (an engine older than 0.6.2)" add office "$S2"
F=$R/var/lib/cengarde/wg/ports
check "the file says so, in the engine's format" \
	sh -c 'head -n 1 "$1" | grep -q "^# cengarde WireGuard ports 1, " && grep -qx "34604 router" "$1" && grep -qx "40000 office" "$1" && [ "$(wc -l <"$1")" = 3 ]' sh "$F"
check "and it said so" sh -c 'printf "%s\n" "$1" | grep -q "the ports WireGuard knows the routers by: .*34604 router" && printf "%s\n" "$1" | grep -q "by: .*40000 office"' sh "$OUT"
check "in a directory of its own, the engine's, no leftovers" \
	sh -c '[ "$(stat -c %U "${1%/*}")" = root ] && [ "$(ls -A "${1%/*}")" = ports ]' sh "$F"
export FAKE_WG_ENDPOINTS="router=50000 office=50001"
check "add third" add third "$S3"
check "an existing file is the engine's: left alone" \
	sh -c 'grep -qx "34604 router" "$1" && ! grep -q 50000 "$1"' sh "$F"
unset FAKE_WG_ENDPOINTS

say "engine.local: the engine's global settings by hand, kept by every run"
fresh local
add router "$S1" >/dev/null
printf 'governor = yes\nsplit_spread_ms = 250\n' >"$R/etc/cengarde/engine.local"
check "refresh with an engine.local" setup refresh
check "its settings in the configuration, before the [client] sections" sh -c '
	awk "/^\\[client /{exit} {print}" "$1" | grep -qx "governor = yes" &&
	grep -qx "split_spread_ms = 250" "$1"' sh "$R/etc/cengarde/cengarde.conf"
check "a second refresh changes nothing" sh -c 'cp "$1" "$1.before" && env CENGARDE_ROOT="$2" PATH="$3" CENGARDE_CTL_TIMEOUT=2 sh "$4" refresh >/dev/null 2>&1 && cmp -s "$1" "$1.before"' \
	sh "$R/etc/cengarde/cengarde.conf" "$R" "$BIN:$PATH" "$SETUP"
printf 'governor = yes\n[client x]\nkey = y\n' >"$R/etc/cengarde/engine.local"
check "an engine.local with a [section] refused" not setup refresh
check "with the reason" said "no [section]"
check "and the configuration as it was" grep -qx "split_spread_ms = 250" "$R/etc/cengarde/cengarde.conf"
rm -f "$R/etc/cengarde/engine.local"
check "engine.local removed: refresh" setup refresh
check "its settings gone" not grep -q "^governor" "$R/etc/cengarde/cengarde.conf"

say "install.sh on a server whose running engine refuses the new configuration"
# 0.4.5 to 0.5.0: the static user is there, so the old engine keeps running
# until 6/6, and it refuses a configuration with [client] sections on a
# reload ("key: expected the base64 of 32 bytes"). install.sh leaves the
# configuration to the restart that follows.
fresh inst05
printf 'root:x:0:0::/root:/bin/sh\n' >"$R/etc/passwd"
(umask 077 && echo "$S1" >"$R/etc/cengarde/secret")
export FAKE_CTL=error
check "install.sh, with the running engine refusing any reload" inst
unset FAKE_CTL
check "the router imported, with its [client] section" grep -qx '\[client router\]' "$R/etc/cengarde/cengarde.conf"
check "no reload asked of the old engine, the old service not stopped" not called '^cengarde ctl .*reload$|^systemctl reload cengarde$|^systemctl stop cengarde$'
check "the new engine checked the file, cengarde restarted" \
	sh -c 'grep -q "^cengarde -t -c " "$1" && grep -q "^systemctl restart cengarde$" "$1"' sh "$CALLS"
fresh instnew
rm -f "$R/etc/cengarde/nat.conf"
printf 'ENGINE_USER=root\n' >"$TMP/natconf.keep"
check "install.sh with no nat.conf and no secret" sh -c 'env CENGARDE_ROOT="$1" CENGARDE_ENGINE_USER=root CENGARDE_NAT_CONF="$4" PATH="$2" sh "$3" >/dev/null 2>&1' \
	sh "$R" "$BIN:$PATH" "$INSTALL" "$TMP/natconf.keep"
check "a commented nat.conf written" grep -q '^#FORWARD_ALLOW_LOW=' "$R/etc/cengarde/nat.conf"

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
