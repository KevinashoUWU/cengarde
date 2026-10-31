#!/bin/sh
# Builds this checkout for an OpenWrt router on this PC, with the official
# OpenWrt SDK (as the OpenWrt workflow does), and installs it there over SSH:
# minutes of CI for a test in between become seconds.
#
#   bench/nucpush.sh quick [HOST]   the engine alone (the SDK's musl
#                                   toolchain), cengarde-setup, the init
#                                   script and LuCI's views, copied over the
#                                   installed package; the engine restarts
#   bench/nucpush.sh apk [HOST]     the cengarde and luci-app-cengarde
#                                   packages, built as CI builds them (their
#                                   dependencies the first time only, ~10
#                                   min; then ~15 s), then apk add on the
#                                   router
#   bench/nucpush.sh build          quick's engine only, no router
#
# HOST: the router's SSH target (default the testnet NUC, root@192.168.50.1).
# The SDK (25.12.5, x86/64 unless SDK_TARGET=bcm27xx/bcm2711 for a Pi 4) is
# fetched once, checked against OpenWrt's sha256sums, into
# ~/.cache/cengarde-sdk. The final checks stay CI's.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

MODE=${1:-}
HOST=${2:-root@192.168.50.1}
REL=${SDK_RELEASE:-25.12.5}
TARGET=${SDK_TARGET:-x86/64}
SRC=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${CENGARDE_SDK_CACHE:-$HOME/.cache/cengarde-sdk}/$REL-$(echo "$TARGET" | tr / _)
SDK=$CACHE/sdk
OUT=$CACHE/out
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 ${NUCPUSH_SSH_OPTS:--o HostKeyAlias=10.79.173.140}"

die() {
	echo "nucpush: $*" >&2
	exit 1
}

sdk() {
	[ -x "$SDK/scripts/feeds" ] && return 0
	base=https://downloads.openwrt.org/releases/$REL/targets/$TARGET
	mkdir -p "$CACHE"
	curl -fsS "$base/sha256sums" -o "$CACHE/sums"
	line=$(grep ' \*openwrt-sdk-.*\.tar\.zst$' "$CACHE/sums") || die "no SDK in $base"
	name=${line##*\*}
	echo "nucpush: fetching $name"
	curl -fsS -o "$CACHE/sdk.tar.zst" "$base/$name"
	(cd "$CACHE" && echo "${line%% *}  sdk.tar.zst" | sha256sum -c --quiet) || die "the SDK's checksum does not match"
	mkdir -p "$SDK"
	tar --zstd -xf "$CACHE/sdk.tar.zst" -C "$SDK" --strip-components=1
	rm -f "$CACHE/sdk.tar.zst"
}

version() {
	v=$(sed -n 's/^PKG_VERSION:=//p' "$SRC/openwrt/cengarde/Makefile")
	echo "$v-dev-g$(git -C "$SRC" rev-parse --short HEAD)$(git -C "$SRC" diff --quiet HEAD -- engine || echo +)"
}

# The engine with the SDK's toolchain, in a copy (the checkout's objects stay
# the PC's own).
engine() {
	sdk
	export STAGING_DIR="$SDK/staging_dir"
	tc=$(echo "$STAGING_DIR"/toolchain-*)
	cc=$(echo "$tc"/bin/*-openwrt-linux-musl-gcc)
	mkdir -p "$OUT"
	rsync -a --delete --exclude '*.o' --exclude cengarde --exclude cengarde-test "$SRC/engine/" "$OUT/engine/"
	make -s -C "$OUT/engine" -j"$(nproc)" CC="$cc" CXX="${cc%gcc}g++" CFLAGS="-Os -pipe" CXXFLAGS="-Os -pipe" \
		HARDEN= HARDEN_LDFLAGS= VERSION="$(version)" cengarde
	"${cc%gcc}strip" -o "$OUT/cengarde" "$OUT/engine/cengarde"
	echo "nucpush: built $(version) ($(du -h "$OUT/cengarde" | cut -f1))"
}

# put LOCAL REMOTE MODE: a file onto the router, atomically.
put() {
	$SSH "$HOST" "cat > '$2.new' && chmod $3 '$2.new' && mv '$2.new' '$2'" <"$1"
}

quick() {
	engine
	put "$OUT/cengarde" /usr/sbin/cengarde 755
	put "$SRC/openwrt/cengarde/files/cengarde-setup" /usr/sbin/cengarde-setup 755
	put "$SRC/openwrt/cengarde/files/cengarde.init" /etc/init.d/cengarde 755
	for v in "$SRC"/openwrt/luci-app-cengarde/htdocs/luci-static/resources/view/cengarde/*.js; do
		put "$v" "/www/luci-static/resources/view/cengarde/$(basename "$v")" 644
	done
	put "$SRC/openwrt/luci-app-cengarde/root/usr/share/rpcd/acl.d/luci-app-cengarde.json" \
		/usr/share/rpcd/acl.d/luci-app-cengarde.json 644
	$SSH "$HOST" '/etc/init.d/cengarde restart; rm -rf /tmp/luci-indexcache /tmp/luci-modulecache; sleep 3; cengarde version'
}

apk() {
	sdk
	if [ ! -e "$SDK/package/feeds/luci/luci-base" ]; then
		(cd "$SDK" && ./scripts/feeds update luci >/dev/null)
		mkdir -p "$SDK/package/feeds/luci"
		ln -s ../../../feeds/luci/modules/luci-base "$SDK/package/feeds/luci/luci-base"
	fi
	ln -sfn "$SRC/openwrt/cengarde" "$SDK/package/cengarde"
	ln -sfn "$SRC/openwrt/luci-app-cengarde" "$SDK/package/luci-app-cengarde"
	[ -f "$SDK/.config" ] || make -s -C "$SDK" defconfig >/dev/null
	find "$SDK/bin" -name '*cengarde*.apk' -delete 2>/dev/null || true
	# The SDK's way builds every dependency again, the kernel's module
	# packages included: ~5 minutes a package, each time (CI's too). Once
	# they are built, each package's own Makefile alone takes seconds.
	if [ ! -f "$CACHE/.deps" ]; then
		make -s -C "$SDK" -j"$(nproc)" package/cengarde/compile package/luci-app-cengarde/compile
		touch "$CACHE/.deps"
	fi
	for p in cengarde luci-app-cengarde; do
		make -s -C "$SDK/package/$p" clean compile TOPDIR="$SDK" >/dev/null
	done
	pk=$(find "$SDK/bin/packages" -name 'cengarde-[0-9]*.apk' | head -1)
	[ -n "$pk" ] || die "no package built"
	dir=$(dirname "$pk")
	# shellcheck disable=SC2046 # the SDK's paths have no spaces
	set -- "$pk" $(find "$SDK/bin/packages" -name 'luci-*cengarde*.apk')
	$SSH "$HOST" 'rm -rf /tmp/nucpush && mkdir -p /tmp/nucpush'
	for f; do
		b=$(basename "$f")
		$SSH "$HOST" "cat > /tmp/nucpush/$b" <"$f"
	done
	$SSH "$HOST" 'apk add --allow-untrusted --force-reinstall /tmp/nucpush/*.apk | tail -1; rm -f /etc/config/cengarde.apk-new; cengarde version'
	echo "nucpush: installed from $dir"
}

case "$MODE" in
build) engine ;;
quick) quick ;;
apk) apk ;;
*) die "usage: bench/nucpush.sh quick|apk|build [HOST]" ;;
esac
