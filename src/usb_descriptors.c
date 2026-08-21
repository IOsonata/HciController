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

#include "hci_usb.h"
#include "nrf.h"
#include "tusb.h"

#define HCI_USB_DEVELOPMENT_VID            0xCAFEU
#define HCI_USB_DEVELOPMENT_PID_CDC_H4     0x4070U
#define HCI_USB_DEVELOPMENT_PID_NATIVE_HCI 0x4071U
#define HCI_USB_DEVELOPMENT_PID_LOG_ONLY   0x4072U

#ifndef HCI_USB_VID
#define HCI_USB_VID HCI_USB_DEVELOPMENT_VID
#endif

#ifndef HCI_USB_PID_CDC_H4
#define HCI_USB_PID_CDC_H4 HCI_USB_DEVELOPMENT_PID_CDC_H4
#endif

#ifndef HCI_USB_PID_NATIVE_HCI
#define HCI_USB_PID_NATIVE_HCI HCI_USB_DEVELOPMENT_PID_NATIVE_HCI
#endif

#ifndef HCI_USB_PID_LOG_ONLY
#define HCI_USB_PID_LOG_ONLY HCI_USB_DEVELOPMENT_PID_LOG_ONLY
#endif

/*
 * Shipping/product builds can set this to 1 so the development VID/PIDs can
 * never escape into a production image by accident. The actual assigned IDs
 * belong to the product build and are not invented here.
 */
#ifndef HCI_USB_REQUIRE_ASSIGNED_IDS
#define HCI_USB_REQUIRE_ASSIGNED_IDS 0
#endif

#if HCI_USB_REQUIRE_ASSIGNED_IDS && \
    (HCI_USB_VID == HCI_USB_DEVELOPMENT_VID || \
     HCI_USB_PID_CDC_H4 == HCI_USB_DEVELOPMENT_PID_CDC_H4 || \
     HCI_USB_PID_NATIVE_HCI == HCI_USB_DEVELOPMENT_PID_NATIVE_HCI || \
     HCI_USB_PID_LOG_ONLY == HCI_USB_DEVELOPMENT_PID_LOG_ONLY)
#error "production USB build requires assigned HCI_USB_VID/PID values"
#endif

#ifndef HCI_USB_DEVICE_RELEASE
#define HCI_USB_DEVICE_RELEASE          0x0100U
#endif

#define HCI_USB_BCD                     0x0200U
#define HCI_USB_BT_CLASS                0xE0U
#define HCI_USB_BT_SUBCLASS             0x01U
#define HCI_USB_BT_PROTOCOL             0x01U
#define HCI_USB_STRING_BT               4U
#define HCI_USB_STRING_H4               5U
#define HCI_USB_STRING_LOG              6U

#define HCI_USB_EP_BT_EVENT             0x81U
#define HCI_USB_EP_BT_ACL_OUT           0x02U
#define HCI_USB_EP_BT_ACL_IN            0x82U
#define HCI_USB_EP_NATIVE_LOG_NOTIFY    0x84U
#define HCI_USB_EP_NATIVE_LOG_OUT       0x05U
#define HCI_USB_EP_NATIVE_LOG_IN        0x85U
#define HCI_USB_EP_CDC_H4_NOTIFY        0x81U
#define HCI_USB_EP_CDC_H4_OUT           0x02U
#define HCI_USB_EP_CDC_H4_IN            0x82U
#define HCI_USB_EP_CDC_LOG_NOTIFY       0x83U
#define HCI_USB_EP_CDC_LOG_OUT          0x04U
#define HCI_USB_EP_CDC_LOG_IN           0x84U
#define HCI_USB_EP_LOG_ONLY_NOTIFY      0x81U
#define HCI_USB_EP_LOG_ONLY_OUT         0x02U
#define HCI_USB_EP_LOG_ONLY_IN          0x82U

#define HCI_USB_BT_IAD_LEN              8U
#define HCI_USB_BT_HCI_ALT0_LEN         (9U + 3U * 7U)
#define HCI_USB_BT_HCI_ALT1_LEN         (9U + 2U * 7U)
#define HCI_USB_BT_SCO_ALT0_LEN         9U
#define HCI_USB_BT_DESC_LEN \
    (HCI_USB_BT_IAD_LEN + HCI_USB_BT_HCI_ALT0_LEN + \
     HCI_USB_BT_HCI_ALT1_LEN + HCI_USB_BT_SCO_ALT0_LEN)
#define HCI_USB_CONFIG_CDC_H4_TOTAL \
    (TUD_CONFIG_DESC_LEN + 2U * TUD_CDC_DESC_LEN)
#define HCI_USB_CONFIG_NATIVE_TOTAL \
    (TUD_CONFIG_DESC_LEN + HCI_USB_BT_DESC_LEN + TUD_CDC_DESC_LEN)
#define HCI_USB_CONFIG_LOG_ONLY_TOTAL \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define HCI_USB_BT_INTERFACE(Interface, Alt, EndpointCount, StringIndex) \
    9U, TUSB_DESC_INTERFACE, Interface, Alt, EndpointCount, \
    HCI_USB_BT_CLASS, HCI_USB_BT_SUBCLASS, HCI_USB_BT_PROTOCOL, StringIndex
#define HCI_USB_ENDPOINT(EpAddr, TransferType, PacketSize, Interval) \
    7U, TUSB_DESC_ENDPOINT, EpAddr, TransferType, U16_TO_U8S_LE(PacketSize), Interval

static HciUsbDescriptorMode_t s_DescriptorMode = HCI_USB_DESCRIPTOR_CDC_H4;

#define HCI_USB_DEVICE_DESCRIPTOR(Pid, Class, SubClass, Protocol) \
    { \
        .bLength = sizeof(tusb_desc_device_t), \
        .bDescriptorType = TUSB_DESC_DEVICE, \
        .bcdUSB = HCI_USB_BCD, \
        .bDeviceClass = Class, \
        .bDeviceSubClass = SubClass, \
        .bDeviceProtocol = Protocol, \
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, \
        .idVendor = HCI_USB_VID, \
        .idProduct = Pid, \
        .bcdDevice = HCI_USB_DEVICE_RELEASE, \
        .iManufacturer = 1U, \
        .iProduct = 2U, \
        .iSerialNumber = 3U, \
        .bNumConfigurations = 1U, \
    }

static const tusb_desc_device_t s_DeviceCdcH4 =
    HCI_USB_DEVICE_DESCRIPTOR(HCI_USB_PID_CDC_H4,
                              TUSB_CLASS_MISC,
                              MISC_SUBCLASS_COMMON,
                              MISC_PROTOCOL_IAD);

/*
 * Native HCI is a composite USB device: one Bluetooth function plus one CDC
 * log function. EF/02/01 tells IAD-aware hosts to enumerate those functions
 * independently. The Bluetooth IAD and interfaces below remain E0/01/01.
 */
static const tusb_desc_device_t s_DeviceNativeHci =
    HCI_USB_DEVICE_DESCRIPTOR(HCI_USB_PID_NATIVE_HCI,
                              TUSB_CLASS_MISC,
                              MISC_SUBCLASS_COMMON,
                              MISC_PROTOCOL_IAD);

static const tusb_desc_device_t s_DeviceLogOnly =
    HCI_USB_DEVICE_DESCRIPTOR(HCI_USB_PID_LOG_ONLY,
                              TUSB_CLASS_MISC,
                              MISC_SUBCLASS_COMMON,
                              MISC_PROTOCOL_IAD);

enum
{
    HCI_USB_CDC_H4_ITF_HCI = 0,
    HCI_USB_CDC_H4_ITF_HCI_DATA,
    HCI_USB_CDC_H4_ITF_LOG,
    HCI_USB_CDC_H4_ITF_LOG_DATA,
    HCI_USB_CDC_H4_ITF_TOTAL,
};

static const uint8_t s_ConfigCdcH4[] = {
    TUD_CONFIG_DESCRIPTOR(1U, HCI_USB_CDC_H4_ITF_TOTAL, 0U,
                          HCI_USB_CONFIG_CDC_H4_TOTAL, 0U, 100U),
    TUD_CDC_DESCRIPTOR(HCI_USB_CDC_H4_ITF_HCI, HCI_USB_STRING_H4,
                       HCI_USB_EP_CDC_H4_NOTIFY, 16U,
                       HCI_USB_EP_CDC_H4_OUT, HCI_USB_EP_CDC_H4_IN, 64U),
    TUD_CDC_DESCRIPTOR(HCI_USB_CDC_H4_ITF_LOG, HCI_USB_STRING_LOG,
                       HCI_USB_EP_CDC_LOG_NOTIFY, 16U,
                       HCI_USB_EP_CDC_LOG_OUT, HCI_USB_EP_CDC_LOG_IN, 64U),
};

enum
{
    HCI_USB_NATIVE_ITF_HCI = 0,
    HCI_USB_NATIVE_ITF_SCO,
    HCI_USB_NATIVE_ITF_LOG,
    HCI_USB_NATIVE_ITF_LOG_DATA,
    HCI_USB_NATIVE_ITF_TOTAL,
};

static const uint8_t s_ConfigNativeHci[] = {
    TUD_CONFIG_DESCRIPTOR(1U, HCI_USB_NATIVE_ITF_TOTAL, 0U,
                          HCI_USB_CONFIG_NATIVE_TOTAL, 0U, 100U),

    /* Bluetooth function: HCI data interface plus synchronous interface. */
    HCI_USB_BT_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION,
    HCI_USB_NATIVE_ITF_HCI, 2U, HCI_USB_BT_CLASS, HCI_USB_BT_SUBCLASS,
    HCI_USB_BT_PROTOCOL, HCI_USB_STRING_BT,

    /* Mandatory legacy USB HCI transport. */
    HCI_USB_BT_INTERFACE(HCI_USB_NATIVE_ITF_HCI, 0U, 3U, HCI_USB_STRING_BT),
    HCI_USB_ENDPOINT(HCI_USB_EP_BT_EVENT, TUSB_XFER_INTERRUPT, 16U, 1U),
    HCI_USB_ENDPOINT(HCI_USB_EP_BT_ACL_OUT, TUSB_XFER_BULK, 64U, 0U),
    HCI_USB_ENDPOINT(HCI_USB_EP_BT_ACL_IN, TUSB_XFER_BULK, 64U, 0U),

    /* Optional Bulk Serialization: packet indicator plus packet on bulk I/O. */
    HCI_USB_BT_INTERFACE(HCI_USB_NATIVE_ITF_HCI, 1U, 2U, HCI_USB_STRING_BT),
    HCI_USB_ENDPOINT(HCI_USB_EP_BT_ACL_OUT, TUSB_XFER_BULK, 64U, 0U),
    HCI_USB_ENDPOINT(HCI_USB_EP_BT_ACL_IN, TUSB_XFER_BULK, 64U, 0U),

    /*
     * The nRF52840 SDC image is LE-only and exposes no SCO data path. Keep the
     * required second Bluetooth interface at its zero-bandwidth alternate 0;
     * do not advertise isochronous bandwidth the controller cannot consume.
     */
    HCI_USB_BT_INTERFACE(HCI_USB_NATIVE_ITF_SCO, 0U, 0U, HCI_USB_STRING_BT),

    /* Independent diagnostic log function. */
    TUD_CDC_DESCRIPTOR(HCI_USB_NATIVE_ITF_LOG, HCI_USB_STRING_LOG,
                       HCI_USB_EP_NATIVE_LOG_NOTIFY, 16U,
                       HCI_USB_EP_NATIVE_LOG_OUT, HCI_USB_EP_NATIVE_LOG_IN, 64U),
};

enum
{
    HCI_USB_LOG_ONLY_ITF_LOG = 0,
    HCI_USB_LOG_ONLY_ITF_LOG_DATA,
    HCI_USB_LOG_ONLY_ITF_TOTAL,
};

static const uint8_t s_ConfigLogOnly[] = {
    TUD_CONFIG_DESCRIPTOR(1U, HCI_USB_LOG_ONLY_ITF_TOTAL, 0U,
                          HCI_USB_CONFIG_LOG_ONLY_TOTAL, 0U, 100U),
    TUD_CDC_DESCRIPTOR(HCI_USB_LOG_ONLY_ITF_LOG, HCI_USB_STRING_LOG,
                       HCI_USB_EP_LOG_ONLY_NOTIFY, 16U,
                       HCI_USB_EP_LOG_ONLY_OUT, HCI_USB_EP_LOG_ONLY_IN, 64U),
};

_Static_assert(sizeof(s_ConfigCdcH4) == HCI_USB_CONFIG_CDC_H4_TOTAL,
               "CDC H:4 configuration descriptor length mismatch");
_Static_assert(sizeof(s_ConfigNativeHci) == HCI_USB_CONFIG_NATIVE_TOTAL,
               "native HCI configuration descriptor length mismatch");
_Static_assert(sizeof(s_ConfigLogOnly) == HCI_USB_CONFIG_LOG_ONLY_TOTAL,
               "log-only configuration descriptor length mismatch");
_Static_assert(CFG_TUD_CDC >= 2,
               "legacy H:4 configuration requires two CDC instances");

static const char *const s_StringDescriptors[] = {
    NULL,
    "I-SYST inc.",
    "I-SYST HCI Controller",
    NULL,
    "Bluetooth HCI",
    "Bluetooth HCI H:4",
    "HCI controller log",
};

static uint16_t s_StringDescriptor[33];

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode)
{
    if (Mode != HCI_USB_DESCRIPTOR_LOG_ONLY &&
        Mode != HCI_USB_DESCRIPTOR_CDC_H4 &&
        Mode != HCI_USB_DESCRIPTOR_NATIVE_HCI)
    {
        return false;
    }

    s_DescriptorMode = Mode;
    return true;
}

uint8_t HciUsbDescriptorLogCdcInstance(HciUsbDescriptorMode_t Mode)
{
    return Mode == HCI_USB_DESCRIPTOR_CDC_H4 ? 1U : 0U;
}

static char HciUsbHexDigit(uint8_t Value)
{
    Value &= 0x0FU;
    return Value < 10U ? (char)('0' + Value) : (char)('A' + Value - 10U);
}

static size_t HciUsbSerial(uint16_t *pOutput, size_t Capacity)
{
    if (pOutput == NULL || Capacity < 16U)
    {
        return 0U;
    }

    const uint32_t Words[2] = { NRF_FICR->DEVICEID[1], NRF_FICR->DEVICEID[0] };
    size_t Out = 0U;
    for (size_t Word = 0U; Word < 2U; ++Word)
    {
        for (int Shift = 28; Shift >= 0; Shift -= 4)
        {
            pOutput[Out++] = (uint16_t)HciUsbHexDigit((uint8_t)(Words[Word] >> Shift));
        }
    }
    return Out;
}

uint8_t const *tud_descriptor_device_cb(void)
{
    switch (s_DescriptorMode)
    {
        case HCI_USB_DESCRIPTOR_NATIVE_HCI:
            return (const uint8_t *)&s_DeviceNativeHci;

        case HCI_USB_DESCRIPTOR_LOG_ONLY:
            return (const uint8_t *)&s_DeviceLogOnly;

        case HCI_USB_DESCRIPTOR_CDC_H4:
        default:
            return (const uint8_t *)&s_DeviceCdcH4;
    }
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t Index)
{
    (void)Index;

    switch (s_DescriptorMode)
    {
        case HCI_USB_DESCRIPTOR_NATIVE_HCI:
            return s_ConfigNativeHci;

        case HCI_USB_DESCRIPTOR_LOG_ONLY:
            return s_ConfigLogOnly;

        case HCI_USB_DESCRIPTOR_CDC_H4:
        default:
            return s_ConfigCdcH4;
    }
}

uint16_t const *tud_descriptor_string_cb(uint8_t Index, uint16_t LanguageId)
{
    (void)LanguageId;
    size_t Count = 0U;

    if (Index == 0U)
    {
        s_StringDescriptor[1] = 0x0409U;
        Count = 1U;
    }
    else if (Index == 3U)
    {
        Count = HciUsbSerial(&s_StringDescriptor[1], 32U);
    }
    else
    {
        if (Index >= (sizeof(s_StringDescriptors) / sizeof(s_StringDescriptors[0])) ||
            s_StringDescriptors[Index] == NULL)
        {
            return NULL;
        }

        const char *pString = s_StringDescriptors[Index];
        Count = strlen(pString);
        if (Count > 32U)
        {
            Count = 32U;
        }

        for (size_t Char = 0U; Char < Count; ++Char)
        {
            s_StringDescriptor[Char + 1U] = (uint16_t)(uint8_t)pString[Char];
        }
    }

    s_StringDescriptor[0] =
        (uint16_t)((TUSB_DESC_STRING << 8) | (2U * Count + 2U));
    return s_StringDescriptor;
}
