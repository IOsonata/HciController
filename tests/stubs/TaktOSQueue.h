#ifndef STUB_TAKTOSQUEUE_H
#define STUB_TAKTOSQUEUE_H

#include <stdint.h>
#include <string.h>

static inline void TaktQueueFastCopy(void *pDst,
                                     const void *pSrc,
                                     uint32_t Size)
{
    memcpy(pDst, pSrc, Size);
}

#endif /* STUB_TAKTOSQUEUE_H */
