/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * TinyUSB device-stack integration used to observe its event queue without
 * changing queue depth, drain rate or scheduling.
 *
 * The nRF5x DCD uses the same queue for transfer completions and for deferred
 * EasyDMA retries. queue_event() discards either one when osal_queue_send()
 * returns false; in a release build TU_ASSERT returns from queue_event() and
 * nothing above it is told. A lost completion leaves an endpoint busy. A lost
 * function call can leave a packet in the peripheral with no DMA start left to
 * consume it.
 *
 * Intercept only osal_queue_send() while compiling the installed usbd.c. The
 * real OSAL function still performs the send and its return value is passed
 * through unchanged. The tud_task_ext() wrapper reports a failure later from
 * task context, never from the USB interrupt.
 */

#include <stdbool.h>
#include <stdint.h>

#include "tusb_option.h"
#include "device/dcd.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "hci_trace.h"

#if CFG_TUD_ENABLED

volatile uint32_t g_HciTinyUsbQueueSendCount;
volatile uint32_t g_HciTinyUsbQueueFailCount;
volatile uint32_t g_HciTinyUsbQueueHighWater;
volatile uint32_t g_HciTinyUsbQueueLastFailDepth;
volatile uint32_t g_HciTinyUsbQueueLastFailEvent;
volatile uint32_t g_HciTinyUsbQueueLastFailInIsr;
volatile uint32_t g_HciTinyUsbQueueLastFailEp;
volatile uint32_t g_HciTinyUsbQueueLastFailLen;
volatile uintptr_t g_HciTinyUsbQueueLastFailFunc;
volatile uintptr_t g_HciTinyUsbQueueLastFailParam;

static void HciTinyUsbQueueHighWaterUpdate(uint32_t Depth)
{
    uint32_t high = __atomic_load_n(&g_HciTinyUsbQueueHighWater,
                                    __ATOMIC_RELAXED);

    while (Depth > high &&
           !__atomic_compare_exchange_n(&g_HciTinyUsbQueueHighWater,
                                        &high,
                                        Depth,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
}

static bool HciTinyUsbQueueSend(osal_queue_t Queue,
                                const void *pData,
                                bool InIsr)
{
    const bool success = osal_queue_send(Queue, pData, InIsr);

    __atomic_add_fetch(&g_HciTinyUsbQueueSendCount, 1U, __ATOMIC_RELAXED);

    uint32_t depth = 0U;
    if (Queue != NULL && Queue->item_size != 0U)
    {
        depth = (uint32_t)tu_fifo_count(&Queue->ff) /
                (uint32_t)Queue->item_size;
    }
    HciTinyUsbQueueHighWaterUpdate(depth);

    if (!success)
    {
        const dcd_event_t *event = (const dcd_event_t *)pData;
        uint32_t eventId = DCD_EVENT_INVALID;
        uint32_t ep = 0U;
        uint32_t len = 0U;
        uintptr_t func = 0U;
        uintptr_t param = 0U;

        if (event != NULL)
        {
            eventId = event->event_id;
            if (eventId == DCD_EVENT_XFER_COMPLETE)
            {
                ep = event->xfer_complete.ep_addr;
                len = event->xfer_complete.len;
            }
            else if (eventId == USBD_EVENT_FUNC_CALL)
            {
                func = (uintptr_t)event->func_call.func;
                param = (uintptr_t)event->func_call.param;
            }
        }

        __atomic_store_n(&g_HciTinyUsbQueueLastFailDepth, depth,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailEvent, eventId,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailInIsr,
                         InIsr ? 1U : 0U, __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailEp, ep,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailLen, len,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailFunc, func,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_HciTinyUsbQueueLastFailParam, param,
                         __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_HciTinyUsbQueueFailCount, 1U,
                           __ATOMIC_RELAXED);
    }

    return success;
}

/*
 * The real declarations above are already parsed before these substitutions,
 * so the wrappers can call the installed functions while usbd.c itself sees
 * only the diagnostic names.
 */
#define osal_queue_send HciTinyUsbQueueSend
#define tud_task_ext HciTinyUsbUsbdTaskExt
#include "device/usbd.c"
#undef tud_task_ext
#undef osal_queue_send

static uint32_t s_HciTinyUsbQueueFailReported;

void tud_task_ext(uint32_t TimeoutMs, bool InIsr)
{
    HciTinyUsbUsbdTaskExt(TimeoutMs, InIsr);

    const uint32_t failed =
        __atomic_load_n(&g_HciTinyUsbQueueFailCount, __ATOMIC_RELAXED);
    if (failed == s_HciTinyUsbQueueFailReported)
    {
        return;
    }

    const uint32_t sent =
        __atomic_load_n(&g_HciTinyUsbQueueSendCount, __ATOMIC_RELAXED);
    const uint32_t high =
        __atomic_load_n(&g_HciTinyUsbQueueHighWater, __ATOMIC_RELAXED);
    const uint32_t depth =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailDepth, __ATOMIC_RELAXED);
    const uint32_t eventId =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailEvent, __ATOMIC_RELAXED);
    const uint32_t inIsr =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailInIsr, __ATOMIC_RELAXED);
    const uint32_t ep =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailEp, __ATOMIC_RELAXED);
    const uint32_t len =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailLen, __ATOMIC_RELAXED);
    const uintptr_t func =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailFunc, __ATOMIC_RELAXED);
    const uintptr_t param =
        __atomic_load_n(&g_HciTinyUsbQueueLastFailParam, __ATOMIC_RELAXED);

    HciTrace("usbq: fail=%lu send=%lu high=%lu depth=%lu evt=%lu isr=%lu\r\n",
             (unsigned long)failed,
             (unsigned long)sent,
             (unsigned long)high,
             (unsigned long)depth,
             (unsigned long)eventId,
             (unsigned long)inIsr);
    HciTrace("usbqf: ep=0x%02lX len=%lu fn=0x%08lX arg=0x%08lX\r\n",
             (unsigned long)ep,
             (unsigned long)len,
             (unsigned long)func,
             (unsigned long)param);

    s_HciTinyUsbQueueFailReported = failed;
}

#endif
