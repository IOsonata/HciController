#ifndef STUB_TAKTOS_CRITICAL_H
#define STUB_TAKTOS_CRITICAL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t TaktOSEnterCritical(void);
void TaktOSExitCritical(uint32_t State);
#ifdef __cplusplus
}
#endif
#endif
