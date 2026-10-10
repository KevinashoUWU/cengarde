# LuCI app for cengarde: status, settings and the VPS cloud-config.
#
# Built with the OpenWrt SDK once the LuCI feed is there:
#   ./scripts/feeds update luci && ./scripts/feeds install luci-base
#   ln -s /path/to/cengarde/openwrt/luci-app-cengarde package/luci-app-cengarde
#   make package/luci-app-cengarde/compile
# It also builds luci-i18n-cengarde-es (Spanish).
#
# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for cengarde
LUCI_DEPENDS:=+cengarde +luci-proto-wireguard
LUCI_PKGARCH:=all
LUCI_URL:=https://github.com/KevinashoUWU/cengarde
LUCI_MAINTAINER:=cengarde contributors

PKG_VERSION:=0.5.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
