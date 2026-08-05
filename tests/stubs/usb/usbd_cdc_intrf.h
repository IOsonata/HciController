#ifndef STUB_USBD_CDC_INTRF_H
#define STUB_USBD_CDC_INTRF_H
#include <stdint.h>
#include "cfifo.h"

typedef enum { DEVINTRF_EVT_RX_DATA = 0, DEVINTRF_EVT_TX_READY = 1 } DEVINTRF_EVT;

struct __device_intrf;
typedef int (*DevIntrfEvtHandler_t)(struct __device_intrf *pDev, DEVINTRF_EVT Evt,
                                    uint8_t *pBuffer, int BufferLen);

typedef struct __device_intrf {
    DevIntrfEvtHandler_t EvtCB;
} DevIntrf_t;

#define USBD_CDC_INTRF_TRANSBUFF_MAXLEN 64

typedef struct {
    DevIntrf_t DevIntrf;
    hCFifo_t hRxFifo;
    hCFifo_t hTxFifo;
    uint8_t TransBuff[USBD_CDC_INTRF_TRANSBUFF_MAXLEN];
    int TransBuffLen;
} UsbdCdcDevIntrf_t;

#endif
