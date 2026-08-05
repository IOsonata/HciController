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
            int32_t result = pSdc->Ops.AclPut(pSdc->Ops.pContext, pPacket);
            if (result == pSdc->Ops.RetryError)
            {
                pSdc->PutRetryCount++;
                return false;
            }
            if (result != 0)
            {
                pSdc->AclPutErrorCount++;
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
                pSdc->PutRetryCount++;
                return false;
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

const HciControllerOps_t *HciSdcGetControllerOps(HciSdc_t *pSdc)
{
    return pSdc != NULL ? &pSdc->ControllerOps : NULL;
}
