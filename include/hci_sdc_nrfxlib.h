/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_SDC_NRFXLIB_H
#define HCI_SDC_NRFXLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_counters.h"
#include "hci_sdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * pCounters becomes the command context, which only the counter readout uses.
 * NULL is allowed and makes that one command answer Command Disallowed; every
 * other row in the table is unaffected.
 */
bool HciSdcNrfxlibInit(HciSdc_t *pSdc,
                       uint8_t *pCommandEvent,
                       size_t CommandEventCapacity,
                       HciCounters_t *pCounters);

/*
 * Queue the No Operation Command Complete that says the controller is ready,
 * Vol 4 Part E 7.7.14 with opcode 0x0000. Whether a board needs it is a board
 * fact, so the caller decides: hci_sdc_nrfxlib.cpp cannot see board.h, and a
 * macro tested there gave the one board that sets it an image with the call
 * compiled out.
 *
 * Call after HciSdcNrfxlibInit and before the runtime thread starts.
 */
void HciSdcNrfxlibQueueStartupNop(HciSdc_t *pSdc);

/*
 * Give up the advertising command set the host has chosen.
 *
 * Vol 4 Part E 3.1.1 lets a host use the legacy advertising, scanning and
 * initiating commands or the extended ones, not both, and ties the choice to
 * the last reset. The dispatch layer refuses the second set, so this is what
 * HCI Reset calls, and what a test that walks the whole table needs between
 * rows.
 */
void HciSdcNrfxlibResetAdvCommandType(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_SDC_NRFXLIB_H */
