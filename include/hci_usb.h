/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_USB_H
#define HCI_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_intrf.h"
#include "hci_h4.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HCI_USB_PACKET_SIZE
#define HCI_USB_PACKET_SIZE 1024U
#endif

#define HCI_USB_HCI_TRANSPORT_CDC_H4    1
#define HCI_USB_HCI_TRANSPORT_NATIVE    2

#ifndef HCI_USB_HCI_TRANSPORT
#define HCI_USB_HCI_TRANSPORT HCI_USB_HCI_TRANSPORT_NATIVE
#endif

#define HCI_USB_SERIALIZED_PACKET_SIZE (HCI_USB_PACKET_SIZE + 1U)
#define HCI_USB_TX_LEGACY_OFFSET       4U
#define HCI_USB_COMMAND_SIZE           258U
#define HCI_USB_SYNC_ALT_COUNT         7U
#define HCI_USB_ENDPOINT_DESC_SIZE     7U
#define HCI_USB_FS_BULK_MAX_PACKET     64U

#if defined(__cplusplus)
#define HCI_USB_ALIGN4 alignas(4)
#else
#define HCI_USB_ALIGN4 _Alignas(4)
#endif

typedef enum
{
    HCI_USB_DESCRIPTOR_LOG_ONLY = 0,
    HCI_USB_DESCRIPTOR_CDC_H4 = 1,
    HCI_USB_DESCRIPTOR_NATIVE_HCI = 2,
} HciUsbDescriptorMode_t;

/*
 * Native Bluetooth USB as an IOsonata DeviceIntrf.
 *
 * DevAddr selects HciH4PacketType_t. Every successful Rx/Tx moves exactly one
 * complete HCI packet. The packet type is metadata and is never inserted into
 * legacy USB transfers. Bulk Serialization adds/removes its wire indicator in
 * this implementation below DeviceIntrf.
 */
typedef struct
{
    DevIntrf_t DevIntrf;

    uint8_t HciInterface;
    uint8_t SyncInterface;
    uint8_t EventEp;
    uint8_t BulkOutEp;
    uint8_t BulkInEp;
    uint16_t BulkOutMps;
    uint16_t BulkInMps;
    uint8_t SyncOutEp;
    uint8_t SyncInEp;
    uint8_t HciAlt;
    uint8_t SyncAlt;

    bool Configured;
    bool BulkSerializationSupported;
    bool BulkSerialization;

    HciH4PacketType_t RxSelect;
    HciH4PacketType_t TxSelect;
    uint8_t ControlReply;

    bool CommandPending;
    size_t CommandLen;

    bool BulkRxPending;
    HciH4PacketType_t BulkRxType;
    size_t BulkRxLen;
    size_t BulkRxReceived;
    size_t BulkRxExpected;
    uint32_t BulkRxModeSwitchMark;

    bool SyncRxPending;
    size_t SyncRxLen;

    bool TxPending;
    bool TxActive;
    bool TxPayloadComplete;
    bool TxZlpActive;
    bool TxBufferBulkSerialization;
    HciH4PacketType_t TxType;
    size_t TxLen;
    size_t TxWireLen;

    uint32_t CommandCount;
    uint32_t AclOutCount;
    uint32_t AclInCount;
    uint32_t EventInCount;
    uint32_t IsoOutCount;
    uint32_t IsoInCount;
    uint32_t ScoOutCount;
    uint32_t ScoInCount;
    uint32_t InvalidRxCount;
    uint32_t TxBusyCount;
    uint32_t TxErrorCount;
    uint32_t UsbTransferErrorCount;
    uint32_t ModeSwitchCount;

    HCI_USB_ALIGN4 uint8_t CommandBuffer[HCI_USB_COMMAND_SIZE];
    HCI_USB_ALIGN4 uint8_t BulkRxBuffer[HCI_USB_SERIALIZED_PACKET_SIZE];
    HCI_USB_ALIGN4 uint8_t SyncRxBuffer[HCI_USB_PACKET_SIZE];

    /*
     * Byte 0 belongs to Bulk Serialization's H4 packet indicator. Legacy HCI
     * payload starts at byte 4 so its nRF EasyDMA pointer remains word aligned
     * without aliasing that indicator. Bytes 1..3 are only alignment space in
     * legacy mode and become the start of serialized payload in alt setting 1.
     */
    HCI_USB_ALIGN4 uint8_t TxBuffer[HCI_USB_PACKET_SIZE + HCI_USB_TX_LEGACY_OFFSET];

    uint8_t EventDesc[HCI_USB_ENDPOINT_DESC_SIZE];
    uint8_t BulkOutDesc[HCI_USB_ENDPOINT_DESC_SIZE];
    uint8_t BulkInDesc[HCI_USB_ENDPOINT_DESC_SIZE];
    uint8_t SyncAltPresent[HCI_USB_SYNC_ALT_COUNT];
    uint8_t SyncAltOutDesc[HCI_USB_SYNC_ALT_COUNT][HCI_USB_ENDPOINT_DESC_SIZE];
    uint8_t SyncAltInDesc[HCI_USB_SYNC_ALT_COUNT][HCI_USB_ENDPOINT_DESC_SIZE];
} HciUsb_t;

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode);
uint8_t HciUsbDescriptorLogCdcInstance(HciUsbDescriptorMode_t Mode);

bool HciUsbInit(HciUsb_t *pUsb, DevIntrfEvtHandler_t EvtCB);
void HciUsbDeinit(HciUsb_t *pUsb);
DevIntrf_t *HciUsbGetDeviceIntrf(HciUsb_t *pUsb);
void HciUsbProcess(HciUsb_t *pUsb);
bool HciUsbIsOpen(const HciUsb_t *pUsb);
bool HciUsbBulkSerialization(const HciUsb_t *pUsb);

#ifdef __cplusplus
}
#endif

#undef HCI_USB_ALIGN4

#endif /* HCI_USB_H */
