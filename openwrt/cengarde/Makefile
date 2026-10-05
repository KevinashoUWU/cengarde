# OpenWrt package for cengarde, built from engine/ in this repository.
#
# With the OpenWrt SDK (24.10 builds an .ipk, 25.12 an .apk):
#   ln -s /path/to/cengarde/openwrt/cengarde package/cengarde
#   make defconfig && make package/cengarde/compile
#
# The package directory has to stay inside the repository (a symlink is
# fine): the sources are taken from ../../engine and ../../contrib. Set
# ENGINE_DIR otherwise.
#
# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=cengarde
# 0.4: protocol v3 (IP pass asked of the VPS), control socket, changes
# applied without a restart. Both ends must run the same protocol version.
PKG_VERSION:=0.4.5
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE.txt

ENGINE_DIR ?= $(CURDIR)/../../engine
GIT_REV := $(shell git -C $(ENGINE_DIR) rev-parse HEAD 2>/dev/null)
GIT_SHORT := $(shell git -C $(ENGINE_DIR) rev-parse --short=7 HEAD 2>/dev/null)

include $(INCLUDE_DIR)/package.mk

define Package/cengarde
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Redundant multi-link transport for WireGuard
  URL:=https://github.com/KevinashoUWU/cengarde
  DEPENDS:=+wireguard-tools
endef

define Package/cengarde/description
  cengarde sends every WireGuard datagram over several uplinks at once and
  keeps the first copy that arrives at the other end, so the tunnel survives
  the loss or delay of any single link. It mutes links that lag far behind
  the others. The same binary is the client (the router) and the server.
  Configured in /etc/config/cengarde (or with luci-app-cengarde), which also
  sets up the WireGuard tunnel to the VPS from a single pairing secret.
endef

define Package/cengarde/conffiles
/etc/config/cengarde
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(ENGINE_DIR)/Makefile $(ENGINE_DIR)/src $(ENGINE_DIR)/../LICENSE.txt $(PKG_BUILD_DIR)/
endef

# The SDK brings its own hardening flags: drop the engine's defaults.
define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		HARDEN= HARDEN_LDFLAGS= \
		VERSION="$(PKG_VERSION)-r$(PKG_RELEASE)$(if $(GIT_SHORT),-g$(GIT_SHORT))" \
		cengarde
endef

# The VPS cloud-config builds the server from this package's own commit,
# so that both ends speak the same protocol.
define Package/cengarde/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults \
		$(1)/usr/share/cengarde
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cengarde ./files/cengarde-setup $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/cengarde.init $(1)/etc/init.d/cengarde
	$(INSTALL_CONF) ./files/cengarde.config $(1)/etc/config/cengarde
	$(INSTALL_BIN) ./files/cengarde.defaults $(1)/etc/uci-defaults/90-cengarde
	sed 's/CENGARDE_REF=master;/CENGARDE_REF=$(if $(GIT_REV),$(GIT_REV),master);/' \
		$(ENGINE_DIR)/../contrib/vps/cloud-config.yaml > $(1)/usr/share/cengarde/cloud-config.yaml
endef

# Removal undoes the tunnel, firewall, uplink and UPnP settings it made.
define Package/cengarde/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /usr/sbin/cengarde-setup disable
exit 0
endef

$(eval $(call BuildPackage,cengarde))
