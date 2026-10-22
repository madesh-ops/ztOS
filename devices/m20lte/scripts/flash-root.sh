#!/bin/bash
# Write the ztOS root image into the Galaxy M20's USERDATA from its running
# TWRP, over adb, in verified chunks.
#
#   devices/m20lte/scripts/flash-root.sh <root.img> [chunk MiB]
#
# The S9 does this from ztOS's own rescue shell over the USB network
# (scripts/flash-root.sh); here it is TWRP that does it, so installing never
# depends on the new kernel's USB working. Each chunk is hashed on the phone
# first and only sent when it differs, then hashed again after writing: a run
# that is cut off is finished by running it again.
#
# USERDATA is ERASED. TWRP has it mounted as /data; it is unmounted first.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
. "$ROOT/devices/m20lte/scripts/twrp.sh"
IMG=${1:?root image}
CHUNK=${2:-256}
PART=USERDATA
[ -s "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
twrp_check
part_node "$PART" > /dev/null
SIZE=$(stat -c %s "$IMG")
MIB=1048576
(( SIZE % MIB == 0 )) || { echo "the image is not a whole number of MiB" >&2; exit 1; }
PSIZE=$(part_size "$PART")
[ -n "$PSIZE" ] && [ "$PSIZE" -ge "$SIZE" ] || { echo "USERDATA ($PSIZE bytes) is smaller than the image" >&2; exit 1; }
N=$(( (SIZE / MIB + CHUNK - 1) / CHUNK ))
echo "image $IMG: $SIZE bytes, $N chunks of ${CHUNK}M -> $PART ($PSIZE bytes)"

# TWRP mounts /data (and /sdcard inside it) on its own: let go of it
ash "umount /sdcard 2>/dev/null; umount /data 2>/dev/null; true"
ash "grep -q ' /data ' /proc/mounts" && { echo "TWRP still has /data mounted; unmount it in TWRP (Mount)" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sent=0 skipped=0
for ((i = 0; i < N; i++)); do
	off=$(( i * CHUNK ))
	dd if="$IMG" bs=1M skip="$off" count="$CHUNK" status=none > "$TMP/chunk"
	len=$(stat -c %s "$TMP/chunk")
	want=$(sha256sum "$TMP/chunk" | cut -d' ' -f1)
	have=$(ash "dd if=$BYNAME/$PART bs=1048576 skip=$off count=$(( len / MIB )) 2>/dev/null | sha256sum" | cut -d' ' -f1)
	if [ "$want" = "$have" ]; then
		skipped=$((skipped + 1))
		printf '%3d/%d  skip (identical)  %s\n' "$((i + 1))" "$N" "$(date +%T)"
		continue
	fi
	ok=
	gzip -1 -c "$TMP/chunk" > "$TMP/chunk.gz"
	# adb.exe on Windows (Git Bash) wants a Windows path for the local file
	local_gz=$TMP/chunk.gz
	command -v cygpath > /dev/null && local_gz=$(cygpath -w "$local_gz")
	for try in 1 2 3; do
		# adb push, not a stream into `adb exec-in`: OrangeFox's adbd loses
		# data on exec-in (a 1 MiB test arrived as 950272 bytes)
		adb push "$local_gz" /tmp/ztos-chunk.gz > /dev/null &&
			ash "gunzip -c /tmp/ztos-chunk.gz | dd of=$BYNAME/$PART bs=1048576 seek=$off conv=notrunc 2>/dev/null; sync; rm -f /tmp/ztos-chunk.gz" ||
			true    # judged by the readback below, not by adb
		back=$(ash "dd if=$BYNAME/$PART bs=1048576 skip=$off count=$(( len / MIB )) 2>/dev/null | sha256sum" | cut -d' ' -f1)
		[ "$back" = "$want" ] && { ok=1; break; }
		echo "   chunk $i readback mismatch (try $try)" >&2
	done
	[ -n "$ok" ] || { echo "chunk $i failed three times, stopping" >&2; exit 1; }
	sent=$((sent + 1))
	printf '%3d/%d  written and verified  %s\n' "$((i + 1))" "$N" "$(date +%T)"
done
echo "FLASH_ROOT_OK $sent chunk(s) written, $skipped already identical"
