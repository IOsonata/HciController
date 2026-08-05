#ifndef SDC_STUB_H
#define SDC_STUB_H
#include <stdint.h>
typedef struct {
    const char *LastCall;
    unsigned Calls;
    uint8_t NextStatus;
} SdcStubState_t;
extern SdcStubState_t g_SdcStub;
#endif
