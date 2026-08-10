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
#include "hci_core_profile.h"

#include <string.h>

#define HCI_CONTROLLER_EVENT_COMMAND_COMPLETE         0x0EU
#define HCI_CONTROLLER_STATUS_UNKNOWN_HCI_COMMAND     0x01U
#define HCI_CONTROLLER_OPCODE_READ_LOCAL_VERSION      0x1001U
#define HCI_CONTROLLER_OPCODE_READ_SUPPORTED_COMMANDS 0x1002U
#define HCI_CONTROLLER_LOCAL_VERSION_EVENT_SIZE       14U
#define HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE     6U

/* Core 6.0 commands which current nrfxlib also exposes on nRF52. */
#define HCI_CONTROLLER_OPCODE_LE_READ_ALL_LOCAL_FEATURES  0x2087U
#define HCI_CONTROLLER_OPCODE_LE_READ_ALL_REMOTE_FEATURES 0x2088U
#define HCI_CONTROLLER_OPCODE_LE_SET_HOST_FEATURE_V2      0x2097U

static uint16_t HciControllerReadLe16(const uint8_t *pData)
{
    return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

static bool HciControllerCommandAllowedByCore(uint16_t Opcode)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION != 0U && \
    HCI_CONTROLLER_TARGET_CORE_VERSION < HCI_CORE_VERSION_6_0
    switch (Opcode)
    {
        case HCI_CONTROLLER_OPCODE_LE_READ_ALL_LOCAL_FEATURES:
        case HCI_CONTROLLER_OPCODE_LE_READ_ALL_REMOTE_FEATURES:
        case HCI_CONTROLLER_OPCODE_LE_SET_HOST_FEATURE_V2:
            return false;

        default:
            break;
    }
#else
    (void)Opcode;
#endif
    return true;
}

static bool HciControllerQueueUnknownCommand(HciController_t *pController,
                                             uint16_t Opcode)
{
    if (pController->ControllerPacketPending ||
        HciIntrfTransportTxBusy(&pController->Host))
    {
        return false;
    }

    uint8_t *pEvent = pController->pControllerPacket;
    pEvent[0] = HCI_CONTROLLER_EVENT_COMMAND_COMPLETE;
    pEvent[1] = 4U;
    pEvent[2] = 1U;
    pEvent[3] = (uint8_t)Opcode;
    pEvent[4] = (uint8_t)(Opcode >> 8);
    pEvent[5] = HCI_CONTROLLER_STATUS_UNKNOWN_HCI_COMMAND;

    pController->ControllerPacketType = HCI_H4_PACKET_EVENT;
    pController->ControllerPacketLen = HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE;
    pController->ControllerPacketPending = true;
    return true;
}

static void HciControllerApplyCoreProfile(HciH4PacketType_t Type,
                                          uint8_t *pPacket,
                                          size_t PacketLen)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION != 0U
    if (Type != HCI_H4_PACKET_EVENT ||
        PacketLen < HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE ||
        pPacket[0] != HCI_CONTROLLER_EVENT_COMMAND_COMPLETE ||
        pPacket[5] != 0U)
    {
        return;
    }

    const uint16_t opcode = HciControllerReadLe16(&pPacket[3]);

    if (opcode == HCI_CONTROLLER_OPCODE_READ_LOCAL_VERSION &&
        PacketLen >= HCI_CONTROLLER_LOCAL_VERSION_EVENT_SIZE)
    {
        /*
         * Expose the Core revision implemented by this complete HCI controller,
         * not merely the newer backend library. The cap is downward only; an
         * older backend is never promoted.
         */
        if (pPacket[6] > HCI_CONTROLLER_TARGET_CORE_VERSION)
        {
            pPacket[6] = HCI_CONTROLLER_TARGET_CORE_VERSION;
        }
        if (pPacket[9] > HCI_CONTROLLER_TARGET_CORE_VERSION)
        {
            pPacket[9] = HCI_CONTROLLER_TARGET_CORE_VERSION;
        }
        return;
    }

#if HCI_CONTROLLER_TARGET_CORE_VERSION < HCI_CORE_VERSION_6_0
    if (opcode == HCI_CONTROLLER_OPCODE_READ_SUPPORTED_COMMANDS)
    {
        /*
         * Core 5.4 Vol 4 Part E 6.27 ends assigned command bits at octet 47
         * bit 1. Core 6.0 assigns octet 47 bits 2, 3 and 4 to Read All Local
         * Features, Read All Remote Features and Set Host Feature v2. nrfxlib
         * exposes those backend capabilities on nRF52, so hide them from the
         * 5.4 product profile.
         *
         * Six octets precede the 64-octet Supported_Commands parameter in a
         * Command Complete event: event header, command credit, opcode, status.
         */
        const size_t octet47 = 6U + 47U;
        if (PacketLen > octet47)
        {
            pPacket[octet47] &= (uint8_t)~0x1CU;
        }
    }
#endif
#else
    (void)Type;
    (void)pPacket;
    (void)PacketLen;
#endif
}

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

    if (Type == HCI_H4_PACKET_COMMAND && PacketLen >= 3U)
    {
        const uint16_t opcode = HciControllerReadLe16(pPacket);
        if (!HciControllerCommandAllowedByCore(opcode))
        {
            if (!HciControllerQueueUnknownCommand(pController, opcode))
            {
                pController->HostPacketRetryCount++;
                return false;
            }
            return true;
        }
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

    HciControllerApplyCoreProfile(type,
                                  pController->pControllerPacket,
                                  packetLen);

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
        pControllerPacket == nullptr ||
        ControllerPacketCapacity < HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE ||
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
