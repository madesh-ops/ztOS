# Galaxy M20 cameras: bring-up notes

Sources: Samsung's M20 kernel (SamarV-121/android_kernel_samsung_m20lte,
`exynos7904-m20_camera.dtsi`, `exynos7904-m20_common.dtsi`,
`fimc-is-device-module-*.c`, `cal-if/exynos7885/cmucal-*.c`). The tools here run
from user space (GPIO character device, `/dev/mem`); nothing in the kernel yet.

| Camera | Sensor | I2C (7-bit) | bus (Samsung) | pins SDA/SCL | MCLK | reset | supplies (S2MPB03) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rear main | S5K3L6 13 MP | 0x2d | hsi2c_1 | gpc1-2/3 | 2 (gpc0-2) | gpg3-4 | LDO6 AVDD 2.8, LDO2 DVDD 1.05, LDO3 VDDIO 1.8, AF gpg1-5 |
| front | S5K4HA | 0x6c? (node @6C; to confirm) | hsi2c_0 | gpc1-0/1 | 1 (gpc0-1) | gpg3-5 | LDO5 AVDD 2.8, LDO1 DVDD 1.2, LDO3 |
| rear wide | S5K5E9 | 0x10 | hsi2c_2 | gpc1-4/5 | 0 (gpc0-0) | gpg2-5 | LDO7 AVDD 2.8, LDO4 DVDD 1.2, LDO3 |

- Camera PMIC: S2MPB03 at 0x56 on hsi2c_4 (gpc2-0/1). Registers 0x03-0x09 are
  LDO1-7: bit 7 on, bits 5:0 voltage (LDO1/2/4: 0.7 V + 12.5 mV steps; LDO3: 0.7 V
  + 25 mV; LDO5-7: 1.8 V + 25 mV). All off at boot.
- MCLK: CMU_TOP 0x12060000, mux 0x1008+4n, divider 0x1810+4n, gate 0x2010+4n.
  Samsung's table: mux 0 (OSCCLK 26 MHz), divider 0. The gate's 0x200000 gives no
  clock; 0x300000 (MANUAL) runs it. Pin gpc0-n function 2, no pull.
- Without MCLK the sensor does not answer on I2C.

## Results

- 2026-11-21: `rear-power.sh on` then `bbi2c /dev/gpiochip25 2 3 0x2d r16 0 4`:
  S5K3L6 model ID 0x30c6, revision 0xb0. Bus scan with power on: only 0x2d.
  `rear-power.sh off` restores every register and pin.

- 2026-11-21: the camera blocks. PMU CAM_STATUS (0x11c84084) reads 0x3000f: the
  camera and ISP power domains are on from the bootloader. CMU_CAM (0x14500000) is
  readable with CMU_TOP's CAM bus gate (0x1206200c) forced to 0x300000: bus clock
  from CMU_TOP (0x100 = 0x10), every Q-channel 0x3f0002 (clock request held, so
  CSIS and CSIS DMA clocks run). CSIS1 (rear, 0x14410000) answers: VERSION 0x05010001,
  the S9's CSIS v5.1 (0x05010000) one revision on, so camera/driver's receiver and
  DMA code is the starting point, not Samsung's v4.0. CMN_CTRL 0x4000, CLK_CTRL 0xf0.
  The rear PHY (PMU isolation 0x680) is still isolated.
- `drivers/zrn-m20cam.c` (built by CI's modules_only run): 24 MiB at bus address
  0xd9900000 in CMA, mmap through /dev/zrn-m20cam, filled with 0xa5.

- 2026-11-21: the S5K3L6 streams. `3l6-1052x780.txt` (Samsung setA Global + the
  1052x780 120 fps "FAST AE" mode + `sensor_3l6_cis_stream_on`'s sequence) through
  `bbi2c ... 0x2d batch`: frame counter 0x0005 went 0x0e, 0x28, 0x42 (26 frames per
  ~220 ms, about 118 fps), 0x034c/0x034e read back 1052 x 780, 4 lanes (module).
  It streams into the still isolated PHY; nothing receives it yet.
- Rear PHY (Samsung `phy-exynos-mipi.c`, `samsung,mipi-phy-s4`): PMU 0x11c80680 bit 0
  = 1 (isolation bypass), then SYSREG_CAM 0x14510840 bit 8 cleared and set (reset
  released). Front PHY: isolation 0x67c, reset bit 16; rear wide: 0x67c, bit 24.

- 2026-11-21: **CSIS1 receives the rear camera at 120 fps** (`csis-count.py`). The
  receiver uses Samsung's **v5.0 register layout** (matching VERSION 0x05010001),
  not the v4.0 one the M20 kernel config names: PHY_STATUS 0x700, PHY_CMN_CTRL 0x704
  (settle 24-31, S_CLKSETTLECTL 16-23 = 1, data lanes 1-4, clock lane 0); 0x20-0x3c
  read 0xdeadcafe. PHY tuning registers 0x708+ stay at reset (Samsung sets them only
  on the 8895). Sequence: PHY on (PMU 0x680, SYSREG 0x840 bit 8), CSIS reset, settle,
  4 lanes (CMN 0x4300), ch0 RAW10 (CFG0 0x10ac), RESOL0 0x030c041c, shadow update,
  clock lane on, CSI on, then sensor stream on. FRM_CNT_CH0 (0x100) +30 per 0.25 s.
  INT_SRC0 0x01000104 every frame: FRAMESTART ch0, but also ERR_ECC (bit 2) and
  ERR_LOST_FE ch0 (bit 8); INT_SRC1 0x1000. Settle 18-44 all give this; 6, 12 and 60
  add ERR_SOT_HS. So the errors are not settle: format, pixel mode, lane order or
  the sensor's extra data types are next to check, with a frame in memory.
  No freeze in any run; everything is undone after each.

- 2026-11-21: **first frame in memory** (`csis-capture.py`, picture
  `first-frame-colour-bars.png`, the sensor's colour bars). What it took, on top of
  `csis-count.py`:
  - PHY_CMN_CTRL bit 7 (S_BYTE_CLK_ENABLE), which Samsung's v5.0 driver always sets
    on the 7885 (`csi_hw_s_lane`): without it every frame had ERR_ECC and
    ERR_LOST_FE; with it INT_SRC0 is 0x01100000 (frame start + end), INT_SRC1 0.
    The M20's receiver is driven by `fimc-is-hw-csi-v5_0.c` (it has 7885 branches).
  - The CSIS-DMA common block at 0x14440000 (fimc_is reg 0): it boots with
    IP_PROCESSING 0 and SRAM split 0, and the channel aborts (ACT_CTRL bit 16).
    For CSIS1 alone (`csi_hw_s_dma_common_dynamic`, "DMA1 only"): ARB 2, CTRL |= 2,
    SRAM_SPLIT (320 << 16) | 4, MATRIX 0.
  - DMA0 of CSIS1: FMT 0 (2D, unpacked, legacy, RAW10), RESOL 1052, DMA_DATA_CTRL
    1, eight addresses, CTRL 0x3: on the 7885 DMA_N_DISABLE = 1 means enabled.
  - Result: DMA_SRC 0x111 each frame, 780 lines of 2112 bytes (1052 pixels, 16
    bits each, 10 bits low, padded to 2112), black 64, white 1023, Bayer **GRBG**.
  - The camera SysMMU (0x14540000) is secure: one read of it was an SError and a
    reboot. Its DMA is not translated (the frame lands at the physical address).
  - A scene with the default 120 fps exposure: all at black (64-65).
- 2026-11-21: **first photo** (`first-photo-window.png`): `csis-capture.py 26 exp
  7885` with `3l6-on-exp.txt` (coarse integration 0x0202 = 800 of 816 lines,
  analog gain 0x0204 = 0x80, 4x): median 120, highlights at 1023. Developed at half
  size (black 64, GRBG, grey-world gains R 1.34 B 1.25, gamma 2.2). Soft: the
  DW9808 focus actuator (0x18 on i2c ch 3) is not driven yet.

## Next

1. Front and rear-wide IDs (same steps, their supplies and pins).
2. Streaming: Samsung's S5K3L6 init tables (`fimc-is-cis-3l6-setA.h`), then the
   CSIS receivers (0x14510840 in the stock tree) and their DMA, as
   `camera/driver` does on the S9: risky (a write to a powered-off block hangs
   the bus), needs someone at the phone.
