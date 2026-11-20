# ClassicPi-Metal

A Macintosh Plus running bare-metal on a Raspberry Pi, with no Linux
underneath. It powers on straight into the Mac.

| Pi | Kernel | Tested in QEMU |
|---|---|---|
| Zero, Zero W, Pi 1 | `kernel.img` (32-bit, ARMv6) | Boots System 7.0.1 (`raspi0`) |
| Pi 2 | `kernel7.img` (32-bit, ARMv7) | Can't test: QEMU's `raspi2b` shows nothing, even for Circle's own samples |
| Zero 2 W, Pi 3B / 3B+ / 3A+ | `kernel8.img` (64-bit) | Boots System 7.0.1 (`raspi3b`) |
| Pi 4, Pi 400 | `kernel8-rpi4.img` (64-bit) + `armstub8-rpi4.bin` | Starts, then can't open the SD card in QEMU's `raspi4b` (QEMU or us: unknown) |
| Pi 5, Pi 500 | `kernel_2712.img` (64-bit) | No QEMU model |

One universal card carries every kernel, and the firmware picks the right
one for the board (see `sdcard/config.txt`). None of them has been tried on
real hardware yet.

It uses Matt Evans' [umac](https://github.com/evansm7/umac) emulator (Musashi
68000 core) on [Circle](https://github.com/rsta2/circle) through
[circle-stdlib](https://github.com/smuehlst/circle-stdlib). This is the
bare-metal milestone of [ClassicPi](https://github.com/dylan7474/ClassicPi),
which runs Mini vMac on a Buildroot Linux image.

**What works now** (in QEMU):

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
| `bootcode.bin`, `start.elf`, `fixup.dat` (Pi 1–3), `start4.elf`, `fixup4.dat` (Pi 4), device trees for the Zero W, Zero 2 W, Pi 4/400, Pi 5/500, `overlays/bcm2712d0.dtbo`, `LICENCE.broadcom` | Raspberry Pi firmware (downloaded by `make sdcard`; the Pi 5 boots from its own EEPROM) |
| `config.txt` | Picks the kernel per model, HDMI 1024x768, smallest GPU memory |
| `cmdline.txt` | Sends Circle's log to the serial port, not the screen |
| `kernel.img`, `kernel7.img`, `kernel8.img`, `kernel8-rpi4.img`, `kernel_2712.img`, `armstub8-rpi4.bin` | This program, one kernel per Pi family (see the table at the top); the armstub is Circle's Pi 4 start-up code |
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
Mac (1536x1026). The Pi 5 ignores those lines and always uses the screen's
preferred mode.

**USB:** the Zero, Zero W and Zero 2 W have one micro-USB data port (the one
marked USB, not PWR). For a keyboard and a mouse you need a USB OTG hub, or a keyboard with a
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

- Arm's bare-metal toolchains, both tested with 15.2.Rel1. On Arch/Garuda:
  - 64-bit, `aarch64-none-elf-gcc`: `paru -S arm-gnu-toolchain-aarch64-none-elf-bin`
  - 32-bit, `arm-none-eabi-gcc`: `paru -S arm-gnu-toolchain-arm-none-eabi-bin`
- `rsync`, because each target builds in its own copy of the sources.
- A host C compiler, plus `make`, `git` and `curl`.
- For QEMU testing: `qemu-system-aarch64` and `qemu-system-arm`, `mtools`, `dosfstools`.

```sh
git clone --recursive git@github.com:dylan7474/ClassicPi-Metal.git
cd ClassicPi-Metal
make            # every kernel; the first run builds circle-stdlib five times (~4 min with -j8)
make pi3        # or one target: pi1 pi2 pi3 pi4 pi5
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
make qemu       # 64-bit kernel, QEMU raspi3b
make qemu-pi1   # 32-bit kernel, QEMU raspi0
```

QEMU runs the same kernels unchanged. In 32-bit mode it needs `-bios`
instead of `-kernel`, and there's no `cmdline.txt`, so the target sets the
screen size on QEMU's side. QEMU's speed tells you nothing about the real Pi.

## How it fits together

```
src/kernel.cpp      Circle side: SD card (FatFs), HDMI blit, USB input, 60 Hz / 1 Hz timing
src/platform_hal.h  the ONLY interface between the two sides (plain C)
src/emu_glue.c      umac behind that interface; Mac RAM from the heap
src/keymap_usb.c    USB HID usage codes -> Mac key codes
external/umac       umac + Musashi, unmodified (git submodule)
libs/circle-stdlib  Circle + newlib (git submodule)
build/<target>/     per-target copies of circle-stdlib, umac and src/, plus
                    gen/ (Musashi's opcode tables, generated on the PC).
                    Circle builds in place, so the targets can't share a tree.
```

The emulator core never sees a Circle class, and the Circle code never sees a
umac header. Both sides go through `platform_hal.h`. The 68000 runs in about
5 ms slices. Between slices the main loop feeds in queued keyboard and mouse
events, raises the Mac's 60 Hz VBlank and 1 Hz interrupts, and redraws only
the screen rows that changed.

## Roadmap

1. **First boot on real hardware:** a Zero 2 W, then a Zero W. Check speed (the Zero W's single 1 GHz ARM11 is the slowest), mouse feel and SD write behaviour.
2. **Universal card on real boards:** every kernel is built and on the card. The Pi 2, 4 and 5 kernels can't be fully tested in QEMU, so each needs a first boot on a real board. On the Pi 4, check the SD card first (QEMU couldn't open it).
3. **Speed**, if it needs it: run the blit on a second core, and mark the hot 68000 opcodes (`tools/fn_hot200.txt` in umac).
4. **Sound:** the Mac Plus sound buffer to PWM or I2S audio (the Phonosphere MAX98357A amp would do). umac has no sound yet, so this needs work in the core.
5. **On-screen disk switcher:** pick a `.dsk` from the card with a hot-key overlay. This needs umac's eject and insert path.
6. **Second drive:** umac has two drive slots, but only one is wired up so far.
7. **VIA timers:** missing in umac, so some games run too fast.
8. **Real floppy drive.** Two levels:
   - *USB floppy drive (cheap, easiest).* These read only 1.44 MB MFM disks, and Circle seems to support only bulk-only USB storage (`umsd`), while many floppy drives use the older CBI protocol, so **check a drive before buying**. A Mac Plus can't read 1.44 MB disks itself, but umac's disk driver doesn't care, so an HFS 1.44 MB floppy (written by a later Mac or a PC tool) could be mounted as the second drive.
   - *Original 400K/800K Mac disks.* These are GCR, recorded at varying speeds, and a USB floppy drive can't read them at all. That needs a flux reader (Greaseweazle, FluxEngine or Applesauce) with a real 3.5" drive: read the flux, decode GCR to a disk image, then mount it. It's a bigger job, but it would let the Pi boot genuine old Mac floppies.
9. **The ClassicPi case** with a Zero 2 W inside, and a floppy slot.

## Licence

GPLv3, because Circle is GPLv3 (see `LICENSE`). umac and Musashi are
permissive (MIT-style) and keep their own licences in their submodules.
