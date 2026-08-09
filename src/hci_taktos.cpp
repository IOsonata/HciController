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

/*
 * The thread side of the pending word. A critical section is right here and
 * not in HciTaktOsWake: this runs on the thread, where masking briefly is
 * ordinary, and masking is what makes the read and the clear one operation
 * against the interrupt that sets bits.
 */
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

    /*
     * This is the one function here an interrupt reaches: the USB one through
     * the device stack's event hook, and the MPSL one. Keep the pending-word
     * update atomic so this path does not need a second explicit critical
     * section before TaktOSSemGive performs its own short kernel-protected
     * update.
     *
     * The pair still holds. HciTaktOsTakeEvents masks while it reads the word
     * and clears it, so no interrupt can land between those two, and the
     * fetch_or here is indivisible against anything. Release, so whatever was
     * written before the bit goes in is in front of it.
     */
    const uint32_t was =
        __atomic_fetch_or(&pRuntime->PendingEvents, Events, __ATOMIC_RELEASE);
    __atomic_fetch_add(&pRuntime->WakeCount, 1U, __ATOMIC_RELAXED);

    /*
     * Nothing more to do when the thread had already been told. Whoever set
     * the bit first gave the semaphore and the thread has not taken the word
     * since, or it would be clear, so the thread is already on its way and
     * will find this event with the one that is already there.
     *
     * Worth its own branch because the alternative is one semaphore call per
     * accepted USB event, several per host packet, from an interrupt, and
     * nearly all of them return full: WakeSem holds one. Under load that is
     * the largest thing this interrupt does, and an interrupt that takes too
     * long is the thing to remove before looking anywhere else.
     */
    if ((was & Events) == Events)
    {
        __atomic_fetch_add(&pRuntime->WakeFoldCount, 1U, __ATOMIC_RELAXED);
        return;
    }

    TaktOSErr_t result = TaktOSSemGive(&pRuntime->WakeSem, HciTaktOsIsInIsr());
    if (result == TAKTOS_ERR_FULL)
    {
        __atomic_fetch_add(&pRuntime->SemaphoreFullCount, 1U, __ATOMIC_RELAXED);
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

    if (!pRuntime->ThreadLive)
    {
        /*
         * The thread body was never entered, or has already left, so there is
         * nothing inside SDC or MPSL to wait for. Gating on Started instead
         * would short circuit for the whole of bring up, which is exactly when
         * the thread is deepest inside mpsl_low_priority_process.
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

static void HciTaktOsThreadLeave(HciTaktOs_t *pRuntime)
{
    pRuntime->Running = false;
    pRuntime->ThreadLive = false;

    /*
     * Wake a stop that began while the thread was still live. Giving the
     * binary semaphore after ThreadLive is cleared also makes a later
     * HciTaktOsWaitStopped return immediately without depending on the token.
     */
    (void)TaktOSSemGive(&pRuntime->StoppedSem, false);
}

void HciTaktOsThread(void *pContext)
{
    HciTaktOs_t *pRuntime = static_cast<HciTaktOs_t *>(pContext);
    if (pRuntime == nullptr)
    {
        return;
    }

    /*
     * Set before anything is brought up. From here on a stop has to wait for
     * this thread, because everything below calls into MPSL and SDC.
     */
    pRuntime->ThreadLive = true;

    HciTrace("runtime: target start\r\n");
    if (!pRuntime->Ops.Start(pRuntime->Ops.pContext))
    {
        HciTrace("runtime: target start failed\r\n");
        pRuntime->StartErrorCount++;
        if (pRuntime->Ops.Fault != nullptr)
        {
            pRuntime->Ops.Fault(pRuntime->Ops.pContext, -1);
        }
        HciTaktOsThreadLeave(pRuntime);
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

    HciTaktOsThreadLeave(pRuntime);
}
