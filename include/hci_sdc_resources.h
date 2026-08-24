/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_SDC_RESOURCES_H
#define HCI_SDC_RESOURCES_H

#include <stdint.h>

#include "hci_core_profile.h"
#include "sdc.h"
#include "sdc_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the SoftDevice Controller is configured for, and the memory pool that
 * configuration needs.
 *
 * None of this is specific to a part. nrfxlib ships one sdc.h covering nrf52,
 * nrf53, nrf54h, nrf54l, nrf54lm, nrf54ls, nrf54lv and nrf71, and every
 * SDC_MEM_ macro used below comes from it. A port supplies the clock, the
 * host interface and the errata; it does not get an opinion about how many
 * links the controller supports.
 *
 * This lived in hci_nrf52840.h, where three quarters of the file was this and
 * none of it was about the nRF52840. A second part would have had to copy it.
 */
/*
 * These are constants, not build options. Changing what the controller is
 * configured for means editing the value here, and the pool follows. The costs
 * on an nRF52840 with a 251 octet payload and four packets each way, from the
 * current sdk-nrfxlib:
 *
 *   peripheral link  2951 octets      central link  2855 octets
 *   advertising set   961 octets      scan buffers  1688 for four
 *   accept list        68 for eight   channel survey  40
 *   power control     2227 for eighteen links
 *   subrating         1092 for eighteen links
 *   extended features 4673 for eighteen links at ten pages
 *   frame space update 1236 for eighteen links
 *   shorter intervals  948 for eighteen links
 *   parallel scan and initiate  384
 *   periodic adv set  753 each  periodic sync 1787 each with responses
 *   periodic adv list   8 each  sync transfer 2515 for eighteen links
 *   periodic set with responses 2014 each
 *
 * The product profile needs up to sixteen simultaneous peripheral-role links
 * and two simultaneous central-role links. That is eighteen connection
 * contexts in the controller; it is not an eight-plus-eight split.
 */
#define HCI_SDC_PERIPHERAL_COUNT 16U
#define HCI_SDC_CENTRAL_COUNT     2U

/*
 * The payload the controller advertises in LE Read Buffer Size, and how many
 * buffers it keeps. 251 is the data length extension maximum, and a host that
 * is told 251 can use it: the alternative is a controller that quietly caps
 * throughput at a ninth of what the radio does.
 */
#define HCI_SDC_ACL_PACKET_SIZE  251U
#define HCI_SDC_ACL_PACKET_COUNT 4U

/*
 * Advertising sets. Three, because a periodic advertiser needs one of its own
 * and a periodic advertiser with responses needs another, leaving one for
 * ordinary extended advertising.
 */
#define HCI_SDC_ADV_SET_COUNT    3U
#define HCI_SDC_MAX_ADV_DATA     255U

#define HCI_SDC_SCAN_BUFFER_COUNT 4U

/* Filter accept list entries. The SoftDevice Controller default is eight. */
#define HCI_SDC_FAL_SIZE 8U

/*
 * Feature pages the controller keeps per link, which is what LE Read All
 * Remote Features reads. Ten is the sdk-nrfxlib default and the most a peer
 * may legally use. It is also one of the largest link-scaled terms here: 11
 * plus 19 per link plus 24 per page per link, or 4673 across eighteen links.
 * Three pages would cost much less, and the specification defines rather fewer
 * than ten today.
 *
 * The same value is given to sdc_cfg_set, so the pool and the controller
 * cannot disagree about it.
 */
#define HCI_SDC_EXTENDED_FEATURE_PAGES                                   \
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
#define HCI_SDC_PERIODIC_ADV_COUNT         1U
#define HCI_SDC_PERIODIC_SYNC_COUNT        2U
#define HCI_SDC_PERIODIC_SYNC_BUFFER_COUNT 4U
#define HCI_SDC_PERIODIC_ADV_LIST_SIZE     8U

/*
 * Periodic advertising with responses. The advertiser divides each period into
 * subevents and offers response slots, so a device that heard the broadcast
 * answers in one without forming a connection.
 *
 * Transmit buffers are what the advertiser puts in a subevent, receive buffers
 * what it collects from the response slots. PAwR has Host-to-Controller timing
 * deadlines, so using the minimum one-buffer configuration makes successful
 * operation depend unnecessarily on host event-service latency. Keep the
 * sdk-nrfxlib defaults here: three transmit buffers and two receive buffers.
 * The transmit size caps one subevent rather than the whole period.
 *
 * Failure reporting tells the advertiser about slots that were expected and
 * stayed empty. Off, as it is in sdk-nrfxlib, because it costs 224 and the
 * usual question is what answered rather than what did not. Correct PAwR
 * operation must not depend on those extra failure events being generated.
 */
#define HCI_SDC_PERIODIC_ADV_RSP_COUNT      1U
#define HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS 3U
#define HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS 2U
#define HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA                             \
    SDC_DEFAULT_PERIODIC_ADV_RSP_MAX_TX_DATA
#define HCI_SDC_PERIODIC_ADV_RSP_FAILURE_REPORTING 0U

/* Responses the scanner can hold before the controller sends them. */
#define HCI_SDC_PERIODIC_SYNC_RSP_TX_BUFFERS 1U

/*
 * A sync costs more once the controller can answer in a response slot, and the
 * vendor macro is per sync rather than per feature, so this is the price of
 * every sync rather than an extra term.
 */
#define HCI_SDC_MEM_PER_SYNC                                         \
    SDC_MEM_PER_PERIODIC_SYNC_RSP(HCI_SDC_PERIODIC_SYNC_RSP_TX_BUFFERS,  \
                                  HCI_SDC_PERIODIC_SYNC_BUFFER_COUNT)

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
#define HCI_SDC_MEM_PERIODIC_ADV_RSP                                 \
    (SDC_MEM_PER_PERIODIC_ADV_RSP_SET(                                        \
         HCI_SDC_MAX_ADV_DATA,                                           \
         HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS,                            \
         HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS,                            \
         HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA,                           \
         HCI_SDC_PERIODIC_ADV_RSP_FAILURE_REPORTING) *                   \
     HCI_SDC_PERIODIC_ADV_RSP_COUNT)

/*
 * Isochronous channels. Four roles, because this is a controller and a
 * controller does not get to decide which side of a stream someone wants to
 * be. Two connected groups of four streams between them, and two broadcast
 * groups with two streams out and two in.
 *
 * These counts decide the pool and nothing else. Whether the streams can be
 * encrypted is a separate question, answered by the part rather than by any
 * value here, and the README section on isochronous channels is where that
 * is written down. In short: sdk-nrfxlib lists nRF52820 and nRF52833 as the
 * nRF52 devices that encrypt isochronous packets and nRF52840 is not among
 * them, so raising or lowering these numbers changes what fits and never
 * changes that.
 *
 * Unencrypted broadcast is a real configuration rather than a degraded one,
 * since a public broadcast is unencrypted by design. Unencrypted connected
 * streams are the ones with something still to establish on hardware.
 */
#define HCI_SDC_CIG_COUNT           2U
#define HCI_SDC_CIS_COUNT           4U
#define HCI_SDC_BIG_COUNT           2U
#define HCI_SDC_BIS_SOURCE_COUNT    2U
#define HCI_SDC_BIS_SINK_COUNT      2U

/*
 * The service data unit sizes decide how large a buffer the application has
 * to hand sdc_hci_get. Vol 4 Part E allows an isochronous packet of 4095
 * octets, but sdc.h ties the requirement to the configured size rather than
 * that ceiling, so a modest number here keeps HCI_APP_PACKET_SIZE where it
 * is. 247 octets covers an LC3 frame at every bit rate the codec defines.
 */
#define HCI_SDC_ISO_TX_SDU_SIZE     247U
#define HCI_SDC_ISO_RX_SDU_SIZE     251U
#define HCI_SDC_ISO_TX_SDU_COUNT    4U
#define HCI_SDC_ISO_RX_SDU_COUNT    4U

/*
 * The controller's own ceiling, from sdk-nrfxlib limitations.rst, DRGN-25316:
 * sending and receiving isochronous units larger than 1255 octets is not
 * supported. Vol 4 Part E allows 4095, so a size between the two is a value
 * the specification permits and this controller does not, and the assertions
 * below say so at build time rather than leaving it to be found on a stream
 * that moves nothing.
 */
#define HCI_SDC_ISO_SDU_LIMIT       1255U

static_assert(HCI_SDC_ISO_TX_SDU_SIZE <= HCI_SDC_ISO_SDU_LIMIT,
              "transmit SDU is past what the controller supports");
static_assert(HCI_SDC_ISO_RX_SDU_SIZE <= HCI_SDC_ISO_SDU_LIMIT,
              "receive SDU is past what the controller supports");

/* Protocol data units per stream, each side. */
#define HCI_SDC_ISO_TX_PDU_PER_STREAM 3U
#define HCI_SDC_ISO_RX_PDU_PER_STREAM 3U

/* Every term the four roles above add to the pool, in one place. */
#define HCI_SDC_MEM_ISO                                                  \
    (SDC_MEM_PER_CIG(HCI_SDC_CIG_COUNT) +                                \
     SDC_MEM_PER_CIS(HCI_SDC_CIS_COUNT) +                                \
     SDC_MEM_PER_BIG(HCI_SDC_BIG_COUNT) +                                \
     SDC_MEM_PER_BIS(HCI_SDC_BIS_SOURCE_COUNT +                          \
                     HCI_SDC_BIS_SINK_COUNT) +                           \
     SDC_MEM_ISO_RX_PDU_POOL_PER_STREAM_SIZE(                            \
         HCI_SDC_ISO_RX_PDU_PER_STREAM, HCI_SDC_CIS_COUNT,               \
         HCI_SDC_BIS_SINK_COUNT) +                                       \
     SDC_MEM_ISO_RX_SDU_POOL_SIZE(HCI_SDC_ISO_RX_SDU_COUNT,              \
                                  HCI_SDC_ISO_RX_SDU_SIZE) +             \
     SDC_MEM_ISO_TX_PDU_POOL_SIZE(HCI_SDC_ISO_TX_PDU_PER_STREAM,         \
                                  HCI_SDC_CIS_COUNT,                     \
                                  HCI_SDC_BIS_SOURCE_COUNT) +            \
     SDC_MEM_ISO_TX_SDU_POOL_SIZE(HCI_SDC_ISO_TX_SDU_COUNT,              \
                                  HCI_SDC_ISO_TX_SDU_SIZE))

/*
 * The largest isochronous packet sdc_hci_get can hand back, so the packet
 * buffer above it can be checked against something true rather than against
 * the 4095 octet ceiling the specification allows. sdc_hci.h ties the
 * requirement to rx_sdu_buffer_size, not to that ceiling.
 */
#define HCI_SDC_ISO_PACKET_SIZE                                          \
    (HCI_SDC_ISO_RX_SDU_SIZE + HCI_ISO_DATA_HEADER_SIZE)

/*
 * Core 6.2 adds Frame Space Update and Shorter Connection Intervals. They are
 * real nRF52 multirole SDC capabilities, but their memory must enter the pool
 * only with the same product-profile gate as their sdc_support_* calls and HCI
 * commands. Keeping all three behind one version gate prevents a half-enabled
 * controller profile.
 */
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
#define HCI_SDC_MEM_FRAME_SPACE_UPDATE                                  \
    SDC_MEM_FRAME_SPACE_UPDATE(HCI_SDC_PERIPHERAL_COUNT +               \
                               HCI_SDC_CENTRAL_COUNT)
#define HCI_SDC_MEM_SHORTER_CONNECTION_INTERVALS                        \
    SDC_MEM_SHORTER_CONNECTION_INTERVALS(HCI_SDC_PERIPHERAL_COUNT +     \
                                         HCI_SDC_CENTRAL_COUNT)
#else
#define HCI_SDC_MEM_FRAME_SPACE_UPDATE 0U
#define HCI_SDC_MEM_SHORTER_CONNECTION_INTERVALS 0U
#endif

#define HCI_SDC_MEM_REQUIRED                                         \
    (SDC_MEM_PER_PERIPHERAL_LINK(HCI_SDC_ACL_PACKET_SIZE,                \
                                 HCI_SDC_ACL_PACKET_SIZE,                \
                                 HCI_SDC_ACL_PACKET_COUNT,               \
                                 HCI_SDC_ACL_PACKET_COUNT) *             \
         HCI_SDC_PERIPHERAL_COUNT +                                      \
     SDC_MEM_PER_CENTRAL_LINK(HCI_SDC_ACL_PACKET_SIZE,                   \
                              HCI_SDC_ACL_PACKET_SIZE,                   \
                              HCI_SDC_ACL_PACKET_COUNT,                  \
                              HCI_SDC_ACL_PACKET_COUNT) *                \
         HCI_SDC_CENTRAL_COUNT +                                         \
     SDC_MEM_PERIPHERAL_LINKS_SHARED + SDC_MEM_CENTRAL_LINKS_SHARED +         \
     SDC_MEM_SCAN_EXT(HCI_SDC_SCAN_BUFFER_COUNT) +                       \
     SDC_MEM_PER_ADV_SET(HCI_SDC_MAX_ADV_DATA) *                         \
         HCI_SDC_ADV_SET_COUNT +                                         \
     SDC_MEM_FAL(HCI_SDC_FAL_SIZE) +                                     \
     SDC_MEM_QOS_CHANNEL_SURVEY +                                             \
     SDC_MEM_LE_POWER_CONTROL(HCI_SDC_PERIPHERAL_COUNT +                 \
                              HCI_SDC_CENTRAL_COUNT) +                   \
     SDC_MEM_SUBRATING(HCI_SDC_PERIPHERAL_COUNT +                        \
                       HCI_SDC_CENTRAL_COUNT) +                          \
     SDC_MEM_EXTENDED_FEATURE_SET(HCI_SDC_PERIPHERAL_COUNT +             \
                                      HCI_SDC_CENTRAL_COUNT,             \
                                  HCI_SDC_EXTENDED_FEATURE_PAGES) +      \
     HCI_SDC_MEM_FRAME_SPACE_UPDATE +                                    \
     HCI_SDC_MEM_SHORTER_CONNECTION_INTERVALS +                          \
     SDC_MEM_INITIATOR +                                                      \
     SDC_MEM_PER_PERIODIC_ADV_SET(HCI_SDC_MAX_ADV_DATA) *                \
         HCI_SDC_PERIODIC_ADV_COUNT +                                    \
     HCI_SDC_MEM_PER_SYNC * HCI_SDC_PERIODIC_SYNC_COUNT +       \
     SDC_MEM_PERIODIC_ADV_LIST(HCI_SDC_PERIODIC_ADV_LIST_SIZE) +         \
     SDC_MEM_SYNC_TRANSFER(HCI_SDC_PERIPHERAL_COUNT +                    \
                           HCI_SDC_CENTRAL_COUNT) +                      \
     HCI_SDC_MEM_PERIODIC_ADV_RSP +                                      \
     HCI_SDC_MEM_ISO)

/*
 * sdc.h says the memory requirement defines "may change between minor
 * releases", and the number that decides whether the controller starts is the
 * one sdc_cfg_set answers at run time, not this one. The margin is there so a
 * small rise on the next nrfxlib is absorbed rather than met with a controller
 * that will not enable.
 */
#define HCI_SDC_MEM_MARGIN 512U

#define HCI_SDC_MEM_SIZE                                     \
    (HCI_SDC_MEM_REQUIRED + HCI_SDC_MEM_MARGIN)


/*
 * Applies the whole configuration: every sdc_support_ call the commands in
 * hci_sdc_nrfxlib.cpp need, then the sdc_cfg_set sequence.
 *
 * Called between sdc_init and sdc_enable. Returns the memory sdc_cfg_set says
 * the configuration needs, which the caller compares against the pool it
 * reserved, or a negative nrf_errno on failure.
 */
int32_t HciSdcResourcesApply(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_SDC_RESOURCES_H */
