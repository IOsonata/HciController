#ifndef SDC_STUB_H
#define SDC_STUB_H
#include <stdint.h>
typedef struct {
    const char *LastCall;
    unsigned Calls;
    uint8_t NextStatus;
} SdcStubState_t;
extern SdcStubState_t g_SdcStub;

/*
 * The generated stub normally defines these symbols directly. PAwR response
 * data is the exception: the real SDC returns from the command entry point and
 * raises its Command Complete later through sdc_hci_get(). Rename only the
 * generated definitions so sdc_stub_page2.cpp can wrap them and model that
 * delayed event without editing the generated file.
 *
 * This header is included after the real SDC declarations in the test sources,
 * so the macros affect the generated definitions/calls below the include, not
 * the vendor prototypes.
 */
#define sdc_hci_cmd_le_set_periodic_adv_response_data \
    sdc_stub_hci_cmd_le_set_periodic_adv_response_data
#define sdc_hci_get sdc_stub_hci_get

#endif
