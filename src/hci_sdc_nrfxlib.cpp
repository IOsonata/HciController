/**-------------------------------------------------------------------------
@file	hci_sdc_nrfxlib.cpp

@brief	SoftDevice Controller HCI command adapters and command table.

		Implements HciController command handlers over sdk-nrfxlib SDC,
		including Core, LE, vendor-specific, PAwR, ISO, capability, and
		parameter-shape validation plus advertising command-family policy.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_sdc_nrfxlib.h"

#include "hci_counters.h"

#include <string.h>

#include "nrf_errno.h"
#include "sdc_hci.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_link_control.h"
#include "sdc_hci_cmd_info_params.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_cmd_status_params.h"
#include "sdc_hci_vs.h"

/*
 * The command table targets the multirole SoftDevice Controller library.
 * Central and peripheral roles are selected through HCI.
 *
 * The multirole library does not expose LE Read Supported States directly.
 * The generic HCI layer implements the compatible command response.
 *
 * tests/sdc_symbols.py verifies that required SDC symbols are available.
 *
 * Command groups requiring additional implementation notes:
 *
 * Vol 4 Part E 7.8.74, LE Read Transmit Power, is the minimum and maximum
 * transmit power supported across PHYs. It is independent of LE Power
 * Control and requires no additional sdc_support_ call.
 *
 * Read Remote Version Information, Vol 4 Part E 7.1.23, and the Authenticated
 * Payload Timeout pair, 7.3.93 and 7.3.94, are mandatory for an LE controller
 * and come with LE Ping.
 *
 * The quality of service commands, 0xFD04 to 0xFD1F, and the periodic
 * advertising reports all come back as events rather than command returns:
 * vendor subevents 0x80 to 0x82 for the former, LE meta subevents 0x0E to 0x10
 * and 0x18 for the latter. Those arrive from sdc_hci_get like any other event
 * and the transport forwards them without additional command processing.
 *
 * Channel survey, power control, sleep clock accuracy, subrating, extended
 * features, and periodic advertising require matching sdc_support_ calls
 * before sdc_cfg_set. The shared SDC resource setup enables these features.
 *
 * VS LLPM Mode Set and VS Connection Update belong together. LLPM permits
 * connection intervals of 1 to 7 ms, and the standard LE Connection Update
 * cannot ask for one: its interval is in units of 1.25 ms with a floor of six
 * units, so 7.5 ms is the shortest it can express. The vendor command takes
 * microseconds, which is the only way to reach the range LLPM opens. LLPM is
 * also Nordic to Nordic, so it is a bench and demo feature rather than an
 * interoperable one.
 *
 * LE Set Host Channel Classification pairs with the channel survey: the survey
 * says which channels are busy, this says which to avoid.
 */

typedef enum
{
    HCI_SDC_ADV_CMD_NONE = 0,
    HCI_SDC_ADV_CMD_LEGACY,
    HCI_SDC_ADV_CMD_EXTENDED,
} HciSdcAdvCmdType_t;

static HciSdcAdvCmdType_t s_AdvCmdTypeSinceReset = HCI_SDC_ADV_CMD_NONE;

static HciCmdResult_t HciSdcComplete(uint8_t Status, size_t ReturnLen)
{
    HciCmdResult_t result = {Status, HCI_CMD_RESPONSE_COMPLETE, ReturnLen};
    return result;
}

static HciCmdResult_t HciSdcStatus(uint8_t Status, size_t)
{
    HciCmdResult_t result = {Status, HCI_CMD_RESPONSE_STATUS, 0U};
    return result;
}

/*
 * Host Number Of Completed Packets has no response on success and returns
 * Command Complete on invalid parameters (Core Vol 4 Part E 7.3.40).
 * This helper is retained for possible future command support.
 */
__attribute__((unused))
static HciCmdResult_t HciSdcSilentOnSuccess(uint8_t Status, size_t)
{
    HciCmdResult_t result = {Status,
                             Status == HCI_STATUS_SUCCESS
                                 ? HCI_CMD_RESPONSE_NONE
                                 : HCI_CMD_RESPONSE_COMPLETE,
                             0U};
    return result;
}

static bool HciSdcReturnFits(size_t Required, size_t Capacity)
{
    return Required <= Capacity;
}

/*
 * Number of PHYs named in a scanning or initiating PHY bitmap. The commands
 * that take one carry a trailing array with one element per named PHY.
 */
static size_t HciSdcPhyCount(uint8_t Phys)
{
    size_t count = 0U;

    for (uint8_t mask = Phys; mask != 0U; mask = (uint8_t)(mask & (mask - 1U)))
    {
        count++;
    }

    return count;
}

static HciCmdResult_t HciSdcCmdSetEventMask(void *,
                                            const uint8_t *pParams,
                                            size_t,
                                            uint8_t *,
                                            size_t)
{
    sdc_hci_cmd_cb_set_event_mask_t params;
    memcpy(params.raw, pParams, sizeof(params.raw));
    return HciSdcComplete(sdc_hci_cmd_cb_set_event_mask(&params), 0U);
}

static HciCmdResult_t HciSdcCmdSetEventMaskPage2(void *,
                                                 const uint8_t *pParams,
                                                 size_t,
                                                 uint8_t *,
                                                 size_t)
{
    sdc_hci_cmd_cb_set_event_mask_page_2_t params;
    memcpy(params.raw, pParams, sizeof(params.raw));
    return HciSdcComplete(sdc_hci_cmd_cb_set_event_mask_page_2(&params), 0U);
}

static HciCmdResult_t HciSdcCmdReset(void *pContext,
                                     const uint8_t *,
                                     size_t,
                                     uint8_t *,
                                     size_t)
{
    const uint8_t status = sdc_hci_cmd_cb_reset();

    /*
     * Vol 4 Part E 7.3.2 puts the link layer in standby and drops every
     * connection, reporting none of them. So the routing layer's per link
     * accounting has to be cleared here or nothing clears it, and a handle the
     * controller hands out again after the reset inherits the old in flight
     * count and is throttled or stalled on a link that is actually empty.
     */
    if (status == HCI_STATUS_SUCCESS)
    {
        HciCounters_t *pCounters = static_cast<HciCounters_t *>(pContext);
        if (pCounters != NULL)
        {
            HciSdcResetFlowControl(pCounters->pSdc);
        }

        /*
         * Vol 4 Part E 3.1.1 ties the advertising command set a host has
         * chosen to the last reset, so this is where the choice is given up
         * and the host may pick the other one.
         */
        HciSdcNrfxlibResetAdvCommandType();
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdReadLocalVersion(void *,
                                                const uint8_t *,
                                                size_t,
                                                uint8_t *pReturn,
                                                size_t ReturnCapacity)
{
    sdc_hci_cmd_ip_read_local_version_information_return_t result;
    if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_ip_read_local_version_information(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, &result, sizeof(result));
        return HciSdcComplete(status, sizeof(result));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdReadSupportedCommands(void *,
                                                     const uint8_t *,
                                                     size_t,
                                                     uint8_t *pReturn,
                                                     size_t ReturnCapacity)
{
    sdc_hci_cmd_ip_read_local_supported_commands_return_t supported;
    memset(&supported, 0, sizeof(supported));

    if (!HciSdcReturnFits(sizeof(supported.raw), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    supported.params.hci_set_event_mask = 1U;
    supported.params.hci_reset = 1U;
    supported.params.hci_set_event_mask_page_2 = 1U;
    /*
     * The three controller to host flow control bits stay clear. The
     * controller refuses Host Buffer Size with 0x11 and no sdc_support call
     * turns it on, so advertising it promises something that does not happen.
     * The command table has no rows for them either; see the note there.
     */
    supported.params.hci_read_local_version_information = 1U;
    supported.params.hci_read_local_supported_features = 1U;
    supported.params.hci_read_bd_addr = 1U;
    supported.params.hci_le_set_event_mask = 1U;
    supported.params.hci_le_read_buffer_size_v1 = 1U;
    supported.params.hci_le_read_local_supported_features = 1U;
    supported.params.hci_le_set_random_address = 1U;
    supported.params.hci_le_set_advertising_parameters = 1U;
    supported.params.hci_le_read_advertising_physical_channel_tx_power = 1U;
    supported.params.hci_le_set_advertising_data = 1U;
    supported.params.hci_le_set_scan_response_data = 1U;
    supported.params.hci_le_set_advertising_enable = 1U;
    supported.params.hci_le_set_scan_parameters = 1U;
    supported.params.hci_le_set_scan_enable = 1U;

    supported.params.hci_disconnect = 1U;
    supported.params.hci_read_remote_version_information = 1U;
    supported.params.hci_read_authenticated_payload_timeout = 1U;
    supported.params.hci_write_authenticated_payload_timeout = 1U;
    supported.params.hci_le_create_connection = 1U;
    supported.params.hci_le_create_connection_cancel = 1U;
    supported.params.hci_le_connection_update = 1U;
    supported.params.hci_le_read_channel_map = 1U;
    supported.params.hci_le_read_remote_features = 1U;

    supported.params.hci_le_read_filter_accept_list_size = 1U;
    supported.params.hci_le_clear_filter_accept_list = 1U;
    supported.params.hci_le_add_device_to_filter_accept_list = 1U;
    supported.params.hci_le_remove_device_from_filter_accept_list = 1U;

    supported.params.hci_le_encrypt = 1U;
    supported.params.hci_le_rand = 1U;
    supported.params.hci_le_enable_encryption = 1U;
    supported.params.hci_le_long_term_key_request_reply = 1U;
    supported.params.hci_le_long_term_key_request_negative_reply = 1U;

    supported.params.hci_le_read_transmit_power = 1U;

    supported.params.hci_read_rssi = 1U;

    supported.params.hci_le_receiver_test_v1 = 1U;
    supported.params.hci_le_transmitter_test_v1 = 1U;
    supported.params.hci_le_receiver_test_v2 = 1U;
    supported.params.hci_le_transmitter_test_v2 = 1U;
    supported.params.hci_le_receiver_test_v3 = 1U;
    supported.params.hci_le_transmitter_test_v3 = 1U;
    supported.params.hci_le_transmitter_test_v4 = 1U;
    supported.params.hci_le_test_end = 1U;

    supported.params.hci_le_set_host_feature = 1U;
    supported.params.hci_le_set_host_feature_v2 = 1U;

    supported.params.hci_read_transmit_power_level = 1U;
    supported.params.hci_le_read_rf_path_compensation = 1U;
    supported.params.hci_le_write_rf_path_compensation = 1U;
    supported.params.hci_le_enhanced_read_transmit_power_level = 1U;
    supported.params.hci_le_read_remote_transmit_power_level = 1U;
    supported.params.hci_le_set_path_loss_reporting_parameters = 1U;
    supported.params.hci_le_set_path_loss_reporting_enable = 1U;
    supported.params.hci_le_set_transmit_power_reporting_enable = 1U;

    supported.params.hci_le_set_host_channel_classification = 1U;
    supported.params.hci_le_request_peer_sca = 1U;
    supported.params.hci_le_set_default_subrate_command = 1U;
    supported.params.hci_le_subrate_request_command = 1U;
    supported.params.hci_le_read_all_local_supported_features = 1U;
    supported.params.hci_le_read_all_remote_features = 1U;

    supported.params.hci_le_set_periodic_advertising_parameters = 1U;
    supported.params.hci_le_set_periodic_advertising_data = 1U;
    supported.params.hci_le_set_periodic_advertising_enable = 1U;
    supported.params.hci_le_periodic_advertising_create_sync = 1U;
    supported.params.hci_le_periodic_advertising_create_sync_cancel = 1U;
    supported.params.hci_le_periodic_advertising_terminate_sync = 1U;
    supported.params.hci_le_add_device_to_periodic_advertiser_list = 1U;
    supported.params.hci_le_remove_device_from_periodic_advertiser_list = 1U;
    supported.params.hci_le_clear_periodic_advertiser_list = 1U;
    supported.params.hci_le_read_periodic_advertiser_list_size = 1U;
    supported.params.hci_le_set_periodic_advertising_receive_enable = 1U;
    supported.params.hci_le_periodic_advertising_sync_transfer = 1U;
    supported.params.hci_le_periodic_advertising_set_info_transfer = 1U;
    supported.params.hci_le_set_periodic_advertising_sync_transfer_parameters =
        1U;
    supported.params
        .hci_le_set_default_periodic_advertising_sync_transfer_parameters = 1U;
    supported.params.hci_le_set_periodic_advertising_parameters_v2 = 1U;
    supported.params.hci_le_set_periodic_advertising_subevent_data = 1U;
    supported.params.hci_le_set_periodic_advertising_response_data = 1U;
    supported.params.hci_le_set_periodic_sync_subevent = 1U;

    supported.params.hci_le_set_data_length = 1U;
    supported.params.hci_le_read_suggested_default_data_length = 1U;
    supported.params.hci_le_write_suggested_default_data_length = 1U;
    supported.params.hci_le_read_maximum_data_length = 1U;

    supported.params.hci_le_read_phy = 1U;
    supported.params.hci_le_set_default_phy = 1U;
    supported.params.hci_le_set_phy = 1U;

    supported.params.hci_le_set_advertising_set_random_address = 1U;
    supported.params.hci_le_set_extended_advertising_parameters = 1U;
    supported.params.hci_le_set_extended_advertising_parameters_v2 = 1U;
    supported.params.hci_le_set_extended_advertising_data = 1U;
    supported.params.hci_le_set_extended_scan_response_data = 1U;
    supported.params.hci_le_set_extended_advertising_enable = 1U;
    supported.params.hci_le_read_maximum_advertising_data_length = 1U;
    supported.params.hci_le_read_number_of_supported_advertising_sets = 1U;
    supported.params.hci_le_remove_advertising_set = 1U;
    supported.params.hci_le_clear_advertising_sets = 1U;

    supported.params.hci_le_set_extended_scan_parameters = 1U;
    supported.params.hci_le_set_extended_scan_enable = 1U;
    supported.params.hci_le_extended_create_connection = 1U;
    supported.params.hci_le_extended_create_connection_v2 = 1U;

    supported.params.hci_le_add_device_to_resolving_list = 1U;
    supported.params.hci_le_remove_device_from_resolving_list = 1U;
    supported.params.hci_le_clear_resolving_list = 1U;
    supported.params.hci_le_read_resolving_list_size = 1U;
    supported.params.hci_le_set_address_resolution_enable = 1U;
    supported.params.hci_le_set_resolvable_private_address_timeout = 1U;
    supported.params.hci_le_set_privacy_mode = 1U;
    supported.params.hci_le_set_data_related_address_changes = 1U;

    /*
     * Isochronous channels. A bit here says the command is dispatched and
     * nothing more. Whether the streams it sets up can be encrypted is a
     * property of the part, and the bitmap has no way to express that, so a
     * host learns it from the answer to the command rather than from here.
     */
    supported.params.hci_le_read_buffer_size_v2 = 1U;
    supported.params.hci_le_read_iso_tx_sync = 1U;
    supported.params.hci_le_set_cig_parameters = 1U;
    supported.params.hci_le_set_cig_parameters_test = 1U;
    supported.params.hci_le_create_cis = 1U;
    supported.params.hci_le_remove_cig = 1U;
    supported.params.hci_le_accept_cis_request = 1U;
    supported.params.hci_le_reject_cis_request = 1U;
    supported.params.hci_le_create_big = 1U;
    supported.params.hci_le_create_big_test = 1U;
    supported.params.hci_le_terminate_big = 1U;
    supported.params.hci_le_big_create_sync = 1U;
    supported.params.hci_le_big_terminate_sync = 1U;
    supported.params.hci_le_setup_iso_data_path = 1U;
    supported.params.hci_le_remove_iso_data_path = 1U;
    supported.params.hci_le_iso_transmit_test = 1U;
    supported.params.hci_le_iso_receive_test = 1U;
    supported.params.hci_le_iso_read_test_counters = 1U;
    supported.params.hci_le_iso_test_end = 1U;
    supported.params.hci_le_read_iso_link_quality = 1U;

    memcpy(pReturn, supported.raw, sizeof(supported.raw));
    return HciSdcComplete(HCI_STATUS_SUCCESS, sizeof(supported.raw));
}

static HciCmdResult_t HciSdcCmdReadLocalFeatures(void *,
                                                 const uint8_t *,
                                                 size_t,
                                                 uint8_t *pReturn,
                                                 size_t ReturnCapacity)
{
    sdc_hci_cmd_ip_read_local_supported_features_return_t result;
    if (!HciSdcReturnFits(sizeof(result.raw), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_ip_read_local_supported_features(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, result.raw, sizeof(result.raw));
        return HciSdcComplete(status, sizeof(result.raw));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdReadBdAddr(void *,
                                          const uint8_t *,
                                          size_t,
                                          uint8_t *pReturn,
                                          size_t ReturnCapacity)
{
    sdc_hci_cmd_ip_read_bd_addr_return_t result;
    if (!HciSdcReturnFits(sizeof(result.bd_addr), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_ip_read_bd_addr(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, result.bd_addr, sizeof(result.bd_addr));
        return HciSdcComplete(status, sizeof(result.bd_addr));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdLeSetEventMask(void *,
                                              const uint8_t *pParams,
                                              size_t,
                                              uint8_t *,
                                              size_t)
{
    sdc_hci_cmd_le_set_event_mask_t params;
    memcpy(params.raw, pParams, sizeof(params.raw));
    return HciSdcComplete(sdc_hci_cmd_le_set_event_mask(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeReadBufferSize(void *pContext,
                                                const uint8_t *,
                                                size_t,
                                                uint8_t *pReturn,
                                                size_t ReturnCapacity)
{
    sdc_hci_cmd_le_read_buffer_size_return_t result;
    if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_le_read_buffer_size(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        /*
         * The number the host is told is the number it is entitled to use, so
         * the routing layer takes it from the answer rather than from the
         * build configuration, where the two could drift apart.
         */
        HciCounters_t *pCounters = static_cast<HciCounters_t *>(pContext);
        if (pCounters != NULL)
        {
            HciSdcSetAclLimit(pCounters->pSdc,
                              result.total_num_le_acl_data_packets);
        }

        memcpy(pReturn, &result, sizeof(result));
        return HciSdcComplete(status, sizeof(result));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdLeReadLocalFeatures(void *,
                                                   const uint8_t *,
                                                   size_t,
                                                   uint8_t *pReturn,
                                                   size_t ReturnCapacity)
{
    sdc_hci_cmd_le_read_local_supported_features_return_t result;
    if (!HciSdcReturnFits(sizeof(result.raw), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_le_read_local_supported_features(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, result.raw, sizeof(result.raw));
        return HciSdcComplete(status, sizeof(result.raw));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdLeSetRandomAddress(void *,
                                                  const uint8_t *pParams,
                                                  size_t,
                                                  uint8_t *,
                                                  size_t)
{
    sdc_hci_cmd_le_set_random_address_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_random_address(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeSetAdvParams(void *,
                                              const uint8_t *pParams,
                                              size_t,
                                              uint8_t *,
                                              size_t)
{
    sdc_hci_cmd_le_set_adv_params_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_adv_params(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeReadAdvTxPower(void *,
                                                const uint8_t *,
                                                size_t,
                                                uint8_t *pReturn,
                                                size_t ReturnCapacity)
{
    sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t result;
    if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_le_read_adv_physical_channel_tx_power(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, &result, sizeof(result));
        return HciSdcComplete(status, sizeof(result));
    }

    return HciSdcComplete(status, 0U);
}

static HciCmdResult_t HciSdcCmdLeSetAdvData(void *,
                                            const uint8_t *pParams,
                                            size_t,
                                            uint8_t *,
                                            size_t)
{
    sdc_hci_cmd_le_set_adv_data_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_adv_data(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeSetScanResponse(void *,
                                                 const uint8_t *pParams,
                                                 size_t,
                                                 uint8_t *,
                                                 size_t)
{
    sdc_hci_cmd_le_set_scan_response_data_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_scan_response_data(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeSetAdvEnable(void *,
                                              const uint8_t *pParams,
                                              size_t,
                                              uint8_t *,
                                              size_t)
{
    sdc_hci_cmd_le_set_adv_enable_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_adv_enable(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeSetScanParams(void *,
                                               const uint8_t *pParams,
                                               size_t,
                                               uint8_t *,
                                               size_t)
{
    sdc_hci_cmd_le_set_scan_params_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_scan_params(&params), 0U);
}

static HciCmdResult_t HciSdcCmdLeSetScanEnable(void *,
                                               const uint8_t *pParams,
                                               size_t,
                                               uint8_t *,
                                               size_t)
{
    sdc_hci_cmd_le_set_scan_enable_t params;
    memcpy(&params, pParams, sizeof(params));
    return HciSdcComplete(sdc_hci_cmd_le_set_scan_enable(&params), 0U);
}

/*
 * Most commands are the same few shapes, so they have no handler of their
 * own. The row names the SDC function and the dispatcher makes the call
 * (HciCmdGenericCall in hci_cmd_dispatch.cpp). The shapes are: parameters
 * only, parameters with a return, no parameters, and no parameters with a
 * return. Only the commands with an array whose size comes from the packet,
 * and the ones that do more than pass through, keep a handler below.
 *
 * The length the row gives is checked against the SDC structure at compile
 * time, and so is the size of the buffers the shared path keeps.
 */
template <size_t Size, size_t Given>
constexpr uint16_t HciSdcRowLen()
{
    static_assert(Size == Given, "row length differs from the SDC structure");
    static_assert(Size <= HCI_CMD_GENERIC_IN_MAX, "structure too large for a shared row");
    return (uint16_t)Size;
}

template <size_t Given>
constexpr uint16_t HciSdcRowLenN()
{
    static_assert(Given == 0, "a command without parameters has no length");
    return 0U;
}

template <size_t Size>
constexpr uint16_t HciSdcRowRet()
{
    static_assert(Size <= HCI_CMD_GENERIC_OUT_MAX, "return too large for a shared row");
    return (uint16_t)Size;
}

#define HCI_SDC_ROW_FN(Func) \
    reinterpret_cast<HciCmdHandler_t>(reinterpret_cast<void (*)(void)>(Func))

#define HCI_SDC_ROW_P(Opcode, Resp, SdcType, ParamLen, Func)                  \
    {Opcode, HciSdcRowLen<sizeof(SdcType), (ParamLen)>(), 0U, Resp,           \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_P}

#define HCI_SDC_ROW_PR(Opcode, SdcType, ParamLen, SdcReturn, Func)            \
    {Opcode, HciSdcRowLen<sizeof(SdcType), (ParamLen)>(),                     \
     HciSdcRowRet<sizeof(SdcReturn)>(), HCI_CMD_RESPONSE_COMPLETE,            \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_PR}

#define HCI_SDC_ROW_N(Opcode, Resp, ParamLen, Func)                           \
    {Opcode, HciSdcRowLenN<(ParamLen)>(), 0U, Resp,                           \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_N}

#define HCI_SDC_ROW_NR(Opcode, ParamLen, SdcReturn, Func)                     \
    {Opcode, HciSdcRowLenN<(ParamLen)>(), HciSdcRowRet<sizeof(SdcReturn)>(),  \
     HCI_CMD_RESPONSE_COMPLETE, HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_NR}

/*
 * Variable-length commands contain a trailing array in a packed SDC type.
 * Validate the declared element count against the received parameter length
 * before passing the buffer to SDC. All three count formats require an exact
 * length match (Core Vol 4 Part E 5.4.1), preventing out-of-bounds reads
 * from short packets and rejecting undeclared trailing data.
 */

/* The count field is a byte count, as in Vol 4 Part E 7.8.54. */
#define HCI_SDC_CMD_VB(Name, SdcFunc, SdcType, ArrayField, CountField, Reply) \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t ParamLen,                               \
                               uint8_t *,                                     \
                               size_t)                                        \
    {                                                                         \
        if (ParamLen < offsetof(SdcType, ArrayField))                         \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        const SdcType *pCmd = reinterpret_cast<const SdcType *>(pParams);     \
        if (ParamLen - offsetof(SdcType, ArrayField) !=                       \
            (size_t)pCmd->CountField)                                         \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        return Reply(SdcFunc(pCmd), 0U);                                      \
    }

/* The count field is a number of array elements, as in Vol 4 Part E 7.8.56. */
#define HCI_SDC_CMD_VN(Name, SdcFunc, SdcType, ArrayField, CountField, Reply) \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t ParamLen,                               \
                               uint8_t *,                                     \
                               size_t)                                        \
    {                                                                         \
        if (ParamLen < offsetof(SdcType, ArrayField))                         \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        const SdcType *pCmd = reinterpret_cast<const SdcType *>(pParams);     \
        const size_t needed = (size_t)pCmd->CountField *                      \
                              sizeof(pCmd->ArrayField[0]);                    \
        if (ParamLen - offsetof(SdcType, ArrayField) != needed)               \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        return Reply(SdcFunc(pCmd), 0U);                                      \
    }

/*
 * The byte counted form again, for commands that also answer with a return
 * structure. Periodic advertising with responses is where these appear: every
 * one of its commands echoes a handle back, and three of the four are variable
 * length, a combination that does not occur anywhere else in this table.
 */
#define HCI_SDC_CMD_VBR(Name, SdcFunc, SdcType, SdcReturn, ArrayField,        \
                        CountField)                                           \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t ParamLen,                               \
                               uint8_t *pReturn,                              \
                               size_t ReturnCapacity)                         \
    {                                                                         \
        SdcReturn result;                                                     \
        if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))                \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);   \
        }                                                                     \
        if (ParamLen < offsetof(SdcType, ArrayField))                         \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);     \
        }                                                                     \
        const SdcType *pCmd = reinterpret_cast<const SdcType *>(pParams);     \
        if (ParamLen - offsetof(SdcType, ArrayField) !=                       \
            (size_t)pCmd->CountField)                                         \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);     \
        }                                                                     \
        uint8_t status = SdcFunc(pCmd, &result);                              \
        if (status != HCI_STATUS_SUCCESS)                                     \
        {                                                                     \
            return HciSdcComplete(status, 0U);                                \
        }                                                                     \
        memcpy(pReturn, &result, sizeof(result));                             \
        return HciSdcComplete(status, sizeof(result));                        \
    }

/*
 * The element counted form with a return structure. Isochronous channels are
 * where this first occurs: LE Set CIG Parameters takes one array element per
 * stream and answers with the connection handle of each, so neither the
 * byte counted form nor the plain element counted one fits.
 *
 * The return structures for Set CIG Parameters and Set CIG Parameters Test end
 * in flexible arrays. sizeof(SdcReturn) is therefore only the CIG_ID and
 * CIS_Count prefix. SDC writes one connection-handle element per CIS, so the
 * dispatcher return buffer itself must be the SDC output object; a fixed local
 * object is too small as soon as CIS_Count is non-zero.
 */
#define HCI_SDC_CMD_VNR(Name, SdcFunc, SdcType, SdcReturn, ArrayField,        \
                        CountField)                                           \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t ParamLen,                               \
                               uint8_t *pReturn,                              \
                               size_t ReturnCapacity)                         \
    {                                                                         \
        if (ParamLen < offsetof(SdcType, ArrayField))                         \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);     \
        }                                                                     \
        const SdcType *pCmd = reinterpret_cast<const SdcType *>(pParams);     \
        const size_t needed = (size_t)pCmd->CountField *                      \
                              sizeof(pCmd->ArrayField[0]);                    \
        if (ParamLen - offsetof(SdcType, ArrayField) != needed)               \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);     \
        }                                                                     \
        const size_t returnLen = offsetof(SdcReturn, ArrayField) +            \
            ((size_t)pCmd->CountField *                                       \
             sizeof(((SdcReturn *)0)->ArrayField[0]));                        \
        if (!HciSdcReturnFits(returnLen, ReturnCapacity))                     \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);   \
        }                                                                     \
        memset(pReturn, 0, returnLen);                                        \
        SdcReturn *pResult = reinterpret_cast<SdcReturn *>(pReturn);          \
        uint8_t status = SdcFunc(pCmd, pResult);                              \
        if (status != HCI_STATUS_SUCCESS)                                     \
        {                                                                     \
            return HciSdcComplete(status, 0U);                                \
        }                                                                     \
        return HciSdcComplete(status, returnLen);                             \
    }

/*
 * The count is the number of bits set in a PHY bitmap. Vol 4 Part E 7.8.64
 * and 7.8.66 give one array element per PHY named in the bitmap. A reserved
 * bit therefore raises the required length and the packet is rejected, which
 * is the direction to fail in.
 */
#define HCI_SDC_CMD_VP(Name, SdcFunc, SdcType, ArrayField, PhyField, Reply)   \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t ParamLen,                               \
                               uint8_t *,                                     \
                               size_t)                                        \
    {                                                                         \
        if (ParamLen < offsetof(SdcType, ArrayField))                         \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        const SdcType *pCmd = reinterpret_cast<const SdcType *>(pParams);     \
        const size_t needed = HciSdcPhyCount(pCmd->PhyField) *                \
                              sizeof(pCmd->ArrayField[0]);                    \
        if (ParamLen - offsetof(SdcType, ArrayField) != needed)               \
        {                                                                     \
            return Reply(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);              \
        }                                                                     \
        return Reply(SdcFunc(pCmd), 0U);                                      \
    }

/*
 * The address SDC reports here comes from FICR->DEVICEADDR with the two top
 * bits set, which is the same value IOsonata nrf_get_mac_address() produces,
 * so a board answers with the same identity whatever firmware it runs. BlueZ
 * and Zephyr ask for this before falling back to an address of their own.
 *
 * The return is one count byte followed by 22 bytes per address, so its length
 * depends on the answer rather than the opcode. Two things follow from that.
 *
 * The table row declares 1, which is what an error is padded out to and reads
 * as no addresses, and the handler declares the real length on success.
 *
 * And the call takes no capacity argument, so the buffer has to be able to
 * hold the answer before it is made. Requiring room for the largest return a
 * Command Complete can carry means anything SDC can legally encode fits. That
 * is a bound on the encoding, not a promise from SDC, which documents no
 * maximum for the count.
 */
static HciCmdResult_t HciSdcCmdVsReadStaticAddresses(void *,
                                                     const uint8_t *,
                                                     size_t,
                                                     uint8_t *pReturn,
                                                     size_t ReturnCapacity)
{
    if (!HciSdcReturnFits(HCI_CMD_MAX_RETURN_LEN, ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t *pResult =
        reinterpret_cast<sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t *>(
            pReturn);

    uint8_t status = sdc_hci_cmd_vs_zephyr_read_static_addresses(pResult);
    if (status != HCI_STATUS_SUCCESS)
    {
        return HciSdcComplete(status, 0U);
    }

    const size_t length =
        sizeof(*pResult) +
        (size_t)pResult->num_addresses *
            sizeof(sdc_hci_vs_zephyr_static_address_t);

    if (length > HCI_CMD_MAX_RETURN_LEN)
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    return HciSdcComplete(status, length);
}


/*
 * The vendor equivalent of Read Local Supported Commands, and it needs the
 * same treatment for the same reason.
 *
 * SDC answers with what SDC implements. This layer dispatches a subset of
 * that, so passing the answer through would name commands the table has no row
 * for, and a host that reads the bitmap and then sends one gets Unknown HCI
 * Command back. The standard bitmap is built up bit by bit from what the table
 * declares and is checked against it in the tests; this one starts from SDC and
 * is masked down to the same set, which reaches the same place from the other
 * direction.
 *
 * Masked rather than rebuilt, because a bit means the command and the feature
 * behind it are both there. SDC clearing one is information this layer does not
 * have. So a bit survives only if SDC set it and there is a row to reach.
 */
static HciCmdResult_t HciSdcCmdVsZephyrReadSupportedCommands(void *,
                                                             const uint8_t *,
                                                             size_t,
                                                             uint8_t *pReturn,
                                                             size_t ReturnCapacity)
{
    sdc_hci_cmd_vs_zephyr_read_supported_commands_return_t result;

    if (!HciSdcReturnFits(sizeof(result.raw), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_vs_zephyr_read_supported_commands(&result);
    if (status != HCI_STATUS_SUCCESS)
    {
        return HciSdcComplete(status, 0U);
    }

    /*
     * Everything the table does not carry. Named one by one rather than
     * cleared wholesale, so a future row is a compile error here if the name
     * is removed and a visible omission if it is not.
     */
    result.params.read_supported_features = 0U;
    result.params.set_event_mask = 0U;
    result.params.reset = 0U;
    result.params.set_trace_enable = 0U;
    result.params.read_build_info = 0U;
    result.params.read_host_stack_commands = 0U;
    result.params.set_scan_request_reports = 0U;


    memcpy(pReturn, result.raw, sizeof(result.raw));
    return HciSdcComplete(status, sizeof(result.raw));
}




/*
 * The odd one. Vol 4 Part E 7.8.118 answers this with a Command Status and
 * then, once the controller has the remote power, an LE Transmit Power
 * Reporting event carrying reason 0x02. It is the only command in this group
 * whose answer arrives twice, and the reason it takes no return structure:
 * there is nothing to put in a Command Complete that has not been asked for
 * over the air yet.
 */



/*
 * The three below answer with a Command Status and finish in an LE meta event
 * later, because each of them has to talk to the peer before it knows
 * anything: Request Peer SCA in LE Request Peer SCA Complete, Subrate Request
 * in LE Subrate Change, Read All Remote Features in LE Read All Remote
 * Features Complete. Vol 4 Part E 7.8.108, 7.8.124 and 7.8.150.
 */





/* Command Status, then a VS Connection Update Complete event. */

/* Periodic advertising, the transmitting half. */
/*
 * The same shape as LE Set Extended Advertising Data, a byte counted trailing
 * array, so it gets the same check. Vol 4 Part E 7.8.62.
 */
HCI_SDC_CMD_VB(HciSdcCmdLeSetPeriodicAdvData,
               sdc_hci_cmd_le_set_periodic_adv_data,
               sdc_hci_cmd_le_set_periodic_adv_data_t, adv_data,
               adv_data_length, HciSdcComplete)

/* Following a train. */
/*
 * Command Status, then LE Periodic Advertising Sync Established once the
 * controller has actually found the train, or Sync Lost if it never does.
 * Vol 4 Part E 7.8.67. Nothing useful exists to put in a Command Complete at
 * the moment the command is accepted, which is why it has no return type.
 */

/* Handing a sync to a peer over a connection. */


/*
 * The one command in this table whose trailing array is not an array. Vol 4
 * Part E 7.8.125 gives Num_Subevents entries, each of them four octets
 * followed by Subevent_Data_Length more, so the entries are different sizes
 * and the count cannot be multiplied by anything. The SDC type admits this by
 * declaring the whole tail as uint8_t array_params[].
 *
 * So it is walked. Each step needs four octets of header before it can read
 * the length that says how far the next one starts, and the total has to land
 * exactly on the end of the packet. Anything else is a host that disagrees
 * with itself, and handing it to SDC means SDC walks the same entries with
 * whatever the previous packet left after them.
 */
static HciCmdResult_t HciSdcCmdLeSetPeriodicAdvSubeventData(void *,
                                                            const uint8_t *pParams,
                                                            size_t ParamLen,
                                                            uint8_t *pReturn,
                                                            size_t ReturnCapacity)
{
    sdc_hci_cmd_le_set_periodic_adv_subevent_data_return_t result;
    if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    const size_t head =
        offsetof(sdc_hci_cmd_le_set_periodic_adv_subevent_data_t, array_params);
    if (ParamLen < head)
    {
        return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
    }

    const sdc_hci_cmd_le_set_periodic_adv_subevent_data_t *pCmd =
        reinterpret_cast<
            const sdc_hci_cmd_le_set_periodic_adv_subevent_data_t *>(pParams);

    /*
     * Subevent, Response_Slot_Start, Response_Slot_Count, Subevent_Data_Length,
     * then the data. Vol 4 Part E 7.8.125.
     */
    const size_t entryHead = 4U;
    size_t offset = 0U;
    const size_t available = ParamLen - head;

    for (size_t i = 0U; i < (size_t)pCmd->num_subevents_with_data; i++)
    {
        if (available - offset < entryHead)
        {
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
        }

        const size_t dataLen = pCmd->array_params[offset + 3U];
        if (available - offset - entryHead < dataLen)
        {
            return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
        }

        offset += entryHead + dataLen;
    }

    if (offset != available)
    {
        return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
    }

    uint8_t status = sdc_hci_cmd_le_set_periodic_adv_subevent_data(pCmd,
                                                                   &result);
    if (status != HCI_STATUS_SUCCESS)
    {
        return HciSdcComplete(status, 0U);
    }

    memcpy(pReturn, &result, sizeof(result));
    return HciSdcComplete(status, sizeof(result));
}

/* Byte counted, and answers with the sync handle. 7.8.126 and 7.8.127. */
HCI_SDC_CMD_VBR(HciSdcCmdLeSetPeriodicAdvResponseData,
                sdc_hci_cmd_le_set_periodic_adv_response_data,
                sdc_hci_cmd_le_set_periodic_adv_response_data_t,
                sdc_hci_cmd_le_set_periodic_adv_response_data_return_t,
                response_data, response_data_length)
/*
 * The subevents to follow, one octet each, so the count is also the byte
 * count and the same macro fits.
 */
HCI_SDC_CMD_VBR(HciSdcCmdLeSetPeriodicSyncSubevent,
                sdc_hci_cmd_le_set_periodic_sync_subevent,
                sdc_hci_cmd_le_set_periodic_sync_subevent_t,
                sdc_hci_cmd_le_set_periodic_sync_subevent_return_t,
                subevents, num_subevents_to_sync)

/* Controller and baseband. */

/*
 * Controller-to-host flow control (Core Vol 4 Part E 7.3.38-7.3.40) is
 * not exposed: the underlying SDC rejects Host Buffer Size.
 */

/* Link control. */

/* Connection management. */

/* Filter accept list. */

/*
 * Privacy and the resolving list.
 *
 * The controller resolves a peer's resolvable private address against the
 * identity resolving keys held here, so a bonded peer that reconnects under a
 * new address is recognised as itself. Without it the host sees a stranger
 * every time, which is most of what a device does in the field: phones
 * advertise and connect with resolvable addresses as a matter of course.
 *
 * The list is the controller's, not the host's, so every entry has to be put
 * there over HCI. Vol 4 Part E 7.8.38 to 7.8.45 and 7.8.77.
 */

/* Security. */

/*
 * SDC command availability depends on the linked library variant.
 * Only commands exported by the multirole archive are dispatched.
 */


/*
 * Direct test mode: v1 uses LE 1M PHY; v2 selects the PHY; v3 supports
 * constant tone extension; v4 adds transmit power control.
 * Antenna switching requires supported direction-finding hardware.
 * A zero-length switching pattern does not require antenna switching.
 */

/*
 * Switching_Pattern_Length counts antenna identifiers and each one is a byte,
 * so the byte count form of the check applies unchanged, the same as the
 * advertising data commands.
 */
HCI_SDC_CMD_VB(HciSdcCmdLeReceiverTestV3, sdc_hci_cmd_le_receiver_test_v3,
               sdc_hci_cmd_le_receiver_test_v3_t, antenna_ids,
               switching_pattern_length, HciSdcComplete)
HCI_SDC_CMD_VB(HciSdcCmdLeTransmitterTestV3, sdc_hci_cmd_le_transmitter_test_v3,
               sdc_hci_cmd_le_transmitter_test_v3_t, antenna_ids,
               switching_pattern_length, HciSdcComplete)

/*
 * Transmitter Test v4 places TX_Power_Level after the antenna identifiers
 * (Core Vol 4 Part E 7.8.128). Validate the fixed header, switching pattern,
 * and trailing power parameter before passing the packet to SDC.
 */
static HciCmdResult_t HciSdcCmdLeTransmitterTestV4(void *,
                                                   const uint8_t *pParams,
                                                   size_t ParamLen,
                                                   uint8_t *,
                                                   size_t)
{
    const size_t head =
        offsetof(sdc_hci_cmd_le_transmitter_test_v4_t,
                 antenna_ids_and_remaining_parameters);

    if (ParamLen < head)
    {
        return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
    }

    const sdc_hci_cmd_le_transmitter_test_v4_t *pCmd =
        reinterpret_cast<const sdc_hci_cmd_le_transmitter_test_v4_t *>(pParams);

    /* The antenna identifiers, then one byte of transmit power. */
    if (ParamLen - head != (size_t)pCmd->switching_pattern_length + 1U)
    {
        return HciSdcComplete(HCI_STATUS_INVALID_HCI_PARAMETERS, 0U);
    }

    return HciSdcComplete(sdc_hci_cmd_le_transmitter_test_v4(pCmd), 0U);
}


/*
 * The standalone VS transmitter carrier test command was deprecated and dropped
 * from the SoftDevice Controller library. It now lives under the unified VS DTM
 * command with the transmitter carrier test sub-opcode. The host still sends the
 * legacy two byte parameters (tx_channel, tx_power_level); map them onto the new
 * command so the wire format the host uses is unchanged.
 */
static HciCmdResult_t HciSdcCmdVsTransmitterCarrierTest(void *,
                                                        const uint8_t *pParams,
                                                        size_t,
                                                        uint8_t *,
                                                        size_t)
{
    sdc_hci_cmd_vs_transmitter_carrier_test_t params;
    memcpy(&params, pParams, sizeof(params));

    sdc_hci_cmd_vs_dtm_command_t cmd;
    cmd.command_parameters.transmitter_carrier_test.header.sub_opcode =
        SDC_HCI_VS_DTM_COMMAND_OPCODE_TRANSMITTER_CARRIER_TEST;
    cmd.command_parameters.transmitter_carrier_test.tx_channel = params.tx_channel;
    cmd.command_parameters.transmitter_carrier_test.tx_power_level =
        params.tx_power_level;

    uint8_t retBuf[4];
    uint8_t retLen = 0;

    return HciSdcComplete(sdc_hci_cmd_vs_dtm_command(&cmd, retBuf, &retLen), 0U);
}

/* Read RSSI of the most recently received connection packet
 * (Core Vol 4 Part E 7.5.4).
 */

/*
 * LE Set Host Feature declares supported optional Host features
 * (Core Vol 4 Part E 7.8.115).
 */

/* Data length. */

/* PHY. */

/* Extended advertising. */
/*
 * The v2 form adds the primary and secondary PHY options that periodic
 * advertising with responses needs to place its subevents. Everything else is
 * the v1 parameter block, so the shape and the generator are the same.
 */
HCI_SDC_CMD_VB(HciSdcCmdLeSetExtAdvData, sdc_hci_cmd_le_set_ext_adv_data,
               sdc_hci_cmd_le_set_ext_adv_data_t, adv_data, adv_data_length,
               HciSdcComplete)
HCI_SDC_CMD_VB(HciSdcCmdLeSetExtScanRsp,
               sdc_hci_cmd_le_set_ext_scan_response_data,
               sdc_hci_cmd_le_set_ext_scan_response_data_t,
               scan_response_data, scan_response_data_length, HciSdcComplete)
HCI_SDC_CMD_VN(HciSdcCmdLeSetExtAdvEnable, sdc_hci_cmd_le_set_ext_adv_enable,
               sdc_hci_cmd_le_set_ext_adv_enable_t, array_params, num_sets,
               HciSdcComplete)

/* Extended scanning and initiating. */
HCI_SDC_CMD_VP(HciSdcCmdLeSetExtScanParams, sdc_hci_cmd_le_set_ext_scan_params,
               sdc_hci_cmd_le_set_ext_scan_params_t, array_params,
               scanning_phys, HciSdcComplete)
HCI_SDC_CMD_VP(HciSdcCmdLeExtCreateConn, sdc_hci_cmd_le_ext_create_conn,
               sdc_hci_cmd_le_ext_create_conn_t, array_params,
               initiating_phys, HciSdcStatus)

/*
 * Vol 4 Part E 7.8.66. The v2 form takes an advertising handle and a subevent
 * ahead of the v1 block, which is how a device synchronised to a periodic
 * advertising train with responses initiates the connection. Without it the
 * whole periodic advertising with responses path stops one command short of a
 * link, with the pool memory for it already reserved.
 *
 * Same array shape as v1: one element per bit set in initiating_phys.
 */
HCI_SDC_CMD_VP(HciSdcCmdLeExtCreateConnV2, sdc_hci_cmd_le_ext_create_conn_v2,
               sdc_hci_cmd_le_ext_create_conn_v2_t, array_params,
               initiating_phys, HciSdcStatus)

/*
 * Table entry helpers. Each names the response kind and the return parameter
 * length that a successful call produces, so the dispatcher can shape an error
 * the same way. The suffix follows the handler macros: C is Command Complete
 * with no return parameters, CR is Command Complete carrying a return
 * structure, S is Command Status.
 */
#define HCI_SDC_ENTRY_C(Opcode, ParamLen, Name)                               \
    {Opcode, (uint16_t)(ParamLen), 0U, HCI_CMD_RESPONSE_COMPLETE, Name}

#define HCI_SDC_ENTRY_CR(Opcode, ParamLen, Name, SdcReturn)                   \
    {Opcode, (uint16_t)(ParamLen), (uint16_t)sizeof(SdcReturn),               \
     HCI_CMD_RESPONSE_COMPLETE, Name}

#define HCI_SDC_ENTRY_S(Opcode, ParamLen, Name)                               \
    {Opcode, (uint16_t)(ParamLen), 0U, HCI_CMD_RESPONSE_STATUS, Name}

/*
 * Connected and broadcast isochronous command support depends on the
 * underlying controller capabilities. On nRF52, sdk-nrfxlib documents
 * ISO encryption support for nRF52820 and nRF52833, not nRF52840.
 * ISO encryption capability must not be inferred from command dispatch.
 *
 * LE Read Buffer Size v2 reports ACL and ISO buffer capacities; v1 provides
 * ACL capacities only. The v2 ACL count must update the same flow-control
 * limit used by the v1 command.
 */
static HciCmdResult_t HciSdcCmdLeReadBufferSizeV2(void *pContext,
                                                  const uint8_t *,
                                                  size_t,
                                                  uint8_t *pReturn,
                                                  size_t ReturnCapacity)
{
    sdc_hci_cmd_le_read_buffer_size_v2_return_t result;
    if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))
    {
        return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);
    }

    uint8_t status = sdc_hci_cmd_le_read_buffer_size_v2(&result);
    if (status == HCI_STATUS_SUCCESS)
    {
        /*
         * V2 reports the same ACL packet count as V1, followed by the ISO
         * buffer fields. A host that uses V2 is entitled to the same ACL flow
         * control enforcement as one that used V1.
         */
        HciCounters_t *pCounters = static_cast<HciCounters_t *>(pContext);
        if (pCounters != NULL)
        {
            HciSdcSetAclLimit(pCounters->pSdc,
                              result.total_num_le_acl_data_packets);
        }

        memcpy(pReturn, &result, sizeof(result));
        return HciSdcComplete(status, sizeof(result));
    }

    return HciSdcComplete(status, 0U);
}


/* One array element per stream, and a handle returned for each. */
HCI_SDC_CMD_VNR(HciSdcCmdLeSetCigParams,
                sdc_hci_cmd_le_set_cig_params,
                sdc_hci_cmd_le_set_cig_params_t,
                sdc_hci_cmd_le_set_cig_params_return_t,
                array_params, cis_count)

HCI_SDC_CMD_VNR(HciSdcCmdLeSetCigParamsTest,
                sdc_hci_cmd_le_set_cig_params_test,
                sdc_hci_cmd_le_set_cig_params_test_t,
                sdc_hci_cmd_le_set_cig_params_test_return_t,
                array_params, cis_count)

/*
 * Answers a Command Status and then one LE CIS Established event per stream,
 * so there is no return structure to carry.
 */
HCI_SDC_CMD_VN(HciSdcCmdLeCreateCis,
               sdc_hci_cmd_le_create_cis,
               sdc_hci_cmd_le_create_cis_t,
               array_params, cis_count, HciSdcStatus)







/* One array element per broadcast stream the host wants from the group. */
HCI_SDC_CMD_VN(HciSdcCmdLeBigCreateSync,
               sdc_hci_cmd_le_big_create_sync,
               sdc_hci_cmd_le_big_create_sync_t,
               array_params, num_bis, HciSdcStatus)


/*
 * Byte counted, not element counted: the tail is a codec configuration whose
 * length is declared in octets and whose contents this layer has no opinion
 * about.
 */
HCI_SDC_CMD_VBR(HciSdcCmdLeSetupIsoDataPath,
                sdc_hci_cmd_le_setup_iso_data_path,
                sdc_hci_cmd_le_setup_iso_data_path_t,
                sdc_hci_cmd_le_setup_iso_data_path_return_t,
                codec_config, codec_config_length)











static const HciCmdEntry_t s_HciSdcCommands[] = {
    /* Controller and baseband. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_CB_SET_EVENT_MASK, 8U,
                    HciSdcCmdSetEventMask),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_CB_RESET, 0U, HciSdcCmdReset),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_CB_SET_EVENT_MASK_PAGE_2,
                    sizeof(sdc_hci_cmd_cb_set_event_mask_page_2_t),
                    HciSdcCmdSetEventMaskPage2),
    /*
     * Controller to host flow control is not here: not Set Controller To Host
     * Flow Control, not Host Buffer Size, not Host Number Of Completed
     * Packets.
     *
     * The controller refuses Host Buffer Size. Measured on hardware against a
     * Zephyr host, the answer is status 0x11, Unsupported Feature or Parameter
     * Value, from sdc_hci_cmd_cb_host_buffer_size itself.
     *
     * Not because the function is missing. NCS calls the same one, through
     * sdc_hci_cmd_cb_host_buffer_size_wrapper in
     * subsys/bluetooth/controller/hci_internal_wrappers.c. What NCS also does
     * is condition the whole feature, the bitmap bits and the dispatch rows
     * alike,
     * behind CONFIG_BT_HCI_ACL_FLOW_CONTROL, and the controller sample for
     * this exact board does not set it: samples/bluetooth/hci_lpuart with
     * boards/thingy91_nrf52840.conf. So the reference firmware for the
     * Thingy:91 neither advertises these three nor ever calls them, and never
     * meets the 0x11 that this one did.
     *
     * Why the controller answers 0x11 in a configuration NCS never exercises
     * is not established here. What is established is that advertising it and
     * then failing is worse than not advertising it.
     *
     * Dispatching it anyway was worse than not having it. A host reads the
     * supported command bitmap first and only sends what it is told is there,
     * so the bit and the row together were a promise the controller broke. A
     * Zephyr host takes the refusal as a failure of bt_enable rather than of
     * one command, and an application that treats a failed Bluetooth start as
     * fatal reboots with nothing in its log to say why. That is exactly what
     * the Thingy:91 did: Reset, Read Local Supported Features, Read Local
     * Version, Read Local Supported Commands, then Host Buffer Size answered
     * 0x11, and a restart, every ten seconds.
     *
     * With the bit clear the same host logs that the controller does not
     * support it and goes on, which is the behaviour a controller without
     * the feature is supposed to produce. The bitmap below drops the three
     * bits to match, since a row and a bit have to agree in both directions.
     */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_CB_READ_AUTHENTICATED_PAYLOAD_TIMEOUT, sdc_hci_cmd_cb_read_authenticated_payload_timeout_t, sizeof(sdc_hci_cmd_cb_read_authenticated_payload_timeout_t), sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t, sdc_hci_cmd_cb_read_authenticated_payload_timeout),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_CB_WRITE_AUTHENTICATED_PAYLOAD_TIMEOUT, sdc_hci_cmd_cb_write_authenticated_payload_timeout_t, sizeof(sdc_hci_cmd_cb_write_authenticated_payload_timeout_t), sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t, sdc_hci_cmd_cb_write_authenticated_payload_timeout),

    /* Informational parameters. */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_VERSION_INFORMATION, 0U,
                     HciSdcCmdReadLocalVersion,
                     sdc_hci_cmd_ip_read_local_version_information_return_t),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_COMMANDS, 0U,
                     HciSdcCmdReadSupportedCommands,
                     sdc_hci_cmd_ip_read_local_supported_commands_return_t),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_FEATURES, 0U,
                     HciSdcCmdReadLocalFeatures,
                     sdc_hci_cmd_ip_read_local_supported_features_return_t),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_IP_READ_BD_ADDR, 0U,
                     HciSdcCmdReadBdAddr,
                     sdc_hci_cmd_ip_read_bd_addr_return_t),

    /* Status parameters. */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_SP_READ_RSSI, sdc_hci_cmd_sp_read_rssi_t, sizeof(sdc_hci_cmd_sp_read_rssi_t), sdc_hci_cmd_sp_read_rssi_return_t, sdc_hci_cmd_sp_read_rssi),

    /* LE basics. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EVENT_MASK, 8U,
                    HciSdcCmdLeSetEventMask),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_BUFFER_SIZE, 0U,
                     HciSdcCmdLeReadBufferSize,
                     sdc_hci_cmd_le_read_buffer_size_return_t),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_LOCAL_SUPPORTED_FEATURES, 0U,
                     HciSdcCmdLeReadLocalFeatures,
                     sdc_hci_cmd_le_read_local_supported_features_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_RANDOM_ADDRESS, 6U,
                    HciSdcCmdLeSetRandomAddress),

    /* Legacy advertising and scanning. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS, 15U,
                    HciSdcCmdLeSetAdvParams),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_ADV_PHYSICAL_CHANNEL_TX_POWER,
                     0U, HciSdcCmdLeReadAdvTxPower,
                     sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_ADV_DATA, 32U,
                    HciSdcCmdLeSetAdvData),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_RESPONSE_DATA, 32U,
                    HciSdcCmdLeSetScanResponse),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_ADV_ENABLE, 1U,
                    HciSdcCmdLeSetAdvEnable),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_PARAMS, 7U,
                    HciSdcCmdLeSetScanParams),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_ENABLE, 2U,
                    HciSdcCmdLeSetScanEnable),

    /* Link control. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LC_DISCONNECT, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_lc_disconnect_t, sizeof(sdc_hci_cmd_lc_disconnect_t), sdc_hci_cmd_lc_disconnect),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LC_READ_REMOTE_VERSION_INFORMATION, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_lc_read_remote_version_information_t, sizeof(sdc_hci_cmd_lc_read_remote_version_information_t), sdc_hci_cmd_lc_read_remote_version_information),

    /* Connection management. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_create_conn_t, sizeof(sdc_hci_cmd_le_create_conn_t), sdc_hci_cmd_le_create_conn),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN_CANCEL, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_create_conn_cancel),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CONN_UPDATE, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_conn_update_t, sizeof(sdc_hci_cmd_le_conn_update_t), sdc_hci_cmd_le_conn_update),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_READ_CHANNEL_MAP, sdc_hci_cmd_le_read_channel_map_t, sizeof(sdc_hci_cmd_le_read_channel_map_t), sdc_hci_cmd_le_read_channel_map_return_t, sdc_hci_cmd_le_read_channel_map),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_READ_REMOTE_FEATURES, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_read_remote_features_t, sizeof(sdc_hci_cmd_le_read_remote_features_t), sdc_hci_cmd_le_read_remote_features),

    /* Filter accept list. */
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_FILTER_ACCEPT_LIST_SIZE, 0U, sdc_hci_cmd_le_read_filter_accept_list_size_return_t, sdc_hci_cmd_le_read_filter_accept_list_size),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CLEAR_FILTER_ACCEPT_LIST, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_clear_filter_accept_list),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_FILTER_ACCEPT_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_add_device_to_filter_accept_list_t, sizeof(sdc_hci_cmd_le_add_device_to_filter_accept_list_t), sdc_hci_cmd_le_add_device_to_filter_accept_list),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_FILTER_ACCEPT_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_remove_device_from_filter_accept_list_t, sizeof(sdc_hci_cmd_le_remove_device_from_filter_accept_list_t), sdc_hci_cmd_le_remove_device_from_filter_accept_list),

    /* Privacy and the resolving list. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_RESOLVING_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_add_device_to_resolving_list_t, sizeof(sdc_hci_cmd_le_add_device_to_resolving_list_t), sdc_hci_cmd_le_add_device_to_resolving_list),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_RESOLVING_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_remove_device_from_resolving_list_t, sizeof(sdc_hci_cmd_le_remove_device_from_resolving_list_t), sdc_hci_cmd_le_remove_device_from_resolving_list),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CLEAR_RESOLVING_LIST, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_clear_resolving_list),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_RESOLVING_LIST_SIZE, 0U, sdc_hci_cmd_le_read_resolving_list_size_return_t, sdc_hci_cmd_le_read_resolving_list_size),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_ADDRESS_RESOLUTION_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_address_resolution_enable_t, sizeof(sdc_hci_cmd_le_set_address_resolution_enable_t), sdc_hci_cmd_le_set_address_resolution_enable),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_RESOLVABLE_PRIVATE_ADDRESS_TIMEOUT, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_resolvable_private_address_timeout_t, sizeof(sdc_hci_cmd_le_set_resolvable_private_address_timeout_t), sdc_hci_cmd_le_set_resolvable_private_address_timeout),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_PRIVACY_MODE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_privacy_mode_t, sizeof(sdc_hci_cmd_le_set_privacy_mode_t), sdc_hci_cmd_le_set_privacy_mode),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_DATA_RELATED_ADDRESS_CHANGES, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_data_related_address_changes_t, sizeof(sdc_hci_cmd_le_set_data_related_address_changes_t), sdc_hci_cmd_le_set_data_related_address_changes),

    /* Security. */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ENCRYPT, sdc_hci_cmd_le_encrypt_t, sizeof(sdc_hci_cmd_le_encrypt_t), sdc_hci_cmd_le_encrypt_return_t, sdc_hci_cmd_le_encrypt),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_RAND, 0U, sdc_hci_cmd_le_rand_return_t, sdc_hci_cmd_le_rand),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_ENABLE_ENCRYPTION, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_enable_encryption_t, sizeof(sdc_hci_cmd_le_enable_encryption_t), sdc_hci_cmd_le_enable_encryption),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_REPLY, sdc_hci_cmd_le_long_term_key_request_reply_t, sizeof(sdc_hci_cmd_le_long_term_key_request_reply_t), sdc_hci_cmd_le_long_term_key_request_reply_return_t, sdc_hci_cmd_le_long_term_key_request_reply),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_NEGATIVE_REPLY, sdc_hci_cmd_le_long_term_key_request_negative_reply_t, sizeof(sdc_hci_cmd_le_long_term_key_request_negative_reply_t), sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t, sdc_hci_cmd_le_long_term_key_request_negative_reply),

    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_TRANSMIT_POWER, 0U, sdc_hci_cmd_le_read_transmit_power_return_t, sdc_hci_cmd_le_read_transmit_power),

    /* Direct test mode. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V1, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_receiver_test_v1_t, sizeof(sdc_hci_cmd_le_receiver_test_v1_t), sdc_hci_cmd_le_receiver_test_v1),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V1, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_transmitter_test_v1_t, sizeof(sdc_hci_cmd_le_transmitter_test_v1_t), sdc_hci_cmd_le_transmitter_test_v1),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V2, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_receiver_test_v2_t, sizeof(sdc_hci_cmd_le_receiver_test_v2_t), sdc_hci_cmd_le_receiver_test_v2),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V2, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_transmitter_test_v2_t, sizeof(sdc_hci_cmd_le_transmitter_test_v2_t), sdc_hci_cmd_le_transmitter_test_v2),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V3,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeReceiverTestV3),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V3,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeTransmitterTestV3),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V4,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeTransmitterTestV4),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_TEST_END, 0U, sdc_hci_cmd_le_test_end_return_t, sdc_hci_cmd_le_test_end),

    /* Data length. */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_DATA_LENGTH, sdc_hci_cmd_le_set_data_length_t, sizeof(sdc_hci_cmd_le_set_data_length_t), sdc_hci_cmd_le_set_data_length_return_t, sdc_hci_cmd_le_set_data_length),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_SUGGESTED_DEFAULT_DATA_LENGTH, 0U, sdc_hci_cmd_le_read_suggested_default_data_length_return_t, sdc_hci_cmd_le_read_suggested_default_data_length),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_WRITE_SUGGESTED_DEFAULT_DATA_LENGTH, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_write_suggested_default_data_length_t, sizeof(sdc_hci_cmd_le_write_suggested_default_data_length_t), sdc_hci_cmd_le_write_suggested_default_data_length),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_MAX_DATA_LENGTH, 0U, sdc_hci_cmd_le_read_max_data_length_return_t, sdc_hci_cmd_le_read_max_data_length),

    /* PHY. */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_READ_PHY, sdc_hci_cmd_le_read_phy_t, sizeof(sdc_hci_cmd_le_read_phy_t), sdc_hci_cmd_le_read_phy_return_t, sdc_hci_cmd_le_read_phy),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PHY, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_default_phy_t, sizeof(sdc_hci_cmd_le_set_default_phy_t), sdc_hci_cmd_le_set_default_phy),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_PHY, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_set_phy_t, sizeof(sdc_hci_cmd_le_set_phy_t), sdc_hci_cmd_le_set_phy),

    /* Extended advertising. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_adv_set_random_address_t, sizeof(sdc_hci_cmd_le_set_adv_set_random_address_t), sdc_hci_cmd_le_set_adv_set_random_address),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS, sdc_hci_cmd_le_set_ext_adv_params_t, sizeof(sdc_hci_cmd_le_set_ext_adv_params_t), sdc_hci_cmd_le_set_ext_adv_params_return_t, sdc_hci_cmd_le_set_ext_adv_params),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS_V2, sdc_hci_cmd_le_set_ext_adv_params_v2_t, sizeof(sdc_hci_cmd_le_set_ext_adv_params_v2_t), sdc_hci_cmd_le_set_ext_adv_params_v2_return_t, sdc_hci_cmd_le_set_ext_adv_params_v2),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_DATA,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtAdvData),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_RESPONSE_DATA,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtScanRsp),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_ENABLE,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtAdvEnable),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_MAX_ADV_DATA_LENGTH, 0U, sdc_hci_cmd_le_read_max_adv_data_length_return_t, sdc_hci_cmd_le_read_max_adv_data_length),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_NUMBER_OF_SUPPORTED_ADV_SETS, 0U, sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t, sdc_hci_cmd_le_read_number_of_supported_adv_sets),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_REMOVE_ADV_SET, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_remove_adv_set_t, sizeof(sdc_hci_cmd_le_remove_adv_set_t), sdc_hci_cmd_le_remove_adv_set),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CLEAR_ADV_SETS, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_clear_adv_sets),

    /* Extended scanning and initiating. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_PARAMS,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtScanParams),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_ext_scan_enable_t, sizeof(sdc_hci_cmd_le_set_ext_scan_enable_t), sdc_hci_cmd_le_set_ext_scan_enable),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeExtCreateConn),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN_V2,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeExtCreateConnV2),

    /* Host declared features. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_HOST_FEATURE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_host_feature_t, sizeof(sdc_hci_cmd_le_set_host_feature_t), sdc_hci_cmd_le_set_host_feature),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_HOST_FEATURE_V2, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_host_feature_v2_t, sizeof(sdc_hci_cmd_le_set_host_feature_v2_t), sdc_hci_cmd_le_set_host_feature_v2),

    /* Transmit power, and the path loss the two ends work out from it. */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_CB_READ_TRANSMIT_POWER_LEVEL, sdc_hci_cmd_cb_read_transmit_power_level_t, sizeof(sdc_hci_cmd_cb_read_transmit_power_level_t), sdc_hci_cmd_cb_read_transmit_power_level_return_t, sdc_hci_cmd_cb_read_transmit_power_level),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_RF_PATH_COMPENSATION, 0U, sdc_hci_cmd_le_read_rf_path_compensation_return_t, sdc_hci_cmd_le_read_rf_path_compensation),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_WRITE_RF_PATH_COMPENSATION, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_write_rf_path_compensation_t, sizeof(sdc_hci_cmd_le_write_rf_path_compensation_t), sdc_hci_cmd_le_write_rf_path_compensation),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ENHANCED_READ_TRANSMIT_POWER_LEVEL, sdc_hci_cmd_le_enhanced_read_transmit_power_level_t, sizeof(sdc_hci_cmd_le_enhanced_read_transmit_power_level_t), sdc_hci_cmd_le_enhanced_read_transmit_power_level_return_t, sdc_hci_cmd_le_enhanced_read_transmit_power_level),
    /* Command Status, then an LE Transmit Power Reporting event. 7.8.118. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_READ_REMOTE_TRANSMIT_POWER_LEVEL, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_read_remote_transmit_power_level_t, sizeof(sdc_hci_cmd_le_read_remote_transmit_power_level_t), sdc_hci_cmd_le_read_remote_transmit_power_level),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_PATH_LOSS_REPORTING_PARAMS, sdc_hci_cmd_le_set_path_loss_reporting_params_t, sizeof(sdc_hci_cmd_le_set_path_loss_reporting_params_t), sdc_hci_cmd_le_set_path_loss_reporting_params_return_t, sdc_hci_cmd_le_set_path_loss_reporting_params),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_PATH_LOSS_REPORTING_ENABLE, sdc_hci_cmd_le_set_path_loss_reporting_enable_t, sizeof(sdc_hci_cmd_le_set_path_loss_reporting_enable_t), sdc_hci_cmd_le_set_path_loss_reporting_enable_return_t, sdc_hci_cmd_le_set_path_loss_reporting_enable),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_TRANSMIT_POWER_REPORTING_ENABLE, sdc_hci_cmd_le_set_transmit_power_reporting_enable_t, sizeof(sdc_hci_cmd_le_set_transmit_power_reporting_enable_t), sdc_hci_cmd_le_set_transmit_power_reporting_enable_return_t, sdc_hci_cmd_le_set_transmit_power_reporting_enable),

    /* Which channels the host believes are usable. Five octets of bitmap. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_host_channel_classification_t, sizeof(sdc_hci_cmd_le_set_host_channel_classification_t), sdc_hci_cmd_le_set_host_channel_classification),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_REQUEST_PEER_SCA, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_request_peer_sca_t, sizeof(sdc_hci_cmd_le_request_peer_sca_t), sdc_hci_cmd_le_request_peer_sca),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_SUBRATE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_default_subrate_t, sizeof(sdc_hci_cmd_le_set_default_subrate_t), sdc_hci_cmd_le_set_default_subrate),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SUBRATE_REQUEST, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_subrate_request_t, sizeof(sdc_hci_cmd_le_subrate_request_t), sdc_hci_cmd_le_subrate_request),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_ALL_LOCAL_SUPPORTED_FEATURES, 0U, sdc_hci_cmd_le_read_all_local_supported_features_return_t, sdc_hci_cmd_le_read_all_local_supported_features),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_READ_ALL_REMOTE_FEATURES, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_read_all_remote_features_t, sizeof(sdc_hci_cmd_le_read_all_remote_features_t), sdc_hci_cmd_le_read_all_remote_features),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_SET_ADV_RANDOMNESS, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_set_adv_randomness_t, sizeof(sdc_hci_cmd_vs_set_adv_randomness_t), sdc_hci_cmd_vs_set_adv_randomness),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_LLPM_MODE_SET, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_llpm_mode_set_t, sizeof(sdc_hci_cmd_vs_llpm_mode_set_t), sdc_hci_cmd_vs_llpm_mode_set),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_CONN_UPDATE, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_vs_conn_update_t, sizeof(sdc_hci_cmd_vs_conn_update_t), sdc_hci_cmd_vs_conn_update),

    /* Periodic advertising, transmitting. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_periodic_adv_params_t, sizeof(sdc_hci_cmd_le_set_periodic_adv_params_t), sdc_hci_cmd_le_set_periodic_adv_params),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_DATA,
                    HCI_CMD_VARIABLE_PARAM_LEN,
                    HciSdcCmdLeSetPeriodicAdvData),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_periodic_adv_enable_t, sizeof(sdc_hci_cmd_le_set_periodic_adv_enable_t), sdc_hci_cmd_le_set_periodic_adv_enable),
    /* Following a train. Create Sync finishes in a Sync Established event. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_periodic_adv_create_sync_t, sizeof(sdc_hci_cmd_le_periodic_adv_create_sync_t), sdc_hci_cmd_le_periodic_adv_create_sync),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC_CANCEL, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_periodic_adv_create_sync_cancel),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_TERMINATE_SYNC, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_periodic_adv_terminate_sync_t, sizeof(sdc_hci_cmd_le_periodic_adv_terminate_sync_t), sdc_hci_cmd_le_periodic_adv_terminate_sync),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_PERIODIC_ADV_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_add_device_to_periodic_adv_list_t, sizeof(sdc_hci_cmd_le_add_device_to_periodic_adv_list_t), sdc_hci_cmd_le_add_device_to_periodic_adv_list),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_PERIODIC_ADV_LIST, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_remove_device_from_periodic_adv_list_t, sizeof(sdc_hci_cmd_le_remove_device_from_periodic_adv_list_t), sdc_hci_cmd_le_remove_device_from_periodic_adv_list),
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CLEAR_PERIODIC_ADV_LIST, HCI_CMD_RESPONSE_COMPLETE, 0U, sdc_hci_cmd_le_clear_periodic_adv_list),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_READ_PERIODIC_ADV_LIST_SIZE, 0U, sdc_hci_cmd_le_read_periodic_adv_list_size_return_t, sdc_hci_cmd_le_read_periodic_adv_list_size),
    /* Handing a sync to a peer over a connection. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RECEIVE_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_periodic_adv_receive_enable_t, sizeof(sdc_hci_cmd_le_set_periodic_adv_receive_enable_t), sdc_hci_cmd_le_set_periodic_adv_receive_enable),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_SYNC_TRANSFER, sdc_hci_cmd_le_periodic_adv_sync_transfer_t, sizeof(sdc_hci_cmd_le_periodic_adv_sync_transfer_t), sdc_hci_cmd_le_periodic_adv_sync_transfer_return_t, sdc_hci_cmd_le_periodic_adv_sync_transfer),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_SET_INFO_TRANSFER, sdc_hci_cmd_le_periodic_adv_set_info_transfer_t, sizeof(sdc_hci_cmd_le_periodic_adv_set_info_transfer_t), sdc_hci_cmd_le_periodic_adv_set_info_transfer_return_t, sdc_hci_cmd_le_periodic_adv_set_info_transfer),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SYNC_TRANSFER_PARAMS, sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_t, sizeof(sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_t), sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_return_t, sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PERIODIC_ADV_SYNC_TRANSFER_PARAMS, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params_t, sizeof(
            sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params_t), sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params),
    /*
     * Periodic advertising with responses, the advertiser. Subevent Data
     * declares the return length it produces on success, which is the handle,
     * and that is also what an error is padded out to.
     */
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS_V2, sdc_hci_cmd_le_set_periodic_adv_params_v2_t, sizeof(sdc_hci_cmd_le_set_periodic_adv_params_v2_t), sdc_hci_cmd_le_set_periodic_adv_params_v2_return_t, sdc_hci_cmd_le_set_periodic_adv_params_v2),
    {SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SUBEVENT_DATA,
     HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_periodic_adv_subevent_data_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetPeriodicAdvSubeventData},
    /* And the scanner, which answers in a slot and picks what to follow. */
    {SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RESPONSE_DATA,
     HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_periodic_adv_response_data_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetPeriodicAdvResponseData},
    {SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_SYNC_SUBEVENT,
     HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_periodic_sync_subevent_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetPeriodicSyncSubevent},

    /*
     * Vendor specific, and grouped with direct test mode rather than with the
     * vendor rows below because that is what it is for. LE Test End stops it,
     * the same as any other test mode.
     */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_VS_TRANSMITTER_CARRIER_TEST,
                    sizeof(sdc_hci_cmd_vs_transmitter_carrier_test_t),
                    HciSdcCmdVsTransmitterCarrierTest),

    /*
     * Vendor specific. The return type ends in a flexible array, so sizeof()
     * on it is the count byte alone, which is exactly the minimum this command
     * always returns and what an error is padded out to.
     */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_STATIC_ADDRESSES, 0U,
                     HciSdcCmdVsReadStaticAddresses,
                     sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t),
    /*
     * The rest of the Zephyr family. Read Supported Commands declares the full
     * 64 octet bitmap, which is what it always returns, masked or not.
     */
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_VERSION_INFO, 0U, sdc_hci_cmd_vs_zephyr_read_version_info_return_t, sdc_hci_cmd_vs_zephyr_read_version_info),
    {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_SUPPORTED_COMMANDS, 0U,
     (uint16_t)sizeof(
         sdc_hci_cmd_vs_zephyr_read_supported_commands_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdVsZephyrReadSupportedCommands},
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_WRITE_BD_ADDR, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_zephyr_write_bd_addr_t, sizeof(sdc_hci_cmd_vs_zephyr_write_bd_addr_t), sdc_hci_cmd_vs_zephyr_write_bd_addr),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_CHIP_TEMP, 0U, sdc_hci_cmd_vs_zephyr_read_chip_temp_return_t, sdc_hci_cmd_vs_zephyr_read_chip_temp),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_WRITE_TX_POWER, sdc_hci_cmd_vs_zephyr_write_tx_power_t, sizeof(sdc_hci_cmd_vs_zephyr_write_tx_power_t), sdc_hci_cmd_vs_zephyr_write_tx_power_return_t, sdc_hci_cmd_vs_zephyr_write_tx_power),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_TX_POWER, sdc_hci_cmd_vs_zephyr_read_tx_power_t, sizeof(sdc_hci_cmd_vs_zephyr_read_tx_power_t), sdc_hci_cmd_vs_zephyr_read_tx_power_return_t, sdc_hci_cmd_vs_zephyr_read_tx_power),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_KEY_HIERARCHY_ROOTS, 0U, sdc_hci_cmd_vs_zephyr_read_key_hierarchy_roots_return_t, sdc_hci_cmd_vs_zephyr_read_key_hierarchy_roots),
    /* Quality of service. What the radio saw, rather than what the link did. */
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_QOS_CONN_EVENT_REPORT_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_qos_conn_event_report_enable_t, sizeof(sdc_hci_cmd_vs_qos_conn_event_report_enable_t), sdc_hci_cmd_vs_qos_conn_event_report_enable),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_QOS_CHANNEL_SURVEY_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_qos_channel_survey_enable_t, sizeof(sdc_hci_cmd_vs_qos_channel_survey_enable_t), sdc_hci_cmd_vs_qos_channel_survey_enable),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_VS_READ_AVERAGE_RSSI, sdc_hci_cmd_vs_read_average_rssi_t, sizeof(sdc_hci_cmd_vs_read_average_rssi_t), sdc_hci_cmd_vs_read_average_rssi_return_t, sdc_hci_cmd_vs_read_average_rssi),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_VS_GET_NEXT_CONN_EVENT_COUNTER, sdc_hci_cmd_vs_get_next_conn_event_counter_t, sizeof(sdc_hci_cmd_vs_get_next_conn_event_counter_t), sdc_hci_cmd_vs_get_next_conn_event_counter_return_t, sdc_hci_cmd_vs_get_next_conn_event_counter),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_CONN_ANCHOR_POINT_UPDATE_EVENT_REPORT_ENABLE, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t, sizeof(
            sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t), sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable),
    /*
     * Answered by the routing layer rather than by SDC, so it reports what
     * this firmware refused rather than what the radio did. The length is a
     * constant from the header instead of a sizeof(), because the wire format
     * is written out field by field and owes nothing to a struct layout.
     */
    /*
     * Isochronous channels. The part specific note about encryption is above
     * the handlers. The four test commands need no codec and are what an
     * instrument uses to measure an isochronous link.
     */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_BUFFER_SIZE_V2, 0U,
                     HciSdcCmdLeReadBufferSizeV2,
                     sdc_hci_cmd_le_read_buffer_size_v2_return_t),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_READ_ISO_TX_SYNC, sdc_hci_cmd_le_read_iso_tx_sync_t, sizeof(sdc_hci_cmd_le_read_iso_tx_sync_t), sdc_hci_cmd_le_read_iso_tx_sync_return_t, sdc_hci_cmd_le_read_iso_tx_sync),
    {SDC_HCI_OPCODE_CMD_LE_SET_CIG_PARAMS, HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_cig_params_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetCigParams},
    {SDC_HCI_OPCODE_CMD_LE_SET_CIG_PARAMS_TEST, HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_cig_params_test_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetCigParamsTest},
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_CREATE_CIS,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeCreateCis),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_REMOVE_CIG, sdc_hci_cmd_le_remove_cig_t, sizeof(sdc_hci_cmd_le_remove_cig_t), sdc_hci_cmd_le_remove_cig_return_t, sdc_hci_cmd_le_remove_cig),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_ACCEPT_CIS_REQUEST, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_accept_cis_request_t, sizeof(sdc_hci_cmd_le_accept_cis_request_t), sdc_hci_cmd_le_accept_cis_request),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_REJECT_CIS_REQUEST, sdc_hci_cmd_le_reject_cis_request_t, sizeof(sdc_hci_cmd_le_reject_cis_request_t), sdc_hci_cmd_le_reject_cis_request_return_t, sdc_hci_cmd_le_reject_cis_request),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CREATE_BIG, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_create_big_t, sizeof(sdc_hci_cmd_le_create_big_t), sdc_hci_cmd_le_create_big),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CREATE_BIG_TEST, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_create_big_test_t, sizeof(sdc_hci_cmd_le_create_big_test_t), sdc_hci_cmd_le_create_big_test),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_TERMINATE_BIG, HCI_CMD_RESPONSE_STATUS, sdc_hci_cmd_le_terminate_big_t, sizeof(sdc_hci_cmd_le_terminate_big_t), sdc_hci_cmd_le_terminate_big),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_BIG_CREATE_SYNC,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeBigCreateSync),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_BIG_TERMINATE_SYNC, sdc_hci_cmd_le_big_terminate_sync_t, sizeof(sdc_hci_cmd_le_big_terminate_sync_t), sdc_hci_cmd_le_big_terminate_sync_return_t, sdc_hci_cmd_le_big_terminate_sync),
    {SDC_HCI_OPCODE_CMD_LE_SETUP_ISO_DATA_PATH, HCI_CMD_VARIABLE_PARAM_LEN,
     (uint16_t)sizeof(sdc_hci_cmd_le_setup_iso_data_path_return_t),
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeSetupIsoDataPath},
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_REMOVE_ISO_DATA_PATH, sdc_hci_cmd_le_remove_iso_data_path_t, sizeof(sdc_hci_cmd_le_remove_iso_data_path_t), sdc_hci_cmd_le_remove_iso_data_path_return_t, sdc_hci_cmd_le_remove_iso_data_path),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ISO_TRANSMIT_TEST, sdc_hci_cmd_le_iso_transmit_test_t, sizeof(sdc_hci_cmd_le_iso_transmit_test_t), sdc_hci_cmd_le_iso_transmit_test_return_t, sdc_hci_cmd_le_iso_transmit_test),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ISO_RECEIVE_TEST, sdc_hci_cmd_le_iso_receive_test_t, sizeof(sdc_hci_cmd_le_iso_receive_test_t), sdc_hci_cmd_le_iso_receive_test_return_t, sdc_hci_cmd_le_iso_receive_test),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ISO_READ_TEST_COUNTERS, sdc_hci_cmd_le_iso_read_test_counters_t, sizeof(sdc_hci_cmd_le_iso_read_test_counters_t), sdc_hci_cmd_le_iso_read_test_counters_return_t, sdc_hci_cmd_le_iso_read_test_counters),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_ISO_TEST_END, sdc_hci_cmd_le_iso_test_end_t, sizeof(sdc_hci_cmd_le_iso_test_end_t), sdc_hci_cmd_le_iso_test_end_return_t, sdc_hci_cmd_le_iso_test_end),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_READ_ISO_LINK_QUALITY, sdc_hci_cmd_le_read_iso_link_quality_t, sizeof(sdc_hci_cmd_le_read_iso_link_quality_t), sdc_hci_cmd_le_read_iso_link_quality_return_t, sdc_hci_cmd_le_read_iso_link_quality),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_VS_ISO_READ_TX_TIMESTAMP, sdc_hci_cmd_vs_iso_read_tx_timestamp_t, sizeof(sdc_hci_cmd_vs_iso_read_tx_timestamp_t), sdc_hci_cmd_vs_iso_read_tx_timestamp_return_t, sdc_hci_cmd_vs_iso_read_tx_timestamp),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_BIG_RESERVED_TIME_SET, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_big_reserved_time_set_t, sizeof(sdc_hci_cmd_vs_big_reserved_time_set_t), sdc_hci_cmd_vs_big_reserved_time_set),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_CIG_RESERVED_TIME_SET, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_cig_reserved_time_set_t, sizeof(sdc_hci_cmd_vs_cig_reserved_time_set_t), sdc_hci_cmd_vs_cig_reserved_time_set),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_VS_CIS_SUBEVENT_LENGTH_SET, HCI_CMD_RESPONSE_COMPLETE, sdc_hci_cmd_vs_cis_subevent_length_set_t, sizeof(sdc_hci_cmd_vs_cis_subevent_length_set_t), sdc_hci_cmd_vs_cis_subevent_length_set),
    {HCI_COUNTERS_OPCODE, 0U, (uint16_t)HCI_COUNTERS_RETURN_LEN,
     HCI_CMD_RESPONSE_COMPLETE, HciCountersRead},
};

/*
 * Every fixed length row above takes its parameter length from sizeof() on an
 * SDC type, and the host test sends that same sizeof(), so the two agree by
 * construction whatever either one is worth. Nothing in the tests compares
 * either against the wire format the host actually sends.
 *
 * These do. The numbers are the ones Vol 4 Part E gives, written out rather
 * than derived, so an SDC type that does not match the wire is a build failure
 * instead of a controller that answers 0x12 to a correctly formed command. The
 * return lengths matter for the same reason in the other direction: a host
 * discards a Command Complete shorter than the minimum it holds for the
 * opcode, which reads as no answer at all.
 */
#define HCI_SDC_SPEC_LEN(SdcType, SpecLen)                                    \
    static_assert(sizeof(SdcType) == (SpecLen),                               \
                  #SdcType " is not the length Vol 4 Part E gives")

/* Command parameters. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_set_event_mask_t, 8U);                /* 7.3.1  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_set_event_mask_page_2_t, 8U);         /* 7.3.69 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_set_controller_to_host_flow_control_t,
                 1U);                                                 /* 7.3.38 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_host_buffer_size_t, 7U);              /* 7.3.39 */
/* Variable, so only the fixed head and one array element are pinned. 7.3.40 */
HCI_SDC_SPEC_LEN(sdc_hci_cb_host_number_of_completed_packets_array_params_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_lc_disconnect_t, 3U);                    /* 7.1.6  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_event_mask_t, 8U);                /* 7.8.1  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_random_address_t, 6U);            /* 7.8.4  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_adv_params_t, 15U);               /* 7.8.5  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_adv_data_t, 32U);                 /* 7.8.7  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_scan_response_data_t, 32U);       /* 7.8.8  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_adv_enable_t, 1U);                /* 7.8.9  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_scan_params_t, 7U);               /* 7.8.10 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_scan_enable_t, 2U);               /* 7.8.11 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_create_conn_t, 25U);                  /* 7.8.12 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_add_device_to_filter_accept_list_t, 7U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_remove_device_from_filter_accept_list_t, 7U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_add_device_to_resolving_list_t, 39U); /* 7.8.38 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_remove_device_from_resolving_list_t,
                 7U);                                                 /* 7.8.39 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_resolving_list_size_return_t,
                 1U);                                                 /* 7.8.41 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_address_resolution_enable_t,
                 1U);                                                 /* 7.8.44 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_resolvable_private_address_timeout_t,
                 2U);                                                 /* 7.8.45 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_privacy_mode_t, 8U);              /* 7.8.77 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_data_related_address_changes_t,
                 2U);                                                /* 7.8.122 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_conn_update_t, 14U);                  /* 7.8.18 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_channel_map_t, 2U);              /* 7.8.20 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_remote_features_t, 2U);          /* 7.8.21 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_encrypt_t, 32U);                      /* 7.8.22 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_enable_encryption_t, 28U);            /* 7.8.24 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_long_term_key_request_reply_t, 18U);  /* 7.8.25 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_long_term_key_request_negative_reply_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_receiver_test_v1_t, 1U);              /* 7.8.28 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_transmitter_test_v1_t, 3U);           /* 7.8.29 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_receiver_test_v2_t, 3U);              /* 7.8.50 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_transmitter_test_v2_t, 4U);           /* 7.8.51 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_host_feature_t, 2U);             /* 7.8.115 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_host_feature_v2_t, 3U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_sp_read_rssi_t, 2U);                      /* 7.5.4 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_transmit_power_level_t, 3U);     /* 7.3.35 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_write_rf_path_compensation_t, 4U);    /* 7.8.76 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_enhanced_read_transmit_power_level_t,
                 3U);                                               /* 7.8.117 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_remote_transmit_power_level_t,
                 3U);                                               /* 7.8.118 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_path_loss_reporting_params_t,
                 8U);                                               /* 7.8.119 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_path_loss_reporting_enable_t,
                 3U);                                               /* 7.8.120 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_transmit_power_reporting_enable_t,
                 4U);                                               /* 7.8.121 */
/* Thirty seven data channels in a five octet bitmap. Vol 4 Part E 7.8.19. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_host_channel_classification_t, 5U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_request_peer_sca_t, 2U);            /* 7.8.108 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_default_subrate_t, 10U);        /* 7.8.123 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_subrate_request_t, 12U);            /* 7.8.124 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_all_remote_features_t,
                 3U);                                               /* 7.8.150 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_params_t, 7U);       /* 7.8.61 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_enable_t, 2U);       /* 7.8.63 */
/* Variable, so the fixed head alone is pinned. 7.8.62. */
static_assert(offsetof(sdc_hci_cmd_le_set_periodic_adv_data_t, adv_data) == 3U,
              "LE Set Periodic Advertising Data fixed part is not 3 octets");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_create_sync_t, 14U);     /* 7.8.67 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_terminate_sync_t, 2U);   /* 7.8.69 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_add_device_to_periodic_adv_list_t,
                 8U);                                                 /* 7.8.70 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_remove_device_from_periodic_adv_list_t,
                 8U);                                                 /* 7.8.71 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_receive_enable_t,
                 3U);                                                 /* 7.8.88 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_sync_transfer_t,
                 6U);                                                 /* 7.8.89 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_set_info_transfer_t,
                 5U);                                                 /* 7.8.90 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_t,
                 8U);                                                 /* 7.8.91 */
HCI_SDC_SPEC_LEN(
    sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params_t,
    6U);                                                              /* 7.8.92 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_params_v2_t,
                 12U);                                             /* 7.8.61 v2 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_params_v2_return_t, 1U);
/* Handle and count, then entries this layer walks rather than multiplies. */
static_assert(offsetof(sdc_hci_cmd_le_set_periodic_adv_subevent_data_t,
                       array_params) == 2U,
              "LE Set Periodic Adv Subevent Data fixed part is not 2 octets");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_subevent_data_return_t, 1U);
static_assert(offsetof(sdc_hci_cmd_le_set_periodic_adv_response_data_t,
                       response_data) == 8U,
              "LE Set Periodic Adv Response Data fixed part is not 8 octets");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_response_data_return_t, 2U);
static_assert(offsetof(sdc_hci_cmd_le_set_periodic_sync_subevent_t,
                       subevents) == 5U,
              "LE Set Periodic Sync Subevent fixed part is not 5 octets");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_sync_subevent_return_t, 2U);
/* Vendor, so Nordic gives the length. Handle and a microsecond spread. */
static_assert(sizeof(sdc_hci_cmd_vs_set_adv_randomness_t) == 3U,
              "VS Set Adv Randomness is not 3 octets");
static_assert(sizeof(sdc_hci_cmd_vs_llpm_mode_set_t) == 1U,
              "VS LLPM Mode Set is not 1 octet");
/*
 * The interval is microseconds here rather than the 1.25 ms units the standard
 * command uses, which is the whole point of it, and is why this is 10 octets
 * where LE Connection Update is 14.
 */
static_assert(sizeof(sdc_hci_cmd_vs_conn_update_t) == 10U,
              "VS Connection Update is not 10 octets");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_data_length_t, 6U);               /* 7.8.33 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_write_suggested_default_data_length_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_phy_t, 2U);                      /* 7.8.47 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_default_phy_t, 3U);               /* 7.8.48 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_phy_t, 7U);                       /* 7.8.49 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_adv_set_random_address_t, 7U);    /* 7.8.52 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_t, 25U);           /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_v2_t, 27U);        /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_remove_adv_set_t, 1U);                /* 7.8.59 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_scan_enable_t, 6U);           /* 7.8.65 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_lc_read_remote_version_information_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_authenticated_payload_timeout_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_write_authenticated_payload_timeout_t, 4U);
/*
 * Vendor specific, so the length comes from Nordic rather than from Vol 4
 * Part E. TX_Channel and TX_Power_Level, one octet each.
 */
static_assert(sizeof(sdc_hci_cmd_vs_transmitter_carrier_test_t) == 2U,
              "VS Transmitter Carrier Test is not 2 octets");

/*
 * The fixed part of a variable length command, which is what the length check
 * measures the trailing array against, and the array element itself.
 */
HCI_SDC_SPEC_LEN(sdc_hci_le_set_ext_scan_params_array_params_t, 5U);  /* 7.8.64 */
HCI_SDC_SPEC_LEN(sdc_hci_le_ext_create_conn_array_params_t, 16U);     /* 7.8.66 */
HCI_SDC_SPEC_LEN(sdc_hci_le_ext_create_conn_v2_array_params_t, 16U);  /* 7.8.66 */
static_assert(offsetof(sdc_hci_cmd_le_set_ext_scan_params_t, array_params) == 3U,
              "LE Set Extended Scan Parameters fixed part is not 3 octets");
static_assert(offsetof(sdc_hci_cmd_le_ext_create_conn_t, array_params) == 10U,
              "LE Extended Create Connection fixed part is not 10 octets");
/* The v2 fixed part is the v1 one behind Advertising_Handle and Subevent. */
static_assert(offsetof(sdc_hci_cmd_le_ext_create_conn_v2_t, array_params) == 12U,
              "LE Extended Create Connection v2 fixed part is not 12 octets");

/*
 * The direct test mode commands that carry an antenna switching pattern. All
 * three have the same seven octet head, Vol 4 Part E 7.8.50, 7.8.51 and
 * 7.8.128, and v4 adds one octet of transmit power after the pattern rather
 * than before it, which is why its length check is the odd one.
 */
static_assert(offsetof(sdc_hci_cmd_le_receiver_test_v3_t, antenna_ids) == 7U,
              "LE Receiver Test v3 fixed part is not 7 octets");
static_assert(offsetof(sdc_hci_cmd_le_transmitter_test_v3_t, antenna_ids) == 7U,
              "LE Transmitter Test v3 fixed part is not 7 octets");
static_assert(offsetof(sdc_hci_cmd_le_transmitter_test_v4_t,
                       antenna_ids_and_remaining_parameters) == 7U,
              "LE Transmitter Test v4 fixed part is not 7 octets");

/* Return parameters, status excluded since the event reports it separately. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_version_information_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_supported_features_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_bd_addr_return_t, 6U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_buffer_size_return_t, 3U);       /* 7.8.2  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_buffer_size_v2_return_t, 6U);    /* 7.8.132 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_local_supported_features_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_all_local_supported_features_return_t,
                 249U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t, 1U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_filter_accept_list_size_return_t, 1U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_channel_map_return_t, 7U);       /* 7.8.20 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_encrypt_return_t, 16U);               /* 7.8.22 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_rand_return_t, 8U);                   /* 7.8.23 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_long_term_key_request_reply_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_test_end_return_t, 2U);               /* 7.8.30 */
/* Connection_Handle and RSSI, the handle echoed back. Vol 4 Part E 7.5.4. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_sp_read_rssi_return_t, 3U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_transmit_power_level_return_t, 3U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_rf_path_compensation_return_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_enhanced_read_transmit_power_level_return_t,
                 5U);
/*
 * Three commands whose return is the connection handle alone. Short, and a
 * host that holds two octets for the opcode discards anything shorter, which
 * reads to it as no answer at all.
 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_path_loss_reporting_params_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_path_loss_reporting_enable_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_transmit_power_reporting_enable_return_t,
                 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_periodic_adv_list_size_return_t, 1U);
/* Three more whose return is the connection handle alone. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_sync_transfer_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_periodic_adv_set_info_transfer_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_return_t,
                 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_data_length_return_t, 2U);        /* 7.8.33 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_suggested_default_data_length_return_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_max_data_length_return_t, 8U);   /* 7.8.46 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_phy_return_t, 4U);               /* 7.8.47 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_return_t, 1U);     /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_v2_return_t, 1U);  /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_max_adv_data_length_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t, 1U);
/* Min_TX_Power and Max_TX_Power, one octet each. Vol 4 Part E 7.8.74. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_transmit_power_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_supported_commands_return_t, 64U);
/* Six octets of address and sixteen of identity root, per the Zephyr command. */
HCI_SDC_SPEC_LEN(sdc_hci_vs_zephyr_static_address_t, 22U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t, 1U);
static_assert(offsetof(sdc_hci_cmd_le_set_cig_params_return_t, array_params) == 2U,
              "LE Set CIG Parameters return head is not 2 octets");
static_assert(offsetof(sdc_hci_cmd_le_set_cig_params_test_return_t, array_params) == 2U,
              "LE Set CIG Parameters Test return head is not 2 octets");
HCI_SDC_SPEC_LEN(sdc_hci_le_set_cig_params_output_array_params_t, 2U);
/*
 * The rest of the Zephyr family. Vendor specific, so the lengths come from
 * Nordic and from what Zephyr and BlueZ already send, not from Vol 4 Part E.
 * A mismatch here is a host that formats the command correctly and gets 0x12.
 */
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_read_version_info_return_t) == 12U,
              "Zephyr Read Version Information is not 12 octets");
static_assert(
    sizeof(sdc_hci_cmd_vs_zephyr_read_supported_commands_return_t) == 64U,
    "Zephyr Read Supported Commands is not 64 octets");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_write_bd_addr_t) == 6U,
              "Zephyr Write BD_ADDR is not 6 octets");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_read_chip_temp_return_t) == 1U,
              "Zephyr Read Chip Temperature is not 1 octet");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_write_tx_power_t) == 4U,
              "Zephyr Write Tx Power is not 4 octets");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_write_tx_power_return_t) == 4U,
              "Zephyr Write Tx Power return is not 4 octets");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_read_tx_power_t) == 3U,
              "Zephyr Read Tx Power is not 3 octets");
static_assert(sizeof(sdc_hci_cmd_vs_zephyr_read_tx_power_return_t) == 4U,
              "Zephyr Read Tx Power return is not 4 octets");
/* IR and ER, sixteen octets each. */
static_assert(
    sizeof(sdc_hci_cmd_vs_zephyr_read_key_hierarchy_roots_return_t) == 32U,
    "Zephyr Read Key Hierarchy Roots is not 32 octets");
static_assert(sizeof(sdc_hci_cmd_vs_qos_conn_event_report_enable_t) == 1U,
              "VS QoS Conn Event Report Enable is not 1 octet");
/* One octet of enable and a four octet interval in microseconds. */
static_assert(sizeof(sdc_hci_cmd_vs_qos_channel_survey_enable_t) == 5U,
              "VS QoS Channel Survey Enable is not 5 octets");
static_assert(sizeof(sdc_hci_cmd_vs_read_average_rssi_t) == 2U,
              "VS Read Average RSSI is not 2 octets");
static_assert(sizeof(sdc_hci_cmd_vs_read_average_rssi_return_t) == 3U,
              "VS Read Average RSSI return is not 3 octets");
static_assert(sizeof(sdc_hci_cmd_vs_get_next_conn_event_counter_t) == 2U,
              "VS Get Next Conn Event Counter is not 2 octets");
static_assert(
    sizeof(sdc_hci_cmd_vs_get_next_conn_event_counter_return_t) == 4U,
    "VS Get Next Conn Event Counter return is not 4 octets");
static_assert(
    sizeof(sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t) == 1U,
    "VS Conn Anchor Point Update Report Enable is not 1 octet");
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t, 2U);

#undef HCI_SDC_SPEC_LEN

/*
 * Legacy and extended advertising, Vol 4 Part E 3.1.1.
 *
 * A host uses one set of advertising, scanning and initiating commands or the
 * other, never both, and whichever it uses first makes the other Command
 * Disallowed until the controller is reset.
 *
 * The SDC command interface does not enforce the advertising command-family
 * restriction. Enforce Command Disallowed before dispatching a command from
 * the other family.
 *
 * Command-family classification follows sdk-nrf hci_internal.c. Read Maximum
 * Advertising Data Length is extended; Read Advertising Physical Channel
 * Tx Power is legacy.
 */
static HciSdcAdvCmdType_t HciSdcAdvCommandType(uint16_t Opcode)
{
    switch (Opcode)
    {
        case SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_ADV_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_SCAN_RESPONSE_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_ADV_ENABLE:
        case SDC_HCI_OPCODE_CMD_LE_SET_SCAN_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_SCAN_ENABLE:
        case SDC_HCI_OPCODE_CMD_LE_CREATE_CONN:
        case SDC_HCI_OPCODE_CMD_LE_READ_ADV_PHYSICAL_CHANNEL_TX_POWER:
            return HCI_SDC_ADV_CMD_LEGACY;

        case SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS_V2:
        case SDC_HCI_OPCODE_CMD_LE_READ_NUMBER_OF_SUPPORTED_ADV_SETS:
        case SDC_HCI_OPCODE_CMD_LE_READ_PERIODIC_ADV_LIST_SIZE:
        case SDC_HCI_OPCODE_CMD_LE_READ_MAX_ADV_DATA_LENGTH:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_RESPONSE_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_ENABLE:
        case SDC_HCI_OPCODE_CMD_LE_REMOVE_ADV_SET:
        case SDC_HCI_OPCODE_CMD_LE_CLEAR_ADV_SETS:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS_V2:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_ENABLE:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_ENABLE:
        case SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN:
        case SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN_V2:
        case SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC:
        case SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC_CANCEL:
        case SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_TERMINATE_SYNC:
        case SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_PERIODIC_ADV_LIST:
        case SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_PERIODIC_ADV_LIST:
        case SDC_HCI_OPCODE_CMD_LE_CLEAR_PERIODIC_ADV_LIST:
        case SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PERIODIC_ADV_SYNC_TRANSFER_PARAMS:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SUBEVENT_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_SYNC_SUBEVENT:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RESPONSE_DATA:
        case SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SYNC_TRANSFER_PARAMS:
            return HCI_SDC_ADV_CMD_EXTENDED;

        default:
            return HCI_SDC_ADV_CMD_NONE;
    }
}

void HciSdcNrfxlibResetAdvCommandType(void)
{
    s_AdvCmdTypeSinceReset = HCI_SDC_ADV_CMD_NONE;
}

static uint8_t HciSdcAdvCommandGuard(void *, uint16_t Opcode)
{
    const HciSdcAdvCmdType_t needed = HciSdcAdvCommandType(Opcode);

    if (needed == HCI_SDC_ADV_CMD_NONE)
    {
        return HCI_STATUS_SUCCESS;
    }

    if (s_AdvCmdTypeSinceReset == HCI_SDC_ADV_CMD_NONE)
    {
        s_AdvCmdTypeSinceReset = needed;
        return HCI_STATUS_SUCCESS;
    }

    if (s_AdvCmdTypeSinceReset != needed)
    {
        return HCI_STATUS_COMMAND_DISALLOWED;
    }

    return HCI_STATUS_SUCCESS;
}

bool HciSdcNrfxlibInit(HciSdc_t *pSdc,
                       uint8_t *pCommandEvent,
                       size_t CommandEventCapacity,
                       HciCounters_t *pCounters)
{
    /* The counter readout uses the command context; other handlers access SDC directly. */
    if (!HciSdcInit(pSdc,
                    s_HciSdcCommands,
                    sizeof(s_HciSdcCommands) / sizeof(s_HciSdcCommands[0]),
                    pCounters,
                    pCommandEvent,
                    CommandEventCapacity))
    {
        return false;
    }

    /* Reset legacy/extended advertising command-family selection. */
    HciSdcNrfxlibResetAdvCommandType();
    pSdc->Commands.Guard = HciSdcAdvCommandGuard;

    return true;
}

/* Queue an optional startup No Operation Command Complete event.
 * The board-specific caller decides whether it is required, before HCI processing starts.
 */
void HciSdcNrfxlibQueueStartupNop(HciSdc_t *pSdc)
{
    if (pSdc != NULL)
    {
        HciCmdDispatchQueueNop(&pSdc->Commands);
    }
}
