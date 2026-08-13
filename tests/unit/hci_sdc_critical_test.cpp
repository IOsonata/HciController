/*
 * Regression tests for generic HCI routing below the nrfxlib command table.
 */

#include "hci_sdc.h"
#include "hci_core_profile.h"
#include "sdc_hci_vs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct FakeSdc
{
    int32_t AclResult;
    int32_t GetResult;
    uint8_t GetType;
    uint8_t GetPacket[80];
    unsigned AclCount;
    unsigned GetCount;
};

static int32_t AclPut(void *pContext, const uint8_t *)
{
    FakeSdc *pSdc = static_cast<FakeSdc *>(pContext);
    pSdc->AclCount++;
    return pSdc->AclResult;
}

static int32_t Get(void *pContext, uint8_t *pPacket, uint8_t *pType)
{
    FakeSdc *pSdc = static_cast<FakeSdc *>(pContext);
    pSdc->GetCount++;

    if (pSdc->GetResult == 0)
    {
        memcpy(pPacket, pSdc->GetPacket, sizeof(pSdc->GetPacket));
        *pType = pSdc->GetType;
    }

    return pSdc->GetResult;
}

static HciCmdResult_t Reset(void *, const uint8_t *, size_t, uint8_t *, size_t)
{
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 0U};
}

static HciCmdResult_t ReadSupportedCommands(void *, const uint8_t *, size_t,
                                            uint8_t *pReturn,
                                            size_t ReturnCapacity)
{
    if (ReturnCapacity < 64U)
    {
        return {HCI_STATUS_MEMORY_CAPACITY_EXCEEDED,
                HCI_CMD_RESPONSE_COMPLETE, 0U};
    }

    memset(pReturn, 0, 64U);
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 64U};
}

static const HciCmdEntry_t s_Commands[] = {
    {0x0C03U, 0U, 0U, HCI_CMD_RESPONSE_COMPLETE, Reset},
    {0x1002U, 0U, 64U, HCI_CMD_RESPONSE_COMPLETE, ReadSupportedCommands},
};

static void Init(HciSdc_t *pSdc, FakeSdc *pBackend, uint8_t *pCommandEvent,
                 size_t CommandEventCapacity)
{
    HciSdcOps_t ops = {};
    ops.AclPut = AclPut;
    ops.Get = Get;
    ops.pContext = pBackend;
    ops.RetryError = HCI_SDC_RETRY_ERROR;

    pBackend->GetResult = HCI_SDC_RETRY_ERROR;
    assert(HciSdcInit(pSdc, &ops, s_Commands,
                      sizeof(s_Commands) / sizeof(s_Commands[0]), NULL,
                      pCommandEvent, CommandEventCapacity));
}

static void MakeAcl(uint16_t Handle, uint8_t Packet[HCI_SDC_ACL_HEADER_SIZE])
{
    Packet[0] = (uint8_t)Handle;
    Packet[1] = (uint8_t)((Handle >> 8) & 0x0FU);
    Packet[2] = 0U;
    Packet[3] = 0U;
}

static size_t RunSupplementalCommand(uint16_t Opcode,
                                     const uint8_t *pParams,
                                     size_t ParamLen,
                                     uint8_t *pPacket,
                                     size_t PacketCapacity)
{
    assert(ParamLen <= 32U);

    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    uint8_t command[HCI_DISPATCH_COMMAND_HEADER_SIZE + 32U];
    command[0] = (uint8_t)Opcode;
    command[1] = (uint8_t)(Opcode >> 8);
    command[2] = (uint8_t)ParamLen;
    if (ParamLen != 0U)
    {
        memcpy(&command[HCI_DISPATCH_COMMAND_HEADER_SIZE], pParams, ParamLen);
    }

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    assert(controller != NULL);
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command,
                           HCI_DISPATCH_COMMAND_HEADER_SIZE + ParamLen));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, pPacket,
                           PacketCapacity, &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(backend.GetCount == 0U);
    return packetLen;
}

static void TestReadSupportedStatesCompatibilityCommand(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    assert(controller != NULL);

    assert(HciSdcKnowsCommand(&sdc,
                              HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES,
                              0U));
    assert(!HciSdcKnowsCommand(&sdc,
                               HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES,
                               1U));

    const uint8_t command[] = {0x1CU, 0x20U, 0x00U};
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, sizeof(command)));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);

    static const uint8_t expectedStates[8] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x03U, 0x00U, 0x00U
    };

    assert(type == HCI_H4_PACKET_EVENT);
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE + sizeof(expectedStates));
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[3] == 0x1CU && packet[4] == 0x20U);
    assert(packet[5] == HCI_STATUS_SUCCESS);
    assert(memcmp(&packet[6], expectedStates, sizeof(expectedStates)) == 0);
    assert(backend.GetCount == 0U);

    printf("[ok] mandatory LE Read Supported States is synthesized locally\n");
}

static void TestSupportedCommandsAdvertisesCompatibilityCommand(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    const uint8_t command[] = {0x02U, 0x10U, 0x00U};
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND,
                           command, sizeof(command)));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);

    assert(type == HCI_H4_PACKET_EVENT);
    assert(packetLen == 70U);
    assert(packet[5] == HCI_STATUS_SUCCESS);
    assert((packet[6U + 28U] & (1U << 3)) != 0U);
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
    assert((packet[6U + 48U] & (1U << 1)) != 0U);
    assert((packet[6U + 48U] & (1U << 5)) != 0U);
    assert((packet[6U + 48U] & (1U << 6)) != 0U);
    assert((packet[6U + 48U] & (1U << 7)) != 0U);
#endif

    printf("[ok] Read Local Supported Commands advertises supplemental commands\n");
}

static void TestCore62SupplementalCommands(void)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
    const uint16_t frameSpaceUpdate = 0x209DU;
    const uint16_t connectionRateRequest = 0x20A1U;
    const uint16_t setDefaultRateParameters = 0x20A2U;
    const uint16_t readMinSupportedConnInterval = 0x20A3U;

    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    assert(HciSdcKnowsCommand(&sdc, frameSpaceUpdate, 9U));
    assert(!HciSdcKnowsCommand(&sdc, frameSpaceUpdate, 8U));
    assert(HciSdcKnowsCommand(&sdc, connectionRateRequest, 20U));
    assert(HciSdcKnowsCommand(&sdc, setDefaultRateParameters, 18U));
    assert(HciSdcKnowsCommand(&sdc, readMinSupportedConnInterval, 0U));

    uint8_t params[20] = {};
    uint8_t packet[80];

    size_t packetLen = RunSupplementalCommand(frameSpaceUpdate, params, 9U,
                                               packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_STATUS_SIZE);
    assert(packet[0] == HCI_EVENT_COMMAND_STATUS);
    assert(packet[2] == HCI_STATUS_SUCCESS);
    assert(packet[4] == 0x9DU && packet[5] == 0x20U);

    packetLen = RunSupplementalCommand(connectionRateRequest, params, 20U,
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_STATUS_SIZE);
    assert(packet[0] == HCI_EVENT_COMMAND_STATUS);
    assert(packet[2] == HCI_STATUS_SUCCESS);
    assert(packet[4] == 0xA1U && packet[5] == 0x20U);

    packetLen = RunSupplementalCommand(setDefaultRateParameters, params, 18U,
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[3] == 0xA2U && packet[4] == 0x20U);
    assert(packet[5] == HCI_STATUS_SUCCESS);

    packetLen = RunSupplementalCommand(readMinSupportedConnInterval, NULL, 0U,
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE + 8U);
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[3] == 0xA3U && packet[4] == 0x20U);
    assert(packet[5] == HCI_STATUS_SUCCESS);
    assert(packet[6] == 6U);
    assert(packet[7] == 1U);
    assert(packet[8] == 6U && packet[9] == 0U);
    assert(packet[10] == 24U && packet[11] == 0U);
    assert(packet[12] == 1U && packet[13] == 0U);

    packetLen = RunSupplementalCommand(frameSpaceUpdate, params, 8U,
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_STATUS_SIZE);
    assert(packet[0] == HCI_EVENT_COMMAND_STATUS);
    assert(packet[2] == HCI_STATUS_INVALID_HCI_PARAMETERS);
    assert(packet[4] == 0x9DU && packet[5] == 0x20U);

    printf("[ok] Core 6.2 supplemental commands execute with correct HCI replies\n");
#endif
}

static void TestVendorSupplementalCommands(void)
{
    struct VendorCase
    {
        uint16_t Opcode;
        size_t ParamLen;
        HciCmdResponse_t Response;
    };

    static const VendorCase commands[] = {
        {SDC_HCI_OPCODE_CMD_VS_CONN_EVENT_EXTEND,
         sizeof(sdc_hci_cmd_vs_conn_event_extend_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_EVENT_LENGTH_SET,
         sizeof(sdc_hci_cmd_vs_event_length_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_PERIODIC_ADV_EVENT_LENGTH_SET,
         sizeof(sdc_hci_cmd_vs_periodic_adv_event_length_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_PERIPHERAL_LATENCY_MODE_SET,
         sizeof(sdc_hci_cmd_vs_peripheral_latency_mode_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_WRITE_REMOTE_TX_POWER,
         sizeof(sdc_hci_cmd_vs_write_remote_tx_power_t), HCI_CMD_RESPONSE_STATUS},
        {SDC_HCI_OPCODE_CMD_VS_COMPAT_MODE_WINDOW_OFFSET_SET,
         sizeof(sdc_hci_cmd_vs_compat_mode_window_offset_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_SET_POWER_CONTROL_REQUEST_PARAMS,
         sizeof(sdc_hci_cmd_vs_set_power_control_request_params_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_CENTRAL_ACL_EVENT_SPACING_SET,
         sizeof(sdc_hci_cmd_vs_central_acl_event_spacing_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_ALLOW_PARALLEL_CONNECTION_ESTABLISHMENTS,
         sizeof(sdc_hci_cmd_vs_allow_parallel_connection_establishments_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_MIN_VAL_OF_MAX_ACL_TX_PAYLOAD_SET,
         sizeof(sdc_hci_cmd_vs_min_val_of_max_acl_tx_payload_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_SCAN_CHANNEL_MAP_SET,
         sizeof(sdc_hci_cmd_vs_scan_channel_map_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_SCAN_ACCEPT_EXT_ADV_PACKETS_SET,
         sizeof(sdc_hci_cmd_vs_scan_accept_ext_adv_packets_set_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_SET_ROLE_PRIORITY,
         sizeof(sdc_hci_cmd_vs_set_role_priority_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_SET_EVENT_START_TASK,
         sizeof(sdc_hci_cmd_vs_set_event_start_task_t), HCI_CMD_RESPONSE_COMPLETE},
        {SDC_HCI_OPCODE_CMD_VS_ENABLE_PERIODIC_ADV_EVENT_COUNTER_REPORTS,
         sizeof(sdc_hci_cmd_vs_enable_periodic_adv_event_counter_reports_t), HCI_CMD_RESPONSE_COMPLETE},
    };

    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    uint8_t params[16] = {};
    uint8_t packet[80];
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
    {
        assert(HciSdcKnowsCommand(&sdc, commands[i].Opcode,
                                  commands[i].ParamLen));
        assert(commands[i].ParamLen != 0U);
        assert(!HciSdcKnowsCommand(&sdc, commands[i].Opcode,
                                   commands[i].ParamLen - 1U));

        const size_t packetLen =
            RunSupplementalCommand(commands[i].Opcode, params,
                                   commands[i].ParamLen,
                                   packet, sizeof(packet));
        if (commands[i].Response == HCI_CMD_RESPONSE_STATUS)
        {
            assert(packetLen == HCI_COMMAND_STATUS_SIZE);
            assert(packet[0] == HCI_EVENT_COMMAND_STATUS);
            assert(packet[2] == HCI_STATUS_SUCCESS);
            assert(packet[4] == (uint8_t)commands[i].Opcode);
            assert(packet[5] == (uint8_t)(commands[i].Opcode >> 8));
        }
        else
        {
            assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
            assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
            assert(packet[3] == (uint8_t)commands[i].Opcode);
            assert(packet[4] == (uint8_t)(commands[i].Opcode >> 8));
            assert(packet[5] == HCI_STATUS_SUCCESS);
        }
    }

    size_t packetLen = RunSupplementalCommand(
        SDC_HCI_OPCODE_CMD_VS_CONN_EVENT_EXTEND, params, 0U,
        packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(packet[5] == HCI_STATUS_INVALID_HCI_PARAMETERS);

    packetLen = RunSupplementalCommand(
        SDC_HCI_OPCODE_CMD_VS_WRITE_REMOTE_TX_POWER, params,
        sizeof(sdc_hci_cmd_vs_write_remote_tx_power_t) - 1U,
        packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_STATUS_SIZE);
    assert(packet[0] == HCI_EVENT_COMMAND_STATUS);
    assert(packet[2] == HCI_STATUS_INVALID_HCI_PARAMETERS);

    const uint8_t dtmEnd[] = {SDC_HCI_VS_DTM_COMMAND_OPCODE_TEST_END};
    packetLen = RunSupplementalCommand(SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND,
                                       dtmEnd, sizeof(dtmEnd),
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE +
                        sizeof(sdc_hci_cmd_vs_dtm_test_end_return_t));
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[5] == HCI_STATUS_SUCCESS);
    assert(packet[6] == 0x5AU && packet[7] == 0x5AU);

    const uint8_t dtmCarrier[] = {
        SDC_HCI_VS_DTM_COMMAND_OPCODE_TRANSMITTER_CARRIER_TEST, 0U, 0U
    };
    packetLen = RunSupplementalCommand(SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND,
                                       dtmCarrier, sizeof(dtmCarrier),
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(packet[5] == HCI_STATUS_SUCCESS);

    const uint8_t dtmInvalid[] = {0xFFU};
    packetLen = RunSupplementalCommand(SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND,
                                       dtmInvalid, sizeof(dtmInvalid),
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(packet[5] == HCI_STATUS_INVALID_HCI_PARAMETERS);

    const uint8_t dtmWrongLength[] = {
        SDC_HCI_VS_DTM_COMMAND_OPCODE_TEST_END, 0U
    };
    packetLen = RunSupplementalCommand(SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND,
                                       dtmWrongLength, sizeof(dtmWrongLength),
                                       packet, sizeof(packet));
    assert(packetLen == HCI_COMMAND_COMPLETE_BASE_SIZE);
    assert(packet[5] == HCI_STATUS_INVALID_HCI_PARAMETERS);

    printf("[ok] nRF52840 SDC vendor supplemental commands route and validate\n");
}

static void TestEveryFailedAclCanReturnItsCredit(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    backend.AclResult = -1;
    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    uint8_t acl[HCI_SDC_ACL_HEADER_SIZE];

    for (uint16_t handle = 0U; handle < HCI_SDC_ACL_TRACK_HANDLES; handle++)
    {
        MakeAcl(handle, acl);
        assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                               acl, sizeof(acl)));
    }

    assert(sdc.CreditEntries == HCI_SDC_ACL_TRACK_HANDLES);
    assert(sdc.CreditOverflowCount == 0U);

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
    assert(packet[2] == HCI_SDC_ACL_TRACK_HANDLES);
    assert(packetLen == 3U + ((size_t)HCI_SDC_ACL_TRACK_HANDLES * 4U));

    printf("[ok] failed ACL packets return credits for every tracked handle\n");
}

static void TestSyntheticCreditsCannotStarveControllerQueue(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    backend.AclResult = -1;
    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    uint8_t acl[HCI_SDC_ACL_HEADER_SIZE];
    MakeAcl(0U, acl);

    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
    assert(backend.GetCount == 0U);

    MakeAcl(1U, acl);
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));

    backend.GetResult = 0;
    backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
    backend.GetPacket[0] = 0x3EU;
    backend.GetPacket[1] = 1U;
    backend.GetPacket[2] = 0xAAU;

    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(backend.GetCount == 1U);
    assert(packet[0] == 0x3EU && packet[2] == 0xAAU);

    backend.GetResult = HCI_SDC_RETRY_ERROR;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);

    printf("[ok] synthetic ACL credits cannot starve the controller queue\n");
}

static void TestUnsafeDefensiveCreditGuardIsOffByDefault(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    backend.AclResult = 0;
    HciSdcSetAclLimit(&sdc, 1U);
    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    uint8_t acl[HCI_SDC_ACL_HEADER_SIZE];
    MakeAcl(3U, acl);

    for (unsigned i = 0U; i < 4U; i++)
    {
        assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                               acl, sizeof(acl)));
    }

#if HCI_SDC_ENFORCE_ACL_CREDITS
#error "critical routing test expects the unsafe defensive guard off by default"
#endif
    assert(backend.AclCount == 4U);
    assert(sdc.AclCreditOverrunCount == 0U);

    printf("[ok] host-maskable disconnect events cannot stale-throttle ACL\n");
}

int main(void)
{
    TestReadSupportedStatesCompatibilityCommand();
    TestSupportedCommandsAdvertisesCompatibilityCommand();
    TestCore62SupplementalCommands();
    TestVendorSupplementalCommands();
    TestEveryFailedAclCanReturnItsCredit();
    TestSyntheticCreditsCannotStarveControllerQueue();
    TestUnsafeDefensiveCreditGuardIsOffByDefault();
    printf("All critical SDC routing tests passed.\n");
    return 0;
}
