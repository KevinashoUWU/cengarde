#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# cengarde-vps-setup and install.sh without touching this machine: every
# path under a directory of the test's own (CENGARDE_ROOT), and fake
# systemctl, wg, wg-quick, ip, stty, cengarde-nat, make, sysctl,
# systemd-sysusers and userdel in PATH, which log what they are asked. The
# engine is the real one for "keys", "genkey" and "-t"; its control socket
# is a fake that answers ok, restarting, an error, or nothing. Checks:
# - add, add again (nothing changes), a new secret (refused, then
#   --replace: WireGuard restarted, the engine restarting), --rename,
#   remove, list, forward; a second router refused under protocol 3; a
#   tunnel address and WireGuard ports out of range; --proto 4 refused with
#   the upgrade command;
# - the secret never on a command line, and only in clients/NAME;
#   on a terminal (script(1)), echo off while it is typed, and back on;
# - a configuration the engine refuses, a reload that fails and one that
#   hangs (cut by the timeout): the previous files back, exit 1; with
#   --replace, the engine's IP pass file too (a router may even be called
#   passthrough);
# - the one-shot import of /etc/cengarde/secret: imported once and deleted;
#   after remove, after --replace and with slot 0 taken, running again
#   leaves clients/ as it was;
# - forward off|on|limit|allow|disallow|reserve|unreserve, and their
#   refusals; allow says what protocol 3's IP pass forwards of the port
#   (nothing below PASSTHROUGH_PORTS), and a PASSTHROUGH_PORTS below 1024,
#   which cengarde 0.4 forwarded whole, is warned about;
# - a 0.4 server: wg0 and its IP pass state migrated, by
#   cengarde-vps-setup and by install.sh (firewalld refused before anything
#   changes, a build that fails before wg0 is touched, the old service
#   stopped before systemd-sysusers, the nftables drop-in only when nftables
#   is enabled, run twice); purge writes the secret back for 0.4's
#   install.sh;
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
# SPDX-License-Identifier: GPL-2.0-only
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
		echo "SESSION   NEWEST LINK  STATE    DOWNLOAD LAST RX  ADDRESS                                         LOCAL"
		echo "1a2b3c4d  yes    0     live     active   0.4 s    203.0.113.5:40000                               -"
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
cat >"$BIN/wg" <<'EOF'
#!/bin/sh
echo "wg $*" >>"$CALLS"
case $1 in
pubkey | genkey) exec "$REAL_WG" "$@" ;;
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

S1=$("$ENGINE" genkey) S2=$("$ENGINE" genkey) S3=$("$ENGINE" genkey)
key_of() { printf '%s\n' "$1" | "$ENGINE" keys | sed -n "s/^CG_LINK_KEY='\(.*\)'$/\1/p"; }
K1=$(key_of "$S1") K2=$(key_of "$S2")

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
mode() { stat -c '%a %U' "$1"; }
fingerprint() { find "$R/etc" "$R/var/lib/cengarde" -type f ! -name '.*' -exec md5sum {} + 2>/dev/null | sort; }

say "add a router, with no secret file (a server of your own)"
fresh add
check "add router" add router "$S1"
C=$R/etc/cengarde/clients/router
check "clients/router: 0600 root, in a 0700 directory" [ "$(mode "$C") $(mode "${C%/*}")" = "600 root 700 root" ]
check "it holds the secret, slot 0, 10.79.0.2 (protocol 3), forwarding on" \
	[ "$(val "$C" SECRET) $(val "$C" SLOT) $(val "$C" TUNNEL_ADDR) $(val "$C" LEGACY_ADDR) $(val "$C" FORWARD)" = "$S1 0 10.79.0.2 1 yes" ]
W=$R/etc/wireguard/cg-router.conf
check "cg-router.conf: 0600, port 65501, 10.79.0.1/32, the router's /32, no wg-quick hooks" \
	sh -c '[ "$(stat -c %a "$1")" = 600 ] && grep -qx "ListenPort = 65501" "$1" && grep -qx "Address = 10.79.0.1/32" "$1" &&
		grep -qx "AllowedIPs = 10.79.0.2/32" "$1" && grep -qx "Table = auto" "$1" && ! grep -q "^Post" "$1"' sh "$W"
check "cengarde.conf: 0640, the router's key, its WireGuard, the IP pass file, the reserved set" \
	sh -c '[ "$(stat -c %a "$1")" = 640 ] && grep -qx "key = $2" "$1" && grep -qx "wireguard = 127.0.0.1:65501" "$1" &&
		grep -qx "passthrough_file = /var/lib/cengarde/passthrough" "$1" &&
		grep -qx "# reserved: tcp:1-1023 tcp:8123 udp:1-1023 udp:65500-65532" "$1"' sh "$R/etc/cengarde/cengarde.conf" "$K1"
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
check "the new key is in cengarde.conf" grep -qx "key = $K2" "$R/etc/cengarde/cengarde.conf"
check "WireGuard restarted (a new peer)" called '^systemctl restart wg-quick@cg-router$'

say "the same secret under another name: a rename"
check "refused without --rename" not add home "$S2"
check "it says how" said "--rename router"
: >"$CALLS"
check "add home --rename router" add home "$S2" --rename router --label "Casa de Ana"
check "clients/home, no clients/router" [ -e "$R/etc/cengarde/clients/home" ] && [ ! -e "$C" ]
check "slot, address and label kept or set" \
	[ "$(val "$R/etc/cengarde/clients/home" SLOT) $(val "$R/etc/cengarde/clients/home" TUNNEL_ADDR) $(val "$R/etc/cengarde/clients/home" LABEL)" = "0 10.79.0.2 Casa de Ana" ]
check "wg-quick@cg-router stopped and its file removed, cg-home up" \
	sh -c 'grep -q "^systemctl disable --now wg-quick@cg-router$" "$1" && grep -q "^systemctl enable --now wg-quick@cg-home$" "$1" &&
		[ ! -e "$2/etc/wireguard/cg-router.conf" ] && [ -e "$2/etc/wireguard/cg-home.conf" ]' sh "$CALLS" "$R"

say "limits of protocol 3 and of the ports"
before=$(fingerprint)
check "a second router is refused" not add office "$S3"
check "it says why" said "protocol 3 serves one router"
check "--proto 4 is refused with the upgrade command" not add office "$S3" --proto 4
check "it prints the upgrade command" said "contrib/vps/install.sh"
check "--tunnel-addr 10.79.0.1 is refused" not add home "$S2" --tunnel-addr 10.79.0.1
check "a name too long for cg-NAME is refused" not add abcdefghijklm "$S3"
check "\"default\" is refused" not add default "$S3"
check "a bad secret is refused" not add home "not-a-secret"
check "nothing changed" [ "$(fingerprint)" = "$before" ]
printf 'ENGINE_USER=root\nWG_PORT_BASE=65520\n' >"$R/etc/cengarde/nat.conf"
check "WireGuard ports past 65535 are refused" not add home "$S2"
printf 'ENGINE_USER=root\nCENGARDE_PORT=65510\n' >"$R/etc/cengarde/nat.conf"
check "cengarde's port inside the WireGuard ports is refused" not add home "$S2"
printf 'ENGINE_USER=root\n' >"$R/etc/cengarde/nat.conf"

say "list and forward"
check "list" setup list
check "list shows the router, its port, address, label and last heard" \
	sh -c 'printf "%s\n" "$1" | grep -E "^home +0 +65501 +10\.79\.0\.2 +yes +0\.4 s ago, 1 session +Casa de Ana$"' sh "$OUT"
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
mkdir -p "$R/var/lib/cengarde"
echo on >"$R/var/lib/cengarde/passthrough" # the router's last IP pass wish
before=$(fingerprint)
check "--replace with a new secret, a reload answered with an error: exit 1" not env FAKE_CTL=error sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --replace' \
	sh "$S3" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed, the IP pass file included" [ "$(fingerprint)" = "$before" ]
check "--replace, a configuration the engine refuses: exit 1" not env FAKE_T_FAIL=1 sh -c 'printf "%s\n" "$1" | env CENGARDE_ROOT="$2" PATH="$3" sh "$4" add home --replace' \
	sh "$S3" "$R" "$BIN:$PATH" "$SETUP"
check "nothing changed, the IP pass file included" [ "$(fingerprint)" = "$before" ]
rm -f "$R/var/lib/cengarde/passthrough"

say "forward off|on|limit|allow|disallow|reserve|unreserve"
H=$R/etc/cengarde/clients/home N=$R/etc/cengarde/nat.conf
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
check "protocol 3: it says nothing forwards them yet (outside PASSTHROUGH_PORTS)" \
	said "tcp:443 is allowed, but nothing forwards it yet"
check "forward allow tcp:80 again: once" setup forward allow tcp:80
check "still once" grep -qx 'FORWARD_ALLOW_LOW="tcp:80 tcp:443"' "$N"
printf 'PASSTHROUGH_PORTS=1:65000\n' >>"$N"
check "with PASSTHROUGH_PORTS=1:65000, forward allow tcp:80" setup forward allow tcp:80
check "it says IP pass forwards it" said "tcp:80 is allowed, inside PASSTHROUGH_PORTS (1:65000): IP pass forwards it to its router"
check "and not that nothing does" not said "nothing forwards it yet"
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
check "the engine gets a key that serves nobody" sh -c 'grep -q "^# No router yet" "$1" && ! grep -q "key = $2" "$1"' sh "$R/etc/cengarde/cengarde.conf" "$K2"
check "remove nobody is refused" not setup remove nobody

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
check "the router's last IP pass wish kept: off" [ "$(cat "$R/var/lib/cengarde/passthrough")" = off ]
check "0.4's state file gone" [ ! -e "$R/var/lib/cengarde-nat" ]
check "the router imported with 10.79.0.2 on port 65501" \
	sh -c '[ "$(sed -n "s/^TUNNEL_ADDR=//p" "$1/etc/cengarde/clients/router")" = 10.79.0.2 ] && grep -qx "ListenPort = 65501" "$1/etc/wireguard/cg-router.conf"' sh "$R"

say "purge, for cengarde 0.4"
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
check "its IP pass wish written back for 0.4" [ "$(cat "$R/var/lib/cengarde-nat/passthrough")" = off ]
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
check "the secret file deleted, IP pass on kept" sh -c '[ ! -e "$1/etc/cengarde/secret" ] && [ "$(cat "$1/var/lib/cengarde/passthrough")" = on ]' sh "$R"
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
