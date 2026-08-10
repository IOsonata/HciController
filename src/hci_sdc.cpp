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

/* -------------------------------------------------------------------------
 * Core HCI compatibility commands not exported by the vendor controller.
 * ------------------------------------------------------------------------- */

static HciCmdResult_t HciSdcCompatReadSupportedStates(void *,
                                                       const uint8_t *,
                                                       size_t,
                                                       uint8_t *pReturn,
                                                       size_t ReturnCapacity)
{
    /*
     * Core 5.4 Vol 4 Part E 7.8.27. The configured multirole SDC supports
     * advertising, scanning, initiating and connection state in both roles,
     * including the simultaneous combinations defined by the legacy 42-bit LE
     * States field. Bits 42..63 are reserved and remain zero.
     */
    static const uint8_t states[8] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x03U, 0x00U, 0x00U
    };

    if (ReturnCapacity < sizeof(states))
    {
        HciCmdResult_t error = {
            HCI_STATUS_MEMORY_CAPACITY_EXCEEDED,
            HCI_CMD_RESPONSE_COMPLETE,
            0U,
        };
        return error;
    }

    memcpy(pReturn, states, sizeof(states));
    HciCmdResult_t result = {
        HCI_STATUS_SUCCESS,
        HCI_CMD_RESPONSE_COMPLETE,
        sizeof(states),
    };
    return result;
}

static const HciCmdEntry_t s_HciSdcCompatCommands[] = {
    {HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES,
     0U,
     8U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcCompatReadSupportedStates},
};

/*
 * Read Local Supported Commands is supplied by nrfxlib, but the bit for the
 * compatibility command above must describe the complete HCI controller, not
 * merely the vendor library. Vol 4 Part E 6.27 assigns LE Read Supported
 * States to octet 28 bit 3.
 */
static void HciSdcPatchSupportedCommands(uint8_t *pEvent, size_t EventLen)
{
    const uint16_t readSupportedCommands = 0x1002U;
    const size_t supportedByte = HCI_COMMAND_COMPLETE_BASE_SIZE + 28U;

    if (EventLen <= supportedByte || pEvent[0] != HCI_EVENT_COMMAND_COMPLETE ||
        HciSdcReadLe16(&pEvent[3]) != readSupportedCommands ||
        pEvent[5] != HCI_STATUS_SUCCESS)
    {
        return;
    }

    pEvent[supportedByte] |= (1U << 3);
}

bool HciSdcKnowsCommand(const HciSdc_t *pSdc,
                        uint16_t Opcode,
                        size_t ParamLen)
{
    if (pSdc == NULL)
    {
        return false;
    }

    return HciCmdDispatchKnows(&pSdc->Commands, Opcode, ParamLen) ||
           HciCmdDispatchKnows(&pSdc->CompatCommands, Opcode, ParamLen);
}

/* -------------------------------------------------------------------------
 * ACL credit accounting.
 * ------------------------------------------------------------------------- */

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
        pSdc->CreditOverflowCount++;
        return;
    }

    pSdc->CreditHandle[pSdc->CreditEntries] = handle;
    pSdc->CreditCount[pSdc->CreditEntries] = 1U;
    pSdc->CreditEntries++;
}

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

void HciSdcSetAclLimit(HciSdc_t *pSdc, uint16_t Limit)
{
    if (pSdc != NULL)
    {
        pSdc->AclLimit = Limit;
    }
}

void HciSdcResetFlowControl(HciSdc_t *pSdc)
{
    if (pSdc == NULL)
    {
        return;
    }

    pSdc->AclTrackEntries = 0U;
    pSdc->AclOutstandingTotal = 0U;
    pSdc->CreditEntries = 0U;
    pSdc->CreditEventLast = false;
}

static uint16_t HciSdcHandleOf(const uint8_t *pPacket)
{
    return (uint16_t)(((uint16_t)pPacket[0] |
                       ((uint16_t)pPacket[1] << 8)) & 0x0FFFU);
}

static int HciSdcAclSlot(HciSdc_t *pSdc, uint16_t Handle, bool Create)
{
    for (uint8_t i = 0U; i < pSdc->AclTrackEntries; i++)
    {
        if (pSdc->AclTrackHandle[i] == Handle)
        {
            return (int)i;
        }
    }

    if (!Create)
    {
        return -1;
    }

    if (pSdc->AclTrackEntries >= HCI_SDC_ACL_TRACK_HANDLES)
    {
        pSdc->AclTrackOverflowCount++;
        return -1;
    }

    pSdc->AclTrackHandle[pSdc->AclTrackEntries] = Handle;
    pSdc->AclOutstanding[pSdc->AclTrackEntries] = 0U;
    pSdc->AclTrackEntries++;
    return (int)(pSdc->AclTrackEntries - 1U);
}

#if HCI_SDC_ENFORCE_ACL_CREDITS
static bool HciSdcAclAtLimit(HciSdc_t *pSdc, const uint8_t *pPacket)
{
    if (pSdc->AclLimit == 0U)
    {
        return false;
    }

    const int slot = HciSdcAclSlot(pSdc, HciSdcHandleOf(pPacket), true);
    if (slot < 0)
    {
        return false;
    }

    return pSdc->AclOutstandingTotal >= pSdc->AclLimit;
}
#endif

static void HciSdcAclPutTracked(HciSdc_t *pSdc, const uint8_t *pPacket)
{
    const int slot = HciSdcAclSlot(pSdc, HciSdcHandleOf(pPacket), true);
    if (slot >= 0)
    {
        pSdc->AclOutstanding[slot]++;
        pSdc->AclOutstandingTotal++;
    }
}

static void HciSdcAclForget(HciSdc_t *pSdc, uint16_t Handle)
{
    const int slot = HciSdcAclSlot(pSdc, Handle, false);
    if (slot < 0)
    {
        return;
    }

    if (pSdc->AclOutstandingTotal > pSdc->AclOutstanding[slot])
    {
        pSdc->AclOutstandingTotal =
            (uint16_t)(pSdc->AclOutstandingTotal - pSdc->AclOutstanding[slot]);
    }
    else
    {
        pSdc->AclOutstandingTotal = 0U;
    }

    const uint8_t last = (uint8_t)(pSdc->AclTrackEntries - 1U);
    pSdc->AclTrackHandle[slot] = pSdc->AclTrackHandle[last];
    pSdc->AclOutstanding[slot] = pSdc->AclOutstanding[last];
    pSdc->AclTrackEntries--;
}

static void HciSdcAclTrackEvent(HciSdc_t *pSdc,
                                const uint8_t *pEvent,
                                size_t EventLen)
{
    if (EventLen < 2U)
    {
        return;
    }

    if (pEvent[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS && EventLen >= 3U)
    {
        const size_t handles = pEvent[2];
        for (size_t i = 0U; i < handles; i++)
        {
            const size_t off = 3U + (i * 4U);
            if (off + 4U > EventLen)
            {
                return;
            }

            const uint16_t handle =
                (uint16_t)(((uint16_t)pEvent[off] |
                            ((uint16_t)pEvent[off + 1U] << 8)) & 0x0FFFU);
            const uint16_t done = (uint16_t)((uint16_t)pEvent[off + 2U] |
                                             ((uint16_t)pEvent[off + 3U] << 8));

            const int slot = HciSdcAclSlot(pSdc, handle, false);
            if (slot < 0)
            {
                continue;
            }

            const uint16_t freed = pSdc->AclOutstanding[slot] < done
                                       ? pSdc->AclOutstanding[slot]
                                       : done;

            pSdc->AclOutstanding[slot] =
                (uint16_t)(pSdc->AclOutstanding[slot] - freed);
            pSdc->AclOutstandingTotal =
                (uint16_t)(pSdc->AclOutstandingTotal - freed);
        }
        return;
    }

    if (pEvent[0] == HCI_SDC_EVENT_DISCONNECTION_COMPLETE && EventLen >= 5U)
    {
        if (pEvent[2] != HCI_STATUS_SUCCESS)
        {
            return;
        }

        const uint16_t handle =
            (uint16_t)(((uint16_t)pEvent[3] |
                        ((uint16_t)pEvent[4] << 8)) & 0x0FFFU);
        HciSdcAclForget(pSdc, handle);
    }
}

/* -------------------------------------------------------------------------
 * Controller routing.
 * ------------------------------------------------------------------------- */

static bool HciSdcPutPacket(void *pContext,
                            HciH4PacketType_t Type,
                            const uint8_t *pPacket,
                            size_t PacketLen)
{
    HciSdc_t *pSdc = static_cast<HciSdc_t *>(pContext);

    switch (Type)
    {
        case HCI_H4_PACKET_COMMAND:
        {
            /*
             * There are two command dispatchers now, so the pending check is
             * across both. Otherwise a vendor command could occupy one event
             * slot while a compatibility command occupies the other, granting
             * a second command credit the controller never intended to grant.
             */
            if (pSdc->CommandEventLast ||
                HciCmdDispatchEventPending(&pSdc->Commands) ||
                HciCmdDispatchEventPending(&pSdc->CompatCommands))
            {
                pSdc->CommandDeferredCount++;
                return false;
            }

            if (PacketLen >= HCI_DISPATCH_COMMAND_HEADER_SIZE)
            {
                const uint16_t opcode = HciSdcReadLe16(pPacket);
                if (opcode == HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES)
                {
                    return HciCmdDispatchPut(&pSdc->CompatCommands,
                                             pPacket, PacketLen);
                }
            }

            return HciCmdDispatchPut(&pSdc->Commands, pPacket, PacketLen);
        }

        case HCI_H4_PACKET_ACL:
        {
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

#if HCI_SDC_ENFORCE_ACL_CREDITS
            if (HciSdcAclAtLimit(pSdc, pPacket))
            {
                pSdc->AclCreditOverrunCount++;
                HciSdcOweCredit(pSdc, pPacket);
                return true;
            }
#endif

            int32_t result = pSdc->Ops.AclPut(pSdc->Ops.pContext, pPacket);
            if (result == pSdc->Ops.RetryError)
            {
                pSdc->PutRetryCount++;
                return false;
            }
            if (result != 0)
            {
                pSdc->AclPutErrorCount++;
                HciSdcOweCredit(pSdc, pPacket);
            }
            else
            {
                pSdc->AclPutCount++;
                HciSdcAclPutTracked(pSdc, pPacket);
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
                pSdc->IsoDropCount++;
                return true;
            }
            if (result != 0)
            {
                pSdc->IsoPutErrorCount++;
            }
            else
            {
                pSdc->IsoPutCount++;
            }
            return true;
        }

        default:
            return true;
    }
}

static HciControllerGetResult_t HciSdcGetDispatchEvent(
    HciSdc_t *pSdc,
    HciCmdDispatch_t *pDispatch,
    bool PatchSupportedCommands,
    HciH4PacketType_t *pType,
    uint8_t *pPacket,
    size_t PacketCapacity,
    size_t *pPacketLen)
{
    if (!HciCmdDispatchGet(pDispatch, pPacket, PacketCapacity, pPacketLen))
    {
        pSdc->InvalidOutputLengthCount++;
        return HCI_CONTROLLER_GET_ERROR;
    }

    if (PatchSupportedCommands)
    {
        HciSdcPatchSupportedCommands(pPacket, *pPacketLen);
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

    if (HciCmdDispatchEventPending(&pSdc->Commands))
    {
        return HciSdcGetDispatchEvent(pSdc, &pSdc->Commands, true,
                                      pType, pPacket, PacketCapacity,
                                      pPacketLen);
    }

    if (HciCmdDispatchEventPending(&pSdc->CompatCommands))
    {
        return HciSdcGetDispatchEvent(pSdc, &pSdc->CompatCommands, false,
                                      pType, pPacket, PacketCapacity,
                                      pPacketLen);
    }

    if (!pSdc->CreditEventLast &&
        HciSdcBuildCreditEvent(pSdc, pPacket, PacketCapacity, pPacketLen))
    {
        pSdc->CreditEventLast = true;
        *pType = HCI_H4_PACKET_EVENT;
        return HCI_CONTROLLER_GET_PACKET;
    }

    uint8_t sdcType = HCI_SDC_MSG_TYPE_NONE;
    int32_t result = pSdc->Ops.Get(pSdc->Ops.pContext, pPacket, &sdcType);

    pSdc->CommandEventLast = false;
    pSdc->CreditEventLast = false;

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

    if (*pType == HCI_H4_PACKET_EVENT)
    {
        HciSdcAclTrackEvent(pSdc, pPacket, *pPacketLen);
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

    if (!HciCmdDispatchInit(
            &pSdc->CompatCommands,
            s_HciSdcCompatCommands,
            sizeof(s_HciSdcCompatCommands) / sizeof(s_HciSdcCompatCommands[0]),
            pSdc,
            pSdc->CompatEvent,
            sizeof(pSdc->CompatEvent)))
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
