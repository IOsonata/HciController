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

#if defined(NRF52840_XXAA)
#include "nrf.h"
#endif

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

/*
 * Serial string as the 1.0.0 release emitted it: DEVICEID[1] then DEVICEID[0],
 * 16 upper case hex digits. The IOsonata default reads the words the other
 * way round, which would change the serial number of every dongle already in
 * the field. Kept here, not patched upstream, so other IOsonata devices keep
 * their own serial numbers.
 */
const char *HciUsbDescriptorSerial(void)
{
#if defined(NRF52840_XXAA)
	static char s_Serial[17];
	const uint32_t words[2] = { NRF_FICR->DEVICEID[1], NRF_FICR->DEVICEID[0] };
	size_t out = 0U;

	for (size_t word = 0U; word < 2U; word++)
	{
		for (int shift = 28; shift >= 0; shift -= 4)
		{
			const uint8_t digit = (uint8_t)((words[word] >> shift) & 0xFU);
			s_Serial[out++] = (char)(digit < 10U ? '0' + digit : 'A' + digit - 10U);
		}
	}
	s_Serial[out] = '\0';
	return s_Serial;
#else
	return NULL;
#endif
}
