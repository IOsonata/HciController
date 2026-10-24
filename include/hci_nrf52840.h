/**-------------------------------------------------------------------------
@file	hci_nrf52840.h

@brief	nRF52840 HciController target interface and runtime state.

		Declares the nRF52840 target state and the retained reset
		diagnostics. The target functions are those of hci_target.h.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_NRF52840_H
#define HCI_NRF52840_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"
#include "hci_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The nRF52840 port: its clocks, radio interrupts, errata and retained state.
 * What the SoftDevice Controller is configured for is not
 * here, it is in hci_sdc_resources.h, because it is the same on every part.
 */
typedef struct {
    HciTaktOs_t *pRuntime;
    uint8_t *pSdcMem;
    size_t SdcMemCapacity;

    int32_t RequiredSdcMem;
    int32_t LastError;
    uint32_t FaultCount;
    bool MpslInitialized;
    bool SdcInitialized;
    bool SdcEnabled;
    bool HfclkRequested;

    uint32_t RandRetryCount;
} HciNrf52840_t;

/*
 * Print the reset reason and, when the previous reset followed an MPSL/SDC
 * assertion, the retained assertion source and line. Call once after the trace
 * sink is initialized and before normal target initialization clears runtime
 * state.
 */
void HciNrf52840ResetTrace(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_NRF52840_H */
