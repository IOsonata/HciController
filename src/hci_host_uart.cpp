/**-------------------------------------------------------------------------
@file	hci_host_uart.cpp

@brief	UART H:4 host transport of HciController.

		The UART of board.h (UART_DEVNO, UART_PINS, UART_RATE and
		UART_FLOWCTRL) carries H:4 to a host MCU or a bench PC. A project
		that links this file has the UART host mode; one that does not falls
		back on the weak defaults of hci_app.cpp, which report it absent.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "board.h"
#include "hci_app.h"

#include "coredev/iopincfg.h"
#include "coredev/uart.h"
#include "hci_trace.h"

/*
 * A board.h with no UART_PINS, as the UDG dongle's, has no UART host: the
 * entries below stay defined and the init reports the transport absent, so
 * one project serves the boards with the UART and the one without.
 */
#ifndef UART_DEVNO
#define UART_DEVNO              0
#endif
#ifndef UART_FLOWCTRL
#define UART_FLOWCTRL           UART_FLWCTRL_NONE
#endif
#ifndef HCI_UART_IRQ_PRIORITY
#define HCI_UART_IRQ_PRIORITY   6
#endif

#ifdef UART_PINS

static UART s_HciUart;
static const IOPinCfg_t s_HciUartPins[] = UART_PINS;

static int HciHostUartEvent(UARTDev_t * const, UART_EVT EvtId, uint8_t *, int BufferLen)
{
    if (EvtId == UART_EVT_RXDATA || EvtId == UART_EVT_RXTIMEOUT ||
        EvtId == UART_EVT_TXREADY)
    {
        HciAppWake();
    }
    return BufferLen;
}

bool HciHostUartInit(HciApp_t *pApp)
{
    UARTCfg_t cfg = {};
    cfg.DevNo = UART_DEVNO;
    cfg.pIOPinMap = s_HciUartPins;
    cfg.NbIOPins = sizeof(s_HciUartPins) / sizeof(s_HciUartPins[0]);
    cfg.Rate = UART_RATE;
    cfg.DataBits = 8;
    cfg.Parity = UART_PARITY_NONE;
    cfg.StopBits = 1;
    cfg.FlowControl = UART_FLOWCTRL;
    cfg.bIntMode = true;
    cfg.IntPrio = HCI_UART_IRQ_PRIORITY;
    cfg.EvtCallback = HciHostUartEvent;
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

    HciTargetUartTrace(UART_DEVNO);
    pApp->pHostIntrf = static_cast<DevIntrf_t *>(s_HciUart);
    return true;
}

bool HciHostUartLogOpen(HciApp_t *pApp)
{
    static_assert(sizeof(s_HciUartPins) / sizeof(s_HciUartPins[0]) >= 2U,
                  "UART_PINS must list the receive and transmit pins first");

    UARTCfg_t cfg = {};
    cfg.DevNo = UART_DEVNO;
    cfg.pIOPinMap = s_HciUartPins;
    cfg.NbIOPins = 2;
    cfg.Rate = UART_RATE;
    cfg.DataBits = 8;
    cfg.Parity = UART_PARITY_NONE;
    cfg.StopBits = 1;
    cfg.FlowControl = UART_FLWCTRL_NONE;
    cfg.bIntMode = true;
    cfg.IntPrio = HCI_UART_IRQ_PRIORITY;
    /* The trace must never hold up the controller: when the FIFO is full the
     * excess is dropped, and the rest goes out on the next flush. */
    cfg.bFifoBlocking = false;
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

    /* Records kept while no output was set go out first. */
    HciTraceSetSink(static_cast<DevIntrf_t *>(s_HciUart), 0U);
    HciTraceBootDrain(true);
    (void)HciTraceFlush();
    return true;
}

void HciHostUartRelease(HciApp_t *pApp)
{
    if (pApp->pHostIntrf == static_cast<DevIntrf_t *>(s_HciUart))
    {
        s_HciUart.Disable();
        pApp->pHostIntrf = nullptr;
    }
}

#else

bool HciHostUartLogOpen(HciApp_t *)
{
    return false;
}

bool HciHostUartInit(HciApp_t *)
{
    return false;
}

void HciHostUartRelease(HciApp_t *)
{
}

#endif
