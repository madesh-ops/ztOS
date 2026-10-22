#!/bin/bash
# The Galaxy M20 seen from its running TWRP, over adb: shared by backup.sh,
# flash.sh and flash-root.sh. Sourced, not run.
#
# Partitions are always reached by name, under the eMMC controller's by-name
# directory (fstab.exynos7904 in LineageOS's m20lte tree), and the phone is
# checked to be an M20 before anything is read or written.

BYNAME=/dev/block/platform/13500000.dwmmc0/by-name

ash() { adb shell "$@" | tr -d '\r'; }

twrp_check() {
	command -v adb >/dev/null || { echo "no adb on this computer (android-tools)" >&2; exit 1; }
	[ "$(adb get-state 2>/dev/null)" = recovery ] ||
		{ echo "the phone is not in TWRP over adb (Volume Up + Power with the cable in)" >&2; exit 1; }
	local dev
	dev=$(ash getprop ro.product.device)
	case "$dev" in
	m20lte|m20ltedd) ;;
	*) echo "this is not a Galaxy M20 (ro.product.device=$dev)" >&2; exit 1 ;;
	esac
	ash "test -d $BYNAME" || { echo "no $BYNAME on the phone: is this TWRP for m20lte?" >&2; exit 1; }
}

# the block device behind a partition name, checked to be an eMMC partition
part_node() {
	local node
	node=$(ash "readlink -f $BYNAME/$1")
	case "$node" in
	/dev/block/mmcblk0p[0-9]*) echo "$node" ;;
	*) echo "partition $1 is \"$node\", not an eMMC partition" >&2; exit 1 ;;
	esac
}

part_size() { ash "blockdev --getsize64 $BYNAME/$1" | tr -dc 0-9; }
