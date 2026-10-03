#!/bin/sh
# Two OpenWrt x86-64 VMs for end-to-end tests, joined by three uplinks:
#
#   router: cengarde client with LuCI. eth0 is its LAN, reachable from here
#           as http://127.0.0.1:8080 and ssh -p 2222 root@127.0.0.1;
#           eth1-eth3 are the uplinks.
#   vps:    plays the VPS. eth0 (LAN) on ssh -p 2223; eth1-eth3 are the far
#           ends of the uplinks, set up by vps_setup in e2e.sh.
#
#   vm.sh start IMAGE     boot both from an ext4-combined image (.img or .img.gz)
#   vm.sh wait            until both answer on SSH
#   vm.sh ssh router|vps [COMMAND...]
#   vm.sh stop
#
# Uses KVM when /dev/kvm is usable, plain emulation otherwise (slower: a
# few minutes to boot). Work files go to $VM_DIR (default ./vm-work).
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu

VM_DIR=${VM_DIR:-$PWD/vm-work}
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"

port_of() { [ "$1" = router ] && echo 2222 || echo 2223; }

accel() {
	if [ -w /dev/kvm ]; then
		echo "-accel kvm -cpu host"
	else
		echo "-accel tcg"
	fi
}

# boot NAME LAN_HOSTFWD UPLINK_LOCAL_BASE UPLINK_REMOTE_BASE MAC_BYTE
boot() {
	name=$1 fwd=$2 lbase=$3 rbase=$4 mac=$5
	# shellcheck disable=SC2046 # two words: -accel and its value
	set -- -name "$name" -m 256 -smp 2 $(accel) \
		-drive "file=$VM_DIR/$name.img,format=raw,if=virtio" \
		-netdev "user,id=lan,net=192.168.1.0/24,host=192.168.1.2,dhcpstart=192.168.1.200,$fwd" \
		-device "virtio-net-pci,netdev=lan,mac=52:54:00:00:$mac:00"
	for i in 1 2 3; do
		set -- "$@" \
			-netdev "dgram,id=up$i,local.type=inet,local.host=127.0.0.1,local.port=$((lbase + i)),remote.type=inet,remote.host=127.0.0.1,remote.port=$((rbase + i))" \
			-device "virtio-net-pci,netdev=up$i,mac=52:54:00:00:$mac:0$i"
	done
	qemu-system-x86_64 "$@" -display none -serial "file:$VM_DIR/$name.console" \
		-pidfile "$VM_DIR/$name.pid" -daemonize
}

start() {
	[ -n "${1:-}" ] || { echo "usage: $0 start IMAGE" >&2; exit 2; }
	mkdir -p "$VM_DIR"
	for name in router vps; do
		case "$1" in
		*.gz) gunzip -c "$1" > "$VM_DIR/$name.img" 2>/dev/null || [ -s "$VM_DIR/$name.img" ] ;;
		*) cp "$1" "$VM_DIR/$name.img" ;;
		esac
	done
	boot router "hostfwd=tcp:127.0.0.1:8080-192.168.1.1:80,hostfwd=tcp:127.0.0.1:2222-192.168.1.1:22" 20010 20020 01
	boot vps "hostfwd=tcp:127.0.0.1:2223-192.168.1.1:22" 20020 20010 02
	echo "booting; consoles in $VM_DIR/*.console"
}

vssh() {
	name=$1
	shift
	# shellcheck disable=SC2086
	ssh $SSH_OPTS -p "$(port_of "$name")" root@127.0.0.1 "$@"
}

wait_up() {
	for name in router vps; do
		i=0
		until vssh "$name" true 2>/dev/null; do
			i=$((i + 1))
			[ "$i" -lt 120 ] || { echo "$name: no SSH after 10 minutes" >&2; exit 1; }
			sleep 5
		done
		# wait until procd has run every boot script
		i=0
		until vssh "$name" 'ubus call system board >/dev/null && [ ! -e /etc/uci-defaults/90-cengarde ]' 2>/dev/null; do
			i=$((i + 1))
			[ "$i" -lt 60 ] || break
			sleep 5
		done
		echo "$name: up"
	done
}

stop() {
	for name in router vps; do
		if [ -f "$VM_DIR/$name.pid" ]; then
			kill "$(cat "$VM_DIR/$name.pid")" 2>/dev/null || true
		fi
		rm -f "$VM_DIR/$name.pid"
	done
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case "$cmd" in
start) start "$@" ;;
wait) wait_up ;;
ssh) vssh "$@" ;;
stop) stop ;;
*)
	echo "usage: $0 start IMAGE | wait | ssh router|vps [COMMAND] | stop" >&2
	exit 2
	;;
esac
