/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_CORE_PROFILE_H
#define HCI_CORE_PROFILE_H

/* Assigned HCI/LMP version values. */
#define HCI_CORE_VERSION_5_4 0x0DU
#define HCI_CORE_VERSION_6_0 0x0EU

/*
 * Core revision exposed by this HCI controller product.
 *
 * Current nRF52 builds are audited against Core 5.4 even when the linked
 * nrfxlib SoftDevice Controller is newer. A future nRF54LM20 build can select
 * Core 6.0 (or a later value once that profile is implemented and audited)
 * with a compiler definition rather than forking the generic bridge.
 */
#ifndef HCI_CONTROLLER_TARGET_CORE_VERSION
#define HCI_CONTROLLER_TARGET_CORE_VERSION HCI_CORE_VERSION_5_4
#endif

#if HCI_CONTROLLER_TARGET_CORE_VERSION > 0xFFU
#error "HCI_CONTROLLER_TARGET_CORE_VERSION must fit in one octet"
#endif

#endif /* HCI_CORE_PROFILE_H */
