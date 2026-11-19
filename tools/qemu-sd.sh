#!/bin/sh
# Make a FAT32 SD card image for QEMU testing: tools/qemu-sd.sh <mac.rom> [disk1.dsk]
# Writes build/qemu-sd.img (512 MB, or bigger to fit the disk). Needs mtools + dosfstools.
set -e
ROM=${1:?usage: $0 <mac.rom> [disk1.dsk]}
DISK=$2
OUT=build/qemu-sd.img
SIZE=512
if [ -n "$DISK" ]; then
	NEED=$(( $(stat -c %s "$DISK") / 1048576 + 64 ))
	[ $NEED -gt $SIZE ] && SIZE=$NEED
fi
mkdir -p build
rm -f "$OUT"
truncate -s ${SIZE}M "$OUT"
echo 'start=2048, type=c' | sfdisk -q "$OUT"
mkfs.vfat --offset 2048 -F 32 -n MAC "$OUT" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -i "$OUT@@1M" "$ROM" ::mac.rom
[ -n "$DISK" ] && mcopy -i "$OUT@@1M" "$DISK" ::disk1.dsk
mdir -i "$OUT@@1M" ::
echo "Made $OUT (a copy: your own disk image is not touched)."
