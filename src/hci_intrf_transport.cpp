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

/*
 * An octet refused at a packet boundary is the only evidence in the stream
 * that it is not H:4, and it is evidence about everything after it as well as
 * about itself. Compared against the count at the last quiet moment, so it
 * says "since the stream last started" rather than "ever".
 */
bool HciIntrfTransportSuspect(const HciIntrfTransport_t *pTransport)
{
    return pTransport != nullptr &&
           pTransport->Parser.InvalidTypeCount != pTransport->RejectedMark;
}

static bool HciIntrfTransportCount(void *pContext,
                                   HciH4PacketType_t Type,
                                   const uint8_t *pPacket,
                                   size_t PacketLen)
{
    HciIntrfTransport_t *pTransport =
        static_cast<HciIntrfTransport_t *>(pContext);

    const bool dropped = HciIntrfTransportSuspect(pTransport);

    /*
     * A refused packet is offered again on the next pass, so both the count
     * and the record are taken only once the packet has stopped being offered.
     * Counting on every attempt was the first version of this and it inflated
     * exactly the number a reader would trust most.
     */
    if (!dropped &&
        !pTransport->Handler(pTransport->pHandlerContext, Type, pPacket,
                             PacketLen))
    {
        return false;
    }

    if (pTransport->PktMarkLen < HCI_INTRF_PKT_MARKS)
    {
        const uint8_t slot = pTransport->PktMarkLen;
        pTransport->PktMark[slot].Type = (uint8_t)Type;
        pTransport->PktMark[slot].Head[0] = PacketLen > 0U ? pPacket[0] : 0U;
        pTransport->PktMark[slot].Head[1] = PacketLen > 1U ? pPacket[1] : 0U;
        pTransport->PktMark[slot].Dropped = dropped;
        pTransport->PktMarkLen++;
    }

    if (dropped)
    {
        pTransport->DroppedPacketCount++;
    }
    else
    {
        pTransport->RxPacketCount++;
    }

    return true;
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
    pTransport->Handler = PacketHandler;
    pTransport->pHandlerContext = pPacketContext;

    /*
     * The parser calls in here and this passes the packet on, so a packet that
     * was accepted can be counted in the one place that knows it was. A count
     * of packets next to a count of octets is what separates a link with H:4
     * on it from a link with merely traffic on it.
     */
    return HciH4ParserInit(&pTransport->Parser,
                           pHciRxPacket,
                           HciRxPacketCapacity,
                           HciIntrfTransportCount,
                           pTransport);
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

void HciIntrfTransportIdle(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    /*
     * Whatever was on the wire has stopped, so the next octet starts something
     * new and gets the benefit of the doubt. This is the only way out of
     * suspicion, and it is the right one: the gap is what separates the
     * bootloader's output from the application's, and there is no octet that
     * does.
     */
    pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;

    if (!HciH4ParserIsMidPacket(&pTransport->Parser))
    {
        return;
    }

    /*
     * The chunk goes with it. Whatever is left in it belongs to the packet
     * being abandoned, and feeding it after the reset would rebuild the same
     * wrong packet from the same wrong octets.
     */
    HciH4ParserReset(&pTransport->Parser);
    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    pTransport->ResyncCount++;
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
