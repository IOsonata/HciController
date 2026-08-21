/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_VERSION_H
#define HCI_VERSION_H

/* IOsonata Vers_t format: 0xMMmm, where MM is major and mm is minor. */
#define FIRMWARE_VERSION 0x0100U

/* USB bcdDevice is 1.00 for HciController 1.0.0. */
#define HCI_CONTROLLER_VERSION_BCD FIRMWARE_VERSION

#endif
