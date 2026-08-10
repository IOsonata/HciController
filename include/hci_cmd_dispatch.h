/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_CMD_DISPATCH_H
#define HCI_CMD_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_DISPATCH_COMMAND_HEADER_SIZE    3U
#define HCI_DISPATCH_EVENT_HEADER_SIZE      2U
#define HCI_COMMAND_COMPLETE_BASE_SIZE      6U
#define HCI_COMMAND_STATUS_SIZE             6U

#define HCI_EVENT_COMMAND_COMPLETE          0x0EU
#define HCI_EVENT_COMMAND_STATUS            0x0FU

#define HCI_STATUS_SUCCESS                  0x00U
#define HCI_STATUS_UNKNOWN_HCI_COMMAND      0x01U
#define HCI_STATUS_INVALID_HCI_PARAMETERS   0x12U
#define HCI_STATUS_MEMORY_CAPACITY_EXCEEDED 0x07U
#define HCI_STATUS_COMMAND_DISALLOWED       0x0CU

#define HCI_CMD_VARIABLE_PARAM_LEN          0xFFFFU

/*
 * An HCI event has a single byte length, and Command Complete spends four
 * of those on the header before the return parameters start.
 */
#define HCI_CMD_MAX_RETURN_LEN              251U

typedef enum {
    HCI_CMD_RESPONSE_COMPLETE = 0,
    HCI_CMD_RESPONSE_STATUS,
    HCI_CMD_RESPONSE_NONE,
} HciCmdResponse_t;

typedef struct {
    uint8_t Status;
    HciCmdResponse_t Response;
    size_t ReturnLen;
} HciCmdResult_t;

typedef HciCmdResult_t (*HciCmdHandler_t)(void *pContext,
                                          const uint8_t *pParams,
                                          size_t ParamLen,
                                          uint8_t *pReturn,
                                          size_t ReturnCapacity);

/*
 * One row per opcode.
 *
 * Response and ReturnLen describe what a successful call produces, and they
 * are what lets an error be answered in the same shape. Vol 4 Part E 7.7.15
 * requires a command whose success path is Command Status to answer with
 * Command Status when it fails, and the Return_Parameters list in each command
 * section is a fixed encoding rather than something conditional on the status
 * byte. Without these two fields the dispatcher can only guess, and guessing
 * produces a Command Complete of the wrong length that a host discards.
 *
 * ReturnLen is the number of return parameter bytes after the status byte, and
 * is zero for a Command Status entry.
 */
/*
 * Answers 0 to let a command through, or the status to refuse it with.
 */
typedef uint8_t (*HciCmdGuard_t)(void *pContext, uint16_t Opcode);

typedef struct {
    uint16_t Opcode;
    uint16_t ParamLen;
    uint16_t ReturnLen;
    HciCmdResponse_t Response;
    HciCmdHandler_t Handler;
} HciCmdEntry_t;

typedef struct {
    const HciCmdEntry_t *pEntries;
    size_t EntryCount;
    void *pContext;

    uint8_t *pEvent;
    size_t EventCapacity;
    size_t EventLen;
    bool EventPending;

    uint32_t CommandCount;
    uint32_t UnknownCommandCount;
    uint32_t InvalidPacketCount;
    uint32_t InvalidParamLenCount;
    uint32_t HandlerErrorCount;
    uint32_t EventBackpressureCount;

/*
 * What was answered, as the opcode and the status that went with it.
 *
 * The commands arriving are already recorded a layer down, and on this board
 * they all arrive and none is refused, so the question moved to what was said
 * back. A host that stops after six commands with nothing in its own log has
 * either been told something it did not like or been told it wrongly, and a
 * status of zero against every opcode rules out the first.
 *
 * Two octets of opcode and one of status, which is the whole of what a host
 * decides on for most commands. The return parameters are the vendor's own
 * answers and not this layer's to doubt.
 */
#define HCI_CMD_RSP_MARKS 24U
    struct {
        uint8_t Opcode[2];
        uint8_t Status;
    } RspMark[HCI_CMD_RSP_MARKS];
    uint8_t RspMarkLen;

    /*
     * Asked about every command with a row, after the length check and before
     * the handler. A non zero answer is the status the command is refused
     * with, and the handler is not called.
     *
     * The dispatcher has no opinion about what any opcode means, and this is
     * how it stays that way while still refusing a command for a reason that
     * needs opcodes to state. Vol 4 Part E 3.1.1 is the reason it exists: a
     * host may use the legacy advertising commands or the extended ones and
     * not both, and the one that comes second is Command Disallowed.
     */
    HciCmdGuard_t Guard;
} HciCmdDispatch_t;

/*
 * Queue the Command Complete for the No Operation opcode, which is how a
 * controller says it is ready to take commands. Vol 4 Part E 7.7.14 gives 0x0000
 * that meaning, and the event returns the command credit and the opcode and
 * nothing else: no status, no return parameters, so it is five octets where
 * every other Command Complete is at least six.
 *
 * A host may be waiting for it. Zephyr has BT_WAIT_NOP, which emits it from the
 * controller and waits for it in the host, and it is turned on per board rather
 * than always, so a host either wants this event or does not. One that is not
 * waiting takes it as a command credit it already had, which costs nothing.
 *
 * It goes out ahead of anything else because the dispatcher holds one event and
 * this one is queued before a command can arrive.
 */
void HciCmdDispatchQueueNop(HciCmdDispatch_t *pDispatch);

bool HciCmdDispatchInit(HciCmdDispatch_t *pDispatch,
                        const HciCmdEntry_t *pEntries,
                        size_t EntryCount,
                        void *pContext,
                        uint8_t *pEvent,
                        size_t EventCapacity);

/*
 * Submit one complete HCI Command packet without the H:4 type byte.
 * Returns false only when an earlier response is still pending.
 */
bool HciCmdDispatchPut(HciCmdDispatch_t *pDispatch,
                       const uint8_t *pPacket,
                       size_t PacketLen);

/*
 * Copy the pending HCI Event packet. Returns false when no event is pending or
 * when EventCapacity is too small. A successful call consumes the event.
 */
bool HciCmdDispatchGet(HciCmdDispatch_t *pDispatch,
                       uint8_t *pEvent,
                       size_t EventCapacity,
                       size_t *pEventLen);

bool HciCmdDispatchEventPending(const HciCmdDispatch_t *pDispatch);

/*
 * Whether this controller implements the opcode, and the length agrees with
 * what the table says the command takes.
 *
 * Not a substitute for dispatching, which still answers an opcode it does not
 * know with Unknown Command as the specification requires. This is for a
 * caller that has to decide whether a packet is a command at all, which is a
 * different question and one the octets cannot answer on their own: a stream
 * that is not H:4 produces packets that are well formed and mean nothing, and
 * the only thing separating them from real ones is that a real host sends
 * opcodes that exist.
 */
bool HciCmdDispatchKnows(const HciCmdDispatch_t *pDispatch,
                         uint16_t Opcode,
                         size_t ParamLen);

#ifdef __cplusplus
}
#endif

#endif /* HCI_CMD_DISPATCH_H */
