/* Test-only SDC entry points added separately from the generated stub. */
#include <stdint.h>
#include <string.h>

#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_stub.h"

uint8_t sdc_hci_cmd_cb_set_event_mask_page_2(
    const sdc_hci_cmd_cb_set_event_mask_page_2_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_set_event_mask_page_2";
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
