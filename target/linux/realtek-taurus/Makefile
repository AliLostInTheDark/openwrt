# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=aarch64
BOARD:=realtek-taurus
BOARDNAME:=Realtek Taurus
SUBTARGETS:=rtl9607dq
FEATURES:=dt squashfs nand separate_ramdisk gpio fpu usb

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Realtek Taurus (RTL9607DQ) based GPON ONU boards.
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += \
	kmod-gpio-button-hotplug uboot-envtools

$(eval $(call BuildTarget))
