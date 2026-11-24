# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_VERSION:=2026.10
PKG_RELEASE:=1
PKG_HASH:=3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

define U-Boot/Default
  BUILD_TARGET:=realtek_taurus
  BUILD_SUBTARGET:=rtl9607dq
  UBOOT_IMAGE:=u-boot.bin
endef

define U-Boot/vsol_hg325ax30
  NAME:=VSOL HG325AX30
  BUILD_DEVICES:=vsol_hg325ax30
endef

# The same image without the autoboot delay, for tests from RAM.
define U-Boot/vsol_hg325ax30_ram
  NAME:=VSOL HG325AX30 (RAM test)
  UBOOT_CONFIG:=vsol_hg325ax30 vsol_hg325ax30_ram
endef

# The image BL31 starts from the FIP, linked at the address it loads BL33 to.
define U-Boot/vsol_hg325ax30_bl33
  NAME:=VSOL HG325AX30 (BL33)
  UBOOT_CONFIG:=vsol_hg325ax30 vsol_hg325ax30_bl33
endef

UBOOT_TARGETS:=vsol_hg325ax30 vsol_hg325ax30_ram vsol_hg325ax30_bl33

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./files/. $(PKG_BUILD_DIR)/
endef

# The RAM test image is started from the vendor U-Boot with
# "tftp 6000000 <file>" and "go 6000000"; the BL33 image replaces the vendor
# U-Boot in the FIP.
define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/u-boot.bin \
		$(STAGING_DIR_IMAGE)/$(or $(BUILD_DEVICES),$(lastword $(UBOOT_CONFIG)))-u-boot.bin
endef

$(eval $(call BuildPackage/U-Boot))
