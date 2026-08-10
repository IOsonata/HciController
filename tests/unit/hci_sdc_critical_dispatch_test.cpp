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
    printf("[ok] LE Set CIG Parameters Test returns both CIS handles\n");

    printf("All critical SDC real-header dispatch tests passed.\n");
    return 0;
}
