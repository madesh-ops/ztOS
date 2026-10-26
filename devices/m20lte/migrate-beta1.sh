#!/bin/sh
# Move a Galaxy M20 running ztOS ALPHA-2 to BETA-1 in place: ztOSIAL (the
# daemon, its library and the M20's manifest in /vendor/ztos), the packages
# it needs, the shell and Settings rebuilt against it, and the SDK runtime.
# Nothing the user made is touched; the binaries replaced are kept beside
# them as *.pre-beta1.
#
#   sudo sh migrate-beta1.sh <the source tree, merged, unpacked on the phone>
#
# Done on the M20 on 2026-10-26 by hand, step by step, as recorded here. The
# shell takes over when the session restarts (systemctl restart greetd).
set -eu
B=${1:?the source tree}
U=$(id -nu 1000)
H=$(getent passwd 1000 | cut -d: -f6)
say() { echo "migrate: $*"; }

# (a tree copied from Windows has CRLF line endings: scripts would not run)
grep -rlI "$(printf '\r')" "$B/ial" "$B/ztmetro" "$B/sdk/runtime" 2>/dev/null | xargs -r sed -i 's/\r$//'

say "packages ztOSIAL needs"
pacman -S --needed --noconfirm python-gobject python-dbus

say "ztOSIAL: daemon, CLI, library, specs"
make -C "$B/ial"
make -C "$B/ial" install PREFIX=/usr
install -d /vendor/ztos
cp -a "$B/devices/m20lte/vendor/ztos/." /vendor/ztos/
chown -R 0:0 /vendor
systemctl daemon-reload
systemctl enable --now ztos-iald
ztos-ial check

say "the shell, Settings and the hardware test, against ztOSIAL"
make -C "$B/ztmetro" ztmetro settings/ztsettings devtest/zrndevtest
for b in ztmetro ztsettings zrndevtest; do
	cp -n "/usr/local/bin/$b" "/usr/local/bin/$b.pre-beta1" 2>/dev/null || true
done
install -m755 "$B/ztmetro/ztmetro" /usr/local/bin/ztmetro
install -m755 "$B/ztmetro/settings/ztsettings" /usr/local/bin/ztsettings
install -m755 "$B/ztmetro/devtest/zrndevtest" /usr/local/bin/zrndevtest

say "the SDK runtime"
install -m644 "$B"/sdk/runtime/ztos/*.py "$H/.local/share/ztos/lib/ztos/"
install -m644 "$B/sdk/runtime/zygote.py" "$H/.local/share/ztos/zygote.py"
chown "$U:" "$H/.local/share/ztos/zygote.py" "$H"/.local/share/ztos/lib/ztos/*.py

say "done: restart the session (systemctl restart greetd) to use the new shell"
