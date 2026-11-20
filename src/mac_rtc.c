/*
 * mac_rtc.c - the Mac Plus clock chip (RTC): a seconds counter and 20 bytes
 * of parameter RAM (PRAM), on three lines of VIA port B.  umac calls
 * umac_rtc_lines() / umac_rtc_data_in() (patches/umac-rtc-hooks.patch).
 * Plain C, no Circle.
 *
 * Protocol (Guide to the Macintosh Family Hardware, RTC chapter):
 *   /enable low starts a transfer.  The Mac clocks in a command byte, MSB
 *   first, one bit per rising edge of the clock line.  Command
 *   z a4 a3 a2 a1 a0 0 1:  z = 1 read, 0 write; address a = bits 6..2:
 *     0..7    seconds counter byte (a & 3), 0 = lowest
 *     8..11   PRAM 0x10..0x13
 *     12      test register (write only)
 *     13      write-protect register (write only, bit 7 = protect)
 *     16..31  PRAM 0x00..0x0F
 *   A write is followed by one data byte, clocked in the same way.  A read
 *   is followed by one data byte the chip presents, MSB first; the ROM
 *   raises the clock and then samples the data line, so each rising edge
 *   puts the next bit out.
 *   The seconds counter counts seconds since 1 Jan 1904, local time.
 */
#include <stdint.h>
#include <string.h>

#include "platform_hal.h"

static uint8_t  pram[20];
static uint32_t seconds;
static int      write_protect;

static uint8_t  last_lines = 7;         /* data, clock, /enable all high */
static int      phase;                  /* 0 command, 1 write data, 2 read data, 3 done */
static int      bit_count;
static uint8_t  shift_in;
static uint8_t  command;
static uint8_t  shift_out;
static uint8_t  data_out;               /* level on the data line when reading */

static uint8_t rtc_read(uint8_t cmd)
{
        unsigned a = (cmd >> 2) & 0x1f;

        if (a < 8)
                return (uint8_t)(seconds >> (8 * (a & 3)));
        if (a >= 8 && a <= 11)
                return pram[0x10 + (a & 3)];
        if (a >= 16)
                return pram[a & 0x0f];
        return 0;
}

static void rtc_write(uint8_t cmd, uint8_t value)
{
        unsigned a = (cmd >> 2) & 0x1f;

        if (a == 13) {                  /* write-protect register */
                write_protect = value & 0x80;
                return;
        }
        if (write_protect || a == 12)   /* protected, or the test register */
                return;

        if (a < 8) {
                unsigned shift = 8 * (a & 3);
                seconds = (seconds & ~((uint32_t)0xff << shift)) | ((uint32_t)value << shift);
        } else if (a >= 8 && a <= 11) {
                pram[0x10 + (a & 3)] = value;
        } else if (a >= 16) {
                pram[a & 0x0f] = value;
        } else {
                return;
        }
        hal_rtc_changed();              /* the platform saves PRAM + clock */
}

void umac_rtc_lines(uint8_t lines)
{
        int enabled = !(lines & 4);
        int was_enabled = !(last_lines & 4);
        int rising = (lines & 2) && !(last_lines & 2);

        last_lines = lines;

        if (!enabled)
                return;
        if (!was_enabled) {             /* a new transfer */
                phase = 0;
                bit_count = 0;
                shift_in = 0;
        }
        if (!rising)
                return;

        switch (phase) {
        case 0:                         /* command byte in */
        case 1:                         /* write data byte in */
                shift_in = (uint8_t)((shift_in << 1) | (lines & 1));
                if (++bit_count < 8)
                        break;
                bit_count = 0;
                if (phase == 0) {
                        command = shift_in;
                        if (command & 0x80) {
                                shift_out = rtc_read(command);
                                phase = 2;
                        } else {
                                phase = 1;
                        }
                } else {
                        rtc_write(command, shift_in);
                        phase = 3;
                }
                shift_in = 0;
                break;

        case 2:                         /* read data byte out */
                data_out = (shift_out >> 7) & 1;
                shift_out = (uint8_t)(shift_out << 1);
                if (++bit_count == 8)
                        phase = 3;
                break;

        default:
                break;
        }
}

uint8_t umac_rtc_data_in(void)
{
        return data_out;
}

/* ---- platform_hal.h ------------------------------------------------- */

void emu_rtc_load(const uint8_t *pram20, uint32_t mac_seconds)
{
        if (pram20)
                memcpy(pram, pram20, sizeof pram);
        seconds = mac_seconds;
}

void emu_rtc_save(uint8_t *pram20, uint32_t *mac_seconds)
{
        memcpy(pram20, pram, sizeof pram);
        *mac_seconds = seconds;
}

void emu_rtc_tick(void)
{
        seconds++;
}

void emu_rtc_set_seconds(uint32_t mac_seconds)
{
        seconds = mac_seconds;
}

uint32_t emu_rtc_seconds(void)
{
        return seconds;
}
