// Basilisk II's UAE CPU core (uae_cpu_2021, interpreter) running the loop
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include <string.h>
#include <stdlib.h>
extern "C" {
#include "bench_loop.h"
unsigned long long bench_now_us (void);
}

// What basilisk_glue.cpp / main.cpp would provide
uint32 RAMBaseMac = 0;
uint8 *RAMBaseHost;
uint32 RAMSize;
uint32 ROMBaseMac;
uint8 *ROMBaseHost;
uint32 ROMSize;
uint8 *MacFrameBaseHost;
uint32 MacFrameSize;
int MacFrameLayout;
uintptr MEMBaseDiff;
int CPUType = 3;			// 68030, like a IIci
bool CPUIs68060 = false;
int FPUType = 1;			// 68881/2
bool TwentyFourBitAddressing = false;

static unsigned long long s_nStop;
static int s_nChecks;

void cpu_do_check_ticks (void)		// USE_CPU_EMUL_SERVICES: called every so often
{
	emulated_ticks = 5000;			// next check after 5000 instructions, as Basilisk II settles at
	if (bench_now_us () >= s_nStop)
	{
		quit_program = 1;
		SPCFLAGS_SET (SPCFLAG_BRK);
	}
	s_nChecks++;
}

extern "C" unsigned bench_uae (unsigned nSeconds, unsigned *pChecks)
{
	RAMSize = 8 * 1024 * 1024;
	RAMBaseHost = (uint8 *) calloc (1, RAMSize + 0x100000);
	ROMSize = 0x80000;
	ROMBaseHost = RAMBaseHost + RAMSize;	// a dummy ROM right after RAM
	MEMBaseDiff = (uintptr) RAMBaseHost;
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr (ROMBaseHost);
	memcpy (RAMBaseHost + BENCH_PC, BenchCode, sizeof BenchCode);

	init_m68k ();
	m68k_reset ();
	m68k_setpc (BENCH_PC);
	regs.s = 1;
	m68k_areg (regs, 7) = 0x8000;

	s_nStop = bench_now_us () + nSeconds * 1000000ULL;
	quit_program = 0;
	m68k_execute ();

	*pChecks = s_nChecks;
	return m68k_dreg (regs, 7);		// outer iterations done
}
