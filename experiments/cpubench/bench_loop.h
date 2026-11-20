/* The benchmark loop (68000 code, also valid on a 68030), at 0x1000.
 * Per outer iteration: 64 x (move.l (a0),d0; add.l d6,d0; move.l d0,(a0)+;
 * lsl.l #1,d0; eor.l d0,d1; dbra) + 6 = 389 instructions.  d7 counts. */
#define BENCH_PC        0x1000
#define BENCH_INSNS     389
static const unsigned char BenchCode[] = {
        0x41,0xF9,0x00,0x01,0x00,0x00,  /* 1000 lea $10000,a0 */
        0x7E,0x00,                      /* 1006 moveq #0,d7 */
        0x3C,0x3C,0x00,0x3F,            /* 1008 loop: move.w #63,d6 */
        0x20,0x10,                      /* 100C inner: move.l (a0),d0 */
        0xD0,0x86,                      /* 100E add.l d6,d0 */
        0x20,0xC0,                      /* 1010 move.l d0,(a0)+ */
        0xE3,0x88,                      /* 1012 lsl.l #1,d0 */
        0xB1,0x81,                      /* 1014 eor.l d0,d1 */
        0x51,0xCE,0xFF,0xF4,            /* 1016 dbra d6,inner */
        0x41,0xF9,0x00,0x01,0x00,0x00,  /* 101A lea $10000,a0 */
        0x52,0x87,                      /* 1020 addq.l #1,d7 */
        0x21,0xC7,0x0F,0x00,            /* 1022 move.l d7,$0F00.w */
        0x60,0xE0,                      /* 1026 bra loop */
};
