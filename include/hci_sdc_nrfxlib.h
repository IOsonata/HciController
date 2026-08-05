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

#include "hci_sdc.h"

#ifdef __cplusplus
extern "C" {
#endif

bool HciSdcNrfxlibInit(HciSdc_t *pSdc,
                       uint8_t *pCommandEvent,
                       size_t CommandEventCapacity);

#ifdef __cplusplus
}
#endif

#endif /* HCI_SDC_NRFXLIB_H */
