/*
 * Critical command-dispatch regressions that require the real sdk-nrfxlib SDC
 * types. The SDC entry points themselves are supplied by tests/stubs/sdclink.
 */

#include "hci_counters.h"
#include "hci_sdc_nrfxlib.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_info_params.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_stub.h"

static const HciControllerOps_t *Init(HciSdc_t *pSdc,
                                      HciCounters_t *pCounters,
                                      uint8_t *pCommandEvent,
                                      size_t CommandEventCapacity)
{
    memset(&g_SdcStub, 0, sizeof(g_SdcStub));
    HciCountersInit(pCounters, pSdc, NULL);
    assert(HciSdcNrfxlibInit(pSdc, pCommandEvent, CommandEventCapacity,
                             pCounters));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(pSdc);
    assert(controller != NULL);
    return controller;
}

static size_t SendCommand(const HciControllerOps_t *controller,
                          uint16_t Opcode,
                          const uint8_t *pParams,
                          size_t ParamLen,
                          uint8_t *pEvent,
                          size_t EventCapacity)
{
    assert(ParamLen <= 255U);
    uint8_t command[258];
    command[0] = (uint8_t)Opcode;
    command[1] = (uint8_t)(Opcode >> 8);
    command[2] = (uint8_t)ParamLen;
    if (ParamLen != 0U)
    {
        memcpy(&command[3], pParams, ParamLen);
    }

    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, 3U + ParamLen));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t eventLen = 0U;
    assert(controller->Get(controller->pContext, &type, pEvent,
                           EventCapacity, &eventLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(pEvent[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(pEvent[3] == (uint8_t)Opcode);
    assert(pEvent[4] == (uint8_t)(Opcode >> 8));
    assert(pEvent[5] == HCI_STATUS_SUCCESS);
    return eventLen;
}

static uint8_t SendCommandStatus(const HciControllerOps_t *controller,
                                 uint16_t Opcode,
                                 const uint8_t *pParams,
                                 size_t ParamLen,
                                 uint8_t *pEvent,
                                 size_t EventCapacity)
{
    assert(ParamLen <= 255U);
    uint8_t command[258];
    command[0] = (uint8_t)Opcode;
    command[1] = (uint8_t)(Opcode >> 8);
    command[2] = (uint8_t)ParamLen;
    if (ParamLen != 0U)
    {
        memcpy(&command[3], pParams, ParamLen);
    }

    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, 3U + ParamLen));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t eventLen = 0U;
    assert(controller->Get(controller->pContext, &type, pEvent,
                           EventCapacity, &eventLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(eventLen >= HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(pEvent[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(pEvent[3] == (uint8_t)Opcode);
    assert(pEvent[4] == (uint8_t)(Opcode >> 8));
    return pEvent[5];
}

static void GiveControllerQueueItsTurn(const HciControllerOps_t *controller)
{
    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[16];
    size_t packetLen = 0U;

    /*
     * Command responses deliberately set CommandEventLast. One poll of the SDC
     * queue clears that fairness bit; the generated SDC stub has an empty queue
     * and returns -NRF_EAGAIN.
     */
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_EMPTY);
}

/*
 * The generated real-header SDC stub normally has an empty event queue. PAwR
 * response data needs a delayed Command Complete from that queue, so this one
 * test replaces only HciSdc_t::Ops.Get after the real command table has been
 * initialised. All command handlers still call the generated SDC entry points.
 */
struct TestSdcQueue
{
    int32_t Result;
    uint8_t Type;
    uint8_t Packet[32];
};

static int32_t TestSdcGet(void *pContext, uint8_t *pPacket, uint8_t *pType)
{
    TestSdcQueue *queue = static_cast<TestSdcQueue *>(pContext);
    if (queue->Result == 0)
    {
        memcpy(pPacket, queue->Packet, sizeof(queue->Packet));
        *pType = queue->Type;
    }
    return queue->Result;
}

template <typename CmdT, typename ReturnT>
static void TestCigVariableReturn(const HciControllerOps_t *controller,
                                  uint16_t Opcode,
                                  const char *pExpectedCall)
{
    alignas(4) uint8_t params[128];
    memset(params, 0, sizeof(params));

    CmdT *pCmd = reinterpret_cast<CmdT *>(params);
    pCmd->cis_count = 2U;

    const size_t paramLen = offsetof(CmdT, array_params) +
        ((size_t)pCmd->cis_count * sizeof(pCmd->array_params[0]));
    const size_t returnLen = offsetof(ReturnT, array_params) +
        ((size_t)pCmd->cis_count *
         sizeof(((ReturnT *)0)->array_params[0]));

    assert(paramLen < sizeof(params));
    assert(returnLen == 6U); /* CIG_ID + CIS_Count + two 16-bit handles. */

    uint8_t event[64];
    const size_t eventLen = SendCommand(controller, Opcode, params, paramLen,
                                        event, sizeof(event));

    assert(strcmp(g_SdcStub.LastCall, pExpectedCall) == 0);
    assert(eventLen == HCI_COMMAND_COMPLETE_BASE_SIZE + returnLen);
    assert(event[1] == 4U + returnLen);

    /*
     * The generated stub writes only sizeof(ReturnT), the two-octet fixed
     * prefix. The handler zeroes the complete dynamic result before the call,
     * so the two returned handle slots are present and initialized rather than
     * being absent or containing stack garbage.
     */
    assert(event[6] == 0x5AU && event[7] == 0x5AU);
    for (size_t i = 8U; i < eventLen; i++)
    {
        assert(event[i] == 0U);
    }
}

static void TestAdvertisingSetRandomAddressGuard(
    const HciControllerOps_t *controller)
{
    uint8_t event[64];
    uint8_t legacy[sizeof(sdc_hci_cmd_le_set_adv_params_t)] = {0};
    uint8_t randomAddress[sizeof(sdc_hci_cmd_le_set_adv_set_random_address_t)] =
        {0};

    HciSdcNrfxlibResetAdvCommandType();
    g_SdcStub.LastCall = NULL;
    (void)SendCommand(controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                      legacy, sizeof(legacy), event, sizeof(event));
    assert(strcmp(g_SdcStub.LastCall, "sdc_hci_cmd_le_set_adv_params") == 0);
    GiveControllerQueueItsTurn(controller);

    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(
               controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS,
               randomAddress, sizeof(randomAddress), event, sizeof(event)) ==
           HCI_STATUS_COMMAND_DISALLOWED);
    assert(g_SdcStub.LastCall == NULL);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set Advertising Set Random Address is rejected after legacy\n");

    HciSdcNrfxlibResetAdvCommandType();
    g_SdcStub.LastCall = NULL;
    (void)SendCommand(controller,
                      SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS,
                      randomAddress, sizeof(randomAddress), event,
                      sizeof(event));
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_le_set_adv_set_random_address") == 0);
    GiveControllerQueueItsTurn(controller);

    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                             legacy, sizeof(legacy), event, sizeof(event)) ==
           HCI_STATUS_COMMAND_DISALLOWED);
    assert(g_SdcStub.LastCall == NULL);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set Advertising Set Random Address selects extended mode\n");
}

static void TestEventMaskPage2(const HciControllerOps_t *controller)
{
    uint8_t event[HCI_COMMAND_COMPLETE_BASE_SIZE +
                  sizeof(sdc_hci_cmd_ip_read_local_supported_commands_return_t)];

    const size_t supportedLen = SendCommand(
        controller, SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_COMMANDS,
        NULL, 0U, event, sizeof(event));
    assert(supportedLen == HCI_COMMAND_COMPLETE_BASE_SIZE +
                           sizeof(sdc_hci_cmd_ip_read_local_supported_commands_return_t));

    sdc_hci_cmd_ip_read_local_supported_commands_return_t supported;
    memcpy(supported.raw, &event[HCI_COMMAND_COMPLETE_BASE_SIZE],
           sizeof(supported.raw));
    assert(supported.params.hci_set_event_mask_page_2 == 1U);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] Read Local Supported Commands advertises Set Event Mask Page 2\n");

    uint8_t page2[sizeof(sdc_hci_cmd_cb_set_event_mask_page_2_t)] = {0};
    g_SdcStub.LastCall = NULL;
    const size_t page2Len = SendCommand(
        controller, SDC_HCI_OPCODE_CMD_CB_SET_EVENT_MASK_PAGE_2,
        page2, sizeof(page2), event, sizeof(event));
    assert(page2Len == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_cb_set_event_mask_page_2") == 0);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] Set Event Mask Page 2 reaches the SDC entry point\n");
}

/*
 * The two helpers above both read a Command Complete. LE Extended Create
 * Connection answers with Command Status on success and on failure alike,
 * Vol 4 Part E 7.8.66, so it needs its own reader.
 */
static uint8_t SendForCommandStatusEvent(const HciControllerOps_t *controller,
                                         uint16_t Opcode,
                                         const uint8_t *pParams,
                                         size_t ParamLen,
                                         uint8_t *pEvent,
                                         size_t EventCapacity)
{
    assert(ParamLen <= 255U);
    uint8_t command[258];
    command[0] = (uint8_t)Opcode;
    command[1] = (uint8_t)(Opcode >> 8);
    command[2] = (uint8_t)ParamLen;
    if (ParamLen != 0U)
    {
        memcpy(&command[3], pParams, ParamLen);
    }

    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, 3U + ParamLen));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t eventLen = 0U;
    assert(controller->Get(controller->pContext, &type, pEvent,
                           EventCapacity, &eventLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(eventLen == HCI_COMMAND_STATUS_SIZE);
    assert(pEvent[0] == HCI_EVENT_COMMAND_STATUS);
    assert(pEvent[4] == (uint8_t)Opcode);
    assert(pEvent[5] == (uint8_t)(Opcode >> 8));
    return pEvent[2];
}

/*
 * Periodic advertising with responses is configured and paid for in the SDC
 * pool, and the two v2 commands are the ones a host needs to use it: the v2
 * advertising parameters carry the PHY options, and the v2 create connection
 * is the only way a synchronised device turns a train into a link. Both were
 * absent from the dispatch table while the feature was enabled.
 */
static void TestExtendedAdvertisingV2(const HciControllerOps_t *controller)
{
    uint8_t event[64];

    HciSdcNrfxlibResetAdvCommandType();

    uint8_t params[sizeof(sdc_hci_cmd_le_set_ext_adv_params_v2_t)] = {0};
    g_SdcStub.LastCall = NULL;
    const size_t paramsLen =
        SendCommand(controller, SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS_V2,
                    params, sizeof(params), event, sizeof(event));
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_le_set_ext_adv_params_v2") == 0);
    assert(paramsLen ==
           HCI_COMMAND_COMPLETE_BASE_SIZE +
               sizeof(sdc_hci_cmd_le_set_ext_adv_params_v2_return_t));
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set Extended Advertising Parameters v2 reaches SDC\n");

    /* It belongs to the extended set, so legacy is closed off after it. */
    uint8_t legacy[sizeof(sdc_hci_cmd_le_set_adv_params_t)] = {0};
    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                             legacy, sizeof(legacy), event, sizeof(event)) ==
           HCI_STATUS_COMMAND_DISALLOWED);
    assert(g_SdcStub.LastCall == NULL);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set Extended Advertising Parameters v2 selects extended\n");

    /*
     * One bit in Initiating_PHYs, so exactly one parameter group follows the
     * twelve octet fixed part. Getting that count wrong is what the variable
     * length check exists to catch, so both the right length and a wrong one
     * are driven.
     */
    uint8_t conn[offsetof(sdc_hci_cmd_le_ext_create_conn_v2_t, array_params) +
                 sizeof(sdc_hci_le_ext_create_conn_v2_array_params_t)] = {0};
    sdc_hci_cmd_le_ext_create_conn_v2_t *pConn =
        reinterpret_cast<sdc_hci_cmd_le_ext_create_conn_v2_t *>(conn);
    pConn->initiating_phys = 0x01U;

    g_SdcStub.LastCall = NULL;
    assert(SendForCommandStatusEvent(
               controller, SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN_V2, conn,
               sizeof(conn), event, sizeof(event)) == HCI_STATUS_SUCCESS);
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_le_ext_create_conn_v2") == 0);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Extended Create Connection v2 reaches SDC\n");

    g_SdcStub.LastCall = NULL;
    assert(SendForCommandStatusEvent(
               controller, SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN_V2, conn,
               sizeof(conn) - 1U, event, sizeof(event)) ==
           HCI_STATUS_INVALID_HCI_PARAMETERS);
    assert(g_SdcStub.LastCall == NULL);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Extended Create Connection v2 counts its PHY groups\n");

    /* Both bits have to be in the map, or a host will never send either. */
    uint8_t supportedEvent[HCI_COMMAND_COMPLETE_BASE_SIZE +
                           sizeof(sdc_hci_cmd_ip_read_local_supported_commands_return_t)];
    (void)SendCommand(controller,
                      SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_COMMANDS,
                      NULL, 0U, supportedEvent, sizeof(supportedEvent));

    sdc_hci_cmd_ip_read_local_supported_commands_return_t supported;
    memcpy(supported.raw, &supportedEvent[HCI_COMMAND_COMPLETE_BASE_SIZE],
           sizeof(supported.raw));
    assert(supported.params.hci_le_set_extended_advertising_parameters_v2 == 1U);
    assert(supported.params.hci_le_extended_create_connection_v2 == 1U);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] Read Local Supported Commands advertises both v2 commands\n");

    HciSdcNrfxlibResetAdvCommandType();
}

static void TestPawrResponseDelayedCompletion(HciSdc_t *pSdc,
                                              const HciControllerOps_t *controller)
{
    const uint16_t opcode =
        SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RESPONSE_DATA;
    const size_t head =
        offsetof(sdc_hci_cmd_le_set_periodic_adv_response_data_t,
                 response_data);
    uint8_t event[64];

    HciSdcNrfxlibResetAdvCommandType();

    /*
     * A malformed body is rejected by the real nrfxlib handler before the SDC
     * entry point is called. It has no delayed completion to wait for, so the
     * Invalid HCI Command Parameters answer remains immediate.
     */
    alignas(4) uint8_t malformed[16] = {0};
    assert(head + 1U <= sizeof(malformed));
    sdc_hci_cmd_le_set_periodic_adv_response_data_t *pMalformed =
        reinterpret_cast<sdc_hci_cmd_le_set_periodic_adv_response_data_t *>(
            malformed);
    pMalformed->response_data_length = 2U;
    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(controller, opcode, malformed, head + 1U,
                             event, sizeof(event)) ==
           HCI_STATUS_INVALID_HCI_PARAMETERS);
    assert(g_SdcStub.LastCall == NULL);
    assert(!pSdc->DelayedCommandPending);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] malformed PAwR response data is rejected immediately\n");

    /*
     * Vol 4 Part E 3.1.1 selects an advertising command set when the Host
     * issues a command from that set; success is not required. The malformed
     * 0x2083 therefore still selected the extended command set.
     */
    uint8_t legacy[sizeof(sdc_hci_cmd_le_set_adv_params_t)] = {0};
    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                             legacy, sizeof(legacy), event, sizeof(event)) ==
           HCI_STATUS_COMMAND_DISALLOWED);
    assert(g_SdcStub.LastCall == NULL);
    GiveControllerQueueItsTurn(controller);
    HciSdcNrfxlibResetAdvCommandType();
    printf("[ok] malformed PAwR response data still selects extended mode\n");

    alignas(4) uint8_t params[16] = {0};
    assert(head <= sizeof(params));
    sdc_hci_cmd_le_set_periodic_adv_response_data_t *pParams =
        reinterpret_cast<sdc_hci_cmd_le_set_periodic_adv_response_data_t *>(
            params);
    pParams->response_data_length = 0U;

    /*
     * A command rejected by the advertising-set guard never enters its handler
     * and therefore cannot be waiting on SDC. Select the legacy set first and
     * verify that this extended command gets an immediate Command Disallowed.
     */
    (void)SendCommand(controller, SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                      legacy, sizeof(legacy), event, sizeof(event));
    GiveControllerQueueItsTurn(controller);
    g_SdcStub.LastCall = NULL;
    assert(SendCommandStatus(controller, opcode, params, head,
                             event, sizeof(event)) ==
           HCI_STATUS_COMMAND_DISALLOWED);
    assert(g_SdcStub.LastCall == NULL);
    assert(!pSdc->DelayedCommandPending);
    GiveControllerQueueItsTurn(controller);
    HciSdcNrfxlibResetAdvCommandType();
    printf("[ok] guarded PAwR response data is rejected immediately\n");

    /*
     * From here on supply an explicit controller queue. The direct SDC command
     * stub still supplies the real handler result; only sdc_hci_get is replaced
     * so delayed events can be injected deterministically.
     */
    TestSdcQueue queue = {};
    queue.Result = pSdc->Ops.RetryError;
    pSdc->Ops.Get = TestSdcGet;
    pSdc->Ops.pContext = &queue;

    uint8_t command[32] = {0};
    assert(3U + head <= sizeof(command));
    command[0] = (uint8_t)opcode;
    command[1] = (uint8_t)(opcode >> 8);
    command[2] = (uint8_t)head;
    memcpy(&command[3], params, head);

    const uint8_t markLenBefore = pSdc->Commands.RspMarkLen;
    g_SdcStub.NextStatus = HCI_STATUS_SUCCESS;
    g_SdcStub.LastCall = NULL;
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, 3U + head));
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_le_set_periodic_adv_response_data") == 0);
    assert(pSdc->DelayedCommandPending);
    assert(pSdc->DelayedCommandOpcode == opcode);
    assert(!HciCmdDispatchEventPending(&pSdc->Commands));
    assert(pSdc->Commands.RspMarkLen == markLenBefore);

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t eventLen = 0U;
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_EMPTY);
    assert(pSdc->DelayedCommandPending);
    printf("[ok] successful PAwR response data has no synthetic completion\n");

    /* The Host still has no command credit, so a following Reset stays queued. */
    const uint8_t reset[] = {0x03U, 0x0CU, 0x00U};
    const uint32_t deferred = pSdc->CommandDeferredCount;
    assert(!controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                            reset, sizeof(reset)));
    assert(pSdc->CommandDeferredCount == deferred + 1U);

    /* An unrelated controller event is forwarded and does not clear it. */
    memset(queue.Packet, 0, sizeof(queue.Packet));
    queue.Result = 0;
    queue.Type = HCI_SDC_MSG_TYPE_EVENT;
    queue.Packet[0] = 0x3EU;
    queue.Packet[1] = 0x01U;
    queue.Packet[2] = 0x27U;
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(eventLen == 3U && event[0] == 0x3EU && event[2] == 0x27U);
    assert(pSdc->DelayedCommandPending);

    /* The matching real SDC Command Complete is the one that returns credit. */
    const uint8_t realComplete[] = {
        HCI_EVENT_COMMAND_COMPLETE, 0x06U, 0x01U,
        0x83U, 0x20U, HCI_STATUS_SUCCESS, 0x5AU, 0x5AU,
    };
    memset(queue.Packet, 0, sizeof(queue.Packet));
    memcpy(queue.Packet, realComplete, sizeof(realComplete));
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(eventLen == sizeof(realComplete));
    assert(memcmp(event, realComplete, sizeof(realComplete)) == 0);
    assert(!pSdc->DelayedCommandPending);
    assert(pSdc->DelayedCommandOpcode == 0U);

    queue.Result = pSdc->Ops.RetryError;
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           reset, sizeof(reset)));
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_PACKET);
    assert(event[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(event[3] == 0x03U && event[4] == 0x0CU);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] real PAwR completion releases the following command\n");

    /*
     * Once the command reaches SDC, its Command Complete is delayed even when
     * the direct entry point reports an HCI error; Nordic's wrapper only makes
     * Unknown HCI Command immediate. This distinguishes an SDC refusal from the
     * local guard and length refusals above.
     */
    HciSdcNrfxlibResetAdvCommandType();
    g_SdcStub.NextStatus = HCI_STATUS_COMMAND_DISALLOWED;
    g_SdcStub.LastCall = NULL;
    queue.Result = pSdc->Ops.RetryError;
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, 3U + head));
    assert(strcmp(g_SdcStub.LastCall,
                  "sdc_hci_cmd_le_set_periodic_adv_response_data") == 0);
    assert(pSdc->DelayedCommandPending);
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_EMPTY);

    const uint8_t realError[] = {
        HCI_EVENT_COMMAND_COMPLETE, 0x06U, 0x01U,
        0x83U, 0x20U, HCI_STATUS_COMMAND_DISALLOWED, 0x00U, 0x00U,
    };
    memset(queue.Packet, 0, sizeof(queue.Packet));
    memcpy(queue.Packet, realError, sizeof(realError));
    queue.Result = 0;
    assert(controller->Get(controller->pContext, &type, event, sizeof(event),
                           &eventLen) == HCI_CONTROLLER_GET_PACKET);
    assert(eventLen == sizeof(realError));
    assert(memcmp(event, realError, sizeof(realError)) == 0);
    assert(!pSdc->DelayedCommandPending);
    g_SdcStub.NextStatus = HCI_STATUS_SUCCESS;
    printf("[ok] SDC PAwR errors wait for their real completion\n");
}

int main(void)
{
    HciSdc_t sdc;
    HciCounters_t counters;
    uint8_t commandEvent[HCI_COMMAND_COMPLETE_BASE_SIZE + HCI_CMD_MAX_RETURN_LEN];
    const HciControllerOps_t *controller =
        Init(&sdc, &counters, commandEvent, sizeof(commandEvent));

    /* V2 must establish the same ACL allowance as the V1 command does. */
    uint8_t event[64];
    const size_t v2Len =
        SendCommand(controller, SDC_HCI_OPCODE_CMD_LE_READ_BUFFER_SIZE_V2,
                    NULL, 0U, event, sizeof(event));
    assert(strcmp(g_SdcStub.LastCall, "sdc_hci_cmd_le_read_buffer_size_v2") == 0);
    assert(v2Len == HCI_COMMAND_COMPLETE_BASE_SIZE +
                    sizeof(sdc_hci_cmd_le_read_buffer_size_v2_return_t));
    /* The generated stub fills every return octet with 0x5A. */
    assert(sdc.AclLimit == 0x5AU);
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Read Buffer Size V2 records the ACL packet count\n");

    TestCigVariableReturn<sdc_hci_cmd_le_set_cig_params_t,
                          sdc_hci_cmd_le_set_cig_params_return_t>(
        controller, SDC_HCI_OPCODE_CMD_LE_SET_CIG_PARAMS,
        "sdc_hci_cmd_le_set_cig_params");
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set CIG Parameters returns both CIS handles\n");

    TestCigVariableReturn<sdc_hci_cmd_le_set_cig_params_test_t,
                          sdc_hci_cmd_le_set_cig_params_test_return_t>(
        controller, SDC_HCI_OPCODE_CMD_LE_SET_CIG_PARAMS_TEST,
        "sdc_hci_cmd_le_set_cig_params_test");
    GiveControllerQueueItsTurn(controller);
    printf("[ok] LE Set CIG Parameters Test returns both CIS handles\n");

    TestAdvertisingSetRandomAddressGuard(controller);
    TestEventMaskPage2(controller);
    TestExtendedAdvertisingV2(controller);
    TestPawrResponseDelayedCompletion(&sdc, controller);

    printf("All critical SDC real-header dispatch tests passed.\n");
    return 0;
}
