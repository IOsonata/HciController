/**-------------------------------------------------------------------------
@file	usb_descriptors.c

@brief	USB product identity selection for HciController.

		IOsonata owns device/class descriptor construction and composite
		configuration assembly. HciController owns only its VID/PID policy.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_usb.h"

#define HCI_USB_DEVELOPMENT_VID			0xCAFEU
#define HCI_USB_DEVELOPMENT_PID_CDC_H4	0x4070U
#define HCI_USB_DEVELOPMENT_PID_NATIVE	0x4071U
#define HCI_USB_DEVELOPMENT_PID_LOG		0x4072U

#ifndef HCI_USB_VID
#define HCI_USB_VID HCI_USB_DEVELOPMENT_VID
#endif
#ifndef HCI_USB_PID_CDC_H4
#define HCI_USB_PID_CDC_H4 HCI_USB_DEVELOPMENT_PID_CDC_H4
#endif
#ifndef HCI_USB_PID_NATIVE_HCI
#define HCI_USB_PID_NATIVE_HCI HCI_USB_DEVELOPMENT_PID_NATIVE
#endif
#ifndef HCI_USB_PID_LOG_ONLY
#define HCI_USB_PID_LOG_ONLY HCI_USB_DEVELOPMENT_PID_LOG
#endif

#ifndef HCI_USB_REQUIRE_ASSIGNED_IDS
#define HCI_USB_REQUIRE_ASSIGNED_IDS 0
#endif

#if HCI_USB_REQUIRE_ASSIGNED_IDS && \
	(HCI_USB_VID == HCI_USB_DEVELOPMENT_VID || \
	 HCI_USB_PID_CDC_H4 == HCI_USB_DEVELOPMENT_PID_CDC_H4 || \
	 HCI_USB_PID_NATIVE_HCI == HCI_USB_DEVELOPMENT_PID_NATIVE || \
	 HCI_USB_PID_LOG_ONLY == HCI_USB_DEVELOPMENT_PID_LOG)
#error "production USB build requires assigned HCI_USB_VID/PID values"
#endif

uint16_t HciUsbDescriptorVid(void)
{
	return HCI_USB_VID;
}

uint16_t HciUsbDescriptorPid(HciUsbDescriptorMode_t Mode)
{
	switch (Mode)
	{
		case HCI_USB_DESCRIPTOR_NATIVE_HCI:
			return HCI_USB_PID_NATIVE_HCI;
		case HCI_USB_DESCRIPTOR_LOG_ONLY:
			return HCI_USB_PID_LOG_ONLY;
		case HCI_USB_DESCRIPTOR_CDC_H4:
		default:
			return HCI_USB_PID_CDC_H4;
	}
}
