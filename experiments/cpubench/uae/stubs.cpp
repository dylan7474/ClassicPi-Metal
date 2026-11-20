// What the rest of Basilisk II would supply to its CPU core (benchmark only)
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
B2_mutex *spcflags_lock = NULL;			// one thread: no locking needed
void B2_lock_mutex (B2_mutex *) {}
void B2_unlock_mutex (B2_mutex *) {}
int32 emulated_ticks = 5000;			// instructions until the next tick check
void EmulOp (uint16, M68kRegisters *) {}
int intlev (void) { return 0; }			// no interrupts in the benchmark
void report_double_bus_error () {}
