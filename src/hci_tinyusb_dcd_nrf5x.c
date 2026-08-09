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
 * nRF52840 TinyUSB DCD integration.
 *
 * The nRF USBD has one EasyDMA engine shared by every endpoint. TinyUSB's
 * nRF5x DCD protects it with dma_running. When the engine is busy, upstream
 * retries edpt_dma_start() and xact_out_dma() by putting function calls into
 * the generic USBD event queue.
 *
 * That queue also carries bus and transfer-completion events. With more than
 * one CDC endpoint active, a retry can run while DMA is still busy and put
 * itself back into the same finite queue. Losing a retry means a transfer
 * never starts. Losing a completion means an endpoint stays busy. Either
 * failure leaves an enumerated USB device that no longer moves traffic.
 *
 * Keep TinyUSB's nRF5x DCD as the implementation, but intercept only those DMA
 * retry deferrals. They are retained here in a bounded, de-duplicating table.
 * ISR-context retries are serviced at the tail of the USBD interrupt, after
 * TinyUSB has processed the END event that releases dma_running. Task-context
 * retries also kick the table immediately; this closes the race where END can
 * happen between observing dma_running busy and recording the pending retry.
 *
 * The Eclipse project links this file in place of external/tinyusb's
 * dcd_nrf5x.c. The installed TinyUSB source is included below, so there is one
 * DCD implementation in the image and no source patch has to be applied to the
 * external checkout.
 */

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "tusb_option.h"
#include "device/usbd_pvt.h"

#if CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X

#define HCI_DCD_DMA_PENDING_COUNT 24U
#define HCI_DCD_OUT_REQUEST_BASE  1U

static void HciDcdDefer(osal_task_func_t func, void *param, bool in_isr);
static void HciDcdRunPending(void);

/*
 * Rename the functions that need an IOsonata wrapper while compiling the
 * installed TinyUSB implementation into this translation unit.
 */
#define usbd_defer_func    HciDcdDefer
#define dcd_int_handler    HciTinyUsbDcdIntHandler
#define dcd_disconnect     HciTinyUsbDcdDisconnect
#define dcd_edpt_close_all HciTinyUsbDcdEdptCloseAll
#include "portable/nordic/nrf5x/dcd_nrf5x.c"
#undef dcd_edpt_close_all
#undef dcd_disconnect
#undef dcd_int_handler
#undef usbd_defer_func

/*
 * A pending request fits in one atomic word:
 *
 *   1..EP_ISO_NUM+1  xact_out_dma(epnum)
 *   peripheral addr  edpt_dma_start(task-register)
 *
 * USBD task registers are word aligned in the peripheral address space, so the
 * encodings cannot collide. TinyUSB does not legitimately start a second
 * transfer on an endpoint it still owns, so de-duplicating an identical retry
 * loses no work and prevents one busy endpoint from creating an unbounded
 * retry stream.
 */
static atomic_uintptr_t s_DmaPending[HCI_DCD_DMA_PENDING_COUNT];
static uint8_t s_DmaPendingNext;

static uintptr_t HciDcdEncodeRequest(osal_task_func_t func, void *param)
{
    const uintptr_t fn = (uintptr_t)func;

    if (fn == (uintptr_t)(osal_task_func_t)(uintptr_t)xact_out_dma_wrapper)
    {
        const uintptr_t epnum = (uintptr_t)param;
        if (epnum <= EP_ISO_NUM)
        {
            return HCI_DCD_OUT_REQUEST_BASE + epnum;
        }
        return 0U;
    }

    if (fn == (uintptr_t)(osal_task_func_t)(uintptr_t)edpt_dma_start)
    {
        const uintptr_t task = (uintptr_t)param;

        if ((task & (sizeof(uint32_t) - 1U)) == 0U &&
            task > (HCI_DCD_OUT_REQUEST_BASE + EP_ISO_NUM))
        {
            return task;
        }
    }

    return 0U;
}

static bool HciDcdPend(uintptr_t request)
{
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        const uintptr_t pending =
            atomic_load_explicit(&s_DmaPending[i], memory_order_acquire);

        if (pending == request)
        {
            return true;
        }
    }

    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        uintptr_t expected = 0U;
        if (atomic_compare_exchange_strong_explicit(&s_DmaPending[i],
                                                    &expected,
                                                    request,
                                                    memory_order_release,
                                                    memory_order_relaxed))
        {
            return true;
        }

        /*
         * An interrupt may have inserted the same request between the first
         * scan and this one.
         */
        if (expected == request)
        {
            return true;
        }
    }

    return false;
}

static uintptr_t HciDcdTakePending(void)
{
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        const uint8_t slot =
            (uint8_t)((s_DmaPendingNext + i) % HCI_DCD_DMA_PENDING_COUNT);

        const uintptr_t request =
            atomic_exchange_explicit(&s_DmaPending[slot],
                                     0U,
                                     memory_order_acq_rel);
        if (request != 0U)
        {
            s_DmaPendingNext =
                (uint8_t)((slot + 1U) % HCI_DCD_DMA_PENDING_COUNT);
            return request;
        }
    }

    return 0U;
}

static void HciDcdClearPending(void)
{
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        atomic_store_explicit(&s_DmaPending[i], 0U, memory_order_release);
    }
    s_DmaPendingNext = 0U;
}

static bool HciDcdDmaIdle(void)
{
    /*
     * C11 atomic_flag has no load operation. Probe it without changing its
     * final state. If an interrupt starts another DMA around this probe, the
     * real TinyUSB start routine re-checks dma_running and re-pends the request.
     */
    if (atomic_flag_test_and_set_explicit(&_dcd.dma_running,
                                          memory_order_acquire))
    {
        return false;
    }

    atomic_flag_clear_explicit(&_dcd.dma_running, memory_order_release);
    return true;
}

static void HciDcdRunRequest(uintptr_t request)
{
    if (request <= HCI_DCD_OUT_REQUEST_BASE + EP_ISO_NUM)
    {
        xact_out_dma((uint8_t)(request - HCI_DCD_OUT_REQUEST_BASE));
    }
    else
    {
        edpt_dma_start((volatile uint32_t *)request);
    }
}

static void HciDcdRunPending(void)
{
    /*
     * Usually one request starts a real DMA and the next idle probe stops the
     * loop. EP0STATUS and EP0RCVOUT release TinyUSB's DMA flag immediately, so
     * continue in that case rather than making the next request depend on an
     * unrelated interrupt.
     */
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        if (!HciDcdDmaIdle())
        {
            break;
        }

        const uintptr_t request = HciDcdTakePending();
        if (request == 0U)
        {
            break;
        }

        HciDcdRunRequest(request);
    }
}

static void HciDcdDefer(osal_task_func_t func, void *param, bool in_isr)
{
    const uintptr_t request = HciDcdEncodeRequest(func, param);

    if (request != 0U && HciDcdPend(request))
    {
        /*
         * Lost-wakeup closure for task context:
         *
         *   task sees dma_running busy
         *   END interrupt preempts, clears dma_running, finds no pending work
         *   task resumes and records this request
         *
         * Without this kick there may be no later USB interrupt to service the
         * request, especially for OUT where the host packet is already sitting
         * in the endpoint waiting for EasyDMA. In ISR context the outer wrapper
         * drains the table when TinyUSB's handler returns, so do not recurse
         * into the scheduler from inside the DCD interrupt path.
         */
        if (!in_isr)
        {
            HciDcdRunPending();
        }
        return;
    }

    /*
     * Only the nRF5x EasyDMA retry callbacks are expected here. Preserve
     * TinyUSB behavior for an unexpected future callback, or if the bounded
     * table is ever exhausted, rather than dropping work silently.
     */
    usbd_defer_func(func, param, in_isr);
}

void dcd_int_handler(uint8_t rhport)
{
    /*
     * TinyUSB clears EVENTS_USBRESET at the top of its handler, so remember it
     * first. A reset invalidates every request retained for the old endpoint
     * state.
     */
    const bool reset =
        (NRF_USBD->INTEN & USBD_INTEN_USBRESET_Msk) != 0U &&
        NRF_USBD->EVENTS_USBRESET != 0U;

    HciTinyUsbDcdIntHandler(rhport);

    if (reset)
    {
        HciDcdClearPending();
    }
    else
    {
        HciDcdRunPending();
    }
}

void dcd_edpt_close_all(uint8_t rhport)
{
    HciTinyUsbDcdEdptCloseAll(rhport);
    HciDcdClearPending();
}

void dcd_disconnect(uint8_t rhport)
{
    HciTinyUsbDcdDisconnect(rhport);
    HciDcdClearPending();
}

#endif
