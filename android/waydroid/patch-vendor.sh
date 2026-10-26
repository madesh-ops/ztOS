#!/usr/bin/env bash
# Work on an unmounted copy; preserve vendor.img.stock for rollback.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
here=$(cd -- "$(dirname -- "$0")" && pwd)
images=/usr/share/waydroid-extra/images
[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || true) == STOPPED ]] || {
    echo 'Stop the Waydroid session and container service first.' >&2; exit 1;
}
python3 - <<'PYCONFIG'
import configparser
config = configparser.ConfigParser()
config.read('/var/lib/waydroid/waydroid.cfg')
if not config.get('properties', 'persist.waydroid.width', fallback='').isdigit() or not config.get('properties', 'persist.waydroid.height', fallback='').isdigit():
    raise SystemExit('Composer hotplug fix requires a fixed logical geometry (persist.waydroid.width and height).')
PYCONFIG
cp -an --reflink=auto --sparse=always "$images/vendor.img" "$images/vendor.img.stock"
work=$(mktemp -d "$images/.vendor.XXXXXX")
cleanup() {
    if mountpoint -q "$work/root"; then umount "$work/root" || return; fi
    rm -rf -- "$work"
}
trap cleanup EXIT
cp --reflink=auto --sparse=always --preserve=all "$images/vendor.img.stock" "$work/vendor.img"
mkdir "$work/root"
# The stock image has no free block, and the environment file below needs one
# (beyond the 16 MiB ext4 holds back for itself when mounted).
truncate -s +32M "$work/vendor.img"
e2fsck -fy "$work/vendor.img" >/dev/null || (( $? <= 1 ))
resize2fs "$work/vendor.img" >/dev/null 2>&1
mount -o loop,rw "$work/vendor.img" "$work/root"
python3 "$here/patch-composer.py" "$work/root/lib64/hw/hwcomposer.waydroid.so"
# This phone's own files over the image's (/vendor/ztos/android-vendor, laid
# out as the vendor image is): the M20's Android Mesa with its G71 fix
# (devices/m20lte/mesa/build-android-mesa.sh).
if [[ -d /vendor/ztos/android-vendor ]]; then
    cp -r --no-preserve=ownership /vendor/ztos/android-vendor/. "$work/root/"
    (cd /vendor/ztos/android-vendor && find . -type f) | while read -r f; do
        chown 0:0 "$work/root/$f"; chmod 644 "$work/root/$f"; echo "vendor: ${f#./}"
    done
fi
# This phone's environment for Android's processes (its manifest's
# [android.environment]): Android's Mesa reads its debug options only from the
# environment, and init's export reaches zygote and every app it forks.
python3 - "$work/root/etc/init/ztos-environment.rc" <<'PYENV'
import os, sys, tomllib
try:
    with open('/vendor/ztos/manifest.toml', 'rb') as stream:
        environment = tomllib.load(stream).get('android', {}).get('environment', {})
except FileNotFoundError:
    environment = {}
if environment:
    with open(sys.argv[1], 'w') as rc:
        rc.write('# written by ztOS patch-vendor.sh from /vendor/ztos/manifest.toml\non early-init\n')
        for name, value in environment.items():
            rc.write(f'    export {name} {value}\n')
    os.chmod(sys.argv[1], 0o644)
    print('Android environment:', ', '.join(f'{k}={v}' for k, v in environment.items()))
PYENV
umount "$work/root"
rc=0
e2fsck -fy "$work/vendor.img" || rc=$?
(( rc <= 1 ))
mv -T "$work/vendor.img" "$images/vendor.img"
