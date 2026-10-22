# ztOS on the Samsung Galaxy M20 (m20lte)

**Status: daily bring-up.** ztOS is installed on an SM-M205F (3 GB,
M205FDDS9CWA2 bootloader, replacement display) and boots to ztMETRO with touch,
GPU rendering, backlight, battery and USB networking. "Works" rows below were
seen on that phone; "Written" rows are untried.

The Galaxy M20 (SM-M205F/G/FN) runs an Exynos 7904, which is the Exynos 7885
die: mainline Linux already knows the SoC (clocks, pins, eMMC, UART, I2C) from
the Galaxy A8 2018 port, so unlike the S9 this port needs no out-of-tree SoC
drivers to boot. It needs a device tree, a few small driver changes, and the
same boot chain as the S9 (uniLoader in the BOOT partition, ztOS's
initramfs, the Arch Linux ARM root in USERDATA).

## Install

```
installer/ztos install --target m20lte
```

It needs TWRP for m20lte on the RECOVERY partition, adb, and the phone in
TWRP (Volume Up + Power). Read `installer/ztos guide --target m20lte` first.
Unlike the S9, everything is written from TWRP: the root image goes into
USERDATA over adb (`scripts/flash-root.sh`), so a first install never depends
on the new kernel's USB gadget working.

The phone's stock BOOT, RECOVERY, EFS and CPEFS are saved to `stock/m20lte/`
before anything is written. The boot image is built around the stock BOOT's
header and DTBH (`scripts/mkboot.py`, checked to reproduce the stock image
byte for byte), so sboot sees the layout it expects.

## What works, what doesn't

**Works**: tried on the phone, does what it should. **Written**: done, never
run. **Not started**: nothing yet, with why.

| Part | Chip | Status | How |
|---|---|---|---|
| Boot chain | sboot → uniLoader → Linux 7.1 | Works | uniLoader's Galaxy A30 board (same SoC, same 1080 x 2340 framebuffer at 0xec000000) with ztOS's failsafe added: `uniloader/board-m20lte.c` |
| Way back | PMU INFORM3 | Works | uniLoader arms "recovery" first thing, so a panic (`panic=5`), the initramfs deadman or `zboot.rescue_timeout` lands in TWRP. A Volume Down + Power reset clears INFORM3: hold Volume Up as well to get to TWRP |
| CPUs | 2x A73 + 6x A53 | Works (all 8 online) | mainline `exynos7885.dtsi`, PSCI. No CPU frequency scaling: the cores stay at the bootloader's clocks |
| Memory | 3 or 4 GB | Works (3 GB: 2685 MB usable) | `--ram 3\|4` (default 3, safe on both). Every carve-out from the stock tree (TIMA, modem, GNSS, Wi-Fi, audio DSP, sec_debug) is reserved, read from the stock BOOT's DTBH |
| Storage | eMMC 5.1 | Works (HS400) | mainline `dw-mshc` with the A8 2018's settings; root found by label `ZR_ARCH` |
| Display | HX83112A panel on DECON/DSIM | Works (bootloader framebuffer) | `simple-framebuffer` → simpledrm → `/dev/dri/card0`; ztMETRO draws on the GPU (below) and scans out here. No mainline DECON/DSIM driver for this SoC, so the panel can't be turned off or reconfigured from DRM; its commands can still be sent from userspace over `/dev/mem` (`ztos-m20-dsi`) |
| Brightness | TI LM3632 | Works | `drivers/zrn-lm3632-bl.c`: dims with the LM3632's own 11-bit current code (Samsung dims the panel over DSI; this needs no DSIM driver). Never touches the bias rails. simpledrm only blanks the picture when the screen goes off, so ztMETRO switches the backlight off itself there (`backlight_off()` in `ztmetro.c`) |
| Touch | Himax HX83112A (in-cell) | Works (multi-touch, on the bootloader's panel setup) | the firmware boots from the chip's flash but waits, never scanning or interrupting, until the host writes the HSEN (glove) flag `0x10007f14 = 0`, as Samsung's driver does on every resume (`himax_mcu_set_HSEN_enable`). The driver now does that in probe, after sense on and the TSP_RST pulse. No panel re-init is needed. The firmware leaves unused finger slots at (0, 0), not 0xffff, which the driver took for a touch in the corner (phantom touches in pinch zoom): those are skipped | mainline `himax_hx83112b` with the A's product id, sense on, the HSEN write and the empty-slot check added (`kernel/patch-kernel.py`), on USI0's second HS-I2C channel |
| Keys | Volume, Power | Works (Power turns the screen off and on) | `gpio-keys`, GPA1[5..7] |
| USB | DWC3 + "usb3p1" PHY | Works | device mode only: NCM network + ACM serial at 172.16.42.1, like the S9. The PHY is mainline's Exynos 850 code with the M20's PMU offset (0x674), its five clocks, PLL_USB at 50 MHz turned on before the PHY reset, the PIPE3 side forced idle, Samsung's HS tuning, and the controller never suspending the PHYs (each one was needed; `kernel/patch-kernel.py`, the `usbdrd` nodes) |
| Vibration | LDO on GPP0[3] | Probes | `gpio-vibrator`: the FF_RUMBLE device ztMETRO already uses |
| Battery level | S2MU004 fuel gauge | Works | `drivers/zrn-s2mu004-fg.c`, read-only: voltage, current, charge (5000 mAh design) |
| Motion sensor | LSM6DSL (or LIS2DS at 0x1d), YAS539 compass | Broken | i2c_6 times out waiting for the bus idle. Samsung's tree gives these sensors no regulator, so their supply is not the cause; the pins are muxed and pulled up as in Samsung's tree. Still to try: Samsung's `i2c-sda-delay = 100` and 400 kHz (mainline runs the bus at 65 kHz), and the LIS2DS at 0x1d. Don't read this controller's registers over `/dev/mem`: once the failed probe gates its clock, that hangs the SoC |
| Reboot to TWRP/download | PMU INFORM3 | Written | `syscon-reboot-mode`: `reboot recovery`, `reboot download` |
| Crash log | ramoops | Registers | 2 MiB at 0xeb800000 (unused in the stock tree); read at the next ztOS boot |
| Watchdog | WDT (A53 cluster) | Written, off | `--watchdog`; `kernel/patch-kernel.py` adds the 7885 variant (PMU bit 24). Off until the plain boot is known to work |
| Charging | S2MU004 charger, MUIC | Not started | no mainline driver. The charger keeps its bootloader settings, so the phone should still charge on a cable; nothing switches USB between host and device. Seen on 2026-10-22: on the computer's USB port the battery stays level (+1 mA at 52 %), so the bootloader's settings only carry the load. The Tab A 10.1 2019 tree has an S2MU005 charger driver to start from (`notes/gta3xl-mainline.md`) |
| GPU | Mali-G71 MP2 (r0p0) | Works: OpenGL ES 3.1 through Panfrost, with DVFS 343-845 MHz (a full-screen 1080x2340 shader: 92 FPS at 343 MHz, 281 FPS at 845 MHz) | the bootloader leaves the G3D power domain and clock on, and neither has a mainline driver: the G3D power domain stays the bootloader's. CMU_G3D (patch-kernel.py) runs the GPU from PLL_G3D and parks it on the switching path while the PLL relocks; Panfrost's devfreq (simple_ondemand) scales it over 343/450/545/676/845 MHz with BUCK5 at 600/612.5/650/681.25/762.5 mV over ACPM (zrn-acpm-mbox, acpm-protocol, zrn-s2mpu08-g3d, loaded at boot by modules-load.d/m20-gpu.conf). Levels and voltages are this chip's own: the ECT in sboot (mmcblk0boot0 at 0x44400), ASV table 2 and GPU group 9 (fuses read through EL3); each level was checked under load. No thermal throttling yet: the TMU has no driver here (the T510 tree has one). wlroots renders on renderD128 into simpledrm's dumb buffers, which simpledrm copies to the screen with the CPU; the Mali is not cache coherent, so `patch-kernel.py` makes that copy clean and invalidate the rows it reads (without it, moving tiles showed dashed streaks of stale lines). The panel scans that framebuffer on its own, so the copy also waits for the DSIM's line counter (`zerone,scanout-line` in the DT) and starts just above the changed rows, staying ahead of the scan: no tearing. The framebuffer is declared `x8r8g8b8`, the shell's own format, so the copy is a plain memcpy (converting to ARGB made it slower than the scan). AFBC is off for the whole session (`PAN_MESA_DEBUG=noafbc` in `os/overlay-m20lte/etc/ztos/session.env`): on this r0p0 G71 it left 16x16 blocks of black in Firefox's page tiles. **Known Mesa bug:** screen-space derivatives (`dFdx`, `dFdy`, `fwidth`) are wrong on this G71: `CLPER_OLD` returns the pixel's own value whatever lane it is asked for (tested with plain lane numbers, absolute and XOR lanes, either operand order). Firefox's WebRender and GTK 4 size their anti-aliased edges with them (rounded corners and shadows become blurs, labels nearly vanish), so both draw in software (`gfx.webrender.software` in Firefox's policy, `GSK_RENDERER=cairo`); ztMETRO and Chromium stay on the GPU | `gpu@11500000` (`arm,mali-bifrost`) and `clock-g3d` in the DT, `DRM_PANFROST` built in (`scripts/build-boot.sh`) |
| CPU frequency, thermal | ACPM / TMU | Not started | nothing for the 7885 in mainline. **The phone has no thermal throttling under ztOS**: watch for heat |
| Sound | ABOX + COD3035X codec + TFA9896 amp | Not started (hardware reached) | the speaker is ABOX's UAIF3 into an NXP TFA9896 on i2c_3 at 0x34 (revision 0x1b96 read back; the bus is enabled in the DT). ABOX answers at 0x14a50000 (version 1.21). Earpiece, jack and microphones go through the COD3035X, which sits on Samsung's SPEEDY bus: no mainline driver. The TFA9896 is in `tfa989x` (`patch-kernel.py`). Plan: port the Galaxy Tab A 10.1 2019's working mainline stack (same SoC: ACPM, ABOX remoteproc and PCM, COD3035X over ACPM, a TFA9896 card), in the order `notes/gta3xl-mainline.md` gives |
| Modem | Shannon, shared memory + mailbox | Not started | the S9's `modem/` (Samsung's modem_v1 with a mainline shim) is the starting point; the M20's CP uses the same modem_v1 with `exynos-shd-ipc-mailbox` at 0x12080000 (Samsung's M20 tree) |
| Wi-Fi, Bluetooth, FM | Samsung SCSC "Maxwell" (S612 radio) | Wi-Fi works (2.4 GHz, as the M20 is specified); Bluetooth works (scans, BlueZ) | `drivers/zrn-wlbt.c` boots the stock firmware (VENDOR `etc/wifi/mx140.bin` as `/lib/firmware/scsc/mx140.bin`, plus `mx140_wlan.hcf` beside it) and provides Samsung's SCSC core interface; `drivers/scsc-wlan` is Samsung's WLAN driver ported to 7.1, loaded at boot. NetworkManager connects on `wlan0` (`p2p0` is left unmanaged). The S612 and its A40-based configuration offer 5 GHz, but the M20 is 2.4 GHz only (no 5 GHz path on the board: 5 GHz networks are heard at about -88 dBm), so the driver offers 2.4 GHz only (`scsc_wlan.band_5g=1` to override). After a warm reboot the subsystem is left powered; `zrn-wlbt` recovers it on the service open. Bluetooth: `drivers/scsc-bt` is Samsung's BT driver ported to 7.1 with its BlueZ bridge (hci0 registered at probe, loaded at boot by modules-load.d/m20-bluetooth.conf); BlueZ powers it, and the BT service runs on the subsystem's M4 alongside Wi-Fi. Its config is VENDOR's `etc/wifi/mx140_bt.hcf` beside the firmware; its address is EFS's `bluetooth/bt_addr`, installed as `/lib/firmware/scsc/csa/bluetooth/.bd_addr` in Samsung's NAP UAP LAP form (without it, one from the chip's unique ID). No Bluetooth audio offload (the M20 has no BT-ABOX window) |
| Internet | | Works, through the computer over USB | `scripts/usb-internet.sh` on the computer opens a reverse SSH tunnel: a SOCKS proxy at 127.0.0.1:1080 on the phone, used by everything through `/etc/environment` (sudo keeps it), Firefox's policy and Chromium's flags (`rootfs/overlay`). Programs that resolve names themselves, without the proxy, get no DNS |
| Cameras, flash | S5K3L6, S5K4HA, S5K5E9 | Not started | no mainline Exynos camera ISP driver |
| Proximity/light, compass, grip | GP2AP110S, YAS, TC3xx | Not started | no matching mainline drivers |
| Fingerprint, NFC | | Not started | |
| Suspend | | Not started | `AllowSuspend=no`, as on the S9 before its own work |

## First boot: what to try, in order

Each step needs the one before it. Build with `--serial` too if you have a
Samsung UART cable for the earphone jack.

1. **Kernel on screen.** The kernel log should scroll over the bootloader's
   picture (fbcon on simpledrm, `console=tty0`). If the screen stays on the
   Samsung logo, the kernel did not get as far as the framebuffer: try once
   more with `--no-touch`, then send the build's `out/*.config` and
   `out/*.dts`.
2. **The initramfs.** `zboot|` lines in the log; it looks for `ZR_ARCH` on the
   eMMC. If it can't find it, it drops to the rescue shell on screen and USB.
3. **USB.** Plug into a Linux computer: a "Samsung Galaxy M20 zboot rescue"
   network device should appear, and `telnet 172.16.42.1` works in rescue.
   If not: `dmesg | grep -i -E 'dwc3|usbdrd|udc'` (on screen, or saved to
   `/run/zboot.dmesg` once the root is up).
4. **ztMETRO.** greetd starts the session on the framebuffer, and touch works
   from boot. If it does not: `grep hx83112a /proc/interrupts` should count up
   while you touch, and `dmesg | grep -i himax` should show no "Failed to
   start touch scanning". For the display side, `sudo ztos-m20-dsi init`
   sends Samsung's whole panel init (the screen blinks), and
   `sudo ztos-m20-dsi cmd XX YY` sends one DCS command; in the repository the
   tool is `devices/m20lte/scripts/dsi.py`.
5. **The rest**: `cat /sys/class/power_supply/battery/capacity`,
   `ls /sys/class/backlight/`, `ls /sys/bus/iio/devices/`, and
   ZrnDevTest's sensor page.

If the phone hangs, hold **Volume Down + Power** for about 7 seconds. It
should restart into TWRP, because uniLoader set INFORM3 to recovery; whether
a forced reset keeps INFORM3 on this phone is one of the things to find out
(if it doesn't, hold Volume Up + Power as well to reach TWRP). Reboot to
"System" from TWRP: if memory survived the reset, that ztOS boot saves the
hung boot's kernel log to `/var/lib/ztos-pstore/`.

## What is where

| | |
|---|---|
| `kernel/exynos7904-m20lte.dts` | the device tree: mainline `exynos7885.dtsi` + the M20, every node citing its source |
| `kernel/make-dts.py` | `--ram 3\|4`, `--watchdog`, `--no-touch` |
| `kernel/patch-kernel.py` | the driver changes (USB PHY; touch: id, sense on, HSEN start, empty slots; simpledrm: cache maintenance and scanout-following copies; watchdog), idempotent, every anchor asserted |
| `uniloader/board-m20lte.c` | uniLoader's board, with the failsafe |
| `drivers/` | `zrn-lm3632-bl` (backlight), `zrn-s2mu004-fg` (battery) and `zrn-wlbt` (Wi-Fi/BT subsystem boot), built against the kernel by `build-boot.sh` |
| `notes/` | findings: the GPU's derivative bug, Bluetooth's protocol, what the Tab A 10.1 2019 mainline tree offers and the port order |
| `scripts/build-boot.sh` | the boot image: kernel, modules, uniLoader, `out/<name>.img` |
| `scripts/backup.sh`, `flash.sh`, `flash-root.sh` | everything the installer does to the phone, through TWRP |
| `scripts/mkboot.py` | Samsung boot image packer (`--info` reads one) |
| `scripts/dsi.py`, `scripts/m20_init_seq.py` | the panel's commands from userspace over `/dev/mem` (Samsung's init table, or one DCS command), for the display side only: touch does not need them; installed as `ztos-m20-dsi` |
| `scripts/usb-internet.sh` | run on the computer: internet for the phone through a reverse SSH tunnel |
| `rootfs/` | the system files: `from-s9` lists what is shared with the S9, `overlay/` is the M20's own (proxy settings, browser settings, backlight rule) |

Sources for every hardware detail: Samsung's M20 kernel
([LineageOS/android_kernel_samsung_universal7904](https://github.com/LineageOS/android_kernel_samsung_universal7904)),
the stock M205FDDU8CUI1 BOOT image's device tree, and mainline Linux.
