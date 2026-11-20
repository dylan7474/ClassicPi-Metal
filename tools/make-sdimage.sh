#!/bin/sh
# Build a flashable SD card image: tools/make-sdimage.sh [mac.rom] [disk1.dsk] [out.img]
# With no arguments it uses local/mac.rom and local/disk1.dsk (local/ is gitignored).
# One FAT32 partition holding build/sdcard/ (run `make sdcard` first) + ROM + disk.
# Flash with Raspberry Pi Imager ("Use custom") or dd. Needs mtools + dosfstools.
set -e
ROM=${1:-local/mac.rom}
DISK=${2-local/disk1.dsk}
[ -f "$ROM" ] || { echo "No ROM at $ROM (usage: $0 [mac.rom] [disk1.dsk] [out.img])"; exit 1; }
[ -z "$DISK" ] || [ -f "$DISK" ] || { echo "No disk image at $DISK"; exit 1; }
OUT=${3:-build/ClassicPi-Metal.img}
[ -f build/sdcard/kernel8.img ] || { echo "Run 'make sdcard' first"; exit 1; }
SIZE=256
if [ -n "$DISK" ]; then
	NEED=$(( $(stat -c %s "$DISK") / 1048576 + 128 ))
	[ $NEED -gt $SIZE ] && SIZE=$NEED
fi
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s ${SIZE}M "$OUT"
echo 'start=2048, type=c, bootable' | sfdisk -q "$OUT"
mkfs.vfat --offset 2048 -F 32 -n CLASSICPI "$OUT" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -s -i "$OUT@@1M" build/sdcard/* ::
mcopy -i "$OUT@@1M" "$ROM" ::mac.rom
[ -n "$DISK" ] && mcopy -i "$OUT@@1M" "$DISK" ::disk1.dsk
mdir -i "$OUT@@1M" ::
echo "Made $OUT (${SIZE} MB)"
