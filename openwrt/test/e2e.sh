#!/bin/sh
# End-to-end test of the OpenWrt packages in two VMs (vm.sh):
#
#   e2e.sh IMAGE [OUT_DIR]
#
# IMAGE is an x86-64 ext4-combined image with cengarde, luci-app-cengarde
# and luci-i18n-cengarde-es (see openwrt/README.md). The test:
#  1. boots the router and the VPS VMs;
#  2. gives the router three DHCP uplinks (up1-up3) and the VPS their far
#     ends, with IPv6 too: router advertisements with a global prefix and a
#     ULA on each (an MTU of 1400 on up3), and on up1 DHCPv6 with prefix
#     delegation, which the router takes through an IPv6 interface of its
#     own, up1v6, as a Starlink router does with wan6, so that its LAN gets
#     a global prefix around the tunnel. The VPS has addresses of its own,
#     1.2.3.4 (miniupnpd refuses a reserved one as the external address),
#     1.2.3.5 and 2001:db8::4, and a cengarde server listening on all of them
#     (*), with WireGuard; all keys derive from the router's pairing secret;
#  3. configures cengarde on the router from LuCI only (luci.mjs), which
#     waits for the tunnel, the three uplinks and IP pass on at the VPS,
#     and pauses and resumes an uplink from the status page; up2 and up3
#     get their DHCPv6 companions (up26, up36) on their devices, in the
#     uplinks' zone, up1v6 stops handing its prefix to the LAN, and a PPPoE
#     uplink (configuration only) gets delegate 0;
#  4. turns IP pass off while pinging through the tunnel: the engine takes
#     the change without a restart and without loss, and the VPS follows;
#  5. pings through the tunnel while one uplink goes down: no loss allowed;
#  6. IPv6 to the VPS: with 2001:db8::4 first in the list every link sends
#     from its global address (not the ULA); the path MTU of up3 shows in
#     the status and the log; with IPv6 blocked on up2 only up2 moves to
#     1.2.3.4, without loss; without 2001:db8::4 they all move;
#  7. checks that apply has converged: run again it changes nothing, and
#     nothing reloads for 60 s (the companions' triggers cause no loop);
#  8. disables cengarde and checks that the router is back as it was (the
#     LAN gets its prefix again), and converged again.
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

# The engine's pid on the router, from procd: pidof would also count the
# init script (/etc/init.d/cengarde reload runs under that name).
ENGINE_PID="ubus call service list '{\"name\":\"cengarde\"}' | jsonfilter -e '@.cengarde.instances.*.pid'"
engine_pid() {
	"$VM" ssh router "$ENGINE_PID"
}

# The links of the router's engine that are not excluded (cengarde ctl links).
links() {
	"$VM" ssh router 'cengarde ctl links' | grep -E '^eth[123] '
}

# until SECONDS COMMAND...: retries COMMAND every 2 s until it succeeds.
until_ok() {
	local n=$(($1 / 2))

	shift
	while ! "$@"; do
		n=$((n - 1))
		[ "$n" -gt 0 ] || return 1
		sleep 2
	done
}

# All three links live toward REMOTE.
all_on() {
	[ "$(links | grep -c " live .* $1 ")" = 3 ]
}

# The LAN holds the prefix delegated through up1v6.
lan_prefix() {
	"$VM" ssh router "ubus call network.interface.lan status |
		jsonfilter -e '@[\"ipv6-prefix-assignment\"][*].address'" | grep -q '^2001:db8:100:'
}

mkdir -p "$OUT"
# E2E_KEEP=1 leaves the VMs running at the end, to look around (vm.sh ssh).
[ "${E2E_KEEP:-0}" = 1 ] || trap '"$VM" stop' EXIT
"$VM" start "$IMAGE"
"$VM" wait

say "VPS: uplink ends with DHCP and IPv6, 1.2.3.4, 1.2.3.5, 2001:db8::4, cengarde server and WireGuard"
secret=$("$VM" ssh router 'uci get cengarde.main.secret')
"$VM" ssh vps "SECRET='$secret' sh -s" <<'EOF'
set -e
uci -q delete network.wan || true
uci -q delete network.wan6 || true
# The pool up1 delegates prefixes from, as a carrier does.
uci set network.globals.ula_prefix=2001:db8:100::/48
for i in 1 2 3; do
	uci set network.up$i=interface
	uci set network.up$i.proto=static
	uci set network.up$i.device=eth$i
	uci set network.up$i.ipaddr=10.1.$i.1
	uci set network.up$i.netmask=255.255.255.0
	# A global prefix and a ULA, which the engine must not send from.
	uci add_list network.up$i.ip6addr=2001:db8:$i::1/64
	uci add_list network.up$i.ip6addr=fd00:$i::1/64
	uci set dhcp.up$i=dhcp
	uci set dhcp.up$i.interface=up$i
	uci set dhcp.up$i.start=100
	uci set dhcp.up$i.limit=10
	uci set dhcp.up$i.ra=server
	# A default route in the advertisements, although the VPS has no IPv6
	# upstream of its own.
	uci set dhcp.up$i.ra_default=1
done
uci set network.up1.ip6assign=56
uci set dhcp.up1.dhcpv6=server
uci set dhcp.up1.dhcpv6_pd_min_len=60
uci set dhcp.up3.ra_mtu=1400
uci set network.public=interface
uci set network.public.proto=static
uci set network.public.device=lo
uci add_list network.public.ipaddr=1.2.3.4/32
uci add_list network.public.ipaddr=1.2.3.5/32
uci add_list network.public.ip6addr=2001:db8::4/128
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
# Every address of the VPS: replies leave from the one each packet came to.
cat > /etc/cengarde/server.conf <<EOC
mode = server
key = $CG_LINK_KEY
listen = *:65500
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
/etc/init.d/odhcpd restart
# An uplink taken down below keeps its IPv6 addresses, as a carrier's end
# does when the modem drops; netifd would not put them back.
for i in 1 2 3; do
	sysctl -qw net.ipv6.conf.eth$i.keep_addr_on_down=1
done
/etc/init.d/cengarde restart
EOF

say "router: three DHCP uplinks instead of wan, and up1v6 asking for a prefix"
"$VM" ssh router sh -s <<'EOF'
set -e
uci -q delete network.wan || true
uci -q delete network.wan6 || true
for i in 1 2 3; do
	uci set network.up$i=interface
	uci set network.up$i.proto=dhcp
	uci set network.up$i.device=eth$i
done
uci set network.up1v6=interface
uci set network.up1v6.proto=dhcpv6
uci set network.up1v6.device=@up1
uci set network.up1v6.reqaddress=try
uci set network.up1v6.reqprefix=auto
uci commit network
/etc/init.d/network reload
EOF
sleep 10
"$VM" ssh router 'uci show network; uci show firewall; ip route; ip -6 route' > "$OUT/router-before.txt"
# odhcp6c asks for the prefix again only at its next renewal when the first
# answer had none (the VPS's odhcpd may still be starting): restart it once.
if until_ok 45 lan_prefix ||
	{ say "no prefix yet: up1v6 asks again"; "$VM" ssh router 'ifup up1v6'; until_ok 45 lan_prefix; }; then
	ok "without cengarde the LAN gets the prefix delegated through up1v6"
else
	bad "no prefix delegated to the LAN: the leak test below proves nothing"
	"$VM" ssh router 'ifstatus up1v6; logread | grep -E "odhcp6c|netifd" | tail -20'
	"$VM" ssh vps 'ubus call dhcp ipv6leases; logread | grep odhcpd | tail -20'
fi

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
for i in 2 3; do
	[ "$(uci -q get network.up${i}6.device) $(uci -q get network.up${i}6.delegate)" = "eth$i 0" ] || exit 1
	uci show firewall | grep -q "cengarde_network6=.*'up${i}6'" || exit 1
	# not on a DHCP uplink: netifd would restart its DHCP client
	[ -z "$(uci -q get network.up$i.delegate)" ] || exit 1
done
# up1 has an IPv6 interface already: cengarde adds none and changes that one.
! uci -q get network.up16 >/dev/null &&
	[ "$(uci -q get network.up1v6.delegate) $(uci -q get network.up1v6.cengarde_delegate)" = "0 1" ]
EOF
then
	ok "IPv6 companions up26, up36 on the uplinks' devices, in their zone, delegate 0 on up1v6"
else
	bad "IPv6 companions"
fi
if until_ok 30 eval '! lan_prefix'; then
	ok "IPv6 leak closed: the LAN no longer holds the prefix of up1v6"
else
	bad "IPv6 leak: the LAN still holds the prefix of up1v6"
fi
if "$VM" ssh router 'cengarde-setup status' | grep -q '"code":"ipv6_leak"'; then
	bad "the status reports an IPv6 leak"
else
	ok "the status reports no IPv6 leak"
fi

# No PPPoE server here: only the configuration. PPP copies the uplink's
# delegate onto the IPv6 interface it creates (ppp9_6), so the uplink gets
# delegate 0 while everything goes through the tunnel, and loses it after.
if "$VM" ssh router sh -s <<'EOF'
set -e
uci set network.ppp9=interface
uci set network.ppp9.proto=pppoe
uci set network.ppp9.device=eth9
uci set network.ppp9.auto=0
uci commit network
uci add_list cengarde.main.uplink=ppp9
uci commit cengarde
cengarde-setup apply
on="$(uci -q get network.ppp9.delegate) $(uci -q get network.ppp9.cengarde_delegate)"
uci del_list cengarde.main.uplink=ppp9
uci commit cengarde
cengarde-setup apply
off=$(uci -q get network.ppp9.delegate || echo unset)
for z in $(uci show firewall | sed -n "s/^firewall\.\([^.]*\)\.network=.*'ppp9'.*/\1/p"); do
	uci del_list "firewall.$z.network=ppp9"
done
uci delete network.ppp9
uci commit firewall
uci commit network
/etc/init.d/network reload
/etc/init.d/firewall reload >/dev/null 2>&1
[ "$on $off" = "0 1 unset" ]
EOF
then
	ok "a PPPoE uplink gets delegate 0 while all goes through the tunnel"
else
	bad "PPPoE uplink: no delegate 0, or left behind"
fi

if [ "$("$VM" ssh vps 'cat /var/run/cengarde/passthrough')" = on ]; then
	ok "IP pass: the VPS got the router's request"
else
	bad "IP pass: the VPS has no request from the router"
fi

say "IP pass off while pinging: applied in place, no restart, no loss"
pid=$(engine_pid)
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
if grep -q ' 0% packet loss' "$OUT/ping-reload.txt" && [ -n "$pid" ] && [ "$(engine_pid)" = "$pid" ] &&
	"$VM" ssh router 'logread | grep -q "reload: configuration applied"'; then
	ok "the change went in without restarting the engine or losing a packet"
else
	bad "the change restarted the engine or lost packets"
	"$VM" ssh router "echo before: $pid, now: \$($ENGINE_PID); logread | grep -E 'cengarde|procd' | tail -20"
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

say "IPv6: 2001:db8::4 first in the list"
"$VM" ssh router sh -s <<'EOF'
uci delete cengarde.main.server
for a in 2001:db8::4 1.2.3.4 1.2.3.5; do
	uci add_list cengarde.main.server=$a
done
uci commit cengarde
ubus call service event '{"type":"config.change","data":{"package":"cengarde"}}'
EOF
if until_ok 40 all_on '\[2001:db8::4\]:65500'; then
	ok "every link went to the first address of the new list, over IPv6"
else
	bad "not every link is live on [2001:db8::4]"
	"$VM" ssh router 'ip -6 addr; ip -6 route; logread | grep cengarde | tail -20'
	"$VM" ssh vps 'ip -6 addr; ip -6 route'
fi
links | tee "$OUT/ctl-links6.txt"
if [ "$(grep -cE ' \[2001:db8:[0-9a-f:]+\]:[0-9]+ +\[2001:db8::4\]:65500' "$OUT/ctl-links6.txt")" = 3 ]; then
	ok "each link sends from its global address, not from the ULA"
else
	bad "a link sends from an address that is not its global one"
fi
if [ "$("$VM" ssh vps 'cengarde ctl links' | grep -c ' live .*\[2001:db8::4\]:65500$')" = 3 ]; then
	ok "the VPS answers each path from [2001:db8::4] (LOCAL)"
else
	bad "the VPS LOCAL column"
	"$VM" ssh vps 'cengarde ctl links'
fi
# netifd's source routes take each socket out through its own uplink.
if "$VM" ssh router sh -s <<'EOF'
for i in 1 2 3; do
	src=$(cengarde ctl links | sed -n "s/^eth$i .* \[\(2001:db8:[0-9a-f:]*\)\]:[0-9]*  *\[2001:db8::4\].*/\1/p")
	[ -n "$src" ] || exit 1
	ip -6 route get 2001:db8::4 from "$src" | grep -q " dev eth$i " || exit 1
done
EOF
then
	ok "the route from each link's address leaves through its uplink"
else
	bad "a link's address routes out through another uplink"
fi

say "IPv6: the path MTU of up3 (1400 in its router advertisements)"
"$VM" ssh router 'ping -q -c 8 -s 1340 10.79.0.1' > "$OUT/ping-mtu.txt" 2>&1 || true
"$VM" ssh router 'cengarde-setup status' > "$OUT/status6.json"
if grep -q '"code":"path_mtu","iface":"up3","mtu":1400' "$OUT/status6.json" &&
	! grep -q '"code":"path_mtu","iface":"up[12]"' "$OUT/status6.json"; then
	ok "the status reports the path MTU of up3 only"
else
	bad "path MTU in the status"
fi
if until_ok 20 "$VM" ssh router 'logread | grep -q "link eth3: WireGuard datagrams of .* path MTU of 1400"'; then
	ok "the engine warns about the path MTU of eth3"
else
	bad "no path MTU warning for eth3"
fi

say "IPv6: blocked on up2 while pinging, only up2 moves to IPv4"
"$VM" ssh router 'ping -q -c 25 10.79.0.1' > "$OUT/ping-block6.txt" 2>&1 &
ping_pid=$!
sleep 2
"$VM" ssh vps "nft add table inet cgtest && nft add chain inet cgtest in '{ type filter hook input priority -10; }' &&
	nft add rule inet cgtest in iifname eth2 meta nfproto ipv6 udp dport 65500 drop"
wait "$ping_pid" || true
cat "$OUT/ping-block6.txt"
links | tee "$OUT/ctl-links-block6.txt"
if grep -q '^eth2 .* live .* 1\.2\.3\.4:65500 (2/3)' "$OUT/ctl-links-block6.txt" &&
	[ "$(grep -c ' live .*\[2001:db8::4\]:65500' "$OUT/ctl-links-block6.txt")" = 2 ] &&
	grep -q ' 0% packet loss' "$OUT/ping-block6.txt"; then
	ok "up2 failed over to 1.2.3.4, the others stayed on IPv6, no loss"
else
	bad "failover with IPv6 blocked on up2"
fi
"$VM" ssh vps 'nft delete table inet cgtest'

say "IPv6: 2001:db8::4 removed from the VPS, every link moves to 1.2.3.4"
"$VM" ssh vps 'ip -6 addr del 2001:db8::4/128 dev lo'
if until_ok 30 all_on '1\.2\.3\.4:65500'; then
	ok "all links on 1.2.3.4"
else
	bad "not every link moved to 1.2.3.4"
	links
fi
"$VM" ssh vps 'ip -6 addr add 2001:db8::4/128 dev lo'
if "$VM" ssh router 'ping -q -c 3 10.79.0.1' >/dev/null 2>&1; then
	ok "the tunnel works over IPv4"
else
	bad "no tunnel after moving to 1.2.3.4"
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
leftover=$("$VM" ssh router "$ENGINE_PID"'; uci show network | grep -E "wgcg|cengarde|up[123]6"; uci show firewall | grep -E "cengarde|up[123]6"; ip route | grep wgcg; uci -q get upnpd.config.external_iface' || true)
if [ -z "$leftover" ]; then
	ok "nothing left of the tunnel"
else
	bad "left after disabling: $leftover"
	"$VM" ssh router 'ps w | grep [c]engarde; logread | grep -E "cengarde|procd" | tail -20'
fi
if "$VM" ssh router 'uci show network | grep -qE "peerdns|delegate"'; then
	bad "peerdns or delegate still set on an uplink"
else
	ok "uplinks announce their DNS servers again"
fi
if until_ok 30 lan_prefix; then
	ok "the LAN gets the prefix of up1v6 again"
else
	bad "the LAN did not get the prefix of up1v6 back"
fi
converged "disabled"

if [ "$fails" -eq 0 ]; then
	say "all passed"
else
	say "$fails failed"
	exit 1
fi
