# What the Galaxy Tab A 10.1 (2019) mainline work gives the M20

The SM-T510 ("gta3xl") is an Exynos 7904 like the M20. Mollomm1's tree,
<https://github.com/Mollomm1/linux-exynos7885>, branch `gta3xl` (Linux
6.15-rc1 base, updated October 2026), has mainline drivers for most of what
the M20 is missing. Same SoC, so the SoC side (DT nodes, clocks, ACPM, ABOX,
SCSC) carries over; the board side (panel, touch, amplifier count, sensors)
does not. postmarketOS itself has nothing newer for the M20: its Exynos 7885
devices (A20/A30/A40, jackpotlte) are archived ports on Samsung's 4.4 kernel.

Read on 2026-10-22. Ported since: ACPM and S2MPU08 BUCK5 (GPU DVFS), the TMU,
Wi-Fi and Bluetooth, and on 2026-10-23 sound through the speaker (step 3
below, without the COD3035X: `drivers/abox/`, `kernel/patches/`).

## What is there, and what it means for the M20

| Area | gta3xl branch | For the M20 |
|---|---|---|
| ACPM (the APM firmware's IPC) | `drivers/firmware/samsung/exynos-acpm.c`: Exynos 7885 channel layout, `samsung,exynos7885-acpm-ipc` + `samsung,exynos7885-acpm-mbox` at 0x11c40000 | The foundation for everything below: PMIC regulators, the codec (reached over SPEEDY through ACPM) and Wi-Fi's BUCK2 step (`set_wlbt_flag`). Mainline 7.1 has the gs101 version only |
| PMIC | `regulator/s2mpu08-acpm-buck`: BUCK2/3, LDO2/35 over ACPM | Same S2MPU08 PMIC |
| Display | `drivers/gpu/drm/exynos7885/`: DECON (DPU) with overlay/cursor planes, DSIM, System MMU, display off for sleep | Would replace simpledrm: real vsync instead of our scanout-line wait, the panel really off, no stale-cache workaround. Needs an HX83112A panel driver (the init/off commands are in `scripts/dsi.py`'s captures); gta3xl's panel is an HX8279D |
| Sound | `remoteproc/exynos7885_abox_rproc.c` (boots ABOX's Cortex-A7 with `calliope_sram.bin`/`calliope_dram.bin`, through `sysmmu_abox`), `sound/soc/samsung/exynos7885-abox-pcm.c` (RDMA playback, WDMA capture, UAIF0/UAIF3), `exynos7885-tfa9896.c` (card: TFA9896 speakers on UAIF3, COD3035X headphones/mic on UAIF0, jack), `codecs/cod3035x.c` (over ACPM), audio PLL in `clk-exynos7885.c` | Same ABOX, UAIF3 → TFA9896 speaker, COD3035X. The M20 has one TFA9896 at i2c_3 0x34 (gta3xl has two); our `tfa989x` TFA9896 support may overlap theirs (`ASoC: codecs: add SM-T510 TFA9896 bypass support`). Firmware: VENDOR's `calliope_*.bin` |
| Wi-Fi/BT | `drivers/misc/samsung/scsc*` + `drivers/net/wireless/scsc` (Samsung's stack, ported), `soc/samsung/exynos-scsc-wifibt-main.c` (ACPM prep, shared rail, TZASC, all gated: the driver ships inert) | Their experimental branch `gta3xl-scsc` got the R4 running and setting up its MPU, but its stores to the shared window never landed; suspects were the TZASC grant (SMC 0x82000710) and memory access control. `zrn-wlbt.c` now makes the TZASC call too. They never reached MM_START_IND |
| Cameras | `media/platform/samsung/exynos7885-csis`, `i2c/s5k-smia` (S5K4HA, S5K5E9), DW9808 VCM | The M20 has an S5K4HA and an S5K5E9 too (plus an S5K3L6 main camera) |
| Sensors | LIS2DS12 scale fix (`iio: accel: st_accel: LIS2DS12 has no axis enable bits`) | Applies if the M20's accelerometer turns out to be the LIS2DS at 0x1d. Their USI2 HSI2C nodes do not: the M20's sensors are on i2c_6 |
| CPU | cluster switching clocks, capacities, 1586 MHz A53 rate | Same SoC |

## Order to port in

Each step needs a flashed kernel and someone at the phone to test it.

1. **ACPM** (Exynos 7885 transport, then `devm_acpm_get_by_node()` users), as
   a `patch-kernel.py` step against 7.1's `exynos-acpm.c`. Test: read a PMIC
   register back. Risk: APM owns power; a wrong channel layout can wedge
   DVFS, so it should be tested with a recovery boot at hand.
2. **S2MPU08 regulators over ACPM**: read-only first.
3. **Sound**: audio PLL clocks, `sysmmu_abox` (mainline `exynos-iommu`), the
   ABOX remoteproc + PCM, `cod3035x`, then the machine driver with one
   TFA9896. Firmware from VENDOR. Their remoteproc only starts the firmware
   with `allow_firmware_start=1`, which suits a first test.
4. **Wi-Fi**: `zrn-wlbt.c`'s boot with the TZASC grant first (already in);
   if the R4 still cannot write the window, add `set_wlbt_flag` (BUCK2) and
   the shared-rail vote from their `exynos-scsc-shared-rail.c`.
5. **Display**: the DPU/DSIM driver plus an HX83112A panel driver. Biggest
   win for smoothness and power, and the most work.
6. **Cameras**, after display.

Their code is GPL-2.0; keep their authorship in anything taken from it.
