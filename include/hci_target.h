/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_TARGET_H
#define HCI_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the application needs from a part, and nothing else.
 *
 * Everything here is hardware that differs between parts: the low frequency
 * clock and how MPSL is told about it, the USB device peripheral if there is
 * one, the errata, and the interrupt wiring. What the SoftDevice Controller is
 * configured to do is not in this interface, because it does not vary. That
 * lives in hci_sdc_resources.h and every port calls the same code.
 *
 * The application held an HciNrf52840_t directly before this existed, which
 * meant the whole stack above the radio named one part in nine places for no
 * reason. Adding a second part meant editing all of them.
 *
 * A port publishes one HciTarget_t. It owns its own instance, because a board
 * has one radio and there is nothing to allocate.
 */
typedef struct {
    bool (*Init)(void *pContext,
                 HciTaktOs_t *pRuntime,
                 uint8_t *pSdcMem,
                 size_t SdcMemCapacity,
                 bool UsbEnabled);

    void (*GetTaktOsOps)(void *pContext, HciTaktOsOps_t *pOps);

    /*
     * The USB entries are optional. A part with no USB device peripheral, or a
     * board that only ever talks over its UART, leaves them null and the
     * application skips them rather than testing for a part.
     */
    bool (*UsbStart)(void *pContext);
    void (*UsbPassMark)(void *pContext);
    void (*UsbPowerProcess)(void *pContext);

    void (*Stop)(void *pContext);

    /* Whatever the port last failed with, for the application to report. */
    int32_t (*LastError)(const void *pContext);
} HciTargetOps_t;

typedef struct {
    const HciTargetOps_t *pOps;
    void *pContext;
} HciTarget_t;

static inline bool HciTargetValid(const HciTarget_t *pTarget)
{
    return pTarget != NULL && pTarget->pOps != NULL &&
           pTarget->pOps->Init != NULL &&
           pTarget->pOps->GetTaktOsOps != NULL &&
           pTarget->pOps->Stop != NULL;
}

static inline bool HciTargetHasUsb(const HciTarget_t *pTarget)
{
    return pTarget != NULL && pTarget->pOps != NULL &&
           pTarget->pOps->UsbStart != NULL &&
           pTarget->pOps->UsbPassMark != NULL &&
           pTarget->pOps->UsbPowerProcess != NULL;
}

static inline int32_t HciTargetLastError(const HciTarget_t *pTarget)
{
    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->LastError == NULL)
    {
        return 0;
    }

    return pTarget->pOps->LastError(pTarget->pContext);
}

#ifdef __cplusplus
}
#endif

#endif /* HCI_TARGET_H */
