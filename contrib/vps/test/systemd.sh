#!/bin/sh
# cengarde's VPS side under a real systemd: install.sh from this checkout, as
# the cloud-config runs it, then the services it sets up, with a router in
# network namespaces:
#
#   router [up1 10.250.81.2] -- [10.250.81.1] inet [10.250.80.2, 169.254.79.254]
#     -- [cgsd-pub 10.250.80.1] this machine, the VPS
#
# The router runs cengarde and WireGuard (wgcg, 10.79.0.2) with the keys of
# the VPS's own secret, as an OpenWrt router does, and reaches the VPS
# through inet, the Internet. cgsd-pub is the VPS's public interface
# (PUB_IF in nat.conf), so the test leaves this machine's own interfaces
# alone. inet stands in for the provider's metadata service too, on a
# link-local address. The test checks that:
# - install.sh leaves cengarde, wg-quick@wg0 and cengarde-passthrough.path
#   active, its units pass systemd-analyze verify, and "cengarde ctl status"
#   answers;
# - the router's link goes live, and the tunnel reaches the VPS and, through
#   its NAT, the Internet;
# - IP pass: the router asks for it (passthrough = yes), the server writes
#   /run/cengarde/passthrough, the path unit runs cengarde-nat sync, and a
#   connection from the Internet to the VPS's public address reaches the
#   router; passthrough = no and "cengarde ctl reload" take it away;
# - IP pass stays on across a restart of cengarde with the router away, and
#   the path unit still answers the router when it comes back; a restart of
#   wg-quick@wg0 (down, then up) brings back the same rules, in the same
#   order;
# - install.sh run again, as an upgrade in place, leaves no rule twice, and
#   the tunnel comes back;
# - IPAddressDeny: from cengarde.service's own cgroup 169.254.0.0/16 cannot
#   be reached, nor from a transient unit with the same property, while root
#   reaches it; nor can the router reach it through the tunnel;
# - stopping it all leaves the firewall as it was, behind ufw as on Vultr.
#
#   sudo sh contrib/vps/test/systemd.sh --yes
#
# It installs cengarde on this machine for good and turns ufw on, so it runs
# only on a throwaway one: with CI=true (vps.yml, on GitHub's runners) or
# --yes. Needs systemd as PID 1, the wireguard module, wireguard-tools,
# iptables, python3, curl, make and a C compiler, and ufw if it is to be
# tested too; 10.250.80.0/23 and 10.79.0.0/30 must be free here.
#
# SPDX-License-Identifier: GPL-2.0-only
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
CG=/usr/local/sbin/cengarde NAT=/usr/local/sbin/cengarde-nat # as install.sh installs them
SECRET=/etc/cengarde/secret NAT_CONF=/etc/cengarde/nat.conf
PASS_FILE=/run/cengarde/passthrough STATE_FILE=/var/lib/cengarde-nat/passthrough
ROUTER=cgsd-router INET=cgsd-inet PUB=cgsd-pub
VPS_IP=10.250.80.1 INET_IP=10.250.80.2 CARRIER_IP=10.250.81.1 UPLINK_IP=10.250.81.2
META=169.254.79.254 # a fake metadata service, in inet
TUN_VPS=10.79.0.1 TUN_ROUTER=10.79.0.2
CG_PORT=65500 PASS_PORT=9000
T0=0 TMP='' ROUTER_PID='' RULES0='' DUPS0='' DENY=''
fails=0

say() { echo "systemd: $*"; }
ok() { echo "systemd: ok: $*"; }
bad() {
	echo "systemd: FAIL: $*"
	fails=$((fails + 1))
}
# check WHAT COMMAND...: ok when COMMAND succeeds.
check() {
	what=$1
	shift
	if "$@"; then ok "$what"; else bad "$what"; fi
}
not() { ! "$@"; }
die() {
	echo "systemd.sh: $*" >&2
	exit 2
}
# fatal WHAT: the rest cannot run without it; say so loudly, and stop.
fatal() {
	echo "systemd: FATAL: $*"
	diag
	exit 1
}
# wait_for SECONDS COMMAND...: COMMAND, 0.2 s apart, until it succeeds; no
# new try once SECONDS have passed (one already running may finish first).
wait_for() {
	deadline=$(($(date +%s) + $1))
	shift
	until "$@"; do
		[ "$(date +%s)" -lt "$deadline" ] || return 1
		sleep 0.2
	done
}
# jget FILE EXPR: a value from a status JSON (EXPR in Python, on d).
jget() { python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null; }

cleanup() {
	for ns in "$ROUTER" "$INET"; do
		pids=$(ip netns pids "$ns" 2>/dev/null)
		# shellcheck disable=SC2086 # one PID per word
		[ -z "$pids" ] || kill $pids 2>/dev/null
		ip netns del "$ns" 2>/dev/null
	done
	ip link del "$PUB" 2>/dev/null
	[ -z "$TMP" ] || rm -rf "$TMP"
}

# What a failure needs to be understood.
diag() {
	echo "--- diagnostics"
	systemctl --no-pager --full status cengarde wg-quick@wg0 cengarde-passthrough.path \
		cengarde-passthrough.service 2>&1 | tail -n 60
	journalctl -q --no-pager --since "@$T0" -u cengarde -u wg-quick@wg0 -u cengarde-passthrough.path \
		-u cengarde-passthrough.service 2>&1 | tail -n 80
	iptables -S
	iptables -t nat -S
	wg show 2>&1
	if [ -n "$TMP" ] && [ -e "$TMP/router.log" ]; then
		echo "--- the router's cengarde"
		tail -n 40 "$TMP/router.log"
	fi
}

preflight() {
	case "${1:-}" in
	--yes) ;;
	'')
		[ "${CI:-}" = true ] ||
			die "it installs cengarde on this machine for good and turns ufw on: run it on a throwaway machine, with CI=true or --yes"
		;;
	*) die "usage: $0 [--yes]" ;;
	esac
	[ "$(id -u)" -eq 0 ] || die "run it as root"
	if [ "$(cat /proc/1/comm 2>/dev/null)" != systemd ] || [ ! -d /run/systemd/system ]; then
		echo "systemd: FATAL: systemd is not PID 1 here (PID 1 is $(cat /proc/1/comm 2>/dev/null)): this test needs a machine booted with systemd, such as GitHub's Ubuntu runners" >&2
		exit 1
	fi
	if ! modprobe wireguard; then
		echo "systemd: FATAL: modprobe wireguard failed: this kernel has no WireGuard" >&2
		exit 1
	fi
	for cmd in ip iptables ip6tables wg wg-quick python3 curl ping make cc journalctl systemd-run systemd-analyze; do
		command -v "$cmd" >/dev/null || die "$cmd is missing"
	done
}

# prepare: the engine, a secret of the test's own and the nat.conf that
# cloud-config.yaml writes on a VPS, with PUB_IF facing inet. IP pass starts
# off, so that the router turns it on.
prepare() {
	make -s -C "$SRC/engine" cengarde || fatal "the engine does not build"
	install -d -m 0700 "${SECRET%/*}"
	(umask 077 && "$SRC/engine/cengarde" genkey >"$SECRET") || fatal "cengarde genkey"
	cat >"$NAT_CONF" <<-EOF
		CENGARDE_PORT=$CG_PORT
		PASSTHROUGH=no
		PASSTHROUGH_PORTS=1024:65000
		PUB_IF=$PUB
	EOF
	# A wish left by an earlier run would win over PASSTHROUGH.
	rm -f "$STATE_FILE"
	keys=$("$SRC/engine/cengarde" keys <"$SECRET") || fatal "cengarde keys"
	eval "$keys"
}

# net_up: the router and the Internet, in network namespaces.
net_up() {
	for ns in "$ROUTER" "$INET"; do
		ip netns add "$ns" && ip -n "$ns" link set dev lo up || return 1
	done
	ip link add "$PUB" type veth peer name pub netns "$INET" &&
		ip addr add "$VPS_IP/30" dev "$PUB" &&
		ip link set dev "$PUB" up &&
		ip -n "$INET" addr add "$INET_IP/30" dev pub &&
		ip -n "$INET" addr add "$META/32" dev pub &&
		ip -n "$INET" link set dev pub up &&
		ip link add up1 netns "$ROUTER" type veth peer name isp netns "$INET" &&
		ip -n "$ROUTER" addr add "$UPLINK_IP/30" dev up1 &&
		ip -n "$ROUTER" link set dev up1 up &&
		ip -n "$INET" addr add "$CARRIER_IP/30" dev isp &&
		ip -n "$INET" link set dev isp up &&
		ip netns exec "$INET" sysctl -qw net.ipv4.ip_forward=1 &&
		# This machine reaches the router's uplink and the metadata
		# through inet; the router reaches the VPS through its carrier,
		# and everything else through the tunnel.
		ip route add "$UPLINK_IP/32" via "$INET_IP" &&
		ip route add "$META/32" via "$INET_IP" &&
		ip -n "$ROUTER" route add "$VPS_IP/32" via "$CARRIER_IP" dev up1 || return 1
	# Web servers: the Internet (with the "user data" on $META), and one on
	# the router's LAN side for IP pass to reach.
	mkdir "$TMP/inet" "$TMP/site"
	echo internet >"$TMP/inet/index"
	echo user-data >"$TMP/inet/user-data"
	echo site >"$TMP/site/index"
	ip netns exec "$INET" python3 -m http.server 80 --bind 0.0.0.0 --directory "$TMP/inet" \
		>"$TMP/inet.log" 2>&1 &
	ip netns exec "$ROUTER" python3 -m http.server "$PASS_PORT" --bind 0.0.0.0 --directory "$TMP/site" \
		>"$TMP/site.log" 2>&1 &
	wait_for 10 fetch "$INET" http://127.0.0.1/index internet &&
		wait_for 10 fetch "$ROUTER" "http://127.0.0.1:$PASS_PORT/index" site
}

# fetch NS URL WANT: NS (empty: this machine) reads WANT at URL; CODE is
# curl's exit status.
fetch() {
	if [ -n "$1" ]; then
		got=$(ip netns exec "$1" curl -s -m 3 "$2")
	else
		got=$(curl -s -m 3 "$2")
	fi
	CODE=$?
	[ "$CODE" -eq 0 ] && [ "$got" = "$3" ]
}
# rejected NS URL: NS cannot connect to URL, and learns it at once.
rejected() {
	ip netns exec "$1" curl -s -m 3 -o /dev/null "$2"
	CODE=$?
	[ "$CODE" -eq 7 ] || say "curl exit $CODE: read, or dropped instead of rejected?"
	[ "$CODE" -eq 7 ]
}
# denied STATUS: curl's exit status says it could not connect (7) or got no
# answer (28), not that it could not run.
denied() { [ "$1" = 7 ] || [ "$1" = 28 ]; }
# from_cgroup DIR URL: curl's exit status for URL from inside the cgroup DIR
# (v2), like a process of that service; 99 if it could not get in.
from_cgroup() {
	# shellcheck disable=SC2016 # expanded by the inner sh
	sh -c 'echo $$ >"$1/cgroup.procs" || exit 99; exec curl -s -m 3 -o /dev/null "$2"' sh "$1" "$2"
}
# from_unit URL [PROPERTY]: curl's exit status for URL from a transient
# service, with PROPERTY.
from_unit() {
	if [ $# -gt 1 ]; then
		systemd-run -q --wait --pipe --collect -p "$2" curl -s -m 3 -o /dev/null "$1"
	else
		systemd-run -q --wait --pipe --collect curl -s -m 3 -o /dev/null "$1"
	fi
}

# rules: this machine's firewall, without counters, one rule per line.
rules() {
	for t in filter nat; do
		iptables -t "$t" -S | sed "s/^/4 $t /"
	done
	ip6tables -t filter -S 2>/dev/null | sed 's/^/6 filter /'
}
dups() { rules | sort | uniq -d; }
# same_rules BEFORE WHAT: ok when the firewall is BEFORE again; else what
# changed.
same_rules() {
	now=$(rules)
	if [ "$now" = "$1" ]; then
		ok "$2"
	else
		bad "$2; what changed:"
		printf '%s\n' "$1" >"$TMP/rules.before"
		printf '%s\n' "$now" | diff "$TMP/rules.before" -
	fi
}
# pass_rules: IP pass forwards TCP and UDP to the router.
pass_rules() {
	for proto in tcp udp; do
		iptables -t nat -C PREROUTING -i "$PUB" -p "$proto" --dport 1024:65000 \
			-j DNAT --to-destination "$TUN_ROUTER" 2>/dev/null || return 1
	done
}
no_pass_rules() { ! iptables -t nat -S PREROUTING | grep -q -- "--to-destination $TUN_ROUTER"; }
pass_file() { [ "$(cat "$PASS_FILE" 2>/dev/null)" = "$1" ]; }
state_file() { [ "$(cat "$STATE_FILE" 2>/dev/null)" = "$1" ]; }
# journal TEXT [SINCE]: cengarde-passthrough.service logged TEXT since SINCE
# (seconds since the epoch; by default, the start of the test).
journal() {
	journalctl -q --no-pager --since "@${2:-$T0}" -u cengarde-passthrough.service | grep -qF "$1"
}
server_status() {
	"$CG" ctl status >"$TMP/server.json" 2>/dev/null &&
		[ "$(jget "$TMP/server.json" 'd["mode"]')" = server ]
}
server_live() {
	server_status &&
		[ "$(jget "$TMP/server.json" '"live" in [l["state"] for s in d["sessions"] for l in s["links"]]')" = True ]
}
tunnel_ping() { ip netns exec "$ROUTER" ping -c 1 -W 1 "$TUN_VPS" >/dev/null; }
has_deny() {
	case " $DENY " in *" 169.254.0.0/16 "*) ;; *) return 1 ;; esac
}
# active: the three units install.sh starts are active.
active() {
	for u in cengarde wg-quick@wg0 cengarde-passthrough.path; do
		systemctl is-active -q "$u" || return 1
	done
}
# verify: systemd-analyze verify of the units install.sh put in place, the
# drop-in included. Its output is shown; anything about them fails.
verify() {
	out=$(systemd-analyze verify --man=no /etc/systemd/system/cengarde.service \
		/etc/systemd/system/cengarde-passthrough.path \
		/etc/systemd/system/cengarde-passthrough.service 2>&1)
	rc=$?
	[ -z "$out" ] || printf '%s\n' "$out" | sed 's/^/  verify: /'
	[ "$rc" -eq 0 ] && ! printf '%s\n' "$out" | grep -q cengarde
}

# router_wg: the router's WireGuard, as cengarde-setup sets it up on
# OpenWrt: the keys of the secret, its own cengarde as the endpoint, and
# everything through the tunnel.
router_wg() {
	(umask 077 && printf '%s\n' "$CG_WG_CLIENT_KEY" >"$TMP/wg.key" &&
		printf '%s\n' "$CG_WG_PSK" >"$TMP/wg.psk") &&
		pub=$(printf '%s\n' "$CG_WG_SERVER_KEY" | wg pubkey) &&
		ip -n "$ROUTER" link add wgcg type wireguard &&
		ip netns exec "$ROUTER" wg set wgcg private-key "$TMP/wg.key" peer "$pub" \
			preshared-key "$TMP/wg.psk" endpoint "127.0.0.1:$CG_PORT" \
			persistent-keepalive 25 allowed-ips 0.0.0.0/0 &&
		ip -n "$ROUTER" addr add "$TUN_ROUTER/30" dev wgcg &&
		ip -n "$ROUTER" link set dev wgcg mtu 1380 up &&
		ip -n "$ROUTER" route add default dev wgcg
}
# router_conf yes|no: the router's engine configuration, as cengarde-setup
# writes it, asking for IP pass or not.
router_conf() {
	cat >"$TMP/router.conf" <<-EOF
		mode = client
		key = $CG_LINK_KEY
		listen = 127.0.0.1:$CG_PORT
		server = $VPS_IP:$CG_PORT
		interfaces =
		status_file = $TMP/router.json
		control_socket = $TMP/router.sock
		passthrough = $1

		[link up1]
		label = carrier
	EOF
}
router_start() {
	ip netns exec "$ROUTER" "$CG" -c "$TMP/router.conf" >>"$TMP/router.log" 2>&1 &
	ROUTER_PID=$!
}
router_stop() {
	[ -n "$ROUTER_PID" ] || return 0
	kill "$ROUTER_PID" 2>/dev/null
	wait "$ROUTER_PID" 2>/dev/null
	ROUTER_PID=
}
router_ctl() { "$CG" ctl -s "$TMP/router.sock" "$@"; }
router_live() { router_ctl links 2>/dev/null | grep -Eq '^up1 +[^ ]+ +live '; }

# firewall: ufw on, as on Vultr's Ubuntu images (SSH allowed, everything
# else coming in dropped), when it is installed; then the rules to come back
# to.
firewall() {
	if command -v ufw >/dev/null; then
		say "ufw on"
		{ ufw allow 22/tcp && ufw --force enable; } >/dev/null || fatal "ufw enable"
	else
		say "note: no ufw here, so the test runs without it"
	fi
	RULES0=$(rules)
	DUPS0=$(dups)
}

install_vps() {
	say "install.sh from this checkout, as the cloud-config runs it"
	sh "$SRC/contrib/vps/install.sh" || fatal "install.sh failed"
}

services() {
	check "cengarde, wg-quick@wg0 and cengarde-passthrough.path are active" active
	check "cengarde ctl status answers, as a server" wait_for 10 server_status
	DENY=$(systemctl show -p IPAddressDeny --value cengarde)
	check "cengarde.service has the drop-in's IPAddressDeny=169.254.0.0/16 (it says: $DENY)" has_deny
	check "systemd-analyze verify says nothing about the installed units" verify
}

router_up() {
	say "a router with the same secret, asking for IP pass"
	router_wg || fatal "the router's WireGuard"
	router_conf yes
	router_start
	check "the router's link goes live" wait_for 15 router_live
	check "the VPS sees it live" wait_for 10 server_live
	check "the tunnel works: the router pings the VPS ($TUN_VPS)" wait_for 15 tunnel_ping
	check "the router reaches the Internet through the tunnel and its NAT" \
		fetch "$ROUTER" "http://$INET_IP/index" internet
}

pass_on_off() {
	check "IP pass: the server writes the router's wish, on, to $PASS_FILE" wait_for 10 pass_file on
	check "the path unit runs cengarde-nat sync: TCP and UDP forwarded to the router" wait_for 10 pass_rules
	check "cengarde-passthrough.service logged it" wait_for 5 journal "IP pass on (router)"
	check "a connection to the VPS's public address reaches the router" \
		fetch "$INET" "http://$VPS_IP:$PASS_PORT/index" site
	check "the wish is kept for reboots ($STATE_FILE)" state_file on

	say "the router asks for IP pass off (passthrough = no, cengarde ctl reload)"
	router_conf no
	check "cengarde ctl reload on the router answers ok" [ "$(router_ctl reload)" = ok ]
	check "the server writes off" wait_for 10 pass_file off
	check "the path unit takes the forwarding away" wait_for 10 no_pass_rules
	check "the VPS's public address no longer reaches the router" \
		not fetch "$INET" "http://$VPS_IP:$PASS_PORT/index" site
	check "off is kept for reboots" state_file off
}

restarts() {
	say "IP pass on again, then cengarde restarted with the router away"
	router_conf yes
	router_ctl reload >/dev/null
	check "IP pass on again" wait_for 10 pass_rules
	router_stop
	systemctl restart cengarde
	# The path unit fires as /run/cengarde goes away. Start what it starts
	# only if it did not (it was still busy with the sync above): every start
	# counts toward the unit's start limit (5 in 10 s), and a refused one
	# takes the path unit down with it.
	wait_for 3 journal "IP pass on (router, before)" || systemctl start cengarde-passthrough.service
	check "cengarde is active again" systemctl is-active -q cengarde
	check "nobody has asked the new engine for anything" [ ! -e "$PASS_FILE" ]
	check "IP pass stays on, as the router asked before" pass_rules
	check "cengarde-nat sync said so" wait_for 5 journal "IP pass on (router, before)"
	back=$(date +%s.%N)
	router_start
	check "the router comes back, and asks for on again" wait_for 15 pass_file on
	check "the path unit runs cengarde-nat sync for it" wait_for 10 journal "IP pass on (router)" "$back"
	check "cengarde-passthrough.path is still watching" systemctl is-active -q cengarde-passthrough.path

	say "wg-quick@wg0 restarted: cengarde-nat down, then up, as at a reboot"
	before=$(rules)
	systemctl restart wg-quick@wg0 || bad "systemctl restart wg-quick@wg0"
	same_rules "$before" "the firewall is the same rules, in the same order"
	check "IP pass is back" pass_rules
	check "the tunnel works again" wait_for 30 tunnel_ping
}

reinstall() {
	say "install.sh again, as an upgrade in place"
	sh "$SRC/contrib/vps/install.sh" >"$TMP/install.log" 2>&1 ||
		{ bad "install.sh again"; cat "$TMP/install.log"; }
	check "the three units are active" active
	check "no rule twice" [ "$(dups)" = "$DUPS0" ]
	check "the router's link comes back" wait_for 20 router_live
	check "IP pass is on" wait_for 10 pass_rules
	# install.sh restarted wg-quick@wg0: the new wg0 has no session and no
	# endpoint for the router, so only the router can start a handshake,
	# about 15 s after its data goes unanswered. deny() needs the tunnel.
	check "the tunnel works again" wait_for 30 tunnel_ping
}

deny() {
	say "IPAddressDeny: 169.254.0.0/16 out of the engine's reach, not root's"
	url=http://$META/user-data
	check "root reads the fake metadata at $META" fetch "" "$url" user-data
	cg=/sys/fs/cgroup$(systemctl show -p ControlGroup --value cengarde)
	if [ -e "$cg/cgroup.procs" ]; then
		from_cgroup "$cg" "$url"
		rc=$?
		check "from cengarde.service's own cgroup it cannot (curl exit $rc)" denied "$rc"
	else
		bad "no cgroup v2 directory for cengarde.service ($cg)"
	fi
	from_unit "$url"
	rc=$?
	check "a transient unit reads it (curl exit $rc)" [ "$rc" -eq 0 ]
	from_unit "$url" "IPAddressDeny=$DENY"
	rc=$?
	check "a transient unit with IPAddressDeny=$DENY cannot (curl exit $rc)" denied "$rc"
	check "the router cannot reach it through the tunnel, and is told at once" rejected "$ROUTER" "$url"
	# The provider's own, when there is one (Azure, under GitHub's runners).
	if curl -s -m 3 -o /dev/null http://169.254.169.254/ && [ -e "$cg/cgroup.procs" ]; then
		from_cgroup "$cg" http://169.254.169.254/
		rc=$?
		check "the metadata service at 169.254.169.254 answers root, not cengarde.service (curl exit $rc)" \
			denied "$rc"
	else
		say "note: nothing answers at 169.254.169.254 here; $META stood in for it"
	fi
}

quiet_down() {
	out=$("$NAT" down wg0 2>&1) && [ -z "$out" ]
}

stop_all() {
	# All at once, as a shutdown does: a path unit being stopped starts
	# nothing, and a sync already queued would wait for wg-quick@wg0 to stop
	# (After=) and find no tunnel.
	say "stop: cengarde-passthrough.path, cengarde and wg-quick@wg0 (its PostDown runs cengarde-nat down)"
	router_stop
	systemctl stop cengarde-passthrough.path cengarde wg-quick@wg0
	same_rules "$RULES0" "the firewall is as it was before install.sh"
	check "wg0 is gone" not ip link show wg0 2>/dev/null
	check "cengarde-nat down again succeeds, quietly" quiet_down
}

main() {
	preflight "$@"
	T0=$(date +%s)
	cleanup
	trap cleanup EXIT
	trap 'exit 1' HUP INT TERM
	TMP=$(mktemp -d)
	prepare
	net_up || fatal "the network namespaces"
	firewall
	install_vps
	services
	router_up
	pass_on_off
	restarts
	reinstall
	deny
	stop_all
	if [ "$fails" -eq 0 ]; then
		say "all passed"
	else
		diag
		say "$fails failed"
		exit 1
	fi
}

main "$@"
