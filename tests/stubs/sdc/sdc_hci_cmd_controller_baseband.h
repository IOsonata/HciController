#ifndef TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H
#define TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H

#include <stdint.h>

#define SDC_HCI_OPCODE_CMD_CB_READ_AUTOMATIC_FLUSH_TIMEOUT        0x0C27U
#define SDC_HCI_OPCODE_CMD_CB_WRITE_AUTOMATIC_FLUSH_TIMEOUT       0x0C28U
#define SDC_HCI_OPCODE_CMD_CB_SET_CONTROLLER_TO_HOST_FLOW_CONTROL 0x0C31U
#define SDC_HCI_OPCODE_CMD_CB_HOST_BUFFER_SIZE                     0x0C33U
#define SDC_HCI_OPCODE_CMD_CB_HOST_NUMBER_OF_COMPLETED_PACKETS     0x0C35U

#pragma pack(push, 1)
typedef struct {
    uint16_t conn_accept_timeout;
} sdc_hci_cmd_cb_read_conn_accept_timeout_return_t;

typedef struct {
    uint16_t conn_accept_timeout;
} sdc_hci_cmd_cb_write_conn_accept_timeout_t;

typedef struct {
    uint16_t conn_handle;
} sdc_hci_cmd_cb_read_automatic_flush_timeout_t;

typedef struct {
    uint16_t conn_handle;
    uint16_t flush_timeout;
} sdc_hci_cmd_cb_read_automatic_flush_timeout_return_t;

typedef struct {
    uint16_t conn_handle;
    uint16_t flush_timeout;
} sdc_hci_cmd_cb_write_automatic_flush_timeout_t;

typedef struct {
    uint16_t conn_handle;
} sdc_hci_cmd_cb_write_automatic_flush_timeout_return_t;

typedef struct {
    uint8_t flow_control_enable;
} sdc_hci_cmd_cb_set_controller_to_host_flow_control_t;

typedef struct {
    uint16_t host_acl_data_packet_length;
    uint8_t host_sync_data_packet_length;
    uint16_t host_total_num_acl_data_packets;
    uint16_t host_total_num_sync_data_packets;
} sdc_hci_cmd_cb_host_buffer_size_t;

typedef struct {
    uint16_t conn_handle;
    uint16_t host_num_completed_packets;
} sdc_hci_cb_host_number_of_completed_packets_array_params_t;

/*
 * Nordic's real C header uses a flexible array. The mock only passes this
 * object through a pointer, so one element avoids a C99-extension warning in
 * native C++ builds without changing the wire item layout used by the code.
 */
typedef struct {
    uint8_t num_handles;
    sdc_hci_cb_host_number_of_completed_packets_array_params_t array_params[1];
} sdc_hci_cmd_cb_host_number_of_completed_packets_t;
#pragma pack(pop)

static inline uint8_t sdc_hci_cmd_cb_read_conn_accept_timeout(
    sdc_hci_cmd_cb_read_conn_accept_timeout_return_t *pReturn)
{
    if (pReturn != 0)
    {
        pReturn->conn_accept_timeout = 0x1FA0U;
    }
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_write_conn_accept_timeout(
    const sdc_hci_cmd_cb_write_conn_accept_timeout_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_read_automatic_flush_timeout(
    const sdc_hci_cmd_cb_read_automatic_flush_timeout_t *pParams,
    sdc_hci_cmd_cb_read_automatic_flush_timeout_return_t *pReturn)
{
    if (pReturn != 0)
    {
        pReturn->conn_handle = pParams != 0 ? pParams->conn_handle : 0U;
        pReturn->flush_timeout = 0U;
    }
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_write_automatic_flush_timeout(
    const sdc_hci_cmd_cb_write_automatic_flush_timeout_t *pParams,
    sdc_hci_cmd_cb_write_automatic_flush_timeout_return_t *pReturn)
{
    if (pReturn != 0)
    {
        pReturn->conn_handle = pParams != 0 ? pParams->conn_handle : 0U;
    }
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_set_controller_to_host_flow_control(
    const sdc_hci_cmd_cb_set_controller_to_host_flow_control_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_host_buffer_size(
    const sdc_hci_cmd_cb_host_buffer_size_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_cb_host_number_of_completed_packets(
    const sdc_hci_cmd_cb_host_number_of_completed_packets_t *pParams)
{
    (void)pParams;
    return 0U;
}

#endif /* TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H */
