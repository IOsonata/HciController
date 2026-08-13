#ifndef TEST_SDC_HCI_CMD_LE_H
#define TEST_SDC_HCI_CMD_LE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define TEST_SDC_PACKED __attribute__((packed, aligned(1)))
#else
#define TEST_SDC_PACKED
#endif

typedef struct TEST_SDC_PACKED {
    uint8_t raw[9];
} sdc_hci_cmd_le_frame_space_update_t;

typedef struct TEST_SDC_PACKED {
    uint8_t raw[20];
} sdc_hci_cmd_le_conn_rate_request_t;

typedef struct TEST_SDC_PACKED {
    uint8_t raw[18];
} sdc_hci_cmd_le_set_default_rate_params_t;

typedef struct TEST_SDC_PACKED {
    uint16_t group_min;
    uint16_t group_max;
    uint16_t group_stride;
} sdc_hci_le_read_min_supported_conn_interval_group_t;

typedef struct TEST_SDC_PACKED {
    uint8_t min_supported_conn_interval;
    uint8_t num_groups;
    sdc_hci_le_read_min_supported_conn_interval_group_t groups[41];
} sdc_hci_cmd_le_read_min_supported_conn_interval_return_t;

static inline uint8_t sdc_hci_cmd_le_frame_space_update(
    const sdc_hci_cmd_le_frame_space_update_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_le_conn_rate_request(
    const sdc_hci_cmd_le_conn_rate_request_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_le_set_default_rate_params(
    const sdc_hci_cmd_le_set_default_rate_params_t *pParams)
{
    (void)pParams;
    return 0U;
}

static inline uint8_t sdc_hci_cmd_le_read_min_supported_conn_interval(
    sdc_hci_cmd_le_read_min_supported_conn_interval_return_t *pReturn)
{
    pReturn->min_supported_conn_interval = 6U;
    pReturn->num_groups = 1U;
    pReturn->groups[0].group_min = 6U;
    pReturn->groups[0].group_max = 24U;
    pReturn->groups[0].group_stride = 1U;
    return 0U;
}

#undef TEST_SDC_PACKED

#ifdef __cplusplus
}
#endif

#endif /* TEST_SDC_HCI_CMD_LE_H */
