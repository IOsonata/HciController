/**-------------------------------------------------------------------------
@file	hci_nrf5340_net.h

@brief	nRF5340 network core MPSL/SDC target for HciController.

		The network core has the radio and no USB. Its host is the application
		core, reached through an IpcIntrf.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_NRF5340_NET_H
#define HCI_NRF5340_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"
#include "hci_target.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
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
} HciNrf5340Net_t;

/*
 * This part as a target the application can hold without naming it. There is
 * one radio, so the instance is owned here.
 */
HciTarget_t HciNrf5340NetTarget(void);

/*
 * Host transport mode kept across a reset. The network core has no NVM
 * interface port, so the mode is held in the two retained general purpose
 * registers: it survives a reset of the network core, not a power loss.
 * These registers are not shared with any bootloader on this core.
 */
bool HciNrf5340NetModeLoad(uint8_t *pMode);
bool HciNrf5340NetModeStore(uint8_t Mode);

#ifdef __cplusplus
}
#endif

#endif /* HCI_NRF5340_NET_H */
