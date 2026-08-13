/* Test-only subset of Nordic sdc_hci_vs.h used by generic host tests. */
#ifndef TEST_SDC_HCI_VS_H__
#define TEST_SDC_HCI_VS_H__

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND = 0xFC1F,
    SDC_HCI_OPCODE_CMD_VS_CONN_EVENT_EXTEND = 0xFD03,
    SDC_HCI_OPCODE_CMD_VS_EVENT_LENGTH_SET = 0xFD05,
    SDC_HCI_OPCODE_CMD_VS_PERIODIC_ADV_EVENT_LENGTH_SET = 0xFD06,
    SDC_HCI_OPCODE_CMD_VS_PERIPHERAL_LATENCY_MODE_SET = 0xFD09,
    SDC_HCI_OPCODE_CMD_VS_WRITE_REMOTE_TX_POWER = 0xFD0A,
    SDC_HCI_OPCODE_CMD_VS_COMPAT_MODE_WINDOW_OFFSET_SET = 0xFD0D,
    SDC_HCI_OPCODE_CMD_VS_SET_POWER_CONTROL_REQUEST_PARAMS = 0xFD10,
    SDC_HCI_OPCODE_CMD_VS_CENTRAL_ACL_EVENT_SPACING_SET = 0xFD12,
    SDC_HCI_OPCODE_CMD_VS_ALLOW_PARALLEL_CONNECTION_ESTABLISHMENTS = 0xFD15,
    SDC_HCI_OPCODE_CMD_VS_MIN_VAL_OF_MAX_ACL_TX_PAYLOAD_SET = 0xFD16,
    SDC_HCI_OPCODE_CMD_VS_SCAN_CHANNEL_MAP_SET = 0xFD1B,
    SDC_HCI_OPCODE_CMD_VS_SCAN_ACCEPT_EXT_ADV_PACKETS_SET = 0xFD1C,
    SDC_HCI_OPCODE_CMD_VS_SET_ROLE_PRIORITY = 0xFD1D,
    SDC_HCI_OPCODE_CMD_VS_SET_EVENT_START_TASK = 0xFD1E,
    SDC_HCI_OPCODE_CMD_VS_ENABLE_PERIODIC_ADV_EVENT_COUNTER_REPORTS = 0xFD20,
};

enum
{
    SDC_HCI_VS_DTM_COMMAND_OPCODE_TEST_END = 0x00,
    SDC_HCI_VS_DTM_COMMAND_OPCODE_TRANSMITTER_CARRIER_TEST = 0x01,
};

#pragma pack(push, 1)
typedef struct { uint8_t sub_opcode; } sdc_hci_vs_dtm_command_header_t;
typedef struct
{
    sdc_hci_vs_dtm_command_header_t header;
    uint8_t tx_channel;
    int8_t tx_power_level;
} sdc_hci_cmd_vs_dtm_transmitter_carrier_test_t;
typedef struct { sdc_hci_vs_dtm_command_header_t header; }
    sdc_hci_cmd_vs_dtm_test_end_t;
typedef struct { uint16_t num_packets; }
    sdc_hci_cmd_vs_dtm_test_end_return_t;
typedef struct
{
    union
    {
        sdc_hci_vs_dtm_command_header_t header;
        sdc_hci_cmd_vs_dtm_transmitter_carrier_test_t transmitter_carrier_test;
        sdc_hci_cmd_vs_dtm_test_end_t test_end;
    } command_parameters;
} sdc_hci_cmd_vs_dtm_command_t;

typedef struct { uint8_t enable; } sdc_hci_cmd_vs_conn_event_extend_t;
typedef struct { uint32_t event_length_us; } sdc_hci_cmd_vs_event_length_set_t;
typedef struct { uint32_t event_length_us; }
    sdc_hci_cmd_vs_periodic_adv_event_length_set_t;
typedef struct { uint16_t conn_handle; uint8_t mode; }
    sdc_hci_cmd_vs_peripheral_latency_mode_set_t;
typedef struct { uint16_t conn_handle; uint8_t phy; int8_t delta; }
    sdc_hci_cmd_vs_write_remote_tx_power_t;
typedef struct { uint8_t enable; }
    sdc_hci_cmd_vs_compat_mode_window_offset_set_t;
typedef struct
{
    uint8_t auto_enable;
    uint8_t apr_enable;
    uint16_t beta;
    int8_t lower_limit;
    int8_t upper_limit;
    int8_t lower_target_rssi;
    int8_t upper_target_rssi;
    uint16_t wait_period_ms;
    uint8_t apr_margin;
} sdc_hci_cmd_vs_set_power_control_request_params_t;
typedef struct { uint32_t central_acl_event_spacing_us; }
    sdc_hci_cmd_vs_central_acl_event_spacing_set_t;
typedef struct { uint8_t enable; }
    sdc_hci_cmd_vs_allow_parallel_connection_establishments_t;
typedef struct { uint8_t min_val_of_max_acl_tx_payload; }
    sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set_t;
typedef struct { uint8_t channel_map[5]; }
    sdc_hci_cmd_vs_scan_channel_map_set_t;
typedef struct { uint8_t accept_ext_adv_packets; }
    sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set_t;
typedef struct { uint8_t handle_type; uint16_t handle; uint8_t priority; }
    sdc_hci_cmd_vs_set_role_priority_t;
typedef struct { uint8_t handle_type; uint16_t handle; uint32_t task_address; }
    sdc_hci_cmd_vs_set_event_start_task_t;
typedef struct { uint8_t enable; }
    sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports_t;
#pragma pack(pop)

#define TEST_SDC_VS_NO_RETURN(Name, Type) \
    static inline uint8_t Name(const Type *p_params) \
    { \
        (void)p_params; \
        return 0U; \
    }

TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_conn_event_extend,
                      sdc_hci_cmd_vs_conn_event_extend_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_event_length_set,
                      sdc_hci_cmd_vs_event_length_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_periodic_adv_event_length_set,
                      sdc_hci_cmd_vs_periodic_adv_event_length_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_peripheral_latency_mode_set,
                      sdc_hci_cmd_vs_peripheral_latency_mode_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_write_remote_tx_power,
                      sdc_hci_cmd_vs_write_remote_tx_power_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_compat_mode_window_offset_set,
                      sdc_hci_cmd_vs_compat_mode_window_offset_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_set_power_control_request_params,
                      sdc_hci_cmd_vs_set_power_control_request_params_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_central_acl_event_spacing_set,
                      sdc_hci_cmd_vs_central_acl_event_spacing_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_allow_parallel_connection_establishments,
                      sdc_hci_cmd_vs_allow_parallel_connection_establishments_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set,
                      sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_scan_channel_map_set,
                      sdc_hci_cmd_vs_scan_channel_map_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set,
                      sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_set_role_priority,
                      sdc_hci_cmd_vs_set_role_priority_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_set_event_start_task,
                      sdc_hci_cmd_vs_set_event_start_task_t)
TEST_SDC_VS_NO_RETURN(sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports,
                      sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports_t)

#undef TEST_SDC_VS_NO_RETURN

static inline uint8_t sdc_hci_cmd_vs_dtm_command(
    const sdc_hci_cmd_vs_dtm_command_t *p_params,
    void *p_return_params,
    uint8_t *p_return_params_length_out)
{
    if (p_return_params_length_out != 0)
    {
        *p_return_params_length_out = 0U;
    }

    if (p_params != 0 &&
        p_params->command_parameters.header.sub_opcode ==
            SDC_HCI_VS_DTM_COMMAND_OPCODE_TEST_END)
    {
        if (p_return_params != 0)
        {
            sdc_hci_cmd_vs_dtm_test_end_return_t value = {0x5A5AU};
            memcpy(p_return_params, &value, sizeof(value));
        }
        if (p_return_params_length_out != 0)
        {
            *p_return_params_length_out =
                (uint8_t)sizeof(sdc_hci_cmd_vs_dtm_test_end_return_t);
        }
    }

    return 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* TEST_SDC_HCI_VS_H__ */
