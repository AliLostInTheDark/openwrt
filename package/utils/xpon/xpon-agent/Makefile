# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=xpon-agent
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/xpon-agent
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=xPON
  TITLE:=Unified xPON userspace control plane (GPON OMCI + EPON OAM)
  DEPENDS:=@(TARGET_realtek_luna||TARGET_realtek_taurus) \
	+omcid +omci-vlanscan +TARGET_realtek_luna:omci-management \
	+ucode +libucode +ucode-mod-fs +ucode-mod-log +ucode-mod-math \
	+ucode-mod-ubus +ucode-mod-uci +ucode-mod-uloop \
	+rpcd +rpcd-mod-ucode +libuci +libubus \
	+ip-bridge +nftables-json +kmod-nft-bridge
  PROVIDES:=omci-agent epon-oamd
endef

define Package/xpon-agent/description
 Unified userspace control plane for GPON (ITU-T G.988 OMCI via Generic
 Netlink and omcid) and EPON (IEEE 802.3ah / CTC extended OAM via epon-oamd).
 Manages PON mode selection, exposes xpon.agent and omci.agent over ubus,
 and provides xponctl and omcictl CLI utilities.
endef

define Build/Prepare
	rm -rf $(PKG_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-fPIC -Wall -Wextra -Werror \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/omci.so \
		$(PKG_BUILD_DIR)/omci_ucode.c \
		-shared $(TARGET_LDFLAGS) -lucode
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra \
		-o $(PKG_BUILD_DIR)/epon-oamd \
		$(PKG_BUILD_DIR)/epon-oamd.c
endef

define Package/xpon-agent/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci.so $(1)/usr/lib/ucode/omci.so

	$(INSTALL_DIR) $(1)/usr/share/ucode/omci
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/mib.uc \
		$(1)/usr/share/ucode/omci/mib.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/g988.uc \
		$(1)/usr/share/ucode/omci/g988.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/voice.uc \
		$(1)/usr/share/ucode/omci/voice.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/mcast.uc \
		$(1)/usr/share/ucode/omci/mcast.uc

	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_BIN) ./files/usr/share/rpcd/ucode/omci.agent \
		$(1)/usr/share/rpcd/ucode/omci.agent
	$(INSTALL_BIN) ./files/usr/share/rpcd/ucode/xpon.agent \
		$(1)/usr/share/rpcd/ucode/xpon.agent

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/epon-oamd $(1)/usr/sbin/epon-oamd
	$(INSTALL_BIN) ./files/usr/sbin/xponctl $(1)/usr/sbin/xponctl
	$(INSTALL_BIN) ./files/usr/sbin/omcictl $(1)/usr/sbin/omcictl
	$(INSTALL_BIN) ./files/usr/sbin/omci-diag $(1)/usr/sbin/omci-diag
	$(INSTALL_BIN) ./files/usr/sbin/omci-report $(1)/usr/sbin/omci-report
	$(INSTALL_BIN) ./files/usr/sbin/omci-voice $(1)/usr/sbin/omci-voice
	$(INSTALL_BIN) ./files/usr/sbin/omci-mcast $(1)/usr/sbin/omci-mcast

	$(INSTALL_DIR) $(1)/usr/share/nftables.d/ruleset-post
	$(INSTALL_DATA) ./files/usr/share/nftables.d/ruleset-post/20-pon-guard.nft \
		$(1)/usr/share/nftables.d/ruleset-post/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/xpon-agent $(1)/etc/init.d/xpon-agent
	$(INSTALL_BIN) ./files/etc/init.d/epon-oamd $(1)/etc/init.d/epon-oamd
	$(INSTALL_BIN) ./files/etc/init.d/omci-voice $(1)/etc/init.d/omci-voice
	$(INSTALL_BIN) ./files/etc/init.d/omci-mcast $(1)/etc/init.d/omci-mcast

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omci $(1)/etc/config/omci
	$(INSTALL_CONF) ./files/etc/config/epon $(1)/etc/config/epon

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/79_xpon_migrate \
		$(1)/etc/uci-defaults/79_xpon_migrate
	$(INSTALL_BIN) ./files/etc/uci-defaults/80_omci_reboot \
		$(1)/etc/uci-defaults/80_omci_reboot
	$(INSTALL_BIN) ./files/etc/uci-defaults/81_omci_drop_dying_gasp \
		$(1)/etc/uci-defaults/81_omci_drop_dying_gasp
endef

define Package/xpon-agent/conffiles
/etc/config/omci
/etc/config/epon
endef

define Package/xpon-agent/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/rpcd reload >/dev/null 2>&1 || true
endef

$(eval $(call BuildPackage,xpon-agent))
