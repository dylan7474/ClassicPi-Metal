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

class CKernel : public CStdlibAppScreen
{
public:
	CKernel (void);

	bool Initialize (void) override;
	TShutdownMode Run (void) override;

private:
	// Startup
	void Say (const char *pFormat, ...);	// screen + serial
	void Fatal (const char *pFormat, ...);	// say, then stop for good
	bool LoadROM (void);
	bool OpenDisk (void);

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
