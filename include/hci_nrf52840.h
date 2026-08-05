/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_NRF52840_H
#define HCI_NRF52840_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_NRF52840_DEFAULT_SDC_MEM_SIZE 10000U

typedef struct {
    HciTaktOs_t *pRuntime;
    uint8_t *pSdcMem;
    size_t SdcMemCapacity;

    int32_t RequiredSdcMem;
    int32_t LastError;
    uint32_t FaultCount;
    bool UsbEnabled;
    bool MpslInitialized;
    bool SdcInitialized;
    bool SdcEnabled;
    bool HfclkRequested;
    volatile bool UsbStarted;
    volatile bool UsbReadyDone;
    uint32_t RandFailCount;
    volatile uint32_t UsbIrqCount;
    volatile uint32_t UsbIrqMark;
    volatile uint32_t UsbStuckCauseCount;
    volatile uint32_t UsbEventCause;
    volatile uint32_t UsbStormInten;
    volatile uint32_t UsbStormCause;
    volatile uint32_t UsbStormEvents;
} HciNrf52840_t;

bool HciNrf52840Init(HciNrf52840_t *pTarget,
                     HciTaktOs_t *pRuntime,
                     uint8_t *pSdcMem,
                     size_t SdcMemCapacity,
                     bool UsbEnabled);

void HciNrf52840GetTaktOsOps(HciNrf52840_t *pTarget,
                             HciTaktOsOps_t *pOps);

/*
 * Enables the USB hardware. Must be called after the USB device stack has been
 * initialised, and only when the target was created with UsbEnabled set.
 */
bool HciNrf52840UsbStart(HciNrf52840_t *pTarget);

/*
 * Marks the start of a device stack pump pass. Interrupts counted between two
 * marks are what the storm detector measures.
 */
void HciNrf52840UsbPassMark(HciNrf52840_t *pTarget);

void HciNrf52840Stop(HciNrf52840_t *pTarget);

#ifdef __cplusplus
}
#endif

#endif /* HCI_NRF52840_H */
