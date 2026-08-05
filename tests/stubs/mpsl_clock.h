#ifndef STUB_MPSL_CLOCK_H
#define STUB_MPSL_CLOCK_H
#include <stdint.h>
typedef enum { MPSL_CLOCK_HF_SRC_XO = 0, MPSL_CLOCK_HF_SRC_MAX = 2 } mpsl_clock_hfclk_src_t;
typedef enum { MPSL_CLOCK_EVT_HFCLK_STARTED = 0 } mpsl_clock_evt_type_t;
typedef void (*mpsl_clock_hfclk_callback_t)(void);
typedef void (*mpsl_clock_hfclk_request_callback_t)(mpsl_clock_evt_type_t);
#ifdef __cplusplus
extern "C" {
#endif
int32_t mpsl_clock_hfclk_request(mpsl_clock_hfclk_callback_t);
int32_t mpsl_clock_hfclk_release(void);
int32_t mpsl_clock_hfclk_is_running(uint32_t *);
int32_t mpsl_clock_hfclk_src_request(mpsl_clock_hfclk_src_t, mpsl_clock_hfclk_request_callback_t);
int32_t mpsl_clock_hfclk_src_release(mpsl_clock_hfclk_src_t);
int32_t mpsl_clock_hfclk_src_is_running(mpsl_clock_hfclk_src_t, uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
