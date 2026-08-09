#ifndef STUB_USBD_PVT_H
#define STUB_USBD_PVT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/*
 * The device stack exports this to its class drivers. It reads the busy mark
 * on an endpoint, which is the word that says whether a transfer is still
 * outstanding on it.
 */
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr);
#ifdef __cplusplus
}
#endif
#endif
