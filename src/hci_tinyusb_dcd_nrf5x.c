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
 * The nRF USBD has one EasyDMA engine shared by every endpoint. TinyUSB
 * serializes access with _dcd.dma_running. A request that finds the engine
 * busy is normally deferred through the generic USBD task queue.
 *
 * A successful queue insertion is not sufficient for liveness. When a DMA
 * END interrupt releases dma_running, the same interrupt continues processing
 * endpoint events and can start a fresh DMA before the deferred callback gets
 * task time. Under continuous traffic the callback can run, find DMA busy,
 * defer itself again, and repeatedly lose the slot. That leaves an otherwise
 * healthy USB device with one CDC IN or OUT endpoint permanently waiting.
 *
 * Keep TinyUSB's DCD implementation, but retain only its EasyDMA retry
 * callbacks locally. The important difference from the previous scheduler is
 * where pending work is serviced: atomic_flag_clear() on dma_running is wrapped
 * so an already-waiting request gets first access at the exact DMA-release
 * point, before the ISR can start fresh endpoint work.
 *
 * Task-context deferral also attempts a bounded immediate kick to close the
 * race where DMA finishes just before the task records its pending request.
 * The real TinyUSB start functions still arbitrate on dma_running, so an
 * interrupt that wins between the probe and the start simply causes the
 * request to be retained again for the next release.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "tusb_option.h"
#include "device/usbd_pvt.h"

#if CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X

#define HCI_DCD_DMA_PENDING_COUNT 24U
#define HCI_DCD_OUT_REQUEST_BASE  1U

static void HciDcdDefer(osal_task_func_t Func, void *Param, bool InIsr);
static void HciDcdDmaRelease(volatile atomic_flag *Flag);

/*
 * stdatomic.h normally defines atomic_flag_clear as a macro. Replace only that
 * operation while compiling the installed DCD. atomic_flag_clear_explicit is
 * left untouched and is used by this integration when it needs a plain clear.
 */
#undef atomic_flag_clear
#define atomic_flag_clear(Flag) HciDcdDmaRelease(Flag)

/*
 * Rename the DCD entry points that need IOsonata wrappers, and divert only the
 * nRF5x EasyDMA retry helper away from TinyUSB's generic event queue.
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
#undef atomic_flag_clear

#define HCI_DCD_ERRATA_199_REG (*((volatile uint32_t *)0x40027C1CUL))

typedef struct
{
    volatile uint32_t IrqCount;
    volatile uint32_t Dma199Entry;
    volatile uint32_t Dma199Exit;
    volatile uint32_t PendingEntry;
    volatile uint32_t PendingExit;
    volatile uint32_t Inten;
    volatile uint32_t EventCause;
    volatile uint32_t EpStatus;
    volatile uint32_t EpDataStatus;
    volatile uint32_t EpInEnable;
    volatile uint32_t EpOutEnable;
    volatile uint32_t EpIn2Amount;
    volatile uint32_t EpOut2Amount;
    volatile uint32_t EpIn4Amount;
    volatile uint32_t EpOut4Amount;
    volatile uint32_t UsbEnable;
    volatile uint32_t UsbPullup;
    volatile uint32_t UsbRegStatus;
    volatile uint32_t HfclkStat;

    volatile uint32_t RetryDeferCount;
    volatile uint32_t RetryDedupCount;
    volatile uint32_t RetryReleaseRunCount;
    volatile uint32_t RetryTaskRunCount;
    volatile uint32_t RetryFallbackCount;
    volatile uint32_t RetryPendingHighWater;
    volatile uint32_t RetryOut2DeferCount;
    volatile uint32_t RetryIn2DeferCount;
    volatile uint32_t RetryLogIn4DeferCount;
} HciTinyUsbDcdDiag_t;

/* Intentionally global and volatile: inspect this symbol directly over SWD. */
volatile HciTinyUsbDcdDiag_t g_HciTinyUsbDcdDiag;

/*
 * A pending retry fits in one atomic word:
 *
 *   1 .. EP_ISO_NUM+1  xact_out_dma(epnum)
 *   peripheral address edpt_dma_start(task-register)
 *
 * USBD task registers are word aligned, so the two encodings cannot collide.
 * An endpoint can have only one TinyUSB transfer outstanding, therefore an
 * identical retry for the same endpoint/task is duplicate work rather than a
 * second transaction.
 */
static atomic_uintptr_t s_DmaPending[HCI_DCD_DMA_PENDING_COUNT];
static uint8_t s_DmaPendingNext;
static atomic_flag s_DmaTaskKickRunning = ATOMIC_FLAG_INIT;

static uintptr_t HciDcdEncodeRequest(osal_task_func_t Func, void *Param)
{
    const uintptr_t fn = (uintptr_t)Func;

    if (fn == (uintptr_t)(osal_task_func_t)(uintptr_t)xact_out_dma_wrapper)
    {
        const uintptr_t epnum = (uintptr_t)Param;
        if (epnum <= EP_ISO_NUM)
        {
            return HCI_DCD_OUT_REQUEST_BASE + epnum;
        }
        return 0U;
    }

    if (fn == (uintptr_t)(osal_task_func_t)(uintptr_t)edpt_dma_start)
    {
        const uintptr_t task = (uintptr_t)Param;
        if ((task & (sizeof(uint32_t) - 1U)) == 0U &&
            task > (HCI_DCD_OUT_REQUEST_BASE + EP_ISO_NUM))
        {
            return task;
        }
    }

    return 0U;
}

static uint32_t HciDcdPendingCount(void)
{
    uint32_t count = 0U;
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        if (atomic_load_explicit(&s_DmaPending[i], memory_order_acquire) != 0U)
        {
            count++;
        }
    }
    return count;
}

static void HciDcdPendingHighWaterUpdate(void)
{
    const uint32_t count = HciDcdPendingCount();
    uint32_t high = __atomic_load_n(&g_HciTinyUsbDcdDiag.RetryPendingHighWater,
                                    __ATOMIC_RELAXED);
    while (count > high &&
           !__atomic_compare_exchange_n(&g_HciTinyUsbDcdDiag.RetryPendingHighWater,
                                        &high,
                                        count,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
}

static bool HciDcdPend(uintptr_t Request)
{
    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        if (atomic_load_explicit(&s_DmaPending[i], memory_order_acquire) == Request)
        {
            __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryDedupCount,
                               1U, __ATOMIC_RELAXED);
            return true;
        }
    }

    for (size_t i = 0U; i < HCI_DCD_DMA_PENDING_COUNT; i++)
    {
        uintptr_t expected = 0U;
        if (atomic_compare_exchange_strong_explicit(&s_DmaPending[i],
                                                    &expected,
                                                    Request,
                                                    memory_order_release,
                                                    memory_order_relaxed))
        {
            HciDcdPendingHighWaterUpdate();
            return true;
        }

        if (expected == Request)
        {
            __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryDedupCount,
                               1U, __ATOMIC_RELAXED);
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

static void HciDcdRunRequest(uintptr_t Request)
{
    if (Request <= HCI_DCD_OUT_REQUEST_BASE + EP_ISO_NUM)
    {
        xact_out_dma((uint8_t)(Request - HCI_DCD_OUT_REQUEST_BASE));
    }
    else
    {
        edpt_dma_start((volatile uint32_t *)Request);
    }
}

/*
 * Called only after dma_running has been cleared. Take exactly one waiting
 * request. If it is EP0STATUS/EP0RCVOUT it releases the lock immediately and
 * the nested release hook naturally services the next pending request.
 */
static void HciDcdRunPendingAtRelease(void)
{
    const uintptr_t request = HciDcdTakePending();
    if (request == 0U)
    {
        return;
    }

    __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryReleaseRunCount,
                       1U, __ATOMIC_RELAXED);
    HciDcdRunRequest(request);
}

/*
 * Close the task/release lost-wakeup window without changing ownership rules.
 * Probe dma_running; if idle, return it to idle with a plain explicit clear and
 * try one retained request. If an ISR takes the engine between the clear and
 * the request, TinyUSB's own start function sees busy and retains the retry
 * again. The guard prevents that re-deferral from recursing in task context.
 */
static void HciDcdTaskKick(void)
{
    if (atomic_flag_test_and_set_explicit(&s_DmaTaskKickRunning,
                                          memory_order_acquire))
    {
        return;
    }

    if (!atomic_flag_test_and_set_explicit(&_dcd.dma_running,
                                           memory_order_acquire))
    {
        atomic_flag_clear_explicit(&_dcd.dma_running, memory_order_release);

        const uintptr_t request = HciDcdTakePending();
        if (request != 0U)
        {
            __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryTaskRunCount,
                               1U, __ATOMIC_RELAXED);
            HciDcdRunRequest(request);
        }
    }

    atomic_flag_clear_explicit(&s_DmaTaskKickRunning, memory_order_release);
}

static void HciDcdDmaRelease(volatile atomic_flag *Flag)
{
    /* Perform exactly the clear TinyUSB requested. */
    atomic_flag_clear_explicit(Flag, memory_order_seq_cst);

    if (Flag == &_dcd.dma_running)
    {
        HciDcdRunPendingAtRelease();
    }
}

static void HciDcdDefer(osal_task_func_t Func, void *Param, bool InIsr)
{
    const uintptr_t request = HciDcdEncodeRequest(Func, Param);

    __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryDeferCount,
                       1U, __ATOMIC_RELAXED);

    if (request == HCI_DCD_OUT_REQUEST_BASE + 2U)
    {
        __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryOut2DeferCount,
                           1U, __ATOMIC_RELAXED);
    }
    else if (request == (uintptr_t)&NRF_USBD->TASKS_STARTEPIN[2])
    {
        __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryIn2DeferCount,
                           1U, __ATOMIC_RELAXED);
    }
    else if (request == (uintptr_t)&NRF_USBD->TASKS_STARTEPIN[4])
    {
        __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryLogIn4DeferCount,
                           1U, __ATOMIC_RELAXED);
    }

    if (request != 0U && HciDcdPend(request))
    {
        if (!InIsr)
        {
            HciDcdTaskKick();
        }
        return;
    }

    /* Unknown future callback or an exhausted local table: preserve upstream. */
    __atomic_add_fetch(&g_HciTinyUsbDcdDiag.RetryFallbackCount,
                       1U, __ATOMIC_RELAXED);
    usbd_defer_func(Func, Param, InIsr);
}

static uint32_t HciTinyUsbDcdPendingEvents(void)
{
    uint32_t pending = 0U;
    volatile uint32_t *event = &NRF_USBD->EVENTS_USBRESET;

    for (uint32_t i = 0U; i <= USBD_INTEN_EPDATA_Pos; i++)
    {
        if (event[i] != 0U)
        {
            pending |= 1UL << i;
        }
    }

    return pending;
}

static void HciTinyUsbDcdSnapshot(void)
{
    g_HciTinyUsbDcdDiag.Inten = NRF_USBD->INTEN;
    g_HciTinyUsbDcdDiag.EventCause = NRF_USBD->EVENTCAUSE;
    g_HciTinyUsbDcdDiag.EpStatus = NRF_USBD->EPSTATUS;
    g_HciTinyUsbDcdDiag.EpDataStatus = NRF_USBD->EPDATASTATUS;
    g_HciTinyUsbDcdDiag.EpInEnable = NRF_USBD->EPINEN;
    g_HciTinyUsbDcdDiag.EpOutEnable = NRF_USBD->EPOUTEN;
    g_HciTinyUsbDcdDiag.EpIn2Amount = NRF_USBD->EPIN[2].AMOUNT;
    g_HciTinyUsbDcdDiag.EpOut2Amount = NRF_USBD->EPOUT[2].AMOUNT;
    g_HciTinyUsbDcdDiag.EpIn4Amount = NRF_USBD->EPIN[4].AMOUNT;
    g_HciTinyUsbDcdDiag.EpOut4Amount = NRF_USBD->EPOUT[4].AMOUNT;
    g_HciTinyUsbDcdDiag.UsbEnable = NRF_USBD->ENABLE;
    g_HciTinyUsbDcdDiag.UsbPullup = NRF_USBD->USBPULLUP;
    g_HciTinyUsbDcdDiag.UsbRegStatus = NRF_POWER->USBREGSTATUS;
    g_HciTinyUsbDcdDiag.HfclkStat = NRF_CLOCK->HFCLKSTAT;
}

void dcd_int_handler(uint8_t rhport)
{
    const bool reset =
        (NRF_USBD->INTEN & USBD_INTEN_USBRESET_Msk) != 0U &&
        NRF_USBD->EVENTS_USBRESET != 0U;

    if (reset)
    {
        HciDcdClearPending();
    }

    g_HciTinyUsbDcdDiag.IrqCount++;
    g_HciTinyUsbDcdDiag.Dma199Entry = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingEntry = HciTinyUsbDcdPendingEvents();

    HciTinyUsbDcdIntHandler(rhport);

    g_HciTinyUsbDcdDiag.Dma199Exit = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingExit = HciTinyUsbDcdPendingEvents();
    HciTinyUsbDcdSnapshot();
}

void dcd_edpt_close_all(uint8_t rhport)
{
    HciDcdClearPending();
    HciTinyUsbDcdEdptCloseAll(rhport);
}

void dcd_disconnect(uint8_t rhport)
{
    HciDcdClearPending();
    HciTinyUsbDcdDisconnect(rhport);
}

#endif
