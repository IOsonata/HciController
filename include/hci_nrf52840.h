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
#include "hci_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The nRF52840 port: its clock, its USB device peripheral, its errata and the
 * state those need. What the SoftDevice Controller is configured for is not
 * here, it is in hci_sdc_resources.h, because it is the same on every part.
 */
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
    /* Cable events, set by POWER_CLOCK and applied by the runtime thread. */
    volatile bool UsbAttachPending;
    volatile bool UsbDetachPending;
    volatile uint32_t UsbAttachCount;
    volatile uint32_t UsbDetachCount;

    uint32_t RandRetryCount;

    /* Last MPSL or controller assert, kept for a debugger to read. */
    const char *AssertFile;
    uint32_t AssertLine;
    uint32_t AssertCount;
    bool AssertFromSdc;
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

/*
 * Apply a cable attach or detach recorded by the interrupt handler. Must be
 * called from the same context that pumps the device stack.
 */
void HciNrf52840UsbPowerProcess(HciNrf52840_t *pTarget);

void HciNrf52840Stop(HciNrf52840_t *pTarget);

/*
 * This part as a target the application can hold without naming it. The
 * instance is owned here because there is one radio, so there is nothing to
 * allocate and nothing for the caller to size.
 */
HciTarget_t HciNrf52840Target(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_NRF52840_H */
