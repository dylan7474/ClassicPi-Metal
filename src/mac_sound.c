/*
 * mac_sound.c - the Mac Plus sound output, as samples for the Pi.  Plain C.
 *
 * The real hardware reads one word per scan line from a 370-word buffer
 * near the top of RAM (22.25 kHz, 370 words a frame) and turns the HIGH
 * byte into a pulse width; the low byte is the floppy motor speed.
 * (linux-mac68k.sourceforge.net/devel/plushw.php: main buffer = RAM top
 * - 0x300, alternate = RAM top - 0x5F00; VIA A[2:0] volume, A[3] 1 = main
 * buffer, B[7] 0 = sound on.)  umac passes the VIA ports out through
 * patches/umac-sound-hooks.patch.
 *
 * Called once per frame, just before the Mac's VBL: the buffer then holds
 * the frame the Mac prepared at the last VBL.  The 370 samples are
 * stretched to however many output frames the platform asks for (about
 * 800 at 48 kHz), so the platform can steer its queue level.
 */
#include <stdint.h>

#include "platform_hal.h"

#define SAMPLES_PER_FRAME       370

extern uint8_t umac_via_pa, umac_via_pb;        /* umac-sound-hooks.patch */

const uint8_t *emu_ram_base(void);              /* emu_glue.c */
uint32_t emu_ram_size(void);

static int32_t dc_x1, dc_y1;                    /* DC blocker state */

void emu_sound_render(int16_t *out, unsigned frames)
{
        const uint8_t *ram = emu_ram_base();
        uint32_t top = emu_ram_size();

        if (!ram || frames == 0) {
                for (unsigned i = 0; i < 2 * frames; i++)
                        out[i] = 0;
                return;
        }

        const uint8_t *buf = ram + ((umac_via_pa & 0x08) ? top - 0x300 : top - 0x5f00);
        int on = !(umac_via_pb & 0x80);
        int volume = umac_via_pa & 7;           /* 0..7 */

        for (unsigned i = 0; i < frames; i++) {
                /* position in the 370 samples, 16.16 fixed point */
                uint32_t pos = (uint32_t)(((uint64_t)i * SAMPLES_PER_FRAME << 16) / frames);
                unsigned idx = pos >> 16;
                int32_t frac = (int32_t)(pos & 0xffff);
                int32_t s0 = buf[2 * idx];      /* high byte of each word */
                int32_t s1 = idx + 1 < SAMPLES_PER_FRAME ? buf[2 * (idx + 1)] : s0;
                int32_t x = s0 + (((s1 - s0) * frac) >> 16);    /* 0..255 */

                x = on ? (x - 128) * 256 * volume / 7 : 0;

                /* DC blocker: the Mac's 'silence' is any constant level */
                int32_t y = x - dc_x1 + ((dc_y1 * 4055) >> 12);
                dc_x1 = x;
                dc_y1 = y;

                if (y > 32767)  y = 32767;
                if (y < -32768) y = -32768;
                out[2 * i] = out[2 * i + 1] = (int16_t)y;
        }
}
