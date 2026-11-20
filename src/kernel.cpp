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
#include <circle/net/dnsclient.h>
#include <circle/net/ntpclient.h>
#include <circle/net/mdnspublisher.h>
#include <circle/startup.h>
#include <circle/netdevice.h>
#include <circle/sound/soundbasedevice.h>
#if RASPPI <= 4
#include <circle/sound/hdmisoundbasedevice.h>
#endif
#include "webserver.h"
#include <circle/net/ipaddress.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle_glue.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
	m_pWLAN (0),
	m_pNet (0),
	m_pWPASupplicant (0),
	m_bNetActive (false),
	m_bTimeDone (false),
	m_bWeb (false),
	m_nNetStart (0),
	m_nNetYieldUs (10000),
	m_bClockDirty (false),
	m_nClockChanged (0),
	m_pSound (0),
	m_bSDOK (false),
	m_bScreenOK (false),
	m_bScaleFallback (false),
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

		// First the screen (plain memory writes, works from an exception),
		// then one try at the SD card
		if (s_pThis->m_bScreenOK)
		{
			static const char Head[] = "\x1b[H\x1b[J\x1b[?25h"
				"*** ClassicPi-Metal crashed. Last lines of the boot log:\n\n";
			s_pThis->mScreen.Write (Head, sizeof Head - 1);
			unsigned nLen = s_nBootLog;
			unsigned nFrom = nLen > 1500 ? nLen - 1500 : 0;
			while (nFrom < nLen && nFrom > 0 && s_BootLog[nFrom - 1] != '\n')
			{
				nFrom++;	// start at a line boundary
			}
			s_pThis->mScreen.Write (s_BootLog + nFrom, nLen - nFrom);
		}
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

// ---- Clock + PRAM -----------------------------------------------------------
// SD:/pram.dat keeps the Mac's 20 bytes of PRAM (its control-panel settings)
// and the clock: "CPMR", seconds (big-endian, Mac time), PRAM.  The Pi has
// no clock of its own, so without Wi-Fi the Mac starts at the time last
// saved (or the build time); with Wi-Fi, NTP corrects it a few seconds in.

#define CLOCK_FILE	"SD:/pram.dat"
#define CLOCK_SAVE_US	2000000		// save 2 s after the Mac stops writing
#define WIFI_CONFIG	"SD:/wpa_supplicant.conf"
#define WIFI_FIRMWARE	"SD:/firmware/"
#define HOSTNAME	"classicpi"
#define NET_GIVE_UP_US	120000000	// stop the network after 2 min without time
#define NET_YIELD_US	10000		// network turns every 10 ms while joining + NTP
#define NET_HEALTH_US	30000000	// network health line in the log every 30 s ...
#define NET_HEALTH_FOR	(10 * 60 * 1000000ULL)	// ... for the first 10 minutes

static volatile bool s_bClockChanged;

extern "C" void hal_rtc_changed (void)
{
	s_bClockChanged = true;		// picked up by the main loop
}

// Days from 1 Jan 1970 for a civil date (Howard Hinnant's algorithm)
static long DaysFromCivil (int y, unsigned m, unsigned d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned) (y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long) doe - 719468;
}

static u32 MacSecondsFromLocal (int y, unsigned m, unsigned d, unsigned h, unsigned mi, unsigned s)
{
	long nDays = DaysFromCivil (y, m, d) - DaysFromCivil (1904, 1, 1);
	return (u32) (nDays * 86400L + h * 3600 + mi * 60 + s);
}

// UTC (Unix seconds) -> Mac seconds, local time per TZ (cmdline tz=)
u32 CKernel::UnixToMac (unsigned nUnix)
{
	time_t t = (time_t) nUnix;
	struct tm *pTm = localtime (&t);
	return MacSecondsFromLocal (pTm->tm_year + 1900, pTm->tm_mon + 1, pTm->tm_mday,
				    pTm->tm_hour, pTm->tm_min, pTm->tm_sec);
}

// The build time (__DATE__ "Nov 20 2026", __TIME__ "18:20:00"), as a start
// for a card that has never been told the time
static u32 BuildTimeMac (void)
{
	static const char Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *pDate = __DATE__, *pTime = __TIME__;
	unsigned m = 1;
	for (unsigned i = 0; i < 12; i++)
	{
		if (strncmp (pDate, Months + 3 * i, 3) == 0)
		{
			m = i + 1;
		}
	}
	return MacSecondsFromLocal (atoi (pDate + 7), m, atoi (pDate + 4),
				    atoi (pTime), atoi (pTime + 3), atoi (pTime + 6));
}

static void MacTimeText (u32 nMac, char *pBuf, size_t nSize)
{
	u32 nDays = nMac / 86400, nRest = nMac % 86400;
	time_t t = (time_t) (nDays - 24107) * 86400;	// 1904 -> 1970 is 24107 days
	struct tm *pTm = gmtime (&t);			// date part only
	snprintf (pBuf, nSize, "%04u-%02u-%02u %02u:%02u:%02u",
		  (unsigned) (pTm->tm_year + 1900) % 10000, (unsigned) (pTm->tm_mon + 1) % 100,
		  (unsigned) pTm->tm_mday % 100, (nRest / 3600) % 100, nRest / 60 % 60, nRest % 60);
}

void CKernel::LoadClock (void)
{
	u8 Buf[4 + 4 + EMU_PRAM_SIZE];
	FIL File;
	UINT nRead = 0;
	if (   f_open (&File, CLOCK_FILE, FA_READ) == FR_OK)
	{
		f_read (&File, Buf, sizeof Buf, &nRead);
		f_close (&File);
	}
	if (nRead == sizeof Buf && memcmp (Buf, "CPMR", 4) == 0)
	{
		u32 nSecs = (u32) Buf[4] << 24 | (u32) Buf[5] << 16 | (u32) Buf[6] << 8 | Buf[7];
		emu_rtc_load (Buf + 8, nSecs);
		char Text[48];
		MacTimeText (nSecs, Text, sizeof Text);
		Log ("Clock: PRAM and clock from %s (%s)", CLOCK_FILE + 4, Text);
	}
	else
	{
		emu_rtc_load (0, BuildTimeMac ());	// the ROM sets PRAM defaults
		Log ("Clock: no %s yet; starting at the build time", CLOCK_FILE + 4);
	}
}

void CKernel::SaveClock (void)
{
	if (!m_bSDOK)
	{
		return;
	}
	u8 Buf[4 + 4 + EMU_PRAM_SIZE];
	uint32_t nSecs;
	memcpy (Buf, "CPMR", 4);
	emu_rtc_save (Buf + 8, &nSecs);
	Buf[4] = nSecs >> 24; Buf[5] = nSecs >> 16; Buf[6] = nSecs >> 8; Buf[7] = nSecs;

	FIL File;
	if (f_open (&File, CLOCK_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT nWritten;
		f_write (&File, Buf, sizeof Buf, &nWritten);
		f_close (&File);
	}
}

// ---- Wi-Fi + NTP ---------------------------------------------------------------
// The Mac starts straight away; this task joins Wi-Fi, asks NTP once and
// leaves the answer for the main loop, which sets the Mac's clock.

static volatile unsigned s_nNTPTime;		// Unix seconds, 0 = not yet
static volatile bool s_bNTPDone;		// task finished (with or without time)

class CTimeTask : public CTask
{
public:
	CTimeTask (CNetSubSystem *pNet, const char *pServer)
	:	m_pNet (pNet), m_pServer (pServer)
	{
		SetName ("ntp");
	}

	void Run (void) override
	{
		CScheduler *pSched = CScheduler::Get ();
		while (!m_pNet->IsRunning ())		// Wi-Fi joined + DHCP
		{
			pSched->MsSleep (200);
		}
		CString IP;
		m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
		CLogger::Get ()->Write ("cpmetal", LogNotice, "Network: connected, IP address %s",
					(const char *) IP);

		for (unsigned nTry = 0; nTry < 5 && s_nNTPTime == 0; nTry++)
		{
			CIPAddress Server;
			CDNSClient DNS (m_pNet);
			if (DNS.Resolve (m_pServer, &Server))
			{
				CNTPClient NTP (m_pNet);
				s_nNTPTime = NTP.GetTime (Server);
			}
			if (s_nNTPTime == 0)
			{
				pSched->MsSleep (2000);
			}
		}
		s_bNTPDone = true;
	}

private:
	CNetSubSystem *m_pNet;
	const char *m_pServer;
};

// classicpi.local and the page in the network's service list (mDNS), once
// the network is up
class CNameTask : public CTask
{
public:
	CNameTask (CNetSubSystem *pNet) : m_pNet (pNet)
	{
		SetName ("mdns");
	}

	void Run (void) override
	{
		while (!m_pNet->IsRunning ())
		{
			CScheduler::Get ()->MsSleep (200);
		}
		CmDNSPublisher *pmDNS = new CmDNSPublisher (m_pNet);
		if (pmDNS != 0)
		{
			pmDNS->PublishService ("ClassicPi-Metal", "_http._tcp", 80);
		}
	}

private:
	CNetSubSystem *m_pNet;
};

// The network: Wi-Fi with SD:/wpa_supplicant.conf (and a Wi-Fi chip whose
// firmware is in SD:/firmware/), otherwise wired Ethernet if the board has
// it (Pi 3B/3B+/4 with a cable, a USB adapter, QEMU's usb-net).
// cmdline.txt wifi=off: no network at all; web=off: no disk-images page.
void CKernel::StartWiFi (void)
{
	if (strcmp (mOptions.GetAppOptionString ("wifi", "on"), "off") == 0)
	{
		Log ("Network: off (wifi=off in cmdline.txt)");
		return;
	}

	FILINFO Info;
	bool bWiFi = f_stat (WIFI_CONFIG, &Info) == FR_OK;
	if (bWiFi)
	{
		m_pWLAN = new CBcm4343Device (WIFI_FIRMWARE);
		if (m_pWLAN == 0 || !m_pWLAN->Initialize ())
		{
			Log ("Wi-Fi: no Wi-Fi chip on this board, or its firmware is not in /firmware");
			m_pWLAN = 0;		// a half-started driver is left alone
			bWiFi = false;
		}
	}

	// Without Wi-Fi, only if there is a wired network device (USB found it)
	if (!bWiFi && CNetDevice::GetNetDevice (NetDeviceTypeEthernet) == 0)
	{
		Log ("Network: off (no wpa_supplicant.conf, and no wired Ethernet on this board)");
		return;
	}

	// Fixed address instead of DHCP: cmdline.txt ip= netmask= gateway= dns=
	static u8 IP[4], Mask[4] = { 255, 255, 255, 0 }, Gateway[4], DNS[4];
	bool bStatic = false;
	{
		auto Parse = [this] (const char *pOption, u8 *pOut) -> bool
		{
			const char *p = mOptions.GetAppOptionString (pOption, 0);
			unsigned a, b, c, d;
			if (p == 0 || sscanf (p, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
			{
				return false;
			}
			pOut[0] = a; pOut[1] = b; pOut[2] = c; pOut[3] = d;
			return true;
		};
		bStatic = Parse ("ip", IP);
		if (bStatic)
		{
			Parse ("netmask", Mask);
			if (!Parse ("gateway", Gateway))
			{
				memcpy (Gateway, IP, 3); Gateway[3] = 1;
			}
			if (!Parse ("dns", DNS))
			{
				memcpy (DNS, Gateway, 4);
			}
			Log ("Network: fixed address %u.%u.%u.%u (gateway %u.%u.%u.%u)",
			     IP[0], IP[1], IP[2], IP[3], Gateway[0], Gateway[1], Gateway[2], Gateway[3]);
		}
	}

	m_pNet = new CNetSubSystem (bStatic ? IP : 0, bStatic ? Mask : 0,
				    bStatic ? Gateway : 0, bStatic ? DNS : 0, HOSTNAME,
				    bWiFi ? NetDeviceTypeWLAN : NetDeviceTypeEthernet);
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))
	{
		Log (bWiFi ? "Wi-Fi: network start failed"
			   : "Network: no wpa_supplicant.conf, and no wired Ethernet on this board");
		m_pNet = 0;
		return;
	}

	if (bWiFi)
	{
		m_pWPASupplicant = new CWPASupplicant (WIFI_CONFIG);
		if (m_pWPASupplicant == 0 || !m_pWPASupplicant->Initialize ())
		{
			Log ("Wi-Fi: cannot start WPA (check %s)", WIFI_CONFIG + 4);
			m_pWPASupplicant = 0;
			return;
		}
	}

	new CTimeTask (m_pNet, m_NTPServer);
	m_bNetActive = true;
	m_nNetStart = CTimer::GetClockTicks64 ();
	Log ("%s: joining; the Mac's clock is set from %s when it answers",
	     bWiFi ? "Wi-Fi" : "Ethernet", m_NTPServer);

	m_nNetYieldUs = mOptions.GetAppOptionDecimal ("netyield", 10);
	if (m_nNetYieldUs == 0 || m_nNetYieldUs > 1000)
	{
		m_nNetYieldUs = 10;
	}
	m_nNetYieldUs *= 1000;

	m_bWeb = strcmp (mOptions.GetAppOptionString ("web", "on"), "off") != 0;
	if (m_bWeb)
	{
		new CWebTask (m_pNet, 80);
		new CNameTask (m_pNet);
		Log ("Web:  disk images page on port 80 once connected (web=off turns it off)");
	}
}

// Main loop: give the network a turn now and then until the time is in
void CKernel::UpdateNetwork (u64 nNow)
{
	if (s_bClockChanged)			// the Mac wrote PRAM or its clock
	{
		s_bClockChanged = false;
		m_bClockDirty = true;
		m_nClockChanged = nNow;
	}
	if (m_bClockDirty && nNow - m_nClockChanged > CLOCK_SAVE_US)
	{
		m_bClockDirty = false;
		SaveClock ();
	}

	// Requests from the web page (it runs as a task; the Mac's disk is ours)
	if (g_Web.bSyncRequest)
	{
		g_Web.bSyncRequest = false;
		if (m_bDiskOpen)
		{
			f_sync (&m_DiskFile);
			s_bDiskDirty = false;
		}
		g_Web.bSynced = true;
	}
	if (g_Web.bRestartRequest)
	{
		Log ("Web:  restart requested");
		if (m_bDiskOpen)
		{
			f_close (&m_DiskFile);
		}
		SaveClock ();
		FlushBootLog ();
		reboot ();
	}

	if (!m_bNetActive)
	{
		return;
	}

	// A turn for the network tasks every 10 ms (cmdline netyield=<ms>);
	// every pass while a download or upload runs (the Mac slows down)
	static u64 nNextYield;
	if (g_Web.nTransfers > 0 || nNow >= nNextYield)
	{
		m_Scheduler.Yield ();
		nNextYield = CTimer::GetClockTicks64 () + m_nNetYieldUs;
	}

	// Health line for the first minutes (written to the card each time)
	static u64 nNextHealth = nNow + NET_HEALTH_US;
	if (nNow >= nNextHealth && nNow - m_nNetStart < NET_HEALTH_FOR)
	{
		nNextHealth = nNow + NET_HEALTH_US;
		CString IP;
		if (m_pNet != 0 && m_pNet->IsRunning ())
		{
			m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
		}
		Log ("Net:  %s, link %s, IP %s, web requests %u",
		     m_pNet != 0 && m_pNet->IsRunning () ? "running" : "NOT running",
		     m_pWLAN != 0 ? (m_pWLAN->IsLinkUp () ? "up" : "DOWN") : "wired",
		     IP.GetLength () ? (const char *) IP : "-", g_Web.nRequests);
		FlushBootLog ();
	}

	if (!m_bTimeDone && (s_bNTPDone || nNow - m_nNetStart > NET_GIVE_UP_US))
	{
		m_bTimeDone = true;
		// Keep giving turns only for the page, and only if we're connected
		m_bNetActive = m_bWeb && m_pNet != 0 && m_pNet->IsRunning ();
		if (s_nNTPTime != 0)
		{
			u32 nMac = UnixToMac (s_nNTPTime);
			emu_set_time (nMac);
			SaveClock ();		// next boot without Wi-Fi starts closer
			char Text[48];
			MacTimeText (nMac, Text, sizeof Text);
			Log ("Clock: set from %s: %s (%s)", m_NTPServer, Text, m_TimeZone);
		}
		else
		{
			Log ("Network: no time from %s (not connected, or no answer)", m_NTPServer);
		}
		FlushBootLog ();
	}
}

// ---- Sound over HDMI -----------------------------------------------------------
// The Mac Plus sound (mac_sound.c) to the screen's speakers.  Circle's HDMI
// sound works on the Pi 1-4 and Zeros (HDMI0), not the Pi 5.  Each frame
// the main loop writes enough 48 kHz stereo to keep the queue ~50 ms deep,
// so the Pi's 60 Hz and HDMI's 48 kHz never drift apart.  cmdline.txt
// sound=off turns it off.

#define SOUND_RATE		48000
#define SOUND_QUEUE_MS		200		// room in the queue
#define SOUND_FRAME		(SOUND_RATE / 60)	// 800 per video frame, always
#define SOUND_LOW		(2 * SOUND_FRAME)	// below this: write a frame twice
#define SOUND_HIGH		(6 * SOUND_FRAME)	// above this: skip a frame

void CKernel::StartSound (void)
{
#if RASPPI <= 4
	if (strcmp (mOptions.GetAppOptionString ("sound", "hdmi"), "off") == 0)
	{
		Log ("Sound: off (sound=off in cmdline.txt)");
		return;
	}
	if (!m_bScreenOK)
	{
		Log ("Sound: off (HDMI not running)");
		return;
	}
	CHDMISoundBaseDevice *pSound = new CHDMISoundBaseDevice (&mInterrupt, SOUND_RATE, 384 * 10);
	if (pSound == 0 || !pSound->AllocateQueue (SOUND_QUEUE_MS))
	{
		Log ("Sound: cannot start HDMI sound");
		return;			// left alone: its destructor resets HDMI
	}
	pSound->SetWriteFormat (SoundFormatSigned16, 2);
	if (!pSound->Start ())
	{
		Log ("Sound: cannot start HDMI sound");
		return;			// left alone: its destructor resets HDMI
	}
	m_pSound = pSound;
	Log ("Sound: HDMI, %u Hz (sound=off in cmdline.txt turns it off)", SOUND_RATE);
#else
	Log ("Sound: none (Circle has no HDMI sound on the Pi 5)");
#endif
}

// Always exactly 800 frames per video frame: 60 x 800 = 48000, so the pitch
// never moves.  (Steering the amount per frame made a warble: the HDMI
// driver drains the queue in ~40 ms blocks.)  The Pi's 60 Hz is a hair
// off, so now and then a whole frame is repeated or skipped, only when
// the queue gets near empty or full.
void CKernel::FeedSound (void)
{
	if (m_pSound == 0)
	{
		return;
	}
	CSoundBaseDevice *pSound = (CSoundBaseDevice *) m_pSound;
	static s16 Buffer[SOUND_FRAME * 2];
	emu_sound_render (Buffer, SOUND_FRAME);

	unsigned nQueued = pSound->GetQueueFramesAvail ();	// frames waiting to be played
	if (nQueued > SOUND_HIGH)
	{
		return;				// running ahead: skip this frame
	}
	pSound->Write (Buffer, sizeof Buffer);
	if (nQueued < SOUND_LOW)
	{
		pSound->Write (Buffer, sizeof Buffer);	// running dry: once more
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
	// First the disk chosen on the web page (SD:/disk.txt), then the defaults
	static char ChosenPath[96];
	const char *Candidates[1 + sizeof DiskNames / sizeof DiskNames[0]];
	unsigned nCandidates = 0;
	{
		FIL File;
		char Name[80];
		UINT nRead = 0;
		if (f_open (&File, BOOT_DISK_FILE, FA_READ) == FR_OK)
		{
			f_read (&File, Name, sizeof Name - 1, &nRead);
			f_close (&File);
		}
		Name[nRead] = '\0';
		Name[strcspn (Name, "\r\n")] = '\0';
		if (Name[0] != '\0' && strpbrk (Name, "/\\:") == 0)
		{
			snprintf (ChosenPath, sizeof ChosenPath, "SD:/%s", Name);
			Candidates[nCandidates++] = ChosenPath;
		}
	}
	for (const char *pName : DiskNames)
	{
		Candidates[nCandidates++] = pName;
	}

	for (unsigned i = 0; i < nCandidates; i++)
	{
		const char *pName = Candidates[i];
		bool bChosen = pName == ChosenPath;
		bool bReadOnly = false;
		FRESULT Result = f_open (&m_DiskFile, pName, FA_READ | FA_WRITE);
		if (Result == FR_DENIED || Result == FR_WRITE_PROTECTED)
		{
			bReadOnly = true;
			Result = f_open (&m_DiskFile, pName, FA_READ);
		}
		if (Result != FR_OK)
		{
			if (bChosen)
			{
				Say ("Disk: %s (chosen in %s) not found; using the default",
				     pName + 4, BOOT_DISK_FILE + 4);
			}
			continue;
		}

		FSIZE_t nSize = f_size (&m_DiskFile);
		if ((nSize == 0 || (nSize & 511) != 0) && bChosen)
		{
			f_close (&m_DiskFile);
			Say ("Disk: %s (chosen in %s) is not a raw disk image; using the default",
			     pName + 4, BOOT_DISK_FILE + 4);
			continue;
		}
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
		g_Web.pDiskInUse = m_pDiskName;	// the page won't replace or delete it
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
	m_nOffX = nW < m_nFBWidth  ? ((m_nFBWidth - nW) / 2) & ~1u : 0;	// even: aligned rows
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
		const u8 *pSrc = pMac + y * EMU_SCREEN_STRIDE;
		u8 *pShadow = s_Shadow + y * EMU_SCREEN_STRIDE;
		if (!bForce && memcmp (pSrc, pShadow, EMU_SCREEN_STRIDE) == 0)
		{
			continue;
		}
		memcpy (pShadow, pSrc, EMU_SCREEN_STRIDE);

		// Build the scaled row in ordinary RAM, then copy it to the screen.
		// Never write pixels straight into the framebuffer: at odd scales
		// the compiler packs two 16-bit pixels into one 32-bit store, which
		// lands on a 2-byte boundary every other time.  The Pi 1/Zero's
		// ARM11 faults on misaligned stores to screen memory (Zero W crash
		// at 3x, 2026-11-20); newer cores and QEMU don't.  RAM is fine.
		static u32 LineBuf[4096] __attribute__((aligned(64)));	// 16 KB
		unsigned nLineBytes = nCols * m_nScale * nBytesPP;
		if (nLineBytes > sizeof LineBuf)
		{
			continue;	// wider than 4096 px at 32 bpp: not drawn
		}

		if (nBytesPP == 2)
		{
			u16 *pOut = (u16 *) LineBuf;
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
			u32 *pOut = LineBuf;
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

		// One copy per screen row (also the vertical scaling); the row
		// start is 4-byte aligned (pitch and m_nOffX are kept even)
		u8 *pLine = m_pFB + (m_nOffY + y * m_nScale) * m_nFBPitch + m_nOffX * nBytesPP;
		for (unsigned s = 0; s < m_nScale; s++)
		{
			memcpy (pLine + s * m_nFBPitch, LineBuf, nLineBytes);
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

	// SD card (started in Initialize so the log could be written)
	if (!m_bSDOK)
	{
		Fatal ("Cannot start the SD card, or mount its FAT partition.");
	}

	LoadROM ();
	bool bReadOnly = OpenDisk ();

	// USB keyboard + mouse.  Few log writes during start-up (each costs
	// ~0.3 s on a Zero W): before and after USB, then after the first
	// second of running.  Fatal() and a crash write it too.
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

	// Clock: time zone, saved PRAM + clock, then Wi-Fi (in the background)
	strncpy (m_TimeZone, mOptions.GetAppOptionString ("tz", "GMT0BST,M3.5.0/1,M10.5.0"),
		 sizeof m_TimeZone - 1);
	strncpy (m_NTPServer, mOptions.GetAppOptionString ("ntp", "pool.ntp.org"),
		 sizeof m_NTPServer - 1);
	m_TimeZone[sizeof m_TimeZone - 1] = '\0';
	m_NTPServer[sizeof m_NTPServer - 1] = '\0';
	setenv ("TZ", m_TimeZone, 1);
	tzset ();
	LoadClock ();
	StartWiFi ();

	int nResult = emu_init (m_pROM, m_bDiskOpen ? &m_DiskFile : 0, m_nDiskSize, bReadOnly);
	if (nResult == -1)
	{
		Fatal ("The ROM could not be patched; it is not a supported Mac Plus ROM.");
	}
	else if (nResult != 0)
	{
		Fatal ("Not enough memory for the Mac's %u KB of RAM.", emu_ram_kb ());
	}

	StartSound ();
	Log ("Mac:  started");

	// From here the screen belongs to the Mac (no pause for the banner:
	// errors stop with Fatal() and stay on screen)
	if (m_bScreenOK)
	{
		// Record the scale we're about to draw at; if this boot hangs, the
		// next one sees "drawing N" and falls back (see SetupDisplay)
		char Stage[32];
		snprintf (Stage, sizeof Stage, "drawing %u%s\r\n", m_nScale,
			  m_bScaleFallback ? " fallback" : "");
		WriteStage (Stage);

		static const char ClearScreen[] = "\x1b[H\x1b[J\x1b[?25l";
		mScreen.Write (ClearScreen, sizeof ClearScreen - 1);

		u64 nStart = CTimer::GetClockTicks64 ();
		Blit (true);
		Log ("Mac:  first frame drawn in %u ms",
		     (unsigned) ((CTimer::GetClockTicks64 () - nStart) / 1000));
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
				FlushBootLog ();	// once, at 60 s (a write mid-run is a hitch)
			}
			nSlices = 0;
			nStatsStart = CTimer::GetClockTicks64 ();
		}

		if (nNow >= nNextVsync)
		{
			FeedSound ();		// this frame's sound, before the Mac's VBL
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

		UpdateNetwork (nNow);		// PRAM saves; Wi-Fi time while it runs
	}

	return ShutdownHalt;
}
