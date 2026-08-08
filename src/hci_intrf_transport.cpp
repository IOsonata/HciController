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

    /*
     * Suspect means the stream held something that is not H:4 since it last
     * went quiet. It is recorded and it is not acted on, and that is a change
     * made against evidence.
     *
     * Refusing suspect packets was meant to stop this side answering commands
     * manufactured out of a bootloader banner, because those answers
     * desynchronise the host's parser. It did that. It also refused eight real
     * HCI Resets, which the log named one at a time: "pkt: 01 03 0C drop". A
     * command that is thrown away is a link that never starts. Answers to
     * accidental commands only cost the host a resynchronisation, and the host
     * drains its own port when it opens the transport, so the ones sent before
     * that moment reach nobody.
     *
     * The two failures are not the same size. This layer cannot tell an
     * accidental command from a real one, since only the opcode table can and
     * that is another layer up, so between refusing real commands and
     * answering false ones it now answers.
     */
    const bool suspect = HciIntrfTransportSuspect(pTransport);

    if (suspect && pTransport->SuspectFilter != nullptr &&
        !pTransport->SuspectFilter(pTransport->pFilterContext, Type, pPacket,
                                   PacketLen))
    {
        pTransport->DroppedPacketCount++;
        return true;
    }

    /*
     * A refused packet is offered again on the next pass, so both the count
     * and the record are taken only once the packet has stopped being offered.
     * Counting on every attempt was the first version of this and it inflated
     * exactly the number a reader would trust most.
     */
    if (!pTransport->Handler(pTransport->pHandlerContext, Type, pPacket,
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
        pTransport->PktMark[slot].Suspect = suspect;
        pTransport->PktMarkLen++;
    }

    if (suspect)
    {
        pTransport->SuspectPacketCount++;
    }

    pTransport->RxPacketCount++;
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

/*
 * Bounded, so a port that hands back data forever cannot hold start up.
 */
#define HCI_INTRF_FLUSH_PASSES 64U

void HciIntrfTransportSetSuspectFilter(HciIntrfTransport_t *pTransport,
                                       HciH4PacketHandler_t Filter,
                                       void *pContext)
{
    if (pTransport != nullptr)
    {
        pTransport->SuspectFilter = Filter;
        pTransport->pFilterContext = pContext;
    }
}

void HciIntrfTransportOpen(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    pTransport->Open = true;

    /*
     * Throw away whatever arrived before anyone was listening.
     *
     * The port's own buffer fills from the moment the driver is configured,
     * which on this application is several hundred milliseconds before the
     * controller behind it can answer anything. So the first read after
     * opening returns a run of octets that were spread over that whole time,
     * with every gap between them gone.
     *
     * That matters because a gap is the only thing separating one sender's
     * output from another's, and it is the only thing this layer has to
     * recover from a stream that is not H:4. On the Thingy:91 the nRF9160's
     * bootloader banner and the HCI Reset behind it are hundreds of
     * milliseconds apart on the wire and adjacent in the buffer, so the Reset
     * was read as part of the banner's burst and refused with it. The log said
     * so in as many words: "pkt: 01 03 0C drop".
     *
     * Nothing is lost that was not already lost. A host that sends before this
     * side can answer gets no answer either way, and H:4 has no retry, so the
     * choice is between discarding those octets and misreading them. The host
     * asks again.
     */
    for (uint32_t pass = 0U; pass < HCI_INTRF_FLUSH_PASSES; pass++)
    {
        const int received = DeviceIntrfRx(pTransport->pIntrf,
                                           0U,
                                           pTransport->RxChunk,
                                           (int)sizeof(pTransport->RxChunk));
        if (received <= 0)
        {
            break;
        }

        pTransport->FlushedOctetCount += (uint32_t)received;
    }

    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    HciH4ParserReset(&pTransport->Parser);
    pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;
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
