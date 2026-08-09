/*
 * Host test for the runtime pump. A lost wake must not stall the host.
 */

#include "hci_taktos.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned gTargetStart;
static unsigned gHostStart;
static unsigned gProcess;
static unsigned gMpslProcess;
static unsigned gSemTake;
static unsigned gGiveCount;
static bool gSemAlwaysTimesOut;
static uint32_t gLastWaitTicks;
static HciTaktOs_t *gRuntime;


extern "C" TaktOSErr_t TaktOSSemInit(TaktOSSem_t *pSem, uint32_t Initial, uint32_t Max)
{
    pSem->Count = Initial;
    pSem->Max = Max;
    return TAKTOS_OK;
}

extern "C" TaktOSErr_t TaktOSSemGive(TaktOSSem_t *pSem, bool)
{
    gGiveCount++;
    if (pSem->Count >= pSem->Max)
    {
        return TAKTOS_ERR_FULL;
    }
    pSem->Count++;
    return TAKTOS_OK;
}

extern "C" TaktOSErr_t TaktOSSemTake(TaktOSSem_t *pSem, bool, uint32_t WaitTicks)
{
    gSemTake++;
    gLastWaitTicks = WaitTicks;

    /* Stop the loop once the point is made. */
    if (gSemTake > 20U)
    {
        gRuntime->StopRequested = true;
        return TAKTOS_ERR_FULL;
    }

    if (gSemAlwaysTimesOut)
    {
        return TAKTOS_ERR_FULL;
    }

    if (pSem->Count == 0U)
    {
        return TAKTOS_ERR_FULL;
    }

    pSem->Count--;
    return TAKTOS_OK;
}

static bool TargetStart(void *) { gTargetStart++; return true; }
static bool HostStart(void *) { gHostStart++; return true; }
static void Process(void *) { gProcess++; }
static void MpslProcess(void *) { gMpslProcess++; }

static void Reset(void)
{
    gTargetStart = 0U; gHostStart = 0U; gProcess = 0U; gMpslProcess = 0U;
    gSemTake = 0U; gGiveCount = 0U; gSemAlwaysTimesOut = false; gLastWaitTicks = 0U;
}

static void Run(uint32_t PollMs, bool AlwaysTimeout)
{
    static HciTaktOs_t runtime;
    HciTaktOsOps_t ops = {};
    HciTaktOsHostOps_t hostOps = {};

    Reset();
    gSemAlwaysTimesOut = AlwaysTimeout;
    gRuntime = &runtime;

    ops.Start = TargetStart;
    ops.ProcessMpsl = MpslProcess;
    hostOps.Start = HostStart;
    hostOps.Process = Process;
    hostOps.PollIntervalMs = PollMs;

    assert(HciTaktOsInit(&runtime, &ops, &hostOps));
    HciTaktOsThread(&runtime);

    assert(gTargetStart == 1U && gHostStart == 1U);
    assert(runtime.Started);
}

int main(void)
{
    /* A wake that never arrives still pumps the host on the poll interval. */
    Run(5U, true);
    assert(gLastWaitTicks == 5U);
    assert(gProcess > 0U);
    assert(gRuntime->PollWakeCount > 0U);
    printf("[ok] lost wake still pumps the host\n");

    /* With no poll interval the thread waits for a wake only. */
    Run(0U, true);
    assert(gLastWaitTicks == TAKTOS_WAIT_FOREVER);
    assert(gProcess == 0U);
    assert(gRuntime->PollWakeCount == 0U);
    printf("[ok] uart mode keeps waiting for a wake\n");

    printf("All runtime pump tests passed.\n");
    return 0;
}
