#ifndef _kernel_h
#define _kernel_h
#include <circle_stdlib_app.h>
#include <circle/cputhrottle.h>
class CKernel : public CStdlibAppStdio
{
public:
	CKernel (void) : CStdlibAppStdio ("cpubench"), m_CPUThrottle (CPUSpeedLow) {}
	TShutdownMode Run (void);
private:
	void Say (const char *pFormat, ...);
	void RunOnce (const char *pClock);
	CCPUThrottle m_CPUThrottle;
	char m_Report[4096];
	unsigned m_nReport = 0;
};
#endif
