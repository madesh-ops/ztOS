#!/usr/bin/env python3
"""Galaxy M20 rear camera: does CSIS1 receive the S5K3L6's frames? No DMA.

  sudo python3 -u csis-count.py

Stages, each printed before it runs (a hang shows where):
  A  rear camera power on, sensor global + 1052x780 mode (not streaming)
  B  PHY: PMU 0x11c80680 bit 0 = 1 (isolation bypass), SYSREG_CAM 0x14510840
     bit 8 cleared then set (reset released)           (phy-exynos-mipi.c)
  C  CSIS1 (0x14410000) software reset                  (csi_hw_reset)
  D  settle 26, 4 data lanes, channel 0 RAW10 1052x780  (csi_hw_s_settle/_lane/_config;
     settle, lanes and 1196 Mbps from fimc-is-device-module-3l6.c)
  E  shadow update, D-PHY clock lane on, CSI on        (csi_hw_enable)
  F  sensor stream on; for 2 s read FRM_CNT_CH0 (0x100), INT_SRC0/1, PHY_STATUS
  G  undo all of it: CSI and D-PHY off, sensor off and powered down, PHY reset
     and isolated, CMU_TOP CAM bus gate back to hardware control
No interrupt is unmasked at the GIC (no driver owns SPI 298), and no DMA
channel is enabled, so nothing is written to memory.
"""
import mmap
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)


def region(base, size):
    return mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)


def rd(m, o):
    return struct.unpack_from("<I", m, o)[0]


def wr(m, o, v):
    struct.pack_into("<I", m, o, v & 0xffffffff)


def say(s):
    print(s, flush=True)


def sensor(f):
    subprocess.run([HERE + "/bbi2c", "/dev/gpiochip25", "2", "3", "0x2d", "batch", HERE + "/" + f], check=True)


W, H, RAW10 = 1052, 780, 0x2B
SETTLE = int(sys.argv[1]) if len(sys.argv) > 1 else 26
top = region(0x12060000, 0x3000)
pmu = region(0x11c80000, 0x1000)
sysreg = region(0x14510000, 0x1000)
csis = region(0x14410000, 0x2000)


def stage_g():
    say("G: undo")
    v = rd(csis, 0x704)
    wr(csis, 0x704, v & ~0x1f)                       # D-PHY clock and data lanes off
    wr(csis, 0x04, rd(csis, 0x04) & ~1)              # CSI off
    try:
        sensor("3l6-off.txt")
    except Exception as e:
        say("   sensor off failed: %s" % e)
    subprocess.run([HERE + "/rear-power.sh", "off"], stdout=subprocess.DEVNULL)
    wr(sysreg, 0x840, rd(sysreg, 0x840) & ~(1 << 8))  # PHY reset
    wr(pmu, 0x680, rd(pmu, 0x680) & ~1)              # PHY isolated
    wr(top, 0x200c, 0x200000)
    say("   CMN %#x DPHY %#x, PMU 0x680 %#x, SYSREG 0x840 %#x" % (
        rd(csis, 0x04), rd(csis, 0x704), rd(pmu, 0x680), rd(sysreg, 0x840)))


say("A: rear camera on, sensor init (not streaming)")
subprocess.run([HERE + "/rear-power.sh", "on"], check=True, stdout=subprocess.DEVNULL)
sensor("3l6-init.txt")
try:
    wr(top, 0x200c, 0x300000)                        # CAM bus clock held on
    say("B: PHY isolation bypass and reset release (PMU 0x680 %#x, SYSREG 0x840 %#x)" % (
        rd(pmu, 0x680), rd(sysreg, 0x840)))
    wr(pmu, 0x680, rd(pmu, 0x680) | 1)
    v = rd(sysreg, 0x840)
    wr(sysreg, 0x840, v & ~(1 << 8))
    wr(sysreg, 0x840, v | (1 << 8))
    say("   now PMU 0x680 %#x, SYSREG 0x840 %#x" % (rd(pmu, 0x680), rd(sysreg, 0x840)))

    say("C: CSIS1 software reset (CMN %#x)" % rd(csis, 0x04))
    wr(csis, 0x04, rd(csis, 0x04) | 2)
    for _ in range(100):
        if not rd(csis, 0x04) & 2:
            break
        time.sleep(0.0001)
    say("   CMN %#x" % rd(csis, 0x04))

    say("D: settle %d, 4 lanes, ch0 RAW10 %dx%d" % (SETTLE, W, H))
    d = rd(csis, 0x704)                              # PHY_CMN_CTRL (v5.0 layout)
    d = (d & ~(0xff << 24) & ~(0xff << 16) & ~(0xf << 1)) | (SETTLE << 24) | (1 << 16) | (1 << 7) | (0xf << 1)  # bit 7 S_BYTE_CLK_ENABLE: always on the 7885 (csi_hw_s_lane, v5.0)
    wr(csis, 0x704, d)
    c = rd(csis, 0x04)
    c = (c & ~(3 << 8) & ~(1 << 12) & ~(3 << 10)) | (3 << 8)
    wr(csis, 0x04, c)
    cfg = rd(csis, 0x40)
    cfg = (cfg & ~3 & ~(0x3f << 2) & ~(1 << 11) & ~(1 << 12)) | (RAW10 << 2) | (1 << 12)
    wr(csis, 0x40, cfg)
    wr(csis, 0x44, (H << 16) | W)
    say("   CMN %#x DPHY %#x CFG0 %#x RESOL0 %#x" % (rd(csis, 0x04), rd(csis, 0x704), rd(csis, 0x40), rd(csis, 0x44)))

    say("E: shadow update, D-PHY clock lane on, CSI on")
    wr(csis, 0x04, rd(csis, 0x04) | (0xf << 16))
    wr(csis, 0x704, rd(csis, 0x704) | 1)
    wr(csis, 0x04, rd(csis, 0x04) | 1)
    say("   CMN %#x DPHY %#x PHY_STATUS %#x FRM_CNT0 %#x" % (
        rd(csis, 0x04), rd(csis, 0x704), rd(csis, 0x700), rd(csis, 0x100)))

    say("F: sensor stream on, then 2 s of counters")
    wr(csis, 0x14, 0xffffffff)                       # clear latched sources
    wr(csis, 0x1c, 0xffffffff)
    sensor("3l6-on.txt")
    for i in range(8):
        time.sleep(0.25)
        say("   t=%.2f FRM_CNT0 %d  SRC0 %#010x SRC1 %#010x PHY_STATUS %#x" % (
            (i + 1) * 0.25, rd(csis, 0x100), rd(csis, 0x14), rd(csis, 0x1c), rd(csis, 0x700)))
        wr(csis, 0x14, 0xffffffff)
        wr(csis, 0x1c, 0xffffffff)
finally:
    stage_g()
say("done")
