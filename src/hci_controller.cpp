/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_controller.h"

#include <string.h>

static bool HciControllerHostTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_COMMAND ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_ISO;
}

static bool HciControllerOutputTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_EVENT ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_SCO ||
           Type == HCI_H4_PACKET_ISO;
}

static bool HciControllerH4IndicatorValid(uint8_t Type)
{
    return Type >= (uint8_t)HCI_H4_PACKET_COMMAND &&
           Type <= (uint8_t)HCI_H4_PACKET_ISO;
}

static bool HciControllerHostPacket(void *pContext,
                                    HciH4PacketType_t Type,
                                    const uint8_t *pPacket,
                                    size_t PacketLen)
{
    HciController_t *pController = static_cast<HciController_t *>(pContext);

    if (!HciControllerHostTypeValid(Type))
    {
        pController->InvalidHostPacketCount++;
        return true;
    }

    if (!pController->Controller.Put(pController->Controller.pContext,
                                     Type,
                                     pPacket,
                                     PacketLen))
    {
        pController->HostPacketRetryCount++;
        return false;
    }

    return true;
}

static void HciControllerFetchPacket(HciController_t *pController)
{
    if (pController->ControllerPacketPending ||
        HciIntrfTransportTxBusy(&pController->Host))
    {
        return;
    }

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t packetLen = 0U;
    HciControllerGetResult_t result =
        pController->Controller.Get(pController->Controller.pContext,
                                    &type,
                                    pController->pControllerPacket,
                                    pController->ControllerPacketCapacity,
                                    &packetLen);

    if (result == HCI_CONTROLLER_GET_EMPTY)
    {
        return;
    }

    if (result != HCI_CONTROLLER_GET_PACKET)
    {
        pController->ControllerGetErrorCount++;
        return;
    }

    /*
     * A zero length packet would put a bare indicator byte on the wire with no
     * header behind it, and the host would read the next packet's first bytes
     * as this one's header. Every H:4 packet has at least a header, so nothing
     * shorter can be legitimate.
     */
    if (!HciControllerOutputTypeValid(type) ||
        packetLen == 0U ||
        packetLen > pController->ControllerPacketCapacity)
    {
        pController->InvalidControllerPacketCount++;
        return;
    }

    pController->ControllerPacketType = type;
    pController->ControllerPacketLen = packetLen;
    pController->ControllerPacketPending = true;
}

static void HciControllerStartPacket(HciController_t *pController)
{
    if (!pController->ControllerPacketPending ||
        HciIntrfTransportTxBusy(&pController->Host))
    {
        return;
    }

    /*
     * The transport refuses a packet larger than its stream buffer, and it
     * refuses the same packet every time. Holding it pending would stop the
     * controller to host direction for good, because no further packet is
     * fetched while one is pending. Drop it and count it instead.
     */
    if (pController->ControllerPacketLen + 1U > HCI_INTRF_TX_STREAM_SIZE)
    {
        pController->ControllerPacketPending = false;
        pController->UnsendableControllerPacketCount++;
        return;
    }

    if (HciIntrfTransportSend(&pController->Host,
                              pController->ControllerPacketType,
                              pController->pControllerPacket,
                              pController->ControllerPacketLen))
    {
        pController->ControllerPacketPending = false;
    }
}

bool HciControllerInit(HciController_t *pController,
                       DevIntrf_t *pHostIntrf,
                       uint8_t *pHostPacket,
                       size_t HostPacketCapacity,
                       uint8_t *pControllerPacket,
                       size_t ControllerPacketCapacity,
                       const HciControllerOps_t *pControllerOps)
{
    if (pController == nullptr || pHostIntrf == nullptr ||
        pHostPacket == nullptr || HostPacketCapacity == 0U ||
        pControllerPacket == nullptr || ControllerPacketCapacity == 0U ||
        pControllerOps == nullptr || pControllerOps->Put == nullptr ||
        pControllerOps->Get == nullptr)
    {
        return false;
    }

    memset(pController, 0, sizeof(*pController));
    pController->Controller = *pControllerOps;
    pController->pControllerPacket = pControllerPacket;
    pController->ControllerPacketCapacity = ControllerPacketCapacity;

    return HciIntrfTransportInit(&pController->Host,
                                 pHostIntrf,
                                 pHostPacket,
                                 HostPacketCapacity,
                                 HciControllerHostPacket,
                                 pController);
}

void HciControllerPortOpen(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    HciIntrfTransport_t *pHost = &pController->Host;

    /*
     * The low-level transport intentionally flushes everything that arrived
     * before Open. That fixed the Thingy:91 mixed-UART case: the nRF9160 puts
     * a boot banner in the FIFO, then later sends H:4 on the same wire, and the
     * driver has erased the gap between the two by the time this side opens.
     * Feeding that backlog makes text manufacture a false packet that swallows
     * the real Reset behind it.
     *
     * A controller port has one more useful fact: its peer is supposed to be
     * an H:4 host. If the backlog already starts at a valid H:4 indicator, do
     * not throw it away merely because it arrived during controller bring-up.
     * H:4 has no transport-level retry, so a one-shot Reset lost here can leave
     * an otherwise clean host waiting forever.
     *
     * If the first octet is not H:4, count this first chunk as flushed and let
     * HciIntrfTransportOpen perform its existing bounded flush on the rest.
     * The Thingy:91 banner starts with text, so its measured workaround remains
     * unchanged.
     */
    const int received = DeviceIntrfRx(pHost->pIntrf,
                                       0U,
                                       pHost->RxChunk,
                                       (int)sizeof(pHost->RxChunk));

    if (received > 0 && HciControllerH4IndicatorValid(pHost->RxChunk[0]))
    {
        pHost->Open = true;
        pHost->RxChunkLen = (size_t)received;
        pHost->RxChunkOffset = 0U;
        pHost->RxOctetCount += (uint32_t)received;

        if (pHost->FirstRxLen < sizeof(pHost->FirstRx))
        {
            size_t keep = (size_t)received;
            const size_t room = sizeof(pHost->FirstRx) - pHost->FirstRxLen;
            if (keep > room)
            {
                keep = room;
            }
            memcpy(&pHost->FirstRx[pHost->FirstRxLen], pHost->RxChunk, keep);
            pHost->FirstRxLen += (uint8_t)keep;
        }
        return;
    }

    if (received > 0)
    {
        pHost->FlushedOctetCount += (uint32_t)received;
    }

    HciIntrfTransportOpen(pHost);
}

void HciControllerPortClose(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    HciIntrfTransportClose(&pController->Host);
    pController->ControllerPacketPending = false;
    pController->ControllerPacketLen = 0U;
    pController->ControllerPacketType = HCI_H4_PACKET_NONE;
}

void HciControllerProcess(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    if (pController->Controller.Process != nullptr)
    {
        pController->Controller.Process(pController->Controller.pContext);
    }

    HciIntrfTransportProcess(&pController->Host);
    HciControllerFetchPacket(pController);
    HciControllerStartPacket(pController);
    HciIntrfTransportProcess(&pController->Host);
}
