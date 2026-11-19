# ClassicPi-Metal

A Macintosh Plus running bare-metal on a Raspberry Pi Zero 2 W, with no Linux
underneath. It powers on straight into the Mac.

It uses Matt Evans' [umac](https://github.com/evansm7/umac) emulator (Musashi
68000 core) on [Circle](https://github.com/rsta2/circle) through
[circle-stdlib](https://github.com/smuehlst/circle-stdlib). This is the
bare-metal milestone of [ClassicPi](https://github.com/dylan7474/ClassicPi),
which runs Mini vMac on a Buildroot Linux image.

**What works now** (tested in QEMU's Raspberry Pi 3 model, not yet on a real Zero 2 W):

- System 7.0.1 boots to the Finder from a 200 MB disk image, with 4 MB of Mac RAM.
- HDMI output: the 512x342 screen is scaled by the largest whole number
  that fits and centred on black (2x on a 1024x768 panel).
- USB keyboard and mouse, plugged in at any time.
- Reads and writes go straight to the disk image file on the SD card.
  Writes are flushed one second after the Mac stops writing.
- If the ROM is missing or wrong, the screen says so in plain words.

## What goes on the SD card

Use a FAT32 card (one partition). Run `make sdcard` and copy everything from
`build/sdcard/` to it. Then add your own ROM and disk:

| File | What it is |
|---|---|
| `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2710-rpi-zero-2-w.dtb`, `LICENCE.broadcom` | Raspberry Pi firmware (downloaded by `make sdcard`) |
| `config.txt` | 64-bit mode, HDMI 1024x768, smallest GPU memory |
| `cmdline.txt` | Sends Circle's log to the serial port, not the screen |
| `kernel8.img` | This program |
| `mac.rom` | **Yours:** a Mac Plus v3 ROM, checksum `4D1F8172`, 128 KB (`vMac.ROM` also accepted; extra bytes after 128 KB are ignored) |
| `disk1.dsk` | **Yours:** a raw HFS disk image (`disk1.img` and `disk.dsk` also accepted) |

The ROM and System software are Apple's copyright, so they are never in this
repository (`.gitignore` blocks `*.rom` and `*.dsk`). Dump the ROM from a Mac
you own.

The disk image must be **raw**. Its size must be a multiple of 512 bytes.
A Disk Copy 4.2 image has an 84-byte header that needs stripping first:
`dd if=in.image of=disk1.dsk bs=84 skip=1`. The screen tells you if the size
looks wrong. A file marked read-only on the card is mounted read-only.

**Screen size:** to use your screen's own preferred mode, delete the
`hdmi_group` and `hdmi_mode` lines from `config.txt`. A 1080p TV gives a 3x
Mac (1536x1026).

**USB:** the Zero 2 W has one micro-USB data port (the one marked USB, not
PWR). For a keyboard and a mouse you need a USB OTG hub, or a keyboard with a
built-in trackpad.

**Keys on a PC keyboard:**

| PC key | Mac key |
|---|---|
| Ctrl | Command (cloverleaf) |
| Windows/Super | Command |
| Alt, AltGr | Option |
| Caps Lock | Caps Lock (stays down until pressed again, like the real key) |

Any mouse button is the Mac's one button.

**Serial console:** GPIO 14/15, 115200 baud. It shows the startup messages and
the emulator's own messages.

## Building

You need:

- Arm's bare-metal AArch64 toolchain, `aarch64-none-elf-gcc`. On Arch/Garuda:
  `paru -S arm-gnu-toolchain-aarch64-none-elf-bin`. Tested with 15.2.Rel1.
- A host C compiler, plus `make`, `git` and `curl`.
- For QEMU testing: `qemu-system-aarch64`, `mtools`, `dosfstools`.

```sh
git clone --recursive git@github.com:dylan7474/ClassicPi-Metal.git
cd ClassicPi-Metal
make            # first run builds circle-stdlib (~2 min), then build/kernel8.img
make sdcard     # build/sdcard/ = everything for the card except ROM + disk
```

### A ready-to-flash card image

```sh
make sdcard
tools/make-sdimage.sh                       # uses local/mac.rom + local/disk1.dsk
tools/make-sdimage.sh my.rom my.dsk out.img # or name them
```

This makes `build/ClassicPi-Metal.img`, one FAT32 partition with everything
on it, including your ROM and disk. Flash it with Raspberry Pi Imager
("Use custom") or `dd`. `local/` is gitignored, so your ROM and disks there
never get committed.

`make MEMSIZE=1024` builds a 1 MB Mac. System 6 is fine with that; System 7
needs 2048 or more. The default is 4096.

### Trying it in QEMU

```sh
tools/qemu-sd.sh /path/to/mac.rom /path/to/disk1.dsk   # makes build/qemu-sd.img (a copy)
make qemu
```

QEMU's `raspi3b` machine emulates the same SoC family, so it runs the same
kernel unchanged. Its speed tells you nothing about the real Pi.

## How it fits together

```
src/kernel.cpp      Circle side: SD card (FatFs), HDMI blit, USB input, 60 Hz / 1 Hz timing
src/platform_hal.h  the ONLY interface between the two sides (plain C)
src/emu_glue.c      umac behind that interface; Mac RAM from the heap
src/keymap_usb.c    USB HID usage codes -> Mac key codes
external/umac       umac + Musashi, unmodified (git submodule)
libs/circle-stdlib  Circle + newlib (git submodule)
build/gen           Musashi's generated opcode tables (built on the PC)
```

The emulator core never sees a Circle class, and the Circle code never sees a
umac header. Both sides go through `platform_hal.h`. The 68000 runs in about
5 ms slices. Between slices the main loop feeds in queued keyboard and mouse
events, raises the Mac's 60 Hz VBlank and 1 Hz interrupts, and redraws only
the screen rows that changed.

## Roadmap

1. **First boot on a real Zero 2 W.** Then check speed, mouse feel and SD write behaviour.
2. **Speed**, if it needs it: run the blit on a second core, and mark the hot 68000 opcodes (`tools/fn_hot200.txt` in umac).
3. **Sound:** the Mac Plus sound buffer to PWM or I2S audio (the Phonosphere MAX98357A amp would do). umac has no sound yet, so this needs work in the core.
4. **On-screen disk switcher:** pick a `.dsk` from the card with a hot-key overlay. This needs umac's eject and insert path.
5. **Second drive:** umac has two drive slots, but only one is wired up so far.
6. **VIA timers:** missing in umac, so some games run too fast.
7. **Real floppy drive.** Two levels:
   - *USB floppy drive (cheap, easiest).* These read only 1.44 MB MFM disks, and Circle seems to support only bulk-only USB storage (`umsd`), while many floppy drives use the older CBI protocol, so **check a drive before buying**. A Mac Plus can't read 1.44 MB disks itself, but umac's disk driver doesn't care, so an HFS 1.44 MB floppy (written by a later Mac or a PC tool) could be mounted as the second drive.
   - *Original 400K/800K Mac disks.* These are GCR, recorded at varying speeds, and a USB floppy drive can't read them at all. That needs a flux reader (Greaseweazle, FluxEngine or Applesauce) with a real 3.5" drive: read the flux, decode GCR to a disk image, then mount it. It's a bigger job, but it would let the Pi boot genuine old Mac floppies.
8. **The ClassicPi case** with a Zero 2 W inside, and a floppy slot.

## Licence

GPLv3, because Circle is GPLv3 (see `LICENSE`). umac and Musashi are
permissive (MIT-style) and keep their own licences in their submodules.
