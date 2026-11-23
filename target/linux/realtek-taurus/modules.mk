# SPDX-License-Identifier: GPL-2.0-only

define KernelPackage/sound-soc-rtl9607dq
  TITLE:=Realtek RTL9607DQ SSP (PCM/TDM) support
  KCONFIG:= \
	CONFIG_SND_SOC_BT_SCO \
	CONFIG_SND_SOC_RTL9607DQ_SSP
  FILES:= \
	$(LINUX_DIR)/sound/soc/codecs/snd-soc-bt-sco.ko \
	$(LINUX_DIR)/sound/soc/realtek/snd-soc-rtl9607dq-ssp.ko
  AUTOLOAD:=$(call AutoLoad,56,snd-soc-bt-sco snd-soc-rtl9607dq-ssp)
  DEPENDS:=@TARGET_realtek_taurus +kmod-sound-soc-core +kmod-sound-soc-simple-card
  $(call AddDepends/sound)
endef

define KernelPackage/sound-soc-rtl9607dq/description
 Support for the voice PCM/TDM interface (SSP) of the Realtek RTL9607DQ,
 with a simple-audio-card and a dummy codec for the FXS SLIC.
endef

$(eval $(call KernelPackage,sound-soc-rtl9607dq))
