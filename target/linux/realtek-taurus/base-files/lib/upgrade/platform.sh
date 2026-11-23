REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN='fw_printenv fw_setenv'
RAMFS_COPY_DATA='/etc/fw_env.config /var/lock/fw_printenv.lock'

# The vendor U-Boot boots image A (ubi_k0) or B (ubi_k1) by sw_commit;
# sw_tryactive 2 means no trial boot (which would arm a 60 s watchdog).
# OpenWrt lives in A; stock firmware in B is left alone, as are ubi_Config,
# ubi_framework1/2 and ubi_apps. Stock's A rootfs volume makes room for
# rootfs and rootfs_data.
vsol_hg325ax30_prepare() {
	local ubidev

	ubidev="$(nand_attach_ubi "$CI_UBIPART")" || return 1
	if [ -n "$(nand_find_volume "$ubidev" ubi_r0)" ]; then
		ubirmvol "/dev/$ubidev" -N ubi_r0 || return 1
	fi
	fw_setenv sw_commit 0 && fw_setenv sw_active 0 &&
		fw_setenv sw_tryactive 2
}

# While the image is written only the upgrade LED (PON LOS) blinks: the PON,
# Internet, LAN and USB LEDs keep their triggers otherwise and stay lit.
indicate_upgrade() {
	local upgrade led

	. /etc/diag.sh
	upgrade="$(get_dt_led upgrade)"
	for led in /sys/class/leds/*; do
		[ "${led##*/}" = "$upgrade" ] && continue
		echo none > "$led/trigger" 2>/dev/null
		echo 0 > "$led/brightness" 2>/dev/null
	done
	set_state upgrade
}

platform_check_image() {
	case "$(board_name)" in
	vsol,hg325ax30)
		nand_do_platform_check "$(board_name)" "$1"
		return $?
		;;
	esac

	return 0
}

platform_do_upgrade() {
	case "$(board_name)" in
	vsol,hg325ax30)
		CI_UBIPART="ubi_device"
		CI_KERNPART="ubi_k0"
		vsol_hg325ax30_prepare || {
			echo "cannot prepare image A"
			return 1
		}
		nand_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
