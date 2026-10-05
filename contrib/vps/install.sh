#!/bin/sh
# Builds and installs cengarde on a server (a VPS, or a Debian or Ubuntu box
# of your own) from this checkout, and (re)starts it. The cloud-config runs
# it at first boot; to upgrade, check out the router's commit and run it
# again. From a terminal or an SSH session (sshd among its parents, which
# sudo keeps, unlike SSH_CONNECTION) it goes on as a systemd unit of its own
# (cengarde-upgrade), so a session that drops halfway, because it rode the
# tunnel being restarted, does not stop it; every step checks first what is
# done already, so running it again finishes a half-done upgrade.
#
#   sudo sh contrib/vps/install.sh
#
# Needs build-essential, git, iptables, wireguard-tools and conntrack
# (cloud-config.yaml installs them). With /etc/cengarde/secret (the
# cloud-config writes it) its router is imported; without it, add one with
# "sudo cengarde-vps-setup add NAME". From cengarde 0.4: its wg0 becomes
# cg-router, with the same port and tunnel address, and the service gets a
# static user. Back to 0.4: "sudo cengarde-vps-setup purge", then the 0.4
# checkout's install.sh. firewalld is not supported yet: it stops here.
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu

SRC=$(cd "$(dirname "$0")/../.." && pwd)
PATH=$PATH:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
R=${CENGARDE_ROOT:-}
ENGINE_USER=${CENGARDE_ENGINE_USER:-cengarde}
LOG=$R/var/log/cengarde-upgrade.log
STATUS=$R/run/cengarde-upgrade.status
UNITS=$R/etc/systemd/system
SBIN=$R/usr/local/sbin
VPS=$SRC/contrib/vps

say() { echo "install.sh: $*"; }
die() {
	echo "install.sh: $*" >&2
	exit 1
}

# detach: this script again, as the transient unit cengarde-upgrade, with
# its output in LOG, which this session follows until the unit ends.
detach() {
	local start tailpid rc
	mkdir -p "${LOG%/*}"
	touch "$LOG"
	chmod 0640 "$LOG"
	start=$(($(stat -c %s "$LOG") + 1))
	rm -f "$STATUS"
	systemd-run --unit=cengarde-upgrade --collect --setenv=CENGARDE_UPGRADE_DETACHED=1 \
		-p "StandardOutput=append:$LOG" -p "StandardError=append:$LOG" \
		/bin/sh "$VPS/install.sh" ||
		die "systemd-run failed: is an upgrade running already? (systemctl status cengarde-upgrade)"
	say "running as the unit cengarde-upgrade: it goes on if this session drops"
	say "its output: $LOG (tail -f $LOG; systemctl status cengarde-upgrade)"
	tail -c +"$start" -f "$LOG" &
	tailpid=$!
	while :; do
		case $(systemctl show -p ActiveState --value cengarde-upgrade 2>/dev/null) in
		active | activating | reloading) sleep 1 ;;
		*) break ;;
		esac
	done
	sleep 1
	kill "$tailpid" 2>/dev/null || :
	wait "$tailpid" 2>/dev/null || :
	rc=$(cat "$STATUS" 2>/dev/null || echo 1)
	exit "$rc"
}

# from_ssh: sshd is among this process's parents. sudo resets the
# environment (no SSH_CONNECTION in "ssh vps 'sudo sh install.sh'"), not the
# process tree.
from_ssh() {
	local p=$PPID
	while [ "${p:-0}" -gt 1 ]; do
		case $(cat "/proc/$p/comm" 2>/dev/null) in sshd | sshd-session) return 0 ;; esac
		p=$(sed -n 's/^.*) [A-Za-z] \([0-9][0-9]*\) .*$/\1/p' "/proc/$p/stat" 2>/dev/null)
	done
	return 1
}

[ -n "$R" ] || [ "$(id -u)" -eq 0 ] || die "run it as root: sudo sh $0"
if command -v systemctl >/dev/null && systemctl is-active -q firewalld 2>/dev/null; then
	die "firewalld is active and not supported yet: use ufw or nftables, or stop firewalld"
fi
if [ -z "${CENGARDE_UPGRADE_DETACHED:-}" ] && { [ -t 0 ] || [ -n "${SSH_CONNECTION:-}" ] || from_ssh; } &&
	[ -z "$R" ] && [ -d /run/systemd/system ] && command -v systemd-run >/dev/null; then
	detach
fi
if [ -n "${CENGARDE_UPGRADE_DETACHED:-}" ]; then
	# shellcheck disable=SC2154 # rc is set by the trap itself
	trap 'rc=$?; echo "$rc" >"$STATUS"; [ "$rc" -eq 0 ] && say "done" || say "FAILED (exit $rc)"' EXIT
	say "started $(date -u +%Y-%m-%dT%H:%M:%SZ) from $SRC"
fi

# The build first: one that fails leaves a 0.4 server, and its router's
# tunnel, as they were.
say "1/6: build"
make -C "$SRC/engine"

say "2/6: cengarde 0.4's wg0"
if [ -e "$R/etc/wireguard/wg0.conf" ] &&
	head -n 1 "$R/etc/wireguard/wg0.conf" | grep -q '^# Written by cengarde-vps-setup'; then
	# Before anything new is installed: its PostDown runs the cengarde-nat
	# installed now, 0.4's own "down", or, when this runs again, the new
	# one's, which removes 0.4's rules.
	systemctl disable --now wg-quick@wg0 || :
	rm -f "$R/etc/wireguard/wg0.conf"
	say "wg0 stopped and removed; its router comes back as cg-router, same port and tunnel address"
fi

say "3/6: install"
make -C "$SRC/engine" install DESTDIR="$R" PREFIX=/usr/local
install -d -m 0755 "$SBIN"
install -m 0755 "$VPS/cengarde-nat" "$VPS/cengarde-vps-setup" "$SBIN/"
install -d -m 0755 "$R/etc/sysusers.d"
install -m 0644 "$SRC/contrib/systemd/cengarde.sysusers" "$R/etc/sysusers.d/cengarde.conf"
# The previous unit's DynamicUser was also named cengarde; while it runs,
# systemd-sysusers sees it and would create no static user.
if ! grep -q "^$ENGINE_USER:" "$R/etc/passwd" 2>/dev/null; then
	systemctl stop cengarde 2>/dev/null || :
fi
systemd-sysusers "$R/etc/sysusers.d/cengarde.conf"
grep -q "^$ENGINE_USER:" "$R/etc/passwd" || die "systemd-sysusers did not create the user $ENGINE_USER"
install -d -m 0755 "$UNITS" "$UNITS/cengarde.service.d"
install -m 0644 "$SRC/contrib/systemd/cengarde.service" "$UNITS/cengarde.service"
# On a VPS the engine is kept away from the metadata service (the secret).
install -m 0644 "$VPS/cengarde-vps.conf" "$UNITS/cengarde.service.d/vps.conf"
install -m 0644 "$VPS/cengarde-nat.service" "$VPS/cengarde-nat-check.service" "$VPS/cengarde-nat-check.timer" \
	"$VPS/cengarde-passthrough.path" "$VPS/cengarde-passthrough.service" "$UNITS/"
if systemctl is-enabled -q nftables 2>/dev/null; then
	install -d -m 0755 "$UNITS/nftables.service.d"
	install -m 0644 "$VPS/nftables-cengarde.conf" "$UNITS/nftables.service.d/cengarde.conf"
	say "nftables.service is enabled: cengarde-nat sync runs after its start, reload and stop"
else
	rm -f "$UNITS/nftables.service.d/cengarde.conf"
fi
install -d -m 0755 "$R/etc/sysctl.d"
install -m 0644 "$VPS/sysctl.conf" "$R/etc/sysctl.d/90-cengarde.conf"
sysctl -q --system
install -d -m 0750 -o "$ENGINE_USER" -g "$ENGINE_USER" "$R/var/lib/cengarde"
install -d -m 0750 "$R/etc/cengarde"
if [ ! -e "$R/etc/cengarde/nat.conf" ]; then
	cat >"$R/etc/cengarde/nat.conf" <<-'EOF'
		# Settings of cengarde-nat and cengarde-vps-setup, as shell variables;
		# after a change: sudo cengarde-vps-setup. The defaults:
		#CENGARDE_PORT=65500      the engine's UDP port (the router's "port")
		#WG_PORT_BASE=65501       router k's WireGuard: 127.0.0.1, this + k
		#PASSTHROUGH=no           IP pass until the router asks for it
		#PASSTHROUGH_PORTS=1024:65000
		#SSH_PORT=22              never forwarded
		#FORWARD_ALLOW_LOW=""     ports below 1024 routers may have: "tcp:80 tcp:443"
		#FORWARD_RESERVED=""      more ports never forwarded: "udp:5000-5010"
		#FORWARD_AUTO_RESERVE=yes this server's own listeners and Docker ports
		#FORWARD_UNRESERVE=""     forwarded even though something listens there
		#FORWARD_SKIP_SRC=""      sources never forwarded (set for a private IPv4)
		#PUB_IF=""                only this interface forwarded and masqueraded
		#ROUTER_NAME=router       the name of the cloud-config's router
	EOF
	chmod 0644 "$R/etc/cengarde/nat.conf"
fi
systemctl daemon-reload

say "4/6: the firewall (cengarde-nat apply; cengarde 0.4's rules removed)"
cengarde-nat apply

say "5/6: the routers (cengarde-vps-setup)"
cengarde-vps-setup

say "6/6: the services"
systemctl enable cengarde-nat.service cengarde-nat-check.timer cengarde-passthrough.path cengarde.service
systemctl start cengarde-nat.service
systemctl restart cengarde-nat-check.timer cengarde-passthrough.path
systemctl restart cengarde
say "$(cengarde version 2>/dev/null || echo cengarde) running; sudo cengarde ctl status, or sudo cengarde-vps-setup list"
