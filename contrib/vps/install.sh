#!/bin/sh
# Builds and installs cengarde on a VPS from this checkout and (re)starts
# the server. The cloud-config runs it at first boot; to upgrade, update the
# checkout (to the same commit as the router's package) and run it again.
#
# Needs build-essential, wireguard-tools and iptables, and the pairing
# secret in /etc/cengarde/secret (see cloud-config.yaml).
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu

SRC=$(cd "$(dirname "$0")/../.." && pwd)

make -C "$SRC/engine"
make -C "$SRC/engine" install PREFIX=/usr/local
install -m 0755 "$SRC/contrib/vps/cengarde-nat" "$SRC/contrib/vps/cengarde-vps-setup" /usr/local/sbin/
install -m 0644 "$SRC/contrib/systemd/cengarde.service" /etc/systemd/system/cengarde.service
# On a VPS the engine is kept away from the metadata service (the secret).
install -d -m 0755 /etc/systemd/system/cengarde.service.d
install -m 0644 "$SRC/contrib/vps/cengarde-vps.conf" /etc/systemd/system/cengarde.service.d/vps.conf
install -m 0644 "$SRC/contrib/vps/cengarde-passthrough.path" "$SRC/contrib/vps/cengarde-passthrough.service" \
	/etc/systemd/system/
install -m 0644 "$SRC/contrib/vps/sysctl.conf" /etc/sysctl.d/90-cengarde.conf
sysctl -q --system
/usr/local/sbin/cengarde-vps-setup
systemctl daemon-reload
systemctl enable wg-quick@wg0 cengarde cengarde-passthrough.path
systemctl restart wg-quick@wg0 cengarde cengarde-passthrough.path
echo "cengarde: $(/usr/local/sbin/cengarde version) listening; cengarde ctl status, or /run/cengarde/status.json"
