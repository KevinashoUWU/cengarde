#!/bin/sh
# End-to-end test of the OpenWrt packages in two VMs (vm.sh):
#
#   e2e.sh IMAGE [OUT_DIR]
#
# IMAGE is an x86-64 ext4-combined image with cengarde, luci-app-cengarde
# and luci-i18n-cengarde-es (see openwrt/README.md). The test:
#  1. boots the router and the VPS VMs;
#  2. gives the router three DHCP uplinks (up1-up3) and the VPS their far
#     ends, an address of its own (1.2.3.4: miniupnpd refuses a reserved
#     one as the external address) and a cengarde server with WireGuard,
#     all keys derived from the router's pairing secret;
#  3. configures cengarde on the router from LuCI only (luci.mjs), which
#     waits for the tunnel, the three uplinks and IP pass on at the VPS,
#     and pauses and resumes an uplink from the status page; each uplink
#     gets its DHCPv6 companion (up16-up36) in the uplinks' zone;
#  4. turns IP pass off while pinging through the tunnel: the engine takes
#     the change without a restart and without loss, and the VPS follows;
#  5. pings through the tunnel while one uplink goes down: no loss allowed;
#  6. checks that apply has converged: run again it changes nothing, and
#     nothing reloads for 60 s (the companions' triggers cause no loop);
#  7. disables cengarde and checks that the router is back as it was, and
#     converged again.
# Screenshots and logs go to OUT_DIR (default ./e2e-out). Needs qemu-system-x86,
# ssh and node with "npm install" done in this directory.
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${1:?usage: e2e.sh IMAGE [OUT_DIR]}
OUT=${2:-$PWD/e2e-out}
export VM_DIR="$OUT/vm"
VM="$HERE/vm.sh"
fails=0

say() { echo "e2e: $*"; }
ok() { echo "e2e: ok: $*"; }
bad() {
	echo "e2e: FAIL: $*"
	fails=$((fails + 1))
}

# Log lines that a reload loop would keep adding: cengarde-setup changing
# the configuration, and netifd taking interfaces up or down.
churn() {
	"$VM" ssh router 'logread | grep -cE "cengarde-setup: updated:|netifd: Interface .* is now (up|down)" || true'
}

# converged WHAT: "cengarde-setup apply" changes nothing more, and in the
# next 60 s nothing reloads.
converged() {
	local before after

	before=$(churn)
	"$VM" ssh router 'cengarde-setup apply'
	sleep 60
	after=$(churn)
	if [ "$before" = "$after" ]; then
		ok "$1: apply changes nothing, no reloads in 60 s"
	else
		bad "$1: $((after - before)) new updates or interface changes"
		"$VM" ssh router 'logread | grep -E "cengarde-setup|netifd" | tail -20'
	fi
}

mkdir -p "$OUT"
trap '"$VM" stop' EXIT
"$VM" start "$IMAGE"
"$VM" wait

say "VPS: uplink ends with DHCP, 1.2.3.4, cengarde server and WireGuard"
secret=$("$VM" ssh router 'uci get cengarde.main.secret')
"$VM" ssh vps "SECRET='$secret' sh -s" <<'EOF'
set -e
uci -q delete network.wan || true
uci -q delete network.wan6 || true
for i in 1 2 3; do
	uci set network.up$i=interface
	uci set network.up$i.proto=static
	uci set network.up$i.device=eth$i
	uci set network.up$i.ipaddr=10.1.$i.1
	uci set network.up$i.netmask=255.255.255.0
	uci set dhcp.up$i=dhcp
	uci set dhcp.up$i.interface=up$i
	uci set dhcp.up$i.start=100
	uci set dhcp.up$i.limit=10
done
uci set network.public=interface
uci set network.public.proto=static
uci set network.public.device=lo
uci set network.public.ipaddr=1.2.3.4
uci set network.public.netmask=255.255.255.255
# What cengarde-vps-setup does on Ubuntu, with OpenWrt's own WireGuard.
eval "$(echo "$SECRET" | cengarde keys)"
uci set network.wg0=interface
uci set network.wg0.proto=wireguard
uci set network.wg0.private_key="$CG_WG_SERVER_KEY"
uci set network.wg0.listen_port=65501
uci add_list network.wg0.addresses=10.79.0.1/30
uci set network.router=wireguard_wg0
uci set network.router.public_key="$(echo "$CG_WG_CLIENT_KEY" | wg pubkey)"
uci set network.router.preshared_key="$CG_WG_PSK"
uci add_list network.router.allowed_ips=10.79.0.2/32
uci commit network
uci commit dhcp
mkdir -p /etc/cengarde
# Bound to the address the router uses: this VPS has several (a real one has one).
cat > /etc/cengarde/server.conf <<EOC
mode = server
key = $CG_LINK_KEY
listen = 1.2.3.4:65500
wireguard = 127.0.0.1:65501
status_file = /var/run/cengarde.json
control_socket = /var/run/cengarde/cengarde.sock
passthrough_file = /var/run/cengarde/passthrough
EOC
uci set cengarde.main.config_file=/etc/cengarde/server.conf
uci set cengarde.main.tunnel=0
uci set cengarde.main.enabled=1
uci commit cengarde
/etc/init.d/firewall stop
/etc/init.d/network reload
sleep 3
/etc/init.d/dnsmasq restart >/dev/null 2>&1
/etc/init.d/cengarde restart
EOF

say "router: three DHCP uplinks instead of wan"
"$VM" ssh router sh -s <<'EOF'
set -e
uci -q delete network.wan || true
uci -q delete network.wan6 || true
for i in 1 2 3; do
	uci set network.up$i=interface
	uci set network.up$i.proto=dhcp
	uci set network.up$i.device=eth$i
done
uci commit network
/etc/init.d/network reload
EOF
sleep 10
"$VM" ssh router 'uci show network; uci show firewall; ip route' > "$OUT/router-before.txt"

say "enabled without a VPS address: no tunnel yet, the router keeps its routes and DNS"
"$VM" ssh router sh -s <<'EOF'
uci set cengarde.main.enabled=1
uci commit cengarde
ubus call service event '{"type":"config.change","data":{"package":"cengarde"}}'
EOF
sleep 8
if "$VM" ssh router 'ip route | grep -q wgcg || uci -q get network.wgcg || uci show network | grep -q peerdns'; then
	bad "the tunnel took over before the VPS address was set"
else
	ok "no tunnel without a VPS address"
fi
"$VM" ssh router 'uci set cengarde.main.enabled=0; uci commit cengarde; /etc/init.d/cengarde reload'

say "LuCI: configure, apply, wait for the tunnel"
if node "$HERE/luci.mjs" http://127.0.0.1:8080 "$OUT/screenshots"; then
	ok "configured from LuCI, tunnel up on 3 uplinks"
else
	bad "LuCI test"
fi
"$VM" ssh router 'uci show network; uci show firewall; uci show upnpd; ip route; wg show; cat /var/etc/cengarde.conf | grep -v "^key"; logread | grep cengarde' \
	> "$OUT/router-enabled.txt" 2>&1 || true

if "$VM" ssh router 'pidof miniupnpd >/dev/null && grep -qx ext_ifname=wgcg /var/etc/miniupnpd.conf &&
	grep -qx ext_ip=1.2.3.4 /var/etc/miniupnpd.conf'; then
	ok "IP pass: miniupnpd runs on the tunnel with the VPS address"
else
	bad "IP pass: miniupnpd is not running on the tunnel"
fi

if "$VM" ssh router sh -s <<'EOF'
for i in 1 2 3; do
	[ "$(uci -q get network.up${i}6.device) $(uci -q get network.up${i}6.delegate)" = "@up$i 0" ] || exit 1
	uci show firewall | grep -q "cengarde_network6=.*'up${i}6'" || exit 1
done
EOF
then
	ok "IPv6 companions up16-up36: delegate 0, in the uplinks' zone"
else
	bad "IPv6 companions"
fi

if [ "$("$VM" ssh vps 'cat /var/run/cengarde/passthrough')" = on ]; then
	ok "IP pass: the VPS got the router's request"
else
	bad "IP pass: the VPS has no request from the router"
fi

say "IP pass off while pinging: applied in place, no restart, no loss"
pid=$("$VM" ssh router 'pidof cengarde')
"$VM" ssh router 'ping -q -c 12 10.79.0.1' > "$OUT/ping-reload.txt" 2>&1 &
ping_pid=$!
sleep 3
"$VM" ssh router sh -s <<'EOF'
uci set cengarde.main.ip_pass=0
uci set cengarde.main.mute_behind_ms=200
uci commit cengarde
ubus call service event '{"type":"config.change","data":{"package":"cengarde"}}'
EOF
wait "$ping_pid" || true
cat "$OUT/ping-reload.txt"
if grep -q ' 0% packet loss' "$OUT/ping-reload.txt" && [ "$("$VM" ssh router 'pidof cengarde')" = "$pid" ] &&
	"$VM" ssh router 'logread | grep -q "reload: configuration applied"'; then
	ok "the change went in without restarting the engine or losing a packet"
else
	bad "the change restarted the engine or lost packets"
fi
if [ "$("$VM" ssh vps 'cat /var/run/cengarde/passthrough')" = off ]; then
	ok "IP pass: the VPS follows the switch"
else
	bad "IP pass: the VPS did not get the switch"
fi
if "$VM" ssh router 'cengarde ctl links' > "$OUT/ctl-links.txt" && grep -q '^eth3 .* live ' "$OUT/ctl-links.txt"; then
	ok "cengarde ctl links on the router"
else
	bad "cengarde ctl links"
fi
cat "$OUT/ctl-links.txt"

say "data path: ping through the tunnel while up2 goes down"
"$VM" ssh router 'ping -q -c 12 10.79.0.1' > "$OUT/ping.txt" 2>&1 &
ping_pid=$!
sleep 3
"$VM" ssh vps 'ip link set eth2 down'
sleep 4
"$VM" ssh vps 'ip link set eth2 up'
wait "$ping_pid" || true
cat "$OUT/ping.txt"
if grep -q ' 0% packet loss' "$OUT/ping.txt"; then
	ok "no loss while an uplink was down"
else
	bad "packets lost while an uplink was down"
fi
if "$VM" ssh router 'cengarde-setup status' > "$OUT/status.json" &&
	grep -q '"running":true' "$OUT/status.json"; then
	ok "status reports the engine running"
else
	bad "status"
fi

converged "enabled"

say "disable: the router goes back to how it was"
"$VM" ssh router sh -s <<'EOF'
uci set cengarde.main.enabled=0
uci commit cengarde
ubus call service event '{"type":"config.change","data":{"package":"cengarde"}}'
EOF
sleep 10
"$VM" ssh router 'uci show network; uci show firewall; ip route' > "$OUT/router-after.txt"
leftover=$("$VM" ssh router 'pidof cengarde; uci show network | grep -E "wgcg|cengarde|up[123]6"; uci show firewall | grep -E "cengarde|up[123]6"; ip route | grep wgcg; uci -q get upnpd.config.external_iface' || true)
if [ -z "$leftover" ]; then
	ok "nothing left of the tunnel"
else
	bad "left after disabling: $leftover"
fi
if "$VM" ssh router 'uci show network | grep -q "peerdns"'; then
	bad "peerdns still set on an uplink"
else
	ok "uplinks announce their DNS servers again"
fi
converged "disabled"

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
