/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * The nRF5x DCD behavior in this file was originally based on TinyUSB's
 * Nordic DCD:
 *   SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 *   SPDX-License-Identifier: MIT
 */

/*
 * Standalone nRF52840 USBD DCD for HciController.
 *
 * TinyUSB is the USB device/class layer. This file owns the nRF52840 USBD
 * hardware driver: transfer descriptors, EasyDMA arbitration, endpoint setup,
 * endpoint transfer state, USBD interrupt dispatch, and all IRQ-side USBD
 * register sequencing. No TinyUSB portable DCD source is included here.
 */

#include "tusb_option.h"

#if CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X

#include <inttypes.h>
#include <stdatomic.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#include "nrf.h"
#include "nrf_erratas.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "device/dcd.h"
#include "device/usbd_pvt.h"

enum
{
    HCI_USB_MAX_PACKET_SIZE = 64U,
    HCI_USB_EP_ISO = 8U,
    HCI_USB_EP_CBI_COUNT = 8U
};

#define HCI_USB_EDPT_END_ALL_MASK \
    ((0xFFUL << USBD_INTEN_ENDEPIN0_Pos) | \
     (0xFFUL << USBD_INTEN_ENDEPOUT0_Pos) | \
     USBD_INTEN_ENDISOIN_Msk | USBD_INTEN_ENDISOOUT_Msk)

#define HCI_USB_EVENT_IN_EP 1U
#define HCI_USB_EVENT_IN_STATUS TU_BIT(HCI_USB_EVENT_IN_EP)
#define HCI_USB_EVENT_IN_END_INT \
    TU_BIT(USBD_INTEN_ENDEPIN0_Pos + HCI_USB_EVENT_IN_EP)
#define HCI_USB_BULK_OUT_EP 2U
#define HCI_USB_BULK_OUT_STATUS USBD_EPDATASTATUS_EPOUT2_Msk
#define HCI_USB_BULK_OUT_END_INT \
    TU_BIT(USBD_INTEN_ENDEPOUT0_Pos + HCI_USB_BULK_OUT_EP)
#define HCI_USB_EVENT_ACK_TRACE_DEPTH 13U
#define HCI_USB_IRQ_EVENT_COUNT (USBD_INTEN_EPDATA_Pos + 1U)

#ifndef HCI_USB_IRQ_STORM_LIMIT
#define HCI_USB_IRQ_STORM_LIMIT 2000U
#endif

#define HCI_USB_IRQ_STORM_UNKNOWN 0x80000000UL
#define HCI_USB_EVENTCAUSE_HANDLED \
    (USBD_EVENTCAUSE_SUSPEND_Msk | USBD_EVENTCAUSE_RESUME_Msk | \
     USBD_EVENTCAUSE_USBWUALLOWED_Msk | USBD_EVENTCAUSE_ISOOUTCRC_Msk)
#define HCI_USB_ERRATA_199_REG \
    (*((volatile uint32_t *)0x40027C1CUL))

typedef struct
{
    uint8_t *pBuffer;
    uint16_t TotalLen;
    volatile uint16_t ActualLen;
    uint16_t Mps;
    volatile bool DataReceived;
    volatile bool Started;
    bool IsoInTransferReady;
} HciUsbXfer_t;

typedef struct
{
    HciUsbXfer_t Xfer[HCI_USB_EP_CBI_COUNT + 1U][2];
    bool SofEnabled;
} HciUsbDcdState_t;

static HciUsbDcdState_t s_Dcd;
static atomic_flag s_DmaRunning = ATOMIC_FLAG_INIT;

__attribute__((weak)) uint32_t HciUsbPlatformIrqEnter(void)
{
    return 1U;
}

__attribute__((weak)) void HciUsbPlatformIrqUnexpectedCause(uint32_t Cause)
{
    (void)Cause;
}

__attribute__((weak)) void HciUsbPlatformIrqStorm(uint32_t Inten,
                                                 uint32_t Cause,
                                                 uint32_t Events)
{
    (void)Inten;
    (void)Cause;
    (void)Events;
}

typedef struct
{
    uint32_t EpDataCount;
    uint32_t DmaEndCount;
    uint32_t ContinueCount;
    uint32_t CompleteCount;
    uint32_t BadAmountCount;
    uint32_t StaleStatusCount;
    uint32_t Ep2CollisionCount;
    uint32_t LateStatusCount;
    uint32_t EndOverlapCount;
    uint32_t AckTraceWrite;
    uint32_t AckTrace[HCI_USB_EVENT_ACK_TRACE_DEPTH];
} HciUsbEventInDiag_t;

static volatile HciUsbEventInDiag_t s_EventInDiag;

static inline HciUsbXfer_t *HciUsbGetXfer(uint8_t EpNum, uint8_t Dir)
{
    return &s_Dcd.Xfer[EpNum][Dir];
}

static bool HciUsbInIsr(void)
{
    return (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) != 0U;
}

static void HciUsbRecordEventAck(const HciUsbXfer_t *pXfer,
                                 uint16_t TransferLen)
{
    if (pXfer == NULL || pXfer->pBuffer == NULL || pXfer->Mps == 0U ||
        TransferLen == 0U)
    {
        return;
    }

    uint32_t Word =
        ((uint32_t)(pXfer->ActualLen / pXfer->Mps) << 24);
    Word |= (uint32_t)pXfer->pBuffer[0];
    if (TransferLen > 1U)
    {
        Word |= (uint32_t)pXfer->pBuffer[1] << 8;
    }
    if (TransferLen > 2U)
    {
        Word |= (uint32_t)pXfer->pBuffer[2] << 16;
    }

    const uint32_t Write = s_EventInDiag.AckTraceWrite;
    s_EventInDiag.AckTrace[Write % HCI_USB_EVENT_ACK_TRACE_DEPTH] = Word;
    __DMB();
    s_EventInDiag.AckTraceWrite = Write + 1U;
}

void HciUsbPlatformReadCounters(uint32_t *pValues, size_t Count)
{
    if (pValues == NULL ||
        Count < (9U + HCI_USB_EVENT_ACK_TRACE_DEPTH))
    {
        return;
    }

    pValues[0] = s_EventInDiag.EpDataCount;
    pValues[1] = s_EventInDiag.DmaEndCount;
    pValues[2] = s_EventInDiag.ContinueCount;
    pValues[3] = s_EventInDiag.CompleteCount;
    pValues[4] = s_EventInDiag.BadAmountCount;
    pValues[5] = s_EventInDiag.StaleStatusCount;
    pValues[6] = s_EventInDiag.Ep2CollisionCount;
    pValues[7] = s_EventInDiag.LateStatusCount;
    pValues[8] = s_EventInDiag.EndOverlapCount;

    uint32_t Trace[HCI_USB_EVENT_ACK_TRACE_DEPTH];
    for (unsigned Attempt = 0U; Attempt < 3U; ++Attempt)
    {
        const uint32_t Write = s_EventInDiag.AckTraceWrite;
        __DMB();

        for (unsigned Index = 0U;
             Index < HCI_USB_EVENT_ACK_TRACE_DEPTH;
             ++Index)
        {
            Trace[Index] = 0U;
        }

        const uint32_t Available =
            Write < HCI_USB_EVENT_ACK_TRACE_DEPTH ?
            Write : HCI_USB_EVENT_ACK_TRACE_DEPTH;
        const uint32_t Padding = HCI_USB_EVENT_ACK_TRACE_DEPTH - Available;

        for (uint32_t Index = 0U; Index < Available; ++Index)
        {
            Trace[Padding + Index] =
                s_EventInDiag.AckTrace[
                    (Write - Available + Index) %
                    HCI_USB_EVENT_ACK_TRACE_DEPTH];
        }

        __DMB();
        if (s_EventInDiag.AckTraceWrite == Write)
        {
            break;
        }
    }

    for (unsigned Index = 0U;
         Index < HCI_USB_EVENT_ACK_TRACE_DEPTH;
         ++Index)
    {
        pValues[9U + Index] = Trace[Index];
    }
}

static void HciUsbDmaStart(volatile uint32_t *pTask)
{
    const bool NoDma =
        pTask == &NRF_USBD->TASKS_EP0STATUS ||
        pTask == &NRF_USBD->TASKS_EP0RCVOUT;

    if (!NoDma && nrf52_errata_199())
    {
        HCI_USB_ERRATA_199_REG = 0x00000082UL;
    }

    *pTask = 1U;
    __ISB();
    __DSB();

    if (NoDma)
    {
        atomic_flag_clear(&s_DmaRunning);
    }
}

static void HciUsbDmaEnd(void)
{
    if (nrf52_errata_199())
    {
        HCI_USB_ERRATA_199_REG = 0x00000000UL;
    }
    atomic_flag_clear(&s_DmaRunning);
}

static void HciUsbEdptDmaStart(volatile uint32_t *pTask);

static void HciUsbEdptDmaStartDeferred(void *pContext)
{
    HciUsbEdptDmaStart(
        (volatile uint32_t *)(uintptr_t)pContext);
}

static void HciUsbEdptDmaStart(volatile uint32_t *pTask)
{
    if (atomic_flag_test_and_set(&s_DmaRunning))
    {
        usbd_defer_func(HciUsbEdptDmaStartDeferred,
                        (void *)(uintptr_t)pTask,
                        HciUsbInIsr());
        return;
    }

    HciUsbDmaStart(pTask);
}

static void HciUsbStartOutDma(uint8_t EpNum);

static void HciUsbStartOutDmaDeferred(void *pContext)
{
    HciUsbStartOutDma((uint8_t)(uintptr_t)pContext);
}

static void HciUsbStartOutDma(uint8_t EpNum)
{
    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, TUSB_DIR_OUT);

    if (atomic_flag_test_and_set(&s_DmaRunning))
    {
        usbd_defer_func(HciUsbStartOutDmaDeferred,
                        (void *)(uintptr_t)EpNum,
                        HciUsbInIsr());
        return;
    }

    if (EpNum == HCI_USB_EP_ISO)
    {
        const uint32_t Length = NRF_USBD->SIZE.ISOOUT;
        if ((Length & USBD_SIZE_ISOOUT_ZERO_Msk) != 0U ||
            !pXfer->Started)
        {
            atomic_flag_clear(&s_DmaRunning);
            return;
        }

        NRF_USBD->ISOOUT.PTR = (uint32_t)(uintptr_t)pXfer->pBuffer;
        NRF_USBD->ISOOUT.MAXCNT = Length;
        HciUsbDmaStart(&NRF_USBD->TASKS_STARTISOOUT);
        return;
    }

    if (!pXfer->Started || pXfer->ActualLen >= pXfer->TotalLen)
    {
        pXfer->DataReceived = true;
        atomic_flag_clear(&s_DmaRunning);
        return;
    }

    const uint16_t Remaining = pXfer->TotalLen - pXfer->ActualLen;
    const uint16_t Received = (uint16_t)NRF_USBD->SIZE.EPOUT[EpNum];
    const uint16_t TransferLen =
        Received < Remaining ? Received : Remaining;

    NRF_USBD->EPOUT[EpNum].PTR =
        (uint32_t)(uintptr_t)pXfer->pBuffer;
    NRF_USBD->EPOUT[EpNum].MAXCNT = TransferLen;
    HciUsbDmaStart(&NRF_USBD->TASKS_STARTEPOUT[EpNum]);
}

static void HciUsbDrainReceivedOut(uint8_t EpNum);

static void HciUsbDrainReceivedOutDeferred(void *pContext)
{
    HciUsbDrainReceivedOut((uint8_t)(uintptr_t)pContext);
}

static void HciUsbDrainReceivedOut(uint8_t EpNum)
{
    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, TUSB_DIR_OUT);

    if (atomic_flag_test_and_set(&s_DmaRunning))
    {
        usbd_defer_func(HciUsbDrainReceivedOutDeferred,
                        (void *)(uintptr_t)EpNum,
                        HciUsbInIsr());
        return;
    }

    const uint16_t Remaining =
        pXfer->TotalLen >= pXfer->ActualLen ?
        (uint16_t)(pXfer->TotalLen - pXfer->ActualLen) : 0U;
    const uint16_t Received = (uint16_t)NRF_USBD->SIZE.EPOUT[EpNum];
    const uint16_t TransferLen =
        Received < Remaining ? Received : Remaining;

    NRF_USBD->EPOUT[EpNum].PTR =
        (uint32_t)(uintptr_t)pXfer->pBuffer;
    NRF_USBD->EPOUT[EpNum].MAXCNT = TransferLen;
    HciUsbDmaStart(&NRF_USBD->TASKS_STARTEPOUT[EpNum]);
}

static void HciUsbStartInDma(uint8_t EpNum)
{
    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, TUSB_DIR_IN);
    const uint16_t Remaining = pXfer->TotalLen - pXfer->ActualLen;
    const uint16_t TransferLen =
        Remaining < pXfer->Mps ? Remaining : pXfer->Mps;

    NRF_USBD->EPIN[EpNum].PTR =
        (uint32_t)(uintptr_t)pXfer->pBuffer;
    NRF_USBD->EPIN[EpNum].MAXCNT = TransferLen;
    HciUsbEdptDmaStart(&NRF_USBD->TASKS_STARTEPIN[EpNum]);
}

bool dcd_init(uint8_t RhPort, const tusb_rhport_init_t *pRhInit)
{
    (void)RhPort;
    (void)pRhInit;

    tu_varclr(&s_Dcd);
    atomic_flag_clear(&s_DmaRunning);
    s_Dcd.Xfer[0][TUSB_DIR_IN].Mps = HCI_USB_MAX_PACKET_SIZE;
    s_Dcd.Xfer[0][TUSB_DIR_OUT].Mps = HCI_USB_MAX_PACKET_SIZE;

    TU_LOG2("dcd init\r\n");
    return true;
}

void dcd_int_enable(uint8_t RhPort)
{
    (void)RhPort;
    NVIC_EnableIRQ(USBD_IRQn);
}

void dcd_int_disable(uint8_t RhPort)
{
    (void)RhPort;
    NVIC_DisableIRQ(USBD_IRQn);
}

void dcd_set_address(uint8_t RhPort, uint8_t DevAddr)
{
    (void)RhPort;
    (void)DevAddr;

    const uint32_t Cause = NRF_USBD->EVENTCAUSE;
    NRF_USBD->EVENTCAUSE = Cause;
    NRF_USBD->EVENTS_USBEVENT = 0U;
    NRF_USBD->INTENSET = USBD_INTEN_USBEVENT_Msk;
}

void dcd_remote_wakeup(uint8_t RhPort)
{
    (void)RhPort;
    NRF_USBD->LOWPOWER = 0U;
}

void dcd_disconnect(uint8_t RhPort)
{
    (void)RhPort;
    NRF_USBD->USBPULLUP = 0U;
    dcd_event_bus_signal(0U, DCD_EVENT_UNPLUGGED, false);
}

void dcd_connect(uint8_t RhPort)
{
    (void)RhPort;
    NRF_USBD->USBPULLUP = 1U;
}

void dcd_sof_enable(uint8_t RhPort, bool Enable)
{
    (void)RhPort;

    s_Dcd.SofEnabled = Enable;
    if (Enable)
    {
        NRF_USBD->INTENSET = USBD_INTENSET_SOF_Msk;
    }
    else
    {
        NRF_USBD->INTENCLR = USBD_INTENCLR_SOF_Msk;
    }
}

bool dcd_edpt_open(uint8_t RhPort,
                   const tusb_desc_endpoint_t *pDesc)
{
    (void)RhPort;

    const uint8_t EpAddr = pDesc->bEndpointAddress;
    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);

    TU_ASSERT(EpNum <= HCI_USB_EP_ISO);

    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, Dir);
    pXfer->Mps = tu_edpt_packet_size(pDesc);

    if (pDesc->bmAttributes.xfer != TUSB_XFER_ISOCHRONOUS)
    {
        TU_ASSERT(EpNum < HCI_USB_EP_CBI_COUNT);

        if (Dir == TUSB_DIR_OUT)
        {
            NRF_USBD->INTENSET =
                TU_BIT(USBD_INTEN_ENDEPOUT0_Pos + EpNum);
            NRF_USBD->EPOUTEN |= TU_BIT(EpNum);
            NRF_USBD->SIZE.EPOUT[EpNum] = 0U;
        }
        else
        {
            NRF_USBD->INTENSET =
                TU_BIT(USBD_INTEN_ENDEPIN0_Pos + EpNum);
            NRF_USBD->EPINEN |= TU_BIT(EpNum);
        }

        NRF_USBD->EPSTALL =
            (USBD_EPSTALL_STALL_UnStall << USBD_EPSTALL_STALL_Pos) |
            EpAddr;
        NRF_USBD->DTOGGLE =
            (USBD_DTOGGLE_VALUE_Data0 << USBD_DTOGGLE_VALUE_Pos) |
            EpAddr;
    }
    else
    {
        TU_ASSERT(EpNum == HCI_USB_EP_ISO);

        if (Dir == TUSB_DIR_OUT)
        {
            if (s_Dcd.Xfer[HCI_USB_EP_ISO][TUSB_DIR_IN].Mps != 0U)
            {
                NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_HalfIN;
            }

            NRF_USBD->EVENTS_ENDISOOUT = 0U;
            if ((NRF_USBD->INTEN & USBD_INTEN_SOF_Msk) == 0U)
            {
                NRF_USBD->EVENTS_SOF = 0U;
            }
            NRF_USBD->INTENSET =
                USBD_INTENSET_ENDISOOUT_Msk | USBD_INTENSET_SOF_Msk;
            NRF_USBD->EPOUTEN |= USBD_EPOUTEN_ISOOUT_Msk;
        }
        else
        {
            NRF_USBD->EVENTS_ENDISOIN = 0U;
            if (s_Dcd.Xfer[HCI_USB_EP_ISO][TUSB_DIR_OUT].Mps != 0U)
            {
                NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_HalfIN;
            }

            if ((NRF_USBD->INTEN & USBD_INTEN_SOF_Msk) == 0U)
            {
                NRF_USBD->EVENTS_SOF = 0U;
            }
            NRF_USBD->INTENSET =
                USBD_INTENSET_ENDISOIN_Msk | USBD_INTENSET_SOF_Msk;
            NRF_USBD->EPINEN |= USBD_EPINEN_ISOIN_Msk;
        }
    }

    __ISB();
    __DSB();
    return true;
}

void dcd_edpt_close_all(uint8_t RhPort)
{
    dcd_int_disable(RhPort);

    for (uint8_t EpNum = 1U;
         EpNum < HCI_USB_EP_CBI_COUNT;
         ++EpNum)
    {
        NRF_USBD->INTENCLR =
            TU_BIT(USBD_INTEN_ENDEPOUT0_Pos + EpNum) |
            TU_BIT(USBD_INTEN_ENDEPIN0_Pos + EpNum);
        NRF_USBD->TASKS_STARTEPIN[EpNum] = 0U;
        NRF_USBD->TASKS_STARTEPOUT[EpNum] = 0U;
        tu_memclr(s_Dcd.Xfer[EpNum], 2U * sizeof(HciUsbXfer_t));
    }

    NRF_USBD->INTENCLR =
        USBD_INTENCLR_SOF_Msk |
        USBD_INTENCLR_ENDISOOUT_Msk |
        USBD_INTENCLR_ENDISOIN_Msk;
    NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_OneDir;
    NRF_USBD->TASKS_STARTISOIN = 0U;
    NRF_USBD->TASKS_STARTISOOUT = 0U;
    tu_memclr(s_Dcd.Xfer[HCI_USB_EP_ISO],
              2U * sizeof(HciUsbXfer_t));

    NRF_USBD->EPOUTEN = 1UL;
    NRF_USBD->EPINEN = 1UL;

    dcd_int_enable(RhPort);
}

bool dcd_edpt_iso_alloc(uint8_t RhPort,
                        uint8_t EpAddr,
                        uint16_t LargestPacketSize)
{
    (void)RhPort;
    (void)LargestPacketSize;
    TU_ASSERT(tu_edpt_number(EpAddr) == HCI_USB_EP_ISO);
    return true;
}

bool dcd_edpt_iso_activate(uint8_t RhPort,
                           const tusb_desc_endpoint_t *pDesc)
{
    (void)RhPort;

    const uint8_t EpAddr = pDesc->bEndpointAddress;
    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);

    TU_ASSERT(EpNum == HCI_USB_EP_ISO);

    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, Dir);
    pXfer->Started = false;
    pXfer->DataReceived = false;
    pXfer->IsoInTransferReady = false;
    pXfer->Mps = tu_edpt_packet_size(pDesc);

    if (Dir == TUSB_DIR_OUT)
    {
        if (s_Dcd.Xfer[HCI_USB_EP_ISO][TUSB_DIR_IN].Mps != 0U)
        {
            NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_HalfIN;
        }
        NRF_USBD->EVENTS_ENDISOOUT = 0U;
        if ((NRF_USBD->INTEN & USBD_INTEN_SOF_Msk) == 0U)
        {
            NRF_USBD->EVENTS_SOF = 0U;
        }
        NRF_USBD->INTENSET =
            USBD_INTENSET_ENDISOOUT_Msk | USBD_INTENSET_SOF_Msk;
        NRF_USBD->EPOUTEN |= USBD_EPOUTEN_ISOOUT_Msk;
    }
    else
    {
        NRF_USBD->EVENTS_ENDISOIN = 0U;
        if (s_Dcd.Xfer[HCI_USB_EP_ISO][TUSB_DIR_OUT].Mps != 0U)
        {
            NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_HalfIN;
        }
        if ((NRF_USBD->INTEN & USBD_INTEN_SOF_Msk) == 0U)
        {
            NRF_USBD->EVENTS_SOF = 0U;
        }
        NRF_USBD->INTENSET =
            USBD_INTENSET_ENDISOIN_Msk | USBD_INTENSET_SOF_Msk;
        NRF_USBD->EPINEN |= USBD_EPINEN_ISOIN_Msk;
    }

    __ISB();
    __DSB();
    return true;
}

bool dcd_edpt_xfer(uint8_t RhPort,
                   uint8_t EpAddr,
                   uint8_t *pBuffer,
                   uint16_t TotalBytes,
                   bool IsIsr)
{
    (void)RhPort;
    (void)IsIsr;

    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);

    TU_ASSERT(EpNum <= HCI_USB_EP_ISO);

    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, Dir);
    TU_ASSERT(!pXfer->Started);

    if (EpAddr == (HCI_USB_EVENT_IN_EP | TUSB_DIR_IN_MASK) &&
        (NRF_USBD->EPDATASTATUS & HCI_USB_EVENT_IN_STATUS) != 0U)
    {
        s_EventInDiag.StaleStatusCount++;
        NRF_USBD->EPDATASTATUS = HCI_USB_EVENT_IN_STATUS;
        __ISB();
        __DSB();
    }

    pXfer->pBuffer = pBuffer;
    pXfer->TotalLen = TotalBytes;
    pXfer->ActualLen = 0U;

    const bool ControlStatus =
        EpNum == 0U &&
        TotalBytes == 0U &&
        Dir != tu_edpt_dir((uint8_t)NRF_USBD->BMREQUESTTYPE);

    if (ControlStatus)
    {
        dcd_event_xfer_complete(0U,
                                EpAddr,
                                0U,
                                XFER_RESULT_SUCCESS,
                                HciUsbInIsr());
        HciUsbEdptDmaStart(&NRF_USBD->TASKS_EP0STATUS);
    }
    else if (Dir == TUSB_DIR_OUT)
    {
        pXfer->Started = true;

        if (EpNum == 0U)
        {
            HciUsbEdptDmaStart(&NRF_USBD->TASKS_EP0RCVOUT);
        }
        else
        {
            __ISB();
            __DSB();

            if (pXfer->DataReceived && pXfer->Started)
            {
                pXfer->DataReceived = false;
                HciUsbStartOutDma(EpNum);
            }
        }
    }
    else
    {
        HciUsbStartInDma(EpNum);
    }

    return true;
}

void dcd_edpt_stall(uint8_t RhPort, uint8_t EpAddr)
{
    (void)RhPort;

    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);
    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, Dir);

    if (EpNum == 0U)
    {
        NRF_USBD->TASKS_EP0STALL = 1U;
    }
    else if (EpNum != HCI_USB_EP_ISO)
    {
        NRF_USBD->EPSTALL =
            (USBD_EPSTALL_STALL_Stall << USBD_EPSTALL_STALL_Pos) |
            EpAddr;

        if (Dir == TUSB_DIR_OUT && pXfer->DataReceived)
        {
            pXfer->DataReceived = false;
            HciUsbDrainReceivedOut(EpNum);
        }
    }

    __ISB();
    __DSB();
}

void dcd_edpt_clear_stall(uint8_t RhPort, uint8_t EpAddr)
{
    (void)RhPort;

    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);

    if (EpNum != 0U && EpNum != HCI_USB_EP_ISO)
    {
        NRF_USBD->DTOGGLE = EpAddr;
        NRF_USBD->DTOGGLE =
            (USBD_DTOGGLE_VALUE_Data0 << USBD_DTOGGLE_VALUE_Pos) |
            EpAddr;
        NRF_USBD->EPSTALL =
            (USBD_EPSTALL_STALL_UnStall << USBD_EPSTALL_STALL_Pos) |
            EpAddr;

        if (Dir == TUSB_DIR_OUT)
        {
            NRF_USBD->SIZE.EPOUT[EpNum] = 0U;
        }

        __ISB();
        __DSB();
    }
}

static uint32_t HciUsbCollectEvents(void)
{
    const uint32_t Inten = NRF_USBD->INTEN;
    uint32_t IntStatus = 0U;
    volatile uint32_t *pEvent = &NRF_USBD->EVENTS_USBRESET;

    for (uint8_t Index = 0U; Index < HCI_USB_IRQ_EVENT_COUNT; ++Index)
    {
        const uint32_t Mask = TU_BIT(Index);
        if ((Inten & Mask) == 0U || pEvent[Index] == 0U)
        {
            continue;
        }

        IntStatus |= Mask;
        pEvent[Index] = 0U;
        __ISB();
        __DSB();
    }

    return IntStatus;
}

static uint32_t HciUsbPendingEvents(void)
{
    uint32_t Pending = 0U;
    volatile uint32_t *pEvent = &NRF_USBD->EVENTS_USBRESET;

    for (uint8_t Index = 0U; Index < HCI_USB_IRQ_EVENT_COUNT; ++Index)
    {
        if (pEvent[Index] != 0U)
        {
            Pending |= TU_BIT(Index);
        }
    }

    return Pending;
}

static void HciUsbBusReset(void)
{
    NRF_USBD->EPOUTEN = 1UL;
    NRF_USBD->EPINEN = 1UL;

    for (uint8_t EpNum = 0U;
         EpNum < HCI_USB_EP_CBI_COUNT;
         ++EpNum)
    {
        NRF_USBD->TASKS_STARTEPIN[EpNum] = 0U;
        NRF_USBD->TASKS_STARTEPOUT[EpNum] = 0U;
    }

    NRF_USBD->TASKS_STARTISOIN = 0U;
    NRF_USBD->TASKS_STARTISOOUT = 0U;

    NRF_USBD->EVENTS_USBEVENT = 0U;
    const uint32_t Cause = NRF_USBD->EVENTCAUSE;
    NRF_USBD->EVENTCAUSE = Cause;

    NRF_USBD->INTENCLR = NRF_USBD->INTEN;
    NRF_USBD->INTENSET =
        USBD_INTEN_USBRESET_Msk |
        USBD_INTEN_USBEVENT_Msk |
        USBD_INTEN_EPDATA_Msk |
        USBD_INTEN_EP0SETUP_Msk |
        USBD_INTEN_EP0DATADONE_Msk |
        USBD_INTEN_ENDEPIN0_Msk |
        USBD_INTEN_ENDEPOUT0_Msk;

    tu_varclr(&s_Dcd);
    atomic_flag_clear(&s_DmaRunning);
    s_Dcd.Xfer[0][TUSB_DIR_IN].Mps = HCI_USB_MAX_PACKET_SIZE;
    s_Dcd.Xfer[0][TUSB_DIR_OUT].Mps = HCI_USB_MAX_PACKET_SIZE;
}

void dcd_int_handler(uint8_t RhPort)
{
    (void)RhPort;

    const uint32_t IntStatus = HciUsbCollectEvents();
    if (IntStatus == 0U)
    {
        return;
    }

    uint32_t EventCause = 0U;
    if ((IntStatus & USBD_INTEN_USBEVENT_Msk) != 0U)
    {
        EventCause = NRF_USBD->EVENTCAUSE;
        NRF_USBD->EVENTCAUSE = EventCause;
        __ISB();
        __DSB();

        const uint32_t Unexpected =
            EventCause & ~(uint32_t)HCI_USB_EVENTCAUSE_HANDLED;
        if (Unexpected != 0U)
        {
            HciUsbPlatformIrqUnexpectedCause(Unexpected);
        }
    }

    if ((IntStatus & HCI_USB_EVENT_IN_END_INT) != 0U)
    {
        s_EventInDiag.DmaEndCount++;
        if ((IntStatus & HCI_USB_BULK_OUT_END_INT) != 0U)
        {
            s_EventInDiag.EndOverlapCount++;
        }
    }

    if ((IntStatus & USBD_INTEN_USBRESET_Msk) != 0U)
    {
        HciUsbBusReset();
        dcd_event_bus_reset(0U, TUSB_SPEED_FULL, true);
    }

    if ((IntStatus & USBD_INTEN_ENDISOIN_Msk) != 0U)
    {
        HciUsbXfer_t *pXfer =
            HciUsbGetXfer(HCI_USB_EP_ISO, TUSB_DIR_IN);
        pXfer->ActualLen = NRF_USBD->ISOIN.AMOUNT;
        pXfer->IsoInTransferReady = true;
    }

    if ((IntStatus & USBD_INTEN_SOF_Msk) != 0U)
    {
        bool IsoEnabled = false;

        if ((NRF_USBD->EPOUTEN & USBD_EPOUTEN_ISOOUT_Msk) != 0U)
        {
            IsoEnabled = true;
            if ((IntStatus & USBD_INTEN_USBEVENT_Msk) == 0U ||
                (EventCause & USBD_EVENTCAUSE_ISOOUTCRC_Msk) == 0U)
            {
                HciUsbStartOutDma(HCI_USB_EP_ISO);
            }
        }

        if ((NRF_USBD->EPINEN & USBD_EPINEN_ISOIN_Msk) != 0U)
        {
            IsoEnabled = true;
            HciUsbXfer_t *pXfer =
                HciUsbGetXfer(HCI_USB_EP_ISO, TUSB_DIR_IN);
            if (pXfer->IsoInTransferReady)
            {
                pXfer->IsoInTransferReady = false;
                dcd_event_xfer_complete(
                    0U,
                    HCI_USB_EP_ISO | TUSB_DIR_IN_MASK,
                    pXfer->ActualLen,
                    XFER_RESULT_SUCCESS,
                    true);
            }
        }

        if (!IsoEnabled && !s_Dcd.SofEnabled)
        {
            NRF_USBD->INTENCLR = USBD_INTENCLR_SOF_Msk;
        }

        dcd_event_sof(0U, NRF_USBD->FRAMECNTR, true);
    }

    if ((IntStatus & USBD_INTEN_USBEVENT_Msk) != 0U)
    {
        TU_LOG(3, "EVENTCAUSE = 0x%04" PRIX32 "\r\n", EventCause);

        if ((EventCause & USBD_EVENTCAUSE_SUSPEND_Msk) != 0U)
        {
            NRF_USBD->LOWPOWER = 1U;
            dcd_event_bus_signal(0U, DCD_EVENT_SUSPEND, true);
        }

        if ((EventCause & USBD_EVENTCAUSE_USBWUALLOWED_Msk) != 0U)
        {
            NRF_USBD->DPDMVALUE = USBD_DPDMVALUE_STATE_Resume;
            NRF_USBD->TASKS_DPDMDRIVE = 1U;
            if ((NRF_USBD->INTEN & USBD_INTEN_SOF_Msk) == 0U)
            {
                NRF_USBD->EVENTS_SOF = 0U;
            }
            NRF_USBD->INTENSET = USBD_INTENSET_SOF_Msk;
        }

        if ((EventCause & USBD_EVENTCAUSE_RESUME_Msk) != 0U)
        {
            dcd_event_bus_signal(0U, DCD_EVENT_RESUME, true);
        }
    }

    if ((IntStatus & USBD_INTEN_EP0SETUP_Msk) != 0U)
    {
        const uint8_t Setup[8] =
        {
            NRF_USBD->BMREQUESTTYPE,
            NRF_USBD->BREQUEST,
            NRF_USBD->WVALUEL,
            NRF_USBD->WVALUEH,
            NRF_USBD->WINDEXL,
            NRF_USBD->WINDEXH,
            NRF_USBD->WLENGTHL,
            NRF_USBD->WLENGTHH
        };

        const tusb_control_request_t *pRequest =
            (const tusb_control_request_t *)Setup;

        if (!(TUSB_REQ_RCPT_DEVICE ==
                  pRequest->bmRequestType_bit.recipient &&
              TUSB_REQ_TYPE_STANDARD ==
                  pRequest->bmRequestType_bit.type &&
              TUSB_REQ_SET_ADDRESS == pRequest->bRequest))
        {
            dcd_event_setup_received(0U, Setup, true);
        }
    }

    if ((IntStatus & HCI_USB_EDPT_END_ALL_MASK) != 0U)
    {
        HciUsbDmaEnd();
    }

    for (uint8_t EpNum = 0U;
         EpNum < HCI_USB_EP_CBI_COUNT + 1U;
         ++EpNum)
    {
        if (!tu_bit_test(IntStatus,
                         USBD_INTEN_ENDEPOUT0_Pos + EpNum))
        {
            continue;
        }

        HciUsbXfer_t *pXfer =
            HciUsbGetXfer(EpNum, TUSB_DIR_OUT);
        if (!pXfer->Started)
        {
            continue;
        }

        const uint16_t TransferLen =
            (uint16_t)NRF_USBD->EPOUT[EpNum].AMOUNT;

        pXfer->pBuffer += TransferLen;
        pXfer->ActualLen += TransferLen;

        if (EpNum != HCI_USB_EP_ISO &&
            TransferLen == pXfer->Mps &&
            pXfer->ActualLen < pXfer->TotalLen)
        {
            if (EpNum == 0U)
            {
                HciUsbEdptDmaStart(&NRF_USBD->TASKS_EP0RCVOUT);
            }
        }
        else
        {
            pXfer->TotalLen = pXfer->ActualLen;
            pXfer->Started = false;
            dcd_event_xfer_complete(0U,
                                    EpNum,
                                    pXfer->ActualLen,
                                    XFER_RESULT_SUCCESS,
                                    true);
        }
    }

    if ((IntStatus &
         (USBD_INTEN_EPDATA_Msk | USBD_INTEN_EP0DATADONE_Msk)) != 0U)
    {
        const uint32_t DataStatus = NRF_USBD->EPDATASTATUS;
        NRF_USBD->EPDATASTATUS = DataStatus;
        __ISB();
        __DSB();

        if ((DataStatus & HCI_USB_EVENT_IN_STATUS) != 0U &&
            (DataStatus & HCI_USB_BULK_OUT_STATUS) != 0U)
        {
            s_EventInDiag.Ep2CollisionCount++;
        }

        const bool IsControlIn =
            (IntStatus & USBD_INTEN_EP0DATADONE_Msk) != 0U &&
            (NRF_USBD->BMREQUESTTYPE & TUSB_DIR_IN_MASK) != 0U;
        const bool IsControlOut =
            (IntStatus & USBD_INTEN_EP0DATADONE_Msk) != 0U &&
            (NRF_USBD->BMREQUESTTYPE & TUSB_DIR_IN_MASK) == 0U;

        if ((DataStatus & HCI_USB_BULK_OUT_STATUS) != 0U)
        {
            HciUsbXfer_t *pXfer =
                HciUsbGetXfer(HCI_USB_BULK_OUT_EP, TUSB_DIR_OUT);

            if (pXfer->Started &&
                pXfer->ActualLen < pXfer->TotalLen)
            {
                pXfer->DataReceived = false;
                HciUsbStartOutDma(HCI_USB_BULK_OUT_EP);
            }
            else
            {
                pXfer->DataReceived = true;
            }
        }

        for (uint8_t EpNum = 0U;
             EpNum < HCI_USB_EP_CBI_COUNT;
             ++EpNum)
        {
            if (!tu_bit_test(DataStatus, EpNum) &&
                !(EpNum == 0U && IsControlIn))
            {
                continue;
            }

            HciUsbXfer_t *pXfer =
                HciUsbGetXfer(EpNum, TUSB_DIR_IN);
            const uint8_t TransferLen =
                (uint8_t)NRF_USBD->EPIN[EpNum].AMOUNT;

            if (EpNum == HCI_USB_EVENT_IN_EP)
            {
                s_EventInDiag.EpDataCount++;

                if (pXfer->ActualLen < pXfer->TotalLen)
                {
                    const uint16_t Remaining =
                        pXfer->TotalLen - pXfer->ActualLen;
                    const uint16_t Expected =
                        Remaining < pXfer->Mps ?
                        Remaining : pXfer->Mps;
                    if (TransferLen != Expected)
                    {
                        s_EventInDiag.BadAmountCount++;
                    }
                }

                HciUsbRecordEventAck(pXfer, TransferLen);
            }

            pXfer->pBuffer += TransferLen;
            pXfer->ActualLen += TransferLen;

            if (pXfer->ActualLen < pXfer->TotalLen)
            {
                if (EpNum == HCI_USB_EVENT_IN_EP)
                {
                    s_EventInDiag.ContinueCount++;
                }
                HciUsbStartInDma(EpNum);
            }
            else
            {
                if (EpNum == HCI_USB_EVENT_IN_EP)
                {
                    s_EventInDiag.CompleteCount++;
                }
                dcd_event_xfer_complete(
                    0U,
                    EpNum | TUSB_DIR_IN_MASK,
                    pXfer->ActualLen,
                    XFER_RESULT_SUCCESS,
                    true);
            }
        }

        for (uint8_t EpNum = 0U;
             EpNum < HCI_USB_EP_CBI_COUNT;
             ++EpNum)
        {
            if (EpNum == HCI_USB_BULK_OUT_EP)
            {
                continue;
            }

            if (!tu_bit_test(DataStatus, 16U + EpNum) &&
                !(EpNum == 0U && IsControlOut))
            {
                continue;
            }

            HciUsbXfer_t *pXfer =
                HciUsbGetXfer(EpNum, TUSB_DIR_OUT);

            if (pXfer->Started &&
                pXfer->ActualLen < pXfer->TotalLen)
            {
                pXfer->DataReceived = false;
                HciUsbStartOutDma(EpNum);
            }
            else
            {
                pXfer->DataReceived = true;
            }
        }
    }
}

void USBD_IRQHandler(void)
{
    const uint32_t EntriesThisPass = HciUsbPlatformIrqEnter();
    if (EntriesThisPass == 0U)
    {
        return;
    }

    if (EntriesThisPass > HCI_USB_IRQ_STORM_LIMIT)
    {
        const uint32_t Inten = NRF_USBD->INTEN;
        const uint32_t Cause = NRF_USBD->EVENTCAUSE;
        uint32_t Events = HciUsbPendingEvents();
        if (Events == 0U)
        {
            Events = HCI_USB_IRQ_STORM_UNKNOWN;
        }

        NRF_USBD->INTEN = 0U;
        NRF_USBD->USBPULLUP = 0U;
        NVIC_DisableIRQ(USBD_IRQn);
        __ISB();
        __DSB();

        HciUsbPlatformIrqStorm(Inten, Cause, Events);
        return;
    }

    dcd_int_handler(0U);
}

void HciUsbPlatformEndpointClosed(uint8_t RhPort, uint8_t EpAddr)
{
    const uint8_t EpNum = tu_edpt_number(EpAddr);
    const uint8_t Dir = tu_edpt_dir(EpAddr);

    if (EpNum == 0U || EpNum >= HCI_USB_EP_CBI_COUNT)
    {
        return;
    }

    bool DmaEnded;
    uint32_t DataStatusBit;

    if (Dir == TUSB_DIR_OUT)
    {
        NRF_USBD->INTENCLR =
            TU_BIT(USBD_INTEN_ENDEPOUT0_Pos + EpNum);
        NRF_USBD->EPOUTEN &= ~TU_BIT(EpNum);

        DmaEnded = NRF_USBD->EVENTS_ENDEPOUT[EpNum] != 0U;
        NRF_USBD->EVENTS_ENDEPOUT[EpNum] = 0U;
        DataStatusBit = TU_BIT(16U + EpNum);
    }
    else
    {
        NRF_USBD->INTENCLR =
            TU_BIT(USBD_INTEN_ENDEPIN0_Pos + EpNum);
        NRF_USBD->EPINEN &= ~TU_BIT(EpNum);

        DmaEnded = NRF_USBD->EVENTS_ENDEPIN[EpNum] != 0U;
        NRF_USBD->EVENTS_ENDEPIN[EpNum] = 0U;
        DataStatusBit = TU_BIT(EpNum);
    }

    __ISB();
    __DSB();

    if (DmaEnded)
    {
        HciUsbDmaEnd();
    }

    NRF_USBD->EPDATASTATUS = DataStatusBit;
    __ISB();
    __DSB();

    HciUsbXfer_t *pXfer = HciUsbGetXfer(EpNum, Dir);
    pXfer->Started = false;
    pXfer->DataReceived = false;
    pXfer->IsoInTransferReady = false;

    usbd_edpt_clear_stall(RhPort, EpAddr);
}

#endif /* CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X */
