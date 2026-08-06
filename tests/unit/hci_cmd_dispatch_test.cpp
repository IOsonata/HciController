#include "hci_cmd_dispatch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct TestContext
{
    unsigned Calls;
    uint8_t LastParam;
};

static HciCmdResult_t CompleteHandler(void *pContext,
                                      const uint8_t *pParams,
                                      size_t ParamLen,
                                      uint8_t *pReturn,
                                      size_t ReturnCapacity)
{
    TestContext *ctx = static_cast<TestContext *>(pContext);
    assert(ParamLen == 1U);
    assert(ReturnCapacity >= 2U);
    ctx->Calls++;
    ctx->LastParam = pParams[0];
    pReturn[0] = 0xAAU;
    pReturn[1] = 0x55U;
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 2U};
}

static HciCmdResult_t StatusHandler(void *,
                                    const uint8_t *,
                                    size_t,
                                    uint8_t *,
                                    size_t)
{
    return {0x0CU, HCI_CMD_RESPONSE_STATUS, 0U};
}

static HciCmdResult_t NoEventHandler(void *,
                                     const uint8_t *,
                                     size_t,
                                     uint8_t *,
                                     size_t)
{
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_NONE, 0U};
}

int main()
{
    static const HciCmdEntry_t entries[] = {
        {0x1234U, 1U, 2U, HCI_CMD_RESPONSE_COMPLETE, CompleteHandler},
        {0x2345U, 0U, 0U, HCI_CMD_RESPONSE_STATUS, StatusHandler},
        {0x3456U, HCI_CMD_VARIABLE_PARAM_LEN, 0U, HCI_CMD_RESPONSE_NONE,
         NoEventHandler},
    };

    TestContext context = {};
    uint8_t eventStorage[80];
    HciCmdDispatch_t dispatch;
    assert(HciCmdDispatchInit(&dispatch,
                              entries,
                              sizeof(entries) / sizeof(entries[0]),
                              &context,
                              eventStorage,
                              sizeof(eventStorage)));

    const uint8_t completeCmd[] = {0x34U, 0x12U, 0x01U, 0x77U};
    assert(HciCmdDispatchPut(&dispatch, completeCmd, sizeof(completeCmd)));
    assert(context.Calls == 1U && context.LastParam == 0x77U);
    assert(HciCmdDispatchEventPending(&dispatch));

    assert(!HciCmdDispatchPut(&dispatch, completeCmd, sizeof(completeCmd)));
    assert(dispatch.EventBackpressureCount == 1U);

    uint8_t event[80];
    size_t eventLen = 0U;
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    const uint8_t expectedComplete[] = {
        HCI_EVENT_COMMAND_COMPLETE, 0x06U, 0x01U,
        0x34U, 0x12U, HCI_STATUS_SUCCESS, 0xAAU, 0x55U,
    };
    assert(eventLen == sizeof(expectedComplete));
    assert(memcmp(event, expectedComplete, sizeof(expectedComplete)) == 0);

    const uint8_t statusCmd[] = {0x45U, 0x23U, 0x00U};
    assert(HciCmdDispatchPut(&dispatch, statusCmd, sizeof(statusCmd)));
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    const uint8_t expectedStatus[] = {
        HCI_EVENT_COMMAND_STATUS, 0x04U, 0x0CU, 0x01U, 0x45U, 0x23U,
    };
    assert(eventLen == sizeof(expectedStatus));
    assert(memcmp(event, expectedStatus, sizeof(expectedStatus)) == 0);

    const uint8_t unknownCmd[] = {0xEFU, 0xBEU, 0x00U};
    assert(HciCmdDispatchPut(&dispatch, unknownCmd, sizeof(unknownCmd)));
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    assert(event[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(event[3] == 0xEFU && event[4] == 0xBEU);
    assert(event[5] == HCI_STATUS_UNKNOWN_HCI_COMMAND);
    assert(dispatch.UnknownCommandCount == 1U);

    const uint8_t badLenCmd[] = {0x34U, 0x12U, 0x00U};
    assert(HciCmdDispatchPut(&dispatch, badLenCmd, sizeof(badLenCmd)));
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    assert(event[5] == HCI_STATUS_INVALID_HCI_PARAMETERS);
    assert(dispatch.InvalidParamLenCount == 1U);

    const uint8_t truncatedCmd[] = {0x34U, 0x12U, 0x01U};
    assert(HciCmdDispatchPut(&dispatch, truncatedCmd, sizeof(truncatedCmd)));
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    assert(event[5] == HCI_STATUS_INVALID_HCI_PARAMETERS);
    assert(dispatch.InvalidPacketCount == 1U);

    const uint8_t noEventCmd[] = {0x56U, 0x34U, 0x02U, 0x01U, 0x02U};
    assert(HciCmdDispatchPut(&dispatch, noEventCmd, sizeof(noEventCmd)));
    assert(!HciCmdDispatchEventPending(&dispatch));

    /*
     * The No Operation Command Complete a controller uses to say it is ready.
     * Vol 4 Part E 7.7.14 gives opcode 0x0000 that meaning, and the event has
     * no status and no return parameters, so it is five octets where every
     * other Command Complete is at least six. A host waiting for this one
     * discards anything longer, so the length is the whole point.
     */
    HciCmdDispatchQueueNop(&dispatch);
    assert(HciCmdDispatchEventPending(&dispatch));
    assert(HciCmdDispatchGet(&dispatch, event, sizeof(event), &eventLen));
    assert(eventLen == 5U);
    assert(event[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(event[1] == 3U);          /* parameter length, no status byte */
    assert(event[2] == 1U);          /* one command packet may be sent */
    assert(event[3] == 0x00U && event[4] == 0x00U);
    assert(!HciCmdDispatchEventPending(&dispatch));
    printf("[ok] %-44s %02X %02X %02X %02X %02X\n",
           "the startup NOP is five octets, no status",
           event[0], event[1], event[2], event[3], event[4]);

    /*
     * No buffer behind it queues nothing rather than writing anyway. This used
     * to set EventCapacity to 4 by hand on a dispatcher that had just been
     * initialised, but init refuses any capacity under six, so it asserted on a
     * state the firmware cannot reach and proved nothing about the target. An
     * uninitialised dispatcher is the state that can actually occur.
     */
    {
        HciCmdDispatch_t empty;
        memset(&empty, 0, sizeof(empty));
        HciCmdDispatchQueueNop(&empty);
        assert(!HciCmdDispatchEventPending(&empty));
        HciCmdDispatchQueueNop(NULL);
        printf("[ok] %-44s\n", "no buffer queues nothing");
    }

    printf("All command dispatch tests passed.\n");
    return 0;
}
