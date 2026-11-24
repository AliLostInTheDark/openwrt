REQUIRE_IMAGE_METADATA=1

# The stock firmware is gone for good: the first upgrade removes its volumes,
# which gives rootfs_data their space, and adds the two volumes (one LEB each)
# that hold the environment of U-Boot. ubi_k0 is OpenWrt's kernel and ubi_DTB0
# stays with it, the vendor U-Boot in the standby FIP still reads both.
vsol_hg325ax30_volumes() {
	local ubidev="$1" vol

	for vol in ubi_Config ubi_DTB1 ubi_k1 ubi_r0 ubi_r1 \
		   ubi_framework1 ubi_framework2 ubi_apps; do
		[ -n "$(nand_find_volume "$ubidev" "$vol")" ] || continue
		ubirmvol "/dev/$ubidev" -N "$vol" || return 1
	done

	for vol in ubootenv ubootenv2; do
		[ -n "$(nand_find_volume "$ubidev" "$vol")" ] ||
			ubimkvol "/dev/$ubidev" -N "$vol" -S 1 || return 1
	done
}

vsol_hg325ax30_prepare() {
	local ubidev

	ubidev="$(nand_attach_ubi "$CI_UBIPART")" || return 1
	vsol_hg325ax30_volumes "$ubidev"
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
			echo "cannot prepare the UBI volumes"
			return 1
		}
		nand_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
