#!/usr/bin/env python3
"""Galaxy M20 rear camera: one frame from CSIS1 by DMA into zrn-m20cam's buffer.

  sudo python3 -u csis-capture.py [SETTLE] [bars|scene]

zrn-m20cam must be loaded (its buffer: /dev/zrn-m20cam, bus address in debugfs).
DMA channel 0 (CSIS + 0x1000, v5.0 layout): FMT 0 (2D, unpacked, legacy, RAW10),
RESOL 1052, all eight frame addresses = the buffer, DMA_DATA_CTRL input path 1;
on the 7885 DMA_N_DISABLE set means enabled (Samsung's csi_hw_s_output_dma).
The camera SysMMU (0x14540000) is secure: reading it is an SError. Its state is
thus unknown; the 0xa5 fill outside the written area shows where DMA went.

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
    subprocess.run([HERE + "/bbi2c", "/dev/gpiochip25", SBUS[0], SBUS[1], "0x2d", "batch", HERE + "/" + f], check=True)


FRONT = os.environ.get("CAM") == "front"
# rear: S5K3L6, CSIS1, PHY m0s4A (PMU 0x680, SYSREG_CAM 0x840 bit 8), "DMA1 only";
# front: S5K4HA, CSIS0, PHY m0s4B (PMU 0x67c, bit 16), "DMA0 only" (csi_hw_s_dma_common_dynamic)
CSIS_BASE, ISO, RST, SBUS, SPFX, PWR = ((0x14400000, 0x67c, 16, ("0", "1"), "4ha", "front-power.sh") if FRONT
    else (0x14410000, 0x680, 8, ("2", "3"), "3l6", "rear-power.sh"))
W, H, RAW10 = (1632, 1224, 0x2B) if FRONT else (1052, 780, 0x2B)
SETTLE = int(sys.argv[1]) if len(sys.argv) > 1 else (14 if FRONT else 26)
BARS = (sys.argv[2] if len(sys.argv) > 2 else "bars") == "bars"
POL7885 = (sys.argv[3] if len(sys.argv) > 3 else "v5") == "7885"   # v5: DISABLE bit 0 = enabled
AF = int(sys.argv[4]) if len(sys.argv) > 4 else None   # DW9808 lens position 0 (infinity) .. 1023 (macro)
PHYS = int(open("/sys/kernel/debug/zrn-m20cam/phys").read(), 16)
BUF = mmap.mmap(os.open("/dev/zrn-m20cam", os.O_RDWR), 24 << 20)
BUF[:] = bytes([0xa5]) * (24 << 20)
top = region(0x12060000, 0x3000)
pmu = region(0x11c80000, 0x1000)
sysreg = region(0x14510000, 0x1000)
csis = region(CSIS_BASE, 0x2000)
dmac = region(0x14440000, 0x1000)                   # CSIS-DMA common (fimc_is reg 0)


def stage_g():
    say("G: undo")
    wr(dmac, 0x8, rd(dmac, 0x8) & ~2)                # common DMA clock path back off
    wr(csis, 0x1000, 0 if POL7885 else 1)            # DMA0 off
    v = rd(csis, 0x704)
    wr(csis, 0x704, v & ~0x1f)                       # D-PHY clock and data lanes off
    wr(csis, 0x04, rd(csis, 0x04) & ~1)              # CSI off
    try:
        sensor(SPFX + "-off.txt")
    except Exception as e:
        say("   sensor off failed: %s" % e)
    subprocess.run([HERE + "/" + PWR, "off"], stdout=subprocess.DEVNULL)
    wr(sysreg, 0x840, rd(sysreg, 0x840) & ~(1 << RST))  # PHY reset
    wr(pmu, ISO, rd(pmu, ISO) & ~1)              # PHY isolated
    wr(top, 0x200c, 0x200000)
    say("   CMN %#x DPHY %#x, PMU 0x680 %#x, SYSREG 0x840 %#x" % (
        rd(csis, 0x04), rd(csis, 0x704), rd(pmu, ISO), rd(sysreg, 0x840)))


say("A: rear camera on, sensor init (not streaming)")
subprocess.run([HERE + "/" + PWR, "on"], check=True, stdout=subprocess.DEVNULL)
sensor(SPFX + "-init.txt")


def af(*args):
    # the DW9808 at 0x0c (Samsung's 0x18 is the 8-bit form) on camera I2C 3, gpc1-6/7
    subprocess.run([HERE + "/bbi2c", "/dev/gpiochip25", "6", "7", "0x0c", "w"] + [str(a) for a in args], check=True)


if AF is not None:
    say("A2: focus actuator init, lens to %d" % AF)
    af(0x02, 0x01); af(0x02, 0x00); time.sleep(0.005)       # power-down out, wait 5 ms
    af(0x02, 0x02)                                           # ringing control
    af(0x06, 0x61); af(0x07, 0x36); af(0x0a, 0xff)           # SAC3, Tvib x1; SACT; preset
    af(0x03, AF >> 8, AF & 0xff)                             # position
    time.sleep(0.05)
try:
    wr(top, 0x200c, 0x300000)                        # CAM bus clock held on
    say("B: PHY isolation bypass and reset release (PMU 0x680 %#x, SYSREG 0x840 %#x)" % (
        rd(pmu, ISO), rd(sysreg, 0x840)))
    wr(pmu, ISO, rd(pmu, ISO) | 1)
    v = rd(sysreg, 0x840)
    wr(sysreg, 0x840, v & ~(1 << RST))
    wr(sysreg, 0x840, v | (1 << RST))
    say("   now PMU 0x680 %#x, SYSREG 0x840 %#x" % (rd(pmu, ISO), rd(sysreg, 0x840)))

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

    say("E2: DMA0 -> %#x, 1052 wide, RAW10 unpacked" % PHYS)
    wr(csis, 0x1004, 0)
    wr(csis, 0x1040, W)
    wr(csis, 0x1400, (rd(csis, 0x1400) & ~1) | 1)
    for i in range(8):
        wr(csis, 0x1010 + 4 * i, PHYS)
    wr(csis, 0x1408, 0xffffffff)
    say("   CSIS-DMA common before: ARB %#x CTRL %#x SPLIT %#x MATRIX %#x" % (
        rd(dmac, 0x0), rd(dmac, 0x8), rd(dmac, 0xc), rd(dmac, 0x10)))
    # csi_hw_s_dma_common_dynamic for "DMA1 only" (CSIS1): CSIS1 first in arbitration,
    # Q-channel clock on (IP_PROCESSING), SRAM0 split 4 (CSIS1 gets the rest of SRAM0
    # in matrix 0), SRAM1 split 320, matrix 0
    wr(dmac, 0x0, (rd(dmac, 0x0) & ~3) | (1 if FRONT else 2))
    wr(dmac, 0x8, rd(dmac, 0x8) | 2)
    wr(dmac, 0xc, (320 << 16) | (640 if FRONT else 4))
    wr(dmac, 0x10, 0)
    say("   after: ARB %#x CTRL %#x SPLIT %#x MATRIX %#x" % (rd(dmac, 0x0), rd(dmac, 0x8), rd(dmac, 0xc), rd(dmac, 0x10)))
    say("   DMA_CMN_CTRL %#x -> 0 (no abort request)" % rd(csis, 0x1414))
    wr(csis, 0x1414, 0)
    wr(csis, 0x1000, (1 << 1) | (1 if POL7885 else 0))  # UPDT_PTR_EN + enable
    say("   CTRL %#x ACT_CTRL %#x FMT %#x RESOL %#x DATA_CTRL %#x ADDR1 %#x" % (rd(csis, 0x1000),
        rd(csis, 0x1030), rd(csis, 0x1004), rd(csis, 0x1040), rd(csis, 0x1400), rd(csis, 0x1010)))
    say("F: sensor stream on, then 1 s of counters")
    wr(csis, 0x14, 0xffffffff)                       # clear latched sources
    wr(csis, 0x1c, 0xffffffff)
    sensor(SPFX + ("-on-bars.txt" if BARS else ("-on.txt" if sys.argv[2] == "scene" else "-on-%s.txt" % sys.argv[2])))
    for i in range(4):
        time.sleep(0.25)
        say("   t=%.2f FRM_CNT0 %d SRC0 %#010x SRC1 %#010x DMA_SRC %#010x DMA_ERR %#x ACT_CTRL %#x" % (
            (i + 1) * 0.25, rd(csis, 0x100), rd(csis, 0x14), rd(csis, 0x1c), rd(csis, 0x1408),
            rd(csis, 0x1418), rd(csis, 0x1030)))
        wr(csis, 0x14, 0xffffffff)
        wr(csis, 0x1c, 0xffffffff)
        wr(csis, 0x1408, 0xffffffff)
    wr(csis, 0x1000, 0 if POL7885 else 1)            # DMA off before anything else
    time.sleep(0.05)
    data = BUF[:]
    fill = bytes([0xa5])
    first = len(data) - len(data.lstrip(fill))
    last = len(data.rstrip(fill))
    first = None if first == len(data) else first
    say("   buffer: first changed byte %s, last changed byte %s (of %d)" % (first, last - 1 if first is not None else None, len(data)))
    if first is not None:
        import array
        g = array.array("H")
        g.frombytes(data[:780 * 2112])
        score = 0
        for y in range(195, 585, 3):                         # centre half, every third line
            row = g[y * 1056 + 263: y * 1056 + 789]
            score += sum(abs(row[i + 2] - row[i]) for i in range(0, len(row) - 2, 2))
        say("   sharpness %d" % score)
        open("/tmp/m20cam-frame%s.raw" % ("" if AF is None else "-af%d" % AF), "wb").write(data[:4 << 20])
        say("   saved the first 4 MiB to /tmp/m20cam-frame.raw")
finally:
    stage_g()
say("done")
