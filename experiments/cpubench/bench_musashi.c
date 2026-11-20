/* umac's Musashi 68000 core running the same loop, RAM only */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "bench_loop.h"
unsigned long long bench_now_us (void);

static uint8_t *ram;
#define RAMMASK (8 * 1024 * 1024 - 1)

unsigned int cpu_read_byte (unsigned int a) { return ram[a & RAMMASK]; }
unsigned int cpu_read_word (unsigned int a) { a &= RAMMASK; return ram[a] << 8 | ram[a + 1]; }
unsigned int cpu_read_long (unsigned int a) { return cpu_read_word (a) << 16 | cpu_read_word (a + 2); }
unsigned int (*cpu_read_instr) (unsigned int) = cpu_read_word;  /* umac fetches opcodes through this */
unsigned int cpu_read_word_dasm (unsigned int a) { return cpu_read_word (a); }
unsigned int cpu_read_long_dasm (unsigned int a) { return cpu_read_long (a); }
void cpu_write_byte (unsigned int a, unsigned int v) { ram[a & RAMMASK] = v; }
void cpu_write_word (unsigned int a, unsigned int v) { a &= RAMMASK; ram[a] = v >> 8; ram[a + 1] = v; }
void cpu_write_long (unsigned int a, unsigned int v) { cpu_write_word (a, v >> 16); cpu_write_word (a + 2, v); }
int  cpu_irq_ack (int level) { (void) level; return M68K_INT_ACK_AUTOVECTOR; }
void cpu_pulse_reset (void) {}
void cpu_instr_callback (int pc) { (void) pc; }

unsigned bench_musashi (unsigned nSeconds, unsigned long long *pCycles)
{
	ram = calloc (1, RAMMASK + 1);
	memcpy (ram + BENCH_PC, BenchCode, sizeof BenchCode);
	cpu_write_long (0, 0x8000);		/* reset SSP */
	cpu_write_long (4, BENCH_PC);		/* reset PC */
	m68k_init ();
	m68k_set_cpu_type (M68K_CPU_TYPE_68000);
	m68k_pulse_reset ();

	unsigned long long nStop = bench_now_us () + nSeconds * 1000000ULL, nCycles = 0;
	while (bench_now_us () < nStop)
	{
		nCycles += m68k_execute (40000);	/* umac's slice: 5 ms at 8 MHz */
	}
	*pCycles = nCycles;
	return m68k_get_reg (NULL, M68K_REG_D7);
}
