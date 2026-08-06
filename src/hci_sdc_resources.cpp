/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_sdc_resources.h"

#include <stddef.h>

#include "hci_trace.h"

/*
 * Every sdc_support_ call the dispatch table needs, and the sdc_cfg_set
 * sequence that sizes the controller. None of it touches a peripheral, a
 * clock or an erratum, so it is the same on any part nrfxlib supports.
 *
 * The order matters in three places and sdk-nrfxlib says so:
 *
 *   path loss monitoring needs a power control role first;
 *   scanning while initiating needs a central role first;
 *   periodic advertising with responses needs extended advertising, the
 *   matching plain periodic half, and a sync transfer role, all first.
 */

/*
 * A periodic advertiser needs an advertising set to carry it, and one with
 * responses needs another, so the two periodic counts together cannot exceed
 * the advertising sets. sdc_cfg_set refuses that with an error naming neither
 * number, so it is caught at build time instead.
 */
static_assert((HCI_SDC_PERIODIC_ADV_COUNT + HCI_SDC_PERIODIC_ADV_RSP_COUNT) <=
                  HCI_SDC_ADV_SET_COUNT,
              "periodic advertisers exceed the advertising sets");

static int32_t s_Required;

static bool HciSdcCfgSet(uint8_t Type, const sdc_cfg_t *pCfg)
{
    int32_t required = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, Type, pCfg);
    HciTrace("sdc: cfg type=%u result=%ld\r\n", (unsigned)Type,
             (long)required);
    if (required < 0)
    {
        s_Required = required;
        return false;
    }

    s_Required = required;
    return true;
}

int32_t HciSdcResourcesApply(void)
{
    s_Required = 0;

    sdc_support_ext_adv();
    sdc_support_peripheral();
    sdc_support_ext_central();
    sdc_support_le_2m_phy();
    sdc_support_le_coded_phy();
    sdc_support_dle_peripheral();
    sdc_support_dle_central();
    sdc_support_phy_update_peripheral();
    sdc_support_phy_update_central();
    sdc_support_direct_test_mode();

    /*
     * Address resolution in the controller. Without it the resolving list
     * commands are rejected and a bonded peer arriving under a new resolvable
     * private address is a stranger, which is what a phone looks like on every
     * reconnection. Must be called before sdc_cfg_set and sdc_enable, which is
     * why it sits here with the rest.
     */
    sdc_support_le_privacy();

    /*
     * The channel survey module, which the vendor command at 0xFD0E turns on
     * and off. Without this call that command is rejected, so the two have to
     * agree, and the pool in hci_sdc_resources.h holds the matching 40
     * octets.
     */
    sdc_support_qos_channel_survey();

    /*
     * Both roles, because this image supports both and sdk-nrfxlib asks for a
     * call per role rather than one for the pair. Path loss monitoring has to
     * come after at least one of them, which is why the order here is not
     * alphabetical.
     */
    sdc_support_le_power_control_central();
    sdc_support_le_power_control_peripheral();
    sdc_support_le_path_loss_monitoring();

    sdc_support_sca_central();
    sdc_support_sca_peripheral();

    sdc_support_connection_subrating_central();
    sdc_support_connection_subrating_peripheral();

    sdc_support_extended_feature_set_central();
    sdc_support_extended_feature_set_peripheral();

    /*
     * sdk-nrfxlib asks for a central role before this, which
     * sdc_support_ext_central() above provides.
     */
    sdc_support_parallel_scanning_and_initiating();

    sdc_support_le_periodic_adv();

    sdc_support_le_periodic_sync();

    /*
     * Four calls rather than two. Sending and receiving a sync are separate
     * capabilities and each is per role, so a build that only ever hands a
     * sync out still needs both sender calls, and this image supports both
     * roles.
     */
    sdc_support_periodic_adv_sync_transfer_sender_central();
    sdc_support_periodic_adv_sync_transfer_sender_peripheral();
    sdc_support_periodic_adv_sync_transfer_receiver_central();
    sdc_support_periodic_adv_sync_transfer_receiver_peripheral();

    /*
     * Periodic advertising with responses, last because sdk-nrfxlib requires
     * extended advertising, the matching plain periodic half, and a sync
     * transfer sender or receiver, all of which are above.
     */
    sdc_support_le_periodic_adv_with_rsp();

    sdc_support_le_periodic_sync_with_rsp();

    sdc_cfg_t cfg = {};
    cfg.buffer_cfg.rx_packet_size = HCI_SDC_ACL_PACKET_SIZE;
    cfg.buffer_cfg.tx_packet_size = HCI_SDC_ACL_PACKET_SIZE;
    cfg.buffer_cfg.rx_packet_count = HCI_SDC_ACL_PACKET_COUNT;
    cfg.buffer_cfg.tx_packet_count = HCI_SDC_ACL_PACKET_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_BUFFER_CFG, &cfg)) return s_Required;

    cfg = {};
    cfg.peripheral_count.count = HCI_SDC_PERIPHERAL_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIPHERAL_COUNT, &cfg)) return s_Required;

    cfg = {};
    cfg.central_count.count = HCI_SDC_CENTRAL_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_CENTRAL_COUNT, &cfg)) return s_Required;

    cfg = {};
    cfg.adv_count.count = HCI_SDC_ADV_SET_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_ADV_COUNT, &cfg)) return s_Required;

    cfg = {};
    cfg.adv_buffer_cfg.max_adv_data = HCI_SDC_MAX_ADV_DATA;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_ADV_BUFFER_CFG, &cfg)) return s_Required;

    cfg = {};
    cfg.scan_buffer_cfg.count = HCI_SDC_SCAN_BUFFER_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_SCAN_BUFFER_CFG, &cfg)) return s_Required;

    /*
     * The filter accept list. Left unset the controller takes its own default,
     * which is eight, and a host that reads the size gets a number this build
     * never chose.
     */
    cfg = {};
    cfg.fal_size = HCI_SDC_FAL_SIZE;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_FAL_SIZE, &cfg)) return s_Required;

    /*
     * How many feature pages the controller keeps per link. The pool in
     * hci_sdc_resources.h is computed from the same macro, so the two cannot
     * disagree about what was reserved.
     */
    cfg = {};
    cfg.extended_feature_page_count = HCI_SDC_EXTENDED_FEATURE_PAGES;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_EXTENDED_FEATURE_PAGE_COUNT,
                           &cfg))
    {
        return s_Required;
    }

    /*
     * Periodic advertisers. Each takes one of the advertising sets configured
     * above, which is why the static_assert at the top of this file refuses a
     * count larger than that one
     * at build time rather than letting sdc_cfg_set refuse it here.
     */
    cfg = {};
    cfg.periodic_adv_count.count = HCI_SDC_PERIODIC_ADV_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_COUNT, &cfg))
    {
        return s_Required;
    }

    cfg = {};
    cfg.periodic_sync_count.count = HCI_SDC_PERIODIC_SYNC_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_SYNC_COUNT, &cfg))
    {
        return s_Required;
    }

    cfg = {};
    cfg.periodic_sync_buffer_cfg.count =
        HCI_SDC_PERIODIC_SYNC_BUFFER_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_SYNC_BUFFER_CFG,
                           &cfg))
    {
        return s_Required;
    }

    /*
     * The periodic advertiser list. The controller default is zero, so left
     * unset a host that reads the size is told the list does not work, and
     * every train has to be named by address instead.
     */
    cfg = {};
    cfg.periodic_adv_list_size = HCI_SDC_PERIODIC_ADV_LIST_SIZE;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_LIST_SIZE, &cfg))
    {
        return s_Required;
    }

    cfg = {};
    cfg.periodic_adv_rsp_count.count = HCI_SDC_PERIODIC_ADV_RSP_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_RSP_COUNT, &cfg))
    {
        return s_Required;
    }

    /*
     * All three buffer numbers together, because the vendor structure holds
     * them in one member and the pool macro takes all three. Setting one and
     * leaving the others to their defaults would compute a pool for numbers the
     * controller was never given.
     */
    cfg = {};
    cfg.periodic_adv_rsp_buffer_cfg.tx_buffer_count =
        HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.rx_buffer_count =
        HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.max_tx_data_size =
        HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_RSP_BUFFER_CFG,
                           &cfg))
    {
        return s_Required;
    }

    cfg = {};
    cfg.periodic_adv_rsp_failure_reporting_cfg =
        HCI_SDC_PERIODIC_ADV_RSP_FAILURE_REPORTING;
    if (!HciSdcCfgSet(
            SDC_CFG_TYPE_PERIODIC_ADV_RSP_FAILURE_REPORTING_CFG,
            &cfg))
    {
        return s_Required;
    }

    cfg = {};
    cfg.periodic_sync_rsp_tx_buffer_cfg.count =
        HCI_SDC_PERIODIC_SYNC_RSP_TX_BUFFERS;
    if (!HciSdcCfgSet(
            SDC_CFG_TYPE_PERIODIC_SYNC_RSP_TX_BUFFER_CFG, &cfg))
    {
        return s_Required;
    }


    int32_t required =
        sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, SDC_CFG_TYPE_NONE, nullptr);
    if (required < 0)
    {
        return required;
    }

    return required;
}
