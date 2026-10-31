/**-------------------------------------------------------------------------
@file	hci_app.cpp

@brief	Application-level HCI transport, controller, and target integration.

		Initializes the runtime, the SDC routing, the target services, the
		host transport the project linked, the host state, and the orderly
		controller shutdown. The transports are in hci_host_uart.cpp,
		hci_host_usb.cpp and hci_host_ipc.cpp.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "board.h"
#include "hci_app.h"

#include <new>
#include <string.h>
#include "hci_trace.h"
#include "hci_version.h"
#include "sdc_hci.h"

static_assert(HCI_APP_PACKET_SIZE + 1U <= HCI_INTRF_TX_STREAM_SIZE,
              "controller packet plus indicator must fit the transport stream");
static_assert(HCI_APP_PACKET_SIZE >= HCI_MSG_BUFFER_MAX_SIZE,
              "controller packet must hold the largest SDC message");
static_assert(HCI_APP_PACKET_SIZE >= HCI_SDC_ISO_PACKET_SIZE,
              "controller packet must hold the largest configured ISO SDU");
static_assert(HCI_SDC_ACL_TRACK_HANDLES >=
                  HCI_SDC_PERIPHERAL_COUNT + HCI_SDC_CENTRAL_COUNT,
              "HCI_SDC_ACL_TRACK_HANDLES is smaller than the link count");

#ifndef HCI_SDC_STARTUP_NOP
#define HCI_SDC_STARTUP_NOP 0
#endif
#ifndef HCI_APP_STOP_TIMEOUT_MS
#define HCI_APP_STOP_TIMEOUT_MS 250U
#endif
#ifndef HCI_APP_USB_POLL_MS
#define HCI_APP_USB_POLL_MS 5U
#endif
#ifndef HCI_USB_SOCKET
#define HCI_USB_SOCKET 0
#endif

static HciApp_t *s_pApp;

/*
 * Host transports a project did not link. Every entry reports the transport
 * absent; the strong definitions are in hci_host_uart.cpp, hci_host_usb.cpp
 * and hci_host_ipc.cpp.
 */
__attribute__((weak)) bool HciHostUartInit(HciApp_t *) { return false; }
__attribute__((weak)) void HciHostUartRelease(HciApp_t *) {}
__attribute__((weak)) bool HciHostUartLogOpen(HciApp_t *) { return false; }
__attribute__((weak)) bool HciHostUsbSetup(HciApp_t *) { return false; }
__attribute__((weak)) void HciHostUsbRelease(HciApp_t *) {}
__attribute__((weak)) bool HciHostUsbStart(HciApp_t *) { return false; }
__attribute__((weak)) void HciHostUsbStartLogPort(HciApp_t *) {}
__attribute__((weak)) void HciHostUsbProcess(HciApp_t *) {}
__attribute__((weak)) bool HciHostUsbIsOpen(const HciApp_t *) { return false; }
__attribute__((weak)) DevIntrf_t *HciHostUsbIntrf(const HciApp_t *) { return nullptr; }
__attribute__((weak)) bool HciHostUsbIsPacket(const HciApp_t *) { return false; }
__attribute__((weak)) bool HciHostIpcInit(HciApp_t *) { return false; }
__attribute__((weak)) void HciHostIpcRelease(HciApp_t *) {}

/* A target that does not decode its UART prints nothing for it. */
__attribute__((weak)) void HciTargetUartTrace(uint8_t) {}

void HciAppWake(void)
{
    HciApp_t *pApp = s_pApp;
    if (pApp != nullptr && pApp->Initialized)
    {
        HciTaktOsWake(&pApp->Runtime, HCI_TAKTOS_EVENT_HOST);
    }
}

static bool HciAppResolveMode(HciAppMode_t Mode, HciAppHost_t *pHostType)
{
    switch (Mode)
    {
        case HCI_APP_MODE_UART_H4:
            *pHostType = HCI_APP_HOST_UART;
            return true;

        case HCI_APP_MODE_USB_H4:
        case HCI_APP_MODE_USB_NATIVE:
            *pHostType = HCI_APP_HOST_USB;
            return true;

        case HCI_APP_MODE_IPC:
            *pHostType = HCI_APP_HOST_IPC;
            return true;

        default:
            return false;
    }
}

static const char *HciAppHostName(const HciApp_t *pApp)
{
    if (pApp == nullptr || pApp->HostType == HCI_APP_HOST_UART)
    {
        return "uart";
    }

    if (pApp->HostType == HCI_APP_HOST_IPC)
    {
        return "ipc";
    }

    return pApp->UsbHciNative ? "usb-native" : "usb-h4";
}

/* The host transport in use, by its file. */
static bool HciAppHostInit(HciApp_t *pApp)
{
    switch (pApp->HostType)
    {
        case HCI_APP_HOST_UART:
            return HciHostUartInit(pApp);
        case HCI_APP_HOST_USB:
            return HciHostUsbSetup(pApp);
        case HCI_APP_HOST_IPC:
            return HciHostIpcInit(pApp);
        default:
            return false;
    }
}

/* Roll back the transports when a later initialization stage fails. This
 * is also used by Stop after its worker has left the runtime. Each release
 * is a no-op for a transport that is not up.
 */
static void HciAppHostRelease(HciApp_t *pApp)
{
    HciHostUartRelease(pApp);
    HciHostIpcRelease(pApp);
    HciHostUsbRelease(pApp);
    pApp->pHostIntrf = nullptr;
}

bool HciAppUartEarlyInit(HciApp_t *pApp)
{
    if (pApp == nullptr || s_pApp != nullptr)
    {
        return false;
    }

#if !HCI_UART_EARLY_STARTUP
    new (pApp) HciApp_t();
#endif
    pApp->HostType = HCI_APP_HOST_UART;
    pApp->Mode = HCI_APP_MODE_UART_H4;
    pApp->UsbHciNative = false;
    s_pApp = pApp;

    if (!HciHostUartInit(pApp))
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
    return HciSdcKnowsCommand(pApp->Sdc, opcode, pPacket[2]);
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

bool HciAppHostStart(void)
{
    HciApp_t *pApp = s_pApp;
    if (pApp == nullptr)
    {
        return false;
    }

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        if (!HciHostUsbStart(pApp))
        {
            return false;
        }
        HciAppSetHostOpen(pApp, HciHostUsbIsOpen(pApp));
        HciTrace("host: %s open=%u\r\n", HciAppHostName(pApp), (unsigned)pApp->HostOpen);
        return true;
    }

    /* The UART and an IPC host are both open from the start: the owner of
     * an IPC link is always there, and the other core attaches when it is
     * ready. The log port rides along on a board with a socket. */
    if (pApp->UsbRunning)
    {
        HciHostUsbStartLogPort(pApp);
    }
    HciAppSetHostOpen(pApp, true);
    return true;
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

void HciAppHostProcess(void)
{
    HciApp_t *pApp = s_pApp;
    if (pApp == nullptr)
    {
        return;
    }

    if (pApp->UsbRunning)
    {
        HciHostUsbProcess(pApp);

        if (pApp->HostType == HCI_APP_HOST_USB)
        {
            /*
             * Bus suspend is not the end of the HCI session. The host keeps
             * its configuration and its link state across it; a controller
             * packet that cannot go out while the bus sleeps stays pending in
             * the bridge and leaves with the first poll after resume.
             */
            HciAppSetHostOpen(pApp, HciHostUsbIsOpen(pApp));
        }
    }

    HciControllerProcess(&pApp->Controller);
    if (pApp->Controller.ProcessAgain)
    {
        HciAppWake();
    }
    HciAppResyncOnIdle(pApp);
}

static bool HciAppControllerInit(HciApp_t *pApp, DevIntrf_t *pCtlrIntrf)
{
    if (pApp == nullptr || pCtlrIntrf == nullptr || pApp->pHostIntrf == nullptr)
    {
        return false;
    }

    /* Native USB and the IPC rings carry whole packets; the UART and CDC
     * carry the H:4 byte stream the bridge frames. */
    const bool packet = pApp->HostType == HCI_APP_HOST_IPC ||
                        (pApp->HostType == HCI_APP_HOST_USB && HciHostUsbIsPacket(pApp));

    if (packet)
    {
        return HciControllerInitPacketTransport(&pApp->Controller,
                                                pApp->pHostIntrf,
                                                pApp->HostPacket,
                                                sizeof(pApp->HostPacket),
                                                pApp->ControllerPacket,
                                                sizeof(pApp->ControllerPacket),
                                                pCtlrIntrf);
    }

    return HciControllerInit(&pApp->Controller,
                             pApp->pHostIntrf,
                             pApp->HostPacket,
                             sizeof(pApp->HostPacket),
                             pApp->ControllerPacket,
                             sizeof(pApp->ControllerPacket),
                             pCtlrIntrf);
}

bool HciAppInitMode(HciApp_t *pApp, HciAppMode_t Mode)
{
    HciAppHost_t hostType;
    if (!HciAppResolveMode(Mode, &hostType))
    {
        return false;
    }

    const bool earlyUart =
        pApp != nullptr &&
        Mode == HCI_APP_MODE_UART_H4 &&
        s_pApp == pApp &&
        pApp->HostType == HCI_APP_HOST_UART &&
        pApp->pHostIntrf != nullptr;

    if (pApp == nullptr || (s_pApp != nullptr && !earlyUart))
    {
        return false;
    }

    if (!earlyUart)
    {
        new (pApp) HciApp_t();
        s_pApp = pApp;
    }

    pApp->HostType = hostType;
    pApp->Mode = Mode;
    pApp->UsbHciNative = Mode == HCI_APP_MODE_USB_NATIVE;

    /* An IPC host leaves the UART idle: use it for the trace, before the
     * rest of init so a failure there is seen. */
    if (hostType == HCI_APP_HOST_IPC)
    {
        (void)HciHostUartLogOpen(pApp);
    }
    HciCountersInit(&pApp->Counters, pApp->Sdc, &pApp->Controller);

    if (!pApp->Sdc.Init(pApp->CommandEvent, sizeof(pApp->CommandEvent), &pApp->Counters))
    {
        HciTraceError("init: HciSdcNrfxlibInit failed\r\n");
        pApp->LastError = -1;
        s_pApp = nullptr;
        return false;
    }

#if HCI_SDC_STARTUP_NOP
    HciSdcNrfxlibQueueStartupNop(pApp->Sdc);
#endif

    bool hostReady = earlyUart || HciAppHostInit(pApp);

    /* A UART or IPC host on a board with a USB socket gets the log port
     * when the USB transport is linked; without it there is simply no log
     * port, which is not a failure. */
    if (hostReady && hostType != HCI_APP_HOST_USB && HCI_USB_SOCKET &&
        !HciHostUsbSetup(pApp))
    {
        HciTraceError("init: log port setup failed, running without it\r\n");
        pApp->UsbRunning = false;
    }

    if (!hostReady)
    {
        HciAppHostRelease(pApp);
        HciTraceError("init: host interface failed type=%u\r\n", (unsigned)hostType);
        pApp->LastError = -2;
        s_pApp = nullptr;
        return false;
    }

    if (!HciAppControllerInit(pApp, pApp->Sdc))
    {
        HciTraceError("init: controller transport failed host=%s\r\n",
                 HciAppHostName(pApp));
        pApp->LastError = -3;
        HciAppHostRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    if (HciControllerUsesH4(&pApp->Controller))
    {
#if HCI_H4_STARTUP_RESET_SYNC
        if (hostType == HCI_APP_HOST_UART)
        {
            HciControllerSetH4StartupResetSync(&pApp->Controller, true);
        }
#endif
        HciIntrfTransportSetSuspectFilter(&pApp->Controller.Host,
                                          HciAppSuspectFilter,
                                          pApp);
    }

    if (!HciTargetInit(&pApp->Runtime,
                       reinterpret_cast<uint8_t *>(pApp->SdcMem),
                       sizeof(pApp->SdcMem)))
    {
        HciTraceError("init: target Init failed\r\n");
        pApp->LastError = -4;
        HciAppHostRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    uint32_t sdcRequired = 0U;
    uint32_t sdcCapacity = 0U;
    HciTargetGetSdcMem(&sdcRequired, &sdcCapacity);
    HciCountersSetSdcMem(&pApp->Counters, sdcRequired, sdcCapacity);

    if (!HciTaktOsInit(&pApp->Runtime,
                       pApp->UsbRunning ? HCI_APP_USB_POLL_MS : 0U))
    {
        HciTraceError("init: HciTaktOsInit failed\r\n");
        pApp->LastError = -5;
        HciAppHostRelease(pApp);
        s_pApp = nullptr;
        return false;
    }

    HciTrace("init: ok host=%s\r\n", HciAppHostName(pApp));
    pApp->Initialized = true;
    return true;
}

bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType)
{
    switch (HostType)
    {
        case HCI_APP_HOST_UART:
            return HciAppInitMode(pApp, HCI_APP_MODE_UART_H4);
        case HCI_APP_HOST_USB:
            return HciAppInitMode(pApp, HCI_APP_MODE_USB_NATIVE);
        case HCI_APP_HOST_IPC:
            return HciAppInitMode(pApp, HCI_APP_MODE_IPC);
        default:
            return false;
    }
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

    if (pApp->HostOpen)
    {
        HciAppSetHostOpen(pApp, false);
    }

    HciAppHostRelease(pApp);
    HciTargetStop();

    pApp->Initialized = false;
    s_pApp = nullptr;
}

void HciAppThread(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp != nullptr && pApp->Initialized)
    {
        HciTaktOsThread(&pApp->Runtime);
    }
}

bool HciAppHostIsOpen(const HciApp_t *pApp)
{
    return pApp != nullptr && pApp->HostOpen;
}
