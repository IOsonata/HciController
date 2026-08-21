/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_USB_PRIV_H
#define HCI_USB_PRIV_H

#include "device/usbd_pvt.h"
#include "hci_usb.h"
#include "tusb.h"

#define HCI_USB_CLASS_WIRELESS_CONTROLLER      0xE0U
#define HCI_USB_SUBCLASS_RF_CONTROLLER         0x01U
#define HCI_USB_PROTOCOL_BLUETOOTH             0x01U
#define HCI_USB_HCI_ALT_LEGACY                 0U
#define HCI_USB_HCI_ALT_BULK_SERIALIZATION     1U
#define HCI_USB_COMMAND_HEADER_SIZE             3U
#define HCI_USB_EVENT_HEADER_SIZE               2U
#define HCI_USB_ACL_HEADER_SIZE                 4U
#define HCI_USB_SCO_HEADER_SIZE                 3U
#define HCI_USB_ISO_HEADER_SIZE                 4U
#define HCI_USB_HISTORICAL_COMMAND_REQUEST      0xE0U

static inline bool HciUsbEdptXfer(uint8_t RhPort,
                                  uint8_t EpAddr,
                                  uint8_t *pBuffer,
                                  uint16_t Len)
{
#if defined(TUSB_VERSION_NUMBER) && TUSB_VERSION_NUMBER >= 2100
    return usbd_edpt_xfer(RhPort, EpAddr, pBuffer, Len, false);
#else
    return usbd_edpt_xfer(RhPort, EpAddr, pBuffer, Len);
#endif
}

static inline bool HciUsbEdptXferIsr(uint8_t RhPort,
                                     uint8_t EpAddr,
                                     uint8_t *pBuffer,
                                     uint16_t Len)
{
#if defined(TUSB_VERSION_NUMBER) && TUSB_VERSION_NUMBER >= 2100
    return usbd_edpt_xfer(RhPort, EpAddr, pBuffer, Len, true);
#else
    return usbd_edpt_xfer(RhPort, EpAddr, pBuffer, Len);
#endif
}

extern HciUsb_t *g_HciUsb;

void HciUsbWake(HciUsb_t *pUsb);
bool HciUsbInterfaceMatches(const tusb_desc_interface_t *pInterface);
bool HciUsbEndpointMatches(const tusb_desc_endpoint_t *pEndpoint,
                           uint8_t Direction,
                           uint8_t TransferType);
size_t HciUsbPacketLength(HciH4PacketType_t Type,
                          const uint8_t *pPacket,
                          size_t Available);
void HciUsbResetBulkRx(HciUsb_t *pUsb);
void HciUsbResetBulkRxSession(HciUsb_t *pUsb);
bool HciUsbArmBulkOut(HciUsb_t *pUsb);
bool HciUsbBulkOutXferIsr(HciUsb_t *pUsb, size_t TransferLen);
bool HciUsbArmSyncOut(HciUsb_t *pUsb);
void HciUsbRearmRx(HciUsb_t *pUsb);
bool HciUsbParseBulkOut(HciUsb_t *pUsb, size_t TransferLen);
bool HciUsbKickTx(HciUsb_t *pUsb);
void HciUsbTxComplete(HciUsb_t *pUsb,
                      uint8_t EpAddr,
                      uint32_t Transferred);
void HciUsbTxFailed(HciUsb_t *pUsb, uint8_t EpAddr);

#endif /* HCI_USB_PRIV_H */
