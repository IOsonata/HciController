/**-------------------------------------------------------------------------
@file	usb_descriptors.c

@brief	IOsonata USB descriptors for HciController.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_usb.h"
#include "hci_version.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

#define HCI_USB_STRING_BT	4U
#define HCI_USB_STRING_H4	5U
#define HCI_USB_STRING_LOG	6U

#define HCI_USB_U16_LO(Value)	((uint8_t)((Value) & 0xFFU))
#define HCI_USB_U16_HI(Value)	((uint8_t)(((Value) >> 8) & 0xFFU))

#define HCI_USB_CONFIG_HEADER(Total, Interfaces) \
	9U, USB_DESCTYPE_CONFIGURATION, HCI_USB_U16_LO(Total), \
	HCI_USB_U16_HI(Total), Interfaces, 1U, 0U, USB_CONFATT_RESERVED, 50U

#define HCI_USB_ENDPOINT(Address, Type, Mps, Interval) \
	7U, USB_DESCTYPE_ENDPOINT, Address, Type, HCI_USB_U16_LO(Mps), \
	HCI_USB_U16_HI(Mps), Interval

#define HCI_USB_INTERFACE(Number, Alt, Endpoints, Class, SubClass, Protocol, String) \
	9U, USB_DESCTYPE_INTERFACE, Number, Alt, Endpoints, Class, SubClass, \
	Protocol, String

/* CDC ACM function: IAD, control interface, class descriptors, endpoints. */
#define HCI_USB_CDC_FUNCTION(Ctrl, String, Notify, Out, In) \
	8U, USB_DESCTYPE_IA, Ctrl, 2U, 0x02U, 0x02U, 0x00U, 0U, \
	HCI_USB_INTERFACE(Ctrl, 0U, 1U, 0x02U, 0x02U, 0x00U, String), \
	5U, 0x24U, 0x00U, 0x20U, 0x01U, \
	5U, 0x24U, 0x01U, 0x00U, (Ctrl) + 1U, \
	4U, 0x24U, 0x02U, 0x02U, \
	5U, 0x24U, 0x06U, Ctrl, (Ctrl) + 1U, \
	HCI_USB_ENDPOINT(Notify, USB_ENDPATT_TRANS_INT, 8U, 16U), \
	HCI_USB_INTERFACE((Ctrl) + 1U, 0U, 2U, 0x0AU, 0U, 0U, 0U), \
	HCI_USB_ENDPOINT(Out, USB_ENDPATT_TRANS_BULK, 64U, 0U), \
	HCI_USB_ENDPOINT(In, USB_ENDPATT_TRANS_BULK, 64U, 0U)

#define HCI_USB_CDC_FUNCTION_LEN	66U
#define HCI_USB_CDC_H4_CONFIG_LEN	(9U + 2U * HCI_USB_CDC_FUNCTION_LEN)
#define HCI_USB_NATIVE_BT_LEN		(8U + 30U + 23U + 9U)
#define HCI_USB_NATIVE_CONFIG_LEN	(9U + HCI_USB_NATIVE_BT_LEN + HCI_USB_CDC_FUNCTION_LEN)
#define HCI_USB_LOG_CONFIG_LEN		(9U + HCI_USB_CDC_FUNCTION_LEN)

static HciUsbDescriptorMode_t s_DescriptorMode = HCI_USB_DESCRIPTOR_CDC_H4;
static UsbDevDesc_t s_DeviceDescriptor;
static uint8_t s_StringDescriptor[66U];

static const uint8_t s_ConfigCdcH4[] = {
	HCI_USB_CONFIG_HEADER(HCI_USB_CDC_H4_CONFIG_LEN, 4U),
	HCI_USB_CDC_FUNCTION(0U, HCI_USB_STRING_H4, 0x81U, 0x02U, 0x82U),
	HCI_USB_CDC_FUNCTION(2U, HCI_USB_STRING_LOG, 0x83U, 0x04U, 0x84U),
};

static const uint8_t s_ConfigNative[] = {
	HCI_USB_CONFIG_HEADER(HCI_USB_NATIVE_CONFIG_LEN, 4U),
	8U, USB_DESCTYPE_IA, 0U, 2U, 0xE0U, 0x01U, 0x01U,
		HCI_USB_STRING_BT,
	HCI_USB_INTERFACE(0U, 0U, 3U, 0xE0U, 0x01U, 0x01U,
		HCI_USB_STRING_BT),
	HCI_USB_ENDPOINT(0x81U, USB_ENDPATT_TRANS_INT, 16U, 1U),
	HCI_USB_ENDPOINT(0x02U, USB_ENDPATT_TRANS_BULK, 64U, 0U),
	HCI_USB_ENDPOINT(0x82U, USB_ENDPATT_TRANS_BULK, 64U, 0U),
	HCI_USB_INTERFACE(0U, 1U, 2U, 0xE0U, 0x01U, 0x01U,
		HCI_USB_STRING_BT),
	HCI_USB_ENDPOINT(0x02U, USB_ENDPATT_TRANS_BULK, 64U, 0U),
	HCI_USB_ENDPOINT(0x82U, USB_ENDPATT_TRANS_BULK, 64U, 0U),
	HCI_USB_INTERFACE(1U, 0U, 0U, 0xE0U, 0x01U, 0x01U,
		HCI_USB_STRING_BT),
	/* CDC function 1 maps to interfaces 2/3 and endpoint numbers 3/4. */
	HCI_USB_CDC_FUNCTION(2U, HCI_USB_STRING_LOG, 0x83U, 0x04U, 0x84U),
};

static const uint8_t s_ConfigLog[] = {
	HCI_USB_CONFIG_HEADER(HCI_USB_LOG_CONFIG_LEN, 2U),
	HCI_USB_CDC_FUNCTION(0U, HCI_USB_STRING_LOG, 0x81U, 0x02U, 0x82U),
};

_Static_assert(sizeof(s_ConfigCdcH4) == HCI_USB_CDC_H4_CONFIG_LEN,
	"CDC H4 configuration descriptor length");
_Static_assert(sizeof(s_ConfigNative) == HCI_USB_NATIVE_CONFIG_LEN,
	"native configuration descriptor length");
_Static_assert(sizeof(s_ConfigLog) == HCI_USB_LOG_CONFIG_LEN,
	"log configuration descriptor length");

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode)
{
	if (Mode < HCI_USB_DESCRIPTOR_LOG_ONLY ||
		Mode > HCI_USB_DESCRIPTOR_NATIVE_HCI)
	{
		return false;
	}
	s_DescriptorMode = Mode;
	return true;
}

uint8_t HciUsbDescriptorLogCdcInstance(HciUsbDescriptorMode_t Mode)
{
	return Mode == HCI_USB_DESCRIPTOR_LOG_ONLY ? 0U : 1U;
}

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

static const uint8_t *HciUsbDeviceDescriptor(uint16_t *pLength)
{
	const UsbCfg_t *pCfg = UsbGetCfg(0);
	if (pCfg == NULL || pLength == NULL)
	{
		return NULL;
	}

	memset(&s_DeviceDescriptor, 0, sizeof(s_DeviceDescriptor));
	s_DeviceDescriptor.bLength = sizeof(s_DeviceDescriptor);
	s_DeviceDescriptor.bDescriptorType = USB_DESCTYPE_DEVICE;
	s_DeviceDescriptor.bcdUSB = 0x0200U;
	s_DeviceDescriptor.bDeviceClass = USB_DEVCLASS_MISC;
	s_DeviceDescriptor.bDeviceSubClass = 0x02U;
	s_DeviceDescriptor.bDeviceProtocol = 0x01U;
	s_DeviceDescriptor.bMaxPacketSize = 64U;
	s_DeviceDescriptor.idVendor = pCfg->Vid;
	s_DeviceDescriptor.idProduct = pCfg->Pid;
	s_DeviceDescriptor.bcdDevice = pCfg->DevVer;
	s_DeviceDescriptor.iManufacturer = pCfg->pManufacturer != NULL ? 1U : 0U;
	s_DeviceDescriptor.iProduct = pCfg->pProduct != NULL ? 2U : 0U;
	s_DeviceDescriptor.iSerialNumber = 3U;
	s_DeviceDescriptor.bNumConfigurations = 1U;
	*pLength = sizeof(s_DeviceDescriptor);
	return (const uint8_t *)&s_DeviceDescriptor;
}

static const uint8_t *HciUsbConfigDescriptor(uint8_t Index,
										 uint16_t *pLength)
{
	if (Index != 0U || pLength == NULL)
	{
		return NULL;
	}

	switch (s_DescriptorMode)
	{
		case HCI_USB_DESCRIPTOR_NATIVE_HCI:
			*pLength = sizeof(s_ConfigNative);
			return s_ConfigNative;
		case HCI_USB_DESCRIPTOR_LOG_ONLY:
			*pLength = sizeof(s_ConfigLog);
			return s_ConfigLog;
		case HCI_USB_DESCRIPTOR_CDC_H4:
		default:
			*pLength = sizeof(s_ConfigCdcH4);
			return s_ConfigCdcH4;
	}
}

static const char *HciUsbString(uint8_t Index)
{
	const UsbCfg_t *pCfg = UsbGetCfg(0);
	if (pCfg == NULL)
	{
		return NULL;
	}

	switch (Index)
	{
		case 1U:
			return pCfg->pManufacturer;
		case 2U:
			return pCfg->pProduct;
		case 3U:
			return UsbGetSerial(0);
		case HCI_USB_STRING_BT:
			return "Bluetooth HCI";
		case HCI_USB_STRING_H4:
			return "Bluetooth HCI H:4";
		case HCI_USB_STRING_LOG:
			return "HCI controller log";
		default:
			return NULL;
	}
}

static const uint8_t *HciUsbStringDescriptor(uint8_t Index, uint16_t LangId,
										 uint16_t *pLength)
{
	if (pLength == NULL)
	{
		return NULL;
	}
	if (Index == 0U)
	{
		s_StringDescriptor[0] = 4U;
		s_StringDescriptor[1] = USB_DESCTYPE_STRING;
		s_StringDescriptor[2] = 0x09U;
		s_StringDescriptor[3] = 0x04U;
		*pLength = 4U;
		return s_StringDescriptor;
	}
	if (LangId != 0U && LangId != 0x0409U)
	{
		return NULL;
	}

	const char *pString = HciUsbString(Index);
	if (pString == NULL)
	{
		return NULL;
	}
	size_t length = strlen(pString);
	if (length > 32U)
	{
		length = 32U;
	}
	s_StringDescriptor[0] = (uint8_t)(2U + 2U * length);
	s_StringDescriptor[1] = USB_DESCTYPE_STRING;
	for (size_t i = 0U; i < length; i++)
	{
		s_StringDescriptor[2U + 2U * i] = (uint8_t)pString[i];
		s_StringDescriptor[3U + 2U * i] = 0U;
	}
	*pLength = s_StringDescriptor[0];
	return s_StringDescriptor;
}

const uint8_t *HciUsbDescHandler(uint8_t DescType, uint8_t DescIndex,
								 uint16_t LangId, UsbSpeed_t Speed,
								 uint16_t *pLength, void *pContext)
{
	(void)Speed;
	(void)pContext;

	switch (DescType)
	{
		case USB_DESCTYPE_DEVICE:
			return DescIndex == 0U ? HciUsbDeviceDescriptor(pLength) : NULL;
		case USB_DESCTYPE_CONFIGURATION:
			return HciUsbConfigDescriptor(DescIndex, pLength);
		case USB_DESCTYPE_STRING:
			return HciUsbStringDescriptor(DescIndex, LangId, pLength);
		default:
			return NULL;
	}
}
