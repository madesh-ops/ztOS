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

## Next

1. Front and rear-wide IDs (same steps, their supplies and pins).
2. Streaming: Samsung's S5K3L6 init tables (`fimc-is-cis-3l6-setA.h`), then the
   CSIS receivers (0x14510840 in the stock tree) and their DMA, as
   `camera/driver` does on the S9: risky (a write to a powered-off block hangs
   the bus), needs someone at the phone.
