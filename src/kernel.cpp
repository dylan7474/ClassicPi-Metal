//
// kernel.cpp - ClassicPi-Metal: bare-metal Mac Plus on a Raspberry Pi
//
// The Circle side: screen, USB keyboard/mouse, SD card, timing.
// The Mac itself lives behind platform_hal.h.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "kernel.h"
#include "platform_hal.h"

#include <circle/bcmframebuffer.h>
#include <circle/machineinfo.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle_glue.h>
#include <stdarg.h>
#include <stdio.h>

#define FROM		"cpmetal"
#define VERSION		"0.1.0"

#define VSYNC_US	16667		// 60 Hz
#define SYNC_AFTER_US	1000000		// flush disk writes after 1 s quiet
#define STATS_US	10000000	// speed line on the serial port every 10 s
#define SPEED_FILE_AT	6		// write the speed file after the 6th (60 s)

// ROM and disk names looked for in the root of the SD card (case does not matter)
static const char *const ROMNames[]  = { "SD:/mac.rom", "SD:/vMac.ROM", "SD:/MacPlus.ROM" };
static const char *const DiskNames[] = { "SD:/disk1.dsk", "SD:/disk1.img", "SD:/disk.dsk" };

#define MAC_PLUS_V3_CHECKSUM	0x4D1F8172

CKernel *CKernel::s_pThis = 0;

static u8 s_Shadow[EMU_SCREEN_HEIGHT * EMU_SCREEN_STRIDE];	// last frame drawn

// ---- Input queue, filled in interrupt context ----------------------------

struct TKeyEvent { u8 Usage; u8 Down; };

static TKeyEvent s_KeyQueue[64];
static volatile unsigned s_nKeyIn, s_nKeyOut;
static volatile int s_nMouseDX, s_nMouseDY;
static volatile unsigned s_nMouseButtons;

static void QueueKey (u8 Usage, bool bDown)
{
	unsigned nNext = (s_nKeyIn + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);
	if (nNext == s_nKeyOut)
	{
		return;		// full: drop
	}
	s_KeyQueue[s_nKeyIn].Usage = Usage;
	s_KeyQueue[s_nKeyIn].Down = bDown;
	s_nKeyIn = nNext;
}

// ---- Disk access for the core (platform_hal.h) ---------------------------

static volatile bool s_bDiskDirty;
static u64 s_nLastWrite;

extern "C" int hal_disk_read (void *ctx, uint8_t *buf, uint32_t offset, uint32_t len)
{
	FIL *pFile = (FIL *) ctx;
	UINT nRead;
	if (   f_lseek (pFile, offset) != FR_OK
	    || f_read (pFile, buf, len, &nRead) != FR_OK
	    || nRead != len)
	{
		return -1;
	}
	return 0;
}

extern "C" int hal_disk_write (void *ctx, const uint8_t *buf, uint32_t offset, uint32_t len)
{
	FIL *pFile = (FIL *) ctx;
	UINT nWritten;
	if (   f_lseek (pFile, offset) != FR_OK
	    || f_write (pFile, buf, len, &nWritten) != FR_OK
	    || nWritten != len)
	{
		return -1;
	}
	s_bDiskDirty = true;
	s_nLastWrite = CTimer::GetClockTicks64 ();
	return 0;
}

// ---- Kernel ---------------------------------------------------------------

// ---- Boot log -------------------------------------------------------------
// Everything said during startup, Circle's own log lines and the speed lines,
// kept in RAM and written to SD:/bootlog.txt at each stage (a dozen small
// writes per boot, then it stops).  Survives a dead screen.

#define BOOTLOG_SIZE	32768

static char s_BootLog[BOOTLOG_SIZE];
static volatile unsigned s_nBootLog;

static void BootLogAppend (const char *p, size_t n)
{
	EnterCritical ();
	if (s_nBootLog + n > BOOTLOG_SIZE - 1)
	{
		n = BOOTLOG_SIZE - 1 - s_nBootLog;	// full: keep the start
	}
	memcpy (s_BootLog + s_nBootLog, p, n);
	s_nBootLog += n;
	LeaveCritical ();
}

int CLogTee::Write (const void *pBuffer, size_t nCount)
{
	BootLogAppend ((const char *) pBuffer, nCount);
	return m_pSerial->Write (pBuffer, nCount);
}

// ---- Kernel ---------------------------------------------------------------

CKernel::CKernel (void)
:	CStdlibAppScreen ("ClassicPi-Metal"),
	m_USBHCI (&mInterrupt, &mTimer, TRUE),
	m_EMMC (&mInterrupt, &mTimer, &mActLED),
	m_SerialConsole (&mSerial, &mSerial),
	m_LogTee (&mSerial),
	m_bSDOK (false),
	m_bScreenOK (false),
	m_bScaleFallback (false),
	m_bTraceBlit (false),
	m_nUSBLogFlushes (0),
	m_bUSBRemoved (false),
	m_pROM (0),
	m_bDiskOpen (false),
	m_nDiskSize (0),
	m_pDiskName (0),
	m_pFB (0)
{
	s_pThis = this;
	for (unsigned i = 0; i < MAX_INPUTS; i++)
	{
		m_pKeyboards[i] = 0;
		m_pMice[i] = 0;
	}
}

// Our own start-up order (not CStdlibAppScreen::Initialize, which gives up
// silently if the screen fails): serial and log first, then the SD card so
// a log can be written, then the screen.  Nothing here stops the boot
// except the interrupt system; failures are reported by Run().
bool CKernel::Initialize (void)
{
	if (!CStdlibApp::Initialize ())		// interrupts
	{
		return false;
	}

	mSerial.Initialize (115200);
	mLogger.Initialize (&m_LogTee);		// UART + boot log, never the screen
	mLogger.RegisterPanicHandler (PanicHandler);	// crash -> try to save the log
	mTimer.Initialize ();

	// Board tag for the file names: drop "Raspberry Pi ", lower case,
	// spaces to '-', anything else odd dropped ("Zero 2 W" -> "zero-2-w")
	char Tag[32];
	const char *pName = CMachineInfo::Get ()->GetMachineName ();
	static const char Prefix[] = "Raspberry Pi ";
	if (strncmp (pName, Prefix, sizeof Prefix - 1) == 0)
	{
		pName += sizeof Prefix - 1;
	}
	unsigned n = 0;
	for (; *pName != '\0' && n < sizeof Tag - 1; pName++)
	{
		char c = *pName;
		if (c >= 'A' && c <= 'Z')
		{
			c += 'a' - 'A';
		}
		if (c == ' ' || c == '-')
		{
			c = '-';
		}
		else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+'))
		{
			continue;
		}
		Tag[n++] = c;
	}
	Tag[n] = '\0';
	if (n == 0)
	{
		strcpy (Tag, "unknown");
	}
	snprintf (m_SpeedFile, sizeof m_SpeedFile, "SD:/speed-%s.txt", Tag);
	snprintf (m_BootLogFile, sizeof m_BootLogFile, "SD:/bootlog-%s.txt", Tag);
	snprintf (m_StageFile, sizeof m_StageFile, "SD:/stage-%s.txt", Tag);

	// The core's printf/fprintf go to the UART, never onto the Mac screen
	if (m_SerialConsole.Initialize ())
	{
		CGlueStdioInit (m_SerialConsole);
	}

	m_bSDOK =    m_EMMC.Initialize ()
		  && f_mount (&m_FileSystem, "SD:", 1) == FR_OK;

	// Keep the previous boot's log (a hung boot is followed by a power
	// cycle, and that log is the one that says where it stopped)
	if (m_bSDOK)
	{
		char Prev[72];
		snprintf (Prev, sizeof Prev, "%.*s-prev.txt",
			  (int) (strlen (m_BootLogFile) - 4), m_BootLogFile);
		f_unlink (Prev);
		f_rename (m_BootLogFile, Prev + 3);	// FatFs: new name without "SD:"
	}

	m_bScreenOK = mScreen.Initialize ();

	return true;
}

static int FormatLine (char *pBuf, size_t nSize, const char *pFormat, va_list Args)
{
	int n = vsnprintf (pBuf, nSize - 2, pFormat, Args);
	if (n < 0)
	{
		return 0;
	}
	if (n > (int) nSize - 3)
	{
		n = nSize - 3;
	}
	pBuf[n++] = '\n';
	pBuf[n] = '\0';
	return n;
}

// Boot log copy gets an uptime stamp; screen and serial get the plain line
static void LogLine (CDevice *pSerial, const char *pLine, int n)
{
	char Stamp[24];
	u64 nMs = CTimer::GetClockTicks64 () / 1000;
	int s = snprintf (Stamp, sizeof Stamp, "[%5u.%03u] ",
			  (unsigned) (nMs / 1000), (unsigned) (nMs % 1000));
	BootLogAppend (Stamp, s);
	BootLogAppend (pLine, n);
	pSerial->Write (pLine, n);
}

void CKernel::Say (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = FormatLine (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	if (m_bScreenOK)
	{
		mScreen.Write (Buf, n);
	}
	LogLine (&mSerial, Buf, n);
}

void CKernel::Log (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = FormatLine (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	LogLine (&mSerial, Buf, n);
}

// Circle calls this after logging a panic (e.g. a CPU exception), just
// before it halts.  The SD driver may not work from here, but it's worth
// one try: the panic text is the most useful line in the log.
void CKernel::PanicHandler (void)
{
	static bool bOnce;
	if (!bOnce && s_pThis != 0)
	{
		bOnce = true;
		s_pThis->FlushBootLog ();
	}
}

void CKernel::WriteStage (const char *pText)
{
	if (!m_bSDOK)
	{
		return;
	}
	FIL File;
	if (f_open (&File, m_StageFile, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT nWritten;
		f_write (&File, pText, strlen (pText), &nWritten);
		f_close (&File);
	}
}

bool CKernel::ReadStage (char *pBuffer, unsigned nSize)
{
	if (!m_bSDOK)
	{
		return false;
	}
	FIL File;
	if (f_open (&File, m_StageFile, FA_READ) != FR_OK)
	{
		return false;
	}
	UINT nRead = 0;
	f_read (&File, pBuffer, nSize - 1, &nRead);
	f_close (&File);
	pBuffer[nRead] = '\0';
	return nRead > 0;
}

void CKernel::FlushBootLog (void)
{
	if (!m_bSDOK)
	{
		return;
	}
	FIL File;
	if (f_open (&File, m_BootLogFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}
	UINT nWritten;
	f_write (&File, s_BootLog, s_nBootLog, &nWritten);
	f_close (&File);
}

void CKernel::LogBoard (void)
{
	CMachineInfo *pInfo = CMachineInfo::Get ();
	Log ("Board:  %s (revision code %08X), %s, %u MB RAM",
	     pInfo->GetMachineName (), (unsigned) pInfo->GetRevisionRaw (),
	     pInfo->GetSoCName (), pInfo->GetRAMSize ());
	Log ("Kernel: %u-bit, Circle RASPPI=%u, built " __DATE__ " " __TIME__,
	     (unsigned) (sizeof (void *) * 8), (unsigned) RASPPI);
	Log ("Options: width=%u height=%u (0 = screen's preferred mode)",
	     mOptions.GetWidth (), mOptions.GetHeight ());
	Log ("SD card: %s", m_bSDOK ? "OK" : "FAILED");
	if (m_bScreenOK)
	{
		CBcmFrameBuffer *pFB = mScreen.GetFrameBuffer ();
		Log ("Screen: OK, %ux%u, %u bpp, pitch %u",
		     pFB->GetWidth (), pFB->GetHeight (), pFB->GetDepth (), pFB->GetPitch ());
	}
	else
	{
		Log ("Screen: FAILED to start (no HDMI picture); the Mac runs anyway");
	}
}

void CKernel::Fatal (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	Say ("");
	Say ("*** %s", Buf);
	Say ("");
	Say ("Fix the SD card on a PC and power the Pi off and on again.");
	FlushBootLog ();

	while (1)
	{
		mActLED.On ();
		CTimer::SimpleMsDelay (200);
		mActLED.Off ();
		CTimer::SimpleMsDelay (800);
	}
}

bool CKernel::LoadROM (void)
{
	static u8 ROM[EMU_ROM_SIZE] __attribute__((aligned(64)));

	for (const char *pName : ROMNames)
	{
		FIL File;
		if (f_open (&File, pName, FA_READ) != FR_OK)
		{
			continue;
		}

		FSIZE_t nSize = f_size (&File);
		if (nSize < EMU_ROM_SIZE)
		{
			f_close (&File);
			Fatal ("%s is only %u bytes; a Mac Plus ROM is 131072 bytes.",
			       pName + 4, (unsigned) nSize);
		}

		UINT nRead;
		FRESULT Result = f_read (&File, ROM, EMU_ROM_SIZE, &nRead);
		f_close (&File);
		if (Result != FR_OK || nRead != EMU_ROM_SIZE)
		{
			Fatal ("Cannot read %s (FatFs error %d).", pName + 4, (int) Result);
		}

		u32 nChecksum =   (u32) ROM[0] << 24 | (u32) ROM[1] << 16
				| (u32) ROM[2] << 8  | (u32) ROM[3];
		if (nChecksum != MAC_PLUS_V3_CHECKSUM)
		{
			Fatal ("%s has checksum %08X. umac needs the Mac Plus v3 ROM (4D1F8172).",
			       pName + 4, nChecksum);
		}

		Say ("ROM:  %s, Mac Plus v3 (4D1F8172)%s", pName + 4,
		     nSize > EMU_ROM_SIZE ? ", extra bytes after 128 KB ignored" : "");
		m_pROM = ROM;
		return true;
	}

	Fatal ("No Mac ROM found. Put a Mac Plus v3 ROM in the root of the SD card,\n"
	       "    named mac.rom (vMac.ROM also works).");
	return false;
}

bool CKernel::OpenDisk (void)
{
	for (const char *pName : DiskNames)
	{
		bool bReadOnly = false;
		FRESULT Result = f_open (&m_DiskFile, pName, FA_READ | FA_WRITE);
		if (Result == FR_DENIED || Result == FR_WRITE_PROTECTED)
		{
			bReadOnly = true;
			Result = f_open (&m_DiskFile, pName, FA_READ);
		}
		if (Result != FR_OK)
		{
			continue;
		}

		FSIZE_t nSize = f_size (&m_DiskFile);
		if (nSize == 0 || (nSize & 511) != 0)
		{
			f_close (&m_DiskFile);
			Fatal ("%s is %u bytes: not a raw disk image (size must be a multiple of 512).\n"
			       "    A Disk Copy 4.2 image has an 84-byte header: strip it with dd.",
			       pName + 4, (unsigned) nSize);
		}

		m_bDiskOpen = true;
		m_nDiskSize = (u32) nSize;
		m_pDiskName = pName + 4;
		Say ("Disk: %s, %u MB%s", m_pDiskName, (unsigned) (nSize >> 20),
		     bReadOnly ? ", READ-ONLY" : "");
		return bReadOnly;
	}

	Say ("Disk: none found (looked for disk1.dsk, disk1.img, disk.dsk).");
	Say ("      The Mac will show the flashing question-mark disk.");
	CTimer::SimpleMsDelay (4000);
	return false;
}

// Once per boot, 60 s in: a small text file so a board's speed can be read
// on a PC without a serial cable.  One write, overwritten next boot.
void CKernel::WriteSpeedFile (unsigned nPercent)
{
	char Text[512];
	int n = snprintf (Text, sizeof Text,
		"ClassicPi-Metal %s\r\n"
		"Board:     %s\r\n"
		"Kernel:    %u-bit, Circle RASPPI=%u\r\n"
		"Screen:    %ux%u, %u bpp, Mac at %ux\r\n"
		"Mac RAM:   %u KB\r\n"
		"Mac speed: %u.%02ux real time (umac's own 8 MHz clock, approximate;\r\n"
		"           1.00 = a real Mac Plus; averaged over 10 s, 60 s after start)\r\n"
		"\r\n"
		"Written once per boot, 60 s after the Mac starts.\r\n",
		VERSION, CMachineInfo::Get ()->GetMachineName (),
		(unsigned) (sizeof (void *) * 8), (unsigned) RASPPI,
		m_nFBWidth, m_nFBHeight, m_nFBDepth, m_nScale,
		emu_ram_kb (), nPercent / 100, nPercent % 100);
	if (n <= 0)
	{
		return;
	}
	if (n >= (int) sizeof Text)
	{
		n = sizeof Text - 1;
	}

	FIL File;
	if (f_open (&File, m_SpeedFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		Log ("cannot write %s", m_SpeedFile);
		return;
	}
	UINT nWritten;
	f_write (&File, Text, n, &nWritten);
	f_close (&File);
	Log ("wrote %s", m_SpeedFile);
}

void CKernel::SetupDisplay (void)
{
	if (!m_bScreenOK)
	{
		m_pFB = 0;
		m_nFBWidth = m_nFBHeight = m_nFBPitch = m_nFBDepth = 0;
		m_nScale = m_nOffX = m_nOffY = 0;
		return;
	}

	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	m_pFB       = (u8 *) (uintptr) pFrameBuffer->GetBuffer ();
	m_nFBWidth  = pFrameBuffer->GetWidth ();
	m_nFBHeight = pFrameBuffer->GetHeight ();
	m_nFBPitch  = pFrameBuffer->GetPitch ();
	m_nFBDepth  = pFrameBuffer->GetDepth ();

	unsigned nScaleX = m_nFBWidth / EMU_SCREEN_WIDTH;
	unsigned nScaleY = m_nFBHeight / EMU_SCREEN_HEIGHT;
	m_nScale = nScaleX < nScaleY ? nScaleX : nScaleY;
	if (m_nScale == 0)
	{
		m_nScale = 1;		// screen smaller than 512x342: top-left only
	}

	// cmdline.txt "macscale=N" forces a smaller whole-number scale
	unsigned nWanted = mOptions.GetAppOptionDecimal ("macscale", 0);
	if (nWanted >= 1 && nWanted != (unsigned) -1)
	{
		if (nWanted < m_nScale)
		{
			m_nScale = nWanted;
		}
		Log ("Screen: macscale=%u in cmdline.txt -> %ux", nWanted, m_nScale);
	}

	// Safety net: SD:/stage-<board>.txt says how the last boot went.
	//   "drawing N"           it never finished drawing at N: use N-1
	//   "ok N fallback"       a fallback to N worked: stay at N
	// Delete the file to try the largest scale again.
	char Stage[32];
	unsigned nLast;
	if (ReadStage (Stage, sizeof Stage) && sscanf (Stage, "%*s %u", &nLast) == 1)
	{
		bool bDrawing = strncmp (Stage, "drawing", 7) == 0;
		bool bFallback = strstr (Stage, "fallback") != 0;
		unsigned nUse = bDrawing ? (nLast > 1 ? nLast - 1 : 1)
			      : (bFallback ? nLast : m_nScale);
		if (nUse < m_nScale)
		{
			Log ("Screen: last boot %s at %ux -> using %ux (delete %s to retry)",
			     bDrawing ? "never finished drawing" : "fell back", nLast, nUse,
			     m_StageFile + 4);
			m_nScale = nUse;
			m_bScaleFallback = true;
		}
	}
	unsigned nW = EMU_SCREEN_WIDTH * m_nScale;
	unsigned nH = EMU_SCREEN_HEIGHT * m_nScale;
	m_nOffX = nW < m_nFBWidth  ? (m_nFBWidth - nW) / 2 : 0;
	m_nOffY = nH < m_nFBHeight ? (m_nFBHeight - nH) / 2 : 0;

	Say ("Screen: %ux%u, %u bpp, Mac at %ux scale", m_nFBWidth, m_nFBHeight,
	     m_nFBDepth, m_nScale);
}

// Draw changed rows of the Mac's 1-bit screen, scaled, into the HDMI framebuffer.
void CKernel::Blit (bool bForce)
{
	if (m_pFB == 0)
	{
		return;		// no screen: the Mac runs blind (see bootlog.txt)
	}
	const u8 *pMac = emu_framebuffer ();
	const unsigned nBytesPP = m_nFBDepth / 8;
	unsigned nRows = EMU_SCREEN_HEIGHT;
	unsigned nCols = EMU_SCREEN_WIDTH;
	if (m_nOffY + nRows * m_nScale > m_nFBHeight)
	{
		nRows = (m_nFBHeight - m_nOffY) / m_nScale;
	}
	if (m_nOffX + nCols * m_nScale > m_nFBWidth)
	{
		nCols = (m_nFBWidth - m_nOffX) / m_nScale;
	}

	for (unsigned y = 0; y < nRows; y++)
	{
		if (m_bTraceBlit && y % 57 == 0)
		{
			Log ("Mac:  first frame, row %u of %u", y, nRows);
			FlushBootLog ();
		}
		const u8 *pSrc = pMac + y * EMU_SCREEN_STRIDE;
		u8 *pShadow = s_Shadow + y * EMU_SCREEN_STRIDE;
		if (!bForce && memcmp (pSrc, pShadow, EMU_SCREEN_STRIDE) == 0)
		{
			continue;
		}
		memcpy (pShadow, pSrc, EMU_SCREEN_STRIDE);

		u8 *pLine = m_pFB + (m_nOffY + y * m_nScale) * m_nFBPitch + m_nOffX * nBytesPP;

		if (nBytesPP == 2)
		{
			u16 *pOut = (u16 *) pLine;
			for (unsigned x = 0; x < nCols; x++)
			{
				u16 Colour = (pSrc[x >> 3] & (0x80 >> (x & 7))) ? 0x0000 : 0xFFFF;
				for (unsigned s = 0; s < m_nScale; s++)
				{
					*pOut++ = Colour;
				}
			}
		}
		else if (nBytesPP == 4)
		{
			u32 *pOut = (u32 *) pLine;
			for (unsigned x = 0; x < nCols; x++)
			{
				u32 Colour = (pSrc[x >> 3] & (0x80 >> (x & 7))) ? 0xFF000000 : 0xFFFFFFFF;
				for (unsigned s = 0; s < m_nScale; s++)
				{
					*pOut++ = Colour;
				}
			}
		}
		else
		{
			continue;	// 8 bpp not supported (Circle default is 16)
		}

		// Repeat the line for vertical scaling
		for (unsigned s = 1; s < m_nScale; s++)
		{
			memcpy (pLine + s * m_nFBPitch, pLine, nCols * m_nScale * nBytesPP);
		}
	}
}

// Several keyboards and mice can be in use at once (a keyboard often shows
// up as more than one device, e.g. ukbd1 + ukbd2, and so do wireless
// keyboard/mouse receivers).  Each has a slot with its own key state.
static unsigned char s_ucOldModifiers[MAX_INPUTS];
static unsigned char s_OldKeys[MAX_INPUTS][6];
static volatile unsigned s_nMouseButtonsOf[MAX_INPUTS];

static bool Holds (CDevice * volatile const *pSlots, CDevice *pDevice)
{
	for (unsigned i = 0; i < MAX_INPUTS; i++)
	{
		if (pSlots[i] == pDevice)
		{
			return true;
		}
	}
	return false;
}

void CKernel::UpdateUSB (void)
{
	if (!m_USBHCI.UpdatePlugAndPlay ())
	{
		// A removal may not count as an update, but its handler has run
		if (m_bUSBRemoved)
		{
			m_bUSBRemoved = false;
			Log ("USB:  now %u keyboard(s), %u mouse/mice",
			     CountInputs ((CDevice * volatile *) m_pKeyboards),
			     CountInputs ((CDevice * volatile *) m_pMice));
			if (m_nUSBLogFlushes < USB_LOG_FLUSHES)
			{
				m_nUSBLogFlushes++;
				FlushBootLog ();
			}
		}
		return;
	}
	m_bUSBRemoved = false;

	for (unsigned n = 1; n <= MAX_INPUTS; n++)
	{
		char Name[16];

		snprintf (Name, sizeof Name, "ukbd%u", n);
		CUSBKeyboardDevice *pKeyboard =
			(CUSBKeyboardDevice *) mDeviceNameService.GetDevice (Name, FALSE);
		if (pKeyboard != 0 && !Holds ((CDevice * volatile *) m_pKeyboards, pKeyboard))
		{
			for (unsigned i = 0; i < MAX_INPUTS; i++)
			{
				if (m_pKeyboards[i] == 0)
				{
					s_ucOldModifiers[i] = 0;
					memset (s_OldKeys[i], 0, 6);
					m_pKeyboards[i] = pKeyboard;
					pKeyboard->RegisterRemovedHandler (KeyRemovedHandler, (void *) (uintptr) i);
					pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler, FALSE,
										(void *) (uintptr) i);
					Log ("USB:  keyboard %s attached", Name);
					break;
				}
			}
		}

		snprintf (Name, sizeof Name, "mouse%u", n);
		CMouseDevice *pMouse = (CMouseDevice *) mDeviceNameService.GetDevice (Name, FALSE);
		if (pMouse != 0 && !Holds ((CDevice * volatile *) m_pMice, pMouse))
		{
			for (unsigned i = 0; i < MAX_INPUTS; i++)
			{
				if (m_pMice[i] == 0)
				{
					s_nMouseButtonsOf[i] = 0;
					m_pMice[i] = pMouse;
					pMouse->RegisterRemovedHandler (MouseRemovedHandler, (void *) (uintptr) i);
					pMouse->RegisterStatusHandler (MouseStatusHandler, (void *) (uintptr) i);
					Log ("USB:  mouse %s attached", Name);
					break;
				}
			}
		}
	}

	Log ("USB:  devices changed; now %u keyboard(s), %u mouse/mice",
	     CountInputs ((CDevice * volatile *) m_pKeyboards),
	     CountInputs ((CDevice * volatile *) m_pMice));

	// Hot-plug events go to the card even after the first minute (capped)
	if (m_nUSBLogFlushes < USB_LOG_FLUSHES)
	{
		m_nUSBLogFlushes++;
		FlushBootLog ();
	}
}

unsigned CKernel::CountInputs (CDevice * volatile const *pSlots)
{
	unsigned nCount = 0;
	for (unsigned i = 0; i < MAX_INPUTS; i++)
	{
		nCount += pSlots[i] != 0;
	}
	return nCount;
}

// Interrupt context: turn a USB report into key down/up events.
void CKernel::KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6], void *pArg)
{
	unsigned nSlot = (unsigned) (uintptr) pArg;
	unsigned char &ucOldModifiers = s_ucOldModifiers[nSlot];
	unsigned char *OldKeys = s_OldKeys[nSlot];

	unsigned char ucChanged = ucModifiers ^ ucOldModifiers;
	for (unsigned i = 0; i < 8; i++)
	{
		if (ucChanged & (1 << i))
		{
			QueueKey (0xE0 + i, (ucModifiers >> i) & 1);
		}
	}
	ucOldModifiers = ucModifiers;

	// 0x01 = keyboard rollover error: keep the previous state
	for (unsigned i = 0; i < 6; i++)
	{
		if (RawKeys[i] == 0x01)
		{
			return;
		}
	}

	for (unsigned i = 0; i < 6; i++)		// released
	{
		u8 Key = OldKeys[i];
		if (Key == 0)
		{
			continue;
		}
		bool bStill = false;
		for (unsigned j = 0; j < 6; j++)
		{
			bStill |= RawKeys[j] == Key;
		}
		if (!bStill)
		{
			QueueKey (Key, false);
		}
	}
	for (unsigned i = 0; i < 6; i++)		// pressed
	{
		u8 Key = RawKeys[i];
		if (Key == 0)
		{
			continue;
		}
		bool bWas = false;
		for (unsigned j = 0; j < 6; j++)
		{
			bWas |= OldKeys[j] == Key;
		}
		if (!bWas)
		{
			QueueKey (Key, true);
		}
	}
	memcpy (OldKeys, RawKeys, 6);
}

// Called inside UpdatePlugAndPlay() (task level) when a keyboard is unplugged:
// release whatever it was holding down, so no key or modifier sticks.
void CKernel::KeyRemovedHandler (CDevice *pDevice, void *pContext)
{
	unsigned nSlot = (unsigned) (uintptr) pContext;
	for (unsigned i = 0; i < 8; i++)
	{
		if (s_ucOldModifiers[nSlot] & (1 << i))
		{
			QueueKey (0xE0 + i, false);
		}
	}
	for (unsigned i = 0; i < 6; i++)
	{
		if (s_OldKeys[nSlot][i] > 0x01)
		{
			QueueKey (s_OldKeys[nSlot][i], false);
		}
	}
	s_ucOldModifiers[nSlot] = 0;
	memset (s_OldKeys[nSlot], 0, 6);
	s_pThis->m_pKeyboards[nSlot] = 0;
	s_pThis->Log ("USB:  a keyboard was unplugged");
	s_pThis->m_bUSBRemoved = true;
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDX, int nDY, int nWheel, void *pArg)
{
	s_nMouseDX += nDX;
	s_nMouseDY += nDY;
	s_nMouseButtonsOf[(unsigned) (uintptr) pArg] = nButtons;
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	unsigned nSlot = (unsigned) (uintptr) pContext;
	s_nMouseButtonsOf[nSlot] = 0;
	s_pThis->m_pMice[nSlot] = 0;
	s_pThis->Log ("USB:  a mouse was unplugged");
	s_pThis->m_bUSBRemoved = true;
}

void CKernel::DrainInput (void)
{
	EnterCritical ();
	int nDX = s_nMouseDX;
	int nDY = s_nMouseDY;
	unsigned nButtons = 0;
	for (unsigned i = 0; i < MAX_INPUTS; i++)
	{
		nButtons |= s_nMouseButtonsOf[i];
	}
	s_nMouseDX = 0;
	s_nMouseDY = 0;
	LeaveCritical ();

	// Any button is the Mac's one button
	emu_mouse (nDX, nDY, nButtons != 0);

	while (s_nKeyOut != s_nKeyIn)
	{
		TKeyEvent Event = s_KeyQueue[s_nKeyOut];
		s_nKeyOut = (s_nKeyOut + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);
		emu_usb_key (Event.Usage, Event.Down);
	}
}

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	Say ("ClassicPi-Metal " VERSION " - Mac Plus (umac), %u KB RAM", emu_ram_kb ());
	Say ("");
	LogBoard ();
	FlushBootLog ();

	// SD card (started in Initialize so the log could be written)
	if (!m_bSDOK)
	{
		Fatal ("Cannot start the SD card, or mount its FAT partition.");
	}

	LoadROM ();
	bool bReadOnly = OpenDisk ();
	FlushBootLog ();

	// USB keyboard + mouse
	Say ("USB:  starting...");
	FlushBootLog ();			// in case USB start-up hangs
	if (!m_USBHCI.Initialize ())
	{
		Say ("USB:  failed to start; no keyboard or mouse.");
	}
	UpdateUSB ();
	Say ("USB:  %u keyboard(s), %u mouse/mice found (more can be plugged in later)",
	     CountInputs ((CDevice * volatile *) m_pKeyboards),
	     CountInputs ((CDevice * volatile *) m_pMice));
	FlushBootLog ();

	SetupDisplay ();

	int nResult = emu_init (m_pROM, m_bDiskOpen ? &m_DiskFile : 0, m_nDiskSize, bReadOnly);
	if (nResult == -1)
	{
		Fatal ("The ROM could not be patched; it is not a supported Mac Plus ROM.");
	}
	else if (nResult != 0)
	{
		Fatal ("Not enough memory for the Mac's %u KB of RAM.", emu_ram_kb ());
	}

	Log ("Mac:  started");
	FlushBootLog ();
	CTimer::SimpleMsDelay (1500);	// let people read the banner

	// From here the screen belongs to the Mac
	if (m_bScreenOK)
	{
		// Record the scale we're about to draw at; if this boot hangs, the
		// next one sees "drawing N" and falls back (see SetupDisplay)
		char Stage[32];
		snprintf (Stage, sizeof Stage, "drawing %u%s\r\n", m_nScale,
			  m_bScaleFallback ? " fallback" : "");
		WriteStage (Stage);

		Log ("Mac:  clearing the text screen");
		FlushBootLog ();
		static const char ClearScreen[] = "\x1b[H\x1b[J\x1b[?25l";
		mScreen.Write (ClearScreen, sizeof ClearScreen - 1);
		Log ("Mac:  drawing the first frame");
		FlushBootLog ();

		u64 nStart = CTimer::GetClockTicks64 ();
		m_bTraceBlit = true;		// progress marks while drawing it
		Blit (true);
		m_bTraceBlit = false;
		Log ("Mac:  first frame drawn in %u ms",
		     (unsigned) ((CTimer::GetClockTicks64 () - nStart) / 1000));
		FlushBootLog ();
	}
	bool bLoggedFirstSecond = false;

	u64 nNow = CTimer::GetClockTicks64 ();
	u64 nNextVsync = nNow + VSYNC_US;
	u64 nNext1Hz = nNow + 1000000;
	u64 nNextUSB = nNow + 100000;

	// Speed: umac runs the 68000 as fast as the Pi allows, so count slices
	u64 nStatsStart = nNow;
	unsigned nSlices = 0;
	unsigned nReports = 0;

	while (1)
	{
		DrainInput ();
		emu_run_slice ();
		nSlices++;

		nNow = CTimer::GetClockTicks64 ();

		if (nNow - nStatsStart >= STATS_US)
		{
			u64 nMacUs = (u64) nSlices * emu_slice_us ();
			unsigned nPercent = (unsigned) (nMacUs * 100 / (nNow - nStatsStart));
			Log ("Mac speed %u.%02ux real time", nPercent / 100, nPercent % 100);
			if (++nReports == SPEED_FILE_AT)
			{
				WriteSpeedFile (nPercent);
			}
			if (nReports <= SPEED_FILE_AT)
			{
				FlushBootLog ();	// first minute only, then the log stops
			}
			nSlices = 0;
			nStatsStart = CTimer::GetClockTicks64 ();
		}

		if (nNow >= nNextVsync)
		{
			emu_vsync ();
			Blit (false);
			nNextVsync += VSYNC_US;
			if (nNow > nNextVsync + 4 * VSYNC_US)
			{
				nNextVsync = nNow + VSYNC_US;	// fell behind: don't burst
			}
		}

		if (nNow >= nNext1Hz)
		{
			emu_1hz ();
			nNext1Hz += 1000000;
			if (!bLoggedFirstSecond)
			{
				bLoggedFirstSecond = true;
				Log ("Mac:  running (first second: %u slices = %u ms of Mac time)",
				     nSlices, nSlices * emu_slice_us () / 1000);
				FlushBootLog ();
				if (m_bScreenOK)
				{
					char Stage[32];
					snprintf (Stage, sizeof Stage, "ok %u%s\r\n", m_nScale,
						  m_bScaleFallback ? " fallback" : "");
					WriteStage (Stage);
				}
			}
		}

		if (nNow >= nNextUSB)
		{
			UpdateUSB ();
			nNextUSB = nNow + 100000;
		}

		if (s_bDiskDirty && nNow - s_nLastWrite > SYNC_AFTER_US)
		{
			s_bDiskDirty = false;
			f_sync (&m_DiskFile);
		}
	}

	return ShutdownHalt;
}
