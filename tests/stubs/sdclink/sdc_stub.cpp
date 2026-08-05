/* Generated from the SDC headers. Records calls and succeeds. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sdc_hci.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_link_control.h"
#include "sdc_hci_cmd_info_params.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_stub.h"

SdcStubState_t g_SdcStub;

uint8_t sdc_hci_cmd_cb_read_authenticated_payload_timeout(const sdc_hci_cmd_cb_read_authenticated_payload_timeout_t * a0, sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_read_authenticated_payload_timeout";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_cb_reset(void)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_reset";
    g_SdcStub.Calls++;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_cb_set_event_mask(const sdc_hci_cmd_cb_set_event_mask_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_set_event_mask";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_cb_write_authenticated_payload_timeout(const sdc_hci_cmd_cb_write_authenticated_payload_timeout_t * a0, sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_write_authenticated_payload_timeout";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_ip_read_bd_addr(sdc_hci_cmd_ip_read_bd_addr_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_ip_read_bd_addr";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_ip_read_bd_addr_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_ip_read_local_supported_features(sdc_hci_cmd_ip_read_local_supported_features_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_ip_read_local_supported_features";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_ip_read_local_supported_features_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_ip_read_local_version_information(sdc_hci_cmd_ip_read_local_version_information_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_ip_read_local_version_information";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_ip_read_local_version_information_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_lc_disconnect(const sdc_hci_cmd_lc_disconnect_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_lc_disconnect";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_lc_read_remote_version_information(const sdc_hci_cmd_lc_read_remote_version_information_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_lc_read_remote_version_information";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_add_device_to_filter_accept_list(const sdc_hci_cmd_le_add_device_to_filter_accept_list_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_add_device_to_filter_accept_list";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_clear_adv_sets(void)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_clear_adv_sets";
    g_SdcStub.Calls++;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_clear_filter_accept_list(void)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_clear_filter_accept_list";
    g_SdcStub.Calls++;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_conn_update(const sdc_hci_cmd_le_conn_update_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_conn_update";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_create_conn(const sdc_hci_cmd_le_create_conn_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_create_conn";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_create_conn_cancel(void)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_create_conn_cancel";
    g_SdcStub.Calls++;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_enable_encryption(const sdc_hci_cmd_le_enable_encryption_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_enable_encryption";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_encrypt(const sdc_hci_cmd_le_encrypt_t * a0, sdc_hci_cmd_le_encrypt_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_encrypt";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_encrypt_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_ext_create_conn(const sdc_hci_cmd_le_ext_create_conn_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_ext_create_conn";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_long_term_key_request_negative_reply(const sdc_hci_cmd_le_long_term_key_request_negative_reply_t * a0, sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_long_term_key_request_negative_reply";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_long_term_key_request_reply(const sdc_hci_cmd_le_long_term_key_request_reply_t * a0, sdc_hci_cmd_le_long_term_key_request_reply_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_long_term_key_request_reply";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_long_term_key_request_reply_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_rand(sdc_hci_cmd_le_rand_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_rand";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_rand_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_adv_physical_channel_tx_power(sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_adv_physical_channel_tx_power";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_adv_physical_channel_tx_power_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_buffer_size(sdc_hci_cmd_le_read_buffer_size_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_buffer_size";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_buffer_size_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_channel_map(const sdc_hci_cmd_le_read_channel_map_t * a0, sdc_hci_cmd_le_read_channel_map_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_channel_map";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_read_channel_map_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_filter_accept_list_size(sdc_hci_cmd_le_read_filter_accept_list_size_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_filter_accept_list_size";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_filter_accept_list_size_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_local_supported_features(sdc_hci_cmd_le_read_local_supported_features_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_local_supported_features";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_local_supported_features_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_max_adv_data_length(sdc_hci_cmd_le_read_max_adv_data_length_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_max_adv_data_length";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_max_adv_data_length_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_max_data_length(sdc_hci_cmd_le_read_max_data_length_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_max_data_length";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_max_data_length_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_number_of_supported_adv_sets(sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_number_of_supported_adv_sets";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_number_of_supported_adv_sets_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_phy(const sdc_hci_cmd_le_read_phy_t * a0, sdc_hci_cmd_le_read_phy_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_phy";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_read_phy_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_remote_features(const sdc_hci_cmd_le_read_remote_features_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_remote_features";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_suggested_default_data_length(sdc_hci_cmd_le_read_suggested_default_data_length_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_suggested_default_data_length";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_suggested_default_data_length_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_supported_states(sdc_hci_cmd_le_read_supported_states_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_supported_states";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_supported_states_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_read_transmit_power(sdc_hci_cmd_le_read_transmit_power_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_read_transmit_power";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_read_transmit_power_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_receiver_test_v1(const sdc_hci_cmd_le_receiver_test_v1_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_receiver_test_v1";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_remove_adv_set(const sdc_hci_cmd_le_remove_adv_set_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_remove_adv_set";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_remove_device_from_filter_accept_list(const sdc_hci_cmd_le_remove_device_from_filter_accept_list_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_remove_device_from_filter_accept_list";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_adv_data(const sdc_hci_cmd_le_set_adv_data_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_adv_data";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_adv_enable(const sdc_hci_cmd_le_set_adv_enable_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_adv_enable";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_adv_params(const sdc_hci_cmd_le_set_adv_params_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_adv_params";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_adv_set_random_address(const sdc_hci_cmd_le_set_adv_set_random_address_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_adv_set_random_address";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_data_length(const sdc_hci_cmd_le_set_data_length_t * a0, sdc_hci_cmd_le_set_data_length_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_data_length";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_set_data_length_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_default_phy(const sdc_hci_cmd_le_set_default_phy_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_default_phy";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_event_mask(const sdc_hci_cmd_le_set_event_mask_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_event_mask";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_adv_data(const sdc_hci_cmd_le_set_ext_adv_data_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_adv_data";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_adv_enable(const sdc_hci_cmd_le_set_ext_adv_enable_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_adv_enable";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_adv_params(const sdc_hci_cmd_le_set_ext_adv_params_t * a0, sdc_hci_cmd_le_set_ext_adv_params_return_t * a1)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_adv_params";
    g_SdcStub.Calls++;
    (void)a0;
    if (a1 != NULL) { memset(a1, 0x5A, sizeof(sdc_hci_cmd_le_set_ext_adv_params_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_scan_enable(const sdc_hci_cmd_le_set_ext_scan_enable_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_scan_enable";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_scan_params(const sdc_hci_cmd_le_set_ext_scan_params_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_scan_params";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_ext_scan_response_data(const sdc_hci_cmd_le_set_ext_scan_response_data_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_ext_scan_response_data";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_phy(const sdc_hci_cmd_le_set_phy_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_phy";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_random_address(const sdc_hci_cmd_le_set_random_address_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_random_address";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_scan_enable(const sdc_hci_cmd_le_set_scan_enable_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_scan_enable";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_scan_params(const sdc_hci_cmd_le_set_scan_params_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_scan_params";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_set_scan_response_data(const sdc_hci_cmd_le_set_scan_response_data_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_set_scan_response_data";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_test_end(sdc_hci_cmd_le_test_end_return_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_test_end";
    g_SdcStub.Calls++;
    if (a0 != NULL) { memset(a0, 0x5A, sizeof(sdc_hci_cmd_le_test_end_return_t)); }
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_transmitter_test_v1(const sdc_hci_cmd_le_transmitter_test_v1_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_transmitter_test_v1";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}

uint8_t sdc_hci_cmd_le_write_suggested_default_data_length(const sdc_hci_cmd_le_write_suggested_default_data_length_t * a0)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_le_write_suggested_default_data_length";
    g_SdcStub.Calls++;
    (void)a0;
    return g_SdcStub.NextStatus;
}


int32_t sdc_hci_data_put(uint8_t const *p_data_in)
{
    (void)p_data_in;
    g_SdcStub.LastCall = "sdc_hci_data_put";
    return 0;
}

int32_t sdc_hci_iso_data_put(uint8_t const *p_data_in)
{
    (void)p_data_in;
    g_SdcStub.LastCall = "sdc_hci_iso_data_put";
    return 0;
}

int32_t sdc_hci_get(uint8_t *p_packet_out, uint8_t *p_msg_type_out)
{
    (void)p_packet_out;
    if (p_msg_type_out != NULL)
    {
        *p_msg_type_out = 0;
    }
    return -NRF_EAGAIN;
}
