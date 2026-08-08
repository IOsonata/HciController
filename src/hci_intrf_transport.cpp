/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_intrf_transport.h"

#include <string.h>

#define HCI_INTRF_MAX_RX_PASSES 64U
#define HCI_INTRF_MAX_TX_PASSES 8U

static bool HciIntrfPacketTypeValid(HciH4PacketType_t Type)
{
    return Type >= HCI_H4_PACKET_COMMAND && Type <= HCI_H4_PACKET_ISO;
}

bool HciIntrfTransportInit(HciIntrfTransport_t *pTransport,
                           DevIntrf_t *pIntrf,
                           uint8_t *pHciRxPacket,
                           size_t HciRxPacketCapacity,
                           HciH4PacketHandler_t PacketHandler,
                           void *pPacketContext)
{
    if (pTransport == nullptr || pIntrf == nullptr ||
        pHciRxPacket == nullptr || HciRxPacketCapacity == 0U ||
        PacketHandler == nullptr)
    {
        return false;
    }

    memset(pTransport, 0, sizeof(*pTransport));
    pTransport->pIntrf = pIntrf;

    return HciH4ParserInit(&pTransport->Parser,
                           pHciRxPacket,
                           HciRxPacketCapacity,
                           PacketHandler,
                           pPacketContext);
}

void HciIntrfTransportOpen(HciIntrfTransport_t *pTransport)
{
    if (pTransport != nullptr)
    {
        pTransport->Open = true;
    }
}

void HciIntrfTransportClose(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    pTransport->Open = false;
    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    pTransport->TxStreamLen = 0U;
    pTransport->TxStreamOffset = 0U;
    HciH4ParserReset(&pTransport->Parser);
}

static void HciIntrfTransportProcessRx(HciIntrfTransport_t *pTransport)
{
    for (uint32_t pass = 0U; pass < HCI_INTRF_MAX_RX_PASSES; pass++)
    {
        if (pTransport->RxChunkOffset < pTransport->RxChunkLen ||
            HciH4ParserDeliveryPending(&pTransport->Parser))
        {
            const size_t remaining = pTransport->RxChunkLen - pTransport->RxChunkOffset;
            const uint8_t *pData = remaining > 0U ?
                                   &pTransport->RxChunk[pTransport->RxChunkOffset] :
                                   nullptr;

            pTransport->RxChunkOffset +=
                HciH4ParserFeed(&pTransport->Parser, pData, remaining);

            if (pTransport->RxChunkOffset < pTransport->RxChunkLen ||
                HciH4ParserDeliveryPending(&pTransport->Parser))
            {
                return;
            }

            pTransport->RxChunkLen = 0U;
            pTransport->RxChunkOffset = 0U;
        }

        int received = DeviceIntrfRx(pTransport->pIntrf,
                                     0U,
                                     pTransport->RxChunk,
                                     (int)sizeof(pTransport->RxChunk));
        if (received < 0)
        {
            pTransport->RxErrorCount++;
            return;
        }

        if (received == 0)
        {
            return;
        }

        if (pTransport->FirstRxLen < sizeof(pTransport->FirstRx))
        {
            size_t room = sizeof(pTransport->FirstRx) - pTransport->FirstRxLen;
            size_t keep = (size_t)received;
            if (keep > room)
            {
                keep = room;
            }
            memcpy(&pTransport->FirstRx[pTransport->FirstRxLen],
                   pTransport->RxChunk, keep);
            pTransport->FirstRxLen += (uint8_t)keep;
        }

        pTransport->RxOctetCount += (uint32_t)received;
        pTransport->RxChunkLen = (size_t)received;
        pTransport->RxChunkOffset = 0U;
    }
}

static void HciIntrfTransportProcessTx(HciIntrfTransport_t *pTransport)
{
    for (uint32_t pass = 0U;
         pass < HCI_INTRF_MAX_TX_PASSES && pTransport->TxStreamLen != 0U;
         pass++)
    {
        const size_t remaining = pTransport->TxStreamLen - pTransport->TxStreamOffset;
        int sent = DeviceIntrfTx(pTransport->pIntrf,
                                 0U,
                                 &pTransport->TxStream[pTransport->TxStreamOffset],
                                 (int)remaining);
        if (sent < 0)
        {
            pTransport->TxErrorCount++;
            return;
        }

        if (sent == 0)
        {
            pTransport->TxBusyCount++;
            return;
        }

        pTransport->TxOctetCount += (uint32_t)sent;
        pTransport->TxStreamOffset += (size_t)sent;
        if (pTransport->TxStreamOffset >= pTransport->TxStreamLen)
        {
            pTransport->TxStreamLen = 0U;
            pTransport->TxStreamOffset = 0U;
        }
    }
}

void HciIntrfTransportProcess(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr || pTransport->pIntrf == nullptr || !pTransport->Open)
    {
        return;
    }

    HciIntrfTransportProcessRx(pTransport);
    HciIntrfTransportProcessTx(pTransport);
}

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen)
{
    if (pTransport == nullptr || !pTransport->Open ||
        pTransport->TxStreamLen != 0U ||
        !HciIntrfPacketTypeValid(Type) ||
        PacketLen + 1U > sizeof(pTransport->TxStream) ||
        (PacketLen > 0U && pPacket == nullptr))
    {
        if (pTransport != nullptr && PacketLen + 1U > sizeof(pTransport->TxStream))
        {
            pTransport->TxOversizeCount++;
        }
        return false;
    }

    pTransport->TxStream[0] = (uint8_t)Type;
    if (PacketLen > 0U)
    {
        memcpy(&pTransport->TxStream[1], pPacket, PacketLen);
    }

    pTransport->TxStreamLen = PacketLen + 1U;
    pTransport->TxStreamOffset = 0U;
    return true;
}

bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport)
{
    return pTransport != nullptr && pTransport->TxStreamLen != 0U;
}
