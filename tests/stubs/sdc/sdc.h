#ifndef STUB_SDC_H
#define STUB_SDC_H
#include <stdint.h>
#define SDC_DEFAULT_RESOURCE_CFG_TAG 0
#define SDC_CFG_TYPE_NONE            0
#define SDC_CFG_TYPE_BUFFER_CFG      1
#define SDC_CFG_TYPE_PERIPHERAL_COUNT 2
#define SDC_CFG_TYPE_CENTRAL_COUNT   3
#define SDC_CFG_TYPE_ADV_COUNT       4
#define SDC_CFG_TYPE_ADV_BUFFER_CFG  5
#define SDC_CFG_TYPE_SCAN_BUFFER_CFG 6
/* Real sdc.h numbers these differently; only distinctness matters here. */
#define SDC_CFG_TYPE_FAL_SIZE        7
#define SDC_CFG_TYPE_EXTENDED_FEATURE_PAGE_COUNT 8
#define SDC_CFG_TYPE_PERIODIC_ADV_COUNT          9
#define SDC_CFG_TYPE_PERIODIC_SYNC_COUNT        10
#define SDC_CFG_TYPE_PERIODIC_SYNC_BUFFER_CFG   11
#define SDC_CFG_TYPE_PERIODIC_ADV_LIST_SIZE     12
#define SDC_CFG_TYPE_PERIODIC_ADV_RSP_COUNT     13
#define SDC_CFG_TYPE_PERIODIC_ADV_RSP_BUFFER_CFG 14
#define SDC_CFG_TYPE_PERIODIC_ADV_RSP_FAILURE_REPORTING_CFG 15
#define SDC_CFG_TYPE_PERIODIC_SYNC_RSP_TX_BUFFER_CFG 16
typedef struct { uint8_t count; } sdc_cfg_role_count_t;
typedef struct { uint8_t rx_packet_size; uint8_t tx_packet_size;
                 uint8_t rx_packet_count; uint8_t tx_packet_count; } sdc_cfg_buffer_cfg_t;
typedef struct { uint16_t max_adv_data; } sdc_cfg_adv_buffer_cfg_t;
typedef struct { uint8_t count; } sdc_cfg_scan_buffer_cfg_t;
typedef struct { uint8_t tx_buffer_count; uint8_t max_tx_data_size;
                 uint8_t rx_buffer_count; } sdc_cfg_periodic_adv_rsp_buffer_cfg_t;
typedef union {
    sdc_cfg_buffer_cfg_t buffer_cfg;
    sdc_cfg_role_count_t peripheral_count;
    sdc_cfg_role_count_t central_count;
    sdc_cfg_role_count_t adv_count;
    sdc_cfg_adv_buffer_cfg_t adv_buffer_cfg;
    sdc_cfg_scan_buffer_cfg_t scan_buffer_cfg;
    /* A bare integer in the real header, not a role count. */
    uint16_t fal_size;
    uint8_t extended_feature_page_count;
    sdc_cfg_role_count_t periodic_adv_count;
    sdc_cfg_role_count_t periodic_sync_count;
    sdc_cfg_scan_buffer_cfg_t periodic_sync_buffer_cfg;
    uint8_t periodic_adv_list_size;
    sdc_cfg_role_count_t periodic_adv_rsp_count;
    sdc_cfg_periodic_adv_rsp_buffer_cfg_t periodic_adv_rsp_buffer_cfg;
    uint8_t periodic_adv_rsp_failure_reporting_cfg;
    sdc_cfg_scan_buffer_cfg_t periodic_sync_rsp_tx_buffer_cfg;
} sdc_cfg_t;
typedef void (*sdc_fault_handler_t)(const char *, uint32_t);
typedef void (*sdc_callback_t)(void);
typedef struct { void (*rand_poll)(uint8_t *, uint8_t); } sdc_rand_source_t;
#ifdef __cplusplus
extern "C" {
#endif
int32_t sdc_init(sdc_fault_handler_t);
int32_t sdc_rand_source_register(const sdc_rand_source_t *);
int32_t sdc_cfg_set(uint8_t, uint8_t, const sdc_cfg_t *);
int32_t sdc_enable(sdc_callback_t, uint8_t *);
int32_t sdc_disable(void);
void sdc_support_ext_adv(void);
void sdc_support_peripheral(void);
void sdc_support_ext_central(void);
void sdc_support_le_2m_phy(void);
void sdc_support_le_coded_phy(void);
void sdc_support_dle_peripheral(void);
void sdc_support_dle_central(void);
void sdc_support_phy_update_peripheral(void);
void sdc_support_phy_update_central(void);
void sdc_support_direct_test_mode(void);
void sdc_support_le_privacy(void);
void sdc_support_qos_channel_survey(void);
void sdc_support_le_power_control_central(void);
void sdc_support_le_power_control_peripheral(void);
void sdc_support_le_path_loss_monitoring(void);
void sdc_support_sca_central(void);
void sdc_support_sca_peripheral(void);
void sdc_support_connection_subrating_central(void);
void sdc_support_connection_subrating_peripheral(void);
void sdc_support_extended_feature_set_central(void);
void sdc_support_extended_feature_set_peripheral(void);
void sdc_support_parallel_scanning_and_initiating(void);
void sdc_support_le_periodic_adv(void);
void sdc_support_le_periodic_sync(void);
void sdc_support_periodic_adv_sync_transfer_sender_central(void);
void sdc_support_periodic_adv_sync_transfer_sender_peripheral(void);
void sdc_support_periodic_adv_sync_transfer_receiver_central(void);
void sdc_support_periodic_adv_sync_transfer_receiver_peripheral(void);
void sdc_support_le_periodic_adv_with_rsp(void);
void sdc_support_le_periodic_sync_with_rsp(void);
#ifdef __cplusplus
}
#endif

/*
 * Memory requirement macros, copied verbatim from the real
 * softdevice_controller/include/sdc.h so the pool in hci_nrf52840.h is
 * computed in the host build too.
 *
 * These are copies, so they can drift. What catches that is the pair of
 * hci_nrf52840_usb_test, which builds against this file, and
 * hci_nrf52840_resources_test, which builds against the vendor one. Both are
 * measured against unit/hci_nrf52840_expected_resources.h, so a copy that no
 * longer matches fails here while the real header still passes, and which of
 * the two failed says which side moved.
 *
 * An earlier version of this comment credited the dispatch test with that.
 * It never did: the dispatch test does not include hci_nrf52840.h and has no
 * opinion about memory.
 */
#define SDC_DEFAULT_EXTENDED_FEATURE_PAGE_COUNT 10
#define SDC_DEFAULT_TX_PACKET_SIZE 27
#define SDC_DEFAULT_RX_PACKET_SIZE 27

#define __MEM_MINIMAL_CENTRAL_LINK_SIZE    795
#define __MEM_MINIMAL_PERIPHERAL_LINK_SIZE 891
#define __MEM_TX_BUFFER_OVERHEAD_SIZE 15
#define __MEM_RX_BUFFER_OVERHEAD_SIZE 15

#define __MEM_ADDITIONAL_LINK_SIZE(tx_size, rx_size, tx_count, rx_count) \
    ((tx_count) * ((tx_size) + __MEM_TX_BUFFER_OVERHEAD_SIZE) - \
     (SDC_DEFAULT_TX_PACKET_SIZE + __MEM_TX_BUFFER_OVERHEAD_SIZE) + \
     (rx_count) * ((rx_size) + __MEM_RX_BUFFER_OVERHEAD_SIZE) - \
     (SDC_DEFAULT_RX_PACKET_SIZE + __MEM_RX_BUFFER_OVERHEAD_SIZE))

/** @brief Maximum memory required per central link.
 *
 * @param[in] tx_size Link Layer TX packet size.
 * @param[in] rx_size Link Layer RX packet size.
 * @param[in] tx_count Link Layer TX packet count.
 * @param[in] rx_count Link Layer RX packet count.
 */
#define SDC_MEM_PER_CENTRAL_LINK(tx_size, rx_size, tx_count, rx_count) \
    (__MEM_MINIMAL_CENTRAL_LINK_SIZE + \
     __MEM_ADDITIONAL_LINK_SIZE(tx_size, rx_size, tx_count, rx_count))

/** @brief Maximum memory required per peripheral link.
 *
 * @param[in] tx_size Link Layer TX packet size.
 * @param[in] rx_size Link Layer RX packet size.
 * @param[in] tx_count Link Layer TX packet count.
 * @param[in] rx_count Link Layer RX packet count.
 */
#define SDC_MEM_PER_PERIPHERAL_LINK(tx_size, rx_size, tx_count, rx_count) \
    (__MEM_MINIMAL_PERIPHERAL_LINK_SIZE + \
     __MEM_ADDITIONAL_LINK_SIZE(tx_size, rx_size, tx_count, rx_count))

/** Maximum shared memory required for central links. */
#define SDC_MEM_CENTRAL_LINKS_SHARED 21

/** Maximum shared memory required for peripheral links. */
#define SDC_MEM_PERIPHERAL_LINKS_SHARED  17

/** @brief Maximum memory required when supporting LE Power Control.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 */
#define SDC_MEM_LE_POWER_CONTROL(num_links) ((num_links) > 0 ? (13 + (num_links) * 123) : 0)

/** @brief Maximum memory required when supporting subrating.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 */
#define SDC_MEM_SUBRATING(num_links) ((num_links) > 0 ? (12 + (num_links) * 60) : 0)

/** @brief Maximum memory required when supporting periodic advertising sync transfer.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 */
#define SDC_MEM_SYNC_TRANSFER(num_links) ((num_links) > 0 ? (13 + (num_links) * 139) : 0)

/** @brief Auxiliary defines, not to be used outside of this file. */
#define __MEM_PER_EXTENDED_FEATURE_PAGE 24

/** @brief Maximum memory required when supporting extended feature set.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 * @param[in] num_pages Total number of extended feature pages supported.
 */
#define SDC_MEM_EXTENDED_FEATURE_SET(num_links, num_pages) \
     ((num_links) > 0 ? (11 + (num_links) * (19 + (num_pages) * __MEM_PER_EXTENDED_FEATURE_PAGE)) : 0)

/** @brief Maximum memory required when supporting frame space update.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 */
#define SDC_MEM_FRAME_SPACE_UPDATE(num_links) ((num_links) > 0 ? (12 + (num_links) * 68) : 0)

/** @brief Maximum memory required when supporting shorter connection intervals.
 *
 * @param[in] num_links Total number of peripheral and central links supported.
 */
#define SDC_MEM_SHORTER_CONNECTION_INTERVALS(num_links) ((num_links) > 0 ? (12 + (num_links) * 52) : 0)

/** Memory required for Quality of Service (QoS) channel survey module. */
#define SDC_MEM_QOS_CHANNEL_SURVEY (40)

/** Memory required for the scanner when only supporting legacy scanning. */
#define SDC_MEM_SCAN(buffer_count) (408 + (buffer_count) * 104)

/** Memory required for the scanner when supporting extended scanning. */
#define SDC_MEM_SCAN_EXT(buffer_count) (408 + (buffer_count) * 320)

/** Additional memory required for the initiator when supporting scanning
 *  and initiating at the same time.
 */
#define SDC_MEM_INITIATOR (384)

#define __MEM_PER_PERIODIC_ADV_SET_LOW(max_adv_data) ((3104+(max_adv_data)*18)/10)
#define __MEM_PER_PERIODIC_ADV_SET_HIGH(max_adv_data) (498+(max_adv_data))
#define SDC_MEM_PER_PERIODIC_ADV_SET(max_adv_data) ((max_adv_data<255)?\
    (__MEM_PER_PERIODIC_ADV_SET_LOW(max_adv_data)):\
    (__MEM_PER_PERIODIC_ADV_SET_HIGH(max_adv_data)))
#define SDC_MEM_PER_PERIODIC_SYNC(buffer_count) (256 + (buffer_count) * 278)
#define SDC_MEM_PERIODIC_ADV_LIST(list_size) ((list_size) * 8)
#define SDC_DEFAULT_PERIODIC_ADV_RSP_MAX_TX_DATA 73
#define __MEM_PER_PERIODIC_ADV_RSP_TX_BUFFER(max_tx_data_size) ((max_tx_data_size) + 5)
#define __MEM_PER_PERIODIC_ADV_RSP_RX_BUFFER (283)
#define __MEM_MINIMAL_PERIODIC_ADV_RSP_SET_SIZE_WITH_RX (461)
#define __MEM_MINIMAL_PERIODIC_ADV_RSP_SET_SIZE_WITHOUT_RX (161)
#define __MEM_FOR_PERIODIC_ADV_RSP_FAILURE_REPORTING (224)
#define SDC_MEM_PER_PERIODIC_SYNC_RSP(tx_buffer_count, rx_buffer_count) \
    (671 + (tx_buffer_count - 1) * 255 + (rx_buffer_count) * 279)
#define SDC_MEM_PER_PERIODIC_ADV_RSP_SET(max_adv_data, tx_buffer_count, rx_buffer_count, max_tx_data_size, failure_reporting_enabled) \
     (SDC_MEM_PER_PERIODIC_ADV_SET(max_adv_data) \
     + ((rx_buffer_count) > 0 ? __MEM_MINIMAL_PERIODIC_ADV_RSP_SET_SIZE_WITH_RX : \
                             __MEM_MINIMAL_PERIODIC_ADV_RSP_SET_SIZE_WITHOUT_RX ) \
     + (tx_buffer_count) * __MEM_PER_PERIODIC_ADV_RSP_TX_BUFFER(max_tx_data_size) \
     + (rx_buffer_count) * __MEM_PER_PERIODIC_ADV_RSP_RX_BUFFER \
     + ((failure_reporting_enabled) ? __MEM_FOR_PERIODIC_ADV_RSP_FAILURE_REPORTING : 0))

/** Memory required for the Filter Accept List */
#define SDC_MEM_FAL(max_num_entries) ((max_num_entries) > 0 ? (4 + (max_num_entries) * 8) : 0)

/** @brief Auxiliary defines, not to be used outside of this file. */
#define __MEM_PER_ADV_SET_LOW(max_adv_data) ((5184+(max_adv_data)*18)/10)
#define __MEM_PER_ADV_SET_HIGH(max_adv_data) (706+(max_adv_data))
#define __MEM_PER_PERIODIC_ADV_SET_LOW(max_adv_data) ((3104+(max_adv_data)*18)/10)
#define __MEM_PER_PERIODIC_ADV_SET_HIGH(max_adv_data) (498+(max_adv_data))

/** @brief Maximum required memory for a given advertising buffer size.
 *
 * @param[in] max_adv_data The desired advertising data size.
 */
#define SDC_MEM_PER_ADV_SET(max_adv_data) ((max_adv_data<255)?\
    (__MEM_PER_ADV_SET_LOW(max_adv_data)):\
    (__MEM_PER_ADV_SET_HIGH(max_adv_data)))

/** @brief Additional memory required for periodic advertising.
 *
 * @note The max_adv_data must be the same as for advertising in @ref SDC_MEM_PER_ADV_SET.
 *
 * @param[in] max_adv_data The desired periodic advertising data size.
 */

#endif
