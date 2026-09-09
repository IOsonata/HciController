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
#define HCI_USB_NATIVE_CONFIG_MAX_LEN \
	(9U + sizeof(BtHciUsbFullDesc_t) + HCI_USB_CDC_FUNCTION_LEN)
#define HCI_USB_LOG_CONFIG_LEN		(9U + HCI_USB_CDC_FUNCTION_LEN)

static HciUsbDescriptorMode_t s_DescriptorMode = HCI_USB_DESCRIPTOR_CDC_H4;
static UsbDevDesc_t s_DeviceDescriptor;
static uint8_t s_StringDescriptor[66U];
static uint8_t s_ConfigNative[HCI_USB_NATIVE_CONFIG_MAX_LEN];
static uint16_t s_ConfigNativeLength;
static bool s_NativeConfigValid;

static const uint8_t s_ConfigCdcH4[] = {
	HCI_USB_CONFIG_HEADER(HCI_USB_CDC_H4_CONFIG_LEN, 4U),
	HCI_USB_CDC_FUNCTION(0U, HCI_USB_STRING_H4, 0x81U, 0x02U, 0x82U),
	HCI_USB_CDC_FUNCTION(2U, HCI_USB_STRING_LOG, 0x83U, 0x04U, 0x84U),
};

static const uint8_t s_ConfigLog[] = {
	HCI_USB_CONFIG_HEADER(HCI_USB_LOG_CONFIG_LEN, 2U),
	HCI_USB_CDC_FUNCTION(0U, HCI_USB_STRING_LOG, 0x81U, 0x02U, 0x82U),
};

_Static_assert(sizeof(s_ConfigCdcH4) == HCI_USB_CDC_H4_CONFIG_LEN,
	"CDC H4 configuration descriptor length");
_Static_assert(sizeof(s_ConfigLog) == HCI_USB_LOG_CONFIG_LEN,
	"log configuration descriptor length");

static uint8_t HciUsbFreeEndpoint(uint16_t InMask, uint16_t OutMask,
								  bool Pair)
{
	for (uint8_t ep = 1U; ep < 16U; ep++)
	{
		const uint16_t bit = (uint16_t)(1U << ep);
		if ((InMask & bit) == 0U && (!Pair || (OutMask & bit) == 0U))
		{
			return ep;
		}
	}
	return 0U;
}

static bool HciUsbDescriptorBuildNative(const void *pHci,
									 size_t HciLength,
									 uint8_t EventAddr,
									 uint8_t AclOutAddr,
									 uint8_t AclInAddr,
									 uint8_t ScoOutAddr,
									 uint8_t ScoInAddr)
{
	if (pHci == NULL || HciLength == 0U ||
		9U + HciLength + HCI_USB_CDC_FUNCTION_LEN > sizeof(s_ConfigNative))
	{
		return false;
	}

	const uint8_t eventEp = USB_ENDPADDR_NUM(EventAddr);
	const uint8_t aclOutEp = USB_ENDPADDR_NUM(AclOutAddr);
	const uint8_t aclInEp = USB_ENDPADDR_NUM(AclInAddr);
	if (eventEp == 0U || aclOutEp == 0U || aclOutEp != aclInEp)
	{
		return false;
	}

	uint16_t inMask = (uint16_t)((1U << eventEp) | (1U << aclInEp));
	uint16_t outMask = (uint16_t)(1U << aclOutEp);
	if (ScoOutAddr != 0U || ScoInAddr != 0U)
	{
		const uint8_t scoOutEp = USB_ENDPADDR_NUM(ScoOutAddr);
		const uint8_t scoInEp = USB_ENDPADDR_NUM(ScoInAddr);
		if (scoOutEp == 0U || scoInEp == 0U || scoOutEp != scoInEp ||
			(ScoOutAddr & USB_ENDPADDR_DIR_MASK) != 0U ||
			(ScoInAddr & USB_ENDPADDR_DIR_MASK) == 0U)
		{
			return false;
		}
		inMask |= (uint16_t)(1U << scoInEp);
		outMask |= (uint16_t)(1U << scoOutEp);
	}

	const uint8_t notifyEp = HciUsbFreeEndpoint(inMask, outMask, false);
	if (notifyEp == 0U)
	{
		return false;
	}
	inMask |= (uint16_t)(1U << notifyEp);
	const uint8_t dataEp = HciUsbFreeEndpoint(inMask, outMask, true);
	if (dataEp == 0U)
	{
		return false;
	}

	const uint16_t total =
		(uint16_t)(9U + HciLength + HCI_USB_CDC_FUNCTION_LEN);
	const uint8_t header[] = {
		HCI_USB_CONFIG_HEADER(total, 4U),
	};
	const uint8_t log[] = {
		HCI_USB_CDC_FUNCTION(2U, HCI_USB_STRING_LOG,
			USB_ENDPADDR_DIRIN(notifyEp), USB_ENDPADDR_DIROUT(dataEp),
			USB_ENDPADDR_DIRIN(dataEp)),
	};
	_Static_assert(sizeof(header) == 9U, "configuration header length");
	_Static_assert(sizeof(log) == HCI_USB_CDC_FUNCTION_LEN,
		"native log CDC descriptor length");

	memcpy(s_ConfigNative, header, sizeof(header));
	memcpy(&s_ConfigNative[sizeof(header)], pHci, HciLength);
	memcpy(&s_ConfigNative[sizeof(header) + HciLength], log, sizeof(log));
	s_ConfigNativeLength = total;
	s_NativeConfigValid = true;
	return true;
}

static bool HciUsbSerialDescriptorValid(const BtHciUsbSerialDesc_t *pHci)
{
	return pHci != NULL &&
		pHci->Association.bFirstInterface == 0U &&
		pHci->Association.bInterfaceCount == 2U &&
		pHci->Hci.bInterfaceNumber == 0U &&
		pHci->Hci.bAlternateSetting == 0U &&
		pHci->Serialized.Interface.bInterfaceNumber == 0U &&
		pHci->Serialized.Interface.bAlternateSetting == 1U &&
		pHci->Serialized.Interface.bNumEndpoints == 2U &&
		pHci->Sync.bInterfaceNumber == 1U &&
		pHci->Sync.bAlternateSetting == 0U &&
		(pHci->EventIn.bEndpointAddress & USB_ENDPADDR_DIR_MASK) != 0U &&
		(pHci->AclOut.bEndpointAddress & USB_ENDPADDR_DIR_MASK) == 0U &&
		(pHci->AclIn.bEndpointAddress & USB_ENDPADDR_DIR_MASK) != 0U &&
		pHci->Serialized.Out.bEndpointAddress == pHci->AclOut.bEndpointAddress &&
		pHci->Serialized.In.bEndpointAddress == pHci->AclIn.bEndpointAddress &&
		(pHci->Serialized.Out.bmAttributes & 0x03U) == USB_ENDPATT_TRANS_BULK &&
		(pHci->Serialized.In.bmAttributes & 0x03U) == USB_ENDPATT_TRANS_BULK;
}

bool HciUsbDescriptorSetHci(const BtHciUsbDesc_t *pHci)
{
	if (pHci == NULL || pHci->Association.bFirstInterface != 0U ||
		pHci->Association.bInterfaceCount != 2U ||
		pHci->Hci.bInterfaceNumber != 0U ||
		pHci->Sync.bInterfaceNumber != 1U ||
		(pHci->EventIn.bEndpointAddress & USB_ENDPADDR_DIR_MASK) == 0U ||
		(pHci->AclOut.bEndpointAddress & USB_ENDPADDR_DIR_MASK) != 0U ||
		(pHci->AclIn.bEndpointAddress & USB_ENDPADDR_DIR_MASK) == 0U)
	{
		return false;
	}

	return HciUsbDescriptorBuildNative(
		pHci, sizeof(*pHci), pHci->EventIn.bEndpointAddress,
		pHci->AclOut.bEndpointAddress, pHci->AclIn.bEndpointAddress, 0U, 0U);
}

bool HciUsbDescriptorSetSerialHci(const BtHciUsbSerialDesc_t *pHci)
{
	if (!HciUsbSerialDescriptorValid(pHci))
	{
		return false;
	}

	return HciUsbDescriptorBuildNative(
		pHci, sizeof(*pHci), pHci->EventIn.bEndpointAddress,
		pHci->AclOut.bEndpointAddress, pHci->AclIn.bEndpointAddress, 0U, 0U);
}

bool HciUsbDescriptorSetFullHci(const BtHciUsbFullDesc_t *pHci)
{
	if (pHci == NULL || !HciUsbSerialDescriptorValid(&pHci->Base))
	{
		return false;
	}

	const uint8_t scoOutAddr = pHci->Alt[0].Out.bEndpointAddress;
	const uint8_t scoInAddr = pHci->Alt[0].In.bEndpointAddress;
	const uint8_t scoEp = USB_ENDPADDR_NUM(scoOutAddr);
	if (scoEp == 0U || scoEp != USB_ENDPADDR_NUM(scoInAddr))
	{
		return false;
	}

	for (uint8_t i = 0U; i < BT_HCI_USB_SCO_ALT_COUNT; i++)
	{
		const BtHciUsbScoAltDesc_t *pAlt = &pHci->Alt[i];
		if (pAlt->Interface.bInterfaceNumber != pHci->Base.Sync.bInterfaceNumber ||
			pAlt->Interface.bAlternateSetting != (uint8_t)(i + 1U) ||
			pAlt->Interface.bNumEndpoints != 2U ||
			(pAlt->Out.bEndpointAddress & USB_ENDPADDR_DIR_MASK) != 0U ||
			(pAlt->In.bEndpointAddress & USB_ENDPADDR_DIR_MASK) == 0U ||
			USB_ENDPADDR_NUM(pAlt->Out.bEndpointAddress) != scoEp ||
			USB_ENDPADDR_NUM(pAlt->In.bEndpointAddress) != scoEp ||
			(pAlt->Out.bmAttributes & 0x03U) != USB_ENDPATT_TRANS_ISO ||
			(pAlt->In.bmAttributes & 0x03U) != USB_ENDPATT_TRANS_ISO ||
			pAlt->Out.wMaxPacketSize == 0U ||
			pAlt->Out.wMaxPacketSize > HCI_USB_SCO_MAX_MPS ||
			pAlt->In.wMaxPacketSize != pAlt->Out.wMaxPacketSize ||
			pAlt->Out.bInterval == 0U ||
			pAlt->In.bInterval != pAlt->Out.bInterval)
		{
			return false;
		}
	}

	return HciUsbDescriptorBuildNative(
		pHci, sizeof(*pHci), pHci->Base.EventIn.bEndpointAddress,
		pHci->Base.AclOut.bEndpointAddress, pHci->Base.AclIn.bEndpointAddress,
		scoOutAddr, scoInAddr);
}

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode)
{
	if (Mode < HCI_USB_DESCRIPTOR_LOG_ONLY ||
		Mode > HCI_USB_DESCRIPTOR_NATIVE_HCI)
	{
		return false;
	}
	s_DescriptorMode = Mode;
	if (Mode == HCI_USB_DESCRIPTOR_NATIVE_HCI)
	{
		s_NativeConfigValid = false;
		s_ConfigNativeLength = 0U;
	}
	return true;
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
			if (!s_NativeConfigValid || s_ConfigNativeLength == 0U)
			{
				return NULL;
			}
			*pLength = s_ConfigNativeLength;
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