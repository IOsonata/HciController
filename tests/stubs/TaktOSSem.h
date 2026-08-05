#ifndef STUB_TAKTOSSEM_H
#define STUB_TAKTOSSEM_H
#include <stdint.h>
#include <stdbool.h>
typedef enum { TAKTOS_OK = 0, TAKTOS_ERR_FULL = 1 } TaktOSErr_t;
typedef struct { uint32_t Count; uint32_t Max; } TaktOSSem_t;
#define TAKTOS_WAIT_FOREVER 0xFFFFFFFFU
#define TAKTOS_NO_WAIT      0U
#ifdef __cplusplus
extern "C" {
#endif
TaktOSErr_t TaktOSSemInit(TaktOSSem_t *, uint32_t, uint32_t);
TaktOSErr_t TaktOSSemGive(TaktOSSem_t *, bool);
TaktOSErr_t TaktOSSemTake(TaktOSSem_t *, bool, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
