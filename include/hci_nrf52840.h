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
 * These are constants, not build options. Changing what the controller is
 * configured for means editing the value here, and the pool follows. The costs
 * on an nRF52840 with a 251 octet payload and four packets each way, from the
 * current sdk-nrfxlib:
 *
 *   peripheral link  2935 octets      central link  2839 octets
 *   advertising set   961 octets      scan buffers  1688 for four
 *   accept list        68 for eight   channel survey  40
 *   power control     997 for eight links
 *   subrating         492 for eight links
 *   extended features 2083 for eight links at ten pages
 *   parallel scan and initiate  384
 *   periodic adv set  753 each  periodic sync 1787 each with responses
 *   periodic adv list   8 each  sync transfer 1125 for eight links
 *   periodic set with responses 1575 each
 */
#define HCI_NRF52840_PERIPHERAL_COUNT 4U
#define HCI_NRF52840_CENTRAL_COUNT    4U

/*
 * The payload the controller advertises in LE Read Buffer Size, and how many
 * buffers it keeps. 251 is the data length extension maximum, and a host that
 * is told 251 can use it: the alternative is a controller that quietly caps
 * throughput at a ninth of what the radio does.
 */
#define HCI_NRF52840_ACL_PACKET_SIZE  251U
#define HCI_NRF52840_ACL_PACKET_COUNT 4U

/*
 * Advertising sets. Three, because a periodic advertiser needs one of its own
 * and a periodic advertiser with responses needs another, leaving one for
 * ordinary extended advertising.
 */
#define HCI_NRF52840_ADV_SET_COUNT    3U
#define HCI_NRF52840_MAX_ADV_DATA     255U

#define HCI_NRF52840_SCAN_BUFFER_COUNT 4U

/* Filter accept list entries. The SoftDevice Controller default is eight. */
#define HCI_NRF52840_FAL_SIZE 8U

/*
 * Feature pages the controller keeps per link, which is what LE Read All
 * Remote Features reads. Ten is the sdk-nrfxlib default and the most a peer
 * may legally use. It is also the most expensive number here: 11 plus 19 per
 * link plus 24 per page per link, so 2083 across eight links. Three pages
 * would be 739, and the specification defines rather fewer than ten today.
 *
 * The same value is given to sdc_cfg_set, so the pool and the controller
 * cannot disagree about it.
 */
#define HCI_NRF52840_EXTENDED_FEATURE_PAGES                                   \
    SDC_DEFAULT_EXTENDED_FEATURE_PAGE_COUNT

/*
 * Periodic advertising. One train transmitted, two followed, and an eight
 * entry advertiser list so a scanner can name trains rather than carry every
 * address in the host. The controller defaults that list to zero entries, so
 * a host reading the size would otherwise be told it does not work.
 *
 * The syncs are the expensive part, 1787 each with responses enabled, and the
 * first numbers to bring down if the pool has to shrink.
 */
#define HCI_NRF52840_PERIODIC_ADV_COUNT         1U
#define HCI_NRF52840_PERIODIC_SYNC_COUNT        2U
#define HCI_NRF52840_PERIODIC_SYNC_BUFFER_COUNT 4U
#define HCI_NRF52840_PERIODIC_ADV_LIST_SIZE     8U

/*
 * Periodic advertising with responses. The advertiser divides each period into
 * subevents and offers response slots, so a device that heard the broadcast
 * answers in one without forming a connection.
 *
 * Transmit buffers are what the advertiser puts in a subevent, receive buffers
 * what it collects from the response slots. The transmit size caps one
 * subevent rather than the whole period, so it is smaller than it looks.
 *
 * Failure reporting tells the advertiser about slots that were expected and
 * stayed empty. Off, as it is in sdk-nrfxlib, because it costs 224 and the
 * usual question is what answered rather than what did not.
 */
#define HCI_NRF52840_PERIODIC_ADV_RSP_COUNT      1U
#define HCI_NRF52840_PERIODIC_ADV_RSP_TX_BUFFERS 1U
#define HCI_NRF52840_PERIODIC_ADV_RSP_RX_BUFFERS 1U
#define HCI_NRF52840_PERIODIC_ADV_RSP_MAX_TX_DATA                             \
    SDC_DEFAULT_PERIODIC_ADV_RSP_MAX_TX_DATA
#define HCI_NRF52840_PERIODIC_ADV_RSP_FAILURE_REPORTING 0U

/* Responses the scanner can hold before the controller sends them. */
#define HCI_NRF52840_PERIODIC_SYNC_RSP_TX_BUFFERS 1U

/*
 * A sync costs more once the controller can answer in a response slot, and the
 * vendor macro is per sync rather than per feature, so this is the price of
 * every sync rather than an extra term.
 */
#define HCI_NRF52840_SDC_MEM_PER_SYNC                                         \
    SDC_MEM_PER_PERIODIC_SYNC_RSP(HCI_NRF52840_PERIODIC_SYNC_RSP_TX_BUFFERS,  \
                                  HCI_NRF52840_PERIODIC_SYNC_BUFFER_COUNT)

/*
 * SDC_MEM_PER_PERIODIC_ADV_RSP_SET already includes a plain periodic set
 * inside itself, so this is a whole set rather than an increment on one, and
 * it adds to the plain sets rather than replacing them.
 *
 * Whether sdk-nrfxlib counts a responding set against periodic_adv_count as
 * well is not stated. Reserving for both is the safe direction: too much pool
 * wastes RAM, too little is a controller that will not enable. The figure
 * sdc_cfg_set answers at run time is what settles it, and that is reported in
 * the counter block.
 */
#define HCI_NRF52840_SDC_MEM_PERIODIC_ADV_RSP                                 \
    (SDC_MEM_PER_PERIODIC_ADV_RSP_SET(                                        \
         HCI_NRF52840_MAX_ADV_DATA,                                           \
         HCI_NRF52840_PERIODIC_ADV_RSP_TX_BUFFERS,                            \
         HCI_NRF52840_PERIODIC_ADV_RSP_RX_BUFFERS,                            \
         HCI_NRF52840_PERIODIC_ADV_RSP_MAX_TX_DATA,                           \
         HCI_NRF52840_PERIODIC_ADV_RSP_FAILURE_REPORTING) *                   \
     HCI_NRF52840_PERIODIC_ADV_RSP_COUNT)

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
     SDC_MEM_FAL(HCI_NRF52840_FAL_SIZE) +                                     \
     SDC_MEM_QOS_CHANNEL_SURVEY +                                             \
     SDC_MEM_LE_POWER_CONTROL(HCI_NRF52840_PERIPHERAL_COUNT +                 \
                              HCI_NRF52840_CENTRAL_COUNT) +                   \
     SDC_MEM_SUBRATING(HCI_NRF52840_PERIPHERAL_COUNT +                        \
                       HCI_NRF52840_CENTRAL_COUNT) +                          \
     SDC_MEM_EXTENDED_FEATURE_SET(HCI_NRF52840_PERIPHERAL_COUNT +             \
                                      HCI_NRF52840_CENTRAL_COUNT,             \
                                  HCI_NRF52840_EXTENDED_FEATURE_PAGES) +      \
     SDC_MEM_INITIATOR +                                                      \
     SDC_MEM_PER_PERIODIC_ADV_SET(HCI_NRF52840_MAX_ADV_DATA) *                \
         HCI_NRF52840_PERIODIC_ADV_COUNT +                                    \
     HCI_NRF52840_SDC_MEM_PER_SYNC * HCI_NRF52840_PERIODIC_SYNC_COUNT +       \
     SDC_MEM_PERIODIC_ADV_LIST(HCI_NRF52840_PERIODIC_ADV_LIST_SIZE) +         \
     SDC_MEM_SYNC_TRANSFER(HCI_NRF52840_PERIPHERAL_COUNT +                    \
                           HCI_NRF52840_CENTRAL_COUNT) +                      \
     HCI_NRF52840_SDC_MEM_PERIODIC_ADV_RSP)

/*
 * sdc.h says the memory requirement defines "may change between minor
 * releases", and the number that decides whether the controller starts is the
 * one sdc_cfg_set answers at run time, not this one. The margin is there so a
 * small rise on the next nrfxlib is absorbed rather than met with a controller
 * that will not enable.
 */
#define HCI_NRF52840_SDC_MEM_MARGIN 512U

#define HCI_NRF52840_DEFAULT_SDC_MEM_SIZE                                     \
    (HCI_NRF52840_SDC_MEM_REQUIRED + HCI_NRF52840_SDC_MEM_MARGIN)

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
