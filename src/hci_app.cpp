/**-------------------------------------------------------------------------
@file	hci_app.cpp

@brief	Application-level HCI transport, controller, and target integration.

		Initializes runtime transport modes, SDC routing, target services,
		diagnostic USB logging, Host state, and orderly controller shutdown.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_app.h"

#include <string.h>

#include "board.h"
#include "coredev/iopincfg.h"
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
static UsbdCdc s_HostCdc;
static UsbdCdc s_LogCdc;
static UsbdHci s_HciUsb;

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

static bool HciAppResolveMode(HciAppMode_t Mode,
                              HciAppHost_t *pHostType,
                              HciUsbDescriptorMode_t *pUsbMode)
{
    if (pHostType == nullptr || pUsbMode == nullptr)
    {
        return false;
    }

    switch (Mode)
    {
        case HCI_APP_MODE_UART_H4:
            *pHostType = HCI_APP_HOST_UART;
            *pUsbMode = HCI_USB_DESCRIPTOR_LOG_ONLY;
            return true;

        case HCI_APP_MODE_USB_H4:
            *pHostType = HCI_APP_HOST_USB;
            *pUsbMode = HCI_USB_DESCRIPTOR_CDC_H4;
            return true;

        case HCI_APP_MODE_USB_NATIVE:
            *pHostType = HCI_APP_HOST_USB;
            *pUsbMode = HCI_USB_DESCRIPTOR_NATIVE_HCI;
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

    return pApp->UsbHciNative ? "usb-native" : "usb-h4";
}

static bool HciAppUsbSetup(HciApp_t *pApp, HciUsbDescriptorMode_t Mode)
{
    if (!HciUsbDescriptorSetMode(Mode))
    {
        return false;
    }

    pApp->UsbDescriptorMode = Mode;

    UsbCfg_t usbCfg = {};
    usbCfg.DevNo = 0;
    usbCfg.Vid = HciUsbDescriptorVid();
    usbCfg.Pid = HciUsbDescriptorPid(Mode);
    usbCfg.DevVer = HCI_CONTROLLER_VERSION_BCD;
    usbCfg.pManufacturer = "I-SYST inc.";
    usbCfg.pProduct = "I-SYST HCI Controller";
    usbCfg.pSerial = nullptr;
    usbCfg.pFuncName = "Bluetooth HCI";
    usbCfg.NbCdc = Mode == HCI_USB_DESCRIPTOR_CDC_H4 ? 2 : 1;
    usbCfg.IntPrio = 7;
    usbCfg.bSelfPowered = false;
    usbCfg.bLowPowerSuspend = false;
    usbCfg.MaxPower = 100U;
    usbCfg.DescHandler = HciUsbDescHandler;
    if (!UsbInit(&usbCfg))
    {
        return false;
    }

    // Register the host function before the diagnostic CDC. IOsonata assigns
    // CDC interfaces and endpoints from the remaining USB resources.
    if (Mode == HCI_USB_DESCRIPTOR_NATIVE_HCI)
    {
        UsbdHciCfg_t hciCfg = {};
        hciCfg.bBlocking = true;
        hciCfg.RxFifoMemSize = sizeof(pApp->UsbRxFifoMem);
        hciCfg.pRxFifoMem = pApp->UsbRxFifoMem;
        hciCfg.TxFifoMemSize = sizeof(pApp->UsbTxFifoMem);
        hciCfg.pTxFifoMem = pApp->UsbTxFifoMem;
        hciCfg.DevNo = 0;
        hciCfg.InterfaceString = HCI_USB_STRING_BT;
        hciCfg.EvtCB = HciAppUsbEvent;
        if (!s_HciUsb.Init(hciCfg))
        {
            UsbDisable(0);
            return false;
        }

        UsbdHciDesc_t hciDesc = {};
        if (!s_HciUsb.MakeDesc(&hciDesc, USB_SPEED_FULL) ||
            !HciUsbDescriptorSetHci(&hciDesc))
        {
            UsbDisable(0);
            return false;
        }
    }
    else if (Mode == HCI_USB_DESCRIPTOR_CDC_H4)
    {
        UsbdCdcCfg_t hostCfg = {};
        hostCfg.bBlocking = true;
        hostCfg.RxFifoMemSize = sizeof(pApp->UsbRxFifoMem);
        hostCfg.pRxFifoMem = pApp->UsbRxFifoMem;
        hostCfg.TxFifoMemSize = sizeof(pApp->UsbTxFifoMem);
        hostCfg.pTxFifoMem = pApp->UsbTxFifoMem;
        hostCfg.DevNo = 0;
        hostCfg.EvtCB = HciAppUsbEvent;
        if (!s_HostCdc.Init(hostCfg))
        {
            UsbDisable(0);
            return false;
        }
    }

    UsbdCdcCfg_t logCfg = {};
    logCfg.bBlocking = true;
    logCfg.RxFifoMemSize = sizeof(pApp->LogRxFifoMem);
    logCfg.pRxFifoMem = pApp->LogRxFifoMem;
    logCfg.TxFifoMemSize = sizeof(pApp->LogTxFifoMem);
    logCfg.pTxFifoMem = pApp->LogTxFifoMem;
    logCfg.DevNo = 0;
    logCfg.EvtCB = HciAppUsbEvent;
    if (!s_LogCdc.Init(logCfg))
    {
        UsbDisable(0);
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

    HciTraceSetSink(nullptr, 0U);
    UsbDisable(0);
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
    (void)pApp;
    return false;
#endif
}

bool HciAppUartEarlyInit(HciApp_t *pApp, HciTarget_t Target)
{
    if (pApp == nullptr || !HciTargetValid(&Target) || s_pApp != nullptr)
    {
        return false;
    }

#if !HCI_UART_EARLY_STARTUP
    memset(static_cast<void *>(pApp), 0, sizeof(*pApp));
#endif
    pApp->HostType = HCI_APP_HOST_UART;
    pApp->Mode = HCI_APP_MODE_UART_H4;
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

    return pApp->UsbHciNative ? s_HciUsb.Rate() != 0U
                              : s_HostCdc.IsPortOpen();
}

static void HciAppStartLogPort(HciApp_t *pApp)
{
    if (!UsbEnable(0))
    {
        HciTrace("log: UsbEnable failed\r\n");
        HciAppUsbRelease(pApp);
        return;
    }

    HciTrace("log: usb up\r\n");
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
        if (!UsbEnable(0))
        {
            HciTrace("host: UsbEnable failed\r\n");
            return false;
        }

        for (uint32_t pass = 0U; pass < HCI_APP_USB_SETTLE_PASSES; pass++)
        {
            if (pApp->Runtime.Ops.ProcessMpsl != nullptr)
            {
                pApp->Runtime.Ops.ProcessMpsl(pApp->Runtime.Ops.pContext);
            }

            UsbProcess(0);

            if (UsbConfigured(0))
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
                HciTrace("host: settling pass=%lu\r\n",
                         (unsigned long)pass + 1UL);
            }
        }

        HciAppSetHostOpen(pApp, HciAppUsbHostIsOpen(pApp));
        HciTrace("host: %s up configured=%u open=%u\r\n",
                 HciAppHostName(pApp),
                 (unsigned)UsbConfigured(0),
                 (unsigned)pApp->HostOpen);
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

static void HciAppLogPortOpened(HciApp_t *pApp)
{
    const bool open = s_LogCdc.IsPortOpen();
    if (open == pApp->LogPortOpen)
    {
        return;
    }

    pApp->LogPortOpen = open;
    HciTraceSetSink(open ? s_LogCdc.Data() : nullptr, 0U);
    if (open)
    {
        HciTrace("log: port open, host=%s\r\n", HciAppHostName(pApp));
    }
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
    if (pApp->LogPortOpen)
    {
        (void)HciTraceFlush();
    }
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
        UsbProcess(0);

        if (pApp->HostType == HCI_APP_HOST_USB)
        {
            HciAppSetHostOpen(pApp, HciAppUsbHostIsOpen(pApp));
        }
        HciAppDrainLog(pApp);
    }

    HciControllerProcess(&pApp->Controller);
    HciAppResyncOnIdle(pApp);

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
        pApp->pHostIntrf = s_HciUsb.Data();

        return HciControllerInitPacketTransport(&pApp->Controller,
                                                pApp->pHostIntrf,
                                                pApp->HostPacket,
                                                sizeof(pApp->HostPacket),
                                                pApp->ControllerPacket,
                                                sizeof(pApp->ControllerPacket),
                                                pControllerOps);
    }

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        pApp->pHostIntrf = s_HostCdc.Data();
    }

    return HciControllerInit(&pApp->Controller,
                             pApp->pHostIntrf,
                             pApp->HostPacket,
                             sizeof(pApp->HostPacket),
                             pApp->ControllerPacket,
                             sizeof(pApp->ControllerPacket),
                             pControllerOps);
}

bool HciAppInitMode(HciApp_t *pApp, HciAppMode_t Mode, HciTarget_t Target)
{
    HciAppHost_t hostType;
    HciUsbDescriptorMode_t usbMode;
    if (!HciAppResolveMode(Mode, &hostType, &usbMode) ||
        !HciTargetValid(&Target))
    {
        return false;
    }

    const bool earlyUart =
        pApp != nullptr &&
        Mode == HCI_APP_MODE_UART_H4 &&
        s_pApp == pApp &&
        pApp->HostType == HCI_APP_HOST_UART &&
        pApp->pHostIntrf == &pApp->Uart.DevIntrf;

    if (pApp == nullptr || (s_pApp != nullptr && !earlyUart))
    {
        return false;
    }

    if (!earlyUart)
    {
        memset(static_cast<void *>(pApp), 0, sizeof(*pApp));
        s_pApp = pApp;
    }

    pApp->HostType = hostType;
    pApp->Mode = Mode;
    pApp->Target = Target;
    pApp->UsbHciNative = Mode == HCI_APP_MODE_USB_NATIVE;

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
    if (hostType == HCI_APP_HOST_USB)
    {
        hostReady = HciAppUsbSetup(pApp, usbMode);
    }
    else
    {
        hostReady = earlyUart || HciAppInitUart(pApp);

        if (hostReady && HCI_USB_SOCKET &&
            !HciAppUsbSetup(pApp, HCI_USB_DESCRIPTOR_LOG_ONLY))
        {
            HciTrace("init: log port setup failed, running without it\r\n");
            pApp->UsbRunning = false;
        }
    }

    if (!hostReady)
    {
        HciTrace("init: host interface failed type=%u\r\n", (unsigned)hostType);
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

    if (!pApp->Target.pOps->Init(pApp->Target.pContext,
                                 &pApp->Runtime,
                                 reinterpret_cast<uint8_t *>(pApp->SdcMem),
                                 sizeof(pApp->SdcMem)))
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

bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType, HciTarget_t Target)
{
    if (HostType == HCI_APP_HOST_UART)
    {
        return HciAppInitMode(pApp, HCI_APP_MODE_UART_H4, Target);
    }
    if (HostType != HCI_APP_HOST_USB)
    {
        return false;
    }

#if HCI_USB_HCI_TRANSPORT == HCI_USB_HCI_TRANSPORT_NATIVE
    return HciAppInitMode(pApp, HCI_APP_MODE_USB_NATIVE, Target);
#else
    return HciAppInitMode(pApp, HCI_APP_MODE_USB_H4, Target);
#endif
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

    if (!pApp->UsbHciNative && pApp->pHostIntrf != nullptr)
    {
        DeviceIntrfDisable(pApp->pHostIntrf);
    }
    HciAppUsbRelease(pApp);
    pApp->Target.pOps->Stop(pApp->Target.pContext);
    pApp->pHostIntrf = nullptr;

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
