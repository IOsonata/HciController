#ifndef TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H
#define TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H

#include <stdint.h>

typedef struct {
    uint16_t conn_accept_timeout;
} sdc_hci_cmd_cb_read_conn_accept_timeout_return_t;

typedef struct {
    uint16_t conn_accept_timeout;
} sdc_hci_cmd_cb_write_conn_accept_timeout_t;

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

#endif /* TEST_SDC_HCI_CMD_CONTROLLER_BASEBAND_H */
