#!/usr/bin/env python3
"""Test client for /dev/zrn-cam (zrn-m20cam): stream, read frame info, save a frame.

  sudo python3 zrn-cam-test.py [rear|rearfull] [SECONDS] [af N] [exp C G]

Reads struct zrn_cam_frame every 100 ms (the driver stops 3 s after the last
read), prints progress, then saves the newest frame to /tmp/zrn-cam-frame.raw
with its geometry in /tmp/zrn-cam-frame.txt.
"""
import mmap
import os
import struct
import sys
import time

mode = sys.argv[1] if len(sys.argv) > 1 else "rear"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3
extra = " ".join(sys.argv[3:]).split(",") if len(sys.argv) > 3 else []
FMT = "<9I"
NAMES = "seq index w h stride bayer errors stopped area".split()

fd = os.open("/dev/zrn-cam", os.O_RDWR)


def info():
    return dict(zip(NAMES, struct.unpack(FMT, os.read(fd, struct.calcsize(FMT)))))


t0 = time.time()
os.write(fd, mode.encode())
print("started %s in %.2f s" % (mode, time.time() - t0), flush=True)
for cmd in extra:
    cmd = cmd.strip()
    if cmd:
        try:
            os.write(fd, cmd.encode())
            print("sent", cmd)
        except OSError as e:
            print("command %r: %s" % (cmd, e))
last = None
end = time.time() + secs
while time.time() < end:
    i = info()
    if i["seq"] != (last or {}).get("seq"):
        last = i
    time.sleep(0.1)
i = info()
dt = time.time() - t0
print("frames %d in %.1f s, errors %d, stopped %d, %dx%d stride %d bayer %d area %d index %d" % (
    i["seq"], dt, i["errors"], i["stopped"], i["w"], i["h"], i["stride"], i["bayer"], i["area"], i["index"]))
if i["seq"]:
    m = mmap.mmap(fd, 2 * 4128 * 2 * 3096, mmap.MAP_SHARED, mmap.PROT_READ)
    base = i["index"] * i["area"]
    open("/tmp/zrn-cam-frame.raw", "wb").write(m[base:base + i["stride"] * i["h"]])
    open("/tmp/zrn-cam-frame.txt", "w").write("%d %d %d %d\n" % (i["w"], i["h"], i["stride"], i["bayer"]))
    print("saved frame %d (buffer %d)" % (i["seq"], i["index"]))
os.write(fd, b"stop")
time.sleep(0.3)
print("stopped:", info()["stopped"])
