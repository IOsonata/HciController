/*
 * Enough of the SoftDevice Controller HCI transport for a host build.
 *
 * The sizes are copied from softdevice_controller/include/sdc_hci.h. The
 * application asserts its packet buffer against HCI_MSG_BUFFER_MAX_SIZE, so
 * that number has to be here for src/hci_app.cpp to compile at all off
 * target.
 *
 * A copy can drift. The dispatch test builds against the vendor headers, so a
 * size that moved shows up there.
 */

#ifndef SDC_HCI_H__
#define SDC_HCI_H__

#include <stdint.h>

#define HCI_CMD_HEADER_SIZE      (3)
#define HCI_DATA_HEADER_SIZE     (4)
#define HCI_ISO_DATA_HEADER_SIZE (12)
#define HCI_EVENT_HEADER_SIZE    (2)

#define HCI_CMD_MAX_SIZE      (255)
#define HCI_DATA_MAX_SIZE     (251)
#define HCI_ISO_DATA_MAX_SIZE (4095)
#define HCI_EVENT_MAX_SIZE    (255)

#define HCI_CMD_PACKET_MAX_SIZE      (HCI_CMD_MAX_SIZE + HCI_CMD_HEADER_SIZE)
#define HCI_DATA_PACKET_MAX_SIZE     (HCI_DATA_MAX_SIZE + HCI_DATA_HEADER_SIZE)
#define HCI_ISO_DATA_PACKET_MAX_SIZE                                          \
    (HCI_ISO_DATA_MAX_SIZE + HCI_ISO_DATA_HEADER_SIZE)
#define HCI_EVENT_PACKET_MAX_SIZE (HCI_EVENT_MAX_SIZE + HCI_EVENT_HEADER_SIZE)

#define HCI_MSG_BUFFER_MAX_SIZE     HCI_CMD_PACKET_MAX_SIZE
#define HCI_MSG_BUFFER_ISO_MAX_SIZE HCI_ISO_DATA_PACKET_MAX_SIZE

#ifdef __cplusplus
extern "C" {
#endif

int32_t sdc_hci_cmd_put(uint8_t const *p_cmd_in);
int32_t sdc_hci_data_put(uint8_t const *p_data_in);
int32_t sdc_hci_get(uint8_t *p_packet_out, uint8_t *p_msg_type_out);

#ifdef __cplusplus
}
#endif

#endif /* SDC_HCI_H__ */
