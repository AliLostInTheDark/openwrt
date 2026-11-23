# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=rtl8192xb-firmware
PKG_RELEASE:=2

PKG_LICENSE:=Proprietary

PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/rtl8192xb-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=RealTek RTL8192XB firmware
  PKGARCH:=all
endef

define Package/rtl8192xb-firmware/description
  Firmware 0.27.109.0 for the RealTek RTL8192XB 2.4 GHz 802.11ax PCIe
  chip: the NIC and WoWLAN images for both chip cuts, in the rtw89
  multi-firmware format. It is multiFW.bin of Realtek's GPL rtk_wifi6_ap
  driver as published in github.com/BPI-SINOVOIP/BPI-RT2 (c24c6c0).
endef

define Build/Compile
endef

define Package/rtl8192xb-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/rtw89
	$(INSTALL_DATA) ./files/rtw8192xb_fw.bin $(1)/lib/firmware/rtw89
endef

$(eval $(call BuildPackage,rtl8192xb-firmware))
