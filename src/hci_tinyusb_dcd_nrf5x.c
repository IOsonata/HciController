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
 * nRF52840 TinyUSB DCD integration used for post-mortem diagnostics.
 *
 * Keep the installed TinyUSB nRF5x DCD scheduling unchanged. The wrapper
 * records the state needed to distinguish an OUT endpoint which is armed in
 * the DCD from one whose TinyUSB core busy state has diverged from the DCD.
 *
 * The previous local EasyDMA scheduler was removed after it failed to change
 * the intermittent EP2 OUT stall. The remaining diagnostics are passive:
 * dcd_edpt_xfer() and usbd_defer_func() are counted and then passed straight
 * through, while dcd_int_handler() snapshots the stock DCD state.
 */

#include <stdbool.h>
#include <stdint.h>

#include "tusb_option.h"
#include "device/usbd_pvt.h"
#include "hci_trace.h"

#if CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X

static void HciDcdDefer(osal_task_func_t Func, void *Param, bool InIsr);

#define usbd_defer_func HciDcdDefer
#define dcd_int_handler HciTinyUsbDcdIntHandler
#define dcd_edpt_xfer HciTinyUsbDcdEdptXfer
#include "portable/nordic/nrf5x/dcd_nrf5x.c"
#undef dcd_edpt_xfer
#undef dcd_int_handler
#undef usbd_defer_func

#define HCI_DCD_ERRATA_199_REG (*((volatile uint32_t *)0x40027C1CUL))

typedef struct
{
    volatile uint32_t IrqCount;
    volatile uint32_t Dma199Entry;
    volatile uint32_t Dma199Exit;
    volatile uint32_t Dma199Now;
    volatile uint32_t PendingEntry;
    volatile uint32_t PendingExit;
    volatile uint32_t Inten;
    volatile uint32_t EventCause;
    volatile uint32_t EpStatus;
    volatile uint32_t EpDataStatus;
    volatile uint32_t EpInEnable;
    volatile uint32_t EpOutEnable;
    volatile uint32_t UsbEnable;
    volatile uint32_t UsbPullup;
    volatile uint32_t UsbRegStatus;
    volatile uint32_t HfclkStat;

    volatile uint32_t Ep2OutArmCount;
    volatile uint32_t Ep2OutArmFailCount;
    volatile uint32_t Ep2OutArmWhileStartedCount;
    volatile uint32_t Ep2OutArmWithDataCount;
    volatile uint32_t Ep2OutEpdataCount;
    volatile uint32_t Ep2OutEndCount;
    volatile uint32_t DeferCount;
    volatile uint32_t DeferIsrCount;
    volatile uint32_t DeferTaskCount;
    volatile uint32_t DeferOut2Count;
    volatile uint32_t DeferIn2Count;
    volatile uint32_t DeferLogIn4Count;

    volatile uint32_t Ep2OutStarted;
    volatile uint32_t Ep2OutDataReceived;
    volatile uint32_t Ep2OutActualLen;
    volatile uint32_t Ep2OutTotalLen;
    volatile uint32_t Ep2OutMps;
    volatile uint32_t Ep2OutSize;
    volatile uint32_t Ep2OutAmount;
    volatile uint32_t Ep2OutPtr;
    volatile uint32_t Ep2OutMaxCnt;
    volatile uint32_t Ep2OutBuffer;
} HciTinyUsbDcdDiag_t;

volatile HciTinyUsbDcdDiag_t g_HciTinyUsbDcdDiag;

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
    const xfer_td_t *xfer = get_td(2U, TUSB_DIR_OUT);

    g_HciTinyUsbDcdDiag.Dma199Now = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.Inten = NRF_USBD->INTEN;
    g_HciTinyUsbDcdDiag.EventCause = NRF_USBD->EVENTCAUSE;
    g_HciTinyUsbDcdDiag.EpStatus = NRF_USBD->EPSTATUS;
    g_HciTinyUsbDcdDiag.EpDataStatus = NRF_USBD->EPDATASTATUS;
    g_HciTinyUsbDcdDiag.EpInEnable = NRF_USBD->EPINEN;
    g_HciTinyUsbDcdDiag.EpOutEnable = NRF_USBD->EPOUTEN;
    g_HciTinyUsbDcdDiag.UsbEnable = NRF_USBD->ENABLE;
    g_HciTinyUsbDcdDiag.UsbPullup = NRF_USBD->USBPULLUP;
    g_HciTinyUsbDcdDiag.UsbRegStatus = NRF_POWER->USBREGSTATUS;
    g_HciTinyUsbDcdDiag.HfclkStat = NRF_CLOCK->HFCLKSTAT;

    g_HciTinyUsbDcdDiag.Ep2OutStarted = xfer->started ? 1U : 0U;
    g_HciTinyUsbDcdDiag.Ep2OutDataReceived = xfer->data_received ? 1U : 0U;
    g_HciTinyUsbDcdDiag.Ep2OutActualLen = xfer->actual_len;
    g_HciTinyUsbDcdDiag.Ep2OutTotalLen = xfer->total_len;
    g_HciTinyUsbDcdDiag.Ep2OutMps = xfer->mps;
    g_HciTinyUsbDcdDiag.Ep2OutSize = NRF_USBD->SIZE.EPOUT[2];
    g_HciTinyUsbDcdDiag.Ep2OutAmount = NRF_USBD->EPOUT[2].AMOUNT;
    g_HciTinyUsbDcdDiag.Ep2OutPtr = NRF_USBD->EPOUT[2].PTR;
    g_HciTinyUsbDcdDiag.Ep2OutMaxCnt = NRF_USBD->EPOUT[2].MAXCNT;
    g_HciTinyUsbDcdDiag.Ep2OutBuffer = (uint32_t)(uintptr_t)xfer->buffer;
}

static void HciDcdDefer(osal_task_func_t Func, void *Param, bool InIsr)
{
    g_HciTinyUsbDcdDiag.DeferCount++;
    if (InIsr)
    {
        g_HciTinyUsbDcdDiag.DeferIsrCount++;
    }
    else
    {
        g_HciTinyUsbDcdDiag.DeferTaskCount++;
    }

    if ((uintptr_t)Func ==
            (uintptr_t)(osal_task_func_t)(uintptr_t)xact_out_dma_wrapper &&
        (uintptr_t)Param == 2U)
    {
        g_HciTinyUsbDcdDiag.DeferOut2Count++;
    }
    else if ((uintptr_t)Func ==
                 (uintptr_t)(osal_task_func_t)(uintptr_t)edpt_dma_start &&
             (uintptr_t)Param == (uintptr_t)&NRF_USBD->TASKS_STARTEPIN[2])
    {
        g_HciTinyUsbDcdDiag.DeferIn2Count++;
    }
    else if ((uintptr_t)Func ==
                 (uintptr_t)(osal_task_func_t)(uintptr_t)edpt_dma_start &&
             (uintptr_t)Param == (uintptr_t)&NRF_USBD->TASKS_STARTEPIN[4])
    {
        g_HciTinyUsbDcdDiag.DeferLogIn4Count++;
    }

    usbd_defer_func(Func, Param, InIsr);
}

bool dcd_edpt_xfer(uint8_t rhport,
                   uint8_t ep_addr,
                   uint8_t *buffer,
                   uint16_t total_bytes,
                   bool is_isr)
{
    const bool ep2Out =
        tu_edpt_number(ep_addr) == 2U && tu_edpt_dir(ep_addr) == TUSB_DIR_OUT;

    if (ep2Out)
    {
        g_HciTinyUsbDcdDiag.Ep2OutArmCount++;
        if (_dcd.xfer[2][TUSB_DIR_OUT].started)
        {
            g_HciTinyUsbDcdDiag.Ep2OutArmWhileStartedCount++;
        }
        if (_dcd.xfer[2][TUSB_DIR_OUT].data_received)
        {
            g_HciTinyUsbDcdDiag.Ep2OutArmWithDataCount++;
        }
    }

    const bool result =
        HciTinyUsbDcdEdptXfer(rhport, ep_addr, buffer, total_bytes, is_isr);

    if (ep2Out)
    {
        if (!result)
        {
            g_HciTinyUsbDcdDiag.Ep2OutArmFailCount++;
        }
        HciTinyUsbDcdSnapshot();
    }

    return result;
}

void dcd_int_handler(uint8_t rhport)
{
    const uint32_t inten = NRF_USBD->INTEN;
    const uint32_t epdata = NRF_USBD->EPDATASTATUS;

    if ((inten & USBD_INTEN_EPDATA_Msk) != 0U &&
        NRF_USBD->EVENTS_EPDATA != 0U &&
        (epdata & TU_BIT(16U + 2U)) != 0U)
    {
        g_HciTinyUsbDcdDiag.Ep2OutEpdataCount++;
    }

    if ((inten & TU_BIT(USBD_INTEN_ENDEPOUT0_Pos + 2U)) != 0U &&
        NRF_USBD->EVENTS_ENDEPOUT[2] != 0U)
    {
        g_HciTinyUsbDcdDiag.Ep2OutEndCount++;
    }

    g_HciTinyUsbDcdDiag.IrqCount++;
    g_HciTinyUsbDcdDiag.Dma199Entry = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingEntry = HciTinyUsbDcdPendingEvents();

    HciTinyUsbDcdIntHandler(rhport);

    g_HciTinyUsbDcdDiag.Dma199Exit = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingExit = HciTinyUsbDcdPendingEvents();
    HciTinyUsbDcdSnapshot();
}

void HciTinyUsbDcdTrace(void)
{
    dcd_int_disable(0U);
    HciTinyUsbDcdSnapshot();
    dcd_int_enable(0U);

    HciTrace("dcd2: arm=%lu fail=%lu pre=%lu wdata=%lu epdata=%lu end=%lu defer=%lu\r\n",
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutArmCount,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutArmFailCount,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutArmWhileStartedCount,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutArmWithDataCount,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutEpdataCount,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutEndCount,
             (unsigned long)g_HciTinyUsbDcdDiag.DeferOut2Count);

    HciTrace("dcd2s: start=%lu data=%lu act=%lu total=%lu mps=%lu size=%lu amount=%lu\r\n",
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutStarted,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutDataReceived,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutActualLen,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutTotalLen,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutMps,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutSize,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutAmount);

    HciTrace("dcd2p: buf=0x%08lX ptr=0x%08lX max=%lu dma199=0x%08lX din2=%lu dlog=%lu\r\n",
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutBuffer,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutPtr,
             (unsigned long)g_HciTinyUsbDcdDiag.Ep2OutMaxCnt,
             (unsigned long)g_HciTinyUsbDcdDiag.Dma199Now,
             (unsigned long)g_HciTinyUsbDcdDiag.DeferIn2Count,
             (unsigned long)g_HciTinyUsbDcdDiag.DeferLogIn4Count);
}

#endif
