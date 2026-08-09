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
 * nothing above it is told.
 *
 * Intercept only osal_queue_send() while compiling the installed usbd.c. The
 * real OSAL function still performs the send and its return value is passed
 * through unchanged. tud_task_ext() reports queue and DCD state from task
 * context at low rate, never from USBD_IRQHandler.
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

extern void HciTinyUsbDcdTrace(void);

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

#define osal_queue_send HciTinyUsbQueueSend
#define tud_task_ext HciTinyUsbUsbdTaskExt
#include "device/usbd.c"
#undef tud_task_ext
#undef osal_queue_send

#ifndef HCI_TINYUSB_DIAG_TASK_INTERVAL
#define HCI_TINYUSB_DIAG_TASK_INTERVAL 400U
#endif

static uint32_t s_HciTinyUsbQueueFailReported;
static uint32_t s_HciTinyUsbDiagTaskCount;

static uint32_t HciTinyUsbQueueDepth(void)
{
    if (_usbd_q == NULL || _usbd_q->item_size == 0U)
    {
        return 0U;
    }

    return (uint32_t)tu_fifo_count(&_usbd_q->ff) /
           (uint32_t)_usbd_q->item_size;
}

void tud_task_ext(uint32_t TimeoutMs, bool InIsr)
{
    HciTinyUsbUsbdTaskExt(TimeoutMs, InIsr);

    const uint32_t failed =
        __atomic_load_n(&g_HciTinyUsbQueueFailCount, __ATOMIC_RELAXED);
    const bool newFailure = failed != s_HciTinyUsbQueueFailReported;

    s_HciTinyUsbDiagTaskCount++;
    bool periodic = false;
    if (s_HciTinyUsbDiagTaskCount >= HCI_TINYUSB_DIAG_TASK_INTERVAL)
    {
        s_HciTinyUsbDiagTaskCount = 0U;
        periodic = true;
    }

    if (!periodic && !newFailure)
    {
        return;
    }

    const uint32_t sent =
        __atomic_load_n(&g_HciTinyUsbQueueSendCount, __ATOMIC_RELAXED);
    const uint32_t high =
        __atomic_load_n(&g_HciTinyUsbQueueHighWater, __ATOMIC_RELAXED);
    const uint32_t depth = HciTinyUsbQueueDepth();

    HciTrace("usbqs: send=%lu fail=%lu high=%lu depth=%lu\r\n",
             (unsigned long)sent,
             (unsigned long)failed,
             (unsigned long)high,
             (unsigned long)depth);

    if (newFailure)
    {
        const uint32_t failDepth =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailDepth,
                            __ATOMIC_RELAXED);
        const uint32_t eventId =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailEvent,
                            __ATOMIC_RELAXED);
        const uint32_t inIsr =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailInIsr,
                            __ATOMIC_RELAXED);
        const uint32_t ep =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailEp,
                            __ATOMIC_RELAXED);
        const uint32_t len =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailLen,
                            __ATOMIC_RELAXED);
        const uintptr_t func =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailFunc,
                            __ATOMIC_RELAXED);
        const uintptr_t param =
            __atomic_load_n(&g_HciTinyUsbQueueLastFailParam,
                            __ATOMIC_RELAXED);

        HciTrace("usbqf: depth=%lu evt=%lu isr=%lu ep=0x%02lX len=%lu\r\n",
                 (unsigned long)failDepth,
                 (unsigned long)eventId,
                 (unsigned long)inIsr,
                 (unsigned long)ep,
                 (unsigned long)len);
        HciTrace("usbqfp: fn=0x%08lX arg=0x%08lX\r\n",
                 (unsigned long)func,
                 (unsigned long)param);

        s_HciTinyUsbQueueFailReported = failed;
    }

    HciTinyUsbDcdTrace();
}

#endif
