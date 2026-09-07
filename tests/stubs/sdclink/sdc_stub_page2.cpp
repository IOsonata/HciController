/* Test-only SDC entry points added separately from the generated stub. */
#include <stdint.h>
#include <string.h>

#include "sdc_hci.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_vs.h"
#include "sdc_stub.h"

/*
 * sdc_stub.h renames these two generated definitions so this file can provide
 * their externally visible wrappers. Undo the source-level aliases here before
 * declaring the wrappers themselves.
 */
#undef sdc_hci_cmd_le_set_periodic_adv_response_data
#undef sdc_hci_get

uint8_t sdc_stub_hci_cmd_le_set_periodic_adv_response_data(
    const sdc_hci_cmd_le_set_periodic_adv_response_data_t *pParams,
    sdc_hci_cmd_le_set_periodic_adv_response_data_return_t *pReturn);
int32_t sdc_stub_hci_get(uint8_t *pPacketOut, uint8_t *pMsgTypeOut);

#if !defined(SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY) || \
    !SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY
static bool s_PawrResponseCompletePending;
static uint8_t s_PawrResponseStatus;
static uint8_t s_PawrResponseReturn[
    sizeof(sdc_hci_cmd_le_set_periodic_adv_response_data_return_t)];
#endif

uint8_t sdc_hci_cmd_le_set_periodic_adv_response_data(
    const sdc_hci_cmd_le_set_periodic_adv_response_data_t *pParams,
    sdc_hci_cmd_le_set_periodic_adv_response_data_return_t *pReturn)
{
    const uint8_t status =
        sdc_stub_hci_cmd_le_set_periodic_adv_response_data(pParams, pReturn);

#if !defined(SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY) || \
    !SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY
    /*
     * Older SDC revisions complete implemented 0x2083 commands later through
     * sdc_hci_get(). Unknown HCI Command is immediate because no delayed event
     * follows it. Current SDC revisions define
     * SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY and must not queue this extra event.
     */
    if (status != 0x01U)
    {
        s_PawrResponseStatus = status;
        if (pReturn != NULL)
        {
            memcpy(s_PawrResponseReturn, pReturn,
                   sizeof(s_PawrResponseReturn));
        }
        else
        {
            memset(s_PawrResponseReturn, 0, sizeof(s_PawrResponseReturn));
        }
        s_PawrResponseCompletePending = true;
    }
#endif

    return status;
}

int32_t sdc_hci_get(uint8_t *pPacketOut, uint8_t *pMsgTypeOut)
{
#if !defined(SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY) || \
    !SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY
    if (s_PawrResponseCompletePending)
    {
        if (pPacketOut == NULL || pMsgTypeOut == NULL)
        {
            return sdc_stub_hci_get(pPacketOut, pMsgTypeOut);
        }

        /* Command Complete: Num_HCI_Command_Packets, opcode, status, sync handle. */
        pPacketOut[0] = 0x0EU;
        pPacketOut[1] =
            (uint8_t)(4U + sizeof(sdc_hci_cmd_le_set_periodic_adv_response_data_return_t));
        pPacketOut[2] = 0x01U;
        pPacketOut[3] = 0x83U;
        pPacketOut[4] = 0x20U;
        pPacketOut[5] = s_PawrResponseStatus;
        memcpy(&pPacketOut[6], s_PawrResponseReturn,
               sizeof(s_PawrResponseReturn));
        *pMsgTypeOut = SDC_HCI_MSG_TYPE_EVT;
        s_PawrResponseCompletePending = false;
        return 0;
    }
#endif

    return sdc_stub_hci_get(pPacketOut, pMsgTypeOut);
}

uint8_t sdc_hci_cmd_cb_set_event_mask_page_2(
    const sdc_hci_cmd_cb_set_event_mask_page_2_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_set_event_mask_page_2";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_cb_read_conn_accept_timeout(
    sdc_hci_cmd_cb_read_conn_accept_timeout_return_t *pReturn)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_read_conn_accept_timeout";
    g_SdcStub.Calls++;
    if (pReturn != NULL)
    {
        memset(pReturn, 0x5A, sizeof(*pReturn));
    }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_cb_write_conn_accept_timeout(
    const sdc_hci_cmd_cb_write_conn_accept_timeout_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_write_conn_accept_timeout";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_all_local_supported_features(
    sdc_hci_cmd_le_read_all_local_supported_features_return_t *pReturn)
{
    g_SdcStub.LastCall =
        "sdc_hci_cmd_le_read_all_local_supported_features";
    g_SdcStub.Calls++;
    if (pReturn != NULL)
    {
        memset(pReturn, 0x5A, sizeof(*pReturn));
    }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_host_feature_v2(
    const sdc_hci_cmd_le_set_host_feature_v2_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_host_feature_v2";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_adv_params_v2(
    const sdc_hci_cmd_le_set_ext_adv_params_v2_t *pParams,
    sdc_hci_cmd_le_set_ext_adv_params_v2_return_t *pReturn)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_adv_params_v2";
    g_SdcStub.Calls++;
    (void)pParams;
    if (pReturn != NULL)
    {
        memset(pReturn, 0x5A, sizeof(*pReturn));
    }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_ext_create_conn_v2(
    const sdc_hci_cmd_le_ext_create_conn_v2_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_ext_create_conn_v2";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_frame_space_update(
    const sdc_hci_cmd_le_frame_space_update_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_frame_space_update";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_conn_rate_request(
    const sdc_hci_cmd_le_conn_rate_request_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_conn_rate_request";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_default_rate_params(
    const sdc_hci_cmd_le_set_default_rate_params_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_default_rate_params";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_min_supported_conn_interval(
    sdc_hci_cmd_le_read_min_supported_conn_interval_return_t *pReturn)
{
    g_SdcStub.LastCall =
        "sdc_hci_cmd_le_read_min_supported_conn_interval";
    g_SdcStub.Calls++;
    if (pReturn != NULL)
    {
        pReturn->min_supported_conn_interval = 6U;
        pReturn->num_groups = 1U;
        pReturn->groups[0].group_min = 6U;
        pReturn->groups[0].group_max = 24U;
        pReturn->groups[0].group_stride = 1U;
    }
    return g_SdcStub.NextStatus;
}

#define SDC_STUB_VS_NO_RETURN(Name, Type) \
    uint8_t Name(const Type *pParams) \
    { \
        g_SdcStub.LastCall = #Name; \
        g_SdcStub.Calls++; \
        (void)pParams; \
        return g_SdcStub.NextStatus; \
    }

SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_conn_event_extend,
                      sdc_hci_cmd_vs_conn_event_extend_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_event_length_set,
                      sdc_hci_cmd_vs_event_length_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_periodic_adv_event_length_set,
                      sdc_hci_cmd_vs_periodic_adv_event_length_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_peripheral_latency_mode_set,
                      sdc_hci_cmd_vs_peripheral_latency_mode_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_write_remote_tx_power,
                      sdc_hci_cmd_vs_write_remote_tx_power_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_compat_mode_window_offset_set,
                      sdc_hci_cmd_vs_compat_mode_window_offset_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_set_power_control_request_params,
                      sdc_hci_cmd_vs_set_power_control_request_params_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_central_acl_event_spacing_set,
                      sdc_hci_cmd_vs_central_acl_event_spacing_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_allow_parallel_connection_establishments,
                      sdc_hci_cmd_vs_allow_parallel_connection_establishments_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set,
                      sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_scan_channel_map_set,
                      sdc_hci_cmd_vs_scan_channel_map_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set,
                      sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_set_role_priority,
                      sdc_hci_cmd_vs_set_role_priority_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_set_event_start_task,
                      sdc_hci_cmd_vs_set_event_start_task_t)
SDC_STUB_VS_NO_RETURN(sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports,
                      sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports_t)

#undef SDC_STUB_VS_NO_RETURN

uint8_t sdc_hci_cmd_vs_dtm_command(
    const sdc_hci_cmd_vs_dtm_command_t *pParams,
    void *pReturn,
    uint8_t *pReturnLen)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_vs_dtm_command";
    g_SdcStub.Calls++;

    if (pReturnLen != NULL)
    {
        *pReturnLen = 0U;
    }

    if (pParams != NULL &&
        pParams->command_parameters.header.sub_opcode ==
            SDC_HCI_VS_DTM_COMMAND_OPCODE_TEST_END)
    {
        if (pReturn != NULL)
        {
            memset(pReturn, 0x5A,
                   sizeof(sdc_hci_cmd_vs_dtm_test_end_return_t));
        }
        if (pReturnLen != NULL)
        {
            *pReturnLen =
                (uint8_t)sizeof(sdc_hci_cmd_vs_dtm_test_end_return_t);
        }
    }

    return g_SdcStub.NextStatus;
}
