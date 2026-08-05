/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_tinyusb.h"

#include "tusb.h"

#include "hci_trace.h"

/* Roothub port used for the device stack. */
#ifndef HCI_TINYUSB_RHPORT
#define HCI_TINYUSB_RHPORT 0U
#endif

#include <string.h>

static HciTinyUsb_t *s_pUsb;

static void HciTinyUsbWake(HciTinyUsb_t *pUsb)
{
    if (pUsb != nullptr && pUsb->Wake != nullptr)
    {
        pUsb->Wake(pUsb->pWakeContext);
    }
}

bool HciTinyUsbInit(HciTinyUsb_t *pUsb,
                    UsbdCdcDevIntrf_t *pIntrf,
                    uint8_t Interface,
                    HciTinyUsbWake_t Wake,
                    void *pWakeContext)
{
    if (pUsb == nullptr || pIntrf == nullptr ||
        pIntrf->hRxFifo == nullptr || pIntrf->hTxFifo == nullptr ||
        (s_pUsb != nullptr && s_pUsb != pUsb))
    {
        return false;
    }

    memset(pUsb, 0, sizeof(*pUsb));
    pUsb->pIntrf = pIntrf;
    pUsb->Interface = Interface;
    pUsb->Wake = Wake;
    pUsb->pWakeContext = pWakeContext;
    s_pUsb = pUsb;
    return true;
}

bool HciTinyUsbStart(HciTinyUsb_t *pUsb)
{
    if (pUsb == nullptr || pUsb != s_pUsb || pUsb->Started)
    {
        return false;
    }

    /*
     * tusb_rhport_init, not tud_init. tud_init is a static inline that calls
     * tud_rhport_init directly and never reaches tusb.c, so the roothub port
     * role stays TUSB_ROLE_INVALID. tusb_int_handler dispatches on that role,
     * so with tud_init the interrupt handler does nothing at all: no event is
     * ever cleared and no setup packet is ever seen.
     */
    tusb_rhport_init_t rhInit;
    memset(&rhInit, 0, sizeof(rhInit));
    rhInit.role = TUSB_ROLE_DEVICE;
    rhInit.speed = TUSB_SPEED_FULL;

    if (!tusb_rhport_init(HCI_TINYUSB_RHPORT, &rhInit))
    {
        HciTrace("tinyusb: tusb_rhport_init failed\r\n");
        return false;
    }

    HciTrace("tinyusb: rhport init ok inited=%u\r\n", (unsigned)tusb_inited());
    pUsb->Started = true;
    pUsb->RequestedOpen = (tud_cdc_n_get_line_state(pUsb->Interface) & 0x01U) != 0U;
    pUsb->LineStatePending = true;
    return true;
}

static void HciTinyUsbProcessRx(HciTinyUsb_t *pUsb)
{
    if (!pUsb->RequestedOpen)
    {
        return;
    }

    uint8_t data[64];
    while (tud_cdc_n_available(pUsb->Interface) > 0U)
    {
        int fifoAvail = CFifoAvail(pUsb->pIntrf->hRxFifo);
        if (fifoAvail <= 0)
        {
            break;
        }

        uint32_t available = tud_cdc_n_available(pUsb->Interface);
        uint32_t readLen = available < sizeof(data) ? available : sizeof(data);
        if (readLen > (uint32_t)fifoAvail)
        {
            readLen = (uint32_t)fifoAvail;
        }

        uint32_t actual = tud_cdc_n_read(pUsb->Interface, data, readLen);
        if (actual == 0U || actual > readLen)
        {
            pUsb->ReadErrorCount++;
            break;
        }

        int written = CFifoWrite(pUsb->pIntrf->hRxFifo, data, (int)actual);
        if (written != (int)actual)
        {
            pUsb->RxDropCount += actual - (uint32_t)(written > 0 ? written : 0);
            break;
        }

        if (pUsb->pIntrf->DevIntrf.EvtCB != nullptr)
        {
            pUsb->pIntrf->DevIntrf.EvtCB(&pUsb->pIntrf->DevIntrf,
                                         DEVINTRF_EVT_RX_DATA,
                                         nullptr,
                                         CFifoUsed(pUsb->pIntrf->hRxFifo));
        }
    }
}

static void HciTinyUsbProcessTx(HciTinyUsb_t *pUsb)
{
    if (!pUsb->RequestedOpen)
    {
        return;
    }

    while (CFifoUsed(pUsb->pIntrf->hTxFifo) > 0)
    {
        uint32_t available = tud_cdc_n_write_available(pUsb->Interface);
        if (available == 0U)
        {
            pUsb->WriteBusyCount++;
            break;
        }

        int len = CFifoUsed(pUsb->pIntrf->hTxFifo);
        if (len > (int)sizeof(pUsb->pIntrf->TransBuff))
        {
            len = sizeof(pUsb->pIntrf->TransBuff);
        }
        if (len > (int)available)
        {
            len = (int)available;
        }

        int count = CFifoRead(pUsb->pIntrf->hTxFifo,
                              pUsb->pIntrf->TransBuff,
                              len);
        if (count <= 0)
        {
            break;
        }

        uint32_t written = tud_cdc_n_write(pUsb->Interface,
                                           pUsb->pIntrf->TransBuff,
                                           (uint32_t)count);
        if (written != (uint32_t)count)
        {
            pUsb->WriteErrorCount++;
            break;
        }
    }

    (void)tud_cdc_n_write_flush(pUsb->Interface);
}

void HciTinyUsbProcess(HciTinyUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->Started)
    {
        return;
    }

    pUsb->TaskCount++;
    tud_task_ext(HCI_TINYUSB_RHPORT, false);
    HciTinyUsbProcessRx(pUsb);
    HciTinyUsbProcessTx(pUsb);
}

bool HciTinyUsbIsOpen(const HciTinyUsb_t *pUsb)
{
    return pUsb != nullptr && pUsb->Started && pUsb->RequestedOpen;
}

bool HciTinyUsbIsMounted(const HciTinyUsb_t *pUsb)
{
    return pUsb != nullptr && pUsb->Started && tud_mounted();
}

extern "C" void tud_cdc_rx_cb(uint8_t itf)
{
    if (s_pUsb == nullptr || itf != s_pUsb->Interface)
    {
        if (s_pUsb != nullptr)
        {
            s_pUsb->CallbackInterfaceErrorCount++;
        }
        return;
    }

    HciTinyUsbWake(s_pUsb);
}

extern "C" void tud_cdc_tx_complete_cb(uint8_t itf)
{
    if (s_pUsb == nullptr || itf != s_pUsb->Interface)
    {
        if (s_pUsb != nullptr)
        {
            s_pUsb->CallbackInterfaceErrorCount++;
        }
        return;
    }

    HciTinyUsbWake(s_pUsb);
}

extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;

    if (s_pUsb == nullptr || itf != s_pUsb->Interface)
    {
        if (s_pUsb != nullptr)
        {
            s_pUsb->CallbackInterfaceErrorCount++;
        }
        return;
    }

    HciTrace("tinyusb: line state dtr=%u\r\n", (unsigned)dtr);
    s_pUsb->RequestedOpen = dtr;
    s_pUsb->LineStatePending = true;
    HciTinyUsbWake(s_pUsb);
}

extern "C" void tud_umount_cb(void)
{
    if (s_pUsb != nullptr)
    {
        s_pUsb->RequestedOpen = false;
        s_pUsb->LineStatePending = true;
        HciTinyUsbWake(s_pUsb);
    }
}

extern "C" void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    (void)eventid;
    (void)in_isr;
    HciTinyUsbWake(s_pUsb);
}
