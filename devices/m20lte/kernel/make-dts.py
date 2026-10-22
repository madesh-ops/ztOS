#!/usr/bin/env python3
"""Make the Galaxy M20's device tree for one build.

  make-dts.py <exynos7904-m20lte.dts> <out.dts> [--ram 3|4] [--watchdog]
              [--no-touch]

Like kernel/make-dts.py for the S9, every edit is asserted, so a changed
source fails the build instead of quietly making a different tree.

  --ram 3|4     the M20 comes with 3 or 4 GB. The tree has to say which:
                uniLoader hands the kernel this tree, not the one sboot
                fixes up. 3 is the default and is safe on both (a 4 GB phone
                just leaves 1 GB unused); 4 on a 3 GB phone crashes at boot.
  --watchdog    the A53 cluster watchdog, with the kernel petting it (needs
                the driver change in patch-kernel.py). Off by default until it
                has been seen to work: if it is on and nothing pets it, the
                phone resets into TWRP every 30 s.
  --no-touch    leave the touch controller out (for a first boot, if the
                touch driver turns out to hang the bus)
"""
import argparse
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("out")
ap.add_argument("--ram", choices=("3", "4"), default="3")
ap.add_argument("--watchdog", action="store_true")
ap.add_argument("--no-touch", action="store_true")
args = ap.parse_args()

s = Path(args.src).read_text()


def sub(old, new, count=1):
    global s
    if s.count(old) != count:
        sys.exit(f"make-dts: expected {count}x {old!r}, found {s.count(old)}")
    s = s.replace(old, new)


# The high bank: 1 GB on a 3 GB phone, 2 GB on a 4 GB one (the low two banks
# are the same on both: Samsung's tree, memory@80000000 and memory@C0000000).
if args.ram == "4":
    sub("<0x8 0x80000000 0x40000000>;", "<0x8 0x80000000 0x80000000>;")

if args.watchdog:
    sub("""		samsung,syscon-phandle = <&pmu_system_controller>;
		status = "disabled";""", """		samsung,syscon-phandle = <&pmu_system_controller>;
		status = "okay";""")

if args.no_touch:
    sub("""				pinctrl-0 = <&touch_irq>;
			};""", """				pinctrl-0 = <&touch_irq>;
				status = "disabled";
			};""")

Path(args.out).write_text(s)
print(f"make-dts: {args.out} (ram={args.ram} GB watchdog={args.watchdog} touch={not args.no_touch})")
