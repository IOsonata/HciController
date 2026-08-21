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
 * The deprecated standalone VS transmitter carrier test is another exception.
 * Current nRF52 SDC headers still declare it, but the multirole archive no
 * longer exports it. Rename the generated definition so the real-header tests
 * cannot accidentally satisfy a firmware reference that would fail to link on
 * the target. HciController uses sdc_hci_cmd_vs_dtm_command instead.
 *
 * This header is included after the real SDC declarations in the test sources,
 * so the macros affect the generated definitions/calls below the include, not
 * the vendor prototypes.
 */
#define sdc_hci_cmd_le_set_periodic_adv_response_data \
    sdc_stub_hci_cmd_le_set_periodic_adv_response_data
#define sdc_hci_get sdc_stub_hci_get
#define sdc_hci_cmd_vs_transmitter_carrier_test \
    sdc_stub_absent_hci_cmd_vs_transmitter_carrier_test

#endif
