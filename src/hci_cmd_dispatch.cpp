/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_cmd_dispatch.h"

#include <string.h>

static uint16_t HciCmdReadLe16(const uint8_t *pData)
{
    return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

static const HciCmdEntry_t *HciCmdFindEntry(const HciCmdDispatch_t *pDispatch,
                                             uint16_t Opcode)
{
    for (size_t i = 0; i < pDispatch->EntryCount; ++i)
    {
        if (pDispatch->pEntries[i].Opcode == Opcode)
        {
            return &pDispatch->pEntries[i];
        }
    }

    return NULL;
}

bool HciCmdDispatchKnows(const HciCmdDispatch_t *pDispatch,
                         uint16_t Opcode,
                         size_t ParamLen)
{
    if (pDispatch == NULL)
    {
        return false;
    }

    const HciCmdEntry_t *pEntry = HciCmdFindEntry(pDispatch, Opcode);
    if (pEntry == NULL)
    {
        return false;
    }

    /*
     * A variable length command declares no length, so only the opcode can be
     * checked for those. Everything else has to agree, which is what makes
     * this worth asking at all: an opcode that exists by accident is unlikely
     * to arrive with the right number of octets behind it as well.
     */
    return pEntry->ParamLen == HCI_CMD_VARIABLE_PARAM_LEN ||
           pEntry->ParamLen == ParamLen;
}

static void HciCmdRecordRsp(HciCmdDispatch_t *pDispatch,
                            uint16_t Opcode,
                            uint8_t Status)
{
    if (pDispatch->RspMarkLen >= HCI_CMD_RSP_MARKS)
    {
        return;
    }

    const uint8_t slot = pDispatch->RspMarkLen;
    pDispatch->RspMark[slot].Opcode[0] = (uint8_t)Opcode;
    pDispatch->RspMark[slot].Opcode[1] = (uint8_t)(Opcode >> 8);
    pDispatch->RspMark[slot].Status = Status;
    pDispatch->RspMarkLen++;
}

static void HciCmdBuildComplete(HciCmdDispatch_t *pDispatch,
                                uint16_t Opcode,
                                uint8_t Status,
                                size_t ReturnLen)
{
    HciCmdRecordRsp(pDispatch, Opcode, Status);

    pDispatch->pEvent[0] = HCI_EVENT_COMMAND_COMPLETE;
    pDispatch->pEvent[1] = (uint8_t)(4U + ReturnLen);
    pDispatch->pEvent[2] = 1U;
    pDispatch->pEvent[3] = (uint8_t)Opcode;
    pDispatch->pEvent[4] = (uint8_t)(Opcode >> 8);
    pDispatch->pEvent[5] = Status;
    pDispatch->EventLen = HCI_COMMAND_COMPLETE_BASE_SIZE + ReturnLen;
    pDispatch->EventPending = true;
}

void HciCmdDispatchQueueNop(HciCmdDispatch_t *pDispatch)
{
    if (pDispatch == NULL || pDispatch->pEvent == NULL ||
        pDispatch->EventCapacity < 5U)
    {
        return;
    }

    /*
     * Five octets, not six. The No Operation opcode has no status byte and no
     * return parameters, so the parameter length is three: the credit and the
     * two opcode octets.
     */
    pDispatch->pEvent[0] = HCI_EVENT_COMMAND_COMPLETE;
    pDispatch->pEvent[1] = 3U;
    pDispatch->pEvent[2] = 1U;
    pDispatch->pEvent[3] = 0x00U;
    pDispatch->pEvent[4] = 0x00U;
    pDispatch->EventLen = 5U;
    pDispatch->EventPending = true;
}

/*
 * Zero the declared return parameter area and give back the length actually
 * used. A declared length the event buffer cannot hold collapses to zero
 * rather than writing past the end.
 */
static size_t HciCmdZeroReturn(HciCmdDispatch_t *pDispatch, size_t ReturnLen)
{
    const size_t capacity = pDispatch->EventCapacity -
                            HCI_COMMAND_COMPLETE_BASE_SIZE;

    if (ReturnLen > capacity || ReturnLen > HCI_CMD_MAX_RETURN_LEN)
    {
        return 0U;
    }

    memset(&pDispatch->pEvent[HCI_COMMAND_COMPLETE_BASE_SIZE], 0, ReturnLen);
    return ReturnLen;
}

static void HciCmdBuildStatus(HciCmdDispatch_t *pDispatch,
                              uint16_t Opcode,
                              uint8_t Status)
{
    HciCmdRecordRsp(pDispatch, Opcode, Status);

    pDispatch->pEvent[0] = HCI_EVENT_COMMAND_STATUS;
    pDispatch->pEvent[1] = 4U;
    pDispatch->pEvent[2] = Status;
    pDispatch->pEvent[3] = 1U;
    pDispatch->pEvent[4] = (uint8_t)Opcode;
    pDispatch->pEvent[5] = (uint8_t)(Opcode >> 8);
    pDispatch->EventLen = HCI_COMMAND_STATUS_SIZE;
    pDispatch->EventPending = true;
}

/*
 * Answer an error in the shape the opcode's success path would have taken.
 *
 * A Command Status opcode answers with Command Status. A Command Complete
 * opcode answers with its full declared return parameter length, zero filled,
 * because the Return_Parameters encoding in Vol 4 Part E is fixed and a host
 * that checks the length discards anything shorter.
 */
static void HciCmdBuildError(HciCmdDispatch_t *pDispatch,
                             const HciCmdEntry_t *pEntry,
                             uint16_t Opcode,
                             uint8_t Status)
{
    if (pEntry->Response == HCI_CMD_RESPONSE_STATUS)
    {
        HciCmdBuildStatus(pDispatch, Opcode, Status);
        return;
    }

    HciCmdBuildComplete(pDispatch, Opcode, Status,
                        HciCmdZeroReturn(pDispatch, pEntry->ReturnLen));
}

bool HciCmdDispatchInit(HciCmdDispatch_t *pDispatch,
                        const HciCmdEntry_t *pEntries,
                        size_t EntryCount,
                        void *pContext,
                        uint8_t *pEvent,
                        size_t EventCapacity)
{
    if (pDispatch == NULL || pEntries == NULL || EntryCount == 0U ||
        pEvent == NULL || EventCapacity < HCI_COMMAND_COMPLETE_BASE_SIZE)
    {
        return false;
    }

    memset(pDispatch, 0, sizeof(*pDispatch));
    pDispatch->pEntries = pEntries;
    pDispatch->EntryCount = EntryCount;
    pDispatch->pContext = pContext;
    pDispatch->pEvent = pEvent;
    pDispatch->EventCapacity = EventCapacity;

    return true;
}

bool HciCmdDispatchPut(HciCmdDispatch_t *pDispatch,
                       const uint8_t *pPacket,
                       size_t PacketLen)
{
    if (pDispatch == NULL || pPacket == NULL)
    {
        return false;
    }

    if (pDispatch->EventPending)
    {
        pDispatch->EventBackpressureCount++;
        return false;
    }

    pDispatch->CommandCount++;

    if (PacketLen < HCI_DISPATCH_COMMAND_HEADER_SIZE)
    {
        pDispatch->InvalidPacketCount++;
        return true;
    }

    const uint16_t opcode = HciCmdReadLe16(pPacket);
    const size_t paramLen = pPacket[2];

    /*
     * The entry comes first, because every rejection below has to be shaped by
     * the response kind this opcode would have used on success.
     */
    const HciCmdEntry_t *pEntry = HciCmdFindEntry(pDispatch, opcode);
    if (pEntry == NULL)
    {
        /*
         * An opcode with no entry gives nothing to shape the answer with, so
         * it takes the Command Complete form that a host expects for an
         * unsupported command.
         */
        pDispatch->UnknownCommandCount++;
        HciCmdBuildComplete(pDispatch, opcode, HCI_STATUS_UNKNOWN_HCI_COMMAND, 0U);
        return true;
    }

    if (PacketLen != HCI_DISPATCH_COMMAND_HEADER_SIZE + paramLen)
    {
        pDispatch->InvalidPacketCount++;
        HciCmdBuildError(pDispatch, pEntry, opcode,
                         HCI_STATUS_INVALID_HCI_PARAMETERS);
        return true;
    }

    if (pEntry->ParamLen != HCI_CMD_VARIABLE_PARAM_LEN &&
        pEntry->ParamLen != paramLen)
    {
        pDispatch->InvalidParamLenCount++;
        HciCmdBuildError(pDispatch, pEntry, opcode,
                         HCI_STATUS_INVALID_HCI_PARAMETERS);
        return true;
    }

    if (pEntry->Handler == NULL)
    {
        pDispatch->HandlerErrorCount++;
        HciCmdBuildError(pDispatch, pEntry, opcode,
                         HCI_STATUS_UNKNOWN_HCI_COMMAND);
        return true;
    }

    /*
     * After the length check, so a malformed block is still answered for what
     * is wrong with it rather than for the state it arrived in, and before
     * the handler, so a refused command never reaches the controller.
     */
    if (pDispatch->Guard != NULL)
    {
        const uint8_t refuse = pDispatch->Guard(pDispatch->pContext, opcode);
        if (refuse != HCI_STATUS_SUCCESS)
        {
            HciCmdBuildError(pDispatch, pEntry, opcode, refuse);
            return true;
        }
    }

    uint8_t *pReturn = &pDispatch->pEvent[HCI_COMMAND_COMPLETE_BASE_SIZE];
    const size_t returnCapacity = pDispatch->EventCapacity - HCI_COMMAND_COMPLETE_BASE_SIZE;
    HciCmdResult_t result = pEntry->Handler(pDispatch->pContext,
                                            &pPacket[HCI_DISPATCH_COMMAND_HEADER_SIZE],
                                            paramLen,
                                            pReturn,
                                            returnCapacity);

    if (result.ReturnLen > returnCapacity ||
        result.ReturnLen > HCI_CMD_MAX_RETURN_LEN)
    {
        pDispatch->HandlerErrorCount++;
        HciCmdBuildError(pDispatch, pEntry, opcode,
                         HCI_STATUS_MEMORY_CAPACITY_EXCEEDED);
        return true;
    }

    switch (result.Response)
    {
        case HCI_CMD_RESPONSE_COMPLETE:
            /*
             * A handler that failed writes no return parameters, but the
             * encoding is fixed, so pad out to the declared length with zeros
             * rather than emitting a short event the host will discard. Only
             * the gap is written, so a partial return is left alone.
             */
            if (result.ReturnLen < pEntry->ReturnLen &&
                pEntry->ReturnLen <= returnCapacity &&
                pEntry->ReturnLen <= HCI_CMD_MAX_RETURN_LEN)
            {
                memset(&pReturn[result.ReturnLen], 0,
                       pEntry->ReturnLen - result.ReturnLen);
                result.ReturnLen = pEntry->ReturnLen;
            }
            HciCmdBuildComplete(pDispatch, opcode, result.Status, result.ReturnLen);
            break;

        case HCI_CMD_RESPONSE_STATUS:
            HciCmdBuildStatus(pDispatch, opcode, result.Status);
            break;

        case HCI_CMD_RESPONSE_NONE:
            pDispatch->EventLen = 0U;
            pDispatch->EventPending = false;
            break;

        default:
            pDispatch->HandlerErrorCount++;
            HciCmdBuildComplete(pDispatch, opcode, HCI_STATUS_UNKNOWN_HCI_COMMAND, 0U);
            break;
    }

    return true;
}

bool HciCmdDispatchGet(HciCmdDispatch_t *pDispatch,
                       uint8_t *pEvent,
                       size_t EventCapacity,
                       size_t *pEventLen)
{
    if (pDispatch == NULL || pEvent == NULL || pEventLen == NULL ||
        !pDispatch->EventPending || EventCapacity < pDispatch->EventLen)
    {
        return false;
    }

    memcpy(pEvent, pDispatch->pEvent, pDispatch->EventLen);
    *pEventLen = pDispatch->EventLen;
    pDispatch->EventLen = 0U;
    pDispatch->EventPending = false;

    return true;
}

bool HciCmdDispatchEventPending(const HciCmdDispatch_t *pDispatch)
{
    return pDispatch != NULL && pDispatch->EventPending;
}
