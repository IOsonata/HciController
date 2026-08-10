/*
 * Host test for the runtime pump and thread lifecycle handshake.
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
static bool gTargetStartResult;
static uint32_t gLastWaitTicks;
static HciTaktOs_t *gRuntime;

extern "C" uint32_t TaktOSEnterCritical(void) { return 0U; }
extern "C" void TaktOSExitCritical(uint32_t) {}

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

    if (gSemTake > 20U && gRuntime != nullptr)
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

static bool TargetStart(void *) { gTargetStart++; return gTargetStartResult; }
static bool HostStart(void *) { gHostStart++; return true; }
static void Process(void *) { gProcess++; }
static void MpslProcess(void *) { gMpslProcess++; }

static void Reset(void)
{
    gTargetStart = 0U;
    gHostStart = 0U;
    gProcess = 0U;
    gMpslProcess = 0U;
    gSemTake = 0U;
    gGiveCount = 0U;
    gSemAlwaysTimesOut = false;
    gTargetStartResult = true;
    gLastWaitTicks = 0U;
    gRuntime = nullptr;
}

static void SetOps(HciTaktOsOps_t *pOps, HciTaktOsHostOps_t *pHostOps)
{
    memset(pOps, 0, sizeof(*pOps));
    memset(pHostOps, 0, sizeof(*pHostOps));
    pOps->Start = TargetStart;
    pOps->ProcessMpsl = MpslProcess;
    pHostOps->Start = HostStart;
    pHostOps->Process = Process;
}

static void Run(uint32_t PollMs, bool AlwaysTimeout)
{
    static HciTaktOs_t runtime;
    HciTaktOsOps_t ops;
    HciTaktOsHostOps_t hostOps;

    Reset();
    gSemAlwaysTimesOut = AlwaysTimeout;
    gRuntime = &runtime;
    SetOps(&ops, &hostOps);
    hostOps.PollIntervalMs = PollMs;

    assert(HciTaktOsInit(&runtime, &ops, &hostOps));
    HciTaktOsThreadArm(&runtime);
    HciTaktOsThread(&runtime);

    assert(gTargetStart == 1U && gHostStart == 1U);
    assert(runtime.Started);
    assert(!runtime.ThreadArmed);
    assert(!runtime.ThreadLive);
}

int main(void)
{
    Run(5U, true);
    assert(gLastWaitTicks == 5U);
    assert(gProcess > 0U);
    assert(gRuntime->PollWakeCount > 0U);
    printf("[ok] lost wake still pumps the host\n");

    Run(0U, true);
    assert(gLastWaitTicks == TAKTOS_WAIT_FOREVER);
    assert(gProcess == 0U);
    assert(gRuntime->PollWakeCount == 0U);
    printf("[ok] uart mode keeps waiting for a wake\n");

    {
        HciTaktOs_t runtime = {};
        HciTaktOsOps_t ops;
        HciTaktOsHostOps_t hostOps;
        Reset();
        SetOps(&ops, &hostOps);
        assert(HciTaktOsInit(&runtime, &ops, &hostOps));

        HciTaktOsWake(&runtime, HCI_TAKTOS_EVENT_HOST);
        assert(gGiveCount == 1U);
        assert(runtime.WakeFoldCount == 0U);

        HciTaktOsWake(&runtime, HCI_TAKTOS_EVENT_HOST);
        HciTaktOsWake(&runtime, HCI_TAKTOS_EVENT_HOST);
        assert(gGiveCount == 1U);
        assert(runtime.WakeFoldCount == 2U);

        HciTaktOsWake(&runtime, HCI_TAKTOS_EVENT_MPSL);
        assert(gGiveCount == 2U);
    }
    printf("[ok] duplicate pending wake leaves the semaphore alone\n");

    {
        HciTaktOs_t runtime = {};
        HciTaktOsOps_t ops;
        HciTaktOsHostOps_t hostOps;

        Reset();
        gRuntime = &runtime;
        gTargetStartResult = false;
        SetOps(&ops, &hostOps);

        assert(HciTaktOsInit(&runtime, &ops, &hostOps));
        HciTaktOsThreadArm(&runtime);
        HciTaktOsThread(&runtime);

        assert(gTargetStart == 1U);
        assert(gHostStart == 0U);
        assert(!runtime.Started);
        assert(!runtime.Running);
        assert(!runtime.ThreadLive);
        assert(!runtime.ThreadArmed);
        assert(runtime.StoppedSem.Count == 1U);
    }
    printf("[ok] target start failure completes the thread handshake\n");

    /*
     * The key scheduler race: after thread creation but before entry, Armed is
     * true and Live is false. WaitStopped must still wait rather than report
     * the target safe to tear down.
     */
    {
        HciTaktOs_t runtime = {};
        HciTaktOsOps_t ops;
        HciTaktOsHostOps_t hostOps;

        Reset();
        SetOps(&ops, &hostOps);
        assert(HciTaktOsInit(&runtime, &ops, &hostOps));
        HciTaktOsThreadArm(&runtime);
        assert(runtime.ThreadArmed && !runtime.ThreadLive);

        gSemAlwaysTimesOut = true;
        assert(!HciTaktOsWaitStopped(&runtime, 7U));
        assert(gSemTake == 1U);
        assert(gLastWaitTicks == 7U);
        HciTaktOsThreadDisarm(&runtime);
        assert(!runtime.ThreadArmed);
    }
    printf("[ok] armed pre-entry thread cannot be mistaken for stopped\n");

    /* A stop before first entry must exit without bringing the target up. */
    {
        HciTaktOs_t runtime = {};
        HciTaktOsOps_t ops;
        HciTaktOsHostOps_t hostOps;

        Reset();
        gRuntime = &runtime;
        SetOps(&ops, &hostOps);
        assert(HciTaktOsInit(&runtime, &ops, &hostOps));
        HciTaktOsThreadArm(&runtime);
        HciTaktOsStop(&runtime);
        HciTaktOsThread(&runtime);

        assert(gTargetStart == 0U);
        assert(gHostStart == 0U);
        assert(!runtime.ThreadArmed);
        assert(!runtime.ThreadLive);
        assert(runtime.StoppedSem.Count == 1U);
    }
    printf("[ok] stop before first instruction never starts the target\n");

    {
        HciTaktOs_t runtime = {};
        runtime.HostStarted = true;
        HciTaktOsHostDown(&runtime);
        assert(!runtime.HostStarted);
    }
    printf("[ok] runtime can return a failed host to start state\n");

    printf("All runtime pump tests passed.\n");
    return 0;
}
