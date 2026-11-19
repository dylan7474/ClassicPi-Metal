#
# ClassicPi-Metal - bare-metal Mac Plus (umac) for the Raspberry Pi Zero 2 W
#
#   make            build build/kernel8.img
#   make sdcard     also gather everything for the SD card in build/sdcard/
#   make clean      remove our build output (keeps circle-stdlib)
#   make distclean  also clean circle-stdlib
#   make qemu       run it in QEMU (see tools/qemu-sd.sh)
#

PREFIX    ?= aarch64-none-elf-
RASPPI    ?= 3
MEMSIZE   ?= 4096
HOSTCC    ?= cc

STDLIB    = libs/circle-stdlib
CIRCLE    = $(STDLIB)/libs/circle
MUSASHI   = external/umac/external/Musashi
GEN       = build/gen
STDLIB_STAMP = $(STDLIB)/install/aarch64-none-circle/lib/libc.a

.PHONY: all kernel sdcard clean distclean submodules qemu

all: build/kernel8.img

submodules:
	@test -f $(CIRCLE)/Rules.mk && test -f $(MUSASHI)/m68kmake.c || \
		git submodule update --init --recursive

$(STDLIB_STAMP): | submodules
	cd $(STDLIB) && ./configure -r $(RASPPI) -p $(PREFIX) && $(MAKE)

# Musashi's opcode generator runs on the build PC, output into build/gen
$(GEN)/m68kmake: $(MUSASHI)/m68kmake.c | submodules
	mkdir -p $(GEN)
	$(HOSTCC) -O2 -o $@ $<

$(GEN)/m68kops.c $(GEN)/m68kops.h: $(GEN)/m68kmake $(MUSASHI)/m68k_in.c
	$(GEN)/m68kmake $(GEN) $(MUSASHI)/m68k_in.c

kernel: $(STDLIB_STAMP) $(GEN)/m68kops.c
	$(MAKE) -C src MEMSIZE=$(MEMSIZE)

build/kernel8.img: kernel
	mkdir -p build
	cp src/kernel8.img build/kernel8.img
	@ls -l build/kernel8.img

# SD card contents: Pi firmware (pinned to the revision Circle uses),
# config.txt, cmdline.txt and the kernel.  Add mac.rom + disk1.dsk yourself.
FIRMWARE_REV = 0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68
FIRMWARE_URL = https://raw.githubusercontent.com/raspberrypi/firmware/$(FIRMWARE_REV)/boot
FIRMWARE     = bootcode.bin start.elf fixup.dat bcm2710-rpi-zero-2-w.dtb LICENCE.broadcom

build/firmware/.done:
	mkdir -p build/firmware
	for f in $(FIRMWARE); do \
		curl -fsSL -o build/firmware/$$f $(FIRMWARE_URL)/$$f || exit 1; \
	done
	touch $@

sdcard: build/kernel8.img build/firmware/.done
	mkdir -p build/sdcard
	cp $(addprefix build/firmware/,$(FIRMWARE)) build/sdcard/
	cp sdcard/config.txt sdcard/cmdline.txt build/kernel8.img build/sdcard/
	@echo
	@echo "build/sdcard/ is ready: copy it to a FAT32 card, then add mac.rom and disk1.dsk."

clean:
	-$(MAKE) -C src clean
	rm -rf build/kernel8.img build/sdcard build/gen
	rm -f external/umac/src/*.o external/umac/src/*.d \
	      $(MUSASHI)/*.o $(MUSASHI)/*.d $(MUSASHI)/softfloat/*.o $(MUSASHI)/softfloat/*.d

distclean: clean
	-cd $(STDLIB) && $(MAKE) mrproper

# Try it in QEMU's Raspberry Pi 3 model (make the card first with tools/qemu-sd.sh).
# Serial output (the core's messages) goes to the terminal.
qemu: build/kernel8.img
	@test -f build/qemu-sd.img || { echo "Run tools/qemu-sd.sh <mac.rom> [disk1.dsk] first"; exit 1; }
	qemu-system-aarch64 -M raspi3b -kernel build/kernel8.img \
		-append "width=1024 height=768 logdev=ttyS1" \
		-drive file=build/qemu-sd.img,if=sd,format=raw \
		-serial stdio -usb -device usb-kbd -device usb-mouse
