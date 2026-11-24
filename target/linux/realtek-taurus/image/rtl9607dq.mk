define Device/vsol_hg325ax30
  DEVICE_VENDOR := VSOL
  DEVICE_MODEL := HG325AX30
  DEVICE_DTS := rtl9607dq-vsol-hg325ax30
  SOC := rtl9607dq
  DEVICE_PACKAGES := dxs-firmware xpon-agent kmod-rtw89-8852ce kmod-rtw89-8192xbe \
	rtl8852ce-firmware rtw89-vsol-hg325ax30 rtl8192xb-firmware wpad-basic-mbedtls \
	kmod-usb3 kmod-usb-dwc3 kmod-usb-ledtrig-usbport
  # U-Boot reads a fixed 6 MiB from ubi_k0, so the kernel volume keeps that size
  KERNEL_SIZE := 6144k
  KERNEL := kernel-bin | lzma | \
	fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb | \
	pad-to $$(KERNEL_SIZE)
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef
TARGET_DEVICES += vsol_hg325ax30
