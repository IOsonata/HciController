/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error "Define CFG_TUSB_MCU=OPT_MCU_NRF5X in the firmware project"
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

/*
 * Two. One is the HCI byte stream and one is a log, and the second exists
 * because a controller that can only be watched with a debugger cannot be
 * watched at all on a sealed board or on somebody else's product. The nRF52840
 * has seven bulk or interrupt endpoints each way and two CDC take four in and
 * two out, so this is not close to a limit.
 *
 * The log one is there whichever port the HCI stream is on. With HCI over
 * UART the first is simply unused and the log still arrives.
 */
#define CFG_TUD_CDC    2
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_NOTIFY     1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_RX_EPSIZE  64
#define CFG_TUD_CDC_TX_EPSIZE  64

#endif /* TUSB_CONFIG_H */
