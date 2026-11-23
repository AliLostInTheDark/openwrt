# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=collectd-mod-xpon
PKG_VERSION:=1.0
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/collectd-mod-xpon
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=xPON optical DDM telemetry collectd plugin
  DEPENDS:=+collectd
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -fPIC -shared \
		$(PKG_BUILD_DIR)/xpon.c -o $(PKG_BUILD_DIR)/xpon.so \
		$(TARGET_LDFLAGS)
endef

define Package/collectd-mod-xpon/install
	$(INSTALL_DIR) $(1)/usr/lib/collectd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xpon.so $(1)/usr/lib/collectd/
	$(INSTALL_DIR) $(1)/usr/share/collectd/plugin
	$(INSTALL_DATA) ./files/usr/share/collectd/plugin/xpon.json \
		$(1)/usr/share/collectd/plugin/xpon.json
	$(INSTALL_DIR) $(1)/etc/hotplug.d/ntp
	$(INSTALL_DATA) ./files/etc/hotplug.d/ntp/70-collectd \
		$(1)/etc/hotplug.d/ntp/70-collectd
endef

$(eval $(call BuildPackage,collectd-mod-xpon))
