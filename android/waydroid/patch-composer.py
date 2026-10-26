#!/usr/bin/env python3
"""Backport composer fixes for the tested 20260927 arm64 binary and fixed size.

Upstream removes configureRpcThreadpool(1, true) from hwc_binder_thread.
The composer service already configures four threads. Replace only that BL
instruction with AArch64 NOP; reject any other binary before writing.

Also suppress configure-time hotplug for this fixed-resolution deployment.
With persist.waydroid.width and height fixed, choose_width_height always selects
those same dimensions. This is the fixed-size case of upstream 5af0c11.
Initial display registration remains intact. Resolution changes require a
session restart. Do not use this patch for dynamically sized displays.
https://github.com/waydroid/android_hardware_waydroid/commit/5af0c114a19fc916de837ad553524cd2aade2719
https://github.com/waydroid/android_hardware_waydroid/commit/4aee4c8d953e5cf7cb58d838242b71899a51269a

Videos in apps: in multi-window mode the composer puts a layer in an app's
window only when SurfaceFlinger's name for it carries the task ("TID:70#...").
A layer an app makes itself through SurfaceControl has its own name
("bbq-wrapper#105": Instagram's and Discord's video players) and was dropped,
so the video played in Android and the screen showed black. In
multi_window_mode::handle_layer, when get_window finds no window, a plain-named
layer whose name starts with "bbq-" now goes into the window of the task layer
just above it (the app's own, which it is drawn under), in a cave after .plt
that the executable segment is extended to cover:

    0x4bf34  b    cave1        (was: add x0, x0, #8; the layer list into the
                                free stack slot [sp, #0x18], then on)
    0x4bf4c  cbz  x0, cave2    (was: cbz x0, 0x4bf70: no window, no layer)
    cave2    layer i of type RawName (1) named "bbq-...", layer i + 1 in the
             list and of type TID (0): get_window(pdev, &layers[i + 1]), and
             on to apply_hwc_layer_to_window (0x4bf50); else 0x4bf70 as before
"""
import hashlib
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
nop = bytes.fromhex('1f2003d5')
CAVE = bytes.fromhex(
    '00200091e00f00f9f47aff17e90f40f92a2d40a90c0a80d28d2a0c9bae0140b9df050071'
    '61020054af214039b02500914f000036b00d40f9110240b94c4c8c522caea5723f020c6b'
    '41010054a14101913f000bebe20000542e0040b9ae000035e00315aa0bfdff97400000b4'
    'e17aff17e87aff17')
# offset: (stock bytes, patched bytes)
patches = {
    0x3EEE8: (bytes.fromhex('deb30094'), nop),  # configureRpcThreadpool(1, true)
    0x40C70: (bytes.fromhex('e8ac0094'), nop),  # wl_shell configure: do_hotplug
    0x42E40: (bytes.fromhex('74a40094'), nop),  # xdg_toplevel configure: do_hotplug
    0x4BF34: (bytes.fromhex('00200091'), bytes.fromhex('0b850014')),  # b cave1
    0x4BF4C: (bytes.fromhex('200100b4'), bytes.fromhex('00a110b4')),  # cbz x0, cave2
    0x6D360: (bytes(len(CAVE)), CAVE),
    # the R E segment's p_filesz and p_memsz: 0x33360 -> 0x333d4, over the cave
    0xD0: (bytes.fromhex('6033030000000000'), bytes.fromhex('d433030000000000')),
    0xD8: (bytes.fromhex('6033030000000000'), bytes.fromhex('d433030000000000')),
}
stock_hash = 'c5847fd6b1228afb803f36a0818d037ce65369e6b95e96d85666b67f2158463f'
data = bytearray(path.read_bytes())
check = data.copy()
for offset, (original, new) in patches.items():
    if data[offset:offset + len(original)] not in (original, new):
        sys.exit('Unsupported composer instructions; no changes made.')
    check[offset:offset + len(original)] = original
if hashlib.sha256(check).hexdigest() != stock_hash:
    sys.exit('Unsupported composer binary; no changes made. Rebuild the upstream fixes for this vendor image.')
# Modify in place to preserve ownership, mode and SELinux xattrs inside ext4.
with path.open('r+b') as output:
    for offset, (_, new) in patches.items():
        output.seek(offset)
        output.write(new)
print('Composer fixes applied; SHA256=' + hashlib.sha256(path.read_bytes()).hexdigest())
