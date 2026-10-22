#!/bin/bash
# Write a boot image to BOOT or RECOVERY of the Galaxy M20 from its running
# TWRP (scripts/flash.sh is the S9's).
#
#   devices/m20lte/scripts/flash.sh BOOT|RECOVERY <image>
#
# Refuses to run without the stock backups (backup.sh), checks the phone and
# the partition, verifies the copy on the phone before writing and the
# partition after. Never reboots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
. "$ROOT/devices/m20lte/scripts/twrp.sh"
PART=${1:?BOOT or RECOVERY}
IMG=${2:?image}
case "$PART" in
BOOT|RECOVERY) ;;
*) echo "only BOOT or RECOVERY" >&2; exit 2 ;;
esac
for f in BOOT RECOVERY EFS; do
	[ -s "$ROOT/stock/m20lte/$f.img" ] || { echo "stock/m20lte/$f.img missing: run backup.sh first" >&2; exit 1; }
done
[ "$(head -c 8 "$IMG")" = "ANDROID!" ] || { echo "not an Android boot image" >&2; exit 1; }
twrp_check
part_node "$PART" > /dev/null
SIZE=$(stat -c %s "$IMG")
[ "$SIZE" -le "$(part_size "$PART")" ] || { echo "image does not fit $PART" >&2; exit 1; }
SUM=$(sha256sum "$IMG" | cut -d' ' -f1)
adb push "$IMG" /tmp/ztos-flash.img > /dev/null
[ "$(ash sha256sum /tmp/ztos-flash.img | cut -d' ' -f1)" = "$SUM" ] || { echo "USB copy mismatch" >&2; exit 1; }
ash "dd if=/tmp/ztos-flash.img of=$BYNAME/$PART bs=1048576 2>/dev/null && sync"
BACK=$(ash "head -c $SIZE $BYNAME/$PART | sha256sum" | cut -d' ' -f1)
ash rm -f /tmp/ztos-flash.img
[ "$BACK" = "$SUM" ] || { echo "READBACK MISMATCH on $PART" >&2; exit 1; }
echo "FLASHED $PART <- $(basename "$IMG") sha256=$SUM bytes=$SIZE (readback verified)"
