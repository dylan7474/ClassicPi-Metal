/*
 * platform_hal.h - the only interface between the Mac emulator core (C)
 * and the bare-metal platform (Circle, C++).
 *
 * The core (umac + Musashi, plus emu_glue.c) never sees a Circle class.
 * The platform never sees a umac header.  Everything crosses here.
 */
#ifndef PLATFORM_HAL_H
#define PLATFORM_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Emulated machine geometry -------------------------------------- */
#define EMU_SCREEN_WIDTH        512
#define EMU_SCREEN_HEIGHT       342
#define EMU_SCREEN_STRIDE       (EMU_SCREEN_WIDTH / 8)  /* bytes per row, 1 bpp */
#define EMU_ROM_SIZE            (128 * 1024)            /* Mac Plus ROM */

/* ---- Provided by the platform, called by the core -------------------- */

/* Disk image access.  ctx is whatever the platform passed to emu_init().
 * Offsets and lengths are byte values, always multiples of 512.
 * Return 0 on success, negative on error. */
int     hal_disk_read(void *ctx, uint8_t *buf, uint32_t offset, uint32_t len);
int     hal_disk_write(void *ctx, const uint8_t *buf, uint32_t offset, uint32_t len);

/* The Mac changed PRAM or set its clock: the platform should save them
 * (emu_rtc_save) soon, after a quiet spell. */
void    hal_rtc_changed(void);

/* ---- Provided by the core, called by the platform -------------------- */

/* rom: EMU_ROM_SIZE bytes, writable (it gets patched in place).
 * disk_ctx may be 0 (no disk: the Mac shows the "?" floppy).
 * Returns 0 on success, -1 if the ROM is not a supported one,
 * -2 if there is no memory for the Mac's RAM. */
int     emu_init(uint8_t *rom, void *disk_ctx, uint32_t disk_size, int read_only);

/* Clock chip (mac_rtc.c).  Mac time = seconds since 1 Jan 1904, local.
 * PRAM is 20 bytes.  emu_rtc_load() before emu_init() (pram may be 0). */
#define EMU_PRAM_SIZE           20
void    emu_rtc_load(const uint8_t *pram20, uint32_t mac_seconds);
void    emu_rtc_save(uint8_t *pram20, uint32_t *mac_seconds);
uint32_t emu_rtc_seconds(void);
/* Set the time while the Mac runs (e.g. from NTP): the clock chip and the
 * Mac's own running clock (low-memory global Time, 0x20C). */
void    emu_set_time(uint32_t mac_seconds);

/* Run the 68000 for one quantum (about 5 ms of Mac time). */
void    emu_run_slice(void);

/* Mac time covered by one emu_run_slice(), in microseconds.  The core runs
 * as fast as the host allows, so slices per second x this = Mac speed. */
unsigned emu_slice_us(void);

/* Sound (mac_sound.c): the Mac's current frame of sound (370 samples at
 * 22.25 kHz) as `frames` stereo 16-bit frames, stretched to fit (about 800
 * for 48 kHz at 60 Hz).  Call once per frame, just before emu_vsync(). */
void    emu_sound_render(int16_t *out, unsigned frames);

/* Time events: call at 60 Hz and 1 Hz. */
void    emu_vsync(void);
void    emu_1hz(void);

/* Input.  usage is a USB HID keyboard usage code (0x04 = A ...);
 * modifiers use the standard usages 0xE0-0xE7 (LCtrl ... RGUI).
 * The core translates to Mac key codes (keymap_usb.c). */
void    emu_usb_key(uint8_t usage, int down);
/* dx right-positive, dy down-positive (screen convention); button 0/1. */
void    emu_mouse(int dx, int dy, int button);

/* The live 1-bit framebuffer: EMU_SCREEN_HEIGHT rows of
 * EMU_SCREEN_STRIDE bytes, MSB = leftmost pixel, 1 = black. */
const uint8_t *emu_framebuffer(void);

/* RAM size the core was built with, in KB (for the startup banner). */
unsigned emu_ram_kb(void);

#ifdef __cplusplus
}
#endif

#endif
