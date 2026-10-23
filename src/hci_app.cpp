/**-------------------------------------------------------------------------
@file	hci_app.cpp

@brief	Application-level HCI transport, controller, and target integration.

		Initializes runtime transport modes, SDC routing, target services,
	diagnostic USB logging, Host state, and orderly controller shutdown.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "board.h"
#include "hci_app.h"

#include <string.h>
#include "coredev/iopincfg.h"
#include "coredev/interrupt.h"
#include "hci_trace.h"
#include "hci_version.h"
#include "sdc_hci.h"

#if HCI_APP_HAS_IPC
#include "bluetooth/bt_hci_ipc.h"
#endif

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

#if HCI_APP_HAS_USB
#define HCI_USB_WORK_COUNT 16U
#define HCI_USB_WORK_PER_PASS 30U
typedef struct
{
    uint32_t EvtId;
    void *pCtx;
    UsbEvtQueHandler_t Handler;
} HciUsbWork_t;
#endif

static HciApp_t *s_pApp;

#if HCI_APP_HAS_UART
static UART s_HciUart;
static DevIntrf_t *HciAppUartIntrf(void)
{
    return static_cast<DevIntrf_t *>(s_HciUart);
}
static void HciAppUartDisable(void)
{
    s_HciUart.Disable();
}
#else
static DevIntrf_t *HciAppUartIntrf(void)
{
    return nullptr;
}
static void HciAppUartDisable(void)
{
}
#endif

#if HCI_APP_HAS_USB
static UsbdCdc s_HostCdc;
static UsbdCdc s_LogCdc;
#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
static UART s_BridgeUart;
static UsbdCdc s_BridgeCdc;
static const IOPinCfg_t s_BridgeUartPins[] = HCI_BRIDGE_UART_PINS;
#endif
static BtHciUsb s_HciUsb;

/* Preserve queued USB work across stop/start to avoid losing callbacks
 * while the USB process-queued flag remains set. The HCI worker executes
 * these callbacks.
 */
alignas(HciUsbWork_t) static uint8_t s_UsbWorkMem[
    CFIFO_TOTAL_MEMSIZE(HCI_USB_WORK_COUNT, sizeof(HciUsbWork_t))];
static hCFifo_t s_hUsbWork;
#endif

#if HCI_APP_HAS_UART && defined(UART_PINS)
static const IOPinCfg_t s_HciUartPins[] = UART_PINS;
#endif

static void HciAppWake(void *pContext);

#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
/* Receive nRF91 diagnostic UART output on the board-selected
 * bridge UART. Never pass these bytes through HCI H:4.
 */
static int HciBridgeUartEvent(UARTDev_t * const, UART_EVT evt,
                             uint8_t *, int len)
{
    if (evt == UART_EVT_RXDATA || evt == UART_EVT_RXTIMEOUT)
    {
        HciAppWake(s_pApp);
    }
    return len;
}

static bool HciUartBridgeStart(HciApp_t *app)
{
    UARTCfg_t cfg = {};
    cfg.DevNo = HCI_BRIDGE_UART_DEVNO;
    cfg.pIOPinMap = s_BridgeUartPins;
    cfg.NbIOPins = sizeof(s_BridgeUartPins) / sizeof(s_BridgeUartPins[0]);
    cfg.Rate = HCI_BRIDGE_UART_RATE;
    cfg.DataBits = 8;
    cfg.Parity = UART_PARITY_NONE;
    cfg.StopBits = 1;
    cfg.FlowControl = UART_FLWCTRL_NONE;
    cfg.bIntMode = true;
    cfg.IntPrio = HCI_APP_UART_IRQ_PRIORITY;
    cfg.EvtCallback = HciBridgeUartEvent;
    cfg.bFifoBlocking = false;
    cfg.bDMAMode = true;
    cfg.Duplex = UART_DUPLEX_FULL;
    cfg.Mode = UART_MODE_UART;
    cfg.RxMemSize = sizeof(app->BridgeUartRxMem);
    cfg.pRxMem = app->BridgeUartRxMem;
    cfg.TxMemSize = sizeof(app->BridgeUartTxMem);
    cfg.pTxMem = app->BridgeUartTxMem;
    app->BridgeUartReady = s_BridgeUart.Init(cfg);
    app->BridgeBaudRate = app->BridgeUartReady ? cfg.Rate : 0U;
    return app->BridgeUartReady;
}

/* UART RX -> USB CDC TX. Keep an unsent tail when CDC is busy. */
static void HciUartBridgePump(HciApp_t *app)
{
    if (!app->BridgeUartReady || !app->UsbRunning ||
        !s_BridgeCdc.IsPortOpen())
    {
        return;
    }

    /* USB CDC line coding is accepted by IOsonata. Apply host-selected
     * baud rate to the separate bridge UART, never to the HCI UART.
     */
    const UsbCdcLineCoding_t *pLine = s_BridgeCdc.LineCoding();
    if (pLine != nullptr && pLine->dwDTERate > 0U &&
        pLine->dwDTERate != app->BridgeBaudRate)
    {
        const uint32_t actual = s_BridgeUart.Rate(pLine->dwDTERate);
        if (actual != 0U)
        {
            app->BridgeBaudRate = pLine->dwDTERate;
        }
    }

    if (app->BridgePendingOffset == app->BridgePendingLen)
    {
        app->BridgePendingOffset = 0U;
        const int len = s_BridgeUart.Rx(app->BridgePending,
                                           sizeof(app->BridgePending));
        if (len > 0)
        {
            app->BridgePendingLen = (uint16_t)len;
        }
        else
        {
            app->BridgePendingLen = 0U;
        }
    }

    if (app->BridgePendingOffset < app->BridgePendingLen)
    {
        const int sent = s_BridgeCdc.Tx(
            0U, app->BridgePending + app->BridgePendingOffset,
            app->BridgePendingLen - app->BridgePendingOffset);
        if (sent > 0)
        {
            app->BridgePendingOffset += (uint16_t)sent;
        }
    }

    /* CDC RX -> UART TX. Retain the unsent tail under UART backpressure. */
    if (app->BridgeInputOffset == app->BridgeInputLen)
    {
        app->BridgeInputOffset = 0U;
        const int len = s_BridgeCdc.Rx(
            0U, app->BridgeInputPending, sizeof(app->BridgeInputPending));
        app->BridgeInputLen = len > 0 ? (uint16_t)len : 0U;
    }

    if (app->BridgeInputOffset < app->BridgeInputLen)
    {
        const int sent = s_BridgeUart.Tx(
            app->BridgeInputPending + app->BridgeInputOffset,
            app->BridgeInputLen - app->BridgeInputOffset);
        if (sent > 0)
        {
            app->BridgeInputOffset += (uint16_t)sent;
        }
    }
}
#endif

static void HciAppWake(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp != nullptr && pApp->Initialized)
    {
        HciTaktOsWake(&pApp->Runtime, HCI_TAKTOS_EVENT_HOST);
    }
}

#if HCI_APP_HAS_USB
static int HciAppUsbEvent(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    HciAppWake(s_pApp);
    return Len;
}

bool UsbEvtQue(uint32_t EvtId, void *pCtx, UsbEvtQueHandler_t Handler)
{
    if (s_hUsbWork == nullptr || Handler == nullptr)
    {
        return false;
    }
    // Both the USB interrupt and the worker produce events. Publish the
    // complete entry before either can observe or reuse it.
    const uint32_t state = DisableInterrupt();
    HciUsbWork_t *p = reinterpret_cast<HciUsbWork_t *>(CFifoPut(s_hUsbWork));
    if (p != nullptr)
    {
        *p = { EvtId, pCtx, Handler };
    }
    EnableInterrupt(state);
    if (p == nullptr)
    {
        return false;
    }
    HciAppWake(s_pApp);
    return true;
}

static void HciAppUsbWorkExec(void)
{
    for (unsigned count = HCI_USB_WORK_PER_PASS; count > 0U; count--)
    {
        const HciUsbWork_t *p =
            reinterpret_cast<const HciUsbWork_t *>(CFifoPeek(s_hUsbWork));
        if (p == nullptr)
        {
            // Retry the core's process event if a full queue refused it.
            UsbCheckStatus();
            break;
        }
        const HciUsbWork_t work = *p;
        (void)CFifoGet(s_hUsbWork);
        work.Handler(work.EvtId, work.pCtx);
    }
    if (CFifoPeek(s_hUsbWork) != nullptr)
    {
        // A bounded pass must not leave the worker asleep with work queued.
        HciAppWake(s_pApp);
    }
}
#else
static void HciAppUsbWorkExec(void)
{
}
#endif

#if HCI_APP_HAS_UART
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
#endif

#if HCI_APP_HAS_IPC
/*
 * The host is on another core, reached through BtHciIpc (IOsonata). It is a
 * packet interface like native USB: one ring slot is one HCI packet and
 * DevAddr is the packet type, so nothing is framed from a byte stream.
 * HciController is the service provider, so it is the IPC owner: it
 * partitions and formats the shared memory block and the host core attaches
 * to what it finds there.
 */
#ifndef HCI_IPC_MEM_ADDR
#error "board.h must define HCI_IPC_MEM_ADDR, the shared memory block address"
#endif
#ifndef HCI_IPC_MEM_SIZE
#error "board.h must define HCI_IPC_MEM_SIZE, the shared memory block size"
#endif
#ifndef HCI_IPC_NB_PKT
#define HCI_IPC_NB_PKT            8U
#endif
#ifndef HCI_IPC_TX_CHAN
#define HCI_IPC_TX_CHAN           0
#endif
#ifndef HCI_IPC_RX_CHAN
#define HCI_IPC_RX_CHAN           1
#endif
#ifndef HCI_IPC_IRQ_PRIORITY
#define HCI_IPC_IRQ_PRIORITY      6
#endif

static_assert(BT_HCI_IPC_MEMSIZE(HCI_IPC_NB_PKT) <= HCI_IPC_MEM_SIZE,
              "the IPC rings do not fit the shared memory block");
static_assert(BT_HCI_IPC_PKT_MAXLEN >= HCI_MSG_BUFFER_MAX_SIZE &&
              BT_HCI_IPC_PKT_MAXLEN >= HCI_SDC_ISO_PACKET_SIZE,
              "an IPC slot must hold the largest packet the controller sends");

static BtHciIpcIntrf s_HciIpc;

/* RX data and TX room are the two reasons to run the host side again. */
static int HciAppIpcEvent(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    HciAppWake(s_pApp);
    return Len;
}

static bool HciAppInitIpc(HciApp_t *pApp)
{
    BtHciIpcIntrfCfg_t cfg = {};

    cfg.pMemBlk = reinterpret_cast<uint8_t *>(HCI_IPC_MEM_ADDR);
    cfg.MemSize = HCI_IPC_MEM_SIZE;
    cfg.bOwner = true;
    cfg.NbPkt = HCI_IPC_NB_PKT;
    cfg.TxChan = HCI_IPC_TX_CHAN;
    cfg.RxChan = HCI_IPC_RX_CHAN;
    cfg.IntPrio = HCI_IPC_IRQ_PRIORITY;
    cfg.EvtCB = HciAppIpcEvent;

    if (!s_HciIpc.Init(cfg))
    {
        return false;
    }

    pApp->pHostIntrf = s_HciIpc.Data();
    return true;
}

static void HciAppIpcRelease(HciApp_t *pApp)
{
    if (pApp->HostType == HCI_APP_HOST_IPC &&
        pApp->pHostIntrf == s_HciIpc.Data())
    {
        s_HciIpc.Disable();
        pApp->pHostIntrf = nullptr;
    }
}
#else
static bool HciAppInitIpc(HciApp_t *)
{
    return false;
}

static void HciAppIpcRelease(HciApp_t *)
{
}
#endif

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

        case HCI_APP_MODE_IPC:
            *pHostType = HCI_APP_HOST_IPC;
            *pUsbMode = HCI_USB_DESCRIPTOR_LOG_ONLY;
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

#if HCI_APP_HAS_USB
static bool HciAppUsbSetup(HciApp_t *pApp, HciUsbDescriptorMode_t Mode)
{
    if (Mode < HCI_USB_DESCRIPTOR_LOG_ONLY ||
        Mode > HCI_USB_DESCRIPTOR_NATIVE_HCI)
    {
        return false;
    }

    pApp->UsbDescriptorMode = Mode;

    if (s_hUsbWork == nullptr)
    {
        s_hUsbWork = CFifoInit(s_UsbWorkMem, sizeof(s_UsbWorkMem),
            sizeof(HciUsbWork_t), true);
        if (s_hUsbWork == nullptr)
        {
            return false;
        }
    }

    UsbCfg_t usbCfg = {};
    usbCfg.DevNo = 0;
    usbCfg.Mode = USB_MODE_DEVICE;
    usbCfg.Vid = HciUsbDescriptorVid();
    usbCfg.Pid = HciUsbDescriptorPid(Mode);
    usbCfg.DevVer = HCI_CONTROLLER_VERSION_BCD;
    usbCfg.pManufacturer = "I-SYST inc.";
    usbCfg.pProduct = "I-SYST HCI Controller";
    usbCfg.pSerial = HciUsbDescriptorSerial();
    usbCfg.pFuncName = "HCI Controller";
    usbCfg.IntPrio = 7;
    usbCfg.DeviceClass = USB_DEVCLASS_MISC;
    usbCfg.DeviceSubClass = 2U;
    usbCfg.DeviceProtocol = 1U;
    usbCfg.bSelfPowered = false;
    usbCfg.bRemoteWakeup = false;
    usbCfg.bLowPowerSuspend = false;
    usbCfg.MaxPower = 100U;
    usbCfg.EvtHandler = nullptr;
    if (!UsbInit(&usbCfg))
    {
        return false;
    }

    // Register the host class before the diagnostic CDC. IOsonata allocates
    // interfaces/endpoints and assembles the complete configuration descriptor
    // from the class-owned descriptor fragments in registration order.
    if (Mode == HCI_USB_DESCRIPTOR_NATIVE_HCI)
    {
        BtHciUsbCfg_t hciCfg = {};
        hciCfg.bBlocking = true;
        // LE-only SDC: no SCO data path, so no synchronous alternates.
        hciCfg.bSco = false;
        hciCfg.bBulkSerialization = true;
        hciCfg.RxFifoMemSize = sizeof(pApp->HostRxFifoMem);
        hciCfg.pRxFifoMem = pApp->HostRxFifoMem;
        hciCfg.TxFifoMemSize = sizeof(pApp->HostTxFifoMem);
        hciCfg.pTxFifoMem = pApp->HostTxFifoMem;
        hciCfg.DevNo = 0;
        hciCfg.InterfaceString = HCI_USB_STRING_FUNCTION;
        hciCfg.EvtCB = HciAppUsbEvent;
        if (!s_HciUsb.Init(hciCfg))
        {
            UsbDisable(0);
            return false;
        }
    }
    else if (Mode == HCI_USB_DESCRIPTOR_CDC_H4)
    {
        UsbdCdcCfg_t hostCfg = {};
        hostCfg.bBlocking = true;
        hostCfg.RxFifoMemSize = sizeof(pApp->HostRxFifoMem);
        hostCfg.pRxFifoMem = pApp->HostRxFifoMem;
        hostCfg.TxFifoMemSize = sizeof(pApp->HostTxFifoMem);
        hostCfg.pTxFifoMem = pApp->HostTxFifoMem;
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

#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    /* A distinct CDC function: HciController's own trace stays on s_LogCdc. */
    UsbdCdcCfg_t bridgeCfg = {};
    bridgeCfg.bBlocking = false;
    bridgeCfg.RxFifoMemSize = sizeof(pApp->BridgeUsbRxMem);
    bridgeCfg.pRxFifoMem = pApp->BridgeUsbRxMem;
    bridgeCfg.TxFifoMemSize = sizeof(pApp->BridgeUsbTxMem);
    bridgeCfg.pTxFifoMem = pApp->BridgeUsbTxMem;
    bridgeCfg.DevNo = 0;
    bridgeCfg.EvtCB = HciAppUsbEvent;
    if (!s_BridgeCdc.Init(bridgeCfg))
    {
        UsbDisable(0);
        return false;
    }
#endif

    pApp->UsbRunning = true;
    return true;
}
#else
static bool HciAppUsbSetup(HciApp_t *, HciUsbDescriptorMode_t)
{
    return false;
}
#endif

/* Roll back transport peripherals when a later initialization stage fails.
 * This is also used by Stop after its worker has left the runtime.
 */
static void HciAppUartRelease(HciApp_t *pApp)
{
#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    if (pApp->BridgeUartReady)
    {
        s_BridgeUart.Disable();
        pApp->BridgeUartReady = false;
        pApp->BridgeBaudRate = 0U;
    }
    pApp->BridgePendingLen = 0U;
    pApp->BridgePendingOffset = 0U;
    pApp->BridgeInputLen = 0U;
    pApp->BridgeInputOffset = 0U;
#endif
    if (pApp->HostType == HCI_APP_HOST_UART &&
        pApp->pHostIntrf == HciAppUartIntrf())
    {
        HciAppUartDisable();
        pApp->pHostIntrf = nullptr;
    }

    /* Same roll back points serve the IPC host. */
    HciAppIpcRelease(pApp);
}

#if HCI_APP_HAS_USB
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
#else
static void HciAppUsbRelease(HciApp_t *)
{
}
#endif

#ifndef UART_FLOWCTRL
#define HCI_APP_UART_FLOWCTRL UART_FLWCTRL_NONE
#else
#define HCI_APP_UART_FLOWCTRL UART_FLOWCTRL
#endif

static bool HciAppInitUart(HciApp_t *pApp)
{
#if HCI_APP_HAS_UART && defined(UART_PINS)
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
    cfg.RxMemSize = sizeof(pApp->HostRxFifoMem);
    cfg.pRxMem = pApp->HostRxFifoMem;
    cfg.TxMemSize = sizeof(pApp->HostTxFifoMem);
    cfg.pTxMem = pApp->HostTxFifoMem;
    cfg.bDMAMode = true;
    cfg.Duplex = UART_DUPLEX_FULL;
    cfg.Mode = UART_MODE_UART;

    if (!s_HciUart.Init(cfg))
    {
        return false;
    }

    HciTargetUartTrace(&pApp->Target, HCI_APP_UART_DEVICE);
    pApp->pHostIntrf = HciAppUartIntrf();
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

#if HCI_APP_HAS_USB
static bool HciAppUsbHostIsOpen(const HciApp_t *pApp)
{
    if (pApp == nullptr || pApp->HostType != HCI_APP_HOST_USB)
    {
        return false;
    }

    return pApp->UsbHciNative ? s_HciUsb.Rate() != 0U
                              : s_HostCdc.IsPortOpen();
}

static void HciAppStartLogPort(HciApp_t *)
{
    if (!UsbEnable(0))
    {
        // No VBUS at boot is normal for a UART host. Keep the initialized
        // classes and worker alive so a later cable event can connect them.
        HciTrace("log: waiting for USB connection\r\n");
        return;
    }

    HciTrace("log: usb up\r\n");
}
#else
static bool HciAppUsbHostIsOpen(const HciApp_t *)
{
    return false;
}

static void HciAppStartLogPort(HciApp_t *)
{
}
#endif

static bool HciAppHostStart(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return false;
    }

#if HCI_APP_HAS_USB
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

            HciAppUsbWorkExec();

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
#endif
    {
        /* The UART and an IPC host are both open from the start: the owner
         * of an IPC link is always there, and the other core attaches when it
         * is ready. */
        if (pApp->UsbRunning)
        {
            HciAppStartLogPort(pApp);
        }
        HciAppSetHostOpen(pApp, true);
    }

    return true;
}

#if HCI_APP_HAS_USB
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
#endif

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

#if HCI_APP_HAS_USB
/* Consume diagnostic CDC OUT bytes without interpreting them. This prevents
 * the standard blocking RX FIFO from remaining full under host traffic.
 */
static void HciAppDiscardLogInput(void)
{
    uint8_t data[64];

    for (unsigned pass = 0U; pass < 16U; pass++)
    {
        if (s_LogCdc.Rx(0U, data, sizeof(data)) <= 0)
        {
            break;
        }
    }
}

static void HciAppDrainLog(HciApp_t *pApp)
{
    HciAppLogPortOpened(pApp);
    HciAppDiscardLogInput();
    if (pApp->LogPortOpen)
    {
        (void)HciTraceFlush();
    }
}
#else
static void HciAppDrainLog(HciApp_t *)
{
}
#endif

static void HciAppHostProcess(void *pContext)
{
    HciApp_t *pApp = static_cast<HciApp_t *>(pContext);
    if (pApp == nullptr)
    {
        return;
    }

    if (pApp->UsbRunning)
    {
        HciAppUsbWorkExec();

        if (pApp->HostType == HCI_APP_HOST_USB)
        {
            /*
             * Bus suspend is not the end of the HCI session. The host keeps
             * its configuration and its link state across it; a controller
             * packet that cannot go out while the bus sleeps stays pending in
             * the bridge and leaves with the first poll after resume.
             */
            HciAppSetHostOpen(pApp, HciAppUsbHostIsOpen(pApp));
        }
        HciAppDrainLog(pApp);
    }

#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    HciUartBridgePump(pApp);
#endif

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

#if HCI_APP_HAS_USB
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
#endif

#if HCI_APP_HAS_IPC
    if (pApp->HostType == HCI_APP_HOST_IPC)
    {
        return HciControllerInitPacketTransport(&pApp->Controller,
                                                pApp->pHostIntrf,
                                                pApp->HostPacket,
                                                sizeof(pApp->HostPacket),
                                                pApp->ControllerPacket,
                                                sizeof(pApp->ControllerPacket),
                                                pControllerOps);
    }
#endif

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
        pApp->pHostIntrf == HciAppUartIntrf();

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
    else if (hostType == HCI_APP_HOST_IPC)
    {
        hostReady = HciAppInitIpc(pApp);
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

#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    if (hostReady && pApp->UsbRunning && !HciUartBridgeStart(pApp))
    {
        HciTrace("init: nRF91 bridge UART initialization failed\r\n");
        hostReady = false;
    }
#endif

    if (!hostReady)
    {
        HciAppUartRelease(pApp);
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
        HciAppUartRelease(pApp);
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
        HciAppUartRelease(pApp);
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
        HciAppUartRelease(pApp);
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
        if (pApp->HostType == HCI_APP_HOST_UART)
        {
            HciAppUartDisable();
        }
#if HCI_APP_HAS_USB
        else if (pApp->HostType == HCI_APP_HOST_USB)
        {
            s_HostCdc.Disable();
        }
#endif
    }
    HciAppUartRelease(pApp);
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

