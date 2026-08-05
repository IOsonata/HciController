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
#endif
