/*
 * Enough of the IOsonata USB CDC interface for a host build.
 *
 * The device interface itself comes from device_intrf.h, which is the same
 * header the UART stub uses. It used to be repeated here, which worked only
 * because no test ever included both.
 */

#ifndef STUB_USBD_CDC_INTRF_H
#define STUB_USBD_CDC_INTRF_H

#include <stdint.h>

#include "cfifo.h"
#include "device_intrf.h"

#define USBD_CDC_INTRF_TRANSBUFF_MAXLEN 64

typedef struct {
    DevIntrf_t DevIntrf;
    hCFifo_t hRxFifo;
    hCFifo_t hTxFifo;
    uint8_t TransBuff[USBD_CDC_INTRF_TRANSBUFF_MAXLEN];
    int TransBuffLen;
} UsbdCdcDevIntrf_t;

/* Member names copied from IOsonata usb/usbd_cdc_intrf.h. */
typedef struct {
    bool bBlocking;
    int RxFifoMemSize;
    uint8_t *pRxFifoMem;
    int TxFifoMemSize;
    uint8_t *pTxFifoMem;
    DevIntrfEvtHandler_t EvtCB;
} UsbdCdcIntrfCfg_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Records what it was given and answers gStubUsbdCdcInitResult, so a test can
 * read back what the application asked for.
 */
extern UsbdCdcIntrfCfg_t gStubUsbdCdcCfg;
extern int gStubUsbdCdcInitCount;
extern bool gStubUsbdCdcInitResult;

bool UsbdCdcIntrfInit(UsbdCdcDevIntrf_t *pDev, const UsbdCdcIntrfCfg_t *pCfg);

#ifdef __cplusplus
}
#endif

#endif
