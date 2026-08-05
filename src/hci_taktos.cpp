/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_taktos.h"

#include <string.h>

#include "TaktOSCriticalSection.h"
#include "hci_trace.h"

static bool HciTaktOsIsInIsr(void)
{
#if defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH)
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
    return ipsr != 0U;
#else
    return false;
#endif
}

static uint32_t HciTaktOsTakeEvents(HciTaktOs_t *pRuntime)
{
    uint32_t state = TaktOSEnterCritical();
    uint32_t events = pRuntime->PendingEvents;
    pRuntime->PendingEvents = 0U;
    TaktOSExitCritical(state);
    return events;
}

bool HciTaktOsInit(HciTaktOs_t *pRuntime,
                   const HciTaktOsOps_t *pOps,
                   const HciTaktOsHostOps_t *pHostOps)
{
    if (pRuntime == nullptr || pOps == nullptr || pHostOps == nullptr ||
        pOps->Start == nullptr || pOps->ProcessMpsl == nullptr ||
        pHostOps->Start == nullptr || pHostOps->Process == nullptr)
    {
        return false;
    }

    memset(pRuntime, 0, sizeof(*pRuntime));
    pRuntime->Ops = *pOps;
    pRuntime->HostOps = *pHostOps;

    if (TaktOSSemInit(&pRuntime->StoppedSem, 0U, 1U) != TAKTOS_OK)
    {
        return false;
    }

    return TaktOSSemInit(&pRuntime->WakeSem, 0U, 1U) == TAKTOS_OK;
}

void HciTaktOsWake(HciTaktOs_t *pRuntime, uint32_t Events)
{
    if (pRuntime == nullptr || Events == 0U)
    {
        return;
    }

    uint32_t state = TaktOSEnterCritical();
    pRuntime->PendingEvents |= Events;
    pRuntime->WakeCount++;
    TaktOSExitCritical(state);

    TaktOSErr_t result = TaktOSSemGive(&pRuntime->WakeSem, HciTaktOsIsInIsr());
    if (result == TAKTOS_ERR_FULL)
    {
        state = TaktOSEnterCritical();
        pRuntime->SemaphoreFullCount++;
        TaktOSExitCritical(state);
    }
}

void HciTaktOsStop(HciTaktOs_t *pRuntime)
{
    if (pRuntime == nullptr)
    {
        return;
    }

    uint32_t state = TaktOSEnterCritical();
    pRuntime->StopRequested = true;
    TaktOSExitCritical(state);

    HciTaktOsWake(pRuntime, HCI_TAKTOS_EVENT_STOP);
}

bool HciTaktOsWaitStopped(HciTaktOs_t *pRuntime, uint32_t TimeoutMs)
{
    if (pRuntime == nullptr)
    {
        return true;
    }

    if (!pRuntime->Started)
    {
        /*
         * The thread never reached its loop, so there is nothing inside SDC or
         * MPSL to wait for.
         */
        return true;
    }

    /*
     * The caller is about to tear down SDC and MPSL, which the runtime thread
     * calls into, so it has to be out of its loop first. The thread gives
     * StoppedSem as it leaves.
     */
    return TaktOSSemTake(&pRuntime->StoppedSem, true, TimeoutMs) == TAKTOS_OK;
}

/*
 * Pump the host, bringing it up first if an earlier attempt failed. MPSL is
 * serviced either way, which is the reason the loop keeps running at all.
 */
static void HciTaktOsServiceHost(HciTaktOs_t *pRuntime)
{
    if (!pRuntime->HostStarted)
    {
        pRuntime->HostStarted =
            pRuntime->HostOps.Start(pRuntime->HostOps.pContext);
        if (!pRuntime->HostStarted)
        {
            pRuntime->HostRetryCount++;
            return;
        }
        HciTrace("runtime: host up on retry\r\n");
    }

    pRuntime->HostOps.Process(pRuntime->HostOps.pContext);
}

void HciTaktOsThread(void *pContext)
{
    HciTaktOs_t *pRuntime = static_cast<HciTaktOs_t *>(pContext);
    if (pRuntime == nullptr)
    {
        return;
    }

    HciTrace("runtime: target start\r\n");
    if (!pRuntime->Ops.Start(pRuntime->Ops.pContext))
    {
        HciTrace("runtime: target start failed\r\n");
        pRuntime->StartErrorCount++;
        if (pRuntime->Ops.Fault != nullptr)
        {
            pRuntime->Ops.Fault(pRuntime->Ops.pContext, -1);
        }
        return;
    }

    /*
     * The target is up from here, which means MPSL and the radio are running
     * and mpsl_low_priority_process has a deadline. Leaving this function
     * would stop the only caller of it, so a host that fails to start marks
     * the host down and the loop below is entered anyway. The host is retried
     * on every pass, so a cable arriving late still brings it up.
     */
    HciTrace("runtime: host start\r\n");
    pRuntime->HostStarted = pRuntime->HostOps.Start(pRuntime->HostOps.pContext);
    if (!pRuntime->HostStarted)
    {
        HciTrace("runtime: host start failed, servicing mpsl without it\r\n");
        pRuntime->StartErrorCount++;
        if (pRuntime->Ops.Fault != nullptr)
        {
            pRuntime->Ops.Fault(pRuntime->Ops.pContext, -1);
        }
    }

    HciTrace("runtime: started\r\n");
    pRuntime->Started = true;
    pRuntime->Running = true;
    HciTaktOsWake(pRuntime, HCI_TAKTOS_EVENT_HOST);

    const uint32_t waitTicks = pRuntime->HostOps.PollIntervalMs != 0U ?
                               pRuntime->HostOps.PollIntervalMs :
                               TAKTOS_WAIT_FOREVER;

    while (!pRuntime->StopRequested)
    {
        if (TaktOSSemTake(&pRuntime->WakeSem, true, waitTicks) != TAKTOS_OK)
        {
            if (pRuntime->HostOps.PollIntervalMs == 0U)
            {
                continue;
            }

            /*
             * Timed out with no wake. Pump the host anyway, a lost wake must
             * not be able to stall the transport.
             */
            pRuntime->PollWakeCount++;
            HciTaktOsServiceHost(pRuntime);
            continue;
        }

        for (;;)
        {
            uint32_t events = HciTaktOsTakeEvents(pRuntime);
            if (events == 0U)
            {
                pRuntime->EmptyWakeCount++;
            }

            if ((events & HCI_TAKTOS_EVENT_MPSL) != 0U)
            {
                pRuntime->Ops.ProcessMpsl(pRuntime->Ops.pContext);
                pRuntime->MpslProcessCount++;
            }

            HciTaktOsServiceHost(pRuntime);
            pRuntime->LoopCount++;

            if ((events & HCI_TAKTOS_EVENT_STOP) != 0U || pRuntime->StopRequested)
            {
                break;
            }

            if (TaktOSSemTake(&pRuntime->WakeSem, false, TAKTOS_NO_WAIT) != TAKTOS_OK)
            {
                break;
            }
        }
    }

    pRuntime->Running = false;

    /* Release anything waiting to tear the target down behind us. */
    (void)TaktOSSemGive(&pRuntime->StoppedSem, false);
}
