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
 * The H:4 host transport prefixes one indicator byte. Keep this true even when
 * the image selects native USB, because UART and CDC/H:4 remain supported from
 * the same source tree.
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

#if HCI_USB_HCI_TRANSPORT != HCI_USB_HCI_TRANSPORT_CDC_H4 && \
    HCI_USB_HCI_TRANSPORT != HCI_USB_HCI_TRANSPORT_NATIVE
#error "HCI_USB_HCI_TRANSPORT must select CDC_H4 or NATIVE"
#endif

#ifndef UART_DEVNO
#define HCI_APP_UART_DEVICE       0
#else
#define HCI_APP_UART_DEVICE       UART_DEVNO
#endif

#define HCI_APP_UART_IRQ_PRIORITY 6

#ifndef HCI_SDC_STARTUP_NOP
#define HCI_SDC_STARTUP_NOP 0
#endif

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

#ifndef HCI_USB_SOCKET
#define HCI_USB_SOCKET 0
#endif

static HciUsbDescriptorMode_t HciAppUsbDescriptorMode(HciAppHost_t HostType)
{
    if (HostType != HCI_APP_HOST_USB)
    {
        return HCI_USB_DESCRIPTOR_LOG_ONLY;
    }

#if HCI_USB_HCI_TRANSPORT == HCI_USB_HCI_TRANSPORT_NATIVE
    return HCI_USB_DESCRIPTOR_NATIVE_HCI;
#else
    return HCI_USB_DESCRIPTOR_CDC_H4;
#endif
}

static const char *HciAppHostName(const HciApp_t *pApp)
{
    if (pApp == nullptr || pApp->HostType == HCI_APP_HOST_UART)
    {
        return "uart";
    }

    return pApp->UsbHciNative ? "usb-native" : "usb-h4";
}

/*
 * Bring up the TinyUSB/CDC object that pumps the device stack. With CDC/H:4 it
 * also owns the HCI stream; with native HCI or UART HCI it owns only the log.
 * Native Bluetooth USB is a packet-oriented DevIntrf_t. Its DevAddr selector
 * is the HCI packet type, while UART and CDC are byte-stream DevIntrf_t
 * instances wrapped by the H:4-to-packet DeviceIntrf adapter.
 */
static bool HciAppUsbSetup(HciApp_t *pApp, HciUsbDescriptorMode_t Mode)
{
    if (!HciUsbDescriptorSetMode(Mode))
    {
        return false;
    }

    pApp->UsbDescriptorMode = Mode;
    pApp->LogCdcInterface = HciUsbDescriptorLogCdcInstance(Mode);

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

static void HciAppUsbRelease(HciApp_t *pApp)
{
    if (pApp == nullptr || !pApp->UsbRunning)
    {
        return;
    }

    if (pApp->UsbHciNative)
    {
        HciUsbDeinit(&pApp->NativeUsb);
    }

    HciTinyUsbStop(&pApp->Usb);
    pApp->UsbRunning = false;
}

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

    HciTargetUartTrace(&pApp->Target, HCI_APP_UART_DEVICE);
    pApp->pHostIntrf = &pApp->Uart.DevIntrf;
    return true;
#else
    return false;
#endif
}

bool HciAppUartEarlyInit(HciApp_t *pApp, HciTarget_t Target)
{
    if (pApp == nullptr || !HciTargetValid(&Target) || s_pApp != nullptr)
    {
        return false;
    }

    /*
     * Thingy:91 calls this at main entry with a static HciApp_t that ResetEntry
     * has already zeroed. Do not clear that large object a second time before
     * arming UARTE. Keep the clear for any future non-Thingy caller that uses
     * this API with the previous initialization semantics.
     */
#if BOARD != THINGY91_NRF52840
    memset(pApp, 0, sizeof(*pApp));
#endif
    pApp->HostType = HCI_APP_HOST_UART;
    pApp->Target = Target;
    pApp->UsbHciNative = false;
    s_pApp = pApp;

    if (!HciAppInitUart(pApp))
    {
        pApp->LastError = -2;
        s_pApp = nullptr;
        return false;
    }

    return true;
}

static bool HciAppSuspectFilter(void *pContext,
                                HciH4PacketType_t Type,
                                const uint8_t *pPacket,
                                size_t PacketLen)
{
    const HciApp_t *pApp = static_cast<const HciApp_t *>(pContext);

    if (Type != HCI_H4_PACKET_COMMAND || PacketLen < 3U)
    {
        return false;
    }

    const uint16_t opcode = (uint16_t)pPacket[0] | ((uint16_t)pPacket[1] << 8);
    return HciSdcKnowsCommand(&pApp->Sdc, opcode, pPacket[2]);
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

static bool HciAppUsbHostIsOpen(const HciApp_t *pApp)
{
    if (pApp == nullptr || pApp->HostType != HCI_APP_HOST_USB)
    {
        return false;
    }

    if (pApp->UsbHciNative)
    {
        return HciUsbIsOpen(&pApp->NativeUsb);
    }

    return HciTinyUsbIsOpen(&pApp->Usb);
}

static void HciAppStartLogPort(HciApp_t *pApp)
{
    if (!HciTinyUsbStart(&pApp->Usb))
    {
        HciTrace("log: HciTinyUsbStart failed\r\n");
        HciAppUsbRelease(pApp);
        return;
    }

    if (!pApp->Target.pOps->UsbStart(pApp->Target.pContext))
    {
        HciTrace("log: target UsbStart failed err=%ld\r\n",
                 (long)HciTargetLastError(&pApp->Target));
        HciAppUsbRelease(pApp);
        return;
    }

    HciTrace("log: usb up on cdc %u\r\n", (unsigned)pApp->LogCdcInterface);
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

        for (uint32_t pass = 0U; pass < HCI_APP_USB_SETTLE_PASSES; pass++)
        {
            if (pApp->Runtime.Ops.ProcessMpsl != nullptr)
            {
                pApp->Runtime.Ops.ProcessMpsl(pApp->Runtime.Ops.pContext);
            }

            pApp->Target.pOps->UsbPowerProcess(pApp->Target.pContext);

            if (HciTargetUsbStuck(&pApp->Target))
            {
                HciTargetUsbTrace(&pApp->Target, "storm", pass + 1U);
                HciAppSetHostOpen(pApp, false);
                return false;
            }

            pApp->Target.pOps->UsbPassMark(pApp->Target.pContext);
            HciTinyUsbProcess(&pApp->Usb);
            if (pApp->UsbHciNative)
            {
                HciUsbProcess(&pApp->NativeUsb);
            }

            if (HciTinyUsbIsMounted(&pApp->Usb))
            {
                break;
            }

            if (pApp->Runtime.StopRequested)
            {
                HciTrace("host: settle abandoned, stop requested\r\n");
                break;
            }

            if ((pass % HCI_APP_USB_SETTLE_REPORT) ==
                (HCI_APP_USB_SETTLE_REPORT - 1U))
            {
                HciTargetUsbTrace(&pApp->Target, "settling", pass + 1U);
            }
        }

        HciAppSetHostOpen(pApp, HciAppUsbHostIsOpen(pApp));
        HciTrace("host: %s up mounted=%u open=%u task=%lu\r\n",
                 HciAppHostName(pApp),
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

static size_t HciAppLogWrite(void *pContext, const uint8_t *pData, size_t Len)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return 0U;
    }

    return HciTinyUsbWrite(pApp->LogCdcInterface, pData, Len);
}

static void HciAppLogPortOpened(HciApp_t *pApp)
{
    const bool open = HciTinyUsbPortIsOpen(pApp->LogCdcInterface);
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
                   HciAppHostName(pApp));
}

#ifndef HCI_APP_LINK_IDLE_PASSES
#define HCI_APP_LINK_IDLE_PASSES 4U
#endif

static void HciAppResyncOnIdle(HciApp_t *pApp)
{
    if (!HciControllerUsesH4(&pApp->Controller))
    {
        return;
    }

    HciIntrfTransport_t *pHost = &pApp->Controller.Host;

    if (pHost->RxOctetCount != pApp->LinkIdleOctets)
    {
        pApp->LinkIdleOctets = pHost->RxOctetCount;
        pApp->LinkIdlePasses = 0U;
        return;
    }

    if (pApp->LinkIdlePasses < HCI_APP_LINK_IDLE_PASSES)
    {
        pApp->LinkIdlePasses++;
        if (pApp->LinkIdlePasses == HCI_APP_LINK_IDLE_PASSES)
        {
            HciIntrfTransportIdle(pHost);
        }
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
        pApp->Target.pOps->UsbPowerProcess(pApp->Target.pContext);

        if (HciTargetUsbStuck(&pApp->Target))
        {
            HciTargetUsbTrace(&pApp->Target, "runtime storm", 0U);
            if (pApp->HostType == HCI_APP_HOST_USB)
            {
                HciAppSetHostOpen(pApp, false);
            }
            HciTaktOsHostDown(&pApp->Runtime);
            return;
        }

        pApp->Target.pOps->UsbPassMark(pApp->Target.pContext);
        HciTinyUsbProcess(&pApp->Usb);
        if (pApp->UsbHciNative)
        {
            HciUsbProcess(&pApp->NativeUsb);
        }

        if (pApp->HostType == HCI_APP_HOST_USB)
        {
            HciAppSetHostOpen(pApp, HciAppUsbHostIsOpen(pApp));
        }

        HciAppDrainLog(pApp);
    }

    HciControllerProcess(&pApp->Controller);
    HciAppResyncOnIdle(pApp);

    if (pApp->UsbRunning)
    {
        if (pApp->UsbHciNative)
        {
            HciUsbProcess(&pApp->NativeUsb);
        }
        HciTinyUsbProcess(&pApp->Usb);
    }
}

static bool HciAppControllerInit(HciApp_t *pApp,
                                 const HciControllerOps_t *pControllerOps)
{
    if (pApp == nullptr || pControllerOps == nullptr)
    {
        return false;
    }

    if (pApp->UsbHciNative)
    {
        if (!HciUsbInit(&pApp->NativeUsb, HciAppUsbEvent))
        {
            return false;
        }

        pApp->pHostIntrf = HciUsbGetDeviceIntrf(&pApp->NativeUsb);
        if (pApp->pHostIntrf == nullptr)
        {
            HciUsbDeinit(&pApp->NativeUsb);
            return false;
        }

        return HciControllerInitPacketTransport(&pApp->Controller,
                                                pApp->pHostIntrf,
                                                pApp->HostPacket,
                                                sizeof(pApp->HostPacket),
                                                pApp->ControllerPacket,
                                                sizeof(pApp->ControllerPacket),
                                                pControllerOps);
    }

    return HciControllerInit(&pApp->Controller,
                             pApp->pHostIntrf,
                             pApp->HostPacket,
                             sizeof(pApp->HostPacket),
                             pApp->ControllerPacket,
                             sizeof(pApp->ControllerPacket),
                             pControllerOps);
}

bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType, HciTarget_t Target)
{
    if (!HciTargetValid(&Target) ||
        (HostType == HCI_APP_HOST_USB && !HciTargetHasUsb(&Target)))
    {
        return false;
    }

    const bool earlyUart =
        pApp != nullptr &&
        HostType == HCI_APP_HOST_UART &&
        s_pApp == pApp &&
        pApp->HostType == HCI_APP_HOST_UART &&
        pApp->pHostIntrf == &pApp->Uart.DevIntrf;

    if (pApp == nullptr ||
        (HostType != HCI_APP_HOST_UART && HostType != HCI_APP_HOST_USB) ||
        (s_pApp != nullptr && !earlyUart))
    {
        return false;
    }

    if (!earlyUart)
    {
        memset(pApp, 0, sizeof(*pApp));
        s_pApp = pApp;
    }

    pApp->HostType = HostType;
    pApp->Target = Target;
    pApp->UsbHciNative =
        HostType == HCI_APP_HOST_USB &&
        HCI_USB_HCI_TRANSPORT == HCI_USB_HCI_TRANSPORT_NATIVE;

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
    HciSdcNrfxlibQueueStartupNop(&pApp->Sdc);
#endif

    bool hostReady;
    if (HostType == HCI_APP_HOST_USB)
    {
        const HciUsbDescriptorMode_t Mode = HciAppUsbDescriptorMode(HostType);
        hostReady = HciAppUsbSetup(pApp, Mode);
        if (hostReady && !pApp->UsbHciNative)
        {
            pApp->pHostIntrf = &pApp->UsbIntrf.DevIntrf;
        }
    }
    else
    {
        hostReady = earlyUart || HciAppInitUart(pApp);

        if (hostReady && HCI_USB_SOCKET && HciTargetHasUsb(&pApp->Target) &&
            !HciAppUsbSetup(pApp, HCI_USB_DESCRIPTOR_LOG_ONLY))
        {
            HciTrace("init: log port setup failed, running without it\r\n");
            pApp->UsbRunning = false;
        }
    }

    if (!hostReady)
    {
        HciTrace("init: host interface failed type=%u\r\n", (unsigned)HostType);
        pApp->LastError = -2;
        HciAppUsbRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    const HciControllerOps_t *pControllerOps = HciSdcGetControllerOps(&pApp->Sdc);
    if (!HciAppControllerInit(pApp, pControllerOps))
    {
        HciTrace("init: controller transport failed host=%s\r\n",
                 HciAppHostName(pApp));
        pApp->LastError = -3;
        HciAppUsbRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    if (HciControllerUsesH4(&pApp->Controller))
    {
#if BOARD == THINGY91_NRF52840
        if (HostType == HCI_APP_HOST_UART)
        {
            HciControllerSetH4StartupResetSync(&pApp->Controller, true);
        }
#endif

        HciIntrfTransportSetSuspectFilter(&pApp->Controller.Host,
                                          HciAppSuspectFilter,
                                          pApp);
    }

    if (!pApp->Target.pOps->Init(pApp->Target.pContext,
                                 &pApp->Runtime,
                                 reinterpret_cast<uint8_t *>(pApp->SdcMem),
                                 sizeof(pApp->SdcMem),
                                 pApp->UsbRunning))
    {
        HciTrace("init: target Init failed\r\n");
        pApp->LastError = -4;
        HciAppUsbRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

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
    hostOps.PollIntervalMs = pApp->UsbRunning ? HCI_APP_USB_POLL_MS : 0U;

    if (!HciTaktOsInit(&pApp->Runtime, &runtimeOps, &hostOps))
    {
        HciTrace("init: HciTaktOsInit failed\r\n");
        pApp->LastError = -5;
        HciAppUsbRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    HciTrace("init: ok host=%s\r\n", HciAppHostName(pApp));
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

    if (!HciTaktOsWaitStopped(&pApp->Runtime, HCI_APP_STOP_TIMEOUT_MS))
    {
        HciTrace("stop: runtime still running, target and app left owned\r\n");
        pApp->LastError = -6;
        return;
    }

    /*
     * Native USB owns pHostIntrf inside NativeUsb. Release the controller's
     * DeviceIntrf reference before HciUsbDeinit clears that object. H:4 hosts
     * still use the physical UART/CDC DevIntrf and keep their existing final
     * disable below.
     */
    if (pApp->UsbHciNative && pApp->HostOpen)
    {
        HciAppSetHostOpen(pApp, false);
    }

    pApp->Target.pOps->Stop(pApp->Target.pContext);
    HciAppUsbRelease(pApp);

    if (!pApp->UsbHciNative && pApp->pHostIntrf != nullptr)
    {
        DeviceIntrfDisable(pApp->pHostIntrf);
    }
    if (pApp->UsbHciNative)
    {
        pApp->pHostIntrf = nullptr;
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
