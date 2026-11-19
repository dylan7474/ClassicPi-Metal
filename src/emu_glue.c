/*
 * emu_glue.c - wraps umac behind platform_hal.h.  Plain C, no Circle.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "platform_hal.h"
#include "umac.h"
#include "rom.h"
#include "machw.h"
#include "keymap.h"

#if DISP_WIDTH != EMU_SCREEN_WIDTH || DISP_HEIGHT != EMU_SCREEN_HEIGHT
#error "umac display size does not match platform_hal.h"
#endif

/* Mac RAM comes from the heap: up to 4 MB is far too big for the
 * kernel image area (Circle reserves only 4 MB for code + BSS). */
static uint8_t *mac_ram;

static int disk_read_cb(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
        return hal_disk_read(ctx, data, offset, len);
}

static int disk_write_cb(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
        return hal_disk_write(ctx, data, offset, len);
}

int emu_init(uint8_t *rom, void *disk_ctx, uint32_t disk_size, int read_only)
{
        if (rom_patch(rom))
                return -1;

        mac_ram = malloc(RAM_SIZE);
        if (!mac_ram)
                return -2;

        disc_descr_t discs[DISC_NUM_DRIVES];
        memset(discs, 0, sizeof(discs));
        memset(mac_ram, 0, RAM_SIZE);

        if (disk_ctx) {
                discs[0].base = 0;      /* 0 = use the read/write callbacks */
                discs[0].size = disk_size;
                discs[0].read_only = read_only;
                discs[0].op_ctx = disk_ctx;
                discs[0].op_read = disk_read_cb;
                discs[0].op_write = disk_write_cb;
        }

        umac_init(mac_ram, rom, discs);
        return 0;
}

void emu_run_slice(void)
{
        umac_loop();
}

void emu_vsync(void)
{
        umac_vsync_event();
}

void emu_1hz(void)
{
        umac_1hz_event();
}

uint8_t usb_to_mac_key(uint8_t usage);   /* keymap_usb.c */

static void mac_key(uint8_t mac_keycode, int down)
{
        /* umac wants the keyboard-protocol form: code << 1 | 1 */
        umac_kbd_event((uint8_t)((mac_keycode << 1) | 1), down);
}

void emu_usb_key(uint8_t usage, int down)
{
        static int caps_on;
        uint8_t mk = usb_to_mac_key(usage);

        if (mk == MKC_None)
                return;
        if (mk == MKC_CapsLock) {
                /* The Mac key physically locks down; a USB key does not.
                 * Toggle on each press. */
                if (!down)
                        return;
                caps_on = !caps_on;
                mac_key(mk, caps_on);
                return;
        }
        mac_key(mk, down);
}

void emu_mouse(int dx, int dy, int button)
{
        /* umac's y axis is up-positive */
        umac_mouse(dx, -dy, button);
}

const uint8_t *emu_framebuffer(void)
{
        return mac_ram + umac_get_fb_offset();
}

unsigned emu_ram_kb(void)
{
        return UMAC_MEMSIZE;
}
