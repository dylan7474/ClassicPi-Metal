//
// webserver.cpp - the Mac's disk images over Wi-Fi: a page at http://<pi>/
// to list, download, upload and delete them, choose the one the Mac starts
// from, and restart.  One client at a time, HTTP/1.0, no login (the network
// is the user's own; see the README).
//
// Downloads and uploads stream through a small buffer, so a 200 MB disk
// image needs no memory to speak of.  An upload goes to upload.part and is
// renamed when complete, so a dropped upload never leaves half a disk.
// The disk the Mac is using can't be replaced or deleted while it runs.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "webserver.h"

#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/sched/scheduler.h>
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>

TWebShared g_Web;

#define FROM		"web"
#define UPLOAD_TEMP	"SD:/upload.part"
#define IDLE_TIMEOUT_MS	30000		// a silent client is dropped
#define SEND_CHUNK	1024		// Circle's TCP sends a frame at a time
#define FILE_CHUNK	16384

// ---- helpers ------------------------------------------------------------------

static bool SendAll (CSocket *pSocket, const void *pData, unsigned nLength)
{
	const u8 *p = (const u8 *) pData;
	while (nLength > 0)
	{
		unsigned n = nLength < SEND_CHUNK ? nLength : SEND_CHUNK;
		if (pSocket->Send (p, n, 0) < 0)
		{
			return false;
		}
		p += n;
		nLength -= n;
	}
	return true;
}

static bool SendText (CSocket *pSocket, const char *pText)
{
	return SendAll (pSocket, pText, strlen (pText));
}

// Receive up to nSize bytes; 0 = nothing for IDLE_TIMEOUT_MS, <0 = closed
static int ReceiveSome (CSocket *pSocket, void *pBuffer, unsigned nSize)
{
	unsigned nStart = CTimer::Get ()->GetTicks ();
	while (1)
	{
		int n = pSocket->Receive (pBuffer, nSize, MSG_DONTWAIT);
		if (n != 0)
		{
			return n;
		}
		if (CTimer::Get ()->GetTicks () - nStart > IDLE_TIMEOUT_MS * HZ / 1000)
		{
			return 0;
		}
		CScheduler::Get ()->MsSleep (1);
	}
}

static void Respond (CSocket *pSocket, unsigned nStatus, const char *pReason, const char *pText)
{
	CString Head;
	Head.Format ("HTTP/1.0 %u %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
		     "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
		     nStatus, pReason, (unsigned) strlen (pText));
	SendText (pSocket, Head);
	SendText (pSocket, pText);
}

static int HexDigit (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void UrlDecode (const char *pIn, char *pOut, unsigned nSize)
{
	unsigned n = 0;
	while (*pIn && n < nSize - 1)
	{
		if (pIn[0] == '%' && HexDigit (pIn[1]) >= 0 && HexDigit (pIn[2]) >= 0)
		{
			pOut[n++] = (char) (HexDigit (pIn[1]) * 16 + HexDigit (pIn[2]));
			pIn += 3;
		}
		else
		{
			pOut[n++] = *pIn++;
		}
	}
	pOut[n] = '\0';
}

static bool EndsWithNoCase (const char *p, const char *pEnd)
{
	size_t n = strlen (p), m = strlen (pEnd);
	return n >= m && strcasecmp (p + n - m, pEnd) == 0;
}

// A disk image name we may touch: root of the card, sensible characters, a
// disk-image extension, and never one of the Pi's own .img files
static bool IsDiskName (const char *p)
{
	size_t n = strlen (p);
	if (n == 0 || n > 64 || p[0] == '.')
	{
		return false;
	}
	for (const char *q = p; *q; q++)
	{
		if (*q < ' ' || strchr ("/\\:*?\"<>|", *q) != 0)
		{
			return false;
		}
	}
	if (strncasecmp (p, "kernel", 6) == 0 || strncasecmp (p, "armstub", 7) == 0)
	{
		return false;
	}
	return    EndsWithNoCase (p, ".dsk") || EndsWithNoCase (p, ".img")
	       || EndsWithNoCase (p, ".image") || EndsWithNoCase (p, ".hfv");
}

static bool InUse (const char *pName)
{
	return g_Web.pDiskInUse != 0 && strcasecmp (pName, g_Web.pDiskInUse) == 0;
}

static void ReadBootChoice (char *pBuffer, unsigned nSize)
{
	pBuffer[0] = '\0';
	FIL File;
	if (f_open (&File, BOOT_DISK_FILE, FA_READ) != FR_OK)
	{
		return;
	}
	UINT nRead = 0;
	f_read (&File, pBuffer, nSize - 1, &nRead);
	f_close (&File);
	pBuffer[nRead] = '\0';
	for (char *p = pBuffer; *p; p++)		// first line only
	{
		if (*p == '\r' || *p == '\n')
		{
			*p = '\0';
			break;
		}
	}
}

static void HtmlEscape (const char *pIn, CString *pOut)
{
	*pOut = "";
	for (; *pIn; pIn++)
	{
		switch (*pIn)
		{
		case '&':  pOut->Append ("&amp;");  break;
		case '<':  pOut->Append ("&lt;");   break;
		case '>':  pOut->Append ("&gt;");   break;
		case '"':  pOut->Append ("&quot;"); break;
		case '\'': pOut->Append ("&#39;");  break;
		default:   { char c[2] = { *pIn, 0 }; pOut->Append (c); }
		}
	}
}

static void SizeText (FSIZE_t nBytes, char *pBuf, unsigned nSize)
{
	if (nBytes >= 1024 * 1024)
	{
		unsigned nTenths = (unsigned) ((nBytes * 10 + 512 * 1024) / (1024 * 1024));
		snprintf (pBuf, nSize, "%u.%u MB", nTenths / 10, nTenths % 10);
	}
	else
	{
		snprintf (pBuf, nSize, "%u KB", (unsigned) ((nBytes + 1023) / 1024));
	}
}

// ---- the page -------------------------------------------------------------------

static const char PageHead[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>ClassicPi-Metal disks</title><style>"
":root{color-scheme:light dark;--bg:#f4f4f2;--fg:#1d1d1b;--card:#fff;--line:#c9c9c4;--muted:#5d5d58;--accent:#1f5fbf}"
"@media(prefers-color-scheme:dark){:root{--bg:#1b1b1a;--fg:#ececea;--card:#262625;--line:#454542;--muted:#a5a5a0;--accent:#7fb0ff}}"
"body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}"
"main{max-width:860px;margin:0 auto;padding:16px}"
"h1{font-size:1.4em;margin:.2em 0}.sub{color:var(--muted);margin:0 0 1em}"
"section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 16px;margin:12px 0}"
"table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:8px 6px;border-bottom:1px solid var(--line);vertical-align:top}"
"th{font-size:.85em;color:var(--muted);font-weight:600}td.n{word-break:break-all;font-weight:600}"
".tag{display:inline-block;border:1px solid currentColor;border-radius:4px;padding:0 6px;font-size:.8em;font-weight:600;margin:2px 4px 2px 0}"
"button,a.b{font:inherit;font-size:.9em;padding:4px 10px;margin:2px 4px 2px 0;border:1px solid var(--line);border-radius:6px;"
"background:var(--bg);color:var(--fg);cursor:pointer;text-decoration:none;display:inline-block}"
"button:hover,a.b:hover{border-color:var(--accent)}button:disabled{opacity:.45;cursor:default}"
"progress{width:100%;height:14px}#msg{font-weight:600;min-height:1.4em}.note{color:var(--muted);font-size:.9em}"
"</style></head><body><main>";

static const char PageScript[] =
"<script>"
"const msg=t=>document.getElementById('msg').textContent=t;"
"async function post(u,ok){try{const r=await fetch(u,{method:'POST'});const t=await r.text();msg(t);if(r.ok&&ok)setTimeout(()=>location.reload(),600)}catch(e){msg('Failed: '+e)}}"
"function del(n){if(confirm('Delete '+n+' from the card?'))post('/delete/'+encodeURIComponent(n),1)}"
"function boot(n){post('/boot/'+encodeURIComponent(n),1)}"
"function restart(){if(confirm('Restart the Pi now? The Mac stops without shutting down: choose Special > Shut Down on the Mac first.'))"
"post('/restart',0).then(()=>setTimeout(()=>location.reload(),20000))}"
"function up(){const f=document.getElementById('file').files[0];if(!f){msg('Choose a disk image first.');return}"
"const x=new XMLHttpRequest(),p=document.getElementById('prog'),b=document.getElementById('upb');"
"x.open('PUT','/disk/'+encodeURIComponent(f.name));b.disabled=true;p.hidden=false;"
"x.upload.onprogress=e=>{if(e.lengthComputable){p.value=e.loaded/e.total;msg('Uploading '+f.name+': '+Math.floor(100*e.loaded/e.total)+'%')}};"
"x.onload=()=>{msg(x.responseText);b.disabled=false;if(x.status==200)setTimeout(()=>location.reload(),800)};"
"x.onerror=()=>{msg('Upload failed (connection lost).');b.disabled=false};x.send(f)}"
"</script>";

static void SendPage (CSocket *pSocket)
{
	char BootChoice[80];
	ReadBootChoice (BootChoice, sizeof BootChoice);

	CString Body;
	Body.Append (PageHead);

	CString Line;
	Line.Format ("<h1>ClassicPi-Metal: disk images</h1><p class=sub>%s. The Mac is running from "
		     "<b>%s</b>.</p><p id=msg role=status></p>",
		     CMachineInfo::Get ()->GetMachineName (),
		     g_Web.pDiskInUse ? g_Web.pDiskInUse : "no disk");
	Body.Append (Line);

	Body.Append ("<section><table><tr><th>Disk image</th><th>Size</th><th>Status</th><th>Actions</th></tr>");

	DIR Dir;
	FILINFO Info;
	unsigned nFound = 0;
	if (f_opendir (&Dir, "SD:/") == FR_OK)
	{
		while (f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
		{
			if ((Info.fattrib & AM_DIR) || !IsDiskName (Info.fname))
			{
				continue;
			}
			nFound++;

			CString Name, Size;
			HtmlEscape (Info.fname, &Name);
			char SizeBuf[24];
			SizeText (Info.fsize, SizeBuf, sizeof SizeBuf);

			bool bInUse = InUse (Info.fname);
			bool bNext = BootChoice[0] != '\0' ? strcasecmp (BootChoice, Info.fname) == 0
							   : bInUse;
			CString Status;
			if (bInUse) Status.Append ("<span class=tag>In use now</span>");
			if (bNext)  Status.Append ("<span class=tag>Starts next time</span>");

			// The name goes into JS via a data attribute (escaped as HTML)
			Line.Format ("<tr><td class=n>%s</td><td>%s</td><td>%s</td><td>"
				     "<a class=b href='/disk/%s' download>Download</a>",
				     (const char *) Name, SizeBuf, (const char *) Status,
				     (const char *) Name);
			Body.Append (Line);
			if (!bNext)
			{
				Line.Format ("<button data-n=\"%s\" onclick='boot(this.dataset.n)'>Start from this</button>",
					     (const char *) Name);
				Body.Append (Line);
			}
			if (!bInUse)
			{
				Line.Format ("<button data-n=\"%s\" onclick='del(this.dataset.n)'>Delete</button>",
					     (const char *) Name);
				Body.Append (Line);
			}
			Body.Append ("</td></tr>");
		}
		f_closedir (&Dir);
	}
	if (nFound == 0)
	{
		Body.Append ("<tr><td colspan=4>No disk images on the card yet.</td></tr>");
	}
	Body.Append ("</table>");

	DWORD nFreeClusters = 0;
	FATFS *pFS;
	if (f_getfree ("SD:", &nFreeClusters, &pFS) == FR_OK)
	{
		u64 nFree = (u64) nFreeClusters * pFS->csize * 512;
		Line.Format ("<p class=note>Free space on the card: %u MB.</p>",
			     (unsigned) (nFree / (1024 * 1024)));
		Body.Append (Line);
	}
	Body.Append ("</section>");

	Body.Append ("<section><b>Upload a disk image</b> (.dsk, .img, .image or .hfv: a raw HFS image;"
		     " a Disk Copy 4.2 image needs its 84-byte header removed first)<p>"
		     "<input type=file id=file> <button id=upb onclick='up()'>Upload</button></p>"
		     "<progress id=prog value=0 max=1 hidden></progress>"
		     "<p class=note>An image with the same name is replaced (never the one in use)."
		     " Uploading slows the Mac down while it runs.</p></section>");

	Body.Append ("<section><b>Start-up disk</b><p class=note>&ldquo;Start from this&rdquo; takes effect"
		     " the next time the Pi starts. Shut the Mac down first (Special &gt; Shut Down),"
		     " then restart here or switch the Pi off and on.</p>"
		     "<button onclick='restart()'>Restart the Pi</button>"
		     "<p class=note>Downloading the disk in use gives a copy as it is on the card"
		     " right now; for a clean copy, shut the Mac down first.</p></section>");

	Body.Append (PageScript);
	Body.Append ("</main></body></html>");

	CString Head;
	Head.Format ("HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
		     "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
		     Body.GetLength ());
	SendText (pSocket, Head);
	SendAll (pSocket, (const char *) Body, Body.GetLength ());
}

// ---- downloads and uploads ------------------------------------------------------

static void Download (CSocket *pSocket, const char *pName)
{
	char Path[96];
	snprintf (Path, sizeof Path, "SD:/%s", pName);

	if (InUse (pName))			// have the main loop flush it first
	{
		g_Web.bSynced = false;
		g_Web.bSyncRequest = true;
		for (unsigned i = 0; i < 200 && !g_Web.bSynced; i++)
		{
			CScheduler::Get ()->MsSleep (10);
		}
	}

	FIL File;
	if (f_open (&File, Path, FA_READ) != FR_OK)
	{
		Respond (pSocket, 404, "Not Found", "No such disk image on the card.");
		return;
	}

	CString Head;
	Head.Format ("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
		     "Content-Length: %u\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
		     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
		     (unsigned) f_size (&File), pName);
	SendText (pSocket, Head);

	static u8 Buffer[FILE_CHUNK];
	g_Web.nTransfers++;
	unsigned nTotal = 0;
	while (1)
	{
		UINT nRead = 0;
		if (f_read (&File, Buffer, sizeof Buffer, &nRead) != FR_OK || nRead == 0)
		{
			break;
		}
		if (!SendAll (pSocket, Buffer, nRead))
		{
			break;
		}
		nTotal += nRead;
	}
	g_Web.nTransfers--;
	f_close (&File);
	CLogger::Get ()->Write (FROM, LogNotice, "sent %s (%u bytes)", pName, nTotal);
}

static void Upload (CSocket *pSocket, const char *pName, unsigned nLength,
		    const u8 *pAlready, unsigned nAlready)
{
	if (InUse (pName))
	{
		Respond (pSocket, 409, "Conflict",
			 "That disk is in use by the Mac. Upload it under another name, or start"
			 " from another disk first.");
		return;
	}

	DWORD nFreeClusters = 0;
	FATFS *pFS;
	if (   f_getfree ("SD:", &nFreeClusters, &pFS) == FR_OK
	    && (u64) nFreeClusters * pFS->csize * 512 < (u64) nLength + 1024 * 1024)
	{
		Respond (pSocket, 507, "Insufficient Storage", "Not enough free space on the card.");
		return;
	}

	FIL File;
	f_unlink (UPLOAD_TEMP);
	if (f_open (&File, UPLOAD_TEMP, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		Respond (pSocket, 500, "Internal Server Error", "Cannot write to the card.");
		return;
	}

	static u8 Buffer[32768];
	unsigned nFill = 0, nGot = 0;
	bool bOK = true;
	g_Web.nTransfers++;

	if (nAlready > nLength)
	{
		nAlready = nLength;
	}
	memcpy (Buffer, pAlready, nAlready);
	nFill = nGot = nAlready;

	while (bOK && nGot < nLength)
	{
		unsigned nWant = sizeof Buffer - nFill;
		if (nWant > nLength - nGot)
		{
			nWant = nLength - nGot;
		}
		int n = ReceiveSome (pSocket, Buffer + nFill, nWant);
		if (n <= 0)
		{
			bOK = false;			// dropped or silent
			break;
		}
		nFill += n;
		nGot += n;
		if (nFill == sizeof Buffer || nGot == nLength)
		{
			UINT nWritten;
			if (f_write (&File, Buffer, nFill, &nWritten) != FR_OK || nWritten != nFill)
			{
				bOK = false;
			}
			nFill = 0;
		}
	}
	g_Web.nTransfers--;
	f_close (&File);

	if (!bOK)
	{
		f_unlink (UPLOAD_TEMP);
		Respond (pSocket, 500, "Internal Server Error", "Upload failed; nothing was changed.");
		CLogger::Get ()->Write (FROM, LogWarning, "upload of %s failed at %u of %u bytes",
					pName, nGot, nLength);
		return;
	}

	char Path[96];
	snprintf (Path, sizeof Path, "SD:/%s", pName);
	f_unlink (Path);			// replace an older one
	if (f_rename (UPLOAD_TEMP, Path + 3) != FR_OK)	// FatFs: new name without "SD:"
	{
		Respond (pSocket, 500, "Internal Server Error", "Upload received, but it could not be renamed.");
		return;
	}
	CLogger::Get ()->Write (FROM, LogNotice, "received %s (%u bytes)", pName, nLength);

	CString Text;
	Text.Format ("Uploaded %s (%u MB). Choose \"Start from this\" to use it.", pName,
		     (nLength + 512 * 1024) / (1024 * 1024));
	Respond (pSocket, 200, "OK", Text);
}

// ---- one request ------------------------------------------------------------------

static void Serve (CSocket *pSocket)
{
	static u8 Request[8192];
	unsigned nHave = 0;
	char *pBodyStart = 0;

	while (pBodyStart == 0)
	{
		if (nHave >= sizeof Request - 1)
		{
			Respond (pSocket, 431, "Request Header Fields Too Large", "Request too large.");
			return;
		}
		int n = ReceiveSome (pSocket, Request + nHave, sizeof Request - 1 - nHave);
		if (n <= 0)
		{
			return;
		}
		nHave += n;
		Request[nHave] = '\0';
		pBodyStart = strstr ((char *) Request, "\r\n\r\n");
	}
	*pBodyStart = '\0';
	pBodyStart += 4;
	unsigned nBodyHave = nHave - (unsigned) ((u8 *) pBodyStart - Request);

	// "METHOD /path HTTP/1.x"
	char Method[8], RawPath[256];
	if (sscanf ((char *) Request, "%7s %255s", Method, RawPath) != 2)
	{
		Respond (pSocket, 400, "Bad Request", "Bad request.");
		return;
	}

	unsigned nLength = 0;
	for (char *p = (char *) Request; (p = strchr (p, '\n')) != 0; )
	{
		p++;
		if (strncasecmp (p, "Content-Length:", 15) == 0)
		{
			sscanf (p + 15, "%u", &nLength);
		}
	}

	char Arg[128] = "";
	const char *pRoute = RawPath;
	static const char *const Prefixes[] = { "/disk/", "/delete/", "/boot/" };
	for (const char *pPrefix : Prefixes)
	{
		size_t n = strlen (pPrefix);
		if (strncmp (RawPath, pPrefix, n) == 0)
		{
			UrlDecode (RawPath + n, Arg, sizeof Arg);
			RawPath[n - 1] = '\0';		// route without the name, e.g. "/disk"
			break;
		}
	}

	if (strcmp (Method, "GET") == 0 && strcmp (pRoute, "/") == 0)
	{
		SendPage (pSocket);
	}
	else if (strcmp (pRoute, "/disk") == 0 || strcmp (pRoute, "/delete") == 0
		 || strcmp (pRoute, "/boot") == 0)
	{
		if (!IsDiskName (Arg))
		{
			Respond (pSocket, 400, "Bad Request",
				 "Not a disk image name (.dsk, .img, .image or .hfv; no folders).");
		}
		else if (strcmp (pRoute, "/disk") == 0 && strcmp (Method, "GET") == 0)
		{
			Download (pSocket, Arg);
		}
		else if (strcmp (pRoute, "/disk") == 0 && strcmp (Method, "PUT") == 0)
		{
			Upload (pSocket, Arg, nLength, (const u8 *) pBodyStart, nBodyHave);
		}
		else if (strcmp (pRoute, "/delete") == 0 && strcmp (Method, "POST") == 0)
		{
			char Path[96];
			snprintf (Path, sizeof Path, "SD:/%s", Arg);
			if (InUse (Arg))
			{
				Respond (pSocket, 409, "Conflict", "That disk is in use by the Mac.");
			}
			else if (f_unlink (Path) != FR_OK)
			{
				Respond (pSocket, 404, "Not Found", "No such disk image on the card.");
			}
			else
			{
				CString Text;
				Text.Format ("Deleted %s.", Arg);
				Respond (pSocket, 200, "OK", Text);
			}
		}
		else if (strcmp (pRoute, "/boot") == 0 && strcmp (Method, "POST") == 0)
		{
			char Path[96];
			snprintf (Path, sizeof Path, "SD:/%s", Arg);
			FILINFO Info;
			FIL File;
			if (f_stat (Path, &Info) != FR_OK)
			{
				Respond (pSocket, 404, "Not Found", "No such disk image on the card.");
			}
			else if (f_open (&File, BOOT_DISK_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
			{
				Respond (pSocket, 500, "Internal Server Error", "Cannot write to the card.");
			}
			else
			{
				UINT nWritten;
				f_write (&File, Arg, strlen (Arg), &nWritten);
				f_write (&File, "\r\n", 2, &nWritten);
				f_close (&File);
				CString Text;
				Text.Format ("The Mac will start from %s next time.", Arg);
				Respond (pSocket, 200, "OK", Text);
			}
		}
		else
		{
			Respond (pSocket, 405, "Method Not Allowed", "Method not allowed.");
		}
	}
	else if (strcmp (Method, "POST") == 0 && strcmp (pRoute, "/restart") == 0)
	{
		Respond (pSocket, 200, "OK", "Restarting the Pi: this page comes back in about 20 seconds.");
		CScheduler::Get ()->MsSleep (300);	// let the reply go out
		g_Web.bRestartRequest = true;
	}
	else
	{
		Respond (pSocket, 404, "Not Found", "Not found.");
	}
}

// ---- the task ------------------------------------------------------------------------

CWebTask::CWebTask (CNetSubSystem *pNet, unsigned nPort)
:	m_pNet (pNet), m_nPort (nPort)
{
	SetName ("web");
}

void CWebTask::Run (void)
{
	while (!m_pNet->IsRunning ())
	{
		CScheduler::Get ()->MsSleep (200);
	}

	CSocket Listen (m_pNet, IPPROTO_TCP);
	if (Listen.Bind ((u16) m_nPort) < 0 || Listen.Listen (2) < 0)
	{
		CLogger::Get ()->Write (FROM, LogError, "cannot listen on port %u", m_nPort);
		return;
	}

	CString IP;
	m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
	CLogger::Get ()->Write (FROM, LogNotice, "disk images page at http://%s%s (or http://classicpi.local)",
				(const char *) IP, m_nPort == 80 ? "/" : ":port/");

	while (1)
	{
		CIPAddress Client;
		u16 usPort;
		CSocket *pClient = Listen.Accept (&Client, &usPort);
		if (pClient == 0)
		{
			CScheduler::Get ()->MsSleep (200);
			continue;
		}
		Serve (pClient);
		delete pClient;			// closes the connection
	}
}
