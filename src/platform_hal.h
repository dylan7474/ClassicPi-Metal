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

/* ---- Provided by the core, called by the platform -------------------- */

/* rom: EMU_ROM_SIZE bytes, writable (it gets patched in place).
 * disk_ctx may be 0 (no disk: the Mac shows the "?" floppy).
 * Returns 0 on success, -1 if the ROM is not a supported one,
 * -2 if there is no memory for the Mac's RAM. */
int     emu_init(uint8_t *rom, void *disk_ctx, uint32_t disk_size, int read_only);

/* Run the 68000 for one quantum (about 5 ms of Mac time). */
void    emu_run_slice(void);

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
