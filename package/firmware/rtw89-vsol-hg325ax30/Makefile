# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=rtw89-vsol-hg325ax30
PKG_RELEASE:=6

# Same tarball as package/firmware/linux-firmware
PKG_SOURCE_VERSION:=20261019
PKG_SOURCE_URL:=@KERNEL/linux/kernel/firmware
PKG_SOURCE:=linux-firmware-$(PKG_SOURCE_VERSION).tar.xz
PKG_HASH:=a10d1ab5baf2c74ba92e8c7cf313e691696c26bddfa6df00db795e48d70cd4ca

PKG_LICENSE:=Proprietary
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/rtw89-vsol-hg325ax30
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=RTL8852CE and RTL8192XB firmware for the VSOL HG325AX30
  PKGARCH:=all
endef

define Package/rtw89-vsol-hg325ax30/description
  RTL8852CE: linux-firmware's rtw8852c_fw-2.bin, whose BB and RF tables
  already cover RFE type 54, with the board's RTK66587_BPF front-end module
  overlay added to those tables and the TX power tables of RFE type 54
  appended as rtw89 firmware elements.
  RTL8192XB: Realtek's firmware with the BB, RF, NCTL, TX power and TX
  shape tables of RFE type 54 (RTK66287 front-end module) appended, as
  linux-firmware has no RTL8192XB firmware. All tables are the vendor
  parameter files of the board's stock firmware.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(TAR) -C $(PKG_BUILD_DIR) --strip-components=1 -xJf $(DL_DIR)/$(PKG_SOURCE) \
		linux-firmware-$(PKG_SOURCE_VERSION)/rtw89/rtw8852c_fw-2.bin
endef

# RTL8192XB elements: $(1) firmware, $(2) output, $(3) RFE dir, $(4) FEM dir,
# $(5) extra arguments
define Build/Elements
	python3 ./files/rtw89-elements.py -r 54 -f $(1) -o $(2) \
		--bb $(3)/PHY_REG.txt --bb $(4)/PHY_REG.txt \
		--gain $(3)/PHY_REG_GAIN.txt --gain $(4)/PHY_REG_GAIN.txt \
		--radio-a $(3)/RadioA.txt --radio-a $(4)/RadioA.txt \
		--radio-b $(3)/RadioB.txt --radio-b $(4)/RadioB.txt \
		--byrate $(4)/TXPWR_ByRate.txt \
		--lmt $(4)/TXPWR_LMT.txt \
		--lmt-ru $(4)/TXPWR_LMT_RU.txt \
		--trk $(4)/TXPWR_TrackTSSI.txt $(5)
endef

define Build/Compile
	python3 ./files/rtw89-elements.py -r 54 \
		-f $(PKG_BUILD_DIR)/rtw89/rtw8852c_fw-2.bin \
		-o $(PKG_BUILD_DIR)/rtw8852c_fw-2.bin \
		--bb-ovl ./files/rtl8852ce/RFE54/RTK66587_BPF/PHY_REG.txt \
		--gain-ovl ./files/rtl8852ce/RFE54/RTK66587_BPF/PHY_REG_GAIN.txt \
		--radio-a-ovl ./files/rtl8852ce/RFE54/RTK66587_BPF/RadioA.txt \
		--radio-b-ovl ./files/rtl8852ce/RFE54/RTK66587_BPF/RadioB.txt \
		--byrate ./files/rtl8852ce/RFE54/TXPWR_ByRate.txt \
		--lmt ./files/rtl8852ce/RFE54/TXPWR_LMT.txt \
		--lmt-ru ./files/rtl8852ce/RFE54/TXPWR_LMT_RU.txt
	$(call Build/Elements,../rtl8192xb-firmware/files/rtw8192xb_fw.bin, \
		$(PKG_BUILD_DIR)/rtw8192xb_fw-2.bin, \
		./files/rtl8192xbe/RFE54,./files/rtl8192xbe/RFE54/RTK66287, \
		--nctl ./files/rtl8192xbe/NCTL.txt --tx-shape)
endef

define Package/rtw89-vsol-hg325ax30/install
	$(INSTALL_DIR) $(1)/lib/firmware/rtw89
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/rtw8852c_fw-2.bin $(1)/lib/firmware/rtw89
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/rtw8192xb_fw-2.bin $(1)/lib/firmware/rtw89
endef

$(eval $(call BuildPackage,rtw89-vsol-hg325ax30))
