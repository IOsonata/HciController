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

#include "sdc.h"

#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Radio resources, and the memory pool they need.
 *
 * These live in the header rather than beside the code that applies them
 * because the pool is declared by the application, from
 * HCI_NRF52840_DEFAULT_SDC_MEM_SIZE, and the two have to agree. They used to
 * be a set of defines in the source and a hand picked 10000 here, kept in step
 * by a static assert. That works until somebody raises the link count and does
 * not think about the pool, and then the controller refuses to start, which is
 * the one failure the application cannot work around.
 *
 * So the pool is computed from the configuration instead. Raising a count now
 * grows the array that holds it, and the assert has nothing left to catch.
 *
 * Override any of them from the build. The costs on an nRF52840 with a 251
 * octet payload and four packets each way, from the current sdk-nrfxlib:
 *
 *   peripheral link  2935 octets      central link  2839 octets
 *   advertising set   961 octets      scan buffers  1688 for four
 *   accept list        68 for eight
 */
#ifndef HCI_NRF52840_PERIPHERAL_COUNT
#define HCI_NRF52840_PERIPHERAL_COUNT 4U
#endif

#ifndef HCI_NRF52840_CENTRAL_COUNT
#define HCI_NRF52840_CENTRAL_COUNT 4U
#endif

/*
 * The payload the controller advertises in LE Read Buffer Size, and how many
 * buffers it keeps. 251 is the data length extension maximum, and a host that
 * is told 251 can use it: the alternative is a controller that quietly caps
 * throughput at a ninth of what the radio does.
 */
#ifndef HCI_NRF52840_ACL_PACKET_SIZE
#define HCI_NRF52840_ACL_PACKET_SIZE 251U
#endif

#ifndef HCI_NRF52840_ACL_PACKET_COUNT
#define HCI_NRF52840_ACL_PACKET_COUNT 4U
#endif

#ifndef HCI_NRF52840_ADV_SET_COUNT
#define HCI_NRF52840_ADV_SET_COUNT 2U
#endif

#ifndef HCI_NRF52840_SCAN_BUFFER_COUNT
#define HCI_NRF52840_SCAN_BUFFER_COUNT 4U
#endif

#ifndef HCI_NRF52840_MAX_ADV_DATA
#define HCI_NRF52840_MAX_ADV_DATA 255U
#endif

/* Filter accept list entries. The SoftDevice Controller default is eight. */
#ifndef HCI_NRF52840_FAL_SIZE
#define HCI_NRF52840_FAL_SIZE 8U
#endif

#define HCI_NRF52840_SDC_MEM_REQUIRED                                         \
    (SDC_MEM_PER_PERIPHERAL_LINK(HCI_NRF52840_ACL_PACKET_SIZE,                \
                                 HCI_NRF52840_ACL_PACKET_SIZE,                \
                                 HCI_NRF52840_ACL_PACKET_COUNT,               \
                                 HCI_NRF52840_ACL_PACKET_COUNT) *             \
         HCI_NRF52840_PERIPHERAL_COUNT +                                      \
     SDC_MEM_PER_CENTRAL_LINK(HCI_NRF52840_ACL_PACKET_SIZE,                   \
                              HCI_NRF52840_ACL_PACKET_SIZE,                   \
                              HCI_NRF52840_ACL_PACKET_COUNT,                  \
                              HCI_NRF52840_ACL_PACKET_COUNT) *                \
         HCI_NRF52840_CENTRAL_COUNT +                                         \
     SDC_MEM_PERIPHERAL_LINKS_SHARED + SDC_MEM_CENTRAL_LINKS_SHARED +         \
     SDC_MEM_SCAN_EXT(HCI_NRF52840_SCAN_BUFFER_COUNT) +                       \
     SDC_MEM_PER_ADV_SET(HCI_NRF52840_MAX_ADV_DATA) *                         \
         HCI_NRF52840_ADV_SET_COUNT +                                         \
     SDC_MEM_FAL(HCI_NRF52840_FAL_SIZE))

/*
 * sdc.h says the memory requirement defines "may change between minor
 * releases", and the number that decides whether the controller starts is the
 * one sdc_cfg_set answers at run time, not this one. The margin is there so a
 * small rise on the next nrfxlib is absorbed rather than met with a controller
 * that will not enable.
 */
#ifndef HCI_NRF52840_SDC_MEM_MARGIN
#define HCI_NRF52840_SDC_MEM_MARGIN 512U
#endif

#ifndef HCI_NRF52840_DEFAULT_SDC_MEM_SIZE
#define HCI_NRF52840_DEFAULT_SDC_MEM_SIZE                                     \
    (HCI_NRF52840_SDC_MEM_REQUIRED + HCI_NRF52840_SDC_MEM_MARGIN)
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* HCI_NRF52840_H */
