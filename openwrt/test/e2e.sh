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
#     (*), with WireGuard; all keys derive from the router's pairing secret,
#     and so does the router's address in the tunnel (10.79.x.y, a /32, with
#     10.79.0.1/32 on the VPS side) and the ULA prefix of the IPv6 inside it
#     (fdXX:XXXX:XXXX::2/128 on the router, ::1/128 on the VPS side);
#  3. configures cengarde on the router from LuCI only (luci.mjs), which
#     saves and applies once with the split on and two weights (the engine's
#     configuration carries them) and again with it off, then
#     waits for the tunnel, the three uplinks and IP pass on at the VPS,
#     and pauses and resumes an uplink from the status page; up2 and up3
#     get their DHCPv6 companions (up26, up36) on their devices, in the
#     uplinks' zone, up1v6 stops handing its prefix to the LAN, and a PPPoE
#     uplink (configuration only) gets delegate 0;
#     wgcg has the derived address as a /32 and a route to 10.79.0.1; the
#     status and "cengarde-setup cloud-config" carry the router's name and
#     address, and an invalid tunnel_addr is refused; the engine
#     configuration gives each link the DNS server its uplink got by DHCP
#     (the VPS's dnsmasq, 10.1.N.1) as its resolver, and carries the
#     reordering and split options and the weight of each uplink (invalid
#     ones left out), in a file the engine takes;
#  4. turns IP pass off while pinging through the tunnel: the engine takes
#     the change without a restart and without loss, and the VPS follows;
#  5. pings through the tunnel while one uplink goes down: no loss allowed;
#  6. IPv6 to the VPS: with 2001:db8::4 first in the list every link sends
#     from its global address (not the ULA); the path MTU of up3 shows in
#     the status and the log; with IPv6 blocked on up2 only up2 moves to
#     1.2.3.4, without loss; without 2001:db8::4 they all move;
#  7. restarts the router's engine and WireGuard with its clock an hour
#     behind, as a router without a battery-backed clock comes back: the VPS
#     ignores its handshakes, so the tunnel comes back only because the VPS
#     engine gets a handshake WireGuard starts to the router (it pokes it
#     with wireguard_poke when it has nothing to send);
#  8. link threads: link_threads off (the per-link structure in one
#     thread), then on (a thread per uplink, musl's threads), then legacy
#     again: the engine restarts in place each time, every link comes back
#     live, pings get through without loss, and with on "cengarde ctl
#     threads" shows the hub and a thread per uplink with their CPU (from
#     their CPU clocks: OpenWrt's kernel has no schedstat);
#  9. checks that apply has converged: run again it changes nothing, and
#     nothing reloads for 60 s (the companions' triggers cause no loop);
# 10. a VPS name that only the uplinks' DNS can answer (dnsmasq at the VPS
#     end of each uplink): the engine looks it up itself, through its links
#     and not through the tunnel, brings every link up on its address, and
#     follows it when the address changes and the old one goes away; a name
#     nobody knows is a problem in the status; the IP list back;
# 11. route_all off: 10.79.0.1 stays reachable through wgcg by the route
#     cengarde keeps (the /1 routes are gone, and a /32 has no connected
#     one), converged again; route_all back on;
# 12. IPv6 in the tunnel (tunnel_ipv6 on): wgcg holds the ULA address derived
#     from the secret, a route6 leads to the VPS end, which answers a ping, the
#     NAT66 rule is in the firewall, the LAN's odhcpd is told to announce a
#     default route, and the status and the cloud-config say so; converged;
#     off again: all of it gone, as it was, and converged;
# 13. the native tunnel: the VPS's server goes to tunnel = native (wg0
#     down, the engine's own TUN cg0 with 10.79.0.1/16) and the router to
#     tunnel_mode native: wgcg and its peer give way to a static interface
#     cg0 on the engine's TUN with the derived address, the route to
#     10.79.0.1 and the /1 routes, in the wan zone; the engine runs tunnel =
#     native with that address; 1.2.3.5 answers through cg0 without loss;
#     the status, the engine and the cloud-config say native; after a
#     restart of the engine netifd puts cg0 back with its address and routes;
#     converged; IPv6 in it (the ULA on cg0 and in tun_addr, ::/1 and
#     8000::/1, NAT66, the VPS answers) and off again; then both ends back to
#     WireGuard: wgcg as before, nothing left of cg0, converged;
# 14. disables cengarde and checks that the router is back as it was (the
#     LAN gets its prefix again), and converged again.
# Screenshots and logs go to OUT_DIR (default ./e2e-out). Needs qemu-system-x86,
# ssh and node with "npm install" done in this directory.
#
# SPDX-License-Identifier: GPL-3.0-or-later
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

# Log lines that a reload loop would keep adding since the marker MARK:
# cengarde-setup changing the configuration, and netifd taking interfaces up
# or down. Counted after a marker of its own, not as a total: once logd's ring
# buffer is full, old lines leave it and a total can go down.
churn() {
	"$VM" ssh router "logread | awk '/e2e-mark: $1\$/ { n = 0; on = 1; next }
		on && /cengarde-setup: updated:|netifd: Interface .* is now (up|down)/ { n++ } END { print n + 0 }'"
}

# converged WHAT: "cengarde-setup apply" changes nothing more, and in the
# next 60 s nothing reloads.
CONVERGED=0
converged() {
	local mark n

	CONVERGED=$((CONVERGED + 1))
	mark=converged-$CONVERGED
	"$VM" ssh router "logger -t e2e-mark $mark; cengarde-setup apply"
	sleep 60
	n=$(churn "$mark")
	if [ "$n" = 0 ]; then
		ok "$1: apply changes nothing, no reloads in 60 s"
	else
		bad "$1: $n new updates or interface changes"
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

# All three links live, whichever VPS address.
all_live() {
	[ "$(links | grep -c ' live ')" = 3 ]
}

# All three links live toward REMOTE, which may be the only candidate (the
# table shows "(n/m)" after it only when there are several).
all_to() {
	[ "$(links | grep -cE " live .* $1( |\$)")" = 3 ]
}

# The link_threads mode the engine runs.
lt_mode() {
	"$VM" ssh router "cengarde ctl status | jsonfilter -e '@.threads.mode'"
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
# The router's address in the tunnel: a new install has no tunnel_addr, so it
# derives it from the secret.
keys=$("$VM" ssh router "echo '$secret' | cengarde keys")
addr=$(echo "$keys" | sed -n "s/^CG_TUNNEL_ADDR='\(.*\)'\$/\1/p")
case "$addr" in
10.79.*) ;;
*)
	echo "e2e: no tunnel address derived from the router's secret ($addr)"
	exit 1
	;;
esac
# And the ULA /48 prefix of the IPv6 in the tunnel: the router is ::2 in it and
# the VPS ::1.
ula=$(echo "$keys" | sed -n "s/^CG_TUNNEL_ULA='\(.*\)'\$/\1/p")
case "$ula" in
fd??:????:????) ;;
*)
	echo "e2e: no ULA prefix derived from the router's secret ($ula)"
	exit 1
	;;
esac
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
uci add_list network.wg0.addresses=10.79.0.1/32
uci add_list network.wg0.addresses=${CG_TUNNEL_ULA}::1/128
uci set network.router=wireguard_wg0
uci set network.router.public_key="$(echo "$CG_WG_CLIENT_KEY" | wg pubkey)"
uci set network.router.preshared_key="$CG_WG_PSK"
# The router's addresses alone, with their routes: a /32 and a /128 have no
# connected one.
uci add_list network.router.allowed_ips=$CG_TUNNEL_ADDR/32
uci add_list network.router.allowed_ips=${CG_TUNNEL_ULA}::2/128
uci set network.router.route_allowed_ips=1
uci commit network
uci commit dhcp
mkdir -p /etc/cengarde
# Every address of the VPS: replies leave from the one each packet came to.
cat > /etc/cengarde/server.conf <<EOC
mode = server
key = $CG_LINK_KEY
listen = *:65500
wireguard = 127.0.0.1:65501
wireguard_poke = $CG_TUNNEL_ADDR:9
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
# up1 and up1v6 in the wan zone, as wan and wan6 are on a stock router: the
# DHCPv6 answers come in through its rules. Without a zone they only came in
# through the rules wan left on eth1, until the firewall next reloaded. up2
# and up3 stay without a zone: cengarde-setup puts them in it.
zone=$(uci show firewall | sed -n "s/^\(firewall\.[^.]*\)\.name='wan'$/\1/p")
uci add_list "$zone.network=up1"
uci add_list "$zone.network=up1v6"
uci commit network
uci commit firewall
/etc/init.d/network reload
/etc/init.d/firewall reload >/dev/null 2>&1
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

# A new install derives its address from the secret; wgcg holds it as a /32,
# and the route to the VPS end is the one cengarde keeps (with a /32 there is
# no connected route).
if [ -z "$("$VM" ssh router 'uci -q get cengarde.main.tunnel_addr')" ] &&
	[ "$("$VM" ssh router 'uci -q get network.wgcg.addresses')" = "$addr/32" ] &&
	"$VM" ssh router "ip -4 addr show dev wgcg | grep -q 'inet $addr/32 ' &&
		ip route | grep -q '^10\.79\.0\.1 dev wgcg' &&
		uci -q get network.cengarde_vps_route.target | grep -qx 10.79.0.1/32"; then
	ok "wgcg has the address derived from the secret ($addr/32) and a route to 10.79.0.1"
else
	bad "wgcg does not have $addr/32 and a route to 10.79.0.1"
	"$VM" ssh router 'uci show cengarde.main; ip addr show dev wgcg; ip route'
fi
"$VM" ssh router 'cengarde-setup status' > "$OUT/status-addr.json"
cloud=$("$VM" ssh router 'cengarde-setup cloud-config')
if grep -q "\"tunnel_addr\":\"$addr\"" "$OUT/status-addr.json" &&
	grep -q "\"tunnel_addr_derived\":\"$addr\"" "$OUT/status-addr.json" &&
	grep -q '"vps_name":"router"' "$OUT/status-addr.json" &&
	printf '%s\n' "$cloud" | grep -q '^ *ROUTER_NAME=router$' &&
	printf '%s\n' "$cloud" | grep -q "^ *ROUTER_TUNNEL_ADDR=$addr\$"; then
	ok "the status and the cloud-config carry the router's name and address"
else
	bad "the status or the cloud-config lack the router's name or address"
	cat "$OUT/status-addr.json"
	printf '%s\n' "$cloud" | grep ROUTER_ || true
fi
# An address that is not valid is refused, not half applied.
if "$VM" ssh router sh -s <<'EOF'
uci set cengarde.main.tunnel_addr=10.79.0.1
uci commit cengarde
r=0
cengarde-setup cloud-config >/dev/null 2>&1 && r=1
cengarde-setup status | grep -q '"tunnel_addr":null' || r=1
cengarde-setup status | grep -q '"code":"tunnel_addr_invalid"' || r=1
uci delete cengarde.main.tunnel_addr
uci commit cengarde
exit $r
EOF
then
	ok "tunnel_addr 10.79.0.1 (the VPS's) is refused: no cloud-config, a problem in the status"
else
	bad "tunnel_addr 10.79.0.1 was not refused"
fi

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

# The DNS server each uplink got by DHCP is the VPS's dnsmasq, 10.1.N.1. With
# peerdns 0 (set above) netifd keeps it as inactive; the engine configuration
# hands it to the link as its resolver, so that the engine can look up a VPS
# name through that link, and holds nothing global (the engine has a default).
if "$VM" ssh router sh -s <<'EOF'
conf=$(cengarde-setup engine-conf) || exit 1
for i in 1 2 3; do
	echo "$conf" | sed -n "/^\[link eth$i\]/,/^\$/p" | grep -qE "^resolver = (.* )?10\.1\.$i\.1( |\$)" || exit 1
done
! echo "$conf" | sed '/^\[link/,$d' | grep -q '^resolver'
EOF
then
	ok "engine-conf: the resolver of each link is the DNS server of its uplink (10.1.N.1)"
else
	bad "engine-conf: no resolver line with the DNS server of each uplink"
	"$VM" ssh router sh -s <<'EOF'
cengarde-setup engine-conf | grep -v '^key'
for i in up1 up16 up2 up3; do
	ubus call network.interface.$i status | jsonfilter -e '@["dns-server"][*]' -e '@.inactive["dns-server"][*]'
done
EOF
fi

# Reordering and the experimental split: the options of /etc/config/cengarde
# reach the engine's file (the split as "on", never "force"; a weight only in
# the section of an uplink of the router, from 0 to 1000; a type, story 015,
# only in the section of an uplink and one the engine knows, auto written as
# nothing; the entries that are not valid are left out and logged), and the
# engine takes that file. The
# options are cleared first and after, so that nothing else runs with them.
if "$VM" ssh router sh -s <<'EOF'
conf() { cengarde-setup engine-conf; }
section() { conf | sed -n "/^\[link $1\]/,/^\$/p"; }
clear() {
	for o in reorder reorder_max_wait_ms reorder_dup_wait_ms reorder_margin_ms reorder_bypass reorder_pool_kb \
		split split_small split_weight split_spread_ms split_spread_copy governor governor_target_ms \
		capacity_weights split_eta pacing uplink_type; do
		uci -q delete "cengarde.main.$o"
	done
	uci commit cengarde
}
r=0
clear
# nothing set: none of the keys (the engine's defaults rule)
conf | grep -qE '^(reorder|split|weight|type)' && { echo "keys without options"; r=1; }
uci set cengarde.main.reorder=1
uci set cengarde.main.reorder_max_wait_ms=300
uci set cengarde.main.reorder_dup_wait_ms=20
uci set cengarde.main.reorder_bypass=64
uci set cengarde.main.split=1
uci set cengarde.main.split_small=300
uci set cengarde.main.split_spread_ms=50
uci set cengarde.main.split_spread_copy=0
uci set cengarde.main.governor=1
uci set cengarde.main.governor_target_ms=15
uci set cengarde.main.capacity_weights=1
uci set cengarde.main.split_eta=1
uci set cengarde.main.pacing=1
uci add_list cengarde.main.split_weight=up1=3
uci add_list cengarde.main.split_weight=up3=0
uci add_list cengarde.main.split_weight=up9=2
uci add_list cengarde.main.split_weight=up2=1001
uci add_list cengarde.main.uplink_type=up1=starlink
uci add_list cengarde.main.uplink_type=up2=cellular
uci add_list cengarde.main.uplink_type=up3=auto
uci add_list cengarde.main.uplink_type=up9=radio
uci add_list cengarde.main.uplink_type=up3=satellite
uci commit cengarde
for kv in 'reorder = yes' 'reorder_max_wait_ms = 300' 'reorder_dup_wait_ms = 20' 'reorder_bypass = 64' 'split = on' \
	'split_small = 300' 'split_spread_ms = 50' 'split_spread_copy = no' 'governor = yes' 'governor_target_ms = 15' \
	'capacity_weights = yes' 'split_eta = yes' 'pacing = yes'; do
	conf | grep -qxF "$kv" || { echo "missing: $kv"; r=1; }
done
section eth1 | grep -qx 'weight = 3' || { echo "up1 (eth1) has no weight 3"; r=1; }
section eth3 | grep -qx 'weight = 0' || { echo "up3 (eth3) has no weight 0"; r=1; }
section eth2 | grep -q '^weight' && { echo "up2 (eth2) has a weight"; r=1; }
[ "$(conf | grep -c '^weight')" = 2 ] || { echo "not two weights"; r=1; }
logread | grep -q "split_weight 'up9=2'" || { echo "up9=2 not logged"; r=1; }
logread | grep -q "split_weight 'up2=1001'" || { echo "up2=1001 not logged"; r=1; }
section eth1 | grep -qx 'type = starlink' || { echo "up1 (eth1) is not starlink"; r=1; }
section eth2 | grep -qx 'type = cellular' || { echo "up2 (eth2) is not cellular"; r=1; }
section eth3 | grep -q '^type' && { echo "up3 (eth3) has a type (auto is written as nothing)"; r=1; }
[ "$(conf | grep -c '^type')" = 2 ] || { echo "not two types"; r=1; }
logread | grep -q "uplink_type 'up9=radio'" || { echo "up9=radio not logged"; r=1; }
logread | grep -q "uplink_type 'up3=satellite'" || { echo "up3=satellite not logged"; r=1; }
# the engine's own parser takes the file; its shape is checked too
(umask 077 && conf > /tmp/split.conf)
cengarde -t -c /tmp/split.conf || r=1
rm -f /tmp/split.conf
uci set cengarde.main.reorder=0
uci commit cengarde
conf | grep -qxF 'reorder = no' || { echo "reorder 0 is not no"; r=1; }
uci set cengarde.main.split=0
uci commit cengarde
conf | grep -q '^split = ' && { echo "split 0 is written"; r=1; }
clear
conf | grep -qE '^(reorder|split|weight|type)' && { echo "keys left after clearing"; r=1; }
exit $r
EOF
then
	ok "engine-conf: reordering and split options and per-uplink weights and types reach the engine's file, which it takes"
else
	bad "engine-conf: reordering and split options or weights"
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

say "clock: the router's engine and WireGuard come back with its clock an hour behind"
"$VM" ssh router sh -s <<'EOF'
/etc/init.d/cengarde stop
ifdown wgcg
date -s @$(($(date +%s) - 3600)) >/dev/null
EOF
sleep 5
"$VM" ssh router '/etc/init.d/cengarde start; ifup wgcg'
# WireGuard on the VPS ignores the router's handshakes: the tunnel is back
# only once WireGuard there starts one (on its own when it had something to
# send, or poked by the engine) and the engine gets it to the router's new
# session (redirect or port). Back means a handshake on the new wgcg and a
# ping only wgcg can carry: 10.79.0.1 also answers over an uplink.
tunnel_back() {
	"$VM" ssh router "wg show wgcg latest-handshakes | awk '\$2 > 0 { ok = 1 } END { exit !ok }' &&
		ping -q -c 1 -W 2 -I wgcg 10.79.0.1 >/dev/null 2>&1"
}
if until_ok 90 tunnel_back; then
	ok "tunnel back with the router's clock an hour behind"
else
	bad "no tunnel with the router's clock an hour behind"
fi
"$VM" ssh vps 'logread | grep cengarde | grep -E "poking it|goes down session|takes over the port" | tail -3'
if "$VM" ssh router 'ping -q -c 10 -I wgcg 10.79.0.1' | grep -q ' 0% packet loss'; then
	ok "and it stays up"
else
	bad "the tunnel did not stay up after coming back"
fi
"$VM" ssh router sh -s <<'EOF'
date -s @$(($(date +%s) + 3600)) >/dev/null
EOF

say "link threads: off, on, then legacy again"
for mode in off on legacy; do
	"$VM" ssh router "uci set cengarde.main.link_threads=$mode; uci commit cengarde; /etc/init.d/cengarde reload"
	if until_ok 30 all_live && [ "$(lt_mode)" = "$mode" ] &&
		"$VM" ssh router 'ping -q -c 5 10.79.0.1' > "$OUT/ping-threads-$mode.txt" 2>&1 &&
		grep -q ' 0% packet loss' "$OUT/ping-threads-$mode.txt"; then
		ok "link_threads $mode: every link live, no loss"
	else
		bad "link_threads $mode"
		links
		"$VM" ssh router 'logread | grep cengarde | tail -20'
	fi
	[ "$mode" = on ] || continue
	sleep 3 # two samples of each thread's CPU clock, a second apart
	"$VM" ssh router 'cengarde ctl threads' | tee "$OUT/ctl-threads.txt"
	if [ "$(grep -cE '^cg-(hub|eth[123]) +[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+\.[0-9]$' "$OUT/ctl-threads.txt")" = 4 ]; then
		ok "ctl threads: the hub and a thread per uplink, each with its CPU"
	else
		bad "ctl threads does not show the hub and three uplink threads with their CPU"
	fi
done

converged "enabled"

# A name of the VPS that only the uplinks' DNS can answer: dnsmasq at the VPS
# end of each uplink, which they got by DHCP as their DNS server, is told where
# vps.test is. The engine looks the name up itself, through each of its links
# and not through the tunnel, and follows it: the old address goes away at the
# VPS, the DNS says the new one, and every link moves. A second name that
# nobody knows only shows up as a problem. The list of IPs comes back at the end.
vps_dns() { # [ADDRESS]: where vps.test is, or nowhere (a name dnsmasq answers
	# with NODATA for AAAA, as a DNS does for a name with no IPv6 address; an
	# address= entry would answer NXDOMAIN instead)
	"$VM" ssh vps "uci -q delete dhcp.vpstest
		${1:+uci set dhcp.vpstest=domain; uci set dhcp.vpstest.name=vps.test; uci set dhcp.vpstest.ip=$1}
		uci commit dhcp
		/etc/init.d/dnsmasq restart >/dev/null 2>&1"
}
server_list() { # ADDRESS_OR_NAME...
	"$VM" ssh router "uci -q delete cengarde.main.server
		for a in $*; do uci add_list cengarde.main.server=\$a; done
		uci commit cengarde
		ubus call service event '{\"type\":\"config.change\",\"data\":{\"package\":\"cengarde\"}}'"
}
name_field() { # N FIELD: of names[N] in the engine's status
	"$VM" ssh router "cengarde ctl status | jsonfilter -e '@.names[$1].$2'"
}
unresolved() { # NAME: the status reports it as a problem
	"$VM" ssh router 'cengarde-setup status' | grep -q "\"code\":\"server_unresolved\",\"name\":\"$1\""
}
tunnel_works() { "$VM" ssh router 'ping -q -c 5 -I wgcg 10.79.0.1' | grep -q ' 0% packet loss'; }

say "VPS name: only the uplinks' DNS knows vps.test, and nobody nope.test"
vps_dns 1.2.3.4
server_list vps.test nope.test
if until_ok 90 all_to '1\.2\.3\.4:65500'; then
	ok "every link went to 1.2.3.4, the address its uplink's DNS gives vps.test"
else
	bad "not every link is live on 1.2.3.4 through the name vps.test"
	links
	"$VM" ssh router 'cengarde ctl status | jsonfilter -e "@.names[*]"; logread | grep cengarde | tail -20'
	"$VM" ssh vps 'logread | grep dnsmasq | tail -5'
fi
# The answer came through a link, from its uplink's DNS server (the VPS's end
# of it), and not from a global resolver.
if [ "$(name_field 0 name)" = vps.test ] && [ "$(name_field 0 state)" = ok ] &&
	[ "$(name_field 0 'addresses[*]')" = 1.2.3.4 ] &&
	name_field 0 via | grep -qE '^eth[123] (10\.1\.[123]\.1|\[(2001:db8|fd00)[0-9a-f:]*\]):53$'; then
	ok "the status: vps.test is ok at 1.2.3.4, answered through a link by its uplink's DNS server"
else
	bad "the status does not show vps.test at 1.2.3.4 from an uplink's DNS server"
	"$VM" ssh router 'cengarde ctl status | jsonfilter -e "@.names[*]"' || true
fi
if tunnel_works; then
	ok "the tunnel works through the name"
else
	bad "no tunnel through the name vps.test"
fi
if until_ok 60 unresolved nope.test && ! unresolved vps.test; then
	ok "the status reports nope.test as a problem, and not vps.test"
else
	bad "the status does not report nope.test (and only it) as unresolved"
	"$VM" ssh router 'cengarde-setup status; cengarde ctl status | jsonfilter -e "@.names[*]"' || true
fi
if node "$HERE/luci.mjs" http://127.0.0.1:8080 "$OUT/screenshots-names" names; then
	ok "LuCI status: the names, their addresses and the problem of nope.test"
else
	bad "LuCI status of the names"
fi

say "VPS name: its address changes (1.2.3.4 goes away, the DNS says 1.2.3.5)"
vps_dns 1.2.3.5
"$VM" ssh vps 'ip addr del 1.2.3.4/32 dev lo'
if until_ok 90 all_to '1\.2\.3\.5:65500' && [ "$(name_field 0 'addresses[*]')" = 1.2.3.5 ]; then
	ok "every link followed vps.test to 1.2.3.5"
else
	bad "the links did not follow vps.test to 1.2.3.5"
	links
	"$VM" ssh router 'cengarde ctl status | jsonfilter -e "@.names[*]"; logread | grep cengarde | tail -20'
fi
if until_ok 30 tunnel_works; then
	ok "the tunnel works after the move"
else
	bad "no tunnel after vps.test moved"
fi

"$VM" ssh vps 'ip addr add 1.2.3.4/32 dev lo'
vps_dns
server_list 2001:db8::4 1.2.3.4 1.2.3.5
if until_ok 60 all_on '\[2001:db8::4\]:65500' && ! unresolved nope.test; then
	ok "the list of IPs back: every link on [2001:db8::4], no name left"
else
	bad "the list of IPs did not bring the links back to [2001:db8::4]"
	links
fi

# Without route_all the /1 routes are gone (cengarde-setup restarts wgcg for
# that: netifd does not look at its peer), and the address is a /32: 10.79.0.1
# is reachable only by the route cengarde keeps. The VPS also answers 10.79.0.1
# over an uplink, so ask the routing table and count the bytes wgcg sends.
say "route_all off: 10.79.0.1 stays reachable through wgcg"
route_all() {
	"$VM" ssh router "uci set cengarde.main.route_all=$1; uci commit cengarde
		ubus call service event '{\"type\":\"config.change\",\"data\":{\"package\":\"cengarde\"}}'"
}
wg_tx() { "$VM" ssh router "wg show wgcg transfer | awk '{ t = \$3 } END { print t + 0 }'"; }
via_wgcg() { "$VM" ssh router "ip route get 10.79.0.1 | grep -q ' dev wgcg '"; }
no_halves() { ! "$VM" ssh router "ip route | grep -q '^0\.0\.0\.0/1 '"; }
route_all 0
if until_ok 60 no_halves && until_ok 60 via_wgcg && until_ok 90 tunnel_back; then
	before=$(wg_tx)
	"$VM" ssh router 'ping -q -c 5 -W 2 10.79.0.1' > "$OUT/ping-route-all-off.txt" 2>&1 || true
	cat "$OUT/ping-route-all-off.txt"
	after=$(wg_tx)
	# five echoes and replies of ~150 bytes each, a keepalive adds 32 at most
	if grep -q ' 0% packet loss' "$OUT/ping-route-all-off.txt" && [ $((after - before)) -ge 500 ]; then
		ok "route_all off: the ping to 10.79.0.1 went through wgcg ($((after - before)) bytes)"
	else
		bad "route_all off: 10.79.0.1 does not answer through wgcg (sent $((after - before)) bytes)"
		"$VM" ssh router 'ip route; wg show wgcg; logread | grep -E "cengarde|netifd" | tail -20'
	fi
else
	bad "route_all off: no route to 10.79.0.1 through wgcg"
	"$VM" ssh router 'uci show network.cengarde_vps_route; ip route; logread | grep -E "cengarde|netifd" | tail -20'
fi
sleep 15 # up1v6 gets its prefix delegation back: let that settle first
converged "route_all off"
route_all 1
halves() { "$VM" ssh router "ip route | grep -q '^0\.0\.0\.0/1 .*wgcg'"; }
if until_ok 60 halves && until_ok 90 tunnel_back && until_ok 30 eval '! lan_prefix'; then
	ok "route_all on again: the /1 routes are back, the tunnel answers, the IPv6 leak is closed"
else
	bad "route_all on again: no /1 routes, no tunnel or the LAN keeps the prefix"
	"$VM" ssh router 'ip route; wg show wgcg; logread | grep -E "cengarde|netifd" | tail -20'
fi

# IPv6 inside the tunnel: the router's ULA address from the secret, a route to the
# VPS end (::1 of the same prefix), NAT66 into wgcg and a default route
# announced to the LAN (which here has the router's own ULA prefix). The VPS has
# the other end of the addresses (wg0). The kernel prints an address its own way
# (no leading zeros, "::" for a run of zeros): ask it with the address as an
# argument, and match what it prints only by the parts that cannot change.
say "IPv6 in the tunnel: on"
tunnel_ipv6() {
	"$VM" ssh router "uci set cengarde.main.tunnel_ipv6=$1; uci commit cengarde
		ubus call service event '{\"type\":\"config.change\",\"data\":{\"package\":\"cengarde\"}}'"
}
wg6_addr() { "$VM" ssh router "ip -6 addr show dev wgcg scope global | grep -Eq 'inet6 ${ula%%:*}:[0-9a-f:]*:2/128'"; }
via6_wgcg() { "$VM" ssh router "ip -6 route get ${ula}::1 | grep -q ' dev wgcg '"; }
nft_masq() { "$VM" ssh router "nft list ruleset | grep -q 'cengarde: IPv6 into the tunnel'"; }
ra_default() { "$VM" ssh router 'uci -q get dhcp.lan.ra_default || echo unset'; }
ra_before=$(ra_default)
ula_prefix=$("$VM" ssh router 'uci -q get network.globals.ula_prefix || true')
tunnel_ipv6 1
if until_ok 90 wg6_addr && until_ok 60 via6_wgcg && until_ok 90 tunnel_back &&
	until_ok 30 "$VM" ssh router "ping -6 -q -c 3 -W 2 ${ula}::1 >/dev/null 2>&1"; then
	"$VM" ssh router "ping -6 -q -c 8 -W 2 ${ula}::1" > "$OUT/ping6-tunnel.txt" 2>&1 || true
	cat "$OUT/ping6-tunnel.txt"
	if grep -q ' 0% packet loss' "$OUT/ping6-tunnel.txt"; then
		ok "tunnel_ipv6 on: ${ula}::2 on wgcg, ${ula}::1 through wgcg, and the VPS answers a ping over IPv6"
	else
		bad "tunnel_ipv6 on: packets lost to ${ula}::1"
	fi
else
	bad "tunnel_ipv6 on: no ${ula}::2 on wgcg, no route to ${ula}::1 through it, or no answer from it"
	"$VM" ssh router 'uci show network.wgcg network.cengarde_vps network.cengarde_vps_route6; ip -6 addr; ip -6 route; wg show wgcg; logread | grep -E "cengarde|netifd" | tail -20'
	"$VM" ssh vps 'ip -6 addr show dev wg0; ip -6 route; wg show'
fi
if [ "$("$VM" ssh router 'uci -q get network.wgcg.addresses')" = "$addr/32 ${ula}::2/128" ] &&
	[ "$("$VM" ssh router 'uci -q get network.cengarde_vps.allowed_ips')" = "0.0.0.0/1 128.0.0.0/1 ::/1 8000::/1" ] &&
	[ "$("$VM" ssh router 'uci -q get network.cengarde_vps_route6.target')" = "${ula}::1/128" ] &&
	[ "$("$VM" ssh router 'uci -q get network.cengarde_vps_route6.interface')" = wgcg ]; then
	ok "tunnel_ipv6 on: the addresses, the peer's allowed IPs and the route6 section are as cengarde-setup keeps them"
else
	bad "tunnel_ipv6 on: wgcg addresses, allowed IPs or route6 section"
	"$VM" ssh router 'uci show network.wgcg network.cengarde_vps network.cengarde_vps_route6'
fi
# With route_all, the /1 halves of IPv6 lead into wgcg too.
if "$VM" ssh router "ip -6 route | grep -q '^::/1 .*wgcg' && ip -6 route | grep -q '^8000::/1 .*wgcg'"; then
	ok "tunnel_ipv6 on: ::/1 and 8000::/1 lead into wgcg (route_all)"
else
	bad "tunnel_ipv6 on: no ::/1 and 8000::/1 routes through wgcg"
	"$VM" ssh router 'ip -6 route'
fi
if until_ok 30 nft_masq; then
	ok "tunnel_ipv6 on: the firewall masquerades IPv6 into the tunnel (fw4 include)"
else
	bad "tunnel_ipv6 on: no NAT66 rule in the firewall"
	"$VM" ssh router 'uci show firewall | grep -A5 cengarde_nat66; nft list chain inet fw4 srcnat; logread | grep -E "firewall|fw4" | tail -10'
fi
if [ "$(ra_default)" = 1 ] && [ "$("$VM" ssh router 'uci -q get dhcp.lan.cengarde_ra_default')" = 1 ] &&
	"$VM" ssh router 'pidof odhcpd >/dev/null'; then
	ok "tunnel_ipv6 on: ra_default 1 on the LAN, marked, and odhcpd runs"
else
	bad "tunnel_ipv6 on: ra_default on the LAN"
	"$VM" ssh router 'uci show dhcp.lan; pidof odhcpd'
fi
"$VM" ssh router 'cengarde-setup status' > "$OUT/status-ipv6.json"
cloud=$("$VM" ssh router 'cengarde-setup cloud-config')
if grep -q '"tunnel_ipv6":true' "$OUT/status-ipv6.json" &&
	grep -q "\"tunnel_ula\":\"${ula}::2\"" "$OUT/status-ipv6.json" &&
	printf '%s\n' "$cloud" | grep -q '^ *TUNNEL_IPV6=yes$'; then
	ok "tunnel_ipv6 on: the status has the ULA address and the cloud-config says TUNNEL_IPV6=yes"
else
	bad "tunnel_ipv6 on: the status or the cloud-config does not say so"
	cat "$OUT/status-ipv6.json"
	printf '%s\n' "$cloud" | grep -E 'TUNNEL_IPV6|PASSTHROUGH=' || true
fi
# The router has the ULA prefix OpenWrt makes on first boot: with it, no problem.
if [ -n "$ula_prefix" ]; then
	if grep -q '"code":"no_ula_prefix"' "$OUT/status-ipv6.json"; then
		bad "tunnel_ipv6 on: the status reports no ULA prefix, and the router has $ula_prefix"
	else
		ok "tunnel_ipv6 on: the router has a ULA prefix ($ula_prefix): no problem reported"
	fi
elif grep -q '"code":"no_ula_prefix"' "$OUT/status-ipv6.json"; then
	ok "tunnel_ipv6 on: the router has no ULA prefix and the status says so"
else
	bad "tunnel_ipv6 on: the router has no ULA prefix and the status does not say so"
fi
sleep 15 # wgcg came back up: let netifd settle first
converged "tunnel_ipv6 on"

say "IPv6 in the tunnel: off again"
tunnel_ipv6 0
if until_ok 90 eval '! wg6_addr' && until_ok 60 eval '! via6_wgcg' && until_ok 30 eval '! nft_masq' &&
	until_ok 90 tunnel_back; then
	ok "tunnel_ipv6 off: no ${ula}::2 on wgcg, no route to ${ula}::1, no NAT66 rule, the tunnel is back"
else
	bad "tunnel_ipv6 off: IPv6 still in the tunnel, or no tunnel"
	"$VM" ssh router 'ip -6 addr show dev wgcg; ip -6 route; nft list chain inet fw4 srcnat; logread | grep -E "cengarde|netifd" | tail -20'
fi
leftover=$("$VM" ssh router 'uci show network | grep -E "route6|::2/128"; uci -q get network.cengarde_vps.allowed_ips | grep -F :;
	uci show firewall | grep cengarde_nat66; uci show dhcp | grep cengarde_ra_default' || true)
if [ -z "$leftover" ] && [ "$(ra_default)" = "$ra_before" ]; then
	ok "tunnel_ipv6 off: no route6 section, no firewall include, ra_default back to $ra_before"
else
	bad "tunnel_ipv6 off: left behind: $leftover (ra_default $(ra_default), it was $ra_before)"
fi
"$VM" ssh router 'cengarde-setup status' > "$OUT/status-ipv6-off.json"
if grep -q '"tunnel_ipv6":false' "$OUT/status-ipv6-off.json" &&
	"$VM" ssh router 'cengarde-setup cloud-config' | grep -q '^ *TUNNEL_IPV6=no$'; then
	ok "tunnel_ipv6 off: the status and the cloud-config say so"
else
	bad "tunnel_ipv6 off: the status or the cloud-config still say on"
fi
sleep 15
converged "tunnel_ipv6 off"

# Native mode: no WireGuard, the engines carry the IP packets through a TUN
# of their own (cg0) on both ends. The VPS runs a server with tunnel = native
# instead (its wg0 down, the engine setting its addresses itself, as no
# netifd interface holds cg0 there); the router only gets tunnel_mode native.
say "native tunnel: the VPS and the router switch from WireGuard to the engine's TUN"
"$VM" ssh vps "SECRET='$secret' sh -s" <<'EOF'
set -e
eval "$(echo "$SECRET" | cengarde keys)"
cp /etc/cengarde/server.conf /etc/cengarde/server-wg.conf
ifdown wg0
cat > /etc/cengarde/server.conf <<EOC
mode = server
key = $CG_LINK_KEY
listen = *:65500
tunnel = native
tun = cg0
tun_addr = 10.79.0.1/16 ${CG_TUNNEL_ULA}::1/64
status_file = /var/run/cengarde.json
control_socket = /var/run/cengarde/cengarde.sock
passthrough_file = /var/run/cengarde/passthrough
EOC
/etc/init.d/cengarde restart
EOF
tunnel_mode() {
	"$VM" ssh router "uci set cengarde.main.tunnel_mode=$1; uci commit cengarde
		ubus call service event '{\"type\":\"config.change\",\"data\":{\"package\":\"cengarde\"}}'"
}
cg0_up() { [ "$("$VM" ssh router "ubus call network.interface.cg0 status 2>/dev/null | jsonfilter -e '@.up' 2>/dev/null")" = true ]; }
native_back() { "$VM" ssh router 'ping -q -c 1 -W 2 -I cg0 10.79.0.1 >/dev/null 2>&1'; }
tunnel_mode native
if until_ok 90 cg0_up && until_ok 90 native_back && until_ok 60 all_live; then
	ok "tunnel_mode native: cg0 up, 10.79.0.1 answers through it, the three links live"
else
	bad "tunnel_mode native: cg0 down, or no answer from 10.79.0.1 through it"
	"$VM" ssh router 'uci show network.cg0; ifstatus cg0; ip addr show dev cg0; ip route; cengarde ctl links; logread | grep -E "cengarde|netifd" | tail -30'
	"$VM" ssh vps 'ip addr show dev cg0; ip route; cengarde ctl links; logread | grep cengarde | tail -20'
fi
"$VM" ssh router 'uci show network; uci show firewall; ip addr; ip route; cat /var/etc/cengarde.conf | grep -v "^key"' \
	> "$OUT/router-native.txt" 2>&1 || true
conf_native=$("$VM" ssh router 'cat /var/etc/cengarde.conf')
if [ "$("$VM" ssh router 'uci -q get network.cg0.proto; uci -q get network.cg0.device; uci -q get network.cg0.ipaddr' | tr '\n' ' ')" = "static cg0 $addr/32 " ] &&
	! "$VM" ssh router 'uci -q get network.wgcg || uci -q get network.cengarde_vps' >/dev/null &&
	[ "$("$VM" ssh router 'uci -q get network.cengarde_vps_route.interface')" = cg0 ] &&
	printf '%s\n' "$conf_native" | grep -qx 'tunnel = native' &&
	printf '%s\n' "$conf_native" | grep -qx "tun_addr = $addr/32" &&
	! printf '%s\n' "$conf_native" | grep -q '^listen'; then
	ok "tunnel_mode native: cg0 is a static interface on the device cg0 with $addr/32, no wgcg nor peer, the engine runs tunnel = native"
else
	bad "tunnel_mode native: the interface or the engine's configuration"
	"$VM" ssh router 'uci show network | grep -E "cg0|wgcg|cengarde"'
	printf '%s\n' "$conf_native" | grep -v '^key'
fi
if "$VM" ssh router "ip -4 addr show dev cg0 | grep -q 'inet $addr/32 ' &&
		ip route | grep -q '^10\.79\.0\.1 dev cg0' && ip route | grep -q '^0\.0\.0\.0/1 dev cg0' &&
		ip route | grep -q '^128\.0\.0\.0/1 dev cg0' && ip route get 1.2.3.5 | grep -q ' dev cg0 '"; then
	ok "tunnel_mode native: $addr/32 on cg0, the route to 10.79.0.1 and the /1 routes through it"
else
	bad "tunnel_mode native: addresses or routes on cg0"
	"$VM" ssh router 'ip addr show dev cg0; ip route'
fi
zone=$("$VM" ssh router "uci show firewall | sed -n \"s/^firewall\.\([^.]*\)\.name='wan'\$/\1/p\"")
if "$VM" ssh router "uci -q get firewall.$zone.network" | tr ' ' '\n' | grep -qx cg0 &&
	! "$VM" ssh router "uci -q get firewall.$zone.network" | tr ' ' '\n' | grep -qx wgcg; then
	ok "tunnel_mode native: cg0 in the wan zone instead of wgcg"
else
	bad "tunnel_mode native: the wan zone has $("$VM" ssh router "uci -q get firewall.$zone.network")"
fi
# Through the /1 routes to an address of the VPS: what the LAN's traffic does.
"$VM" ssh router 'ping -q -c 15 -W 2 1.2.3.5' > "$OUT/ping-native.txt" 2>&1 || true
cat "$OUT/ping-native.txt"
if grep -q ' 0% packet loss' "$OUT/ping-native.txt"; then
	ok "tunnel_mode native: 1.2.3.5 answers through cg0 without loss"
else
	bad "tunnel_mode native: packets lost through cg0"
fi
"$VM" ssh router 'cengarde-setup status' > "$OUT/status-native.json"
if grep -q '"tunnel_mode":"native"' "$OUT/status-native.json" &&
	[ "$("$VM" ssh router "cengarde ctl status | jsonfilter -e '@.tunnel'")" = native ] &&
	"$VM" ssh router 'cengarde-setup cloud-config' | grep -q '^ *TUNNEL=native$'; then
	ok "tunnel_mode native: the status, the engine and the cloud-config say so"
else
	bad "tunnel_mode native: the status, the engine or the cloud-config"
	cat "$OUT/status-native.json"
fi
# The engine restarts: its TUN goes and comes back, and netifd puts cg0's
# addresses and routes back on it with nothing from cengarde-setup.
"$VM" ssh router '/etc/init.d/cengarde restart'
sleep 3
if until_ok 60 cg0_up && until_ok 60 native_back &&
	"$VM" ssh router "ip -4 addr show dev cg0 | grep -q 'inet $addr/32 ' && ip route | grep -q '^0\.0\.0\.0/1 dev cg0'"; then
	ok "tunnel_mode native: after a restart of the engine cg0 is back with its address and routes"
else
	bad "tunnel_mode native: no cg0, address or routes after a restart of the engine"
	"$VM" ssh router 'ifstatus cg0; ip addr show dev cg0; ip route; logread | grep -E "cengarde|netifd" | tail -30'
fi
sleep 15
converged "tunnel_mode native"

say "native tunnel: IPv6 in it"
tunnel_ipv6 1
cg6_addr() { "$VM" ssh router "ip -6 addr show dev cg0 scope global 2>/dev/null | grep -Eq 'inet6 ${ula%%:*}:[0-9a-f:]*:2/128'"; }
if until_ok 90 cg6_addr && until_ok 60 native_back &&
	until_ok 60 "$VM" ssh router "ping -6 -q -c 3 -W 2 ${ula}::1 >/dev/null 2>&1" &&
	"$VM" ssh router "ip -6 route | grep -q '^::/1 dev cg0' && ip -6 route | grep -q '^8000::/1 dev cg0'" &&
	"$VM" ssh router 'cat /var/etc/cengarde.conf' | grep -qx "tun_addr = $addr/32 ${ula}::2/128"; then
	ok "tunnel_mode native, tunnel_ipv6 on: ${ula}::2 on cg0 (and in tun_addr), ::/1 and 8000::/1 through it, the VPS answers over IPv6"
else
	bad "tunnel_mode native, tunnel_ipv6 on: no ULA on cg0, no IPv6 routes through it, or no answer"
	"$VM" ssh router 'uci show network.cg0; ip -6 addr show dev cg0; ip -6 route; grep tun_ /var/etc/cengarde.conf'
fi
if until_ok 30 nft_masq; then
	ok "tunnel_mode native, tunnel_ipv6 on: the NAT66 rule is in the firewall"
else
	bad "tunnel_mode native, tunnel_ipv6 on: no NAT66 rule"
fi
tunnel_ipv6 0
if until_ok 90 eval '! cg6_addr' && until_ok 60 native_back &&
	[ -z "$("$VM" ssh router 'uci show network | grep -E "route6|::2/128"' || true)" ]; then
	ok "tunnel_mode native, tunnel_ipv6 off: no ULA on cg0, no route6 sections, the tunnel answers"
else
	bad "tunnel_mode native, tunnel_ipv6 off: IPv6 left on cg0"
	"$VM" ssh router 'uci show network | grep -E "route6|cg0"; ip -6 addr show dev cg0'
fi

say "native tunnel: back to WireGuard on both ends"
"$VM" ssh vps 'cp /etc/cengarde/server-wg.conf /etc/cengarde/server.conf && ifup wg0 && sleep 2 && /etc/init.d/cengarde restart'
tunnel_mode wireguard
if until_ok 90 tunnel_back && until_ok 60 all_live; then
	ok "tunnel_mode wireguard: the WireGuard tunnel is back on the three links"
else
	bad "tunnel_mode wireguard: no WireGuard tunnel"
	"$VM" ssh router 'uci show network.wgcg; wg show; ip route; logread | grep -E "cengarde|netifd" | tail -30'
fi
leftover=$("$VM" ssh router "uci show network | grep -E 'cg0|cengarde_route6?_all'; ip link show cg0 2>/dev/null;
	uci -q get firewall.$zone.network | tr ' ' '\n' | grep -x cg0; grep -E '^(tunnel|tun|tun_addr|tun_mtu) ' /var/etc/cengarde.conf" || true)
if [ -z "$leftover" ] && [ "$("$VM" ssh router 'uci -q get network.wgcg.addresses')" = "$addr/32" ] &&
	[ "$("$VM" ssh router 'uci -q get network.cengarde_vps_route.interface')" = wgcg ] &&
	"$VM" ssh router "uci -q get firewall.$zone.network" | tr ' ' '\n' | grep -qx wgcg &&
	"$VM" ssh router 'cengarde-setup cloud-config' | grep -q '^ *TUNNEL=wireguard$'; then
	ok "tunnel_mode wireguard: wgcg and its route back, in the wan zone, nothing left of cg0"
else
	bad "tunnel_mode wireguard: left behind: $leftover"
	"$VM" ssh router 'uci show network | grep -E "cg0|wgcg|cengarde"'
fi
sleep 15
converged "tunnel_mode wireguard again"

say "disable: the router goes back to how it was"
"$VM" ssh router sh -s <<'EOF'
uci set cengarde.main.enabled=0
uci commit cengarde
ubus call service event '{"type":"config.change","data":{"package":"cengarde"}}'
EOF
sleep 10
"$VM" ssh router 'uci show network; uci show firewall; ip route' > "$OUT/router-after.txt"
leftover=$("$VM" ssh router "$ENGINE_PID"'; uci show network | grep -E "wgcg|cg0|cengarde|up[123]6"; uci show firewall | grep -E "cengarde|up[123]6"; uci show dhcp | grep cengarde; ip route | grep wgcg; uci -q get upnpd.config.external_iface' || true)
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
