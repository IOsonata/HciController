#ifndef TEST_SDC_HCI_CMD_LE_H
#define TEST_SDC_HCI_CMD_LE_H

#include <stdint.h>

typedef struct {
    uint16_t conn_handle;
} sdc_hci_cmd_le_request_peer_sca_t;

static inline uint8_t sdc_hci_cmd_le_request_peer_sca(
    const sdc_hci_cmd_le_request_peer_sca_t *pParams)
{
    (void)pParams;
    return 0U;
}

#endif /* TEST_SDC_HCI_CMD_LE_H */
