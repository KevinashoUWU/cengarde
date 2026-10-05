#!/bin/sh
# shellcheck disable=SC2016 # sh -c scripts: expanded by the inner sh
# cengarde's server side under a real systemd, from cengarde 0.4.2 to this
# checkout and back, with a router in network namespaces:
#
#   router [up1 10.250.81.2] -- [10.250.81.1] inet [10.250.80.2, 169.254.79.254,
#     198.51.100.7] -- [cgsd-pub 10.250.80.1] this machine, the server
#
# The router runs cengarde and WireGuard (wgcg, 10.79.0.2) with the keys of
# the server's secret, as an OpenWrt router of protocol 3 does, and reaches
# the server through inet, the Internet; a web server on its side stands in
# for its LAN. inet stands in for the provider's metadata service too, on a
# link-local address. The test:
# - installs cengarde 0.4.2 (CENGARDE_OLD_REF, from this repository's
#   history) with its install.sh, as its cloud-config did, with PUB_IF
#   facing inet: the tunnel works and IP pass reaches the router;
# - upgrades to this checkout with "sudo sh install.sh" on a terminal
#   (script) that hangs up once wg0 is down (its step 2, after the build),
#   as an SSH session riding the tunnel does: the transient unit
#   cengarde-upgrade finishes anyway; wg0 is gone, cg-router has port 65501
#   and the router's address, no rule of 0.4 is left, the engine runs as
#   the static user cengarde, the secret file was imported and deleted, and
#   the router comes back with IP pass on, as it last asked;
# - the units: systemd-analyze verify, Restart=on-failure accepted for the
#   oneshot cengarde-nat.service; "cengarde ctl reload" answers;
# - IP pass on and off through cengarde-passthrough.path and the file the
#   engine keeps in /var/lib/cengarde; kept across a restart of cengarde;
#   cengarde-nat.service started before the engine;
# - PUB_IF removed: the rules name no interface, and a route added after
#   cengarde-nat.service started works for the routers' Internet;
# - the server's own services: a listener on 8123 and, with Docker, a port
#   it publishes, inside IP pass's range: both stay on the server after the
#   check timer's run, and "cengarde-vps-setup forward" lists them;
# - install.sh again, as "ssh vps 'sudo sh install.sh'" runs it (no
#   terminal, and sudo drops SSH_CONNECTION): it sees the SSH session all
#   the same, goes on as cengarde-upgrade, and changes no rule; plain runs,
#   as cloud-init's, stay attached; IPAddressDeny keeps the engine away from
#   169.254.0.0/16, and the tunnel too;
# - back: "cengarde-vps-setup purge", quick, with cengarde-nat.service's
#   ExecStop on time (purge takes the admin lock it needs only after the
#   units are stopped), then 0.4.2's install.sh: the tunnel and IP pass work
#   again; then this checkout's install.sh again;
# - firewalld active: install.sh and cengarde-nat apply refuse;
# - everything stopped: the firewall is as it was before 0.4.2, behind ufw;
# - nftables.service with a forward chain whose policy is drop: the drop-in
#   puts the rules back after its restart and reload, check prints the
#   lines to add, and with them the router's Internet works through it;
# - "cengarde-vps-setup remove": the router's interface and forwarding gone.
#
#   sudo sh contrib/vps/test/systemd.sh --yes
#
# It installs cengarde on this machine for good, turns ufw and nftables on
# and flushes the firewall, so it runs only on a throwaway one: with CI=true
# (vps.yml, on GitHub's runners) or --yes. Needs systemd as PID 1, the
# wireguard module, wireguard-tools, iptables, conntrack, nftables, git and
# the repository's history (CENGARDE_OLD_REF), python3, curl, make and a C
# compiler, script, sudo and pgrep, and ufw and docker if they are to be
# tested too;
# 10.250.80.0/23, 198.51.100.7 and 10.79.0.0/16 must be free here.
#
# SPDX-License-Identifier: GPL-2.0-only
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
OLD_REF=${CENGARDE_OLD_REF:-1c6005175e01c004c9c7b706f230d86c26b96f76} # 0.4.2
CG=/usr/local/sbin/cengarde NAT=/usr/local/sbin/cengarde-nat SETUP=/usr/local/sbin/cengarde-vps-setup
SECRET=/etc/cengarde/secret NAT_CONF=/etc/cengarde/nat.conf
PASS_FILE=/var/lib/cengarde/passthrough
UPGRADE_LOG=/var/log/cengarde-upgrade.log UPGRADE_STATUS=/run/cengarde-upgrade.status
ROUTER=cgsd-router INET=cgsd-inet PUB=cgsd-pub
VPS_IP=10.250.80.1 INET_IP=10.250.80.2 CARRIER_IP=10.250.81.1 UPLINK_IP=10.250.81.2
FAR_IP=198.51.100.7 # on inet, reached through a route added later
META=169.254.79.254 # a fake metadata service, in inet
TUN_VPS=10.79.0.1 TUN_ROUTER=10.79.0.2
CG_PORT=65500 PASS_PORT=9000
T0=0 TMP='' ROUTER_PID='' RULES0='' DENY='' LOCAL_PID='' DOCKER_ID=''
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

UNITS="cengarde wg-quick@wg0 wg-quick@cg-router cengarde-nat cengarde-nat-check.timer cengarde-nat-check
cengarde-passthrough.path cengarde-passthrough cengarde-upgrade nftables"

cleanup() {
	[ -z "$LOCAL_PID" ] || kill "$LOCAL_PID" 2>/dev/null
	[ -z "$DOCKER_ID" ] || docker rm -f "$DOCKER_ID" >/dev/null 2>&1
	systemctl stop firewalld 2>/dev/null
	for ns in "$ROUTER" "$INET"; do
		pids=$(ip netns pids "$ns" 2>/dev/null)
		# shellcheck disable=SC2086 # one PID per word
		[ -z "$pids" ] || kill $pids 2>/dev/null
		ip netns del "$ns" 2>/dev/null
	done
	ip link del "$PUB" 2>/dev/null
	ip route del "$FAR_IP/32" 2>/dev/null
	[ -z "$TMP" ] || rm -rf "$TMP"
}

# What a failure needs to be understood.
diag() {
	echo "--- diagnostics"
	# shellcheck disable=SC2086 # one unit per word
	systemctl --no-pager --full status $UNITS 2>&1 | tail -n 80
	# shellcheck disable=SC2046 # one -u per unit
	journalctl -q --no-pager --since "@$T0" $(for u in $UNITS; do printf -- '-u %s ' "$u"; done) 2>&1 | tail -n 120
	tail -n 40 "$UPGRADE_LOG" 2>/dev/null
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
			die "it installs cengarde on this machine for good, turns ufw and nftables on and flushes the firewall: run it on a throwaway machine, with CI=true or --yes"
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
	for cmd in ip iptables ip6tables wg wg-quick conntrack nft git python3 curl ping make cc journalctl \
		systemd-run systemd-analyze setsid script sudo pgrep; do
		command -v "$cmd" >/dev/null || die "$cmd is missing"
	done
	git -c safe.directory="$SRC" -C "$SRC" cat-file -e "$OLD_REF:contrib/vps/install.sh" 2>/dev/null ||
		die "commit $OLD_REF (cengarde 0.4.2) is not in this repository's history: fetch it (actions/checkout with fetch-depth: 0)"
}

# prepare: the engine for the router, a secret of the test's own, 0.4.2's
# tree, and the nat.conf of 0.4.2's cloud-config with PUB_IF facing inet.
# IP pass starts off, so that the router turns it on.
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
	rm -rf /var/lib/cengarde-nat /var/lib/cengarde
	keys=$("$SRC/engine/cengarde" keys <"$SECRET") || fatal "cengarde keys"
	eval "$keys"
	mkdir "$TMP/old"
	git -c safe.directory="$SRC" -C "$SRC" archive "$OLD_REF" | tar -x -C "$TMP/old" || fatal "git archive $OLD_REF"
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
		ip -n "$INET" addr add "$FAR_IP/32" dev lo &&
		ip -n "$INET" link set dev pub up &&
		ip link add up1 netns "$ROUTER" type veth peer name isp netns "$INET" &&
		ip -n "$ROUTER" addr add "$UPLINK_IP/30" dev up1 &&
		ip -n "$ROUTER" link set dev up1 up &&
		ip -n "$INET" addr add "$CARRIER_IP/30" dev isp &&
		ip -n "$INET" link set dev isp up &&
		ip netns exec "$INET" sysctl -qw net.ipv4.ip_forward=1 &&
		# This machine reaches the router's uplink and the metadata
		# through inet; the router reaches the server through its
		# carrier, and everything else through the tunnel.
		ip route add "$UPLINK_IP/32" via "$INET_IP" &&
		ip route add "$META/32" via "$INET_IP" &&
		ip -n "$ROUTER" route add "$VPS_IP/32" via "$CARRIER_IP" dev up1 || return 1
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
# pass_rules: IP pass forwards TCP and UDP to the router (0.4.2's rules or
# cengarde-nat's chain).
pass_rules() {
	for proto in tcp udp; do
		iptables -t nat -S | grep -q -- "-p $proto -m $proto --dport 1024:65000 -j DNAT --to-destination $TUN_ROUTER\$" ||
			return 1
	done
}
no_pass_rules() { ! iptables -t nat -S | grep -q -- "--to-destination $TUN_ROUTER\$"; }
pass_file() { [ "$(cat "$PASS_FILE" 2>/dev/null)" = "$1" ]; }
# legacy_rules: any rule of cengarde 0.4.
legacy_rules() { rules | grep -Eq -- '-i wg0|-o wg0|10\.79\.0\.0/30|-i cgsd-pub -p (tcp|udp) -m (tcp|udp) --dport'; }
# journal UNIT TEXT [SINCE]: UNIT logged TEXT since SINCE (seconds since the
# epoch; by default, the start of the test).
journal() {
	journalctl -q --no-pager --since "@${3:-$T0}" -u "$1" | grep -qF "$2"
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
active() { # UNIT...: all active
	for u; do
		systemctl is-active -q "$u" || return 1
	done
}
# active_new: the units of this checkout are active.
active_new() { active cengarde wg-quick@cg-router cengarde-nat cengarde-nat-check.timer cengarde-passthrough.path; }
# verify: systemd-analyze verify of the units install.sh put in place, the
# drop-ins included. Its output is shown; anything about them fails.
verify() {
	set -- /etc/systemd/system/cengarde.service /etc/systemd/system/cengarde-nat.service \
		/etc/systemd/system/cengarde-nat-check.service /etc/systemd/system/cengarde-nat-check.timer \
		/etc/systemd/system/cengarde-passthrough.path /etc/systemd/system/cengarde-passthrough.service
	[ ! -e /etc/systemd/system/nftables.service.d/cengarde.conf ] || set -- "$@" nftables.service
	out=$(systemd-analyze verify --man=no "$@" 2>&1)
	rc=$?
	[ -z "$out" ] || printf '%s\n' "$out" | sed 's/^/  verify: /'
	[ "$rc" -eq 0 ] && ! printf '%s\n' "$out" | grep -q cengarde
}
engine_user() { ps -o user= -p "$(systemctl show -p MainPID --value cengarde)" | tr -d ' '; }

# router_wg: the router's WireGuard, as cengarde-setup 0.4 sets it up on
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
	ip netns exec "$ROUTER" "$SRC/engine/cengarde" -c "$TMP/router.conf" >>"$TMP/router.log" 2>&1 &
	ROUTER_PID=$!
}
router_stop() {
	[ -n "$ROUTER_PID" ] || return 0
	kill "$ROUTER_PID" 2>/dev/null
	wait "$ROUTER_PID" 2>/dev/null
	ROUTER_PID=
}
router_ctl() { "$SRC/engine/cengarde" ctl -s "$TMP/router.sock" "$@"; }
router_live() { router_ctl links 2>/dev/null | grep -Eq '^up1 +[^ ]+ +live '; }
# tunnel_back WHAT: the router's link live, the tunnel and the Internet
# through it, after the server changed under it. WireGuard on a new
# interface has no session: only the router can start a handshake, about
# 15 s after its data goes unanswered.
tunnel_back() {
	check "$1: the router's link is live" wait_for 30 router_live
	check "$1: the tunnel works" wait_for 40 tunnel_ping
	check "$1: the router reaches the Internet through it" fetch "$ROUTER" "http://$INET_IP/index" internet
}
pass_reaches() { fetch "$INET" "http://$VPS_IP:$PASS_PORT/index" site; }

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
}

install_old() {
	say "cengarde 0.4.2's install.sh, as its cloud-config ran it"
	sh "$TMP/old/contrib/vps/install.sh" </dev/null >"$TMP/old-install.log" 2>&1 ||
		{ cat "$TMP/old-install.log"; fatal "0.4.2's install.sh failed"; }
	check "0.4.2: cengarde, wg-quick@wg0 and cengarde-passthrough.path are active" \
		active cengarde wg-quick@wg0 cengarde-passthrough.path
	say "a router with the same secret, asking for IP pass"
	router_wg || fatal "the router's WireGuard"
	router_conf yes
	router_start
	tunnel_back "0.4.2"
	check "0.4.2: IP pass forwards to the router" wait_for 10 pass_rules
	check "0.4.2: the server's public address reaches the router's site" wait_for 10 pass_reaches
}

# upgrade: this checkout's install.sh as the guide says, "sudo sh
# install.sh" on the terminal of an SSH session, which hangs up once wg0 is
# down; the transient unit cengarde-upgrade finishes the upgrade.
upgrade() {
	say "upgrade: sudo sh install.sh on a terminal that hangs up once wg0 is down"
	rm -f "$UPGRADE_STATUS"
	start=$(stat -c %s "$UPGRADE_LOG" 2>/dev/null || echo 0)
	# script gives it a terminal. dash has no job control, so setsid does not
	# fork: $! is script's.
	setsid script -qec "sudo sh '$SRC/contrib/vps/install.sh'" /dev/null </dev/null >"$TMP/session.log" 2>&1 &
	session=$!
	# Step 3 starts once step 2 has taken wg0 down.
	wg0_down() { tail -c +"$((start + 1))" "$UPGRADE_LOG" 2>/dev/null | grep -q 'install.sh: 3/6'; }
	check "it runs as the unit cengarde-upgrade, logging to $UPGRADE_LOG" wait_for 60 wg0_down
	# The terminal hangs up, as when the SSH session drops: SIGHUP reaches
	# sudo, and through it the session's install.sh, which follows the log.
	kill -KILL "$session" 2>/dev/null
	wait "$session" 2>/dev/null
	follower() { pgrep -f -- "-f $UPGRADE_LOG" >/dev/null; }
	check "the hangup ended the session's install.sh, which followed the log" wait_for 30 not follower
	check "the session said where to follow it" grep -q "running as the unit cengarde-upgrade" "$TMP/session.log"
	done_upgrade() { [ -s "$UPGRADE_STATUS" ] && ! systemctl is-active -q cengarde-upgrade; }
	check "the session is gone, and the unit finishes anyway" wait_for 900 done_upgrade
	check "with success" [ "$(cat "$UPGRADE_STATUS" 2>/dev/null)" = 0 ]
	tail -c +"$((start + 1))" "$UPGRADE_LOG" | sed 's/^/  upgrade: /' | tail -n 30
}

after_upgrade() {
	check "wg0 is gone" not ip link show wg0
	check "the new units are active" active_new
	check "cg-router listens on 65501 (0.4's WireGuard port)" [ "$(wg show cg-router listen-port 2>/dev/null)" = 65501 ]
	check "no rule of 0.4 is left" not legacy_rules
	check "the static user cengarde exists" grep -q '^cengarde:' /etc/passwd
	check "the engine runs as cengarde (it says: $(engine_user))" [ "$(engine_user)" = cengarde ]
	check "cengarde.conf is 0640 root:cengarde" [ "$(stat -c '%a %U %G' /etc/cengarde/cengarde.conf)" = "640 root cengarde" ]
	check "the secret file was imported and deleted" [ ! -e "$SECRET" ]
	check "the router keeps 10.79.0.2" grep -qx 'TUNNEL_ADDR=10.79.0.2' /etc/cengarde/clients/router
	check "0.4's IP pass state moved to $PASS_FILE: on" pass_file on
	check "cengarde-nat.service: Restart=on-failure accepted" [ "$(systemctl show -p Restart --value cengarde-nat)" = on-failure ]
	check "systemd-analyze verify says nothing about the installed units" verify
	DENY=$(systemctl show -p IPAddressDeny --value cengarde)
	check "cengarde.service has the drop-in's IPAddressDeny=169.254.0.0/16 (it says: $DENY)" has_deny
	tunnel_back "upgraded"
	check "IP pass is on again" wait_for 15 pass_rules
	check "the server's public address reaches the router's site" wait_for 10 pass_reaches
	check "cengarde ctl reload answers ok (the engine reads the file itself)" [ "$("$CG" ctl reload)" = ok ]
	check "cengarde-vps-setup list shows the router" sh -c '"$1" list | grep -q "^router "' sh "$SETUP"
}

pass_on_off() {
	say "the router asks for IP pass off (passthrough = no, cengarde ctl reload)"
	router_conf no
	check "cengarde ctl reload on the router answers ok" [ "$(router_ctl reload)" = ok ]
	check "the server writes off to $PASS_FILE" wait_for 10 pass_file off
	check "the path unit takes the forwarding away" wait_for 10 no_pass_rules
	check "the server's public address no longer reaches the router" not pass_reaches
	say "on again, then cengarde restarted with the router away"
	router_conf yes
	router_ctl reload >/dev/null
	check "IP pass on again" wait_for 10 pass_rules
	router_stop
	systemctl restart cengarde
	check "cengarde is active again" systemctl is-active -q cengarde
	check "IP pass stays on: the engine's file is kept" pass_rules
	router_start
	check "the router comes back" wait_for 30 router_live
	check "cengarde-passthrough.path is still watching" systemctl is-active -q cengarde-passthrough.path
	say "everything stopped and started: the rules before the engine"
	systemctl stop cengarde cengarde-nat
	check "cengarde-nat stopped: no rule of its own left" not sh -c 'iptables-save | grep -q CG_'
	systemctl start cengarde
	check "starting cengarde does not start cengarde-nat (only orders it)" not systemctl is-active -q cengarde-nat
	systemctl stop cengarde
	systemctl start cengarde-nat cengarde
	# shellcheck disable=SC2046 # two numbers
	set -- $(systemctl show -p ActiveEnterTimestampMonotonic --value cengarde-nat) \
		$(systemctl show -p ExecMainStartTimestampMonotonic --value cengarde)
	check "started together: cengarde-nat is active before the engine starts ($1 < $2)" [ "${1:-0}" -lt "${2:-0}" ]
	check "and IP pass is on" pass_rules
	check "the router comes back" wait_for 30 router_live
}

no_pub_if() {
	say "PUB_IF removed: rules without interface names"
	sed -i '/^PUB_IF=/d' "$NAT_CONF"
	systemctl reload cengarde-nat || bad "systemctl reload cengarde-nat"
	check "no rule names $PUB" not sh -c 'iptables-save | grep -E "CG_|MASQUERADE" | grep -q "$1"' sh "$PUB"
	check "IP pass works without it" pass_reaches
	ip route add "$FAR_IP/32" via "$INET_IP"
	check "a route added after cengarde-nat started: the router reaches $FAR_IP through the tunnel" \
		fetch "$ROUTER" "http://$FAR_IP/index" internet
}

own_services() {
	say "the server's own services, inside IP pass's range"
	mkdir -p "$TMP/local"
	echo local >"$TMP/local/index"
	python3 -m http.server 8123 --bind 0.0.0.0 --directory "$TMP/local" >"$TMP/local.log" 2>&1 &
	LOCAL_PID=$!
	wait_for 10 fetch "" http://127.0.0.1:8123/index local
	if command -v docker >/dev/null && DOCKER_ID=$(timeout 120 docker run -d -p 18080:80 busybox httpd -f -p 80 2>/dev/null); then
		:
	else
		DOCKER_ID=
		say "note: no Docker here (or no busybox image): Docker's ports are not checked"
	fi
	systemctl start cengarde-nat-check.service
	check "after the check timer's run, 8123 stays on the server" fetch "$INET" "http://$VPS_IP:8123/index" local
	check "cengarde-vps-setup forward lists it, with its process" sh -c '"$1" forward | grep -q "tcp:8123 .*something listening here: python3"' sh "$SETUP"
	if [ -n "$DOCKER_ID" ]; then
		check "Docker's 18080 is reserved too" sh -c '"$1" forward | grep -Eq "tcp:18080 "' sh "$SETUP"
		check "and a connection to it reaches the container (any HTTP answer), not the router" \
			wait_for 10 ip netns exec "$INET" curl -s -m 3 -o /dev/null "http://$VPS_IP:18080/"
		docker rm -f "$DOCKER_ID" >/dev/null 2>&1
		DOCKER_ID=
	fi
	check "the engine's configuration has the same reserved set" \
		[ "$(sed -n 's/^# reserved: //p' /etc/cengarde/cengarde.conf)" = "$("$NAT" reserved | tr '\n' ' ' | sed 's/ $//')" ]
	kill "$LOCAL_PID"
	LOCAL_PID=
	systemctl start cengarde-nat-check.service
	check "the listener gone, the check gives 8123 back to IP pass" sh -c '! "$1" reserved | grep -qx tcp:8123' sh "$NAT"
}

reinstall() {
	say "install.sh again, as an upgrade in place, as \"ssh vps 'sudo sh install.sh'\" runs it"
	before=$(rules)
	# No terminal, and sudo drops SSH_CONNECTION: install.sh can only see the
	# session as an sshd among its parents (here sh, under that name).
	ln -sf "$(command -v sh)" "$TMP/sshd"
	"$TMP/sshd" -c 'sudo sh "$1" </dev/null; exit $?' sh "$SRC/contrib/vps/install.sh" >"$TMP/install.log" 2>&1 ||
		{ bad "install.sh again"; cat "$TMP/install.log"; }
	check "it saw the SSH session through sudo: it ran as the unit cengarde-upgrade" \
		grep -q "running as the unit cengarde-upgrade" "$TMP/install.log"
	check "the units are active" active_new
	same_rules "$before" "the same rules"
	check "the router's link comes back" wait_for 30 router_live
	check "IP pass is on" wait_for 10 pass_rules
	check "the tunnel works again" wait_for 40 tunnel_ping
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

rollback() {
	say "back to 0.4.2: cengarde-vps-setup purge, then 0.4.2's install.sh"
	echo "PUB_IF=$PUB" >>"$NAT_CONF" # 0.4.2's rules need it
	t=$(date +%s)
	"$SETUP" purge >"$TMP/purge.log" 2>&1 || { bad "purge"; cat "$TMP/purge.log"; }
	took=$(($(date +%s) - t))
	# cengarde-nat.service's ExecStop takes the admin lock; had purge held it
	# while stopping the unit, systemd would have waited TimeoutStopSec (90 s).
	check "purge takes less than 30 s (it took $took s)" [ "$took" -lt 30 ]
	check "purge: cengarde-nat.service stopped in time" not journal cengarde-nat "Stopping timed out" "$t"
	check "purge: cengarde-nat.service is not failed" not systemctl is-failed -q cengarde-nat
	check "purge: no unit of this checkout left" not sh -c 'ls /etc/systemd/system | grep -Eq "^cengarde-nat|^cengarde-passthrough"'
	check "purge: no rule of its own left" not sh -c 'iptables-save | grep -q CG_'
	check "purge: the secret written back" [ -s "$SECRET" ]
	sh "$TMP/old/contrib/vps/install.sh" </dev/null >"$TMP/old-install.log" 2>&1 ||
		{ bad "0.4.2's install.sh after purge"; cat "$TMP/old-install.log"; }
	check "0.4.2 again: cengarde, wg-quick@wg0 and cengarde-passthrough.path are active" \
		active cengarde wg-quick@wg0 cengarde-passthrough.path
	check "0.4.2 again: wg0 on 65501" [ "$(wg show wg0 listen-port 2>/dev/null)" = 65501 ]
	tunnel_back "0.4.2 again"
	check "0.4.2 again: IP pass on" wait_for 15 pass_rules
	check "0.4.2 again: the server's public address reaches the router's site" wait_for 10 pass_reaches
	say "forward again to this checkout"
	sh "$SRC/contrib/vps/install.sh" </dev/null >"$TMP/install.log" 2>&1 ||
		{ bad "install.sh after the rollback"; cat "$TMP/install.log"; }
	check "upgraded again: the units are active" active_new
	check "no rule of 0.4 is left" not legacy_rules
	check "the secret file, the same router's, removed" [ ! -e "$SECRET" ]
	tunnel_back "upgraded again"
	check "IP pass on" wait_for 15 pass_rules
	sed -i '/^PUB_IF=/d' "$NAT_CONF"
	systemctl reload cengarde-nat
}

firewalld() {
	say "firewalld active: refused"
	systemd-run -q --unit=firewalld sleep 600 || bad "a stand-in firewalld"
	before=$(rules)
	out=$(sh "$SRC/contrib/vps/install.sh" </dev/null 2>&1)
	rc=$?
	check "install.sh refuses (exit $rc)" [ "$rc" -ne 0 ]
	check "with its message" sh -c 'printf "%s\n" "$1" | grep -q "firewalld is active and not supported yet"' sh "$out"
	check "cengarde-nat apply refuses" not "$NAT" apply
	same_rules "$before" "nothing changed"
	systemctl stop firewalld
}

stop_all() {
	# All at once, as a shutdown does.
	say "stop: everything"
	router_stop
	systemctl stop cengarde-passthrough.path cengarde-nat-check.timer cengarde wg-quick@cg-router cengarde-nat
	same_rules "$RULES0" "the firewall is as it was before cengarde 0.4.2"
	check "cg-router is gone" not ip link show cg-router
}

nftables() {
	say "nftables.service, with a forward chain whose policy is drop"
	cat >/etc/nftables.conf <<-'EOF'
		#!/usr/sbin/nft -f
		flush ruleset
		table inet filter {
			chain input { type filter hook input priority filter; policy accept; }
			chain forward { type filter hook forward priority filter; policy drop; }
			chain output { type filter hook output priority filter; policy accept; }
		}
	EOF
	systemctl enable --now nftables || bad "systemctl enable --now nftables"
	sh "$SRC/contrib/vps/install.sh" </dev/null >"$TMP/install.log" 2>&1 ||
		{ bad "install.sh with nftables enabled"; cat "$TMP/install.log"; }
	check "the drop-in is installed" [ -e /etc/systemd/system/nftables.service.d/cengarde.conf ]
	check "systemd-analyze verify says nothing about it" verify
	router_start
	check "the router's link is live" wait_for 30 router_live
	systemctl restart nftables
	check "after nftables' restart (flush ruleset) the rules are back" sh -c 'iptables-save -t nat | grep -q -- "-j CG_PRE"'
	out=$("$NAT" check 2>&1)
	check "check says which lines to add to the forward chain" \
		sh -c 'printf "%s\n" "$1" | grep -q "chain inet filter forward drops" && printf "%s\n" "$1" | grep -qF "iifname \"cg-*\" accept"' sh "$out"
	check "without them the router's Internet is dropped (the test sees it)" not fetch "$ROUTER" "http://$INET_IP/index" internet
	sed -i 's/policy drop; }/policy drop;\n\t\tiifname "cg-*" accept\n\t\toifname "cg-*" ct state established,related accept\n\t\toifname "cg-*" ct status dnat accept\n\t}/' /etc/nftables.conf
	systemctl reload nftables
	check "after nftables' reload the rules are still there" sh -c 'iptables-save -t nat | grep -q -- "-j CG_PRE"'
	out=$("$NAT" check 2>&1)
	check "check is quiet about it now" not sh -c 'printf "%s\n" "$1" | grep -q "chain inet filter forward"' sh "$out"
	check "the router's Internet works through it" wait_for 40 fetch "$ROUTER" "http://$INET_IP/index" internet
}

remove() {
	say "cengarde-vps-setup remove router"
	"$SETUP" remove router >"$TMP/remove.log" 2>&1 || { bad "remove"; cat "$TMP/remove.log"; }
	cat "$TMP/remove.log"
	check "it says what forwarding it released" grep -q "released its forwarding" "$TMP/remove.log"
	check "cg-router is gone" not ip link show cg-router
	check "nothing is forwarded to it" no_pass_rules
	check "the engine is still running, serving nobody" sh -c 'systemctl is-active -q cengarde && grep -q "^# No router yet" /etc/cengarde/cengarde.conf'
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
	install_old
	upgrade
	after_upgrade
	pass_on_off
	no_pub_if
	own_services
	reinstall
	deny
	rollback
	firewalld
	stop_all
	nftables
	remove
	if [ "$fails" -eq 0 ]; then
		say "all passed"
	else
		diag
		say "$fails failed"
		exit 1
	fi
}

main "$@"
