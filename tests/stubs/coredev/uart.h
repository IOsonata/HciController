/*
 * Enough of the IOsonata UART for a host build.
 *
 * The member names, the enumeration names and the UARTInit signature are
 * copied from IOsonata include/coredev/uart.h. The bodies are not: UARTInit
 * here records the configuration and answers true, so a test can see what the
 * application asked the driver for without a UARTE peripheral.
 *
 * This is a copy and copies drift. What it buys is that src/hci_app.cpp gets
 * compiled at all on the host, which is where the field names, the callback
 * signature and the trace arguments are checked. A driver change that renames
 * a member fails here rather than only on the target.
 */

#ifndef UART_H__
#define UART_H__

#include <stdbool.h>
#include <stdint.h>

#include "cfifo.h"
#include "device_intrf.h"

typedef enum {
    UART_PARITY_ODD = 0,
    UART_PARITY_EVEN = 1,
    UART_PARITY_MARK = 2,
    UART_PARITY_SPACE = 3,
    UART_PARITY_NONE = 4,
} UART_PARITY;

typedef enum {
    UART_FLWCTRL_NONE,
    UART_FLWCTRL_XONXOFF,
    UART_FLWCTRL_HW,
} UART_FLWCTRL;

typedef enum {
    UART_MODE_UART,
    UART_MODE_USART,
    UART_MODE_NET,
} UART_MODE;

typedef enum {
    UART_DUPLEX_FULL,
    UART_DUPLEX_HALF,
} UART_DUPLEX;

typedef enum {
    UART_EVT_RXTIMEOUT,
    UART_EVT_RXDATA,
    UART_EVT_TXREADY,
    UART_EVT_LINESTATE,
} UART_EVT;

#define UART_NB_PINS 8

#define UARTPIN_RX_IDX  0
#define UARTPIN_TX_IDX  1
#define UARTPIN_CTS_IDX 2
#define UARTPIN_RTS_IDX 3

typedef struct __Uart_Dev UARTDev_t;

typedef int (*UARTEvtHandler_t)(UARTDev_t *const pDev,
                                UART_EVT EvtId,
                                uint8_t *pBuffer,
                                int BufferLen);

typedef struct {
    int DevNo;
    const void *pIOPinMap;
    int NbIOPins;
    int Rate;
    int DataBits;
    UART_PARITY Parity;
    int StopBits;
    UART_FLWCTRL FlowControl;
    bool bIntMode;
    int IntPrio;
    UARTEvtHandler_t EvtCallback;
    bool bFifoBlocking;
    int RxMemSize;
    uint8_t *pRxMem;
    int TxMemSize;
    uint8_t *pTxMem;
    bool bDMAMode;
    bool bIrDAMode;
    bool bIrDAInvert;
    bool bIrDAFixPulse;
    int IrDAPulseDiv;
    UART_DUPLEX Duplex;
    UART_MODE Mode;
} UARTCfg_t;

struct __Uart_Dev {
    UART_MODE Mode;
    UART_DUPLEX Duplex;
    int Rate;
    int DataBits;
    UART_PARITY Parity;
    int StopBits;
    UART_FLWCTRL FlowControl;
    bool bIrDAMode;
    bool bIrDAInvert;
    bool bIrDAFixPulse;
    int IrDAPulseDiv;
    DevIntrf_t DevIntrf;
    UARTEvtHandler_t EvtCallback;
    void *pObj;
    hCFifo_t hRxFifo;
    hCFifo_t hTxFifo;
    uint32_t LineState;
    int hStdIn;
    int hStdOut;
    uint32_t RxOvrErrCnt;
    uint32_t ParErrCnt;
    uint32_t FramErrCnt;
    uint32_t RxDropCnt;
    uint32_t TxDropCnt;
    volatile bool bRxReady;
    volatile bool bTxReady;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The last configuration UARTInit was given, and how many times it was
 * called, so a test can read back what the application asked for. Defined by
 * whichever test links this.
 */
extern UARTCfg_t gStubUartCfg;
extern int gStubUartInitCount;
extern bool gStubUartInitResult;

bool UARTInit(UARTDev_t *const pDev, const UARTCfg_t *pCfgData);
void UARTSetCtrlLineState(UARTDev_t *const pDev, uint32_t LineState);

#ifdef __cplusplus
}
#endif

#endif /* UART_H__ */
