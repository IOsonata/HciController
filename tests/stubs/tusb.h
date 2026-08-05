#ifndef STUB_TUSB_H
#define STUB_TUSB_H
#include <stdint.h>
#include <stdbool.h>
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
