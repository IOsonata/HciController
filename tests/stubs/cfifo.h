#ifndef STUB_CFIFO_H
#define STUB_CFIFO_H
#include <stdint.h>
typedef struct __CFifo { int Dummy; } *hCFifo_t;
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
