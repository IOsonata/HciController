/**-------------------------------------------------------------------------
@file	hci_trace.cpp

@brief	Buffered HciController diagnostic trace implementation.

		Initializes the IOsonata SysLog record store, formats trace records,
		and selects the output DeviceIntrf.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_trace.h"

#include <stdarg.h>
#include <stdio.h>

#include "syslog.h"

#define HCI_TRACE_SYS_WRITE0 0x04

alignas(4) static uint8_t s_LogFifoMem[
    SYSLOG_MEMSIZE(HCI_TRACE_RECORD_COUNT, HCI_TRACE_RECORD_SIZE)];
static const SysLogCfg_t s_LogCfg = {
    s_LogFifoMem,
    sizeof(s_LogFifoMem),
    HCI_TRACE_RECORD_SIZE,
    false,
};
static SysLog_t s_Log;
static bool s_LogInitialized;

static bool HciTraceEnsureInit(void)
{
    if (s_LogInitialized)
    {
        return true;
    }

    s_LogInitialized =
        SysLogInit(&s_Log, &s_LogCfg, nullptr, 0U, nullptr, 0U);
    return s_LogInitialized;
}

#if HCI_TRACE
static void HciTraceWrite0(const char *pStr)
{
#if defined(__arm__)
    register const char *arg __asm__("r1") = pStr;
    register int op __asm__("r0") = HCI_TRACE_SYS_WRITE0;

    __asm__ volatile ("bkpt 0xAB"
                      : "+r" (op)
                      : "r" (arg)
                      : "memory");
    (void)op;
#else
    fputs(pStr, stderr);
#endif
}
#endif

void HciTrace(const char *pFormat, ...)
{
    if (pFormat == nullptr || !HciTraceEnsureInit())
    {
        return;
    }

    va_list args;
    va_start(args, pFormat);

#if HCI_TRACE
    va_list debugArgs;
    va_copy(debugArgs, args);
    char line[HCI_TRACE_RECORD_SIZE];
    const int len = vsnprintf(line, sizeof(line), pFormat, debugArgs);
    va_end(debugArgs);

    if (len >= 0)
    {
        if (len >= (int)sizeof(line))
        {
            line[sizeof(line) - 4U] = '.';
            line[sizeof(line) - 3U] = '.';
            line[sizeof(line) - 2U] = '.';
            line[sizeof(line) - 1U] = '\0';
        }
        HciTraceWrite0(line);
    }
#endif

    (void)SysLogVPrintf(&s_Log, pFormat, args);
    va_end(args);
}

void HciTraceInit(void)
{
    HciTrace("\r\n--- HciController trace ---\r\n");
}

void HciTraceSetSink(DevIntrf_t *pSink, uint32_t SinkAddr)
{
    if (!HciTraceEnsureInit())
    {
        return;
    }

    SysLogSetSink(&s_Log, pSink, SinkAddr);
}

int HciTraceFlush(void)
{
    return HciTraceEnsureInit() ? SysLogFlush(&s_Log) : 0;
}
