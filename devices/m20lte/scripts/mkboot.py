#!/usr/bin/env python3
"""Pack a Galaxy M20 boot image around uniLoader.

  mkboot.py --ref stock/m20lte/BOOT.img --kernel uniLoader [--ramdisk r.gz]
            [--cmdline "..."] -o out.img
  mkboot.py --info some.img

sboot on the M20 takes the classic Android boot image (header v0) with a
Samsung device-tree section after the second stage: the "dt_size" word at
offset 40, where later headers keep their version (BOARD_KERNEL_SEPARATED_DT
in LineageOS's m20lte tree). The DTBH blob in that section is what sboot
reads the board revision from, so it is copied unchanged from the phone's
own BOOT image; the device tree the kernel runs with is uniLoader's.

Everything else (load addresses, page size, os version) is taken from the
reference too, so the image differs from stock only in what it carries. The
"SEANDROIDENFORCE" trailer is appended as Samsung's tools do; an unlocked
sboot boots without it, but warns.
"""
import argparse
import gzip
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"ANDROID!"
HDR = struct.Struct("<8s10I16s512s32s1024s")   # 1632 bytes
TRAILER = b"SEANDROIDENFORCE"


def parse(blob: bytes) -> dict:
    if blob[:8] != MAGIC:
        sys.exit("mkboot: not an Android boot image")
    (magic, ksize, kaddr, rsize, raddr, ssize, saddr, taddr, page, dtsize, osver,
     name, cmdline, ident, extra) = HDR.unpack_from(blob)
    if page not in (2048, 4096):
        sys.exit(f"mkboot: page size {page}?")
    if dtsize > 64 * 1024 * 1024:
        sys.exit(f"mkboot: the word at offset 40 ({dtsize:#x}) is not a dt size: "
                 "is this a newer header version, not the M20's?")
    pad = lambda n: (n + page - 1) // page * page
    off = page
    sections = {}
    for key, size in (("kernel", ksize), ("ramdisk", rsize), ("second", ssize), ("dt", dtsize)):
        sections[key] = blob[off:off + size]
        off += pad(size)
    return dict(kaddr=kaddr, raddr=raddr, saddr=saddr, taddr=taddr, page=page, osver=osver,
                name=name, cmdline=cmdline.rstrip(b"\0"), extra=extra.rstrip(b"\0"),
                end=off, **sections)


def pack(ref: dict, kernel: bytes, ramdisk: bytes, cmdline: bytes) -> bytes:
    page = ref["page"]
    second, dt = ref["second"], ref["dt"]
    if len(cmdline) > 511:
        sys.exit("mkboot: command line longer than 511 bytes")
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, second):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    if dt:
        sha.update(dt)
        sha.update(struct.pack("<I", len(dt)))
    ident = sha.digest()
    hdr = HDR.pack(MAGIC, len(kernel), ref["kaddr"], len(ramdisk), ref["raddr"], len(second), ref["saddr"],
                   ref["taddr"], page, len(dt), ref["osver"], ref["name"], cmdline, ident, b"")
    out = bytearray(hdr)
    out += b"\0" * (page - len(out))
    for part in (kernel, ramdisk, second, dt):
        out += part
        out += b"\0" * (-len(part) % page)
    return bytes(out) + TRAILER


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref")
    ap.add_argument("--kernel")
    ap.add_argument("--ramdisk")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("-o", "--out")
    ap.add_argument("--info")
    a = ap.parse_args()
    if a.info:
        r = parse(Path(a.info).read_bytes())
        print(f"page {r['page']}  kernel {len(r['kernel'])} @ {r['kaddr']:#x}  ramdisk {len(r['ramdisk'])} @ "
              f"{r['raddr']:#x}  second {len(r['second'])}  dt {len(r['dt'])}  tags @ {r['taddr']:#x}  "
              f"os {r['osver']:#x}  cmdline {r['cmdline'].decode(errors='replace')!r}")
        if r["dt"][:4] != b"DTBH":
            print("note: the dt section does not start with DTBH")
        return
    if not (a.ref and a.kernel and a.out):
        ap.error("--ref, --kernel and -o are needed to pack")
    ref = parse(Path(a.ref).read_bytes())
    if not ref["dt"]:
        sys.exit("mkboot: the reference has no dt section: it is not the M20's stock BOOT")
    ramdisk = Path(a.ramdisk).read_bytes() if a.ramdisk else gzip.compress(b"", compresslevel=9, mtime=0)
    img = pack(ref, Path(a.kernel).read_bytes(), ramdisk, a.cmdline.encode())
    Path(a.out).write_bytes(img)
    # what was written must read back as what was meant
    back = parse(img)
    assert back["kernel"] == Path(a.kernel).read_bytes() and back["dt"] == ref["dt"]
    print(f"mkboot: {a.out} ({len(img)} bytes, dt {len(ref['dt'])} bytes from {a.ref})")


if __name__ == "__main__":
    main()
