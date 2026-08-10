#ifndef STUB_TUSB_H
#define STUB_TUSB_H
#include <stdint.h>
#include <stdbool.h>

/*
 * The real tusb.h reaches tusb_config.h through tusb_option.h, so a file that
 * includes tusb.h sees CFG_TUD_CDC and the rest. Pulling in the real config
 * here keeps that true for a test, and keeps the function count one number
 * rather than one here and another in the firmware.
 */
#include "tusb_config.h"

/* Exercise the current TinyUSB root-port deinit path in hci_tinyusb.cpp. */
#define TUSB_VERSION_MAJOR    0
#define TUSB_VERSION_MINOR    19
#define TUSB_VERSION_REVISION 0
#define TUSB_VERSION_NUMBER \
    (TUSB_VERSION_MAJOR * 10000 + TUSB_VERSION_MINOR * 100 + TUSB_VERSION_REVISION)

typedef enum { TUSB_ROLE_INVALID = 0, TUSB_ROLE_DEVICE = 1, TUSB_ROLE_HOST = 2 } tusb_role_t;
typedef enum { TUSB_SPEED_FULL = 0, TUSB_SPEED_HIGH = 1, TUSB_SPEED_AUTO = 2 } tusb_speed_t;

typedef struct {
    tusb_role_t role;
    tusb_speed_t speed;
} tusb_rhport_init_t;

#ifdef __cplusplus
extern "C" {
#endif
void tusb_int_handler(uint8_t rhport, bool in_isr);
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t *pInit);
bool tusb_deinit(uint8_t rhport);
bool tud_deinit(uint8_t rhport);
bool tusb_inited(void);
void tud_task_ext(uint8_t rhport, bool in_isr);
bool tud_mounted(void);
uint8_t tud_cdc_n_get_line_state(uint8_t itf);
uint32_t tud_cdc_n_available(uint8_t itf);
uint32_t tud_cdc_n_read(uint8_t itf, void *pBuff, uint32_t Len);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, const void *pBuff, uint32_t Len);
uint32_t tud_cdc_n_write_flush(uint8_t itf);
#ifdef __cplusplus
}
#endif
#endif
