/* Test-only SDC entry point added separately from the generated stub. */
#include <stdint.h>

#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_stub.h"

uint8_t sdc_hci_cmd_cb_set_event_mask_page_2(
    const sdc_hci_cmd_cb_set_event_mask_page_2_t *pParams)
{
    g_SdcStub.LastCall = "sdc_hci_cmd_cb_set_event_mask_page_2";
    g_SdcStub.Calls++;
    (void)pParams;
    return g_SdcStub.NextStatus;
}
