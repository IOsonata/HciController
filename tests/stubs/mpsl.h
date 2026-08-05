#ifndef STUB_MPSL_H
#define STUB_MPSL_H
#include <stdint.h>
#include "nrf.h"
#define MPSL_HIGH_IRQ_PRIORITY 0
#define MPSL_RECOMMENDED_RC_CTIV 16
#define MPSL_RECOMMENDED_RC_TEMP_CTIV 2
#define MPSL_DEFAULT_CLOCK_ACCURACY_PPM 250
#define MPSL_DEFAULT_SKIP_WAIT_LFCLK_STARTED false
enum MPSL_CLOCK_LF_SRC { MPSL_CLOCK_LF_SRC_RC = 0, MPSL_CLOCK_LF_SRC_XTAL = 1 };
typedef struct {
    uint8_t source; uint8_t rc_ctiv; uint8_t rc_temp_ctiv;
    uint16_t accuracy_ppm; bool skip_wait_lfclk_started;
} mpsl_clock_lfclk_cfg_t;
typedef void (*mpsl_assert_handler_t)(const char *, uint32_t);
#ifdef __cplusplus
extern "C" {
#endif
int32_t mpsl_init(const mpsl_clock_lfclk_cfg_t *, IRQn_Type, mpsl_assert_handler_t);
void mpsl_uninit(void);
void mpsl_low_priority_process(void);
void MPSL_IRQ_RADIO_Handler(void);
void MPSL_IRQ_RTC0_Handler(void);
void MPSL_IRQ_TIMER0_Handler(void);
void MPSL_IRQ_CLOCK_Handler(void);
#ifdef __cplusplus
}
#endif
#endif
