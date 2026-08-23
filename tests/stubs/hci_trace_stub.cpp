/* Host-test trace sink for tests whose subject is not logging. */
#include "hci_trace.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HCI_TRACE_TEST_CAPACITY 4096U

static char s_Trace[HCI_TRACE_TEST_CAPACITY];
static size_t s_TraceLen;

extern "C" void HciTrace(const char *pFormat, ...)
{
    if (pFormat == nullptr)
    {
        return;
    }

    char line[HCI_TRACE_RECORD_SIZE];
    va_list args;
    va_start(args, pFormat);
    const int len = vsnprintf(line, sizeof(line), pFormat, args);
    va_end(args);

    if (len <= 0 || s_TraceLen >= sizeof(s_Trace) - 1U)
    {
        return;
    }

    size_t count = (size_t)len;
    if (count >= sizeof(line))
    {
        count = sizeof(line) - 1U;
    }

    const size_t available = sizeof(s_Trace) - s_TraceLen - 1U;
    if (count > available)
    {
        count = available;
    }

    memcpy(&s_Trace[s_TraceLen], line, count);
    s_TraceLen += count;
    s_Trace[s_TraceLen] = '\0';
}

extern "C" void HciTraceInit(void)
{
    HciTrace("\r\n--- HciController trace ---\r\n");
}

extern "C" void HciTraceSetSink(DevIntrf_t *, uint32_t)
{
}

extern "C" int HciTraceFlush(void)
{
    return 0;
}

extern "C" uint32_t HciTracePending(void)
{
    return 0U;
}

extern "C" uint32_t HciTraceDropped(void)
{
    return 0U;
}

extern "C" void HciTraceTestReset(void)
{
    s_TraceLen = 0U;
    s_Trace[0] = '\0';
}

extern "C" size_t HciTraceTestTake(char *pOut, size_t Capacity)
{
    if (pOut == nullptr || Capacity == 0U)
    {
        return 0U;
    }

    size_t count = s_TraceLen;
    if (count >= Capacity)
    {
        count = Capacity - 1U;
    }

    memcpy(pOut, s_Trace, count);
    pOut[count] = '\0';
    return count;
}
