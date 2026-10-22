#!/usr/bin/env python3
"""dsi.py: send the Galaxy M20's panel commands straight to its DSI controller, from /dev/mem.

  dsi.py touch         only the panel's touch settings (SEQ_SET_GIP_E7_*_TOUCH,
                       D8_03_LPWG), with the bank each one lives in: the part the
                       bootloader does not send; no sleep-out, so the screen stays on
  dsi.py init          Samsung's whole panel initialisation (hx83112a_init),
                       sleep-out included: a fresh power-on, the screen blinks
  dsi.py cmd XX YY ..  one DCS command (bytes in hex)

The M20's display runs on the bootloader's setup, with no display driver in the
kernel (simpledrm only scans out the framebuffer). The panel is a Himax HX83112A,
display and touch in one chip. Touch does not need any of this: the bootloader's
panel setup is enough once the touch driver writes the firmware's HSEN flag
(kernel/patch-kernel.py). This is a diagnostic tool for the display side. Run
as root.

The 7885's DSIM (0x14870000, dpu_7885/regs-dsim.h) takes a command as payload
words in PAYLOAD (0x5c), then the packet header in PKTHDR (0x58), with the
header FIFO's empty flag in FIFOCTRL (0x68) bit 10: the same as the S9's
(scripts/dev/dsim.py). The video stream keeps running; the DSIM sends
commands in the blanking.
"""
import ctypes
import mmap
import os
import struct
import sys
import time

# In the repository the table sits next to this file; on the phone this is
# installed as /usr/local/sbin/ztos-m20-dsi with the table in
# /usr/local/lib/ztos (installer/ztos_installer/build.py).
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(1, "/usr/local/lib/ztos")
from m20_init_seq import INIT  # noqa: E402

DSIM = 0x14870000
PKTHDR, PAYLOAD, FIFOCTRL = 0x58, 0x5C, 0x68
FIFO_EMPTY_PH = 1 << 10
DCS_SHORT, DCS_SHORT_PARAM, DCS_LONG = 0x05, 0x15, 0x39

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, offset=DSIM)


# One 32-bit load or store per access. struct on an mmap goes through memcpy,
# which on arm64 stores a 4-byte value twice: every PKTHDR/PAYLOAD word then
# lands in the FIFO twice and long packets reach the panel corrupted.
def rd(o):
    return ctypes.c_uint32.from_buffer(m, o).value


def wr(o, v):
    ctypes.c_uint32.from_buffer(m, o).value = v & 0xFFFFFFFF


def fifo_idle():
    for _ in range(20000):
        if rd(FIFOCTRL) & FIFO_EMPTY_PH:
            return True
        time.sleep(0.00005)
    return False


def send(data):
    if not fifo_idle():
        raise SystemExit(f"DSIM header FIFO never emptied (FIFOCTRL={rd(FIFOCTRL):#x})")
    if len(data) == 1:
        wr(PKTHDR, DCS_SHORT | data[0] << 8)
    elif len(data) == 2:
        wr(PKTHDR, DCS_SHORT_PARAM | data[0] << 8 | data[1] << 16)
    else:
        padded = data + b"\0" * (-len(data) % 4)
        for i in range(0, len(padded), 4):
            wr(PAYLOAD, struct.unpack_from("<I", padded, i)[0])
        wr(PKTHDR, DCS_LONG | (len(data) & 0xFF) << 8 | (len(data) >> 8) << 16)
    fifo_idle()
    time.sleep(0.001)


def init():
    for name, data in INIT:
        if name == "SEQ_HX83112A_BL":
            data = bytes([0x51, 0x0F, 0xFF])  # Samsung sends 0 and raises it later: keep the screen lit
        send(data)
        if name == "SEQ_SLEEP_OUT":
            time.sleep(0.12)                  # the panel's own 120 ms before display on
    send(bytes([0x29]))                       # display on
    print(f"sent {len(INIT)} panel commands and display on")


# The touch side's own settings, each with the bank register it goes under:
# Samsung sends them in the middle of the init (E7 in banks 0, 1 and 2, D8 in
# bank 3). Sending just these leaves the display's configuration as the
# bootloader left it, and only tells the touch firmware to run.
TOUCH_BANKS = {
    "SEQ_SET_GIP_E7_00_TOUCH": 0,
    "SEQ_SET_GIP_E7_01_TOUCH": 1,
    "SEQ_SET_GIP_E7_02_TOUCH": 2,
    "SEQ_SET_GIP_D8_03_LPWG": 3,
}


def touch():
    missing = set(TOUCH_BANKS) - {name for name, _ in INIT}
    if missing:
        raise SystemExit(f"the init table lost {', '.join(sorted(missing))}")
    sent = 0
    for name, data in INIT:
        if name in TOUCH_BANKS:
            send(bytes([0xbd, TOUCH_BANKS[name]]))   # bank select
            send(data)
            sent += 1
    send(bytes([0xbd, 0x00]))
    print(f"sent {sent} touch settings")


if __name__ == "__main__":
    if sys.argv[1:2] == ["touch"]:
        touch()
    elif sys.argv[1:2] == ["init"]:
        init()
    elif sys.argv[1:2] == ["cmd"]:
        send(bytes(int(x, 16) for x in sys.argv[2:]))
        print("sent", " ".join(sys.argv[2:]))
    else:
        raise SystemExit(__doc__)
