/**-------------------------------------------------------------------------
@file	hci_host_usb.cpp

@brief	USB host transport of HciController and the diagnostic log port.

		The IOsonata USB device stack with, as the mode asks, the native HCI
		function (hci_usb.h) or a CDC function carrying H:4, always the
		diagnostic CDC port the trace goes to, and on a board that has one
		the UART bridge to a neighbour part. A UART or IPC host on a board
		with a USB socket links this file for the log port alone. A part
		without USB does not link it and the weak defaults of hci_app.cpp
		report USB absent.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "board.h"
#include "hci_app.h"

#include "coredev/interrupt.h"
#include "coredev/iopincfg.h"
#include "coredev/uart.h"
#include "hci_trace.h"
#include "hci_usb.h"
#include "hci_version.h"
#include "usb/usbd_cdc.h"

#ifndef HCI_USB_SETTLE_PASSES
#define HCI_USB_SETTLE_PASSES   200000U
#endif
#ifndef HCI_USB_SETTLE_REPORT
#define HCI_USB_SETTLE_REPORT   50000U
#endif
#ifndef HCI_UART_IRQ_PRIORITY
#define HCI_UART_IRQ_PRIORITY   6
#endif

/* Diagnostic CDC input is consumed without processing. Keep two USB RX
 * packet slots; its TX log retention uses the full FIFO. */
#define HCI_USB_LOG_RX_MEM_SIZE \
    USB_INTRF_RXMEM_SIZE(2U, USBD_CDC_TRANS_WORDS * sizeof(uint32_t))

#define HCI_USB_WORK_COUNT      16U
#define HCI_USB_WORK_PER_PASS   30U

typedef struct
{
    uint32_t EvtId;
    void *pCtx;
    UsbEvtQueHandler_t Handler;
} HciUsbWork_t;

static UsbdCdc s_HostCdc;
static UsbdCdc s_LogCdc;
static BtHciUsb s_HciUsb;

alignas(4) static uint8_t s_LogRxFifoMem[HCI_USB_LOG_RX_MEM_SIZE];
alignas(4) static uint8_t s_LogTxFifoMem[HCI_APP_FIFO_MEM_SIZE];

/* Preserve queued USB work across stop/start to avoid losing callbacks
 * while the USB process-queued flag remains set. The HCI worker executes
 * these callbacks.
 */
alignas(HciUsbWork_t) static uint8_t s_UsbWorkMem[
    CFIFO_TOTAL_MEMSIZE(HCI_USB_WORK_COUNT, sizeof(HciUsbWork_t))];
static hCFifo_t s_hUsbWork;

static int HciHostUsbEvent(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    HciAppWake();
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
    HciAppWake();
    return true;
}

static void HciHostUsbWorkExec(void)
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
        HciAppWake();
    }
}

/*---------------------------------------------------------------------------
 * UART bridge: a neighbour part's UART (the nRF91 on a Thingy:91) brought
 * out on a CDC function of its own, on the boards whose board.h defines it.
 *-------------------------------------------------------------------------*/
#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE

static UART s_BridgeUart;
static UsbdCdc s_BridgeCdc;
static const IOPinCfg_t s_BridgeUartPins[] = HCI_BRIDGE_UART_PINS;
alignas(4) static uint8_t s_BridgeUartRxMem[HCI_APP_FIFO_MEM_SIZE];
alignas(4) static uint8_t s_BridgeUartTxMem[HCI_APP_FIFO_MEM_SIZE];
alignas(4) static uint8_t s_BridgeUsbRxMem[HCI_APP_FIFO_MEM_SIZE];
alignas(4) static uint8_t s_BridgeUsbTxMem[HCI_APP_FIFO_MEM_SIZE];

static struct
{
    bool UartReady;
    uint32_t BaudRate;
    uint8_t Pending[64];
    uint16_t PendingLen;
    uint16_t PendingOffset;
    uint8_t InputPending[64];
    uint16_t InputLen;
    uint16_t InputOffset;
} s_Bridge;

/* Receive the neighbour's diagnostic UART output on the board-selected
 * bridge UART. Never pass these bytes through HCI H:4.
 */
static int HciBridgeUartEvent(UARTDev_t * const, UART_EVT evt, uint8_t *, int len)
{
    if (evt == UART_EVT_RXDATA || evt == UART_EVT_RXTIMEOUT)
    {
        HciAppWake();
    }
    return len;
}

static bool HciBridgeCdcInit(void)
{
    /* A distinct CDC function: HciController's own trace stays on s_LogCdc. */
    UsbdCdcCfg_t bridgeCfg = {};
    bridgeCfg.bBlocking = false;
    bridgeCfg.RxFifoMemSize = sizeof(s_BridgeUsbRxMem);
    bridgeCfg.pRxFifoMem = s_BridgeUsbRxMem;
    bridgeCfg.TxFifoMemSize = sizeof(s_BridgeUsbTxMem);
    bridgeCfg.pTxFifoMem = s_BridgeUsbTxMem;
    bridgeCfg.DevNo = 0;
    bridgeCfg.EvtCB = HciHostUsbEvent;
    return s_BridgeCdc.Init(bridgeCfg);
}

static bool HciBridgeUartStart(void)
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
    cfg.IntPrio = HCI_UART_IRQ_PRIORITY;
    cfg.EvtCallback = HciBridgeUartEvent;
    cfg.bFifoBlocking = false;
    cfg.bDMAMode = true;
    cfg.Duplex = UART_DUPLEX_FULL;
    cfg.Mode = UART_MODE_UART;
    cfg.RxMemSize = sizeof(s_BridgeUartRxMem);
    cfg.pRxMem = s_BridgeUartRxMem;
    cfg.TxMemSize = sizeof(s_BridgeUartTxMem);
    cfg.pTxMem = s_BridgeUartTxMem;
    s_Bridge.UartReady = s_BridgeUart.Init(cfg);
    s_Bridge.BaudRate = s_Bridge.UartReady ? cfg.Rate : 0U;
    return s_Bridge.UartReady;
}

static void HciBridgeRelease(void)
{
    if (s_Bridge.UartReady)
    {
        s_BridgeUart.Disable();
    }
    memset(&s_Bridge, 0, sizeof(s_Bridge));
}

/* UART RX -> USB CDC TX. Keep an unsent tail when CDC is busy. */
static void HciBridgePump(void)
{
    if (!s_Bridge.UartReady || !s_BridgeCdc.IsPortOpen())
    {
        return;
    }

    /* USB CDC line coding is accepted by IOsonata. Apply host-selected
     * baud rate to the separate bridge UART, never to the HCI UART.
     */
    const UsbCdcLineCoding_t *pLine = s_BridgeCdc.LineCoding();
    if (pLine != nullptr && pLine->dwDTERate > 0U &&
        pLine->dwDTERate != s_Bridge.BaudRate)
    {
        const uint32_t actual = s_BridgeUart.Rate(pLine->dwDTERate);
        if (actual != 0U)
        {
            s_Bridge.BaudRate = pLine->dwDTERate;
        }
    }

    if (s_Bridge.PendingOffset == s_Bridge.PendingLen)
    {
        s_Bridge.PendingOffset = 0U;
        const int len = s_BridgeUart.Rx(s_Bridge.Pending, sizeof(s_Bridge.Pending));
        s_Bridge.PendingLen = len > 0 ? (uint16_t)len : 0U;
    }

    if (s_Bridge.PendingOffset < s_Bridge.PendingLen)
    {
        const int sent = s_BridgeCdc.Tx(0U, s_Bridge.Pending + s_Bridge.PendingOffset,
                                        s_Bridge.PendingLen - s_Bridge.PendingOffset);
        if (sent > 0)
        {
            s_Bridge.PendingOffset += (uint16_t)sent;
        }
    }

    /* CDC RX -> UART TX. Retain the unsent tail under UART backpressure. */
    if (s_Bridge.InputOffset == s_Bridge.InputLen)
    {
        s_Bridge.InputOffset = 0U;
        const int len = s_BridgeCdc.Rx(0U, s_Bridge.InputPending, sizeof(s_Bridge.InputPending));
        s_Bridge.InputLen = len > 0 ? (uint16_t)len : 0U;
    }

    if (s_Bridge.InputOffset < s_Bridge.InputLen)
    {
        const int sent = s_BridgeUart.Tx(s_Bridge.InputPending + s_Bridge.InputOffset,
                                         s_Bridge.InputLen - s_Bridge.InputOffset);
        if (sent > 0)
        {
            s_Bridge.InputOffset += (uint16_t)sent;
        }
    }
}

#else

static bool HciBridgeCdcInit(void) { return true; }
static bool HciBridgeUartStart(void) { return true; }
static void HciBridgeRelease(void) {}
static void HciBridgePump(void) {}

#endif

/*---------------------------------------------------------------------------
 * The device stack and its functions
 *-------------------------------------------------------------------------*/

/* The descriptor set follows the mode: an HCI function only on a USB host. */
static HciUsbDescriptorMode_t HciHostUsbDescriptorMode(const HciApp_t *pApp)
{
    if (pApp->HostType != HCI_APP_HOST_USB)
    {
        return HCI_USB_DESCRIPTOR_LOG_ONLY;
    }
    return pApp->UsbHciNative ? HCI_USB_DESCRIPTOR_NATIVE_HCI : HCI_USB_DESCRIPTOR_CDC_H4;
}

bool HciHostUsbSetup(HciApp_t *pApp)
{
    const HciUsbDescriptorMode_t mode = HciHostUsbDescriptorMode(pApp);

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
    usbCfg.Pid = HciUsbDescriptorPid(mode);
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
    if (mode == HCI_USB_DESCRIPTOR_NATIVE_HCI)
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
        hciCfg.EvtCB = HciHostUsbEvent;
        if (!s_HciUsb.Init(hciCfg))
        {
            UsbDisable(0);
            return false;
        }
    }
    else if (mode == HCI_USB_DESCRIPTOR_CDC_H4)
    {
        UsbdCdcCfg_t hostCfg = {};
        hostCfg.bBlocking = true;
        hostCfg.RxFifoMemSize = sizeof(pApp->HostRxFifoMem);
        hostCfg.pRxFifoMem = pApp->HostRxFifoMem;
        hostCfg.TxFifoMemSize = sizeof(pApp->HostTxFifoMem);
        hostCfg.pTxFifoMem = pApp->HostTxFifoMem;
        hostCfg.DevNo = 0;
        hostCfg.EvtCB = HciHostUsbEvent;
        if (!s_HostCdc.Init(hostCfg))
        {
            UsbDisable(0);
            return false;
        }
    }

    UsbdCdcCfg_t logCfg = {};
    logCfg.bBlocking = true;
    logCfg.RxFifoMemSize = sizeof(s_LogRxFifoMem);
    logCfg.pRxFifoMem = s_LogRxFifoMem;
    logCfg.TxFifoMemSize = sizeof(s_LogTxFifoMem);
    logCfg.pTxFifoMem = s_LogTxFifoMem;
    logCfg.DevNo = 0;
    logCfg.EvtCB = HciHostUsbEvent;
    if (!s_LogCdc.Init(logCfg))
    {
        UsbDisable(0);
        return false;
    }

    if (!HciBridgeCdcInit())
    {
        UsbDisable(0);
        return false;
    }

    pApp->UsbRunning = true;

    if (!HciBridgeUartStart())
    {
        HciTrace("init: bridge UART initialization failed\r\n");
        HciHostUsbRelease(pApp);
        return false;
    }

    if (pApp->HostType == HCI_APP_HOST_USB)
    {
        pApp->pHostIntrf = HciHostUsbIntrf(pApp);
    }
    return true;
}

void HciHostUsbRelease(HciApp_t *pApp)
{
    HciBridgeRelease();

    if (!pApp->UsbRunning)
    {
        return;
    }

    if (pApp->HostType == HCI_APP_HOST_USB && !pApp->UsbHciNative)
    {
        s_HostCdc.Disable();
    }
    if (pApp->pHostIntrf == HciHostUsbIntrf(pApp))
    {
        pApp->pHostIntrf = nullptr;
    }

    HciTraceSetSink(nullptr, 0U);
    UsbDisable(0);
    pApp->UsbRunning = false;
    pApp->LogPortOpen = false;
}

DevIntrf_t *HciHostUsbIntrf(const HciApp_t *pApp)
{
    return pApp->UsbHciNative ? s_HciUsb.Data() : s_HostCdc.Data();
}

bool HciHostUsbIsPacket(const HciApp_t *pApp)
{
    return pApp->UsbHciNative;
}

bool HciHostUsbIsOpen(const HciApp_t *pApp)
{
    if (pApp == nullptr || pApp->HostType != HCI_APP_HOST_USB || !pApp->UsbRunning)
    {
        return false;
    }

    return pApp->UsbHciNative ? s_HciUsb.Rate() != 0U
                              : s_HostCdc.IsPortOpen();
}

void HciHostUsbStartLogPort(HciApp_t *)
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

bool HciHostUsbStart(HciApp_t *pApp)
{
    if (!UsbEnable(0))
    {
        HciTrace("host: UsbEnable failed\r\n");
        return false;
    }

    for (uint32_t pass = 0U; pass < HCI_USB_SETTLE_PASSES; pass++)
    {
        HciTargetProcessMpsl();

        HciHostUsbWorkExec();

        if (UsbConfigured(0))
        {
            break;
        }

        if (pApp->Runtime.StopRequested)
        {
            HciTrace("host: settle abandoned, stop requested\r\n");
            break;
        }

        if ((pass % HCI_USB_SETTLE_REPORT) == (HCI_USB_SETTLE_REPORT - 1U))
        {
            HciTrace("host: settling pass=%lu\r\n", (unsigned long)pass + 1UL);
        }
    }

    HciTrace("host: usb up configured=%u open=%u\r\n",
             (unsigned)UsbConfigured(0), (unsigned)HciHostUsbIsOpen(pApp));
    return true;
}

static void HciHostUsbLogPortOpened(HciApp_t *pApp)
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
        HciTrace("log: port open\r\n");
    }
}

/* Consume diagnostic CDC OUT bytes without interpreting them. This prevents
 * the standard blocking RX FIFO from remaining full under host traffic.
 */
static void HciHostUsbDiscardLogInput(void)
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

void HciHostUsbProcess(HciApp_t *pApp)
{
    if (!pApp->UsbRunning)
    {
        return;
    }

    HciHostUsbWorkExec();
    HciHostUsbLogPortOpened(pApp);
    HciHostUsbDiscardLogInput();
    if (pApp->LogPortOpen)
    {
        (void)HciTraceFlush();
    }
    HciBridgePump();
}
