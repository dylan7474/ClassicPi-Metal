// cpubench: Basilisk II's UAE 68030 core vs umac's Musashi 68000 core on
// the same 68000 loop, at the Pi's default and maximum ARM clock.
#include "kernel.h"
#include <circle/machineinfo.h>
#include <circle/timer.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SECONDS		15
#define BENCH_INSNS	389

extern "C" {
unsigned bench_musashi (unsigned nSeconds, unsigned long long *pCycles);
unsigned bench_uae (unsigned nSeconds, unsigned *pChecks);
unsigned long long bench_now_us (void) { return CTimer::GetClockTicks64 (); }
}

void CKernel::Say (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = vsnprintf (Buf, sizeof Buf - 3, pFormat, Args);
	va_end (Args);
	if (n < 0) return;
	Buf[n++] = '\r'; Buf[n++] = '\n'; Buf[n] = 0;
	mScreen.Write (Buf, n);
	if (m_nReport + n < sizeof m_Report)
	{
		memcpy (m_Report + m_nReport, Buf, n);
		m_nReport += n;
	}
}

void CKernel::RunOnce (const char *pClock)
{
	Say ("");
	Say ("ARM clock %s: %u MHz", pClock, m_CPUThrottle.GetClockRate () / 1000000);

	unsigned long long nCycles;
	unsigned nIter = bench_musashi (SECONDS, &nCycles);
	double fMips = (double) nIter * BENCH_INSNS / SECONDS / 1e6;
	Say ("  umac's Musashi (68000):        %7u iterations/s  %6.2f MIPS  %6.1f MHz of 68000 cycles",
	     nIter / SECONDS, fMips, (double) nCycles / SECONDS / 1e6);

	unsigned nChecks;
	nIter = bench_uae (SECONDS, &nChecks);
	fMips = (double) nIter * BENCH_INSNS / SECONDS / 1e6;
	Say ("  Basilisk II's UAE (68030):     %7u iterations/s  %6.2f MIPS", nIter / SECONDS, fMips);
}

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	Say ("cpubench on %s: %u s per run", CMachineInfo::Get ()->GetMachineName (), SECONDS);
	Say ("Loop: 389 instructions per iteration (move.l/add.l/move.l/lsl.l/eor.l/dbra x64)");
	Say ("For scale (estimates): a real Mac Plus ~1,600-2,000 iterations/s; a real IIci (25 MHz 68030) ~10,000-13,000");

	RunOnce ("default");
	m_CPUThrottle.SetSpeed (CPUSpeedMaximum);
	RunOnce ("maximum");
	m_CPUThrottle.SetSpeed (CPUSpeedLow);

	char Name[64];
	snprintf (Name, sizeof Name, "cpubench-%s.txt", CMachineInfo::Get ()->GetMachineName ());
	for (char *p = Name; *p; p++) if (*p == ' ') *p = '-';
	FILE *pFile = fopen (Name, "w");
	if (pFile)
	{
		fwrite (m_Report, 1, m_nReport, pFile);
		fclose (pFile);
		Say ("Saved %s on the card. Done: switch off.", Name);
	}
	else
	{
		Say ("Could not write %s. Done: switch off.", Name);
	}
	return ShutdownHalt;
}
