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
 * The device stack has one event queue. It holds bus events, transfer
 * completions and the deferred EasyDMA starts the nRF5x port queues whenever a
 * transfer is asked for while another is already running. The nRF52840 USBD has
 * a single EasyDMA engine shared by every endpoint, so two CDC functions moving
 * data at once reach that deferral often.
 *
 * On overflow the stack drops the event through a TU_ASSERT that is a bare
 * early return in a release build. A dropped transfer completion leaves its
 * endpoint marked busy for good, because the only place that mark is cleared is
 * the task handling that event. A dropped deferral leaves a transfer that never
 * starts. Nothing is printed and no counter moves.
 *
 * The depth was the default 16. The drain was, and by default still would be,
 * 16 events per turn of the task, so a deeper queue on its own only lengthens
 * the fuse: the depth has to be matched by a drain that empties it. Zero means
 * empty it.
 *
 * 64 entries costs 64 times sizeof(dcd_event_t), under a kilooctet.
 */
#define CFG_TUD_TASK_QUEUE_SZ      64
#define CFG_TUD_TASK_EVENTS_PER_RUN 0

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
