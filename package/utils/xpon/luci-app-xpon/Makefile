# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.0.0
PKG_RELEASE:=1
LUCI_TITLE:=Unified LuCI support for xPON (GPON OMCI + EPON OAM + PON Mode)
LUCI_PKGARCH:=all
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=@(TARGET_realtek_luna||TARGET_realtek_taurus) +luci-base +xpon-agent +xpon-report

define Package/luci-app-xpon/extra
  PROVIDES:=luci-app-omci luci-app-epon
endef

include $(TOPDIR)/feeds/luci/luci.mk

# The router behind the ONU is an HGU on the HG325AX30: no SFU LAN VLAN page.
ifdef CONFIG_TARGET_realtek_taurus
define Build/Prepare/luci-app-xpon
	rm -f $(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/omci/ports.js
	sed -i '/"admin\/pon\/omci\/ports": {/,/^\t},/d' \
		$(PKG_BUILD_DIR)/root/usr/share/luci/menu.d/luci-app-xpon.json
endef
endif

# call BuildPackage - OpenWrt buildroot signature
