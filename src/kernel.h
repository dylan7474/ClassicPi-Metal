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
#include <fatfs/ff.h>

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
	void LogBoard (void);
	bool LoadROM (void);
	bool OpenDisk (void);
	void WriteSpeedFile (unsigned nPercent);

	// Display
	void SetupDisplay (void);
	void Blit (bool bForce);

	// Input (handlers run in interrupt context: just queue)
	void UpdateUSB (void);
	void DrainInput (void);
	static void KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseStatusHandler (unsigned nButtons, int nDX, int nDY, int nWheel);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CUSBHCIDevice	m_USBHCI;
	CEMMCDevice	m_EMMC;
	FATFS		m_FileSystem;
	CConsole	m_SerialConsole;	// stdout/stderr of the core -> UART
	CLogTee		m_LogTee;

	bool		m_bSDOK;		// SD card started and FAT mounted
	bool		m_bScreenOK;		// HDMI framebuffer started

	CUSBKeyboardDevice * volatile m_pKeyboard;
	CMouseDevice * volatile m_pMouse;

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
