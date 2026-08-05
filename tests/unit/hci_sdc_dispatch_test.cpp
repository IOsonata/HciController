/*
 * Host test for the SDC command dispatch table.
 *
 * Compiled against the real nrfxlib SoftDevice Controller headers, with the
 * SDC entry points stubbed. That checks the wiring rather than the radio:
 * that an opcode reaches the intended SDC call, that the response is a
 * Command Complete or a Command Status as the specification requires, that
 * the declared parameter length matches the wire format, and that the
 * variable length commands reject a truncated packet.
 */

#include "hci_sdc_nrfxlib.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_cmd_info_params.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_cmd_link_control.h"
#include "sdc_hci_vs.h"
#include "sdc_stub.h"

#ifndef HCI_SDC_HAS_READ_SUPPORTED_STATES
#define HCI_SDC_HAS_READ_SUPPORTED_STATES 0
#endif

#ifndef HCI_SDC_HAS_READ_TRANSMIT_POWER
#define HCI_SDC_HAS_READ_TRANSMIT_POWER 0
#endif

#ifndef HCI_SDC_HAS_READ_REMOTE_VERSION
#define HCI_SDC_HAS_READ_REMOTE_VERSION 1
#endif

#ifndef HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
#define HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT 1
#endif

#ifndef HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
#define HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES 1
#endif

#ifndef HCI_SDC_HAS_VS_READ_COUNTERS
#define HCI_SDC_HAS_VS_READ_COUNTERS 1
#endif

#define EVENT_COMMAND_COMPLETE 0x0E
#define EVENT_COMMAND_STATUS   0x0F

static HciSdc_t gSdc;
static uint8_t gEventBuffer[300];
static const HciControllerOps_t *gOps;

/* Return parameters of the last exchange, for tests that read them back. */
static uint8_t gLastReturn[300];

struct Response {
    uint8_t code;
    uint8_t status;
    uint16_t opcode;
    size_t return_len;
};

static Response Exchange(uint16_t opcode, const uint8_t *pParams, size_t len)
{
    uint8_t packet[300];
    packet[0] = (uint8_t)opcode;
    packet[1] = (uint8_t)(opcode >> 8);
    packet[2] = (uint8_t)len;
    if (len > 0U)
    {
        memcpy(&packet[3], pParams, len);
    }

    /*
     * The routing layer holds the next command until the controller queue has
     * had the outgoing slot, so a command can be refused once and has to be
     * offered again, exactly as the H:4 parser does. Model that here rather
     * than assuming the first offer is taken.
     */
    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t out[300];
    size_t outLen = 0U;
    HciControllerGetResult_t result = HCI_CONTROLLER_GET_EMPTY;
    bool put = false;

    for (unsigned pass = 0U; pass < 4U && result != HCI_CONTROLLER_GET_PACKET;
         pass++)
    {
        if (!put)
        {
            put = gOps->Put(gOps->pContext, HCI_H4_PACKET_COMMAND, packet,
                            3U + len);
        }
        result = gOps->Get(gOps->pContext, &type, out, sizeof(out), &outLen);
    }

    assert(put);
    assert(result == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(outLen >= 6U);

    Response rsp;
    rsp.code = out[0];
    if (rsp.code == EVENT_COMMAND_COMPLETE)
    {
        rsp.opcode = (uint16_t)(out[3] | (out[4] << 8));
        rsp.status = out[5];
        rsp.return_len = outLen - 6U;
        if (rsp.return_len > 0U)
        {
            memcpy(gLastReturn, &out[6], rsp.return_len);
        }
    }
    else
    {
        rsp.status = out[2];
        rsp.opcode = (uint16_t)(out[4] | (out[5] << 8));
        rsp.return_len = 0U;
    }
    return rsp;
}

static void ExpectComplete(const char *label, uint16_t opcode,
                           const uint8_t *pParams, size_t len,
                           size_t expectedReturn)
{
    g_SdcStub.NextStatus = 0x00;
    g_SdcStub.LastCall = NULL;
    Response rsp = Exchange(opcode, pParams, len);

    assert(rsp.code == EVENT_COMMAND_COMPLETE);
    assert(rsp.opcode == opcode);
    assert(rsp.status == 0x00);
    assert(rsp.return_len == expectedReturn);
    assert(g_SdcStub.LastCall != NULL);
    printf("[ok] %-38s complete, %zu byte return, %s\n",
           label, expectedReturn, g_SdcStub.LastCall);
}

/*
 * A command the routing layer answers out of its own state. SDC must not be
 * reached at all, which is the difference that matters: a counter readout that
 * went to the radio would be reporting the wrong thing.
 */
static void ExpectCompleteLocal(const char *label, uint16_t opcode,
                                const uint8_t *pParams, size_t len,
                                size_t expectedReturn)
{
    g_SdcStub.NextStatus = 0x00;
    g_SdcStub.LastCall = NULL;
    Response rsp = Exchange(opcode, pParams, len);

    assert(rsp.code == EVENT_COMMAND_COMPLETE);
    assert(rsp.opcode == opcode);
    assert(rsp.status == 0x00);
    assert(rsp.return_len == expectedReturn);
    assert(g_SdcStub.LastCall == NULL);
    printf("[ok] %-38s complete, %zu byte return, no SDC call\n",
           label, expectedReturn);
}

#if HCI_SDC_HAS_VS_READ_COUNTERS
/* Positions in the counter block, fixed by hci_sdc.h. */
enum {
    COUNTER_COMMAND = 0,
    COUNTER_UNKNOWN_COMMAND = 1,
    COUNTER_INVALID_PARAM_LEN = 3,
    COUNTER_ACL_PUT_ERROR = 6,
    COUNTER_PUT_RETRY = 8,
};

/* Unpacks the block left in gLastReturn by the most recent readout. */
static void ReadCounters(uint32_t *pCounters)
{
    for (size_t i = 0U; i < HCI_SDC_COUNTERS_COUNT; i++)
    {
        const uint8_t *p = &gLastReturn[1U + (i * 4U)];
        pCounters[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
}
#endif

static void ExpectStatus(const char *label, uint16_t opcode,
                         const uint8_t *pParams, size_t len)
{
    g_SdcStub.NextStatus = 0x00;
    g_SdcStub.LastCall = NULL;
    Response rsp = Exchange(opcode, pParams, len);

    assert(rsp.code == EVENT_COMMAND_STATUS);
    assert(rsp.opcode == opcode);
    assert(rsp.status == 0x00);
    assert(g_SdcStub.LastCall != NULL);
    printf("[ok] %-38s status, %s\n", label, g_SdcStub.LastCall);
}

/*
 * The dispatcher answers every rejection with Command Complete, including for
 * opcodes whose success path is Command Status. That is what the code does
 * today and what this helper asserts. Vol 4 Part E 7.7.15 asks for Command
 * Status in that case, so this expectation changes when the dispatcher does.
 */
static void ExpectRejected(const char *label, uint16_t opcode,
                           const uint8_t *pParams, size_t len,
                           uint8_t expected)
{
    g_SdcStub.NextStatus = 0x00;
    Response rsp = Exchange(opcode, pParams, len);
    assert(rsp.code == EVENT_COMMAND_COMPLETE);
    assert(rsp.opcode == opcode);
    assert(rsp.status == expected);
    printf("[ok] %-38s rejected with 0x%02X\n", label, expected);
}

/*
 * A handler that answers Command Status on success answers a rejection the
 * same way, because the rejection goes through the same Reply. That is what
 * Vol 4 Part E 7.7.15 asks for.
 */
static void ExpectRejectedStatus(const char *label, uint16_t opcode,
                                 const uint8_t *pParams, size_t len,
                                 uint8_t expected)
{
    g_SdcStub.NextStatus = 0x00;
    Response rsp = Exchange(opcode, pParams, len);
    assert(rsp.code == EVENT_COMMAND_STATUS);
    assert(rsp.opcode == opcode);
    assert(rsp.status == expected);
    printf("[ok] %-38s rejected with 0x%02X, status\n", label, expected);
}

/*
 * A rejection on a Command Complete opcode still carries the full declared
 * return parameter length, zero filled.
 */
static void ExpectRejectedLen(const char *label, uint16_t opcode,
                              const uint8_t *pParams, size_t len,
                              uint8_t expected, size_t expectedReturn)
{
    g_SdcStub.NextStatus = 0x00;
    Response rsp = Exchange(opcode, pParams, len);
    assert(rsp.code == EVENT_COMMAND_COMPLETE);
    assert(rsp.opcode == opcode);
    assert(rsp.status == expected);
    assert(rsp.return_len == expectedReturn);
    printf("[ok] %-38s rejected with 0x%02X, %zu byte return\n",
           label, expected, expectedReturn);
}

int main(void)
{
    /* Unbuffered, so an abort still shows how far the run got. */
    setvbuf(stdout, NULL, _IONBF, 0);

    static uint8_t zeros[300];

    assert(HciSdcNrfxlibInit(&gSdc, gEventBuffer, sizeof(gEventBuffer)));
    gOps = HciSdcGetControllerOps(&gSdc);
    assert(gOps != NULL);

    /* Commands the specification answers with Command Status. */
    ExpectStatus("Disconnect", 0x0406, zeros,
                 sizeof(sdc_hci_cmd_lc_disconnect_t));
#if HCI_SDC_HAS_READ_REMOTE_VERSION
    ExpectStatus("Read Remote Version Information", 0x041D, zeros,
                 sizeof(sdc_hci_cmd_lc_read_remote_version_information_t));
#endif
    ExpectStatus("LE Create Connection", 0x200D, zeros,
                 sizeof(sdc_hci_cmd_le_create_conn_t));
    ExpectStatus("LE Connection Update", 0x2013, zeros,
                 sizeof(sdc_hci_cmd_le_conn_update_t));
    ExpectStatus("LE Read Remote Features", 0x2016, zeros,
                 sizeof(sdc_hci_cmd_le_read_remote_features_t));
    ExpectStatus("LE Enable Encryption", 0x2019, zeros,
                 sizeof(sdc_hci_cmd_le_enable_encryption_t));
    ExpectStatus("LE Set PHY", 0x2032, zeros,
                 sizeof(sdc_hci_cmd_le_set_phy_t));
    /* initiating_phys is zero in the all zero packet, so no array is needed. */
    ExpectStatus("LE Extended Create Connection", 0x2043, zeros,
                 offsetof(sdc_hci_cmd_le_ext_create_conn_t, array_params));

    /* Commands answered with Command Complete, some carrying return data. */
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
    ExpectComplete("Read Authenticated Payload Timeout", 0x0C7B, zeros,
                   sizeof(sdc_hci_cmd_cb_read_authenticated_payload_timeout_t),
        sizeof(sdc_hci_cmd_cb_read_authenticated_payload_timeout_return_t));
    ExpectComplete("Write Authenticated Payload Timeout", 0x0C7C, zeros,
                   sizeof(sdc_hci_cmd_cb_write_authenticated_payload_timeout_t),
        sizeof(sdc_hci_cmd_cb_write_authenticated_payload_timeout_return_t));
#endif
#if HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
    /*
     * Variable return: the count byte plus one 22 byte address, which is what
     * the stub reports. This is what a host asks when the board has no public
     * address, so a wrong length here sends it back to inventing one.
     */
    ExpectComplete("VS Read Static Addresses", 0xFC09, zeros, 0U,
                   1U + sizeof(sdc_hci_vs_zephyr_static_address_t));
#endif
#if HCI_SDC_HAS_VS_READ_COUNTERS
    ExpectCompleteLocal("VS Read Counters", HCI_SDC_OPCODE_VS_READ_COUNTERS,
                        zeros, 0U, HCI_SDC_COUNTERS_RETURN_LEN);
    assert(gLastReturn[0] == HCI_SDC_COUNTERS_VERSION);
    printf("[ok] %-38s version %u\n", "counter block names its version",
           (unsigned)gLastReturn[0]);
#endif
    ExpectComplete("LE Create Connection Cancel", 0x200E, zeros, 0U, 0U);
    ExpectComplete("LE Read Filter Accept List Size", 0x200F, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_filter_accept_list_size_return_t));
    ExpectComplete("LE Clear Filter Accept List", 0x2010, zeros, 0U, 0U);
    ExpectComplete("LE Read Channel Map", 0x2015, zeros,
                   sizeof(sdc_hci_cmd_le_read_channel_map_t),
                   sizeof(sdc_hci_cmd_le_read_channel_map_return_t));
    ExpectComplete("LE Encrypt", 0x2017, zeros,
                   sizeof(sdc_hci_cmd_le_encrypt_t),
                   sizeof(sdc_hci_cmd_le_encrypt_return_t));
    ExpectComplete("LE Rand", 0x2018, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_rand_return_t));
    ExpectComplete("LE LTK Request Reply", 0x201A, zeros,
                   sizeof(sdc_hci_cmd_le_long_term_key_request_reply_t),
                   sizeof(sdc_hci_cmd_le_long_term_key_request_reply_return_t));
    /*
     * Opt in commands. The library variant decides whether the symbol exists,
     * so the table entry is conditional and the test follows it.
     */
#if HCI_SDC_HAS_READ_SUPPORTED_STATES
    ExpectComplete("LE Read Supported States", 0x201C, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_supported_states_return_t));
#else
    ExpectRejected("LE Read Supported States, not built", 0x201C, zeros, 0U,
                   0x01);
#endif
#if HCI_SDC_HAS_READ_TRANSMIT_POWER
    ExpectComplete("LE Read Transmit Power", 0x204B, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_transmit_power_return_t));
#else
    ExpectRejected("LE Read Transmit Power, not built", 0x204B, zeros, 0U,
                   0x01);
#endif
    ExpectComplete("LE Receiver Test", 0x201D, zeros,
                   sizeof(sdc_hci_cmd_le_receiver_test_v1_t), 0U);
    ExpectComplete("LE Test End", 0x201F, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_test_end_return_t));
    ExpectComplete("LE Set Data Length", 0x2022, zeros,
                   sizeof(sdc_hci_cmd_le_set_data_length_t),
                   sizeof(sdc_hci_cmd_le_set_data_length_return_t));
    ExpectComplete("LE Read PHY", 0x2030, zeros,
                   sizeof(sdc_hci_cmd_le_read_phy_t),
                   sizeof(sdc_hci_cmd_le_read_phy_return_t));
    ExpectComplete("LE Set Extended Adv Parameters", 0x2036, zeros,
                   sizeof(sdc_hci_cmd_le_set_ext_adv_params_t),
                   sizeof(sdc_hci_cmd_le_set_ext_adv_params_return_t));
    ExpectComplete("LE Read Max Adv Data Length", 0x203A, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_max_adv_data_length_return_t));
    ExpectComplete("LE Clear Advertising Sets", 0x203D, zeros, 0U, 0U);
    ExpectComplete("LE Set Extended Scan Enable", 0x2042, zeros,
                   sizeof(sdc_hci_cmd_le_set_ext_scan_enable_t), 0U);

    /*
     * Variable length commands with an all zero body declare an empty array,
     * so the exact length is the fixed part on its own. Anything longer is a
     * host that disagrees with itself, Vol 4 Part E 5.4.1.
     */
    ExpectComplete("LE Set Extended Adv Data, empty", 0x2037, zeros,
                   offsetof(sdc_hci_cmd_le_set_ext_adv_data_t, adv_data), 0U);
    ExpectComplete("LE Set Extended Adv Enable, empty", 0x2039, zeros,
                   offsetof(sdc_hci_cmd_le_set_ext_adv_enable_t, array_params),
                   0U);
    ExpectComplete("LE Set Extended Scan Parameters, empty", 0x2041, zeros,
                   offsetof(sdc_hci_cmd_le_set_ext_scan_params_t, array_params),
                   0U);

    ExpectRejected("LE Set Extended Adv Data, surplus bytes", 0x2037, zeros,
                   offsetof(sdc_hci_cmd_le_set_ext_adv_data_t, adv_data) + 8U,
                   0x12);
    ExpectRejected("LE Set Extended Adv Enable, surplus", 0x2039, zeros,
                   offsetof(sdc_hci_cmd_le_set_ext_adv_enable_t, array_params)
                   + 4U, 0x12);

    ExpectRejected("LE Set Extended Adv Data, truncated", 0x2037, zeros, 2U,
                   0x12);
    ExpectRejected("LE Set Extended Scan Parameters, short", 0x2041, zeros, 1U,
                   0x12);

    /*
     * The count field inside the fixed part has to agree with the parameter
     * length. A short packet claiming a large trailing array would otherwise
     * make the controller read past the end of the receive buffer, which holds
     * the previous packet.
     */
    {
        uint8_t advData[8];
        memset(advData, 0, sizeof(advData));
        advData[3] = 16U;    /* adv_data_length, but only 4 bytes follow */
        ExpectRejected("LE Set Extended Adv Data, lying count", 0x2037,
                       advData, 8U, 0x12);

        advData[3] = 4U;     /* now the count agrees with what was sent */
        ExpectComplete("LE Set Extended Adv Data, honest count", 0x2037,
                       advData, 8U, 0U);

        advData[3] = 3U;     /* one byte more sent than declared */
        ExpectRejected("LE Set Extended Adv Data, one byte over", 0x2037,
                       advData, 8U, 0x12);
    }

    {
        uint8_t scanRsp[8];
        memset(scanRsp, 0, sizeof(scanRsp));
        scanRsp[3] = 200U;
        ExpectRejected("LE Set Extended Scan Rsp, lying count", 0x2038,
                       scanRsp, 8U, 0x12);
    }

    {
        uint8_t advEnable[8];
        memset(advEnable, 0, sizeof(advEnable));
        advEnable[1] = 8U;   /* num_sets, but only 6 bytes of array follow */
        ExpectRejected("LE Set Extended Adv Enable, lying count", 0x2039,
                       advEnable, 8U, 0x12);

        const size_t setSize =
            sizeof(sdc_hci_le_set_ext_adv_enable_array_params_t);
        advEnable[1] = 1U;
        ExpectComplete("LE Set Extended Adv Enable, honest count", 0x2039,
                       advEnable,
                       offsetof(sdc_hci_cmd_le_set_ext_adv_enable_t,
                                array_params) + setSize, 0U);
    }

    {
        /*
         * One array element per PHY named in the bitmap, Vol 4 Part E 7.8.64.
         * Three PHYs named with room for one is a rejection.
         */
        uint8_t scanParams[32];
        const size_t phySize =
            sizeof(sdc_hci_le_set_ext_scan_params_array_params_t);
        const size_t fixed =
            offsetof(sdc_hci_cmd_le_set_ext_scan_params_t, array_params);

        memset(scanParams, 0, sizeof(scanParams));
        scanParams[2] = 0x07U;
        ExpectRejected("LE Set Extended Scan Params, three phys", 0x2041,
                       scanParams, fixed + phySize, 0x12);

        scanParams[2] = 0x01U;
        ExpectComplete("LE Set Extended Scan Params, one phy", 0x2041,
                       scanParams, fixed + phySize, 0U);
    }

    {
        uint8_t extConn[64];
        const size_t phySize =
            sizeof(sdc_hci_le_ext_create_conn_array_params_t);
        const size_t fixed =
            offsetof(sdc_hci_cmd_le_ext_create_conn_t, array_params);

        memset(extConn, 0, sizeof(extConn));
        extConn[9] = 0x07U;  /* initiating_phys */
        ExpectRejectedStatus("LE Extended Create Conn, three phys", 0x2043,
                             extConn, fixed + phySize, 0x12);

        extConn[9] = 0x01U;
        ExpectStatus("LE Extended Create Conn, one phy", 0x2043, extConn,
                     fixed + phySize);
    }

    /*
     * A wrong length is caught in the dispatcher rather than the handler, and
     * the answer still has to take the shape the opcode would have used on
     * success. Both of these answer with Command Status, Vol 4 Part E 7.7.15.
     */
    ExpectRejectedStatus("LE Set PHY, wrong length", 0x2032, zeros, 3U, 0x12);
    ExpectRejectedStatus("Disconnect, wrong length", 0x0406, zeros, 1U, 0x12);
    ExpectRejectedStatus("LE Create Connection, wrong length", 0x200D, zeros,
                         2U, 0x12);

    /* A Command Complete opcode keeps Command Complete. */
    ExpectRejected("LE Encrypt, wrong length", 0x2017, zeros, 3U, 0x12);

    /* An unassigned opcode has no entry to take a shape from. */
    ExpectRejected("unassigned opcode", 0x0CFF, zeros, 0U, 0x01);

    /*
     * A rejection must still carry the full declared return parameters. The
     * Return_Parameters encoding is fixed, and BlueZ discards a Command
     * Complete shorter than the minimum it holds for the opcode, so a short
     * event reads to the host as no answer at all.
     */
    ExpectRejectedLen("LE Read Channel Map, wrong length", 0x2015, zeros, 1U,
                      0x12,
                      sizeof(sdc_hci_cmd_le_read_channel_map_return_t));
    ExpectRejectedLen("LE Read PHY, wrong length", 0x2030, zeros, 1U, 0x12,
                      sizeof(sdc_hci_cmd_le_read_phy_return_t));
    ExpectRejectedLen("LE Encrypt, wrong length carries return", 0x2017, zeros,
                      3U, 0x12, sizeof(sdc_hci_cmd_le_encrypt_return_t));

    /* The same applies when the controller itself refuses the command. */
    g_SdcStub.NextStatus = 0x0C;
    Response mapBusy = Exchange(0x2015, zeros,
                                sizeof(sdc_hci_cmd_le_read_channel_map_t));
    assert(mapBusy.code == EVENT_COMMAND_COMPLETE);
    assert(mapBusy.status == 0x0C);
    assert(mapBusy.return_len ==
           sizeof(sdc_hci_cmd_le_read_channel_map_return_t));
    printf("[ok] %-38s error keeps the %zu byte return\n",
           "LE Read Channel Map refused",
           sizeof(sdc_hci_cmd_le_read_channel_map_return_t));

    /* A controller error is passed through rather than masked. */
    g_SdcStub.NextStatus = 0x0C;
    Response busy = Exchange(0x2018, zeros, 0U);
    assert(busy.code == EVENT_COMMAND_COMPLETE);
    assert(busy.status == 0x0C);
    assert(busy.return_len == sizeof(sdc_hci_cmd_le_rand_return_t));
    printf("[ok] %-38s error passed through\n", "LE Rand refused");

    g_SdcStub.NextStatus = 0x0C;
    Response refused = Exchange(0x200D, zeros,
                                sizeof(sdc_hci_cmd_le_create_conn_t));
    assert(refused.code == EVENT_COMMAND_STATUS);
    assert(refused.status == 0x0C);
    printf("[ok] %-38s error passed through\n", "LE Create Connection refused");

    /*
     * Table self consistency. Every fixed length entry declares a response
     * kind and a return parameter length, and both are checked against what
     * the dispatcher actually emits, once on a success and once on a
     * rejection. This is what keeps the declared values honest across all
     * entries rather than the handful named individually above.
     */
    {
        const HciCmdEntry_t *pEntries = gSdc.Commands.pEntries;
        const size_t count = gSdc.Commands.EntryCount;
        size_t checked = 0U;
        size_t skipped = 0U;

        assert(pEntries != NULL && count > 0U);

        for (size_t i = 0U; i < count; i++)
        {
            const HciCmdEntry_t *pEntry = &pEntries[i];

            if (pEntry->ParamLen == HCI_CMD_VARIABLE_PARAM_LEN)
            {
                skipped++;
                continue;
            }

            const uint8_t expectedCode =
                pEntry->Response == HCI_CMD_RESPONSE_STATUS ?
                EVENT_COMMAND_STATUS : EVENT_COMMAND_COMPLETE;

            /*
             * One command carries a variable tail, so its declared length is
             * the minimum rather than the whole answer. Every other entry
             * emits exactly what it declares.
             */
            const bool variableReturn =
                pEntry->Opcode ==
                SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_STATIC_ADDRESSES;

            /* Success: the declared return length is what comes out. */
            g_SdcStub.NextStatus = 0x00;
            Response ok = Exchange(pEntry->Opcode, zeros, pEntry->ParamLen);
            assert(ok.code == expectedCode);
            assert(ok.opcode == pEntry->Opcode);
            assert(ok.status == 0x00);
            if (expectedCode == EVENT_COMMAND_COMPLETE)
            {
                if (variableReturn)
                {
                    assert(ok.return_len >= pEntry->ReturnLen);
                }
                else
                {
                    assert(ok.return_len == pEntry->ReturnLen);
                }
            }

            /* Rejection: same shape, same length, different status. */
            Response bad = Exchange(pEntry->Opcode, zeros,
                                    (size_t)pEntry->ParamLen + 1U);
            assert(bad.code == expectedCode);
            assert(bad.opcode == pEntry->Opcode);
            assert(bad.status == 0x12);
            if (expectedCode == EVENT_COMMAND_COMPLETE)
            {
                assert(bad.return_len == pEntry->ReturnLen);
            }

            checked++;
        }

        printf("[ok] %-38s %zu entries, %zu variable skipped\n",
               "table declares what it emits", checked, skipped);
    }

    /*
     * The dispatch table and the supported commands bitmap are both written by
     * hand, and Vol 4 Part E 6.27 says the bitmap names what the controller
     * will actually accept. A host reads the bitmap and never tries an opcode
     * whose bit is clear, so a table entry with no bit is dead, and a bit with
     * no table entry is a promise the controller breaks with Unknown HCI
     * Command. The two are checked against each other in both directions here,
     * so adding one without the other fails the build rather than the host.
     */
    {
        typedef uint8_t (*BitReader)(const sdc_hci_ip_supported_commands_t *);

        struct BitMap {
            uint16_t Opcode;
            BitReader Bit;      /* NULL where the specification assigns none */
            const char *Name;
        };

#define BITMAP_ENTRY(Opcode, Field)                                           \
    {Opcode,                                                                  \
     [](const sdc_hci_ip_supported_commands_t *p) -> uint8_t                  \
     { return (uint8_t)p->Field; },                                           \
     #Field}

        const BitMap map[] = {
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_CB_SET_EVENT_MASK,
                         hci_set_event_mask),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_CB_RESET, hci_reset),
#if HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_CB_READ_AUTHENTICATED_PAYLOAD_TIMEOUT,
                hci_read_authenticated_payload_timeout),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_CB_WRITE_AUTHENTICATED_PAYLOAD_TIMEOUT,
                hci_write_authenticated_payload_timeout),
#endif
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_VERSION_INFORMATION,
                         hci_read_local_version_information),
            /*
             * Read Local Supported Commands has no bit of its own. Octet 14
             * bit 4, where one would sit by position, is reserved.
             */
            {SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_COMMANDS, NULL,
             "hci_read_local_supported_commands"},
#if HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES
            /*
             * Vendor specific. Vol 4 Part E 6.27 covers the opcodes the
             * specification assigns and has no bit for anything in the 0x3F
             * opcode group, so this row carries no bit either.
             */
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_STATIC_ADDRESSES, NULL,
             "vs_zephyr_read_static_addresses"},
#endif
#if HCI_SDC_HAS_VS_READ_COUNTERS
            {HCI_SDC_OPCODE_VS_READ_COUNTERS, NULL, "vs_read_counters"},
#endif
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_FEATURES,
                         hci_read_local_supported_features),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_IP_READ_BD_ADDR, hci_read_bd_addr),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EVENT_MASK,
                         hci_le_set_event_mask),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_BUFFER_SIZE,
                         hci_le_read_buffer_size_v1),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_LOCAL_SUPPORTED_FEATURES,
                         hci_le_read_local_supported_features),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_RANDOM_ADDRESS,
                         hci_le_set_random_address),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_ADV_PARAMS,
                         hci_le_set_advertising_parameters),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_READ_ADV_PHYSICAL_CHANNEL_TX_POWER,
                hci_le_read_advertising_physical_channel_tx_power),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_ADV_DATA,
                         hci_le_set_advertising_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_RESPONSE_DATA,
                         hci_le_set_scan_response_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_ADV_ENABLE,
                         hci_le_set_advertising_enable),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_PARAMS,
                         hci_le_set_scan_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_SCAN_ENABLE,
                         hci_le_set_scan_enable),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LC_DISCONNECT, hci_disconnect),
#if HCI_SDC_HAS_READ_REMOTE_VERSION
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LC_READ_REMOTE_VERSION_INFORMATION,
                hci_read_remote_version_information),
#endif

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN,
                         hci_le_create_connection),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CREATE_CONN_CANCEL,
                         hci_le_create_connection_cancel),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CONN_UPDATE,
                         hci_le_connection_update),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_CHANNEL_MAP,
                         hci_le_read_channel_map),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_REMOTE_FEATURES,
                         hci_le_read_remote_features),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_FILTER_ACCEPT_LIST_SIZE,
                         hci_le_read_filter_accept_list_size),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CLEAR_FILTER_ACCEPT_LIST,
                         hci_le_clear_filter_accept_list),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_FILTER_ACCEPT_LIST,
                hci_le_add_device_to_filter_accept_list),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_FILTER_ACCEPT_LIST,
                hci_le_remove_device_from_filter_accept_list),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_ENCRYPT, hci_le_encrypt),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_RAND, hci_le_rand),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_ENABLE_ENCRYPTION,
                         hci_le_enable_encryption),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_REPLY,
                         hci_le_long_term_key_request_reply),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_LONG_TERM_KEY_REQUEST_NEGATIVE_REPLY,
                hci_le_long_term_key_request_negative_reply),

#if HCI_SDC_HAS_READ_SUPPORTED_STATES
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_SUPPORTED_STATES,
                         hci_le_read_supported_states),
#endif
#if HCI_SDC_HAS_READ_TRANSMIT_POWER
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_TRANSMIT_POWER,
                         hci_le_read_transmit_power),
#endif

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V1,
                         hci_le_receiver_test_v1),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V1,
                         hci_le_transmitter_test_v1),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_TEST_END, hci_le_test_end),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_DATA_LENGTH,
                         hci_le_set_data_length),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_READ_SUGGESTED_DEFAULT_DATA_LENGTH,
                hci_le_read_suggested_default_data_length),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_WRITE_SUGGESTED_DEFAULT_DATA_LENGTH,
                hci_le_write_suggested_default_data_length),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_MAX_DATA_LENGTH,
                         hci_le_read_maximum_data_length),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_PHY, hci_le_read_phy),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PHY,
                         hci_le_set_default_phy),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PHY, hci_le_set_phy),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_ADV_SET_RANDOM_ADDRESS,
                         hci_le_set_advertising_set_random_address),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_PARAMS,
                         hci_le_set_extended_advertising_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_DATA,
                         hci_le_set_extended_advertising_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_RESPONSE_DATA,
                         hci_le_set_extended_scan_response_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_ADV_ENABLE,
                         hci_le_set_extended_advertising_enable),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_MAX_ADV_DATA_LENGTH,
                         hci_le_read_maximum_advertising_data_length),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_READ_NUMBER_OF_SUPPORTED_ADV_SETS,
                hci_le_read_number_of_supported_advertising_sets),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_REMOVE_ADV_SET,
                         hci_le_remove_advertising_set),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CLEAR_ADV_SETS,
                         hci_le_clear_advertising_sets),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_PARAMS,
                         hci_le_set_extended_scan_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_EXT_SCAN_ENABLE,
                         hci_le_set_extended_scan_enable),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_EXT_CREATE_CONN,
                         hci_le_extended_create_connection),
        };

#undef BITMAP_ENTRY

        const size_t mapCount = sizeof(map) / sizeof(map[0]);

        g_SdcStub.NextStatus = 0x00;
        Response bitmap = Exchange(
            SDC_HCI_OPCODE_CMD_IP_READ_LOCAL_SUPPORTED_COMMANDS, zeros, 0U);
        assert(bitmap.code == EVENT_COMMAND_COMPLETE);
        assert(bitmap.status == 0x00);
        assert(bitmap.return_len ==
               sizeof(sdc_hci_cmd_ip_read_local_supported_commands_return_t));

        sdc_hci_cmd_ip_read_local_supported_commands_return_t reported;
        memcpy(&reported, gLastReturn, sizeof(reported));

        const HciCmdEntry_t *pEntries = gSdc.Commands.pEntries;
        const size_t count = gSdc.Commands.EntryCount;

        /* Every opcode the table accepts is named in the bitmap. */
        for (size_t i = 0U; i < count; i++)
        {
            bool found = false;
            for (size_t j = 0U; j < mapCount && !found; j++)
            {
                if (map[j].Opcode != pEntries[i].Opcode)
                {
                    continue;
                }
                found = true;
                if (map[j].Bit != NULL && map[j].Bit(&reported.params) == 0U)
                {
                    printf("opcode 0x%04X is dispatched but %s is clear\n",
                           pEntries[i].Opcode, map[j].Name);
                    assert(false);
                }
            }
            if (!found)
            {
                printf("opcode 0x%04X is dispatched but has no bitmap row\n",
                       pEntries[i].Opcode);
                assert(false);
            }
        }

        /* And every bit the bitmap sets has an opcode the table accepts. */
        for (size_t j = 0U; j < mapCount; j++)
        {
            if (map[j].Bit == NULL || map[j].Bit(&reported.params) == 0U)
            {
                continue;
            }
            bool found = false;
            for (size_t i = 0U; i < count && !found; i++)
            {
                found = pEntries[i].Opcode == map[j].Opcode;
            }
            if (!found)
            {
                printf("%s is set but opcode 0x%04X is not dispatched\n",
                       map[j].Name, map[j].Opcode);
                assert(false);
            }
        }

        printf("[ok] %-38s %zu opcodes both ways\n",
               "bitmap agrees with the table", count);
    }

#if HCI_SDC_HAS_VS_READ_COUNTERS
    /*
     * The readout is only worth having if the numbers move, and a block of
     * zeros passes every length and shape check there is. Read it, provoke two
     * specific refusals, read it again, and require exactly those two counters
     * to have advanced by one. Deltas rather than absolutes, since everything
     * above this point has already been counted.
     */
    {
        static uint32_t before[HCI_SDC_COUNTERS_COUNT];
        static uint32_t after[HCI_SDC_COUNTERS_COUNT];

        g_SdcStub.NextStatus = 0x00;
        Exchange(HCI_SDC_OPCODE_VS_READ_COUNTERS, zeros, 0U);
        ReadCounters(before);

        /* No entry for this opcode, so it lands on UnknownCommandCount. */
        Exchange(0x0CFF, zeros, 0U);
        /* A known opcode with a length its entry does not accept. */
        Exchange(0x2017, zeros, 3U);

        g_SdcStub.NextStatus = 0x00;
        Exchange(HCI_SDC_OPCODE_VS_READ_COUNTERS, zeros, 0U);
        ReadCounters(after);

        assert(after[COUNTER_UNKNOWN_COMMAND] -
               before[COUNTER_UNKNOWN_COMMAND] == 1U);
        assert(after[COUNTER_INVALID_PARAM_LEN] -
               before[COUNTER_INVALID_PARAM_LEN] == 1U);

        /*
         * Three commands went by between the two readings, the two refusals
         * and the second reading itself. The first reading is already counted
         * in before[].
         */
        assert(after[COUNTER_COMMAND] - before[COUNTER_COMMAND] == 3U);

        /* Nothing here touches the radio, so no ACL counter may have moved. */
        assert(after[COUNTER_ACL_PUT_ERROR] == before[COUNTER_ACL_PUT_ERROR]);
        assert(after[COUNTER_PUT_RETRY] == before[COUNTER_PUT_RETRY]);

        printf("[ok] %-38s unknown +1, bad length +1, commands +3\n",
               "counters follow what the layer refused");
    }
#endif

    printf("All SDC dispatch tests passed.\n");
    return 0;
}
