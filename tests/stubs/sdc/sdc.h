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
typedef struct { uint8_t count; } sdc_cfg_role_count_t;
typedef struct { uint8_t rx_packet_size; uint8_t tx_packet_size;
                 uint8_t rx_packet_count; uint8_t tx_packet_count; } sdc_cfg_buffer_cfg_t;
typedef struct { uint16_t max_adv_data; } sdc_cfg_adv_buffer_cfg_t;
typedef struct { uint8_t count; } sdc_cfg_scan_buffer_cfg_t;
typedef union {
    sdc_cfg_buffer_cfg_t buffer_cfg;
    sdc_cfg_role_count_t peripheral_count;
    sdc_cfg_role_count_t central_count;
    sdc_cfg_role_count_t adv_count;
    sdc_cfg_adv_buffer_cfg_t adv_buffer_cfg;
    sdc_cfg_scan_buffer_cfg_t scan_buffer_cfg;
    /* A bare integer in the real header, not a role count. */
    uint16_t fal_size;
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
#ifdef __cplusplus
}
#endif

/*
 * Memory requirement macros, copied verbatim from the real
 * softdevice_controller/include/sdc.h so the build time floor in
 * hci_nrf52840.cpp is checked in the host build too. If these drift from the
 * vendor header the dispatch test, which compiles against the real one, is
 * what catches it.
 */
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
