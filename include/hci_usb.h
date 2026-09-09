/**-------------------------------------------------------------------------
@file	hci_usb.h

@brief	USB identity and descriptor integration for HciController.

		Native Bluetooth transport is implemented by IOsonata BtHciUsb. This
		header retains only HciController product identities, transport-mode
		selection and composite descriptor assembly.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_USB_H
#define HCI_USB_H

#include <stdbool.h>
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

#define HCI_USB_STRING_BT	4U

typedef enum {
	HCI_USB_DESCRIPTOR_LOG_ONLY = 0,
	HCI_USB_DESCRIPTOR_CDC_H4 = 1,
	HCI_USB_DESCRIPTOR_NATIVE_HCI = 2,
} HciUsbDescriptorMode_t;

#ifdef __cplusplus
extern "C" {
#endif

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode);
bool HciUsbDescriptorSetHci(const BtHciUsbDesc_t *pHci);
bool HciUsbDescriptorSetSerialHci(const BtHciUsbSerialDesc_t *pHci);
bool HciUsbDescriptorSetFullHci(const BtHciUsbFullDesc_t *pHci);
uint16_t HciUsbDescriptorVid(void);
uint16_t HciUsbDescriptorPid(HciUsbDescriptorMode_t Mode);

const uint8_t *HciUsbDescHandler(uint8_t DescType, uint8_t DescIndex,
								 uint16_t LangId, UsbSpeed_t Speed,
								 uint16_t *pLength, void *pContext);

#ifdef __cplusplus
}
#endif

#endif /* HCI_USB_H */
