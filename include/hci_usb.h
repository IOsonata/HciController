/**-------------------------------------------------------------------------
@file	hci_usb.h

@brief	USB identity integration for HciController.

		Native Bluetooth transport, class registration and descriptor assembly
		are implemented by IOsonata. This header retains only HciController
		product identities and transport-mode selection.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_USB_H
#define HCI_USB_H

#include <stddef.h>
#include <stdint.h>

/*
 * The IOsonata USB headers are C++ only (the Nordic usb_ctrlr.h uses
 * alignas), and usb_descriptors.c is C. Only the C++ side needs BtHciUsb.
 */
#ifdef __cplusplus
#include "bluetooth/bt_hci_usb.h"
#endif

#define HCI_USB_HCI_TRANSPORT_CDC_H4	1
#define HCI_USB_HCI_TRANSPORT_NATIVE	2

#ifndef HCI_USB_HCI_TRANSPORT
#define HCI_USB_HCI_TRANSPORT HCI_USB_HCI_TRANSPORT_NATIVE
#endif

#define HCI_USB_STRING_FUNCTION	4U

typedef enum {
	HCI_USB_DESCRIPTOR_LOG_ONLY = 0,
	HCI_USB_DESCRIPTOR_CDC_H4 = 1,
	HCI_USB_DESCRIPTOR_NATIVE_HCI = 2,
} HciUsbDescriptorMode_t;

#ifdef __cplusplus
extern "C" {
#endif

uint16_t HciUsbDescriptorVid(void);
uint16_t HciUsbDescriptorPid(HciUsbDescriptorMode_t Mode);

/* Factory serial in the released byte order, NULL on a host build. */
const char *HciUsbDescriptorSerial(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_USB_H */
