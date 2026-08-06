/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_sdc_nrfxlib.h"

#include "hci_counters.h"

#include <string.h>

#include "nrf_errno.h"
#include "sdc_hci.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_link_control.h"
#include "sdc_hci_cmd_info_params.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_vs.h"

/*
 * The SoftDevice Controller headers declare the whole HCI API, but a library
 * variant only contains the commands it was built with, so a header alone does
 * not mean the symbol links. Every gate below is decided by what is really in
 * the archive:
 *
 *   python3 tests/sdc_symbols.py [path to libsoftdevice_controller_*.a]
 *
 * which reads the archive symbol index and prints what each macro should be.
 * It needs no toolchain. A wrong 1 is a link error, which is loud. A wrong 0
 * is a command answered Unknown HCI Command that the controller could have
 * run, which is silent, so the check is worth running against a new nrfxlib
 * rather than assuming.
 *
 * Read Supported States is genuinely absent from the multirole library. It
 * reports legacy advertising states and is left out of a build that only
 * enables the extended advertiser.
 */
#ifndef HCI_SDC_HAS_READ_SUPPORTED_STATES
#define HCI_SDC_HAS_READ_SUPPORTED_STATES 0
#endif

/*
 * Vol 4 Part E 7.8.74, the minimum and maximum transmit power the controller
 * supports across its PHYs. Nothing to do with LE Power Control, which an
 * earlier comment here claimed, and it needs no sdc_support_ call.
 */
#ifndef HCI_SDC_HAS_READ_TRANSMIT_POWER
#define HCI_SDC_HAS_READ_TRANSMIT_POWER 1
#endif

/*
 * The two below default to 1 because the spec makes them mandatory for a BR or
 * LE controller and the library is expected to carry them. Read Remote Version
 * Information is Vol 4 Part E 7.1.23. The Authenticated Payload Timeout pair,
 * Vol 4 Part E 7.3.93 and 7.3.94, comes with LE Ping and is read and write
 * together, so one macro covers both.
 */
#ifndef HCI_SDC_HAS_READ_REMOTE_VERSION
#define HCI_SDC_HAS_READ_REMOTE_VERSION 1
#endif

#ifndef HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
#define HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT 1
#endif

/*
 * Vendor specific, and the one thing a host can ask when the board carries no
 * public address. A dongle with a blank BD_ADDR that answers nothing here
 * leaves the host to invent an address, so two runs are two different devices.
 */
#ifndef HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
#define HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES 1
#endif

/*
 * Vendor specific counter readout. Costs one table row and no SDC symbol, so
 * it is on unless a build wants the opcode back.
 */
#ifndef HCI_SDC_HAS_VS_READ_COUNTERS
#define HCI_SDC_HAS_VS_READ_COUNTERS 1
#endif

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
#if HCI_SDC_HAS_READ_REMOTE_VERSION
    supported.params.hci_read_remote_version_information = 1U;
#endif
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
    supported.params.hci_read_authenticated_payload_timeout = 1U;
    supported.params.hci_write_authenticated_payload_timeout = 1U;
#endif
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

#if HCI_SDC_HAS_READ_SUPPORTED_STATES
    supported.params.hci_le_read_supported_states = 1U;
#endif
#if HCI_SDC_HAS_READ_TRANSMIT_POWER
    supported.params.hci_le_read_transmit_power = 1U;
#endif

    supported.params.hci_le_receiver_test_v1 = 1U;
    supported.params.hci_le_transmitter_test_v1 = 1U;
    supported.params.hci_le_test_end = 1U;

    supported.params.hci_le_set_data_length = 1U;
    supported.params.hci_le_read_suggested_default_data_length = 1U;
    supported.params.hci_le_write_suggested_default_data_length = 1U;
    supported.params.hci_le_read_maximum_data_length = 1U;

    supported.params.hci_le_read_phy = 1U;
    supported.params.hci_le_set_default_phy = 1U;
    supported.params.hci_le_set_phy = 1U;

    supported.params.hci_le_set_advertising_set_random_address = 1U;
    supported.params.hci_le_set_extended_advertising_parameters = 1U;
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

    supported.params.hci_le_add_device_to_resolving_list = 1U;
    supported.params.hci_le_remove_device_from_resolving_list = 1U;
    supported.params.hci_le_clear_resolving_list = 1U;
    supported.params.hci_le_read_resolving_list_size = 1U;
    supported.params.hci_le_set_address_resolution_enable = 1U;
    supported.params.hci_le_set_resolvable_private_address_timeout = 1U;
    supported.params.hci_le_set_privacy_mode = 1U;
    supported.params.hci_le_set_data_related_address_changes = 1U;

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
 * The handlers below are all the same few shapes, so they are generated to
 * keep the mapping between an opcode and its SDC call in one line each. The
 * shapes are: parameters only, parameters with a return, no parameters, no
 * parameters with a return, and the variable length forms where the command
 * carries an array whose size comes from the packet.
 */
#define HCI_SDC_CMD_P(Name, SdcFunc, SdcType, Reply)                          \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t,                                        \
                               uint8_t *,                                     \
                               size_t)                                        \
    {                                                                         \
        SdcType params;                                                       \
        memcpy(&params, pParams, sizeof(params));                             \
        return Reply(SdcFunc(&params), 0U);                                   \
    }

#define HCI_SDC_CMD_PR(Name, SdcFunc, SdcType, SdcReturn)                     \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *pParams,                        \
                               size_t,                                        \
                               uint8_t *pReturn,                              \
                               size_t ReturnCapacity)                         \
    {                                                                         \
        SdcType params;                                                       \
        SdcReturn result;                                                     \
        if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))                \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);   \
        }                                                                     \
        memcpy(&params, pParams, sizeof(params));                             \
        uint8_t status = SdcFunc(&params, &result);                           \
        if (status != HCI_STATUS_SUCCESS)                                     \
        {                                                                     \
            return HciSdcComplete(status, 0U);                                \
        }                                                                     \
        memcpy(pReturn, &result, sizeof(result));                             \
        return HciSdcComplete(status, sizeof(result));                        \
    }

#define HCI_SDC_CMD_N(Name, SdcFunc)                                          \
    static HciCmdResult_t Name(void *, const uint8_t *, size_t, uint8_t *,    \
                               size_t)                                        \
    {                                                                         \
        return HciSdcComplete(SdcFunc(), 0U);                                 \
    }

#define HCI_SDC_CMD_NR(Name, SdcFunc, SdcReturn)                              \
    static HciCmdResult_t Name(void *,                                        \
                               const uint8_t *,                               \
                               size_t,                                        \
                               uint8_t *pReturn,                              \
                               size_t ReturnCapacity)                         \
    {                                                                         \
        SdcReturn result;                                                     \
        if (!HciSdcReturnFits(sizeof(result), ReturnCapacity))                \
        {                                                                     \
            return HciSdcComplete(HCI_STATUS_MEMORY_CAPACITY_EXCEEDED, 0U);   \
        }                                                                     \
        uint8_t status = SdcFunc(&result);                                    \
        if (status != HCI_STATUS_SUCCESS)                                     \
        {                                                                     \
            return HciSdcComplete(status, 0U);                                \
        }                                                                     \
        memcpy(pReturn, &result, sizeof(result));                             \
        return HciSdcComplete(status, sizeof(result));                        \
    }

/*
 * Variable length commands carry a trailing array. The SDC types are packed
 * and byte aligned, so the packet is handed over directly rather than copied
 * into a fixed local.
 *
 * The fixed part alone is not enough to bound the read. The length of the
 * trailing array is carried in a field inside the fixed part, and the SDC call
 * trusts it. That field has to be checked against the parameter length the
 * host actually sent, or a short packet with a large count makes SDC read past
 * the end of the receive buffer. That buffer is reused for every packet and is
 * not cleared between them, so what lies past the end is the previous packet.
 *
 * Three shapes of count field appear in the commands used here, one macro
 * each. All three compute the number of bytes the array needs and require
 * ParamLen to match exactly before the packet is handed over. Exactly, not at
 * least: Vol 4 Part E 5.4.1 fixes Parameter_Total_Length, so trailing bytes
 * beyond what the count declares mean the host and the controller disagree
 * about the packet, and guessing which one is right is worse than refusing.
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

#if HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
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
#endif

/* Controller and baseband. */
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
HCI_SDC_CMD_PR(HciSdcCmdReadAuthPayloadTimeout,
               sdc_hci_cmd_cb_read_authenticated_payload_timeout,
               sdc_hci_cmd_cb_read_authenticated_payload_timeout_t,
               sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t)
HCI_SDC_CMD_PR(HciSdcCmdWriteAuthPayloadTimeout,
               sdc_hci_cmd_cb_write_authenticated_payload_timeout,
               sdc_hci_cmd_cb_write_authenticated_payload_timeout_t,
               sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t)
#endif

/* Link control. */
HCI_SDC_CMD_P(HciSdcCmdDisconnect, sdc_hci_cmd_lc_disconnect,
              sdc_hci_cmd_lc_disconnect_t, HciSdcStatus)
#if HCI_SDC_HAS_READ_REMOTE_VERSION
HCI_SDC_CMD_P(HciSdcCmdReadRemoteVersion,
              sdc_hci_cmd_lc_read_remote_version_information,
              sdc_hci_cmd_lc_read_remote_version_information_t, HciSdcStatus)
#endif

/* Connection management. */
HCI_SDC_CMD_P(HciSdcCmdLeCreateConn, sdc_hci_cmd_le_create_conn,
              sdc_hci_cmd_le_create_conn_t, HciSdcStatus)
HCI_SDC_CMD_N(HciSdcCmdLeCreateConnCancel, sdc_hci_cmd_le_create_conn_cancel)
HCI_SDC_CMD_P(HciSdcCmdLeConnUpdate, sdc_hci_cmd_le_conn_update,
              sdc_hci_cmd_le_conn_update_t, HciSdcStatus)
HCI_SDC_CMD_PR(HciSdcCmdLeReadChannelMap, sdc_hci_cmd_le_read_channel_map,
               sdc_hci_cmd_le_read_channel_map_t,
               sdc_hci_cmd_le_read_channel_map_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeReadRemoteFeatures, sdc_hci_cmd_le_read_remote_features,
              sdc_hci_cmd_le_read_remote_features_t, HciSdcStatus)

/* Filter accept list. */
HCI_SDC_CMD_NR(HciSdcCmdLeReadAcceptListSize,
               sdc_hci_cmd_le_read_filter_accept_list_size,
               sdc_hci_cmd_le_read_filter_accept_list_size_return_t)
HCI_SDC_CMD_N(HciSdcCmdLeClearAcceptList, sdc_hci_cmd_le_clear_filter_accept_list)
HCI_SDC_CMD_P(HciSdcCmdLeAddToAcceptList,
              sdc_hci_cmd_le_add_device_to_filter_accept_list,
              sdc_hci_cmd_le_add_device_to_filter_accept_list_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeRemoveFromAcceptList,
              sdc_hci_cmd_le_remove_device_from_filter_accept_list,
              sdc_hci_cmd_le_remove_device_from_filter_accept_list_t,
              HciSdcComplete)

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
HCI_SDC_CMD_P(HciSdcCmdLeAddToResolvingList,
              sdc_hci_cmd_le_add_device_to_resolving_list,
              sdc_hci_cmd_le_add_device_to_resolving_list_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeRemoveFromResolvingList,
              sdc_hci_cmd_le_remove_device_from_resolving_list,
              sdc_hci_cmd_le_remove_device_from_resolving_list_t,
              HciSdcComplete)
HCI_SDC_CMD_N(HciSdcCmdLeClearResolvingList,
              sdc_hci_cmd_le_clear_resolving_list)
HCI_SDC_CMD_NR(HciSdcCmdLeReadResolvingListSize,
               sdc_hci_cmd_le_read_resolving_list_size,
               sdc_hci_cmd_le_read_resolving_list_size_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeSetAddressResolutionEnable,
              sdc_hci_cmd_le_set_address_resolution_enable,
              sdc_hci_cmd_le_set_address_resolution_enable_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeSetRpaTimeout,
              sdc_hci_cmd_le_set_resolvable_private_address_timeout,
              sdc_hci_cmd_le_set_resolvable_private_address_timeout_t,
              HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeSetPrivacyMode, sdc_hci_cmd_le_set_privacy_mode,
              sdc_hci_cmd_le_set_privacy_mode_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeSetDataRelatedAddressChanges,
              sdc_hci_cmd_le_set_data_related_address_changes,
              sdc_hci_cmd_le_set_data_related_address_changes_t,
              HciSdcComplete)

/* Security. */
HCI_SDC_CMD_PR(HciSdcCmdLeEncrypt, sdc_hci_cmd_le_encrypt,
               sdc_hci_cmd_le_encrypt_t, sdc_hci_cmd_le_encrypt_return_t)
HCI_SDC_CMD_NR(HciSdcCmdLeRand, sdc_hci_cmd_le_rand,
               sdc_hci_cmd_le_rand_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeEnableEncryption, sdc_hci_cmd_le_enable_encryption,
              sdc_hci_cmd_le_enable_encryption_t, HciSdcStatus)
HCI_SDC_CMD_PR(HciSdcCmdLeLtkReply,
               sdc_hci_cmd_le_long_term_key_request_reply,
               sdc_hci_cmd_le_long_term_key_request_reply_t,
               sdc_hci_cmd_le_long_term_key_request_reply_return_t)
HCI_SDC_CMD_PR(HciSdcCmdLeLtkNegativeReply,
               sdc_hci_cmd_le_long_term_key_request_negative_reply,
               sdc_hci_cmd_le_long_term_key_request_negative_reply_t,
               sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t)

/*
 * Capability. The SoftDevice Controller headers declare the whole HCI API,
 * but a given library variant only contains the commands it was built with,
 * so these two are opt in. Check what the library actually defines with
 *
 *   arm-none-eabi-nm --defined-only libsoftdevice_controller_multirole.a \
 *       | grep sdc_hci_cmd_le_
 *
 * and set the macro when the symbol is present. Read Supported States reports
 * legacy advertising states and is absent from a build that only enables the
 * extended advertiser. Read Transmit Power belongs to LE Power Control.
 */
#if HCI_SDC_HAS_READ_SUPPORTED_STATES
HCI_SDC_CMD_NR(HciSdcCmdLeReadSupportedStates,
               sdc_hci_cmd_le_read_supported_states,
               sdc_hci_cmd_le_read_supported_states_return_t)
#endif

#if HCI_SDC_HAS_READ_TRANSMIT_POWER
HCI_SDC_CMD_NR(HciSdcCmdLeReadTransmitPower,
               sdc_hci_cmd_le_read_transmit_power,
               sdc_hci_cmd_le_read_transmit_power_return_t)
#endif

/* Direct test mode. */
HCI_SDC_CMD_P(HciSdcCmdLeReceiverTest, sdc_hci_cmd_le_receiver_test_v1,
              sdc_hci_cmd_le_receiver_test_v1_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeTransmitterTest, sdc_hci_cmd_le_transmitter_test_v1,
              sdc_hci_cmd_le_transmitter_test_v1_t, HciSdcComplete)
HCI_SDC_CMD_NR(HciSdcCmdLeTestEnd, sdc_hci_cmd_le_test_end,
               sdc_hci_cmd_le_test_end_return_t)

/* Data length. */
HCI_SDC_CMD_PR(HciSdcCmdLeSetDataLength, sdc_hci_cmd_le_set_data_length,
               sdc_hci_cmd_le_set_data_length_t,
               sdc_hci_cmd_le_set_data_length_return_t)
HCI_SDC_CMD_NR(HciSdcCmdLeReadSuggestedDataLength,
               sdc_hci_cmd_le_read_suggested_default_data_length,
               sdc_hci_cmd_le_read_suggested_default_data_length_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeWriteSuggestedDataLength,
              sdc_hci_cmd_le_write_suggested_default_data_length,
              sdc_hci_cmd_le_write_suggested_default_data_length_t,
              HciSdcComplete)
HCI_SDC_CMD_NR(HciSdcCmdLeReadMaxDataLength,
               sdc_hci_cmd_le_read_max_data_length,
               sdc_hci_cmd_le_read_max_data_length_return_t)

/* PHY. */
HCI_SDC_CMD_PR(HciSdcCmdLeReadPhy, sdc_hci_cmd_le_read_phy,
               sdc_hci_cmd_le_read_phy_t, sdc_hci_cmd_le_read_phy_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeSetDefaultPhy, sdc_hci_cmd_le_set_default_phy,
              sdc_hci_cmd_le_set_default_phy_t, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeSetPhy, sdc_hci_cmd_le_set_phy,
              sdc_hci_cmd_le_set_phy_t, HciSdcStatus)

/* Extended advertising. */
HCI_SDC_CMD_P(HciSdcCmdLeSetAdvSetRandomAddr,
              sdc_hci_cmd_le_set_adv_set_random_address,
              sdc_hci_cmd_le_set_adv_set_random_address_t, HciSdcComplete)
HCI_SDC_CMD_PR(HciSdcCmdLeSetExtAdvParams, sdc_hci_cmd_le_set_ext_adv_params,
               sdc_hci_cmd_le_set_ext_adv_params_t,
               sdc_hci_cmd_le_set_ext_adv_params_return_t)
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
HCI_SDC_CMD_NR(HciSdcCmdLeReadMaxAdvDataLength,
               sdc_hci_cmd_le_read_max_adv_data_length,
               sdc_hci_cmd_le_read_max_adv_data_length_return_t)
HCI_SDC_CMD_NR(HciSdcCmdLeReadNumAdvSets,
               sdc_hci_cmd_le_read_number_of_supported_adv_sets,
               sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t)
HCI_SDC_CMD_P(HciSdcCmdLeRemoveAdvSet, sdc_hci_cmd_le_remove_adv_set,
              sdc_hci_cmd_le_remove_adv_set_t, HciSdcComplete)
HCI_SDC_CMD_N(HciSdcCmdLeClearAdvSets, sdc_hci_cmd_le_clear_adv_sets)

/* Extended scanning and initiating. */
HCI_SDC_CMD_VP(HciSdcCmdLeSetExtScanParams, sdc_hci_cmd_le_set_ext_scan_params,
               sdc_hci_cmd_le_set_ext_scan_params_t, array_params,
               scanning_phys, HciSdcComplete)
HCI_SDC_CMD_P(HciSdcCmdLeSetExtScanEnable, sdc_hci_cmd_le_set_ext_scan_enable,
              sdc_hci_cmd_le_set_ext_scan_enable_t, HciSdcComplete)
HCI_SDC_CMD_VP(HciSdcCmdLeExtCreateConn, sdc_hci_cmd_le_ext_create_conn,
               sdc_hci_cmd_le_ext_create_conn_t, array_params,
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

static const HciCmdEntry_t s_HciSdcCommands[] = {
    /* Controller and baseband. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_CB_SET_EVENT_MASK, 8U,
                    HciSdcCmdSetEventMask),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_CB_RESET, 0U, HciSdcCmdReset),
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
    HCI_SDC_ENTRY_CR(
        SDC_HCI_OPCODE_CMD_CB_READ_AUTHENTICATED_PAYLOAD_TIMEOUT,
        sizeof(sdc_hci_cmd_cb_read_authenticated_payload_timeout_t),
        HciSdcCmdReadAuthPayloadTimeout,
        sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t),
    HCI_SDC_ENTRY_CR(
        SDC_HCI_OPCODE_CMD_CB_WRITE_AUTHENTICATED_PAYLOAD_TIMEOUT,
        sizeof(sdc_hci_cmd_cb_write_authenticated_payload_timeout_t),
        HciSdcCmdWriteAuthPayloadTimeout,
        sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t),
#endif

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
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LC_DISCONNECT,
                    sizeof(sdc_hci_cmd_lc_disconnect_t), HciSdcCmdDisconnect),
#if HCI_SDC_HAS_READ_REMOTE_VERSION
    HCI_SDC_ENTRY_S(
        SDC_HCI_OPCODE_CMD_LC_READ_REMOTE_VERSION_INFORMATION,
        sizeof(sdc_hci_cmd_lc_read_remote_version_information_t),
        HciSdcCmdReadRemoteVersion),
#endif

    /* Connection management. */
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN,
                    sizeof(sdc_hci_cmd_le_create_conn_t),
                    HciSdcCmdLeCreateConn),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN_CANCEL, 0U,
                    HciSdcCmdLeCreateConnCancel),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_CONN_UPDATE,
                    sizeof(sdc_hci_cmd_le_conn_update_t),
                    HciSdcCmdLeConnUpdate),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_CHANNEL_MAP,
                     sizeof(sdc_hci_cmd_le_read_channel_map_t),
                     HciSdcCmdLeReadChannelMap,
                     sdc_hci_cmd_le_read_channel_map_return_t),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_READ_REMOTE_FEATURES,
                    sizeof(sdc_hci_cmd_le_read_remote_features_t),
                    HciSdcCmdLeReadRemoteFeatures),

    /* Filter accept list. */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_FILTER_ACCEPT_LIST_SIZE, 0U,
                     HciSdcCmdLeReadAcceptListSize,
                     sdc_hci_cmd_le_read_filter_accept_list_size_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_CLEAR_FILTER_ACCEPT_LIST, 0U,
                    HciSdcCmdLeClearAcceptList),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_FILTER_ACCEPT_LIST,
                    sizeof(sdc_hci_cmd_le_add_device_to_filter_accept_list_t),
                    HciSdcCmdLeAddToAcceptList),
    HCI_SDC_ENTRY_C(
        SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_FILTER_ACCEPT_LIST,
        sizeof(sdc_hci_cmd_le_remove_device_from_filter_accept_list_t),
        HciSdcCmdLeRemoveFromAcceptList),

    /* Privacy and the resolving list. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_RESOLVING_LIST,
                    sizeof(sdc_hci_cmd_le_add_device_to_resolving_list_t),
                    HciSdcCmdLeAddToResolvingList),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_RESOLVING_LIST,
                    sizeof(sdc_hci_cmd_le_remove_device_from_resolving_list_t),
                    HciSdcCmdLeRemoveFromResolvingList),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_CLEAR_RESOLVING_LIST, 0U,
                    HciSdcCmdLeClearResolvingList),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_RESOLVING_LIST_SIZE, 0U,
                     HciSdcCmdLeReadResolvingListSize,
                     sdc_hci_cmd_le_read_resolving_list_size_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_ADDRESS_RESOLUTION_ENABLE,
                    sizeof(sdc_hci_cmd_le_set_address_resolution_enable_t),
                    HciSdcCmdLeSetAddressResolutionEnable),
    HCI_SDC_ENTRY_C(
        SDC_HCI_OPCODE_CMD_LE_SET_RESOLVABLE_PRIVATE_ADDRESS_TIMEOUT,
        sizeof(sdc_hci_cmd_le_set_resolvable_private_address_timeout_t),
        HciSdcCmdLeSetRpaTimeout),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_PRIVACY_MODE,
                    sizeof(sdc_hci_cmd_le_set_privacy_mode_t),
                    HciSdcCmdLeSetPrivacyMode),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_DATA_RELATED_ADDRESS_CHANGES,
                    sizeof(sdc_hci_cmd_le_set_data_related_address_changes_t),
                    HciSdcCmdLeSetDataRelatedAddressChanges),

    /* Security. */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_ENCRYPT,
                     sizeof(sdc_hci_cmd_le_encrypt_t), HciSdcCmdLeEncrypt,
                     sdc_hci_cmd_le_encrypt_return_t),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_RAND, 0U, HciSdcCmdLeRand,
                     sdc_hci_cmd_le_rand_return_t),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_ENABLE_ENCRYPTION,
                    sizeof(sdc_hci_cmd_le_enable_encryption_t),
                    HciSdcCmdLeEnableEncryption),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_REPLY,
                     sizeof(sdc_hci_cmd_le_long_term_key_request_reply_t),
                     HciSdcCmdLeLtkReply,
                     sdc_hci_cmd_le_long_term_key_request_reply_return_t),
    HCI_SDC_ENTRY_CR(
        SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_NEGATIVE_REPLY,
        sizeof(sdc_hci_cmd_le_long_term_key_request_negative_reply_t),
        HciSdcCmdLeLtkNegativeReply,
        sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t),

#if HCI_SDC_HAS_READ_SUPPORTED_STATES
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_SUPPORTED_STATES, 0U,
                     HciSdcCmdLeReadSupportedStates,
                     sdc_hci_cmd_le_read_supported_states_return_t),
#endif
#if HCI_SDC_HAS_READ_TRANSMIT_POWER
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_TRANSMIT_POWER, 0U,
                     HciSdcCmdLeReadTransmitPower,
                     sdc_hci_cmd_le_read_transmit_power_return_t),
#endif

    /* Direct test mode. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V1,
                    sizeof(sdc_hci_cmd_le_receiver_test_v1_t),
                    HciSdcCmdLeReceiverTest),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V1,
                    sizeof(sdc_hci_cmd_le_transmitter_test_v1_t),
                    HciSdcCmdLeTransmitterTest),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_TEST_END, 0U, HciSdcCmdLeTestEnd,
                     sdc_hci_cmd_le_test_end_return_t),

    /* Data length. */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_SET_DATA_LENGTH,
                     sizeof(sdc_hci_cmd_le_set_data_length_t),
                     HciSdcCmdLeSetDataLength,
                     sdc_hci_cmd_le_set_data_length_return_t),
    HCI_SDC_ENTRY_CR(
        SDC_HCI_OPCODE_CMD_LE_READ_SUGGESTED_DEFAULT_DATA_LENGTH, 0U,
        HciSdcCmdLeReadSuggestedDataLength,
        sdc_hci_cmd_le_read_suggested_default_data_length_return_t),
    HCI_SDC_ENTRY_C(
        SDC_HCI_OPCODE_CMD_LE_WRITE_SUGGESTED_DEFAULT_DATA_LENGTH,
        sizeof(sdc_hci_cmd_le_write_suggested_default_data_length_t),
        HciSdcCmdLeWriteSuggestedDataLength),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_MAX_DATA_LENGTH, 0U,
                     HciSdcCmdLeReadMaxDataLength,
                     sdc_hci_cmd_le_read_max_data_length_return_t),

    /* PHY. */
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_PHY,
                     sizeof(sdc_hci_cmd_le_read_phy_t), HciSdcCmdLeReadPhy,
                     sdc_hci_cmd_le_read_phy_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PHY,
                    sizeof(sdc_hci_cmd_le_set_default_phy_t),
                    HciSdcCmdLeSetDefaultPhy),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_SET_PHY,
                    sizeof(sdc_hci_cmd_le_set_phy_t), HciSdcCmdLeSetPhy),

    /* Extended advertising. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS,
                    sizeof(sdc_hci_cmd_le_set_adv_set_random_address_t),
                    HciSdcCmdLeSetAdvSetRandomAddr),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS,
                     sizeof(sdc_hci_cmd_le_set_ext_adv_params_t),
                     HciSdcCmdLeSetExtAdvParams,
                     sdc_hci_cmd_le_set_ext_adv_params_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_DATA,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtAdvData),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_RESPONSE_DATA,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtScanRsp),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_ENABLE,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtAdvEnable),
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_LE_READ_MAX_ADV_DATA_LENGTH, 0U,
                     HciSdcCmdLeReadMaxAdvDataLength,
                     sdc_hci_cmd_le_read_max_adv_data_length_return_t),
    HCI_SDC_ENTRY_CR(
        SDC_HCI_OPCODE_CMD_LE_READ_NUMBER_OF_SUPPORTED_ADV_SETS, 0U,
        HciSdcCmdLeReadNumAdvSets,
        sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_REMOVE_ADV_SET,
                    sizeof(sdc_hci_cmd_le_remove_adv_set_t),
                    HciSdcCmdLeRemoveAdvSet),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_CLEAR_ADV_SETS, 0U,
                    HciSdcCmdLeClearAdvSets),

    /* Extended scanning and initiating. */
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_PARAMS,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeSetExtScanParams),
    HCI_SDC_ENTRY_C(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_ENABLE,
                    sizeof(sdc_hci_cmd_le_set_ext_scan_enable_t),
                    HciSdcCmdLeSetExtScanEnable),
    HCI_SDC_ENTRY_S(SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN,
                    HCI_CMD_VARIABLE_PARAM_LEN, HciSdcCmdLeExtCreateConn),

    /*
     * Vendor specific. The return type ends in a flexible array, so sizeof()
     * on it is the count byte alone, which is exactly the minimum this command
     * always carries and what an error is padded out to.
     */
#if HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
    HCI_SDC_ENTRY_CR(SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_STATIC_ADDRESSES, 0U,
                     HciSdcCmdVsReadStaticAddresses,
                     sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t),
#endif
#if HCI_SDC_HAS_VS_READ_COUNTERS
    /*
     * Answered by the routing layer rather than by SDC, so it reports what
     * this firmware refused rather than what the radio did. The length is a
     * constant from the header instead of a sizeof(), because the wire format
     * is written out field by field and owes nothing to a struct layout.
     */
    {HCI_COUNTERS_OPCODE, 0U, (uint16_t)HCI_COUNTERS_RETURN_LEN,
     HCI_CMD_RESPONSE_COMPLETE, HciCountersRead},
#endif
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
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_data_length_t, 6U);               /* 7.8.33 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_write_suggested_default_data_length_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_phy_t, 2U);                      /* 7.8.47 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_default_phy_t, 3U);               /* 7.8.48 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_phy_t, 7U);                       /* 7.8.49 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_adv_set_random_address_t, 7U);    /* 7.8.52 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_t, 25U);           /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_remove_adv_set_t, 1U);                /* 7.8.59 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_scan_enable_t, 6U);           /* 7.8.65 */
#if HCI_SDC_HAS_READ_REMOTE_VERSION
HCI_SDC_SPEC_LEN(sdc_hci_cmd_lc_read_remote_version_information_t, 2U);
#endif
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_authenticated_payload_timeout_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_write_authenticated_payload_timeout_t, 4U);
#endif

/*
 * The fixed part of a variable length command, which is what the length check
 * measures the trailing array against, and the array element itself.
 */
HCI_SDC_SPEC_LEN(sdc_hci_le_set_ext_scan_params_array_params_t, 5U);  /* 7.8.64 */
HCI_SDC_SPEC_LEN(sdc_hci_le_ext_create_conn_array_params_t, 16U);     /* 7.8.66 */
static_assert(offsetof(sdc_hci_cmd_le_set_ext_scan_params_t, array_params) == 3U,
              "LE Set Extended Scan Parameters fixed part is not 3 octets");
static_assert(offsetof(sdc_hci_cmd_le_ext_create_conn_t, array_params) == 10U,
              "LE Extended Create Connection fixed part is not 10 octets");

/* Return parameters, status excluded since the event carries it separately. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_version_information_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_supported_features_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_bd_addr_return_t, 6U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_buffer_size_return_t, 3U);       /* 7.8.2  */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_local_supported_features_return_t, 8U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t, 1U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_filter_accept_list_size_return_t, 1U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_channel_map_return_t, 7U);       /* 7.8.20 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_encrypt_return_t, 16U);               /* 7.8.22 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_rand_return_t, 8U);                   /* 7.8.23 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_long_term_key_request_reply_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_test_end_return_t, 2U);               /* 7.8.30 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_data_length_return_t, 2U);        /* 7.8.33 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_suggested_default_data_length_return_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_max_data_length_return_t, 8U);   /* 7.8.46 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_phy_return_t, 4U);               /* 7.8.47 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_set_ext_adv_params_return_t, 1U);     /* 7.8.53 */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_max_adv_data_length_return_t, 2U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t, 1U);
#if HCI_SDC_HAS_READ_TRANSMIT_POWER
/* Min_TX_Power and Max_TX_Power, one octet each. Vol 4 Part E 7.8.74. */
HCI_SDC_SPEC_LEN(sdc_hci_cmd_le_read_transmit_power_return_t, 2U);
#endif
HCI_SDC_SPEC_LEN(sdc_hci_cmd_ip_read_local_supported_commands_return_t, 64U);
#if HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
/* Six octets of address and sixteen of identity root, per the Zephyr command. */
HCI_SDC_SPEC_LEN(sdc_hci_vs_zephyr_static_address_t, 22U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_vs_zephyr_read_static_addresses_return_t, 1U);
#endif
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t, 4U);
HCI_SDC_SPEC_LEN(sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t, 2U);
#endif

#undef HCI_SDC_SPEC_LEN

static int32_t HciSdcNrfxlibAclPut(void *, const uint8_t *pPacket)
{
    return sdc_hci_data_put(pPacket);
}

static int32_t HciSdcNrfxlibIsoPut(void *, const uint8_t *pPacket)
{
    return sdc_hci_iso_data_put(pPacket);
}

static int32_t HciSdcNrfxlibGet(void *, uint8_t *pPacket, uint8_t *pType)
{
    uint8_t type = SDC_HCI_MSG_TYPE_NONE;
    int32_t result = sdc_hci_get(pPacket, &type);
    *pType = type;
    return result;
}

bool HciSdcNrfxlibInit(HciSdc_t *pSdc,
                       uint8_t *pCommandEvent,
                       size_t CommandEventCapacity,
                       HciCounters_t *pCounters)
{
    HciSdcOps_t ops = {
        HciSdcNrfxlibAclPut,
        HciSdcNrfxlibIsoPut,
        HciSdcNrfxlibGet,
        NULL,
        NULL,
        -NRF_EAGAIN,
    };

    /*
     * The command context is the counter readout's view of the stack. Every
     * other handler in the table ignores it and talks to SDC through file
     * scope entry points.
     */
    if (!HciSdcInit(pSdc,
                    &ops,
                    s_HciSdcCommands,
                    sizeof(s_HciSdcCommands) / sizeof(s_HciSdcCommands[0]),
                    pCounters,
                    pCommandEvent,
                    CommandEventCapacity))
    {
        return false;
    }

    return true;
}

/*
 * Say the controller is ready with a No Operation Command Complete.
 *
 * Whether a board needs this is a board fact, and this file cannot see board.h,
 * so the caller decides. It used to be a macro tested here, which meant the
 * only board that sets it got an image with the call compiled out and nothing
 * to say so.
 *
 * Call it after init and before the runtime thread starts, which is where the
 * dispatcher is empty and no command can have arrived.
 */
void HciSdcNrfxlibQueueStartupNop(HciSdc_t *pSdc)
{
    if (pSdc != NULL)
    {
        HciCmdDispatchQueueNop(&pSdc->Commands);
    }
}
