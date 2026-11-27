# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_VERSION:=2.14
PKG_RELEASE:=1

PKG_HASH:=14fb6101f2ef424ec84c0296c3cbdeb7f79b22f5fbb46529bea0e362d0fab0d5

PKG_LICENSE:=BSD-3-Clause
PKG_LICENSE_FILES:=docs/license.rst

PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/trusted-firmware-a.mk
include $(INCLUDE_DIR)/package.mk

define Trusted-Firmware-A/Default
  BUILD_TARGET:=realtek_taurus
  BUILD_SUBTARGET:=rtl9607dq
  PLAT:=rtl9607dq
  TFA_IMAGE:=bl31.bin
endef

define Trusted-Firmware-A/vsol_hg325ax30
  NAME:=VSOL HG325AX30 (RTL9607DQ)
  BUILD_DEVICES:=vsol_hg325ax30
endef

TFA_TARGETS:=vsol_hg325ax30

TFA_MAKE_FLAGS += \
	DEBUG=0 \
	LOG_LEVEL=40 \
	bl31

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./files/. $(PKG_BUILD_DIR)/
endef

define Package/trusted-firmware-a/install
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/build/$(PLAT)/release/bl31.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-bl31.bin
endef

$(eval $(call BuildPackage/Trusted-Firmware-A))
