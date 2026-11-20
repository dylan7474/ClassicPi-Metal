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
#define SPEED_FILE_AT	6		// write SD:/speed.txt after the 6th (60 s)
#define SPEED_FILE	"SD:/speed.txt"

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

CKernel::CKernel (void)
:	CStdlibAppScreen ("ClassicPi-Metal"),
	m_USBHCI (&mInterrupt, &mTimer, TRUE),
	m_EMMC (&mInterrupt, &mTimer, &mActLED),
	m_SerialConsole (&mSerial, &mSerial),
	m_pKeyboard (0),
	m_pMouse (0),
	m_pROM (0),
	m_bDiskOpen (false),
	m_nDiskSize (0),
	m_pDiskName (0),
	m_pFB (0)
{
	s_pThis = this;
}

bool CKernel::Initialize (void)
{
	if (!CStdlibAppScreen::Initialize ())
	{
		return false;
	}

	// The core's printf/fprintf go to the UART, never onto the Mac screen
	if (!m_SerialConsole.Initialize ())
	{
		return false;
	}
	CGlueStdioInit (m_SerialConsole);

	return true;
}

void CKernel::Say (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = vsnprintf (Buf, sizeof Buf - 2, pFormat, Args);
	va_end (Args);
	if (n < 0)
	{
		return;
	}
	if (n > (int) sizeof Buf - 3)
	{
		n = sizeof Buf - 3;
	}
	Buf[n++] = '\n';
	Buf[n] = '\0';

	mScreen.Write (Buf, n);
	mSerial.Write (Buf, n);
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
	if (f_open (&File, SPEED_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		printf ("%s: cannot write %s\n", FROM, SPEED_FILE);
		return;
	}
	UINT nWritten;
	f_write (&File, Text, n, &nWritten);
	f_close (&File);
	printf ("%s: wrote %s\n", FROM, SPEED_FILE);
}

void CKernel::SetupDisplay (void)
{
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

void CKernel::UpdateUSB (void)
{
	if (!m_USBHCI.UpdatePlugAndPlay ())
	{
		return;
	}

	if (m_pKeyboard == 0)
	{
		CUSBKeyboardDevice *pKeyboard =
			(CUSBKeyboardDevice *) mDeviceNameService.GetDevice ("ukbd1", FALSE);
		if (pKeyboard != 0)
		{
			pKeyboard->RegisterRemovedHandler (KeyRemovedHandler, this);
			pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler);
			m_pKeyboard = pKeyboard;
			mLogger.Write (FROM, LogNotice, "Keyboard attached");
		}
	}

	if (m_pMouse == 0)
	{
		CMouseDevice *pMouse = (CMouseDevice *) mDeviceNameService.GetDevice ("mouse1", FALSE);
		if (pMouse != 0)
		{
			pMouse->RegisterRemovedHandler (MouseRemovedHandler, this);
			pMouse->RegisterStatusHandler (MouseStatusHandler);
			m_pMouse = pMouse;
			mLogger.Write (FROM, LogNotice, "Mouse attached");
		}
	}
}

// Interrupt context: turn a USB report into key down/up events.
void CKernel::KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	static unsigned char ucOldModifiers;
	static unsigned char OldKeys[6];

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

void CKernel::KeyRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	pThis->m_pKeyboard = 0;
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDX, int nDY, int nWheel)
{
	s_nMouseDX += nDX;
	s_nMouseDY += nDY;
	s_nMouseButtons = nButtons;
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	pThis->m_pMouse = 0;
	s_nMouseButtons = 0;
}

void CKernel::DrainInput (void)
{
	EnterCritical ();
	int nDX = s_nMouseDX;
	int nDY = s_nMouseDY;
	unsigned nButtons = s_nMouseButtons;
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

	// SD card
	if (!m_EMMC.Initialize ())
	{
		Fatal ("Cannot start the SD card.");
	}
	if (f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
	{
		Fatal ("Cannot mount the SD card's FAT partition.");
	}

	LoadROM ();
	bool bReadOnly = OpenDisk ();

	// USB keyboard + mouse
	Say ("USB:  starting...");
	if (!m_USBHCI.Initialize ())
	{
		Say ("USB:  failed to start; no keyboard or mouse.");
	}
	UpdateUSB ();
	Say ("USB:  keyboard %s, mouse %s", m_pKeyboard ? "found" : "not found yet",
	     m_pMouse ? "found" : "not found yet");

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

	CTimer::SimpleMsDelay (1500);	// let people read the banner

	// From here the screen belongs to the Mac
	static const char ClearScreen[] = "\x1b[H\x1b[J\x1b[?25l";
	mScreen.Write (ClearScreen, sizeof ClearScreen - 1);
	Blit (true);

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
			printf ("%s: Mac speed %u.%02ux real time\n", FROM, nPercent / 100, nPercent % 100);
			if (++nReports == SPEED_FILE_AT)
			{
				WriteSpeedFile (nPercent);
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
