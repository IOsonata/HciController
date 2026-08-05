#ifndef STUB_DEVICE_INTRF_H
#define STUB_DEVICE_INTRF_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef enum { DEVINTRF_EVT_RX_DATA = 0 } DEVINTRF_EVT;
struct __device_intrf;
typedef int (*DevIntrfEvtHandler_t)(struct __device_intrf *, DEVINTRF_EVT, uint8_t *, int);
typedef struct __device_intrf { DevIntrfEvtHandler_t EvtCB; } DevIntrf_t;
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
