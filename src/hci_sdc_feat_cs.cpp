/**-------------------------------------------------------------------------
@file	hci_sdc_feat_cs.cpp

@brief	Channel Sounding. Link this file to turn the feature on.

		sdk-nrfxlib has Channel Sounding for the nRF54L family only. The nRF52
		and nRF53 libraries have none of the calls below, so a project for
		those parts does not link this file. The project also sets
		HCI_SDC_CS_COUNT, which sizes the pool (hci_sdc_resources.h).

		Both roles, initiator and reflector, on central and peripheral links,
		step mode 3 and the CS test command. One antenna.

		Bluetooth Core Vol 4 Part E 7.8.130 to 7.8.143 for the commands, plus
		the two CS security requirements commands,
		Vol 6 Part H for the feature.

@author	Nguyen Hoan Hoang
@date	November 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include <stddef.h>
#include <stdint.h>

#include "sdc.h"
#include "sdc_hci_cmd_le.h"

#include "hci_cmd_dispatch.h"
#include "hci_sdc_resources.h"
#include "hci_sdc_row.h"

extern "C" bool HciSdcFeatCsSupport(void)
{
    if (HCI_SDC_CS_COUNT == 0U)
    {
        /* No pool was reserved for it. */
        return false;
    }

    sdc_support_channel_sounding_initiator_role_central();
    sdc_support_channel_sounding_initiator_role_peripheral();
    sdc_support_channel_sounding_reflector_role_central();
    sdc_support_channel_sounding_reflector_role_peripheral();
    sdc_support_channel_sounding_mode3();
    sdc_support_channel_sounding_test();
    return true;
}

extern "C" bool HciSdcFeatCsConfig(void)
{
    sdc_cfg_t cfg = {};

    cfg.cs_count.count = HCI_SDC_CS_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_CS_COUNT, &cfg))
    {
        return false;
    }

    cfg = {};
    cfg.cs_cfg.max_antenna_paths_supported = HCI_SDC_CS_MAX_ANTENNA_PATHS;
    cfg.cs_cfg.num_antennas_supported = HCI_SDC_CS_NUM_ANTENNAS;
    return HciSdcCfgSet(SDC_CFG_TYPE_CS_CFG, &cfg);
}

/*
 * LE CS Test ends in the override parameters, whose length is in the octet
 * before them. The received length has to match it exactly before the packet
 * goes to SDC.
 */
static HciCmdResult_t HciSdcCmdLeCsTest(void *,
                                        const uint8_t *pParams,
                                        size_t ParamLen,
                                        uint8_t *,
                                        size_t)
{
    const size_t head = offsetof(sdc_hci_cmd_le_cs_test_t, override_params_data);
    HciCmdResult_t result = {HCI_STATUS_INVALID_HCI_PARAMETERS,
                             HCI_CMD_RESPONSE_COMPLETE, 0U};

    if (ParamLen < head)
    {
        return result;
    }

    const sdc_hci_cmd_le_cs_test_t *pCmd =
        reinterpret_cast<const sdc_hci_cmd_le_cs_test_t *>(pParams);

    if (ParamLen - head != pCmd->override_params_length)
    {
        return result;
    }

    result.Status = sdc_hci_cmd_le_cs_test(pCmd);
    return result;
}

/*
 * The response of each row is the one its command section names: Command
 * Status for the commands that start a procedure on the link or end the test,
 * Command Complete for the rest.
 */
static const HciCmdEntry_t s_HciSdcCsCommands[] = {
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_CS_READ_LOCAL_SUPPORTED_CAPABILITIES, 0U,
                   sdc_hci_cmd_le_cs_read_local_supported_capabilities_return_t,
                   sdc_hci_cmd_le_cs_read_local_supported_capabilities),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_READ_REMOTE_SUPPORTED_CAPABILITIES,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_read_remote_supported_capabilities_t, 2U,
                  sdc_hci_cmd_le_cs_read_remote_supported_capabilities),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_WRITE_CACHED_REMOTE_SUPPORTED_CAPABILITIES,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities_t, 30U,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities_return_t,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_SECURITY_ENABLE,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_security_enable_t, 2U,
                  sdc_hci_cmd_le_cs_security_enable),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_SET_DEFAULT_SETTINGS,
                   sdc_hci_cmd_le_cs_set_default_settings_t, 5U,
                   sdc_hci_cmd_le_cs_set_default_settings_return_t,
                   sdc_hci_cmd_le_cs_set_default_settings),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_READ_REMOTE_FAE_TABLE,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_read_remote_fae_table_t, 2U,
                  sdc_hci_cmd_le_cs_read_remote_fae_table),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_WRITE_CACHED_REMOTE_FAE_TABLE,
                   sdc_hci_cmd_le_cs_write_cached_remote_fae_table_t, 74U,
                   sdc_hci_cmd_le_cs_write_cached_remote_fae_table_return_t,
                   sdc_hci_cmd_le_cs_write_cached_remote_fae_table),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_CREATE_CONFIG,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_create_config_t, 28U,
                  sdc_hci_cmd_le_cs_create_config),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_REMOVE_CONFIG,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_remove_config_t, 3U,
                  sdc_hci_cmd_le_cs_remove_config),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_SET_CHANNEL_CLASSIFICATION,
                  HCI_CMD_RESPONSE_COMPLETE,
                  sdc_hci_cmd_le_cs_set_channel_classification_t, 10U,
                  sdc_hci_cmd_le_cs_set_channel_classification),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_SET_PROCEDURE_PARAMS,
                   sdc_hci_cmd_le_cs_set_procedure_params_t, 23U,
                   sdc_hci_cmd_le_cs_set_procedure_params_return_t,
                   sdc_hci_cmd_le_cs_set_procedure_params),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_PROCEDURE_ENABLE,
                  HCI_CMD_RESPONSE_STATUS,
                  sdc_hci_cmd_le_cs_procedure_enable_t, 4U,
                  sdc_hci_cmd_le_cs_procedure_enable),
    {SDC_HCI_OPCODE_CMD_LE_CS_TEST, HCI_CMD_VARIABLE_PARAM_LEN, 0U,
     HCI_CMD_RESPONSE_COMPLETE, HciSdcCmdLeCsTest},
    HCI_SDC_ROW_N(SDC_HCI_OPCODE_CMD_LE_CS_TEST_END,
                  HCI_CMD_RESPONSE_STATUS, 0U,
                  sdc_hci_cmd_le_cs_test_end),
    HCI_SDC_ROW_NR(SDC_HCI_OPCODE_CMD_LE_CS_READ_LOCAL_SUPPORTED_CAPABILITIES_V2, 0U,
                   sdc_hci_cmd_le_cs_read_local_supported_capabilities_v2_return_t,
                   sdc_hci_cmd_le_cs_read_local_supported_capabilities_v2),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_WRITE_CACHED_REMOTE_SUPPORTED_CAPABILITIES_V2,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities_v2_t, 36U,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities_v2_return_t,
                   sdc_hci_cmd_le_cs_write_cached_remote_supported_capabilities_v2),
    HCI_SDC_ROW_PR(SDC_HCI_OPCODE_CMD_LE_CS_SET_SECURITY_REQUIREMENTS,
                   sdc_hci_cmd_le_cs_set_security_requirements_t, 10U,
                   sdc_hci_cmd_le_cs_set_security_requirements_return_t,
                   sdc_hci_cmd_le_cs_set_security_requirements),
    HCI_SDC_ROW_P(SDC_HCI_OPCODE_CMD_LE_CS_SET_DEFAULT_SECURITY_REQUIREMENTS,
                  HCI_CMD_RESPONSE_COMPLETE,
                  sdc_hci_cmd_le_cs_set_default_security_requirements_t, 8U,
                  sdc_hci_cmd_le_cs_set_default_security_requirements),
};

extern "C" const HciCmdEntry_t *HciSdcFeatCsCommands(size_t *pCount)
{
    *pCount = sizeof(s_HciSdcCsCommands) / sizeof(s_HciSdcCsCommands[0]);
    return s_HciSdcCsCommands;
}
