#!/bin/bash
# Build the ztOS boot image for the Galaxy M20 (m20lte): a plain mainline 7.1
# kernel (Exynos 7885 support is upstream; the M20's few driver changes come
# from devices/m20lte/kernel/patch-kernel.py), ztOS's initramfs built in,
# wrapped in uniLoader, packed around the stock BOOT image's header and DTBH.
#
#   devices/m20lte/scripts/build-boot.sh <name> [options]
#     --fallback boot|recovery   where a reset / panic lands (default
#                                recovery: TWRP is on RECOVERY, ztOS on BOOT)
#     --ram 3|4                  the phone's memory in GB (default 3: safe on both)
#     --watchdog                 arm the A53 watchdog in uniLoader and hand it
#                                to the kernel (s3c2410_wdt). Off until seen to work.
#     --no-touch                 leave the touch controller out of the tree
#     --serial                   also log to the UART (ttySAC2, 115200)
#     --cmdline "..."            extra kernel command line (zboot.rescue=1 ...)
#
# Needs, as the S9 build does: the Arm GNU toolchain and busybox-arm64 under
# build/, and the phone's own stock BOOT image in stock/m20lte/BOOT.img (the
# installer saves it from TWRP before writing anything).
#
# Output: out/<name>.img (+ .config, .dts, .cmdline, .sha256), and the M20's
# out-of-tree modules in devices/m20lte/drivers/*.ko.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
DEV=$ROOT/devices/m20lte
B=$ROOT/build
KVER=7.1
SRC=$B/linux-$KVER-m20lte
TARBALL=$B/linux-$KVER.tar.xz
TARBALL_URL=https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$KVER.tar.xz
UNILOADER=$B/uniLoader-m20lte
UNILOADER_URL=https://github.com/ivoszbg/uniLoader.git
UNILOADER_REV=a459fb4c777ad9e1f3a433b14266423916f24e60   # has board-a30 (Exynos 7904)
TC=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
# the phone's own stock BOOT (backup.sh); ZTOS_M20_REF points elsewhere, e.g.
# to try the build before the phone has been backed up
REF=${ZTOS_M20_REF:-$ROOT/stock/m20lte/BOOT.img}
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-

NAME=${1:?image name}
shift
FALLBACK=recovery RAM=3 WDT= SERIAL= EXTRA=
DTS_FLAGS=()
while [ $# -gt 0 ]; do
	case "$1" in
	--fallback) FALLBACK=$2; shift ;;
	--ram) RAM=$2; shift ;;
	--watchdog) WDT=1; DTS_FLAGS+=(--watchdog) ;;
	--no-touch) DTS_FLAGS+=(--no-touch) ;;
	--serial) SERIAL=1 ;;
	--cmdline) EXTRA=$2; shift ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case "$FALLBACK" in
boot) INFORM3=0x12345670 ;;
recovery) INFORM3=0x12345674 ;;
*) echo "--fallback boot|recovery" >&2; exit 2 ;;
esac
DTS_FLAGS+=(--ram "$RAM")

need() { [ -e "$1" ] || { echo "missing $1: $2" >&2; exit 1; }; }
need "$TC/${CROSS_COMPILE}gcc" "the Arm GNU toolchain 14.2 (the same one the S9 build uses)"
need "$B/busybox-arm64" "a static aarch64 busybox (the same one the S9 build uses)"
need "$REF" "the phone's stock BOOT image: the installer saves it from TWRP (ztos install --target m20lte)"
python3 -I "$DEV/scripts/mkboot.py" --info "$REF"
mkdir -p "$ROOT/out"
OUT=$ROOT/out/$NAME

echo "=== kernel source ==="
if [ ! -f "$SRC/Makefile" ]; then
	if [ ! -s "$TARBALL" ]; then
		curl -L --fail -o "$TARBALL.part" "$TARBALL_URL"
		mv "$TARBALL.part" "$TARBALL"
	fi
	rm -rf "$SRC.part"; mkdir -p "$SRC.part"
	tar -xf "$TARBALL" -C "$SRC.part" --strip-components=1
	mv "$SRC.part" "$SRC"
fi
python3 -I "$DEV/kernel/patch-kernel.py" "$SRC"

echo "=== device tree ==="
DTS=arch/arm64/boot/dts/exynos/exynos7904-m20lte.dts
python3 -I "$DEV/kernel/make-dts.py" "$DEV/kernel/exynos7904-m20lte.dts" "$SRC/$DTS" "${DTS_FLAGS[@]}"
grep -q exynos7904-m20lte "$SRC/arch/arm64/boot/dts/exynos/Makefile" ||
	sed -i 's|^\(\s*\)exynos7885-jackpotlte.dtb\(.*\)$|&\n\1exynos7904-m20lte.dtb\2|' \
		"$SRC/arch/arm64/boot/dts/exynos/Makefile"
grep -q exynos7904-m20lte "$SRC/arch/arm64/boot/dts/exynos/Makefile"

echo "=== initramfs ==="
LIST=$B/initramfs-m20lte.list
cat > "$LIST" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /run 0755 0 0
dir /tmp 1777 0 0
dir /etc 0755 0 0
dir /newroot 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/kmsg 0600 0 0 c 1 11
file /bin/busybox $B/busybox-arm64 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $ROOT/initramfs/init 0755 0 0
EOF

echo "=== config ==="
cd "$SRC"
[ -f out/.config ] || make -s O=out defconfig
cfg() { ./scripts/config --file out/.config "$@"; }
# Other SoCs, and whole subsystems this phone has nothing of (or nothing a
# mainline driver exists for yet: PCIe, Wi-Fi, sound).
for sym in ARCH_ACTIONS ARCH_AIROHA ARCH_SUNXI ARCH_ALPINE ARCH_APPLE ARCH_BCM \
	ARCH_BCM2835 ARCH_BCMBCA ARCH_BRCMSTB ARCH_BERLIN ARCH_BLAIZE ARCH_EXYNOSAUTOV9 \
	ARCH_SPARX5 ARCH_K3 ARCH_LG1K ARCH_HISI ARCH_KEEMBAY ARCH_MEDIATEK ARCH_MESON \
	ARCH_MVEBU ARCH_MXC ARCH_LAYERSCAPE ARCH_MA35 ARCH_NPCM ARCH_NXP ARCH_QCOM \
	ARCH_REALTEK ARCH_RENESAS ARCH_ROCKCHIP ARCH_S32 ARCH_SEATTLE ARCH_INTEL_SOCFPGA \
	ARCH_STM32 ARCH_SYNQUACER ARCH_TEGRA ARCH_TESLA_FSD ARCH_SPRD ARCH_THUNDER \
	ARCH_THUNDER2 ARCH_UNIPHIER ARCH_VEXPRESS ARCH_VISCONTI ARCH_XGENE ARCH_ZYNQMP \
	ARCH_SOPHGO ARCH_STM32MP ARCH_MMP ARCH_WPCM450 ARCH_VT8500 \
	SOUND SND MEDIA_SUPPORT PCI ACPI EFI NUMA XEN KVM VIRTUALIZATION \
	VIRTIO_MENU INFINIBAND MTD ATA MD NVME_CORE SCSI_LOWLEVEL \
	ETHERNET WLAN WWAN USB_NET_DRIVERS PHYLIB \
	INPUT_JOYSTICK INPUT_TABLET W1 FUSION FIREWIRE MACINTOSH_DRIVERS \
	BT CAN NFC WIRELESS RFKILL \
	BTRFS_FS XFS_FS GFS2_FS OCFS2_FS JFS_FS NTFS3_FS EROFS_FS SQUASHFS \
	NFS_FS NFSD CIFS CEPH_FS 9P_FS CODA_FS AFS_FS ORANGEFS_FS \
	DRM_AMDGPU DRM_NOUVEAU DRM_I915 DRM_RADEON DRM_MSM DRM_ETNAVIV DRM_ROCKCHIP \
	DRM_MEDIATEK DRM_TEGRA DRM_VC4 DRM_SUN4I DRM_ARM DRM_KOMEDA DRM_HDLCD DRM_MALI_DISPLAY \
	DRM_PL111 DRM_RCAR_DU DRM_IMX DRM_MXSFB DRM_MESON DRM_TIDSS DRM_VIRTIO_GPU \
	DRM_LIMA DRM_V3D DRM_EXYNOS DRM_TVE200 DRM_LOGICVC DRM_SSD130X \
	STAGING COMEDI GREYBUS FPGA MAILBOX_TEST TEE OPTEE \
	SERIAL_8250 I2C_MUX SPI_MEM MTD_SPI_NOR \
	COMPAT DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT FTRACE STACK_TRACER \
	SECURITY_SELINUX SECURITY_APPARMOR SECURITY_SMACK SECURITY_TOMOYO \
	NET_SCHED NET_CLS_ACT IP_VS BRIDGE VLAN_8021Q \
	TYPEC WATCHDOG_NOWAYOUT \
	USB_G_SERIAL RANDOMIZE_BASE ARM64_VA_BITS_52 ARM64_PA_BITS_52 \
	PSTORE_PMSG PSTORE_COMPRESS STRICT_DEVMEM CPU_FREQ; do
	cfg --disable "$sym"
done
make -s O=out olddefconfig
# What the M20 runs on: the SoC (clocks, pins, PMU, USI), the eMMC, the
# bootloader's framebuffer as a DRM device, the Mali-G71 (Panfrost, render
# only: the screen stays simpledrm's), the keys, touch, vibration motor,
# motion sensor, USB as a gadget, and what systemd and ztOS need.
for sym in BLK_DEV_INITRD ARCH_EXYNOS PINCTRL_EXYNOS PINCTRL_SAMSUNG EXYNOS_PMU \
	COMMON_CLK_SAMSUNG EXYNOS_ARM64_COMMON_CLK EXYNOS_USI \
	MMC MMC_BLOCK MMC_DW MMC_DW_PLTFM MMC_DW_EXYNOS \
	SERIAL_SAMSUNG SERIAL_SAMSUNG_CONSOLE \
	DRM DRM_SIMPLEDRM DRM_PANFROST DRM_FBDEV_EMULATION FRAMEBUFFER_CONSOLE \
	FRAMEBUFFER_CONSOLE_DETECT_PRIMARY BACKLIGHT_CLASS_DEVICE \
	INPUT_EVDEV KEYBOARD_GPIO INPUT_TOUCHSCREEN TOUCHSCREEN_HIMAX_HX83112B \
	INPUT_MISC INPUT_GPIO_VIBRA INPUT_FF_MEMLESS \
	I2C I2C_CHARDEV I2C_S3C2410 I2C_EXYNOS5 REGMAP_I2C \
	IIO IIO_ST_LSM6DSX IIO_ST_LSM6DSX_I2C \
	POWER_SUPPLY \
	USB_SUPPORT USB_GADGET USB_DWC3 USB_DWC3_GADGET USB_DWC3_EXYNOS GENERIC_PHY \
	PHY_EXYNOS5_USBDRD USB_LIBCOMPOSITE USB_U_SERIAL USB_U_ETHER USB_F_ACM USB_F_NCM \
	USB_F_ECM USB_F_RNDIS USB_F_MASS_STORAGE USB_CONFIGFS USB_CONFIGFS_SERIAL \
	USB_CONFIGFS_ACM USB_CONFIGFS_NCM USB_CONFIGFS_ECM USB_CONFIGFS_RNDIS \
	USB_CONFIGFS_MASS_STORAGE USB_CONFIGFS_F_FS \
	POWER_RESET POWER_RESET_SYSCON REBOOT_MODE SYSCON_REBOOT_MODE MFD_SYSCON \
	EXT4_FS TMPFS DEVTMPFS DEVTMPFS_MOUNT PSTORE PSTORE_RAM PSTORE_CONSOLE \
	MAGIC_SYSRQ DEVMEM MODULES MODULE_UNLOAD ARM64_VA_BITS_48 ARM64_PA_BITS_48 \
	CMDLINE_FORCE NETFILTER CGROUPS NAMESPACES FHANDLE AUTOFS_FS SECCOMP \
	INOTIFY_USER SIGNALFD SECURITY_LANDLOCK TIMERFD EPOLL UNIX INET IPV6 PACKET TUN \
	ZRAM FUSE_FS; do
	cfg --enable "$sym"
done
# USB is a device here, never a host: dual-role needs the S2MU004's cable
# detection, which has no mainline driver.
cfg --enable USB_DWC3_GADGET
cfg --disable USB_DWC3_HOST
cfg --disable USB_DWC3_DUAL_ROLE
if [ -n "$WDT" ]; then
	cfg --enable WATCHDOG
	cfg --enable WATCHDOG_SYSFS
	cfg --enable S3C2410_WATCHDOG
else
	cfg --disable S3C2410_WATCHDOG
fi
cfg --set-str EXTRA_FIRMWARE ""
cfg --set-str INITRAMFS_SOURCE "$LIST"
cfg --set-val CMA_SIZE_MBYTES 64
cfg --set-str LOCALVERSION "-zboot"
# clk_ignore_unused / pd_ignore_unused: the display, its DSI link and the
# panel's power are the bootloader's and have no mainline driver; gating the
# clocks nobody claims would freeze the screen (the stock kernel runs with
# clk_ignore_unused too).
CMDLINE="console=tty0 loglevel=7 panic=5 keep_bootcon clk_ignore_unused pd_ignore_unused maxcpus=8"
[ -n "$SERIAL" ] && CMDLINE="console=ttySAC2,115200 $CMDLINE"
[ -n "$WDT" ] && CMDLINE="$CMDLINE s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60"
CMDLINE="$CMDLINE${EXTRA:+ $EXTRA}"
cfg --set-str CMDLINE "$CMDLINE"
make -s O=out olddefconfig
# Every symbol asked for above that olddefconfig dropped (a dependency is
# missing, or 7.1 renamed it) is worth knowing before flashing.
for sym in ARCH_EXYNOS MMC_DW_EXYNOS DRM_SIMPLEDRM DRM_PANFROST TOUCHSCREEN_HIMAX_HX83112B INPUT_GPIO_VIBRA \
	I2C_EXYNOS5 EXYNOS_USI USB_DWC3_EXYNOS PHY_EXYNOS5_USBDRD USB_CONFIGFS_NCM \
	SYSCON_REBOOT_MODE PSTORE_RAM IIO_ST_LSM6DSX_I2C; do
	grep -q "^CONFIG_$sym=y" out/.config || echo "WARNING: CONFIG_$sym is not =y"
done

echo "=== kernel ==="
make -j"$(nproc)" O=out Image dtbs > "$B/kernel-m20lte-build.log" 2>&1 || {
	grep -nE '\*\*\*|error:|Error [0-9]|undefined reference' "$B/kernel-m20lte-build.log" | head -40
	exit 1
}
make -s -j"$(nproc)" O=out modules_prepare >> "$B/kernel-m20lte-build.log" 2>&1
# The M20's modules only need the kernel's own exports: building every =m
# module of defconfig just to get Module.symvers would take longer than the
# kernel. vmlinux.symvers is exactly those exports.
[ -s out/Module.symvers ] || cp out/vmlinux.symvers out/Module.symvers
# ACPM (how the AP reaches the PMIC on this SoC, patched by patch-kernel.py)
# is a module too: built on its own, which adds its exports to Module.symvers
# for zrn-s2mpu08-g3d, and shipped with the M20's modules.
make -s O=out drivers/firmware/samsung/acpm-protocol.ko >> "$B/kernel-m20lte-build.log" 2>&1 || {
	grep -nE 'error:|Error [0-9]' "$B/kernel-m20lte-build.log" | tail -20
	exit 1
}
IMAGE=$SRC/out/arch/arm64/boot/Image
DTB=$SRC/out/arch/arm64/boot/dts/exynos/exynos7904-m20lte.dtb
ls -l "$IMAGE" "$DTB"

echo "=== M20 modules ==="
make -s -C "$SRC" O="$SRC/out" M="$DEV/drivers" modules > "$B/modules-m20lte-build.log" 2>&1 || {
	tail -n 30 "$B/modules-m20lte-build.log"
	exit 1
}
cp out/drivers/firmware/samsung/acpm-protocol.ko "$DEV/drivers/"
ls -l "$DEV"/drivers/*.ko

echo "=== uniLoader ==="
if [ ! -d "$UNILOADER/.git" ]; then
	git clone -q "$UNILOADER_URL" "$UNILOADER"
fi
git -C "$UNILOADER" fetch -q origin "$UNILOADER_REV" 2>/dev/null || true
git -C "$UNILOADER" checkout -q -f "$UNILOADER_REV"
git -C "$UNILOADER" clean -qfdx
# The A30 is the M20's SoC and screen: its config, with this board file.
sed -e "s/^#define ZTOS_INFORM3.*/#define ZTOS_INFORM3	$INFORM3/" \
	-e "s/^#define ZTOS_WDT.*/#define ZTOS_WDT	${WDT:-0}/" \
	"$DEV/uniloader/board-m20lte.c" > "$UNILOADER/board/samsung/board-a30.c"
grep -q "ZTOS_INFORM3	$INFORM3" "$UNILOADER/board/samsung/board-a30.c"
mkdir -p "$UNILOADER/blob"
cp "$IMAGE" "$UNILOADER/blob/Image"
cp "$DTB" "$UNILOADER/blob/dtb"
printf '' | gzip -n > "$UNILOADER/blob/ramdisk"
(
	cd "$UNILOADER"
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE a30_defconfig
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE -j"$(nproc)"
) > "$B/uniloader-m20lte-build.log" 2>&1 || { tail -n 30 "$B/uniloader-m20lte-build.log"; exit 1; }
ls -l "$UNILOADER/uniLoader"

echo "=== boot image ==="
rm -f "$OUT.img"
python3 -I "$DEV/scripts/mkboot.py" --ref "$REF" --kernel "$UNILOADER/uniLoader" \
	--cmdline "console=tty0" -o "$OUT.img"
# BOOT is 32 MiB on the M20 (BOARD_BOOTIMAGE_PARTITION_SIZE), and uniLoader
# carries the kernel uncompressed: a config that grows past this has to lose
# built-in drivers, there is no compressing it.
BOOT_MAX=33554432
SIZE=$(stat -c %s "$OUT.img")
if [ "$SIZE" -gt "$BOOT_MAX" ]; then
	echo "the boot image is $SIZE bytes; BOOT holds $BOOT_MAX. Turn off more of defconfig." >&2
	exit 1
fi
cp "$SRC/out/.config" "$OUT.config"
cp "$SRC/$DTS" "$OUT.dts"
echo "$CMDLINE" > "$OUT.cmdline"
(cd "$ROOT/out" && sha256sum "$NAME.img" > "$NAME.img.sha256" && cat "$NAME.img.sha256")
echo "BUILD_OK $OUT.img fallback=$FALLBACK ram=${RAM}G watchdog=${WDT:-0}"
