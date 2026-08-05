#ifndef STUB_SYSCLK_H
#define STUB_SYSCLK_H
#include <stdint.h>
typedef enum { OSC_TYPE_RC = 0, OSC_TYPE_XTAL = 1 } OSC_TYPE;
typedef struct { OSC_TYPE Type; uint32_t Freq; uint32_t Accuracy; uint32_t Drive; } OscDesc_t;
#ifdef __cplusplus
extern "C" {
#endif
const OscDesc_t *GetLowFreqOscDesc(void);
uint32_t SystemCoreClockGet(void);
#ifdef __cplusplus
}
#endif
#endif
