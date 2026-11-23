# SPDX-License-Identifier: BSD-2-Clause

include $(TOPDIR)/rules.mk

PKG_NAME:=udsp-slic-dxs
PKG_RELEASE:=5
PKG_LICENSE:=BSD-2-Clause
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

PKG_BUILD_DEPENDS:=udsp

include $(INCLUDE_DIR)/package.mk

define Package/udsp-slic-dxs
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=udsp SLIC plug-in for the MaxLinear DUSLIC-XS
  DEPENDS:=+libudsp
endef

define Package/udsp-slic-dxs/description
 SLIC plug-in for libudsp that drives a MaxLinear DUSLIC-XS (PEF3200x)
 over spidev. The firmware patch is not included; install it as
 /lib/firmware/dxs/fw_dxs.bin.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

MAKE_FLAGS += \
	UDSP_SRC=$(STAGING_DIR)/usr/share/udsp/src \
	UDSP_INC=$(STAGING_DIR)/usr/include/udsp

define Package/udsp-slic-dxs/install
	$(INSTALL_DIR) $(1)/usr/lib/udsp $(1)/usr/sbin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/udsp-slic-dxs.so $(1)/usr/lib/udsp/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/dxs-probe $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/udsp-slic-dxs.keep $(1)/lib/upgrade/keep.d/udsp-slic-dxs
endef

$(eval $(call BuildPackage,udsp-slic-dxs))
