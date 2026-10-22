#!/bin/bash
# Save the Galaxy M20's stock partitions from its running TWRP, before ztOS
# writes anything: BOOT (the boot image build needs its header and DTBH, and
# it is the way back to Android), RECOVERY (TWRP itself), and the phone's
# identity, EFS and CPEFS (IMEI, modem calibration: ztOS never writes them,
# but nothing else can bring them back).
#
#   devices/m20lte/scripts/backup.sh
#
# Writes stock/m20lte/<NAME>.img and .sha256, each read twice from the phone
# and compared. A partition already saved is left alone.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
. "$ROOT/devices/m20lte/scripts/twrp.sh"
DIR=$ROOT/stock/m20lte
mkdir -p "$DIR"
twrp_check

for p in BOOT RECOVERY EFS CPEFS; do
	out=$DIR/$p.img
	if [ -s "$out" ] && [ -s "$out.sha256" ]; then
		echo "$p: already saved ($out)"
		continue
	fi
	node=$(part_node "$p")
	size=$(part_size "$p")
	echo "$p: $node, $size bytes"
	adb exec-out "dd if=$BYNAME/$p bs=1048576 2>/dev/null" > "$out.part"
	[ "$(stat -c %s "$out.part")" = "$size" ] ||
		{ echo "$p: got $(stat -c %s "$out.part") bytes, the partition is $size" >&2; exit 1; }
	here=$(sha256sum "$out.part" | cut -d' ' -f1)
	there=$(ash "sha256sum $BYNAME/$p" | cut -d' ' -f1)
	[ "$here" = "$there" ] || { echo "$p: the copy does not match the phone" >&2; exit 1; }
	mv "$out.part" "$out"
	echo "$here  $p.img" > "$out.sha256"
done
python3 -I "$ROOT/devices/m20lte/scripts/mkboot.py" --info "$DIR/BOOT.img"
echo "BACKUP_OK $DIR"
