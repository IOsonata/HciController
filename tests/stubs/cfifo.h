/*
 * Enough of the IOsonata circular FIFO for a host build.
 *
 * CFIFO_MEMSIZE is what the application sizes its FIFO arrays with, so the
 * header size has to be here for hci_app.h to compile. The value only has to
 * be large enough to be honest about the overhead; nothing off target reads
 * the header.
 */

#ifndef STUB_CFIFO_H
#define STUB_CFIFO_H

#include <stdint.h>

typedef struct __CFifo {
    uint32_t Header[8];
} CFifo_t;

typedef struct __CFifo *hCFifo_t;

#define CFIFO_MEMSIZE(FSIZE) ((FSIZE) + sizeof(CFifo_t))

#ifdef __cplusplus
extern "C" {
#endif

int CFifoAvail(hCFifo_t hFifo);
int CFifoUsed(hCFifo_t hFifo);
int CFifoRead(hCFifo_t hFifo, uint8_t *pBuff, int Len);
int CFifoWrite(hCFifo_t hFifo, uint8_t *pData, int Len);

#ifdef __cplusplus
}
#endif

#endif
