/*
 * Enough of the IOsonata device interface for a host build.
 *
 * The event names and their order are copied from IOsonata device_intrf.h.
 * The order matters, because the enumerators are compared by value in the
 * transport, and a stub that renumbered them would let a wrong comparison
 * pass here.
 */

#ifndef STUB_DEVICE_INTRF_H
#define STUB_DEVICE_INTRF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DEVINTRF_EVT_RX_TIMEOUT,
    DEVINTRF_EVT_RX_DATA,
    DEVINTRF_EVT_RX_FIFO_FULL,
    DEVINTRF_EVT_TX_TIMEOUT,
    DEVINTRF_EVT_TX_READY,
    DEVINTRF_EVT_TX_FIFO_EMPTY,
    DEVINTRF_EVT_STATECHG,
    DEVINTRF_EVT_READ_RQST,
    DEVINTRF_EVT_WRITE_RQST,
    DEVINTRF_EVT_COMPLETED,
} DEVINTRF_EVT;

struct __device_intrf;

typedef int (*DevIntrfEvtHandler_t)(struct __device_intrf *pDev,
                                    DEVINTRF_EVT Evt,
                                    uint8_t *pBuffer,
                                    int BufferLen);

typedef struct __device_intrf {
    DevIntrfEvtHandler_t EvtCB;
} DevIntrf_t;

#ifdef __cplusplus
extern "C" {
#endif

int DeviceIntrfRx(DevIntrf_t *, uint32_t, uint8_t *, int);
int DeviceIntrfTx(DevIntrf_t *, uint32_t, const uint8_t *, int);
void DeviceIntrfEnable(DevIntrf_t *);
void DeviceIntrfDisable(DevIntrf_t *);

#ifdef __cplusplus
}
#endif

#endif
