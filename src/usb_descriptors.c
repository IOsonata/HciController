/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf.h"
#include "tusb.h"

/*
 * Development identity based on TinyUSB's open-source example VID.
 * Override both values when I-SYST receives an assigned production VID/PID.
 */
#ifndef HCI_USB_VID
#define HCI_USB_VID 0xCAFEU
#endif

#ifndef HCI_USB_PID
#define HCI_USB_PID 0x4070U
#endif

#ifndef HCI_USB_DEVICE_RELEASE
#define HCI_USB_DEVICE_RELEASE 0x0100U
#endif

#define HCI_USB_BCD            0x0200U
/*
 * Two CDC functions on one device. The first is the HCI byte stream, the
 * second is the log.
 *
 * Endpoint numbers are per direction on this part, so an IN and an OUT may
 * share a number, and the notification endpoints have to be distinct from
 * both. Four IN and two OUT out of seven each way.
 */
#define HCI_USB_CONFIG_TOTAL   (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

#define HCI_USB_EP_CDC_NOTIFY  0x81U
#define HCI_USB_EP_CDC_OUT     0x02U
#define HCI_USB_EP_CDC_IN      0x82U

#define HCI_USB_EP_LOG_NOTIFY  0x83U
#define HCI_USB_EP_LOG_OUT     0x04U
#define HCI_USB_EP_LOG_IN      0x84U

static tusb_desc_device_t const s_DeviceDescriptor = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = HCI_USB_BCD,
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = HCI_USB_VID,
	.idProduct = HCI_USB_PID,
	.bcdDevice = HCI_USB_DEVICE_RELEASE,
	.iManufacturer = 1U,
	.iProduct = 2U,
	.iSerialNumber = 3U,
	.bNumConfigurations = 1U,
};

/*
 * Interface numbers, in the order the descriptor lists them. A CDC function
 * takes two: the communication interface and the data interface. The host
 * pairs them by the interface association descriptor TUD_CDC_DESCRIPTOR
 * emits, so the two functions must not be interleaved.
 */
enum {
	HCI_USB_ITF_CDC = 0,
	HCI_USB_ITF_CDC_DATA,
	HCI_USB_ITF_LOG,
	HCI_USB_ITF_LOG_DATA,
	HCI_USB_ITF_TOTAL,
};

static uint8_t const s_ConfigurationDescriptor[] = {
	TUD_CONFIG_DESCRIPTOR(1, HCI_USB_ITF_TOTAL, 0, HCI_USB_CONFIG_TOTAL, 0, 100),
	TUD_CDC_DESCRIPTOR(HCI_USB_ITF_CDC, 4, HCI_USB_EP_CDC_NOTIFY, 16,
					   HCI_USB_EP_CDC_OUT, HCI_USB_EP_CDC_IN, 64),
	TUD_CDC_DESCRIPTOR(HCI_USB_ITF_LOG, 5, HCI_USB_EP_LOG_NOTIFY, 16,
					   HCI_USB_EP_LOG_OUT, HCI_USB_EP_LOG_IN, 64),
};

/*
 * The descriptor the host is told to expect and the descriptor that was
 * built. TUD_CDC_DESC_LEN is the vendor's arithmetic and this is ours, so a
 * second function added without the total moving is a build failure rather
 * than a device the host enumerates and then finds short.
 */
_Static_assert(sizeof(s_ConfigurationDescriptor) == HCI_USB_CONFIG_TOTAL,
			   "configuration descriptor length does not match what it declares");

/*
 * The descriptor above declares two CDC functions and the device stack has to
 * be built for two, or it claims the first and leaves the second's endpoints
 * unopened. The host still enumerates two serial ports in that case, because
 * the descriptor said so, and the second one is dead: it opens, it accepts a
 * terminal, and nothing ever comes out of it.
 *
 * That is a symptom with no visible cause, and the cause would be a
 * tusb_config.h other than this project's arriving first on the include path.
 * So it is a build failure here instead.
 */
_Static_assert(CFG_TUD_CDC >= 2,
			   "the descriptor declares two CDC functions, CFG_TUD_CDC does not");

static char const *const s_StringDescriptors[] = {
	NULL,
	"I-SYST inc.",
	"I-SYST HCI Controller",
	NULL,
	"Bluetooth HCI H:4",
	"HCI controller log",
};

static uint16_t s_StringDescriptor[33];

static char HciUsbHexDigit(uint8_t value)
{
	value &= 0x0FU;
	return value < 10U ? (char)('0' + value) : (char)('A' + value - 10U);
}

static size_t HciUsbSerial(uint16_t *pOutput, size_t Capacity)
{
	if (pOutput == NULL || Capacity < 16U)
	{
		return 0U;
	}

	uint32_t words[2] = { NRF_FICR->DEVICEID[1], NRF_FICR->DEVICEID[0] };
	size_t out = 0U;
	for (size_t word = 0U; word < 2U; ++word)
	{
		for (int shift = 28; shift >= 0; shift -= 4)
		{
			pOutput[out++] = (uint16_t)HciUsbHexDigit((uint8_t)(words[word] >> shift));
		}
	}
	return out;
}

uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&s_DeviceDescriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t Index)
{
	(void)Index;
	return s_ConfigurationDescriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t Index, uint16_t LanguageId)
{
	(void)LanguageId;
	size_t count = 0U;

	if (Index == 0U)
	{
		s_StringDescriptor[1] = 0x0409U;
		count = 1U;
	}
	else if (Index == 3U)
	{
		count = HciUsbSerial(&s_StringDescriptor[1], 32U);
	}
	else
	{
		if (Index >= (sizeof(s_StringDescriptors) / sizeof(s_StringDescriptors[0])) ||
			s_StringDescriptors[Index] == NULL)
		{
			return NULL;
		}

		char const *pString = s_StringDescriptors[Index];
		count = strlen(pString);
		if (count > 32U)
		{
			count = 32U;
		}
		for (size_t i = 0U; i < count; ++i)
		{
			s_StringDescriptor[i + 1U] = (uint16_t)(uint8_t)pString[i];
		}
	}

	s_StringDescriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * count + 2U));
	return s_StringDescriptor;
}
