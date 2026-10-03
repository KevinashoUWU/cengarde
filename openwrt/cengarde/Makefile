# OpenWrt package for cengarde, built from engine/ in this repository.
#
# With the OpenWrt SDK (24.10 builds an .ipk, 25.12 an .apk):
#   ln -s /path/to/cengarde/openwrt/cengarde package/cengarde
#   make defconfig && make package/cengarde/compile
#
# The package directory has to stay inside the repository (a symlink is
# fine): the sources are taken from ../../engine. Set ENGINE_DIR otherwise.
#
# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=cengarde
# 0.2: protocol v2. Both ends must run the same protocol version.
PKG_VERSION:=0.2.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE.txt

ENGINE_DIR ?= $(CURDIR)/../../engine
GIT_REV := $(shell git -C $(ENGINE_DIR) rev-parse --short HEAD 2>/dev/null)

include $(INCLUDE_DIR)/package.mk

define Package/cengarde
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Redundant multi-link transport for WireGuard
  URL:=https://github.com/KevinashoUWU/cengarde
endef

define Package/cengarde/description
  cengarde sends every WireGuard datagram over several uplinks at once and
  keeps the first copy that arrives at the other end, so the tunnel survives
  the loss or delay of any single link. It mutes links that lag far behind
  the others. The same binary is the client (the router) and the server.
endef

define Package/cengarde/conffiles
/etc/cengarde/cengarde.conf
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
		VERSION="$(PKG_VERSION)-r$(PKG_RELEASE)$(if $(GIT_REV),-g$(GIT_REV))" \
		cengarde
endef

define Package/cengarde/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/cengarde
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cengarde $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/cengarde.init $(1)/etc/init.d/cengarde
	sed 's|^#status_file = .*|status_file = /var/run/cengarde.json|' \
		$(ENGINE_DIR)/examples/client.conf > $(1)/etc/cengarde/cengarde.conf
	chmod 0600 $(1)/etc/cengarde/cengarde.conf
endef

$(eval $(call BuildPackage,cengarde))
