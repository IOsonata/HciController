/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_app.h"

#include <string.h>

#include "board.h"
#include "coredev/iopincfg.h"
#include "hci_trace.h"
#include "sdc_hci.h"

/*
 * The controller packet buffer feeds the host transport, which prefixes one
 * H:4 indicator byte. A packet the transport cannot take is now dropped rather
 * than retried forever, so getting this wrong loses events instead of wedging
 * the link, but it should never be wrong.
 */
static_assert(HCI_APP_PACKET_SIZE + 1U <= HCI_INTRF_TX_STREAM_SIZE,
              "controller packet plus indicator must fit the transport stream");

/*
 * sdc_hci_get takes no capacity argument, so the buffer has to be at least as
 * large as the largest message the controller can hand back. sdc_hci.h: "the
 * size of the provided buffer is at least HCI_MSG_BUFFER_MAX_SIZE bytes. For
 * Isochronous Channels the provided buffer should be large enough to contain
 * the maximum supported SDU size."
 *
 * ISO is what makes the difference: 258 bytes without it, 4107 with. The
 * second assertion is what stops a future sdc_support_cis_* or
 * sdc_support_bis_* call from silently turning sdc_hci_get into an overflow of
 * HciApp_t, which no downstream length check could catch. Enabling ISO means
 * raising HCI_APP_PACKET_SIZE and defining HCI_APP_ISO_ENABLED with it.
 */
static_assert(HCI_APP_PACKET_SIZE >= HCI_MSG_BUFFER_MAX_SIZE,
              "controller packet must hold the largest SDC message");

#ifdef HCI_APP_ISO_ENABLED
static_assert(HCI_APP_PACKET_SIZE >= HCI_MSG_BUFFER_ISO_MAX_SIZE,
              "controller packet must hold the largest SDC ISO message");
#endif

/*
 * Which UART instance the board's pins belong to. board.h says, since a board
 * that puts the host on UARTE1 would otherwise get UARTE0 configured with its
 * pin map and no diagnostic.
 */
#ifndef UART_DEVNO
#define HCI_APP_UART_DEVICE       0
#else
#define HCI_APP_UART_DEVICE       UART_DEVNO
#endif

#define HCI_APP_UART_IRQ_PRIORITY 6

/*
 * Say the controller is ready with a No Operation Command Complete once the
 * stack is up. Off unless the board asks, because the boards here do not need
 * it. A board answering to a host built with Zephyr CONFIG_BT_WAIT_NOP does:
 * that host waits for this event before it will send anything.
 *
 * The test lives here rather than in hci_sdc_nrfxlib.cpp because that file
 * does not include board.h and so could never see the answer, which is how a
 * board that set it once got an image with the call compiled out.
 */
#ifndef HCI_SDC_STARTUP_NOP
#define HCI_SDC_STARTUP_NOP 0
#endif

/*
 * Device stack pump passes during enumeration, and the steady state tick that
 * keeps it pumped if a wake is ever lost.
 */
#ifndef HCI_APP_USB_SETTLE_PASSES
#define HCI_APP_USB_SETTLE_PASSES 200000U
#endif

#ifndef HCI_APP_USB_SETTLE_REPORT
#define HCI_APP_USB_SETTLE_REPORT 50000U
#endif

#ifndef HCI_APP_STOP_TIMEOUT_MS
#define HCI_APP_STOP_TIMEOUT_MS 250U
#endif

#ifndef HCI_APP_USB_POLL_MS
#define HCI_APP_USB_POLL_MS 5U
#endif

static HciApp_t *s_pApp;

/*
 * The pin list belongs to the board, so board.h owns it. NbIOPins below is
 * taken from sizeof, so a board that needs flow control pins can list them
 * without touching this file.
 */
#ifdef UART_PINS
static const IOPinCfg_t s_HciUartPins[] = UART_PINS;
#endif

static void HciAppWake(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp != nullptr && pApp->Initialized)
    {
        HciTaktOsWake(&pApp->Runtime, HCI_TAKTOS_EVENT_HOST);
    }
}

static int HciAppUsbEvent(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    HciAppWake(s_pApp);
    return Len;
}

static int HciAppUartEvent(UARTDev_t * const,
                           UART_EVT EvtId,
                           uint8_t *,
                           int BufferLen)
{
    if (EvtId == UART_EVT_RXDATA || EvtId == UART_EVT_RXTIMEOUT ||
        EvtId == UART_EVT_TXREADY)
    {
        HciAppWake(s_pApp);
    }
    return BufferLen;
}

static bool HciAppInitUsb(HciApp_t *pApp)
{
    UsbdCdcIntrfCfg_t cfg = {};
    cfg.bBlocking = true;
    cfg.RxFifoMemSize = sizeof(pApp->UsbRxFifoMem);
    cfg.pRxFifoMem = pApp->UsbRxFifoMem;
    cfg.TxFifoMemSize = sizeof(pApp->UsbTxFifoMem);
    cfg.pTxFifoMem = pApp->UsbTxFifoMem;
    cfg.EvtCB = HciAppUsbEvent;

    if (!UsbdCdcIntrfInit(&pApp->UsbIntrf, &cfg))
    {
        return false;
    }

    if (!HciTinyUsbInit(&pApp->Usb,
                        &pApp->UsbIntrf,
                        HCI_APP_CDC_INTERFACE,
                        HciAppWake,
                        pApp))
    {
        return false;
    }

    pApp->pHostIntrf = &pApp->UsbIntrf.DevIntrf;
    return true;
}

/*
 * Hardware flow control is a property of the link, not of this layer, so the
 * board says. Where RTS and CTS go nowhere, none is right and asserting flow
 * control on unconnected pins would stop the link. Where the peer drives them,
 * ignoring them loses data.
 */
#ifndef UART_FLOWCTRL
#define HCI_APP_UART_FLOWCTRL UART_FLWCTRL_NONE
#else
#define HCI_APP_UART_FLOWCTRL UART_FLOWCTRL
#endif

static bool HciAppInitUart(HciApp_t *pApp)
{
#ifdef UART_PINS
    UARTCfg_t cfg = {};
    cfg.DevNo = HCI_APP_UART_DEVICE;
    cfg.pIOPinMap = s_HciUartPins;
    cfg.NbIOPins = sizeof(s_HciUartPins) / sizeof(s_HciUartPins[0]);
    cfg.Rate = UART_RATE;
    cfg.DataBits = 8;
    cfg.Parity = UART_PARITY_NONE;
    cfg.StopBits = 1;
    cfg.FlowControl = HCI_APP_UART_FLOWCTRL;
    cfg.bIntMode = true;
    cfg.IntPrio = HCI_APP_UART_IRQ_PRIORITY;
    cfg.EvtCallback = HciAppUartEvent;
    cfg.bFifoBlocking = true;
    cfg.RxMemSize = sizeof(pApp->UartRxFifoMem);
    cfg.pRxMem = pApp->UartRxFifoMem;
    cfg.TxMemSize = sizeof(pApp->UartTxFifoMem);
    cfg.pTxMem = pApp->UartTxFifoMem;
    cfg.bDMAMode = true;
    cfg.Duplex = UART_DUPLEX_FULL;
    cfg.Mode = UART_MODE_UART;

    if (!UARTInit(&pApp->Uart, &cfg))
    {
        return false;
    }

    pApp->pHostIntrf = &pApp->Uart.DevIntrf;
    return true;
#else

    return false;
#endif
}

static void HciAppSetHostOpen(HciApp_t *pApp, bool Open)
{
    if (pApp->HostOpen == Open)
    {
        return;
    }

    pApp->HostOpen = Open;
    if (Open)
    {
        HciControllerPortOpen(&pApp->Controller);
    }
    else
    {
        HciControllerPortClose(&pApp->Controller);
    }
}

static bool HciAppHostStart(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return false;
    }

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        /*
         * tud_init first, then the hardware. The device stack has to be able
         * to answer setup packets before the pull up goes up.
         */
        if (!HciTinyUsbStart(&pApp->Usb))
        {
            HciTrace("host: HciTinyUsbStart failed\r\n");
            return false;
        }

        if (!HciNrf52840UsbStart(&pApp->Target))
        {
            HciTrace("host: HciNrf52840UsbStart failed err=%ld\r\n",
                     (long)pApp->Target.LastError);
            return false;
        }

        /*
         * Pump the device stack while the host enumerates. Enumeration takes
         * on the order of a hundred milliseconds and must not depend on a wake
         * arriving for every step.
         */
        for (uint32_t pass = 0U; pass < HCI_APP_USB_SETTLE_PASSES; pass++)
        {
            /*
             * MPSL and the radio are already up. This loop runs at the highest
             * thread priority and can take its whole budget on a port that
             * supplies VBUS without enumerating, so low priority processing is
             * pumped here to stay inside its deadline.
             */
            if (pApp->Runtime.Ops.ProcessMpsl != nullptr)
            {
                pApp->Runtime.Ops.ProcessMpsl(pApp->Runtime.Ops.pContext);
            }

            HciNrf52840UsbPowerProcess(&pApp->Target);
            HciNrf52840UsbPassMark(&pApp->Target);
            HciTinyUsbProcess(&pApp->Usb);

            if (HciTinyUsbIsMounted(&pApp->Usb))
            {
                break;
            }

            /* A stop asked for during enumeration must not wait it out. */
            if (pApp->Runtime.StopRequested)
            {
                HciTrace("host: settle abandoned, stop requested\r\n");
                break;
            }

            if (pApp->Target.UsbStormEvents != 0U)
            {
                HciTrace("host: storm pass=%lu irq=%lu inten=0x%08lX "
                         "events=0x%08lX cause=0x%08lX\r\n",
                         (unsigned long)pass + 1UL,
                         (unsigned long)pApp->Target.UsbIrqCount,
                         (unsigned long)pApp->Target.UsbStormInten,
                         (unsigned long)pApp->Target.UsbStormEvents,
                         (unsigned long)pApp->Target.UsbStormCause);
                break;
            }

            if ((pass % HCI_APP_USB_SETTLE_REPORT) == (HCI_APP_USB_SETTLE_REPORT - 1U))
            {
                HciTrace("host: settling pass=%lu irq=%lu stuck=%lu cause=0x%08lX\r\n",
                         (unsigned long)pass + 1UL,
                         (unsigned long)pApp->Target.UsbIrqCount,
                         (unsigned long)pApp->Target.UsbStuckCauseCount,
                         (unsigned long)pApp->Target.UsbEventCause);
            }
        }

        HciAppSetHostOpen(pApp, HciTinyUsbIsOpen(&pApp->Usb));
        HciTrace("host: usb up mounted=%u open=%u irq=%lu task=%lu stuck=%lu cause=0x%08lX\r\n",
                 (unsigned)HciTinyUsbIsMounted(&pApp->Usb),
                 (unsigned)pApp->HostOpen,
                 (unsigned long)pApp->Target.UsbIrqCount,
                 (unsigned long)pApp->Usb.TaskCount,
                 (unsigned long)pApp->Target.UsbStuckCauseCount,
                 (unsigned long)pApp->Target.UsbEventCause);
    }
    else
    {
        HciAppSetHostOpen(pApp, true);
    }

    return true;
}

static void HciAppHostProcess(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return;
    }

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        /*
         * Cable attach and detach are recorded by POWER_CLOCK and applied
         * here, in the same context that pumps the device stack, because the
         * TinyUSB event queue is only protected against this context.
         */
        HciNrf52840UsbPowerProcess(&pApp->Target);

        HciTinyUsbProcess(&pApp->Usb);
        HciAppSetHostOpen(pApp, HciTinyUsbIsOpen(&pApp->Usb));
    }

    HciControllerProcess(&pApp->Controller);

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        HciTinyUsbProcess(&pApp->Usb);
    }
}

bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType)
{
    if (pApp == nullptr ||
        (HostType != HCI_APP_HOST_UART && HostType != HCI_APP_HOST_USB) ||
        (s_pApp != nullptr && s_pApp != pApp))
    {
        return false;
    }

    memset(pApp, 0, sizeof(*pApp));
    pApp->HostType = HostType;
    s_pApp = pApp;

    /*
     * The counter readout spans four layers, and the pointers stay valid
     * whatever order those layers are brought up in, so it is wired here
     * before anything else needs it.
     */
    HciCountersInit(&pApp->Counters, &pApp->Sdc, &pApp->Controller);

    if (!HciSdcNrfxlibInit(&pApp->Sdc,
                           pApp->CommandEvent,
                           sizeof(pApp->CommandEvent),
                           &pApp->Counters))
    {
        HciTrace("init: HciSdcNrfxlibInit failed\r\n");
        pApp->LastError = -1;
        s_pApp = nullptr;
        return false;
    }

#if HCI_SDC_STARTUP_NOP
    /*
     * Queued here, with the dispatcher empty and no command able to have
     * arrived, so it is the first thing the host sees. It cannot reach the wire
     * before the radio is up, because a failure below leaves this function
     * returning false and the runtime thread never starts.
     */
    HciSdcNrfxlibQueueStartupNop(&pApp->Sdc);
#endif

    bool hostReady = HostType == HCI_APP_HOST_USB ?
                     HciAppInitUsb(pApp) : HciAppInitUart(pApp);
    if (!hostReady)
    {
        HciTrace("init: host interface failed type=%u\r\n", (unsigned)HostType);
        pApp->LastError = -2;
        s_pApp = nullptr;
        return false;
    }

    const HciControllerOps_t *pControllerOps = HciSdcGetControllerOps(&pApp->Sdc);
    if (pControllerOps == nullptr ||
        !HciControllerInit(&pApp->Controller,
                           pApp->pHostIntrf,
                           pApp->HostPacket,
                           sizeof(pApp->HostPacket),
                           pApp->ControllerPacket,
                           sizeof(pApp->ControllerPacket),
                           pControllerOps))
    {
        HciTrace("init: HciControllerInit failed\r\n");
        pApp->LastError = -3;
        s_pApp = nullptr;
        return false;
    }

    if (!HciNrf52840Init(&pApp->Target,
                         &pApp->Runtime,
                         reinterpret_cast<uint8_t *>(pApp->SdcMem),
                         sizeof(pApp->SdcMem),
                         HostType == HCI_APP_HOST_USB))
    {
        HciTrace("init: HciNrf52840Init failed\r\n");
        pApp->LastError = -4;
        s_pApp = nullptr;
        return false;
    }

    HciTaktOsOps_t runtimeOps = {};
    HciNrf52840GetTaktOsOps(&pApp->Target, &runtimeOps);

    HciTaktOsHostOps_t hostOps = {};
    hostOps.Start = HciAppHostStart;
    hostOps.Process = HciAppHostProcess;
    hostOps.pContext = pApp;
    hostOps.PollIntervalMs = HostType == HCI_APP_HOST_USB ?
                             HCI_APP_USB_POLL_MS : 0U;

    if (!HciTaktOsInit(&pApp->Runtime, &runtimeOps, &hostOps))
    {
        HciTrace("init: HciTaktOsInit failed\r\n");
        pApp->LastError = -5;
        s_pApp = nullptr;
        return false;
    }

    HciTrace("init: ok host=%s\r\n",
             HostType == HCI_APP_HOST_USB ? "usb" : "uart");
    pApp->Initialized = true;
    return true;
}

void HciAppStop(HciApp_t *pApp)
{
    if (pApp == nullptr || !pApp->Initialized)
    {
        return;
    }

    HciTaktOsStop(&pApp->Runtime);

    /*
     * The runtime thread calls into SDC and MPSL, so it has to be out of its
     * loop before either is torn down. If it does not stop, leave the target
     * running rather than pulling it out from under the thread.
     */
    if (!HciTaktOsWaitStopped(&pApp->Runtime, HCI_APP_STOP_TIMEOUT_MS))
    {
        HciTrace("stop: runtime still running, target left up\r\n");
        pApp->Initialized = false;
        s_pApp = nullptr;
        return;
    }

    HciNrf52840Stop(&pApp->Target);
    if (pApp->pHostIntrf != nullptr)
    {
        DeviceIntrfDisable(pApp->pHostIntrf);
    }
    pApp->Initialized = false;
    s_pApp = nullptr;
}

void HciAppThread(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr || !pApp->Initialized)
    {
        return;
    }

    HciTaktOsThread(&pApp->Runtime);
}

bool HciAppHostIsOpen(const HciApp_t *pApp)
{
    return pApp != nullptr && pApp->HostOpen;
}
