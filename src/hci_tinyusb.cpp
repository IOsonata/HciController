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
#include "device/usbd_pvt.h"
#include "device/dcd.h"

#include "hci_trace.h"

/* Roothub port used for the device stack. */
#ifndef HCI_TINYUSB_RHPORT
#define HCI_TINYUSB_RHPORT 0U
#endif

#include <string.h>

static HciTinyUsb_t *s_pUsb;

/*
 * Whether a callback is about the interface this file owns, and if not,
 * whether that is worth counting.
 *
 * The device has more than one CDC function and this file drives one of them.
 * A callback for the other is ordinary and means a terminal is attached to the
 * log, so counting it as an error made the counter climb whenever somebody
 * opened it. A callback for an interface the device does not have at all is a
 * different thing and still counted, because it can only come from a
 * descriptor and a configuration that disagree.
 */
static bool HciTinyUsbCallbackIsOurs(uint8_t Interface)
{
    if (s_pUsb == nullptr)
    {
        return false;
    }

    if (Interface == s_pUsb->Interface)
    {
        return true;
    }

    if (Interface >= CFG_TUD_CDC)
    {
        s_pUsb->CallbackInterfaceErrorCount++;
    }

    return false;
}

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

/*
 * Which endpoints are still waiting on a transfer, counted in turns of the
 * device stack rather than in time, because it is the number of chances the
 * stack has had to finish one that says whether it is stuck.
 *
 * usbd_edpt_busy reads the same word the stack itself checks before it will
 * start another transfer, so this is the state and not a guess at it.
 */
static const uint8_t s_HciTinyUsbWatchedEp[HCI_TINYUSB_EP_COUNT] = {
    (uint8_t)HCI_USB_EP_CDC_IN,
    (uint8_t)HCI_USB_EP_CDC_OUT,
    (uint8_t)HCI_USB_EP_LOG_IN,
};

static bool HciTinyUsbWatchEndpoints(HciTinyUsb_t *pUsb)
{
    const bool mounted = tud_mounted();
    bool stuck = false;

    for (size_t i = 0U; i < HCI_TINYUSB_EP_COUNT; i++)
    {
        if (!mounted || !usbd_edpt_busy(0U, s_HciTinyUsbWatchedEp[i]))
        {
            pUsb->EpBusyTurns[i] = 0U;
            continue;
        }

        if (pUsb->EpBusyTurns[i] < UINT16_MAX)
        {
            pUsb->EpBusyTurns[i]++;
        }

        if (pUsb->EpBusyTurns[i] > pUsb->EpBusyWorst[i])
        {
            pUsb->EpBusyWorst[i] = pUsb->EpBusyTurns[i];
        }

        if (pUsb->EpBusyTurns[i] >= HCI_TINYUSB_EP_STUCK_TURNS)
        {
            stuck = true;
        }
    }

    return stuck;
}

/*
 * Take the port down and put it back up when an endpoint has stopped.
 *
 * Detaching makes the host enumerate again, and the reset that comes with
 * that is what clears the device stack's endpoint state and the port's own.
 * Nothing else reachable from here does: the busy mark belongs to the stack
 * and the transfer state to the driver, and clearing one without the other
 * leaves a pair that disagree.
 *
 * The detach is held for a stretch of turns rather than a measured time,
 * because this layer has no clock and the turns are what it can count.
 */
static void HciTinyUsbRecoverPort(HciTinyUsb_t *pUsb, bool Stuck)
{
    if (pUsb->DetachTurns != 0U)
    {
        pUsb->DetachTurns--;
        if (pUsb->DetachTurns == 0U)
        {
            dcd_connect(0U);
            HciTrace("tinyusb: port back up after restart %lu\r\n",
                     (unsigned long)pUsb->RestartCount);
        }
        return;
    }

    if (!Stuck)
    {
        return;
    }

    pUsb->RestartCount++;
    pUsb->DetachTurns = HCI_TINYUSB_DETACH_TURNS;

    for (size_t i = 0U; i < HCI_TINYUSB_EP_COUNT; i++)
    {
        pUsb->EpBusyTurns[i] = 0U;
    }

    HciTrace("tinyusb: endpoint stuck, restarting the port, restart=%lu\r\n",
             (unsigned long)pUsb->RestartCount);
    dcd_disconnect(0U);
}

void HciTinyUsbProcess(HciTinyUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->Started)
    {
        return;
    }

    pUsb->TaskCount++;
    /*
     * tud_task_ext takes a timeout in milliseconds and an in_isr flag, not an
     * rhport. Zero means do not block, which is what a pump called from the
     * runtime loop wants.
     */
    tud_task_ext(0U, false);
    HciTinyUsbRecoverPort(pUsb, HciTinyUsbWatchEndpoints(pUsb));
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

bool HciTinyUsbPortIsOpen(uint8_t Interface)
{
    return tud_mounted() &&
           (tud_cdc_n_get_line_state(Interface) & 0x01U) != 0U;
}

size_t HciTinyUsbWrite(uint8_t Interface, const uint8_t *pData, size_t Len)
{
    if (pData == NULL || Len == 0U || !tud_mounted())
    {
        return 0U;
    }

    /*
     * Nothing opened means nobody is reading. Writing anyway fills the
     * endpoint buffer once and then blocks every later write, so the log
     * would stop at the first line and stay stopped even after a terminal
     * arrived.
     */
    if ((tud_cdc_n_get_line_state(Interface) & 0x01U) == 0U)
    {
        return 0U;
    }

    const uint32_t room = tud_cdc_n_write_available(Interface);
    if (room == 0U)
    {
        return 0U;
    }

    size_t len = Len;
    if (len > room)
    {
        len = room;
    }

    const uint32_t written = tud_cdc_n_write(Interface, pData, (uint32_t)len);
    (void)tud_cdc_n_write_flush(Interface);
    return (size_t)written;
}

extern "C" void tud_cdc_rx_cb(uint8_t itf)
{
    if (!HciTinyUsbCallbackIsOurs(itf))
    {
        return;
    }

    HciTinyUsbWake(s_pUsb);
}

extern "C" void tud_cdc_tx_complete_cb(uint8_t itf)
{
    if (!HciTinyUsbCallbackIsOurs(itf))
    {
        return;
    }

    HciTinyUsbWake(s_pUsb);
}

extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;

    if (!HciTinyUsbCallbackIsOurs(itf))
    {
        /*
         * The other function is the log. A drain that had nothing to write
         * when it was last called stops until something wakes this thread, so
         * a terminal opening the port has to be one of the things that does.
         */
        HciTinyUsbWake(s_pUsb);
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
