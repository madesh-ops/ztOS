# SPDX-License-Identifier: GPL-2.0-or-later
"""Where ztOS goes: the QEMU emulator, a Galaxy S9 (starlte) or a Galaxy M20 (m20lte)."""
from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

from .build import EDK2, ROOT, WORK, Fail, say


NAMES = {"qemu": "the emulator", "starlte": "the Galaxy S9", "m20lte": "the Galaxy M20"}

MANUAL_REBOOT = """To reboot the phone:
  - hold Power and Volume+, Volume- keys to reboot."""


def ping_phone() -> bool:
    r = subprocess.run(["ping", "-c", "1", "-W", "1", "172.16.42.1"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0


def reboot_from_rescue() -> None:
    """Restart a phone that is sitting in the ztOS Rescue shell (telnet on USB),
    into ztOS. Never raises: the rescue shell may already be gone, and the
    buttons always work."""
    if not ping_phone():
        say("no phone answers on USB (172.16.42.1)")
        say("is it in ztOS Rescue? (It gets there on its own if a start fails twice.)")
        print(MANUAL_REBOOT)
        return
    try:    # Rescue answers telnet on port 23; running ztOS does not
        socket.create_connection(("172.16.42.1", 23), timeout=5).close()
    except OSError:
        say("the phone answers, but it is not in ztOS Rescue (no rescue shell)")
        say("if it is running ztOS already, there is nothing to do; otherwise:")
        print(MANUAL_REBOOT)
        return
    say("rebooting the phone from ztOS Rescue...")
    try:
        subprocess.run([sys.executable, str(ROOT / "scripts" / "psh.py"), "reboot"],
                       check=False, timeout=60)
    except Exception as e:            # the rescue shell went away mid-command: fine
        say(f"(the rescue shell did not answer: {e})")
    for _ in range(60):
        if not ping_phone():
            say("the phone has left the rescue shell: it is starting ztOS "
                "(the first start takes a minute)")
            return
        time.sleep(1)
    print(MANUAL_REBOOT)


# --- QEMU ----------------------------------------------------------------------

QEMU_SCREEN = (540, 1110)       # a phone's shape; ztMETRO draws it at 1.5 px a point
EGL_HEADLESS = Path("/usr/lib/qemu/ui-egl-headless.so").exists()


def qemu_command(disk: Path, headless: bool = False, monitor: Path | None = None, gl: bool = False) -> list:
    """The emulated phone: the "virt" machine, a virtio GPU, a pointer it takes
    as a finger, a keyboard, and the network (ssh to it on localhost:2222).

    By default nothing of the host's graphics is used: ztMETRO draws with
    software OpenGL in the guest (slower, but it works on any computer). With
    gl=True the guest gets virgl, drawn by the host's GPU through QEMU's GTK
    window; some drivers (NVIDIA on laptops with two GPUs, in particular) make
    QEMU stop with "eglMakeCurrent failed: EGL_BAD_ACCESS"."""
    if not (EDK2 / "QEMU_EFI.fd").exists():
        raise Fail("no UEFI firmware for aarch64: sudo pacman -S edk2-aarch64")
    if shutil.which("qemu-system-aarch64") is None:
        raise Fail("no QEMU: sudo pacman -S qemu-system-aarch64")
    w, h = QEMU_SCREEN
    cmd = ["qemu-system-aarch64", "-name", "ztOS", "-M", "virt", "-cpu", "max", "-smp", "4", "-m", "4G",
           "-bios", str(EDK2 / "QEMU_EFI.fd"),
           "-drive", f"file={disk},if=none,id=disk,format=raw,cache=unsafe",
           "-device", "virtio-blk-pci,drive=disk",
           "-device", f"{'virtio-gpu-gl-pci' if gl and not headless else 'virtio-gpu-pci'},xres={w},yres={h}",
           "-device", "virtio-tablet-pci", "-device", "virtio-keyboard-pci",
           "-netdev", "user,id=net,hostfwd=tcp:127.0.0.1:2222-:22",
           "-device", "virtio-net-pci,netdev=net",
           "-device", "virtio-rng-pci",
           "-audiodev", "none,id=snd"]
    if headless:
        cmd += ["-display", "none", "-vnc", "127.0.0.1:5"]
    else:
        cmd += ["-display", "gtk,gl=on,zoom-to-fit=off" if gl else "gtk,gl=off,zoom-to-fit=off"]
    if monitor is not None:
        cmd += ["-monitor", f"unix:{monitor},server,nowait"]
    cmd += ["-serial", f"file:{WORK / 'qemu' / 'console.log'}"]
    return cmd


def run_qemu(headless: bool = False, wait: bool = True, gl: bool = False) -> subprocess.Popen:
    disk = WORK / "qemu" / "ztos-qemu.img"
    if not disk.exists():
        raise Fail("there is no ztOS disk for QEMU yet: ztos install --target qemu (or ztos build --target qemu)")
    monitor = WORK / "qemu" / "monitor.sock"
    monitor.unlink(missing_ok=True)
    cmd = qemu_command(disk, headless, monitor, gl)
    say("starting the ztOS emulator" + (" (headless: vnc 127.0.0.1:5905)" if headless else ""))
    say("it boots slowly (the whole phone is emulated): the screen stays black for a few minutes, "
        "then ztMETRO's Start appears")
    say("the mouse is your finger: click to tap, drag to swipe; Escape is the phone's back")
    say("ssh into it: ssh -p 2222 <user>@localhost   (its console: build/ztos/qemu/console.log)")
    proc = subprocess.Popen(cmd)
    if wait:
        proc.wait()
    return proc


def screenshot(path: Path) -> bool:
    """A picture of the emulator's screen (through QEMU's monitor)."""
    import socket
    monitor = WORK / "qemu" / "monitor.sock"
    if not monitor.exists():
        return False
    ppm = path.with_suffix(".ppm")
    with socket.socket(socket.AF_UNIX) as s:
        s.connect(str(monitor))
        s.sendall(f"screendump {ppm}\n".encode())
        time.sleep(1.5)
    if ppm.exists() and shutil.which("magick"):
        subprocess.run(["magick", str(ppm), str(path)], check=False)
        ppm.unlink()
    return path.exists() or ppm.exists()


# --- the Galaxy S9 ---------------------------------------------------------------

PHONE_GUIDE = """
Installing ztOS on a Samsung Galaxy S9 (SM-G960N/F, "starlte")
==============================================================

ztOS replaces Android on the phone's internal storage. Everything on the
phone's user data partition is ERASED. The phone's identity (IMEI, modem
calibration: the EFS partitions) is never touched.

You need
  - TWRP for starlte in the phone's RECOVERY partition (unlock the bootloader:
    Settings > Developer options > OEM unlocking, then flash TWRP with Odin or
    Heimdall from download mode: Volume Down + Bixby + Power).
  - adb on this computer (android-tools) and a USB cable.
  - The phone charged above 50 %.

Steps (the installer does 3 to 6 for you, and asks before each)
  1. Back up anything on the phone you want to keep. It will be gone.
  2. Start TWRP: power the phone off, then hold Volume Up + Bixby + Power
     until the TWRP screen. Plug it into this computer.
  3. ztOS's boot image is built (kernel 7.1 with the S9's drivers, ztOS's
     start-up program, made fail-safe: a crash falls back to TWRP).
  4. The ztOS system is built for the phone.
  5. The boot image is written to BOOT, and read back to check it.
  6. The system is written to USERDATA in 256 MiB pieces, each checked; a cut
     cable just means running it again: it carries on.
  7. Reboot: hold Power and Volume+, Volume- keys to reboot. The first start takes a minute:
     the system grows to fill the storage, then the Welcome app shows how ztOS is used.

If anything goes wrong, the phone comes back to TWRP by itself; run the
installer again from step 2.
"""


def adb_state() -> str:
    if shutil.which("adb") is None:
        return "no adb"
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        if "\t" in line:
            return line.split("\t", 1)[1].strip()
    return "none"


def install_phone(root_image: Path, ask) -> None:
    """Steps 3 to 7 of PHONE_GUIDE. `ask(question) -> bool` asks the person at
    the computer; nothing is written to the phone without a yes, and the
    erasing step wants the word typed out."""
    state = adb_state()
    if state != "recovery":
        raise Fail(f"the phone is not in TWRP over adb (adb says: {state}). Start TWRP (Volume Up + Bixby + "
                   "Power) and plug it in, then run this again.")
    boot = ROOT / "out" / "ztos-boot.img"
    if not boot.exists() or ask("Build ztOS's boot image again? (no: use the one already built)"):
        say("building the boot image (kernel, drivers, start-up program)")
        subprocess.run([str(ROOT / "scripts" / "build-boot.sh"), "ztos-boot", "--fallback", "recovery",
                        "--power", "--touch", "--cpufreq", "--gpu", "--decon", "--wlan"], check=True)
    if not ask("Write ztOS's boot image to the phone's BOOT partition now?"):
        raise Fail("stopped before writing anything")
    subprocess.run([str(ROOT / "scripts" / "flash.sh"), "BOOT", str(boot)], check=True)
    print("\nNext: the system goes into USERDATA. EVERYTHING ON THE PHONE'S USER DATA IS ERASED.")
    if input("Type ERASE to go on: ").strip() != "ERASE":
        raise Fail("stopped: the boot image is written, the user data is untouched (Android will not start "
                   "until ztOS is installed or the stock BOOT image is written back)")
    say("clearing old/partial filesystem header from USERDATA...")
    subprocess.run(["adb", "shell", "dd if=/dev/zero of=/dev/block/sda25 bs=1M count=20 conv=fsync"], check=True)
    say("rebooting phone into ztOS rescue shell...")
    subprocess.run(["adb", "reboot"], check=True)
    say("waiting for the phone to start its rescue shell on USB (172.16.42.1)...")
    for _ in range(60):
        time.sleep(1)
        r = subprocess.run(["ping", "-c", "1", "-W", "1", "172.16.42.1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            break
    time.sleep(2)
    subprocess.run([str(ROOT / "scripts" / "flash-root.sh"), str(root_image)], check=True)
    try:
        subprocess.run([sys.executable, str(ROOT / "scripts" / "psh.py"), "-t", "5", "reboot"],
                       capture_output=True, timeout=5)
    except Exception:
        pass
    say("")
    say("Hold Power and Volume+, Volume- keys to reboot.")
    say("done: ztOS starts on the phone (the first start takes a minute)")


# --- the Galaxy M20 ----------------------------------------------------------------

M20 = ROOT / "devices" / "m20lte"

M20_GUIDE = """
Installing ztOS on a Samsung Galaxy M20 (SM-M205F/G/FN, "m20lte")
=================================================================

THIS PORT IS IN BRING-UP: it has not started on a phone yet. Expect the
first attempts to stop at a black screen or the Samsung logo; the phone then
comes back to TWRP by itself (or after Volume Up + Power), and nothing is lost
that the backup does not hold. devices/m20lte/README.md says what works,
what to try first and what to send back.

ztOS replaces Android on the phone's internal storage. Everything on the
phone's user data partition is ERASED. The phone's identity (IMEI, modem
calibration: EFS and CPEFS) is never touched, and is saved first anyway.

You need
  - TWRP for m20lte in the phone's RECOVERY partition (unlock the bootloader:
    Settings > Developer options > OEM unlocking, then flash TWRP with Odin or
    Heimdall from download mode: Volume Up + Volume Down with the cable in).
  - adb on this computer (android-tools) and a USB cable.
  - The phone charged above 50 %.

Steps (the installer does 3 to 7 for you, and asks before each)
  1. Back up anything on the phone you want to keep. It will be gone.
  2. Start TWRP: power the phone off, then hold Volume Up + Power until the
     TWRP screen. Plug it into this computer.
  3. BOOT, RECOVERY, EFS and CPEFS are saved to stock/m20lte/ (once).
  4. ztOS's boot image is built: mainline 7.1 with the M20's device tree, the
     start-up program, made fail-safe (a crash or reset lands in TWRP).
  5. The system is written to USERDATA from TWRP, in 256 MiB pieces, each
     checked; a cut cable just means running it again: it carries on.
  6. The boot image is written to BOOT, and read back to check it.
  7. Reboot from TWRP. The first start takes a minute: the system grows to
     fill the storage, then the Welcome app shows how ztOS is used.

To go back to Android: in TWRP, write stock/m20lte/BOOT.img back to BOOT
(devices/m20lte/scripts/flash.sh BOOT stock/m20lte/BOOT.img) and flash the
stock firmware with Odin.
"""


def guide(target: str) -> str:
    return M20_GUIDE if target == "m20lte" else PHONE_GUIDE


def install(target: str, root_image: Path, ask) -> None:
    if target == "m20lte":
        install_m20lte(root_image, ask)
    else:
        install_phone(root_image, ask)


def install_m20lte(root_image: Path, ask) -> None:
    """Steps 3 to 7 of M20_GUIDE. Everything goes through TWRP, so installing
    never depends on the new kernel's USB working; nothing is written to the
    phone without a yes, and the erasing step wants the word typed out."""
    state = adb_state()
    if state != "recovery":
        raise Fail(f"the phone is not in TWRP over adb (adb says: {state}). Start TWRP (Volume Up + Power) and "
                   "plug it in, then run this again.")
    scripts = M20 / "scripts"
    say("saving the phone's stock BOOT, RECOVERY, EFS and CPEFS (once)")
    subprocess.run(["bash", str(scripts / "backup.sh")], check=True)
    boot = ROOT / "out" / "ztos-m20lte-boot.img"
    if not boot.exists() or ask("Build ztOS's boot image again? (no: use the one already built)"):
        ram = "4" if ask("Is this a 4 GB Galaxy M20? (no: 3 GB, which is also the safe answer if unsure)",
                         default=False) else "3"
        say(f"building the boot image (kernel, device tree for {ram} GB, start-up program)")
        subprocess.run(["bash", str(scripts / "build-boot.sh"), "ztos-m20lte-boot", "--fallback", "recovery",
                        "--ram", ram], check=True)
    print("\nNext: the system goes into USERDATA. EVERYTHING ON THE PHONE'S USER DATA IS ERASED.")
    if input("Type ERASE to go on: ").strip() != "ERASE":
        raise Fail("stopped before writing anything")
    subprocess.run(["bash", str(scripts / "flash-root.sh"), str(root_image)], check=True)
    if not ask("Write ztOS's boot image to the phone's BOOT partition now?"):
        raise Fail("stopped: the system is in USERDATA, BOOT is still Android's (which will not start "
                   "without its data; write ztOS's boot image, or restore the stock firmware)")
    subprocess.run(["bash", str(scripts / "flash.sh"), "BOOT", str(boot)], check=True)
    if ask("Reboot the phone into ztOS now?"):
        subprocess.run(["adb", "reboot"], check=False)
    say("")
    say("done: ztOS starts on the phone (the first start takes a minute).")
    say("If the screen stays black or on the logo for more than 10 minutes, the phone resets into TWRP by "
        "itself; devices/m20lte/README.md says what to collect from there.")
