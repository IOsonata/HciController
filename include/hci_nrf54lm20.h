/** nRF54LM20A/B HciController target. MPL-2.0 (c) 2026 I-SYST inc. */
#ifndef HCI_NRF54LM20_H
#define HCI_NRF54LM20_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hci_target.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    HciTaktOs_t *pRuntime;
    uint8_t *pSdcMem;
    size_t SdcMemCapacity;
    int32_t RequiredSdcMem;
    int32_t LastError;
    uint32_t FaultCount;
    uint32_t RandRetryCount;
    bool MpslInitialized;
    bool SdcInitialized;
    bool SdcEnabled;
} HciNrf54lm20_t;
HciTarget_t HciNrf54lm20Target(void);
#ifdef __cplusplus
}
#endif
#endif
