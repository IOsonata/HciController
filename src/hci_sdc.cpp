/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_sdc.h"

#include <string.h>

static uint16_t HciSdcReadLe16(const uint8_t *pData)
{
    return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

static bool HciSdcPacketLength(HciH4PacketType_t Type,
                               const uint8_t *pPacket,
                               size_t PacketCapacity,
                               size_t *pPacketLen)
{
    size_t packetLen = 0U;

    switch (Type)
    {
        case HCI_H4_PACKET_EVENT:
            if (PacketCapacity < 2U)
            {
                return false;
            }
            packetLen = 2U + pPacket[1];
            break;

        case HCI_H4_PACKET_ACL:
            if (PacketCapacity < 4U)
            {
                return false;
            }
            packetLen = 4U + HciSdcReadLe16(&pPacket[2]);
            break;

        case HCI_H4_PACKET_ISO:
            if (PacketCapacity < 4U)
            {
                return false;
            }
            packetLen = 4U + (HciSdcReadLe16(&pPacket[2]) & 0x3FFFU);
            break;

        default:
            return false;
    }

    if (packetLen > PacketCapacity)
    {
        return false;
    }

    *pPacketLen = packetLen;
    return true;
}

/*
 * Remember a flow control credit owed back for an ACL packet the controller
 * would not take. The connection handle is the low 12 bits of the first two
 * octets of the ACL header, Vol 4 Part E 5.4.2.
 */
static void HciSdcOweCredit(HciSdc_t *pSdc, const uint8_t *pPacket)
{
    const uint16_t handle = (uint16_t)(((uint16_t)pPacket[0] |
                                        ((uint16_t)pPacket[1] << 8)) & 0x0FFFU);

    for (uint8_t i = 0U; i < pSdc->CreditEntries; i++)
    {
        if (pSdc->CreditHandle[i] == handle)
        {
            pSdc->CreditCount[i]++;
            return;
        }
    }

    if (pSdc->CreditEntries >= HCI_SDC_CREDIT_HANDLES)
    {
        /*
         * More links owing credits than the table holds. Dropping the credit
         * costs the host one buffer on that link, which is the same failure
         * this function exists to avoid, so it is counted rather than hidden.
         */
        pSdc->CreditOverflowCount++;
        return;
    }

    pSdc->CreditHandle[pSdc->CreditEntries] = handle;
    pSdc->CreditCount[pSdc->CreditEntries] = 1U;
    pSdc->CreditEntries++;
}

/*
 * Build the Number Of Completed Packets event for everything owed, Vol 4 Part
 * E 7.7.19. One event covers every handle, so the table empties in one go.
 */
static bool HciSdcBuildCreditEvent(HciSdc_t *pSdc,
                                   uint8_t *pPacket,
                                   size_t PacketCapacity,
                                   size_t *pPacketLen)
{
    const size_t len = 3U + ((size_t)pSdc->CreditEntries * 4U);

    if (pSdc->CreditEntries == 0U || len > PacketCapacity)
    {
        return false;
    }

    pPacket[0] = HCI_SDC_EVENT_NUM_COMPLETED_PACKETS;
    pPacket[1] = (uint8_t)(1U + ((size_t)pSdc->CreditEntries * 4U));
    pPacket[2] = pSdc->CreditEntries;

    size_t at = 3U;
    for (uint8_t i = 0U; i < pSdc->CreditEntries; i++)
    {
        pPacket[at++] = (uint8_t)pSdc->CreditHandle[i];
        pPacket[at++] = (uint8_t)(pSdc->CreditHandle[i] >> 8);
        pPacket[at++] = (uint8_t)pSdc->CreditCount[i];
        pPacket[at++] = (uint8_t)(pSdc->CreditCount[i] >> 8);
    }

    pSdc->CreditEntries = 0U;
    *pPacketLen = len;
    return true;
}

static bool HciSdcPutPacket(void *pContext,
                            HciH4PacketType_t Type,
                            const uint8_t *pPacket,
                            size_t PacketLen)
{
    HciSdc_t *pSdc = static_cast<HciSdc_t *>(pContext);

    switch (Type)
    {
        case HCI_H4_PACKET_COMMAND:
            /*
             * Hold the next command until the controller queue has had the
             * outgoing slot. Without this a host that keeps a command in
             * flight, which it is entitled to do because every Command
             * Complete hands back a credit, produces a fresh command event on
             * every pass and sdc_hci_get is never reached. Refusing here is
             * ordinary backpressure: the parser keeps the packet and offers it
             * again next pass.
             */
            if (pSdc->CommandEventLast)
            {
                pSdc->CommandDeferredCount++;
                return false;
            }
            return HciCmdDispatchPut(&pSdc->Commands, pPacket, PacketLen);

        case HCI_H4_PACKET_ACL:
        {
            /*
             * The parser frames the packet but does not judge its size. The
             * controller advertises a maximum payload in LE Read Buffer Size
             * and a host is not entitled to exceed it, so refuse an oversize
             * packet here rather than let the controller decide what to do
             * with it. The credit still has to come back, Vol 4 Part E 4.1.1.
             */
            if (PacketLen < HCI_SDC_ACL_HEADER_SIZE ||
                PacketLen - HCI_SDC_ACL_HEADER_SIZE > HCI_SDC_ACL_MAX_PAYLOAD)
            {
                pSdc->AclOversizeCount++;
                if (PacketLen >= HCI_SDC_ACL_HEADER_SIZE)
                {
                    HciSdcOweCredit(pSdc, pPacket);
                }
                return true;
            }

            int32_t result = pSdc->Ops.AclPut(pSdc->Ops.pContext, pPacket);
            if (result == pSdc->Ops.RetryError)
            {
                pSdc->PutRetryCount++;
                return false;
            }
            if (result != 0)
            {
                /*
                 * The packet is gone but the host still spent a buffer on it,
                 * and nothing else will ever hand that buffer back. Owe the
                 * credit here so it is returned in the next Number Of
                 * Completed Packets event.
                 */
                pSdc->AclPutErrorCount++;
                HciSdcOweCredit(pSdc, pPacket);
            }
            return true;
        }

        case HCI_H4_PACKET_ISO:
        {
            if (pSdc->Ops.IsoPut == NULL)
            {
                pSdc->IsoPutErrorCount++;
                return true;
            }

            int32_t result = pSdc->Ops.IsoPut(pSdc->Ops.pContext, pPacket);
            if (result == pSdc->Ops.RetryError)
            {
                /*
                 * For ISO the shared retry code means the SDU arrived too late
                 * for its transmission point, which retrying can only make
                 * worse. Refusing would pin the packet in the parser and stop
                 * the whole host to controller direction, so it is dropped.
                 * ISO has no packet based flow control, so no credit is owed.
                 */
                pSdc->IsoDropCount++;
                return true;
            }
            if (result != 0)
            {
                pSdc->IsoPutErrorCount++;
            }
            return true;
        }

        default:
            return true;
    }
}

static HciControllerGetResult_t HciSdcGetCommandEvent(HciSdc_t *pSdc,
                                                       HciH4PacketType_t *pType,
                                                       uint8_t *pPacket,
                                                       size_t PacketCapacity,
                                                       size_t *pPacketLen)
{
    if (!HciCmdDispatchGet(&pSdc->Commands, pPacket, PacketCapacity,
                           pPacketLen))
    {
        pSdc->InvalidOutputLengthCount++;
        return HCI_CONTROLLER_GET_ERROR;
    }

    pSdc->CommandEventLast = true;
    *pType = HCI_H4_PACKET_EVENT;
    return HCI_CONTROLLER_GET_PACKET;
}

static HciControllerGetResult_t HciSdcGetPacket(void *pContext,
                                                 HciH4PacketType_t *pType,
                                                 uint8_t *pPacket,
                                                 size_t PacketCapacity,
                                                 size_t *pPacketLen)
{
    HciSdc_t *pSdc = static_cast<HciSdc_t *>(pContext);

    /*
     * A pending command event always goes first. A command handler runs while
     * the command is accepted, so anything it queues in the controller was
     * queued after the response was built, and Vol 4 Part E 7.8.13 and the
     * general rule in 4.4 require the response to reach the host first. Fair
     * sharing of the outgoing slot is handled where the command is accepted,
     * not here, so that nothing can overtake a response.
     */
    if (HciCmdDispatchEventPending(&pSdc->Commands))
    {
        return HciSdcGetCommandEvent(pSdc, pType, pPacket, PacketCapacity,
                                     pPacketLen);
    }

    /*
     * Credits owed for refused ACL packets go out before the controller queue
     * is asked. They are bounded by the number of packets the host had in
     * flight, so this cannot starve the queue.
     */
    if (HciSdcBuildCreditEvent(pSdc, pPacket, PacketCapacity, pPacketLen))
    {
        *pType = HCI_H4_PACKET_EVENT;
        return HCI_CONTROLLER_GET_PACKET;
    }

    uint8_t sdcType = HCI_SDC_MSG_TYPE_NONE;
    int32_t result = pSdc->Ops.Get(pSdc->Ops.pContext, pPacket, &sdcType);

    /* The controller queue has had its turn, so the next command may come in. */
    pSdc->CommandEventLast = false;

    if (result == pSdc->Ops.RetryError)
    {
        return HCI_CONTROLLER_GET_EMPTY;
    }

    if (result != 0)
    {
        pSdc->GetErrorCount++;
        return HCI_CONTROLLER_GET_ERROR;
    }

    switch (sdcType)
    {
        case HCI_SDC_MSG_TYPE_EVENT:
            *pType = HCI_H4_PACKET_EVENT;
            break;

        case HCI_SDC_MSG_TYPE_ACL:
            *pType = HCI_H4_PACKET_ACL;
            break;

        case HCI_SDC_MSG_TYPE_ISO:
            *pType = HCI_H4_PACKET_ISO;
            break;

        default:
            pSdc->InvalidOutputTypeCount++;
            return HCI_CONTROLLER_GET_ERROR;
    }

    if (!HciSdcPacketLength(*pType, pPacket, PacketCapacity, pPacketLen))
    {
        pSdc->InvalidOutputLengthCount++;
        return HCI_CONTROLLER_GET_ERROR;
    }

    return HCI_CONTROLLER_GET_PACKET;
}

static void HciSdcProcessBackend(void *pContext)
{
    HciSdc_t *pSdc = static_cast<HciSdc_t *>(pContext);
    if (pSdc->Ops.Process != NULL)
    {
        pSdc->Ops.Process(pSdc->Ops.pContext);
    }
}

bool HciSdcInit(HciSdc_t *pSdc,
                const HciSdcOps_t *pOps,
                const HciCmdEntry_t *pCommands,
                size_t CommandCount,
                void *pCommandContext,
                uint8_t *pCommandEvent,
                size_t CommandEventCapacity)
{
    if (pSdc == NULL || pOps == NULL || pOps->AclPut == NULL || pOps->Get == NULL)
    {
        return false;
    }

    memset(pSdc, 0, sizeof(*pSdc));
    pSdc->Ops = *pOps;
    if (pSdc->Ops.RetryError == 0)
    {
        pSdc->Ops.RetryError = HCI_SDC_RETRY_ERROR;
    }

    if (!HciCmdDispatchInit(&pSdc->Commands,
                            pCommands,
                            CommandCount,
                            pCommandContext,
                            pCommandEvent,
                            CommandEventCapacity))
    {
        return false;
    }

    pSdc->ControllerOps.Put = HciSdcPutPacket;
    pSdc->ControllerOps.Get = HciSdcGetPacket;
    pSdc->ControllerOps.Process = HciSdcProcessBackend;
    pSdc->ControllerOps.pContext = pSdc;

    return true;
}

static void HciSdcWriteLe32(uint8_t *pData, uint32_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
    pData[2] = (uint8_t)(Value >> 16);
    pData[3] = (uint8_t)(Value >> 24);
}

HciCmdResult_t HciSdcCmdReadCounters(void *pContext,
                                     const uint8_t *,
                                     size_t,
                                     uint8_t *pReturn,
                                     size_t ReturnCapacity)
{
    /*
     * Command Disallowed is the answer when the table carrying this row was
     * given some other command context, since the alternative is to read
     * whatever that context happens to point at and report it as counters.
     */
    HciCmdResult_t result = {HCI_STATUS_COMMAND_DISALLOWED,
                             HCI_CMD_RESPONSE_COMPLETE, 0U};

    const HciSdc_t *pSdc = static_cast<const HciSdc_t *>(pContext);
    if (pSdc == NULL || pReturn == NULL)
    {
        return result;
    }

    if (ReturnCapacity < HCI_SDC_COUNTERS_RETURN_LEN)
    {
        result.Status = HCI_STATUS_MEMORY_CAPACITY_EXCEEDED;
        return result;
    }

    /* Order fixed by the header. Appending is safe, renumbering is not. */
    const uint32_t counters[HCI_SDC_COUNTERS_COUNT] = {
        pSdc->Commands.CommandCount,
        pSdc->Commands.UnknownCommandCount,
        pSdc->Commands.InvalidPacketCount,
        pSdc->Commands.InvalidParamLenCount,
        pSdc->Commands.HandlerErrorCount,
        pSdc->Commands.EventBackpressureCount,
        pSdc->AclPutErrorCount,
        pSdc->IsoPutErrorCount,
        pSdc->PutRetryCount,
        pSdc->GetErrorCount,
        pSdc->InvalidOutputTypeCount,
        pSdc->InvalidOutputLengthCount,
        pSdc->CommandDeferredCount,
        pSdc->AclOversizeCount,
        pSdc->IsoDropCount,
        pSdc->CreditOverflowCount,
    };

    pReturn[0] = (uint8_t)HCI_SDC_COUNTERS_VERSION;
    for (size_t i = 0U; i < HCI_SDC_COUNTERS_COUNT; i++)
    {
        HciSdcWriteLe32(&pReturn[1U + (i * 4U)], counters[i]);
    }

    result.Status = HCI_STATUS_SUCCESS;
    result.ReturnLen = HCI_SDC_COUNTERS_RETURN_LEN;
    return result;
}

const HciControllerOps_t *HciSdcGetControllerOps(HciSdc_t *pSdc)
{
    return pSdc != NULL ? &pSdc->ControllerOps : NULL;
}
