/*
 * The one thing src/usb_descriptors.c reads from nrf.h.
 *
 * That file builds the USB descriptors and, until now, was compiled by the
 * target build and by nothing else. A descriptor whose declared total length
 * disagrees with the bytes after it is not a compile error on its own: it is
 * a device a host enumerates and then rejects, with nothing on either side
 * saying which number is wrong.
 *
 * So it is compiled here against the real TinyUSB headers, which is where
 * TUD_CDC_DESC_LEN and the descriptor macros live. Nothing is copied from
 * them. This header exists only so the serial number, which comes from the
 * factory information registers, does not drag the whole MCU header in.
 */
#ifndef NRF_H_STUB
#define NRF_H_STUB

#include <stdint.h>

typedef struct {
	uint32_t DEVICEID[2];
} NRF_FICR_Type;

extern NRF_FICR_Type *NRF_FICR;

#endif
