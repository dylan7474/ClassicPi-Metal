#
# ClassicPi-Metal - bare-metal Mac Plus (umac) for the Raspberry Pi
#
#   make            build every kernel in TARGETS into build/
#   make pi3        just one target (pi3 = Zero 2 W / Pi 3, pi1 = Zero / Zero W / Pi 1)
#   make sdcard     gather everything for the SD card in build/sdcard/
#   make clean      remove our objects and kernels (keeps the built circle-stdlib copies)
#   make distclean  also remove the circle-stdlib copies and firmware
#   make qemu       run the 64-bit kernel in QEMU (see tools/qemu-sd.sh)
#   make qemu-pi1   run the 32-bit kernel in QEMU's Pi Zero model
#
# Each target builds in its own build/<target>/ copy of circle-stdlib, umac
# and src/, because Circle and Rules.mk build in place.  The submodules
# under libs/ and external/ are only ever read.
#

TARGETS   ?= pi3 pi1
MEMSIZE   ?= 4096
HOSTCC    ?= cc

# target -> Circle model number, toolchain prefix, newlib arch, kernel name
pi3_RASPPI = 3
pi3_PREFIX = aarch64-none-elf-
pi3_ARCH   = aarch64-none-circle
pi3_KERNEL = kernel8.img

pi1_RASPPI = 1
pi1_PREFIX = arm-none-eabi-
pi1_ARCH   = arm-none-circle
pi1_KERNEL = kernel.img

STDLIB    = libs/circle-stdlib
UMAC      = external/umac
MUSASHI   = $(UMAC)/external/Musashi

# Copies keep their build output (excluded files are never deleted)
RSYNC_SRC = rsync -a --delete --exclude=.git --exclude='*.o' --exclude='*.d' \
		--exclude='*.elf' --exclude='*.img' --exclude='*.map' --exclude='*.lst'

.PHONY: all sdcard clean distclean submodules qemu qemu-pi1 $(TARGETS)

all: $(TARGETS)

submodules:
	@test -f $(STDLIB)/libs/circle/Rules.mk && test -f $(MUSASHI)/m68kmake.c || \
		git submodule update --init --recursive

# ---- per-target rules ------------------------------------------------------
define TARGET_RULES

# 1. Our own copy of circle-stdlib, configured and built for this target (~2 min)
build/$(1)/circle-stdlib/install/$($(1)_ARCH)/lib/libc.a: | submodules
	mkdir -p build/$(1)
	rsync -a --exclude=.git --exclude=/build/ --exclude=/install/ \
		--exclude='*.o' --exclude='*.d' --exclude='*.a' --exclude=Config.mk \
		--exclude=Config2.mk $(STDLIB)/ build/$(1)/circle-stdlib/
	mkdir -p build/$(1)/circle-stdlib/build/circle-newlib build/$(1)/circle-stdlib/install
	cd build/$(1)/circle-stdlib && ./configure -r $($(1)_RASPPI) -p $($(1)_PREFIX) && $$(MAKE)

# 2. Musashi's opcode tables, generated on the build PC
build/$(1)/gen/m68kops.c: $(MUSASHI)/m68k_in.c | submodules
	mkdir -p build/$(1)/gen
	$(HOSTCC) -O2 -o build/$(1)/gen/m68kmake $(MUSASHI)/m68kmake.c
	build/$(1)/gen/m68kmake build/$(1)/gen $(MUSASHI)/m68k_in.c

# 3. The kernel: refreshed copies of src/ and umac, then Circle's Rules.mk
$(1): build/$(1)/circle-stdlib/install/$($(1)_ARCH)/lib/libc.a build/$(1)/gen/m68kops.c
	$(RSYNC_SRC) src/ build/$(1)/src/
	$(RSYNC_SRC) $(UMAC)/ build/$(1)/umac/
	$$(MAKE) -C build/$(1)/src MEMSIZE=$(MEMSIZE) \
		CIRCLESTDLIB=../circle-stdlib UMAC=../umac GEN=../gen
	cp build/$(1)/src/$($(1)_KERNEL) build/$($(1)_KERNEL)
	@ls -l build/$($(1)_KERNEL)

endef
$(foreach t,$(TARGETS),$(eval $(call TARGET_RULES,$(t))))

# ---- SD card ----------------------------------------------------------------
# Pi firmware pinned to the revision Circle uses.  bootcode.bin, start.elf
# and fixup.dat boot every Pi up to the Pi 3; config.txt picks the kernel
# by model.  Add mac.rom + disk1.dsk yourself.
FIRMWARE_REV = 0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68
FIRMWARE_URL = https://raw.githubusercontent.com/raspberrypi/firmware/$(FIRMWARE_REV)/boot
FIRMWARE     = bootcode.bin start.elf fixup.dat LICENCE.broadcom \
	       bcm2710-rpi-zero-2-w.dtb bcm2708-rpi-zero-w.dtb

build/firmware/.done-$(FIRMWARE_REV):
	mkdir -p build/firmware
	for f in $(FIRMWARE); do \
		curl -fsSL --retry 3 --retry-all-errors -o build/firmware/$$f $(FIRMWARE_URL)/$$f || exit 1; \
	done
	touch $@

sdcard: all build/firmware/.done-$(FIRMWARE_REV)
	rm -rf build/sdcard
	mkdir -p build/sdcard
	cp $(addprefix build/firmware/,$(FIRMWARE)) build/sdcard/
	cp sdcard/config.txt sdcard/cmdline.txt build/sdcard/
	cp $(foreach t,$(TARGETS),build/$($(t)_KERNEL)) build/sdcard/
	@echo
	@echo "build/sdcard/ is ready: copy it to a FAT32 card, then add mac.rom and disk1.dsk."

# ---- housekeeping -------------------------------------------------------------
clean:
	rm -rf build/sdcard $(foreach t,$(TARGETS),build/$(t)/src build/$(t)/umac \
		build/$(t)/gen build/$($(t)_KERNEL))

distclean: clean
	rm -rf $(foreach t,$(TARGETS),build/$(t)) build/firmware

# Try it in QEMU's Raspberry Pi 3 model (make the card first with tools/qemu-sd.sh).
# Serial output (the core's messages) goes to the terminal.
qemu: pi3
	@test -f build/qemu-sd.img || { echo "Run tools/qemu-sd.sh <mac.rom> [disk1.dsk] first"; exit 1; }
	qemu-system-aarch64 -M raspi3b -kernel build/kernel8.img \
		-append "width=1024 height=768 logdev=ttyS1" \
		-drive file=build/qemu-sd.img,if=sd,format=raw \
		-serial stdio -usb -device usb-kbd -device usb-mouse

# 32-bit: Circle's docs say QEMU needs -bios (not -kernel) and has no cmdline,
# so the screen size is set on QEMU's side.
qemu-pi1: pi1
	@test -f build/qemu-sd.img || { echo "Run tools/qemu-sd.sh <mac.rom> [disk1.dsk] first"; exit 1; }
	qemu-system-arm -M raspi0 -bios build/kernel.img \
		-global bcm2835-fb.xres=1024 -global bcm2835-fb.yres=768 \
		-drive file=build/qemu-sd.img,if=sd,format=raw \
		-serial stdio -usb -device usb-kbd -device usb-mouse
