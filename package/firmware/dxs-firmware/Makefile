# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=dxs-firmware
PKG_RELEASE:=1

PKG_LICENSE:=Proprietary

PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

# The vendor files are not part of the tree. They are taken from this
# directory, which is not under version control, and checked against the
# hashes of the files the udsp DUSLIC-XS plug-in was developed with.
DXS_DIR:=$(TOPDIR)/vsol-local/firmware/dxs
DXS_FW_SHA256:=779afa0255e079319ccb264acba714816505c7bcda60c73b967b0c2259dae824
DXS_BBD_SHA256:=4069017d3ae0c516fc593598d73b98ee7ce71822535223be2d4a4cb5d066b63b

define Package/dxs-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=DUSLIC-XS SLIC firmware and line coefficients
  PKGARCH:=all
endef

define Package/dxs-firmware/description
  The SLIC firmware (fw_dxs.bin) and the line coefficients (bbd.bin) the
  udsp DUSLIC-XS plug-in loads from /lib/firmware/dxs. Both come from the
  stock firmware and are copied from vsol-local/firmware/dxs at build time.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
	@test -f $(DXS_DIR)/fw_dxs.bin -a -f $(DXS_DIR)/bbd.bin || { \
		echo "dxs-firmware: fw_dxs.bin and bbd.bin are missing in $(DXS_DIR)" >&2; \
		false; }
	echo "$(DXS_FW_SHA256)  $(DXS_DIR)/fw_dxs.bin" | sha256sum -c -
	echo "$(DXS_BBD_SHA256)  $(DXS_DIR)/bbd.bin" | sha256sum -c -
endef

define Package/dxs-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/dxs
	$(INSTALL_DATA) $(DXS_DIR)/fw_dxs.bin $(1)/lib/firmware/dxs
	$(INSTALL_DATA) $(DXS_DIR)/bbd.bin $(1)/lib/firmware/dxs
endef

$(eval $(call BuildPackage,dxs-firmware))
