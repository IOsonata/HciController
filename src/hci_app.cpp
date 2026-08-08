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
 * Isochronous channels are enabled, so the second sentence applies. The size
 * that matters is the one configured in sdc_cfg_iso_buffer_cfg_t, not the 4095
 * octet ceiling the specification allows: a controller told its receive SDU
 * buffer is 251 octets never hands back more than that plus the isochronous
 * data header. HCI_SDC_ISO_PACKET_SIZE is that sum, and it moves with the
 * configuration, so raising the configured SDU size without raising
 * HCI_APP_PACKET_SIZE stops the build instead of overflowing HciApp_t, which
 * no downstream length check could catch.
 */
static_assert(HCI_APP_PACKET_SIZE >= HCI_MSG_BUFFER_MAX_SIZE,
              "controller packet must hold the largest SDC message");

static_assert(HCI_APP_PACKET_SIZE >= HCI_SDC_ISO_PACKET_SIZE,
              "controller packet must hold the largest configured ISO SDU");

/*
 * Every link the controller can hold has to be trackable, or the ACL credit
 * guard stands down for the ones that do not fit and the host can overrun the
 * advertised buffer count on those without being refused. This is the one
 * place both numbers are in scope, the routing layer's table size and the
 * target's link counts.
 */
static_assert(HCI_SDC_ACL_TRACK_HANDLES >=
                  HCI_SDC_PERIPHERAL_COUNT + HCI_SDC_CENTRAL_COUNT,
              "HCI_SDC_ACL_TRACK_HANDLES is smaller than the link count");

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

/*
 * Whether the USB socket on the board is wired to this part. board.h says,
 * because it is a property of the PCB and nothing here can read it.
 *
 * It decides one thing: whether a board whose HCI stream is on the UART brings
 * the device stack up anyway, so the log has a port to reach a person on.
 * Where the socket belongs to another part, bringing it up would enumerate a
 * device on somebody else's bus.
 */
#ifndef HCI_USB_SOCKET
#define HCI_USB_SOCKET 0
#endif

/*
 * Bring up the CDC interface and the device stack, without saying what the
 * ports are for. The caller decides whether the first function is the HCI
 * stream or is left unused with only the log on the second.
 */
static bool HciAppUsbSetup(HciApp_t *pApp)
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

    pApp->UsbRunning = true;
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

/*
 * Bring the port up for the log alone, with the HCI stream on the UART.
 *
 * No settling loop here, unlike the path below. A host on the UART may send
 * its first command in the first millisecond, and spending a hundred of them
 * waiting for a terminal that may never be plugged in would lose it. The stack
 * enumerates in the background instead, pumped from the same place as
 * everything else, and the ring holds the log until it does.
 *
 * A failure to start is not one. With no cable there is no VBUS and the
 * peripheral cannot come up at all, which is the ordinary state of a board on
 * a battery. It is recorded, the pumping stops, and the HCI link is untouched.
 * A cable arriving after this point does not bring it back: the port clears
 * the power interrupt when it gives up, so a log wanted on a board already
 * running means plugging in and resetting.
 */
static void HciAppStartLogPort(HciApp_t *pApp)
{
    if (!HciTinyUsbStart(&pApp->Usb))
    {
        HciTrace("log: HciTinyUsbStart failed\r\n");
        pApp->UsbRunning = false;
        return;
    }

    if (!pApp->Target.pOps->UsbStart(pApp->Target.pContext))
    {
        HciTrace("log: target UsbStart failed err=%ld\r\n",
                 (long)HciTargetLastError(&pApp->Target));
        pApp->UsbRunning = false;
        return;
    }

    HciTrace("log: usb up on cdc %u\r\n", (unsigned)HCI_APP_LOG_INTERFACE);
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

        if (!pApp->Target.pOps->UsbStart(pApp->Target.pContext))
        {
            HciTrace("host: target UsbStart failed err=%ld\r\n",
                     (long)HciTargetLastError(&pApp->Target));
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

            pApp->Target.pOps->UsbPowerProcess(pApp->Target.pContext);
            pApp->Target.pOps->UsbPassMark(pApp->Target.pContext);
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

            /*
             * The port says whether the peripheral has reached a state the
             * remaining passes cannot change, and prints what it saw. Which
             * registers those are is not something this layer knows.
             */
            if (HciTargetUsbStuck(&pApp->Target))
            {
                HciTargetUsbTrace(&pApp->Target, "storm", pass + 1U);
                break;
            }

            if ((pass % HCI_APP_USB_SETTLE_REPORT) == (HCI_APP_USB_SETTLE_REPORT - 1U))
            {
                HciTargetUsbTrace(&pApp->Target, "settling", pass + 1U);
            }
        }

        HciAppSetHostOpen(pApp, HciTinyUsbIsOpen(&pApp->Usb));
        HciTrace("host: usb up mounted=%u open=%u task=%lu\r\n",
                 (unsigned)HciTinyUsbIsMounted(&pApp->Usb),
                 (unsigned)pApp->HostOpen,
                 (unsigned long)pApp->Usb.TaskCount);
        HciTargetUsbTrace(&pApp->Target, "usb up", 0U);
    }
    else
    {
        if (pApp->UsbRunning)
        {
            HciAppStartLogPort(pApp);
        }

        HciAppSetHostOpen(pApp, true);
    }

    return true;
}

/*
 * Hand the log to the second CDC function.
 *
 * Only from here, which is the thread that pumps the device stack, because
 * the TinyUSB event queue is protected against this context and no other.
 */
static size_t HciAppLogWrite(void *, const uint8_t *pData, size_t Len)
{
    return HciTinyUsbWrite(HCI_APP_LOG_INTERFACE, pData, Len);
}

/*
 * Say something the moment a terminal opens the log, whatever is queued.
 *
 * Without this, an empty port has three explanations and no way to tell them
 * apart: the port is dead, or nothing was ever written to the log, or
 * everything was written before there was anywhere to put it. A line that
 * appears on open rules out the first, and the counts in it answer the other
 * two, which turns silence into one question rather than three.
 *
 * Written into the ring rather than to the port directly, so it comes out
 * ahead of what was queued rather than in the middle of it.
 */
static void HciAppLogPortOpened(HciApp_t *pApp)
{
    const bool open = HciTinyUsbPortIsOpen(HCI_APP_LOG_INTERFACE);
    if (open == pApp->LogPortOpen)
    {
        return;
    }

    pApp->LogPortOpen = open;
    if (!open)
    {
        return;
    }

    HciSyslogPrint(HciSyslogDefault(),
                   "log: port open, %u octet(s) queued, host=%s",
                   (unsigned)HciSyslogPending(HciSyslogDefault()),
                   pApp->HostType == HCI_APP_HOST_USB ? "usb" : "uart");
}

/*
 * Say what the host link has moved, from the loop that pumps it.
 *
 * Everything above traces once, during bring up, and then the firmware goes
 * quiet for as long as it runs. That is right for a semihosting call, which
 * halts the core, and the log inherited that rule without anyone asking
 * whether it still made sense. It does not: a board that comes up and says
 * nothing more cannot be told from a board whose host link is dead, and on a
 * part whose host is another chip on the same PCB that is the only question
 * worth asking.
 *
 * Not in the octet path, which would flood the ring and put a format call
 * between a packet and its answer. Here instead, on the pump, reporting only
 * when a count moved and no more often than the interval below, with a slow
 * line when nothing moves so that silence still means something specific.
 *
 * Passes rather than milliseconds because this layer has no clock. A pass is
 * one poll interval when the link is idle, which is what the intervals below
 * are counted in, and shorter under traffic, which only makes a report that is
 * already worth making arrive sooner.
 */
#ifndef HCI_APP_LINK_REPORT_PASSES
#define HCI_APP_LINK_REPORT_PASSES 200U
#endif

#ifndef HCI_APP_LINK_QUIET_PASSES
#define HCI_APP_LINK_QUIET_PASSES 2000U
#endif

/*
 * Show the first octets that arrived, once, as hex and as text.
 *
 * Two lines rather than one, because a window long enough to hold a line of
 * somebody's log does not fit both readings on one and the two readings answer
 * different questions. Hex shows the H:4 indicator and length of a real packet
 * and shows noise for what it is. Text is what lets a person recognise a log
 * they have seen before, which no amount of hex would.
 *
 * Hex of the first sixteen only. Past that it stops telling anyone anything
 * the text does not tell them faster.
 */
#define HCI_APP_FIRST_RX_HEX 16U

static void HciAppReportFirstRx(const HciIntrfTransport_t *pHost)
{
    static const char digits[] = "0123456789ABCDEF";
    char line[HCI_INTRF_FIRST_RX_SIZE + 4U];
    size_t at = 0U;

    size_t hexLen = pHost->FirstRxLen;
    if (hexLen > HCI_APP_FIRST_RX_HEX)
    {
        hexLen = HCI_APP_FIRST_RX_HEX;
    }

    for (size_t i = 0U; i < hexLen; i++)
    {
        line[at++] = digits[pHost->FirstRx[i] >> 4];
        line[at++] = digits[pHost->FirstRx[i] & 0x0FU];
        line[at++] = ' ';
    }
    line[at] = '\0';

    HciSyslogPrint(HciSyslogDefault(), "rx first %u hex: %s",
                   (unsigned)pHost->FirstRxLen, line);

    at = 0U;
    for (size_t i = 0U; i < pHost->FirstRxLen; i++)
    {
        const uint8_t octet = pHost->FirstRx[i];
        line[at++] = (octet >= 0x20U && octet < 0x7FU) ? (char)octet : '.';
    }
    line[at] = '\0';

    HciSyslogPrint(HciSyslogDefault(), "rx first text: |%s|", line);
}

static void HciAppReportLink(HciApp_t *pApp)
{
    const HciIntrfTransport_t *pHost = &pApp->Controller.Host;

    pApp->LinkReportPasses++;

    const bool moved = pHost->RxOctetCount != pApp->LinkRxOctets ||
                       pHost->TxOctetCount != pApp->LinkTxOctets;

    if (moved)
    {
        if (pApp->LinkReportPasses < HCI_APP_LINK_REPORT_PASSES)
        {
            return;
        }
    }
    else if (pApp->LinkReportPasses < HCI_APP_LINK_QUIET_PASSES)
    {
        return;
    }

    pApp->LinkReportPasses = 0U;
    pApp->LinkRxOctets = pHost->RxOctetCount;
    pApp->LinkTxOctets = pHost->TxOctetCount;

    /*
     * Here rather than the moment the first octet lands, so the window has had
     * a whole report interval to fill. Reported on the first octet it showed
     * whatever the first read happened to return, which on a busy wire was
     * three octets out of a possible sixty four.
     */
    if (!pApp->LinkFirstRxReported && pHost->FirstRxLen != 0U)
    {
        pApp->LinkFirstRxReported = true;
        HciAppReportFirstRx(pHost);
    }

    /*
     * The parser counts belong here beside the octet counts, because together
     * they say whether the octets are a stream or just traffic. A busy link
     * whose indicator is rejected on nearly every octet is not an H:4 stream at
     * all, and no amount of counting octets alone would have said so.
     */
    HciSyslogPrint(HciSyslogDefault(),
                   "link: %s open=%u rx=%lu tx=%lu rxerr=%lu txerr=%lu "
                   "txbusy=%lu badtype=%lu oversize=%lu retry=%lu",
                   pApp->HostType == HCI_APP_HOST_USB ? "usb" : "uart",
                   (unsigned)pApp->HostOpen,
                   (unsigned long)pHost->RxOctetCount,
                   (unsigned long)pHost->TxOctetCount,
                   (unsigned long)pHost->RxErrorCount,
                   (unsigned long)pHost->TxErrorCount,
                   (unsigned long)pHost->TxBusyCount,
                   (unsigned long)pHost->Parser.InvalidTypeCount,
                   (unsigned long)pHost->Parser.OversizePacketCount,
                   (unsigned long)pHost->Parser.DeliveryRetryCount);

    /*
     * And what the hardware says, which is the half this layer cannot know.
     * Only while nothing has arrived: once octets are moving the pins and the
     * error source have answered their question and repeating them buries the
     * counts that are still worth reading.
     */
    if (pApp->HostType == HCI_APP_HOST_UART && pHost->RxOctetCount == 0U)
    {
        HciTargetUartTrace(&pApp->Target, HCI_APP_UART_DEVICE);
    }
}

static void HciAppDrainLog(HciApp_t *pApp)
{
    HciAppLogPortOpened(pApp);
    HciSyslogDrain(HciSyslogDefault(), HciAppLogWrite, pApp);
}

static void HciAppHostProcess(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return;
    }

    if (pApp->UsbRunning)
    {
        /*
         * Cable attach and detach are recorded by POWER_CLOCK and applied
         * here, in the same context that pumps the device stack, because the
         * TinyUSB event queue is only protected against this context.
         */
        pApp->Target.pOps->UsbPowerProcess(pApp->Target.pContext);

        HciTinyUsbProcess(&pApp->Usb);

        /*
         * Only where the HCI stream is on this port does opening it mean the
         * host is there. With the stream on the UART the port is the log's
         * alone, and whether a terminal is attached to it says nothing about
         * the host, which is another part on the same PCB.
         */
        if (pApp->HostType == HCI_APP_HOST_USB)
        {
            HciAppSetHostOpen(pApp, HciTinyUsbIsOpen(&pApp->Usb));
        }

        HciAppDrainLog(pApp);
    }

    HciControllerProcess(&pApp->Controller);

    HciAppReportLink(pApp);

    if (pApp->UsbRunning)
    {
        HciTinyUsbProcess(&pApp->Usb);
    }
}

bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType, HciTarget_t Target)
{
    if (!HciTargetValid(&Target))
    {
        return false;
    }

    if (pApp == nullptr ||
        (HostType != HCI_APP_HOST_UART && HostType != HCI_APP_HOST_USB) ||
        (s_pApp != nullptr && s_pApp != pApp))
    {
        return false;
    }

    memset(pApp, 0, sizeof(*pApp));
    pApp->HostType = HostType;
    pApp->Target = Target;
    s_pApp = pApp;

    /*
     * The counter readout spans four layers, and the pointers stay valid
     * whatever order those layers are brought up in, so it is wired here
     * before anything else needs it.
     */
    /*
     * Nothing to do for the log. It has been taking lines since before this
     * function was called, including the ones main wrote about which port was
     * chosen and why, and nothing drains it until the device stack runs. The
     * ring holds them until then, which is the whole point of a ring.
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

    bool hostReady;
    if (HostType == HCI_APP_HOST_USB)
    {
        hostReady = HciAppUsbSetup(pApp);
        if (hostReady)
        {
            pApp->pHostIntrf = &pApp->UsbIntrf.DevIntrf;
        }
    }
    else
    {
        hostReady = HciAppInitUart(pApp);

        /*
         * The HCI stream is on the UART and the socket is this part's, so the
         * port is free and the log goes on it. Not being able to set it up is
         * not a bring up failure: an HCI controller that refused to run
         * because it had nowhere to print would be worse than one that runs
         * silently, which is what every build did until now.
         */
        if (hostReady && HCI_USB_SOCKET && HciTargetHasUsb(&pApp->Target) &&
            !HciAppUsbSetup(pApp))
        {
            HciTrace("init: log port setup failed, running without it\r\n");
            pApp->UsbRunning = false;
        }
    }

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

    if (!pApp->Target.pOps->Init(pApp->Target.pContext,
                         &pApp->Runtime,
                         reinterpret_cast<uint8_t *>(pApp->SdcMem),
                         sizeof(pApp->SdcMem),
                         pApp->UsbRunning))
    {
        HciTrace("init: target Init failed\r\n");
        pApp->LastError = -4;
        s_pApp = nullptr;
        return false;
    }

    /*
     * Now that sdc_cfg_set has answered, hand the two pool figures to the
     * counter readout so a host can ask for them. On a sealed dongle the trace
     * that holds them reaches nobody.
     */
    uint32_t sdcRequired = 0U;
    uint32_t sdcCapacity = 0U;
    HciTargetGetSdcMem(&pApp->Target, &sdcRequired, &sdcCapacity);
    HciCountersSetSdcMem(&pApp->Counters, sdcRequired, sdcCapacity);

    HciTaktOsOps_t runtimeOps = {};
    pApp->Target.pOps->GetTaktOsOps(pApp->Target.pContext, &runtimeOps);

    HciTaktOsHostOps_t hostOps = {};
    hostOps.Start = HciAppHostStart;
    hostOps.Process = HciAppHostProcess;
    hostOps.pContext = pApp;
    /*
     * A port with no interrupt that reaches this thread has to be looked at on
     * a tick, and the device stack is one whichever stream is on it. A UART
     * host wakes this thread from its own interrupt as well, so the tick costs
     * it nothing but the passes.
     */
    hostOps.PollIntervalMs = pApp->UsbRunning ? HCI_APP_USB_POLL_MS : 0U;

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

    pApp->Target.pOps->Stop(pApp->Target.pContext);
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
