#!/usr/bin/env python3
"""The Galaxy M20's camera master clocks (MCLK0..2), on or off, from user space.

  sudo mclk.py on|off CH        CH: 0 rear2 S5K5E9, 1 front S5K4HA, 2 rear S5K3L6

CMU_TOP (0x12060000, always on) has per channel a mux (0x1008 + 4*CH), a
divider (0x1810 + 4*CH) and a gate (0x2010 + 4*CH). Samsung's table
(cmucal-vclklut.c) leaves mux 0 (OSCCLK, 26 MHz) and divider 0; the gate's
reset value 0x200000 (CG_VAL, hardware control) gives no clock until MANUAL
(bit 20) is set as well. The pin is gpc0-CH (pin controller 0x139b0000, bank
gpc0 at 0x1c0): function 2 is MCLK, no pull (Samsung's fimc_is_mclkN_fn).
"""
import mmap
import os
import struct
import sys

CMU, PIN = 0x12060000, 0x139b0000
GPC0_CON, GPC0_PUD = 0x1c0, 0x1c8


def rw(base, size):
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    return mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)


def rd(m, o):
    return struct.unpack_from("<I", m, o)[0]


def wr(m, o, v):
    struct.pack_into("<I", m, o, v)


def field(v, ch, val):
    return (v & ~(0xf << (4 * ch))) | (val << (4 * ch))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("on", "off") or sys.argv[2] not in "012":
        sys.exit(__doc__)
    on, ch = sys.argv[1] == "on", int(sys.argv[2])
    cmu, pin = rw(CMU, 0x3000), rw(PIN, 0x1000)
    mux, div, gate = 0x1008 + 4 * ch, 0x1810 + 4 * ch, 0x2010 + 4 * ch
    if on:
        wr(cmu, mux, rd(cmu, mux) & ~1)          # OSCCLK
        wr(cmu, div, rd(cmu, div) & ~0x1f)       # /1
        wr(cmu, gate, 0x300000)                  # MANUAL | CG_VAL: running
        wr(pin, GPC0_PUD, field(rd(pin, GPC0_PUD), ch, 0))
        wr(pin, GPC0_CON, field(rd(pin, GPC0_CON), ch, 2))
    else:
        wr(pin, GPC0_CON, field(rd(pin, GPC0_CON), ch, 0))  # input
        wr(pin, GPC0_PUD, field(rd(pin, GPC0_PUD), ch, 1))  # pull-down, as at boot
        wr(cmu, gate, 0x200000)                  # back to hardware control
    print("MCLK%d %s: mux %#x div %#x gate %#x, gpc0 con %#x pud %#x" % (
        ch, sys.argv[1], rd(cmu, mux), rd(cmu, div), rd(cmu, gate),
        rd(pin, GPC0_CON), rd(pin, GPC0_PUD)))


main()
