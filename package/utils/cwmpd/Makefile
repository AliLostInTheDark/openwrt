# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=cwmpd
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/cwmpd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=TR-069 (CWMP) client that never changes the device
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libuclient +ucode +ucode-mod-ubus
endef

define Package/cwmpd/description
  A TR-069 client that talks to the operator's ACS over IPv4, IPv6 or both,
  HTTP or HTTPS, and changes nothing. What the ACS sets is acknowledged and
  kept in memory only, and logged with passwords hidden; downloads are
  fetched into /tmp and never applied; reboots and factory resets are acted
  out, a reset making the ACS provision from scratch; uploads are refused.
  Every exchange is logged to /tmp/cwmp. For HTTPS install a
  libustream TLS library and ca-bundle.
endef

define Package/cwmpd/conffiles
/etc/config/cwmp
/etc/cwmpd.bootstrapped
endef

TARGET_CFLAGS += -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Werror

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) -I$(STAGING_DIR)/usr/include $(TARGET_CFLAGS) \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib \
		-o $(PKG_BUILD_DIR)/cwmpd ./src/cwmpd.c ./src/crreq.c ./src/stun.c \
		./src/soap.c ./src/model.c -lubox -lubus -luci -lblobmsg_json -luclient
endef

define Package/cwmpd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec/cwmpd $(1)/etc/config \
		$(1)/etc/init.d $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cwmpd $(1)/usr/sbin/cwmpd
	$(INSTALL_BIN) ./files/omci-acs $(1)/usr/libexec/cwmpd/omci-acs
	$(INSTALL_CONF) ./files/cwmp.config $(1)/etc/config/cwmp
	$(INSTALL_BIN) ./files/cwmpd.init $(1)/etc/init.d/cwmpd
	$(INSTALL_BIN) ./files/cwmpd.defaults $(1)/etc/uci-defaults/90_cwmpd
endef

$(eval $(call BuildPackage,cwmpd))
