//
// kernel.h - ClassicPi-Metal: bare-metal Mac Plus on a Raspberry Pi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle_stdlib_app.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/sched/scheduler.h>
#include <circle/net/netsubsystem.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <fatfs/ff.h>

#define MAX_INPUTS	4	// keyboards and mice in use at once
#define USB_LOG_FLUSHES	40	// boot log rewrites for USB changes per boot

// Circle's logger writes here: every line goes to the UART and into the
// boot log kept in RAM (flushed to SD:/bootlog.txt at each stage).
class CLogTee : public CDevice
{
public:
	CLogTee (CDevice *pSerial) : m_pSerial (pSerial) {}
	int Write (const void *pBuffer, size_t nCount) override;
private:
	CDevice *m_pSerial;
};

class CKernel : public CStdlibAppScreen
{
public:
	CKernel (void);

	bool Initialize (void) override;
	TShutdownMode Run (void) override;

private:
	// Startup
	void Say (const char *pFormat, ...);	// screen + boot log + serial
	void Log (const char *pFormat, ...);	// boot log + serial only
	void Fatal (const char *pFormat, ...);	// say, flush the log, then stop for good
	void FlushBootLog (void);		// rewrite SD:/bootlog.txt from RAM
	static void PanicHandler (void);	// Circle panic: save the log once
	void WriteStage (const char *pText);	// SD:/stage-<board>.txt
	bool ReadStage (char *pBuffer, unsigned nSize);
	void LogBoard (void);
	bool LoadROM (void);
	bool OpenDisk (void);

	// Clock + PRAM (SD:/pram.dat) and Wi-Fi time (NTP)
	void LoadClock (void);
	void SaveClock (void);
	void StartWiFi (void);
	void UpdateNetwork (u64 nNow);
	u32 UnixToMac (unsigned nUnix);
	void WriteSpeedFile (unsigned nPercent);

	// Display
	void SetupDisplay (void);
	void Blit (bool bForce);

	// Input (handlers run in interrupt context: just queue)
	void UpdateUSB (void);
	void DrainInput (void);
	static unsigned CountInputs (CDevice * volatile const *pSlots);
	static void KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6], void *pArg);
	static void KeyRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseStatusHandler (unsigned nButtons, int nDX, int nDY, int nWheel, void *pArg);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CScheduler	m_Scheduler;		// first: Circle's network runs as tasks
	CUSBHCIDevice	m_USBHCI;
	CEMMCDevice	m_EMMC;
	FATFS		m_FileSystem;
	CConsole	m_SerialConsole;	// stdout/stderr of the core -> UART
	CLogTee		m_LogTee;

	// Per-board file names, so several boards can share one card:
	// "Raspberry Pi Zero 2 W" -> SD:/speed-zero-2-w.txt, SD:/bootlog-zero-2-w.txt
	char		m_SpeedFile[64];
	char		m_BootLogFile[64];
	char		m_StageFile[64];	// how far the last boot got drawing

	// Wi-Fi (only with SD:/wpa_supplicant.conf): joins, gets the time once
	// from NTP, then the network stops being given time
	CBcm4343Device	*m_pWLAN;
	CNetSubSystem	*m_pNet;
	CWPASupplicant	*m_pWPASupplicant;
	bool		m_bNetActive;		// main loop still yields to the network
	bool		m_bTimeDone;		// NTP answered or given up
	bool		m_bWeb;			// disk-images page on (cmdline web=)
	u64		m_nNetStart;
	char		m_NTPServer[64];	// cmdline.txt ntp=
	char		m_TimeZone[64];		// cmdline.txt tz= (POSIX TZ)
	bool		m_bClockDirty;		// PRAM/clock changed, save soon
	u64		m_nClockChanged;

	bool		m_bSDOK;		// SD card started and FAT mounted
	bool		m_bScreenOK;		// HDMI framebuffer started
	bool		m_bScaleFallback;	// scale lowered after a hung boot

	CUSBKeyboardDevice * volatile m_pKeyboards[MAX_INPUTS];	// slot -> device, 0 = free
	CMouseDevice * volatile m_pMice[MAX_INPUTS];
	unsigned	m_nUSBLogFlushes;	// hot-plug log writes so far
	volatile bool	m_bUSBRemoved;		// set by the removed handlers

	u8		*m_pROM;
	FIL		m_DiskFile;
	bool		m_bDiskOpen;
	u32		m_nDiskSize;
	const char	*m_pDiskName;

	// Display geometry
	u8		*m_pFB;
	unsigned	m_nFBWidth, m_nFBHeight, m_nFBPitch, m_nFBDepth;
	unsigned	m_nScale, m_nOffX, m_nOffY;

	static CKernel *s_pThis;
};

#endif
