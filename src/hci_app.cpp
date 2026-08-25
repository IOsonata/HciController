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

/*
 * Output transport for the diagnostic CDC function. SysLog owns the queued
 * log records in its CFifo. This object only owns one record that DeviceIntrf
 * has accepted and TinyUSB has not completely consumed yet.
 */
typedef struct
{
    HciApp_t *pApp;
    uint8_t Pending[HCI_TRACE_RECORD_SIZE];
    size_t PendingOffset;
    size_t PendingLen;
} HciAppLogIntrf_t;

static HciAppLogIntrf_t s_LogIntrfState;
static DevIntrf_t s_LogIntrf;

static void HciAppLogPump(void)
{
    if (s_LogIntrfState.pApp == nullptr ||
        s_LogIntrfState.PendingOffset >= s_LogIntrfState.PendingLen)
    {
        s_LogIntrfState.PendingOffset = 0U;
        s_LogIntrfState.PendingLen = 0U;
        return;
    }

    const size_t remaining =
        s_LogIntrfState.PendingLen - s_LogIntrfState.PendingOffset;
    const size_t count = HciTinyUsbWrite(
        s_LogIntrfState.pApp->LogCdcInterface,
        &s_LogIntrfState.Pending[s_LogIntrfState.PendingOffset],
        remaining);

    if (count > remaining)
    {
        return;
    }

    s_LogIntrfState.PendingOffset += count;
    if (s_LogIntrfState.PendingOffset == s_LogIntrfState.PendingLen)
    {
        s_LogIntrfState.PendingOffset = 0U;
        s_LogIntrfState.PendingLen = 0U;
    }
}

static void HciAppLogDisable(DevIntrf_t *)
{
}

static void HciAppLogEnable(DevIntrf_t *)
{
}

static uint32_t HciAppLogGetRate(DevIntrf_t *)
{
    return 0U;
}

static uint32_t HciAppLogSetRate(DevIntrf_t *, uint32_t)
{
    return 0U;
}

static bool HciAppLogStartRx(DevIntrf_t *, uint32_t)
{
    return false;
}

static int HciAppLogRxData(DevIntrf_t *, uint8_t *, int)
{
    return 0;
}

static void HciAppLogStopRx(DevIntrf_t *)
{
}

static bool HciAppLogStartTx(DevIntrf_t *, uint32_t)
{
    return s_LogIntrfState.pApp != nullptr &&
           s_LogIntrfState.PendingLen == 0U;
}

static int HciAppLogTxData(DevIntrf_t *, const uint8_t *pData, int DataLen)
{
    if (s_LogIntrfState.pApp == nullptr || pData == nullptr || DataLen <= 0 ||
        DataLen > (int)sizeof(s_LogIntrfState.Pending) ||
        s_LogIntrfState.PendingLen != 0U)
    {
        return 0;
    }

    memcpy(s_LogIntrfState.Pending, pData, (size_t)DataLen);
    s_LogIntrfState.PendingOffset = 0U;
    s_LogIntrfState.PendingLen = (size_t)DataLen;
    HciAppLogPump();
    return DataLen;
}

static void HciAppLogStopTx(DevIntrf_t *)
{
}

static void HciAppLogReset(DevIntrf_t *)
{
    s_LogIntrfState.PendingOffset = 0U;
    s_LogIntrfState.PendingLen = 0U;
}

static void HciAppLogPowerOff(DevIntrf_t *)
{
}

static void *HciAppLogGetHandle(DevIntrf_t *)
{
    return nullptr;
}

static void HciAppLogIntrfInit(HciApp_t *pApp)
{
    memset(&s_LogIntrfState, 0, sizeof(s_LogIntrfState));

    s_LogIntrfState.pApp = pApp;
    s_LogIntrf.pDevData = &s_LogIntrfState;
    s_LogIntrf.IntPrio = 0;
    s_LogIntrf.EvtCB = nullptr;
    s_LogIntrf.MaxRetry = 0;
    s_LogIntrf.Type = DEVINTRF_TYPE_USB;
    s_LogIntrf.bDma = false;
    s_LogIntrf.bIntEn = false;
    s_LogIntrf.Disable = HciAppLogDisable;
    s_LogIntrf.Enable = HciAppLogEnable;
    s_LogIntrf.GetRate = HciAppLogGetRate;
    s_LogIntrf.SetRate = HciAppLogSetRate;
    s_LogIntrf.StartRx = HciAppLogStartRx;
    s_LogIntrf.RxData = HciAppLogRxData;
    s_LogIntrf.StopRx = HciAppLogStopRx;
    s_LogIntrf.StartTx = HciAppLogStartTx;
    s_LogIntrf.TxData = HciAppLogTxData;
    s_LogIntrf.TxSrData = HciAppLogTxData;
    s_LogIntrf.StopTx = HciAppLogStopTx;
    s_LogIntrf.Reset = HciAppLogReset;
    s_LogIntrf.PowerOff = HciAppLogPowerOff;
    s_LogIntrf.GetHandle = HciAppLogGetHandle;
    atomic_flag_clear(&s_LogIntrf.bBusy);
    atomic_store(&s_LogIntrf.bTxReady, true);
    atomic_store(&s_LogIntrf.bNoStop, false);
    atomic_store(&s_LogIntrf.EnCnt, 1);

    HciTraceSetSink(&s_LogIntrf, 0U);
}

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

    HciAppLogIntrfInit(pApp);
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
    s_LogIntrfState.pApp = nullptr;
    s_LogIntrfState.PendingOffset = 0U;
    s_LogIntrfState.PendingLen = 0U;

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
    memset(pApp, 0, sizeof(*pApp));
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

    return pApp->UsbHciNative ? HciUsbIsOpen(&pApp->NativeUsb)
                              : HciTinyUsbIsOpen(&pApp->Usb);
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

static void HciAppLogPortOpened(HciApp_t *pApp)
{
    const bool open = HciTinyUsbPortIsOpen(pApp->LogCdcInterface);
    if (open == pApp->LogPortOpen)
    {
        return;
    }

    pApp->LogPortOpen = open;
    if (open)
    {
        HciTrace("log: port open, %lu record(s) queued, %lu dropped, host=%s\r\n",
                 (unsigned long)HciTracePending(),
                 (unsigned long)HciTraceDropped(),
                 HciAppHostName(pApp));
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
    HciAppLogPump();

    if (pApp->LogPortOpen && s_LogIntrfState.PendingLen == 0U)
    {
        (void)HciTraceFlush();
        HciAppLogPump();
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

bool HciAppInitMode(HciApp_t *pApp, HciAppMode_t Mode, HciTarget_t Target)
{
    HciAppHost_t hostType;
    HciUsbDescriptorMode_t usbMode;
    if (!HciAppResolveMode(Mode, &hostType, &usbMode) ||
        !HciTargetValid(&Target) ||
        (hostType == HCI_APP_HOST_USB && !HciTargetHasUsb(&Target)))
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
        memset(pApp, 0, sizeof(*pApp));
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
    if (pApp != nullptr && pApp->Initialized)
    {
        HciTaktOsThread(&pApp->Runtime);
    }
}

bool HciAppHostIsOpen(const HciApp_t *pApp)
{
    return pApp != nullptr && pApp->HostOpen;
}
