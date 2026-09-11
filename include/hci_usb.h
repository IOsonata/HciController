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

#include <stdint.h>

#include "bluetooth/bt_hci_usb.h"

#define HCI_USB_PACKET_SIZE			BT_HCI_USB_PACKET_MAX_SIZE
#define HCI_USB_COMMAND_SIZE			BT_HCI_USB_COMMAND_MAX_SIZE
#define HCI_USB_FS_BULK_MPS			BT_HCI_USB_ACL_FS_MPS
#define HCI_USB_EVENT_MPS			BT_HCI_USB_EVENT_FS_MPS
#define HCI_USB_PKT_BLKSIZE			BT_HCI_USB_ACL_PKT_BLKSIZE
#define HCI_USB_SCO_MAX_MPS			BT_HCI_USB_SCO_MAX_MPS

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

#ifdef __cplusplus
}
#endif

#endif /* HCI_USB_H */
