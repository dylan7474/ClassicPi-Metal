//
// webserver.h - a small web page on the Pi for the Mac's disk images:
// list, download, upload, delete, choose the start-up disk, restart.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _webserver_h
#define _webserver_h

#include <circle/net/netsubsystem.h>
#include <circle/sched/task.h>

#define BOOT_DISK_FILE	"SD:/disk.txt"	// name of the disk image to start from

// Shared with the kernel's main loop (which owns the Mac and its disk)
struct TWebShared
{
	const char	*pDiskInUse;		// file name of the Mac's disk, or 0
	volatile bool	bSyncRequest;		// web -> main: flush the Mac's disk now
	volatile bool	bSynced;		// main -> web: done
	volatile bool	bRestartRequest;	// web -> main: save, then reboot
	volatile unsigned nTransfers;		// downloads/uploads running: yield more
};

extern TWebShared g_Web;

class CWebTask : public CTask
{
public:
	CWebTask (CNetSubSystem *pNet, unsigned nPort);

	void Run (void) override;

private:
	CNetSubSystem	*m_pNet;
	unsigned	 m_nPort;
};

#endif
