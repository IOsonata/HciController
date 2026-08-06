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
#include "sdc_hci_cmd_status_params.h"
#include "sdc_hci_vs.h"
#include "sdc_stub.h"

#ifndef HCI_SDC_HAS_READ_SUPPORTED_STATES
#define HCI_SDC_HAS_READ_SUPPORTED_STATES 0
#endif

#ifndef HCI_SDC_HAS_READ_TRANSMIT_POWER
#define HCI_SDC_HAS_READ_TRANSMIT_POWER 1
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

/*
 * No bridge in this test, so the upper layers read zero. That is the documented
 * behaviour for a layer that is absent rather than a reason to refuse.
 */
static HciCounters_t gCounters;
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

/*
 * A command that answers nothing when it works. Vol 4 Part E 7.3.40 makes Host
 * Number Of Completed Packets the one such command here, and the point of the
 * check is the silence: the controller has to reach SDC and then emit no
 * event, because a host that is told nothing is what the specification
 * promises and an unexpected Command Complete would spend a credit the host
 * never lent.
 */
static void ExpectSilent(const char *label, uint16_t opcode,
                         const uint8_t *pParams, size_t len)
{
    uint8_t packet[300];
    packet[0] = (uint8_t)opcode;
    packet[1] = (uint8_t)(opcode >> 8);
    packet[2] = (uint8_t)len;
    if (len > 0U)
    {
        memcpy(&packet[3], pParams, len);
    }

    g_SdcStub.NextStatus = 0x00;
    g_SdcStub.LastCall = NULL;

    /*
     * Offered the same way Exchange does, because the routing layer holds the
     * next command until the controller queue has had the outgoing slot and
     * refuses the first offer. The difference is what is asserted afterwards:
     * every Get has to come back empty.
     */
    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t out[300];
    size_t outLen = 0U;
    bool put = false;

    for (unsigned pass = 0U; pass < 4U; pass++)
    {
        if (!put)
        {
            put = gOps->Put(gOps->pContext, HCI_H4_PACKET_COMMAND, packet,
                            3U + len);
        }
        assert(gOps->Get(gOps->pContext, &type, out, sizeof(out), &outLen) !=
               HCI_CONTROLLER_GET_PACKET);
    }

    assert(put);
    assert(g_SdcStub.LastCall != NULL);
    printf("[ok] %-38s no event, %s\n", label, g_SdcStub.LastCall);
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
/* Positions in the counter block, fixed by hci_counters.h. */
enum {
    COUNTER_COMMAND = 0,
    COUNTER_UNKNOWN_COMMAND = 1,
    COUNTER_INVALID_PARAM_LEN = 3,
    COUNTER_ACL_PUT_ERROR = 6,
    COUNTER_PUT_RETRY = 8,
    COUNTER_ACL_PUT = 16,
};

/* Unpacks the block left in gLastReturn by the most recent readout. */
static void ReadCounters(uint32_t *pCounters)
{
    for (size_t i = 0U; i < HCI_COUNTERS_COUNT; i++)
    {
        const uint8_t *p = &gLastReturn[1U + (i * 4U)];
        pCounters[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
}

/* One field of it, for the cases that care about a single index. */
static uint32_t ReadCounter(size_t Index)
{
    const uint8_t *p = &gLastReturn[1U + (Index * 4U)];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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

    HciCountersInit(&gCounters, &gSdc, NULL);
    assert(HciSdcNrfxlibInit(&gSdc, gEventBuffer, sizeof(gEventBuffer),
                             &gCounters));
    gOps = HciSdcGetControllerOps(&gSdc);
    assert(gOps != NULL);

    /*
     * The controller announces itself before it answers anything, so the very
     * first event out is the No Operation Command Complete and every exchange
     * below would otherwise read it as a command response. Take it here, and
     * check it while it is in hand: five octets, no status, and the command
     * credit that lets the host start.
     *
     * Queued explicitly rather than under a build macro. It used to be tested
     * with #if against a default of off, so the check never ran in any build
     * anyone made, and that is what let a board asking for the NOP ship with
     * the call compiled out.
     */
    HciSdcNrfxlibQueueStartupNop(&gSdc);

    {
        HciH4PacketType_t nopType = HCI_H4_PACKET_NONE;
        uint8_t nop[16];
        size_t nopLen = 0U;
        assert(gOps->Get(gOps->pContext, &nopType, nop, sizeof(nop),
                         &nopLen) == HCI_CONTROLLER_GET_PACKET);
        assert(nopType == HCI_H4_PACKET_EVENT);
        assert(nopLen == 5U);
        assert(nop[0] == EVENT_COMMAND_COMPLETE);
        assert(nop[1] == 3U);
        assert(nop[2] == 1U);
        assert(nop[3] == 0x00U && nop[4] == 0x00U);
        printf("[ok] %-38s before anything else\n", "startup NOP goes out");
    }

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
    ExpectComplete("VS Zephyr Read Version Info", 0xFC01, zeros, 0U,
                   sizeof(sdc_hci_cmd_vs_zephyr_read_version_info_return_t));
    ExpectComplete("VS Zephyr Write BD_ADDR", 0xFC06, zeros,
                   sizeof(sdc_hci_cmd_vs_zephyr_write_bd_addr_t), 0U);
    ExpectComplete("VS Zephyr Read Chip Temperature", 0xFC0B, zeros, 0U,
                   sizeof(sdc_hci_cmd_vs_zephyr_read_chip_temp_return_t));
    ExpectComplete("VS Zephyr Write Tx Power", 0xFC0E, zeros,
                   sizeof(sdc_hci_cmd_vs_zephyr_write_tx_power_t),
                   sizeof(sdc_hci_cmd_vs_zephyr_write_tx_power_return_t));
    ExpectComplete("VS Zephyr Read Tx Power", 0xFC0F, zeros,
                   sizeof(sdc_hci_cmd_vs_zephyr_read_tx_power_t),
                   sizeof(sdc_hci_cmd_vs_zephyr_read_tx_power_return_t));

    {
        /*
         * Read Supported Commands is the one that does more than forward. SDC
         * answers with what SDC implements, and this layer dispatches a subset,
         * so the answer is masked down to what the table can actually reach.
         *
         * The stub fills the whole bitmap with 0x5A, which lands set bits on
         * both sides of the mask. So this checks both directions: a command
         * SDC offers and the table does not carry is cleared, and one the
         * table does carry survives. Clearing everything would pass a one
         * sided test.
         */
        ExpectComplete(
            "VS Zephyr Read Supported Commands", 0xFC02, zeros, 0U,
            sizeof(sdc_hci_cmd_vs_zephyr_read_supported_commands_return_t));

        sdc_hci_cmd_vs_zephyr_read_supported_commands_return_t reported;
        memcpy(&reported, gLastReturn, sizeof(reported));

        /* 0x5A sets these, and the table has no row for any of them. */
        assert(reported.params.set_event_mask == 0U);
        assert(reported.params.reset == 0U);
        assert(reported.params.set_trace_enable == 0U);
        assert(reported.params.read_host_stack_commands == 0U);
        assert(reported.params.set_scan_request_reports == 0U);

        /* 0x5A sets these too, and the table does carry them. */
        assert(reported.params.read_tx_power_level == 1U);
        assert(reported.params.read_key_hierarchy_roots == 1U);

        printf("[ok] %-38s %s\n", "vendor bitmap is masked to the table",
               "unreachable bits cleared, reachable kept");
    }
    ExpectComplete(
        "VS Zephyr Read Key Hierarchy Roots", 0xFC0A, zeros, 0U,
        sizeof(sdc_hci_cmd_vs_zephyr_read_key_hierarchy_roots_return_t));
    ExpectComplete("VS QoS Conn Event Report Enable", 0xFD04, zeros,
                   sizeof(sdc_hci_cmd_vs_qos_conn_event_report_enable_t), 0U);
    ExpectComplete("VS QoS Channel Survey Enable", 0xFD0E, zeros,
                   sizeof(sdc_hci_cmd_vs_qos_channel_survey_enable_t), 0U);
    ExpectComplete("VS Read Average RSSI", 0xFD11, zeros,
                   sizeof(sdc_hci_cmd_vs_read_average_rssi_t),
                   sizeof(sdc_hci_cmd_vs_read_average_rssi_return_t));
    ExpectComplete("VS Get Next Conn Event Counter", 0xFD14, zeros,
                   sizeof(sdc_hci_cmd_vs_get_next_conn_event_counter_t),
                   sizeof(sdc_hci_cmd_vs_get_next_conn_event_counter_return_t));
    ExpectComplete(
        "VS Conn Anchor Point Update Enable", 0xFD1F, zeros,
        sizeof(sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t),
        0U);

    /*
     * Channel survey carries a four octet interval after the enable byte, so
     * the enable byte on its own is the shape a host would send if it had
     * mistaken this command for the other two enables here. Refused rather
     * than passed on with four octets of whatever the last packet left.
     */
    ExpectRejected("VS QoS Channel Survey Enable, enable byte only", 0xFD0E,
                   zeros, 1U, 0x12);
    ExpectComplete("Read Transmit Power Level", 0x0C2D, zeros,
                   sizeof(sdc_hci_cmd_cb_read_transmit_power_level_t),
                   sizeof(sdc_hci_cmd_cb_read_transmit_power_level_return_t));
    ExpectComplete("LE Read RF Path Compensation", 0x204C, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_rf_path_compensation_return_t));
    ExpectComplete("LE Write RF Path Compensation", 0x204D, zeros,
                   sizeof(sdc_hci_cmd_le_write_rf_path_compensation_t), 0U);
    ExpectComplete(
        "LE Enhanced Read Transmit Power", 0x2076, zeros,
        sizeof(sdc_hci_cmd_le_enhanced_read_transmit_power_level_t),
        sizeof(sdc_hci_cmd_le_enhanced_read_transmit_power_level_return_t));

    /*
     * The one whose answer arrives twice. Vol 4 Part E 7.8.118 gives a Command
     * Status here and an LE Transmit Power Reporting event later, once the
     * controller has asked the peer. A Command Complete would be wrong even
     * with the right length, so this is the assertion that matters.
     */
    ExpectStatus("LE Read Remote Transmit Power", 0x2077, zeros,
                 sizeof(sdc_hci_cmd_le_read_remote_transmit_power_level_t));

    ExpectComplete(
        "LE Set Path Loss Reporting Params", 0x2078, zeros,
        sizeof(sdc_hci_cmd_le_set_path_loss_reporting_params_t),
        sizeof(sdc_hci_cmd_le_set_path_loss_reporting_params_return_t));
    ExpectComplete(
        "LE Set Path Loss Reporting Enable", 0x2079, zeros,
        sizeof(sdc_hci_cmd_le_set_path_loss_reporting_enable_t),
        sizeof(sdc_hci_cmd_le_set_path_loss_reporting_enable_return_t));
    ExpectComplete(
        "LE Set Transmit Power Reporting Enable", 0x207A, zeros,
        sizeof(sdc_hci_cmd_le_set_transmit_power_reporting_enable_t),
        sizeof(sdc_hci_cmd_le_set_transmit_power_reporting_enable_return_t));

    /*
     * Three commands here take a handle and differ only in what follows it,
     * and two of them are 3 octets while a third is 8. A host that sends the
     * short one to the long opcode has confused them, and the length check is
     * the only thing that catches it before SDC reads past the packet.
     */
    ExpectRejected("LE Set Path Loss Reporting Params, too short", 0x2078,
                   zeros,
                   sizeof(sdc_hci_cmd_le_set_path_loss_reporting_enable_t),
                   0x12);

    /*
     * And the Command Status one still answers with a status when the length
     * is wrong, rather than falling back to a Command Complete. Vol 4 Part E
     * 4.5, the response kind belongs to the opcode and not to the outcome.
     */
    ExpectRejectedStatus(
        "LE Read Remote Transmit Power, wrong length", 0x2077, zeros,
        sizeof(sdc_hci_cmd_le_read_remote_transmit_power_level_t) - 1U, 0x12);

    ExpectComplete(
        "LE Set Host Channel Classification", 0x2014, zeros,
        sizeof(sdc_hci_cmd_le_set_host_channel_classification_t), 0U);
    ExpectComplete("VS Set Adv Randomness", 0xFD0C, zeros,
                   sizeof(sdc_hci_cmd_vs_set_adv_randomness_t), 0U);
    ExpectComplete("VS LLPM Mode Set", 0xFD01, zeros,
                   sizeof(sdc_hci_cmd_vs_llpm_mode_set_t), 0U);
    /* Command Status, then a VS Connection Update Complete event. */
    ExpectStatus("VS Connection Update", 0xFD02, zeros,
                 sizeof(sdc_hci_cmd_vs_conn_update_t));
    ExpectComplete("LE Set Default Subrate", 0x207D, zeros,
                   sizeof(sdc_hci_cmd_le_set_default_subrate_t), 0U);

    /*
     * Three commands that answer twice: a Command Status now and an LE meta
     * event once the peer has replied. Vol 4 Part E 7.8.108, 7.8.124 and
     * 7.8.150. A Command Complete here would be wrong at any length, so the
     * event kind is what these assert.
     */
    ExpectStatus("LE Request Peer SCA", 0x206D, zeros,
                 sizeof(sdc_hci_cmd_le_request_peer_sca_t));
    ExpectStatus("LE Subrate Request", 0x207E, zeros,
                 sizeof(sdc_hci_cmd_le_subrate_request_t));

    /*
     * Set Default Subrate is 10 octets and Subrate Request is 12, the same
     * five parameters with a handle in front. Sending the shorter one to the
     * longer opcode is the mistake this pair invites, and it is refused with
     * a status rather than a complete because the opcode decides that.
     */
    ExpectRejectedStatus("LE Subrate Request, missing the handle", 0x207E,
                         zeros,
                         sizeof(sdc_hci_cmd_le_set_default_subrate_t), 0x12);
    ExpectStatus("LE Read All Remote Features", 0x2088, zeros,
                 sizeof(sdc_hci_cmd_le_read_all_remote_features_t));

    ExpectComplete("LE Set Periodic Adv Params", 0x203E, zeros,
                   sizeof(sdc_hci_cmd_le_set_periodic_adv_params_t), 0U);
    ExpectComplete("LE Set Periodic Adv Enable", 0x2040, zeros,
                   sizeof(sdc_hci_cmd_le_set_periodic_adv_enable_t), 0U);

    {
        /*
         * Byte counted trailing array, the same shape as extended advertising
         * data, so it gets the same two cases: an empty body is the fixed part
         * alone, and a count that disagrees with the length is refused rather
         * than handed to SDC to read past the packet.
         */
        const size_t head =
            offsetof(sdc_hci_cmd_le_set_periodic_adv_data_t, adv_data);

        ExpectComplete("LE Set Periodic Adv Data, empty", 0x203F, zeros, head,
                       0U);

        const uint8_t honest[] = {0x00U, 0x03U, 0x04U,
                                  0xAAU, 0xBBU, 0xCCU, 0xDDU};
        ExpectComplete("LE Set Periodic Adv Data, honest count", 0x203F,
                       honest, sizeof(honest), 0U);

        const uint8_t lying[] = {0x00U, 0x03U, 0x08U, 0xAAU, 0xBBU};
        ExpectRejected("LE Set Periodic Adv Data, lying count", 0x203F, lying,
                       sizeof(lying), 0x12);
    }

    /*
     * Create Sync answers a status now and a Sync Established event once the
     * controller has actually found the train, or Sync Lost if it never does.
     * Vol 4 Part E 7.8.67. A Command Complete would be wrong at any length.
     */
    ExpectStatus("LE Periodic Adv Create Sync", 0x2044, zeros,
                 sizeof(sdc_hci_cmd_le_periodic_adv_create_sync_t));
    ExpectComplete("LE Periodic Adv Create Sync Cancel", 0x2045, zeros, 0U,
                   0U);
    ExpectComplete("LE Periodic Adv Terminate Sync", 0x2046, zeros,
                   sizeof(sdc_hci_cmd_le_periodic_adv_terminate_sync_t), 0U);
    ExpectComplete(
        "LE Add Device To Periodic Adv List", 0x2047, zeros,
        sizeof(sdc_hci_cmd_le_add_device_to_periodic_adv_list_t), 0U);
    ExpectComplete(
        "LE Remove Device From Periodic Adv List", 0x2048, zeros,
        sizeof(sdc_hci_cmd_le_remove_device_from_periodic_adv_list_t), 0U);
    ExpectComplete("LE Clear Periodic Adv List", 0x2049, zeros, 0U, 0U);
    ExpectComplete(
        "LE Read Periodic Adv List Size", 0x204A, zeros, 0U,
        sizeof(sdc_hci_cmd_le_read_periodic_adv_list_size_return_t));

    /*
     * Add and Remove take the same eight octets, and Terminate Sync takes two.
     * A host that sends a sync handle to the list commands has confused them.
     */
    ExpectRejected("LE Add Device To Periodic Adv List, handle sized", 0x2047,
                   zeros,
                   sizeof(sdc_hci_cmd_le_periodic_adv_terminate_sync_t), 0x12);

    ExpectComplete(
        "LE Set Periodic Adv Receive Enable", 0x2059, zeros,
        sizeof(sdc_hci_cmd_le_set_periodic_adv_receive_enable_t), 0U);
    ExpectComplete(
        "LE Periodic Adv Sync Transfer", 0x205A, zeros,
        sizeof(sdc_hci_cmd_le_periodic_adv_sync_transfer_t),
        sizeof(sdc_hci_cmd_le_periodic_adv_sync_transfer_return_t));
    ExpectComplete(
        "LE Periodic Adv Set Info Transfer", 0x205B, zeros,
        sizeof(sdc_hci_cmd_le_periodic_adv_set_info_transfer_t),
        sizeof(sdc_hci_cmd_le_periodic_adv_set_info_transfer_return_t));
    ExpectComplete(
        "LE Set Periodic Adv Sync Transfer Params", 0x205C, zeros,
        sizeof(sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_t),
        sizeof(
            sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params_return_t));
    ExpectComplete(
        "LE Set Default Periodic Adv Sync Transfer Params", 0x205D, zeros,
        sizeof(
            sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params_t),
        0U);

    /*
     * The transfer params pair differ only by a leading connection handle, 8
     * octets against 6, which is the same trap the subrate pair sets.
     */
    ExpectRejected(
        "LE Set Periodic Adv Sync Transfer Params, no handle", 0x205C, zeros,
        sizeof(sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params_t),
        0x12);

    ExpectComplete(
        "LE Set Periodic Adv Params v2", 0x2086, zeros,
        sizeof(sdc_hci_cmd_le_set_periodic_adv_params_v2_t),
        sizeof(sdc_hci_cmd_le_set_periodic_adv_params_v2_return_t));

    {
        /*
         * Subevent Data is the one command here whose trailing array is not an
         * array. Vol 4 Part E 7.8.125 gives entries of four octets plus a
         * declared data length each, so the count cannot be multiplied by
         * anything and the handler walks them instead.
         *
         * These cases are what make the walk worth having: a count of zero, a
         * well formed pair of unequal entries, an entry whose declared length
         * runs off the end, and a body longer than the entries account for.
         */
        const size_t head =
            offsetof(sdc_hci_cmd_le_set_periodic_adv_subevent_data_t,
                     array_params);

        ExpectComplete("LE Set Periodic Adv Subevent Data, none", 0x2082,
                       zeros, head,
                       sizeof(
                           sdc_hci_cmd_le_set_periodic_adv_subevent_data_return_t));

        /*
         * Handle, two entries. First carries three octets, second carries one,
         * so a fixed stride would get the second one wrong.
         */
        const uint8_t two[] = {0x00U, 0x02U,
                               0x00U, 0x00U, 0x01U, 0x03U, 0xAAU, 0xBBU, 0xCCU,
                               0x01U, 0x00U, 0x01U, 0x01U, 0xDDU};
        ExpectComplete(
            "LE Set Periodic Adv Subevent Data, two unequal", 0x2082, two,
            sizeof(two),
            sizeof(sdc_hci_cmd_le_set_periodic_adv_subevent_data_return_t));

        /* One entry declaring eight octets of data with three supplied. */
        const uint8_t over[] = {0x00U, 0x01U,
                                0x00U, 0x00U, 0x01U, 0x08U, 0xAAU, 0xBBU,
                                0xCCU};
        ExpectRejected("LE Set Periodic Adv Subevent Data, entry overruns",
                       0x2082, over, sizeof(over), 0x12);

        /* One entry, correctly formed, then a stray octet after it. */
        const uint8_t trailing[] = {0x00U, 0x01U,
                                    0x00U, 0x00U, 0x01U, 0x01U, 0xAAU, 0xFFU};
        ExpectRejected("LE Set Periodic Adv Subevent Data, surplus", 0x2082,
                       trailing, sizeof(trailing), 0x12);

        /* Two declared, one supplied. */
        const uint8_t shortCount[] = {0x00U, 0x02U,
                                      0x00U, 0x00U, 0x01U, 0x01U, 0xAAU};
        ExpectRejected("LE Set Periodic Adv Subevent Data, count lies", 0x2082,
                       shortCount, sizeof(shortCount), 0x12);
    }

    {
        /*
         * Both of these are byte counted and answer with a handle, which is a
         * shape nothing else in this table has. The empty and lying cases are
         * what prove the count is checked before SDC sees the packet.
         */
        const size_t rspHead =
            offsetof(sdc_hci_cmd_le_set_periodic_adv_response_data_t,
                     response_data);
        ExpectComplete(
            "LE Set Periodic Adv Response Data, empty", 0x2083, zeros,
            rspHead,
            sizeof(sdc_hci_cmd_le_set_periodic_adv_response_data_return_t));

        const uint8_t lying[] = {0x40U, 0x00U, 0x01U, 0x00U,
                                 0x00U, 0x00U, 0x00U, 0x04U, 0xAAU};
        ExpectRejected("LE Set Periodic Adv Response Data, lying count",
                       0x2083, lying, sizeof(lying), 0x12);

        const size_t subHead =
            offsetof(sdc_hci_cmd_le_set_periodic_sync_subevent_t, subevents);
        ExpectComplete(
            "LE Set Periodic Sync Subevent, none", 0x2084, zeros, subHead,
            sizeof(sdc_hci_cmd_le_set_periodic_sync_subevent_return_t));

        const uint8_t three[] = {0x40U, 0x00U, 0x00U, 0x00U, 0x03U,
                                 0x00U, 0x01U, 0x02U};
        ExpectComplete(
            "LE Set Periodic Sync Subevent, three", 0x2084, three,
            sizeof(three),
            sizeof(sdc_hci_cmd_le_set_periodic_sync_subevent_return_t));

        ExpectRejected("LE Set Periodic Sync Subevent, count lies", 0x2084,
                       three, sizeof(three) - 1U, 0x12);
    }
#if HCI_SDC_HAS_VS_READ_COUNTERS
    ExpectCompleteLocal("VS Read Counters", HCI_COUNTERS_OPCODE,
                        zeros, 0U, HCI_COUNTERS_RETURN_LEN);
    assert(gLastReturn[0] == HCI_COUNTERS_VERSION);
    printf("[ok] %-38s version %u\n", "counter block names its version",
           (unsigned)gLastReturn[0]);

    {
        /*
         * The pool figures at 32 and 33. Nothing here has a platform layer, so
         * they read zero, and that is the case worth pinning: a host has to be
         * able to tell not reported from a controller that wants no memory.
         */
        assert(ReadCounter(32U) == 0U);
        assert(ReadCounter(33U) == 0U);

        HciCountersSetSdcMem(&gCounters, 38860U, 39372U);
        ExpectCompleteLocal("VS Read Counters, pool reported",
                            HCI_COUNTERS_OPCODE, zeros, 0U,
                            HCI_COUNTERS_RETURN_LEN);
        assert(ReadCounter(32U) == 38860U);
        assert(ReadCounter(33U) == 39372U);
        printf("[ok] %-38s required %u of %u\n", "pool figures reach the host",
               (unsigned)ReadCounter(32U), (unsigned)ReadCounter(33U));

        /* Left as the platform would leave them for the rest of the run. */
        HciCountersSetSdcMem(&gCounters, 0U, 0U);
    }
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
     * Controller to host flow control. Two ordinary commands and one that is
     * not: Host Number Of Completed Packets answers nothing when it works and
     * a Command Complete carrying 0x12 when it does not, Vol 4 Part E 7.3.40.
     */
    ExpectComplete(
        "Set Controller To Host Flow Control", 0x0C31, zeros,
        sizeof(sdc_hci_cmd_cb_set_controller_to_host_flow_control_t), 0U);
    ExpectComplete("Host Buffer Size", 0x0C33, zeros,
                   sizeof(sdc_hci_cmd_cb_host_buffer_size_t), 0U);

    {
        /* One handle owing two packets: count, handle, then the count back. */
        const uint8_t completed[] = {0x01U, 0x05U, 0x00U, 0x02U, 0x00U};
        ExpectSilent("Host Number Of Completed Packets", 0x0C35, completed,
                     sizeof(completed));

        /* Two handles declared, one supplied. */
        const uint8_t lying[] = {0x02U, 0x05U, 0x00U, 0x02U, 0x00U};
        ExpectRejected("Host Number Of Completed Packets, count lies", 0x0C35,
                       lying, sizeof(lying), 0x12);

        /* Nothing at all still has to be answered rather than ignored. */
        ExpectRejected("Host Number Of Completed Packets, empty", 0x0C35,
                       zeros, 0U, 0x12);
    }

    /*
     * Privacy and the resolving list. Add Device To Resolving List carries two
     * sixteen octet keys and is now the longest fixed length command the table
     * accepts, at 39, so it is the one that proves the parameter path is not
     * quietly bounded somewhere short of what the specification allows.
     */
    ExpectComplete("LE Add Device To Resolving List", 0x2027, zeros,
                   sizeof(sdc_hci_cmd_le_add_device_to_resolving_list_t), 0U);
    ExpectComplete("LE Remove Device From Resolving List", 0x2028, zeros,
                   sizeof(sdc_hci_cmd_le_remove_device_from_resolving_list_t),
                   0U);
    ExpectComplete("LE Clear Resolving List", 0x2029, zeros, 0U, 0U);
    ExpectComplete("LE Read Resolving List Size", 0x202A, zeros, 0U,
                   sizeof(sdc_hci_cmd_le_read_resolving_list_size_return_t));
    ExpectComplete("LE Set Address Resolution Enable", 0x202D, zeros,
                   sizeof(sdc_hci_cmd_le_set_address_resolution_enable_t), 0U);
    ExpectComplete(
        "LE Set RPA Timeout", 0x202E, zeros,
        sizeof(sdc_hci_cmd_le_set_resolvable_private_address_timeout_t), 0U);
    ExpectComplete("LE Set Privacy Mode", 0x204E, zeros,
                   sizeof(sdc_hci_cmd_le_set_privacy_mode_t), 0U);
    ExpectComplete("LE Set Data Related Address Changes", 0x207C, zeros,
                   sizeof(sdc_hci_cmd_le_set_data_related_address_changes_t),
                   0U);

    /*
     * Direct test mode past v1, plus the two commands a modern host expects to
     * find and would otherwise log as unknown.
     */
    ExpectComplete("Read RSSI", 0x1405, zeros,
                   sizeof(sdc_hci_cmd_sp_read_rssi_t),
                   sizeof(sdc_hci_cmd_sp_read_rssi_return_t));
    ExpectComplete("LE Set Host Feature", 0x2074, zeros,
                   sizeof(sdc_hci_cmd_le_set_host_feature_t), 0U);
    ExpectComplete("LE Receiver Test v2", 0x2033, zeros,
                   sizeof(sdc_hci_cmd_le_receiver_test_v2_t), 0U);
    ExpectComplete("LE Transmitter Test v2", 0x2034, zeros,
                   sizeof(sdc_hci_cmd_le_transmitter_test_v2_t), 0U);
    ExpectComplete("VS Transmitter Carrier Test", 0xFD23, zeros,
                   sizeof(sdc_hci_cmd_vs_transmitter_carrier_test_t), 0U);

    {
        /*
         * v3 carries an antenna switching pattern counted in bytes. Zero of
         * them is the normal request on a part with no direction finding, and
         * is the fixed part on its own.
         */
        const size_t head3 =
            offsetof(sdc_hci_cmd_le_receiver_test_v3_t, antenna_ids);

        ExpectComplete("LE Receiver Test v3, no pattern", 0x204F, zeros, head3,
                       0U);
        ExpectComplete("LE Transmitter Test v3, no pattern", 0x2050, zeros,
                       head3, 0U);

        /* Two identifiers declared and two supplied. */
        const uint8_t pattern[] = {0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
                                   0x00U, 0x02U, 0x01U, 0x02U};
        ExpectComplete("LE Receiver Test v3, two antennas", 0x204F, pattern,
                       sizeof(pattern), 0U);

        /* Two declared, one supplied. */
        const uint8_t shortPattern[] = {0x00U, 0x01U, 0x00U, 0x00U,
                                        0x00U, 0x00U, 0x02U, 0x01U};
        ExpectRejected("LE Receiver Test v3, count lies", 0x204F, shortPattern,
                       sizeof(shortPattern), 0x12);

        /*
         * v4 puts one octet of transmit power after the pattern, so the same
         * body that is exact for v3 is one short for v4. That is the whole
         * reason it does not share the macro, and refusing it here is what
         * proves the extra octet is really required.
         */
        ExpectRejected("LE Transmitter Test v4, no power octet", 0x207B, zeros,
                       offsetof(sdc_hci_cmd_le_transmitter_test_v4_t,
                                antenna_ids_and_remaining_parameters),
                       0x12);
        ExpectComplete("LE Transmitter Test v4, no pattern", 0x207B, zeros,
                       offsetof(sdc_hci_cmd_le_transmitter_test_v4_t,
                                antenna_ids_and_remaining_parameters) + 1U,
                       0U);

        /* Two identifiers, then the power octet. */
        const uint8_t v4[] = {0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
                              0x00U, 0x02U, 0x01U, 0x02U, 0x00U};
        ExpectComplete("LE Transmitter Test v4, two antennas", 0x207B, v4,
                       sizeof(v4), 0U);

        /* And the same body without the power octet is refused. */
        ExpectRejected("LE Transmitter Test v4, pattern but no power", 0x207B,
                       v4, sizeof(v4) - 1U, 0x12);
    }

    /*
     * One short of the keys is a host that has mistaken the layout, and the
     * controller has no way to tell which sixteen octets it meant. Refused
     * rather than passed to SDC with whatever followed in the buffer.
     */
    ExpectRejected("LE Add Device To Resolving List, one short", 0x2027, zeros,
                   sizeof(sdc_hci_cmd_le_add_device_to_resolving_list_t) - 1U,
                   0x12);

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
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_CB_SET_CONTROLLER_TO_HOST_FLOW_CONTROL,
                hci_set_controller_to_host_flow_control),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_CB_HOST_BUFFER_SIZE,
                         hci_host_buffer_size),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_CB_HOST_NUMBER_OF_COMPLETED_PACKETS,
                         hci_host_number_of_completed_packets),
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
            {HCI_COUNTERS_OPCODE, NULL, "vs_read_counters"},
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
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_RESOLVING_LIST,
                         hci_le_add_device_to_resolving_list),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_RESOLVING_LIST,
                hci_le_remove_device_from_resolving_list),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CLEAR_RESOLVING_LIST,
                         hci_le_clear_resolving_list),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_RESOLVING_LIST_SIZE,
                         hci_le_read_resolving_list_size),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_ADDRESS_RESOLUTION_ENABLE,
                         hci_le_set_address_resolution_enable),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_RESOLVABLE_PRIVATE_ADDRESS_TIMEOUT,
                hci_le_set_resolvable_private_address_timeout),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PRIVACY_MODE,
                         hci_le_set_privacy_mode),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_DATA_RELATED_ADDRESS_CHANGES,
                hci_le_set_data_related_address_changes),

            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_SP_READ_RSSI, hci_read_rssi),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_CB_READ_TRANSMIT_POWER_LEVEL,
                         hci_read_transmit_power_level),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_RF_PATH_COMPENSATION,
                         hci_le_read_rf_path_compensation),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_WRITE_RF_PATH_COMPENSATION,
                         hci_le_write_rf_path_compensation),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_ENHANCED_READ_TRANSMIT_POWER_LEVEL,
                hci_le_enhanced_read_transmit_power_level),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_READ_REMOTE_TRANSMIT_POWER_LEVEL,
                hci_le_read_remote_transmit_power_level),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PATH_LOSS_REPORTING_PARAMS,
                         hci_le_set_path_loss_reporting_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PATH_LOSS_REPORTING_ENABLE,
                         hci_le_set_path_loss_reporting_enable),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_TRANSMIT_POWER_REPORTING_ENABLE,
                hci_le_set_transmit_power_reporting_enable),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION,
                hci_le_set_host_channel_classification),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_REQUEST_PEER_SCA,
                         hci_le_request_peer_sca),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_SUBRATE,
                         hci_le_set_default_subrate_command),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SUBRATE_REQUEST,
                         hci_le_subrate_request_command),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_ALL_REMOTE_FEATURES,
                         hci_le_read_all_remote_features),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS,
                         hci_le_set_periodic_advertising_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_DATA,
                         hci_le_set_periodic_advertising_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_ENABLE,
                         hci_le_set_periodic_advertising_enable),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC,
                         hci_le_periodic_advertising_create_sync),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_CREATE_SYNC_CANCEL,
                hci_le_periodic_advertising_create_sync_cancel),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_TERMINATE_SYNC,
                         hci_le_periodic_advertising_terminate_sync),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_ADD_DEVICE_TO_PERIODIC_ADV_LIST,
                         hci_le_add_device_to_periodic_advertiser_list),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_REMOVE_DEVICE_FROM_PERIODIC_ADV_LIST,
                hci_le_remove_device_from_periodic_advertiser_list),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_CLEAR_PERIODIC_ADV_LIST,
                         hci_le_clear_periodic_advertiser_list),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_READ_PERIODIC_ADV_LIST_SIZE,
                         hci_le_read_periodic_advertiser_list_size),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RECEIVE_ENABLE,
                hci_le_set_periodic_advertising_receive_enable),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_SYNC_TRANSFER,
                         hci_le_periodic_advertising_sync_transfer),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_PERIODIC_ADV_SET_INFO_TRANSFER,
                         hci_le_periodic_advertising_set_info_transfer),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SYNC_TRANSFER_PARAMS,
                hci_le_set_periodic_advertising_sync_transfer_parameters),
            BITMAP_ENTRY(
                SDC_HCI_OPCODE_CMD_LE_SET_DEFAULT_PERIODIC_ADV_SYNC_TRANSFER_PARAMS,
                hci_le_set_default_periodic_advertising_sync_transfer_parameters),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_PARAMS_V2,
                         hci_le_set_periodic_advertising_parameters_v2),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_SUBEVENT_DATA,
                         hci_le_set_periodic_advertising_subevent_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_ADV_RESPONSE_DATA,
                         hci_le_set_periodic_advertising_response_data),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_PERIODIC_SYNC_SUBEVENT,
                         hci_le_set_periodic_sync_subevent),
            /* Nordic vendor, so Vol 4 Part E 6.27 assigns no bit. */
            {SDC_HCI_OPCODE_CMD_VS_SET_ADV_RANDOMNESS, NULL,
             "vs_set_adv_randomness"},
            {SDC_HCI_OPCODE_CMD_VS_LLPM_MODE_SET, NULL, "vs_llpm_mode_set"},
            {SDC_HCI_OPCODE_CMD_VS_CONN_UPDATE, NULL, "vs_conn_update"},
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_SET_HOST_FEATURE,
                         hci_le_set_host_feature),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V2,
                         hci_le_receiver_test_v2),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V2,
                         hci_le_transmitter_test_v2),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_RECEIVER_TEST_V3,
                         hci_le_receiver_test_v3),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V3,
                         hci_le_transmitter_test_v3),
            BITMAP_ENTRY(SDC_HCI_OPCODE_CMD_LE_TRANSMITTER_TEST_V4,
                         hci_le_transmitter_test_v4),
            /* Vendor specific, so Vol 4 Part E 6.27 assigns it no bit. */
            {SDC_HCI_OPCODE_CMD_VS_TRANSMITTER_CARRIER_TEST, NULL,
             "vs_transmitter_carrier_test"},
            /*
             * Same, and these have a bitmap of their own instead, the one
             * Read Supported Commands answers with. It is checked above.
             */
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_VERSION_INFO, NULL,
             "vs_zephyr_read_version_info"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_SUPPORTED_COMMANDS, NULL,
             "vs_zephyr_read_supported_commands"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_WRITE_BD_ADDR, NULL,
             "vs_zephyr_write_bd_addr"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_CHIP_TEMP, NULL,
             "vs_zephyr_read_chip_temp"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_WRITE_TX_POWER, NULL,
             "vs_zephyr_write_tx_power"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_TX_POWER, NULL,
             "vs_zephyr_read_tx_power"},
            {SDC_HCI_OPCODE_CMD_VS_ZEPHYR_READ_KEY_HIERARCHY_ROOTS, NULL,
             "vs_zephyr_read_key_hierarchy_roots"},
            /*
             * Nordic vendor rather than Zephyr vendor, so they appear in
             * neither bitmap. A host finds them by opcode or not at all.
             */
            {SDC_HCI_OPCODE_CMD_VS_QOS_CONN_EVENT_REPORT_ENABLE, NULL,
             "vs_qos_conn_event_report_enable"},
            {SDC_HCI_OPCODE_CMD_VS_QOS_CHANNEL_SURVEY_ENABLE, NULL,
             "vs_qos_channel_survey_enable"},
            {SDC_HCI_OPCODE_CMD_VS_READ_AVERAGE_RSSI, NULL,
             "vs_read_average_rssi"},
            {SDC_HCI_OPCODE_CMD_VS_GET_NEXT_CONN_EVENT_COUNTER, NULL,
             "vs_get_next_conn_event_counter"},
            {SDC_HCI_OPCODE_CMD_VS_CONN_ANCHOR_POINT_UPDATE_EVENT_REPORT_ENABLE,
             NULL, "vs_conn_anchor_point_update_event_report_enable"},
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
        static uint32_t before[HCI_COUNTERS_COUNT];
        static uint32_t after[HCI_COUNTERS_COUNT];

        g_SdcStub.NextStatus = 0x00;
        Exchange(HCI_COUNTERS_OPCODE, zeros, 0U);
        ReadCounters(before);

        /* No entry for this opcode, so it lands on UnknownCommandCount. */
        Exchange(0x0CFF, zeros, 0U);
        /* A known opcode with a length its entry does not accept. */
        Exchange(0x2017, zeros, 3U);

        g_SdcStub.NextStatus = 0x00;
        Exchange(HCI_COUNTERS_OPCODE, zeros, 0U);
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

        /* Nothing here sends ACL, so none of its counters may have moved. */
        assert(after[COUNTER_ACL_PUT_ERROR] == before[COUNTER_ACL_PUT_ERROR]);
        assert(after[COUNTER_PUT_RETRY] == before[COUNTER_PUT_RETRY]);
        assert(after[COUNTER_ACL_PUT] == before[COUNTER_ACL_PUT]);

        printf("[ok] %-38s unknown +1, bad length +1, commands +3\n",
               "counters follow what the layer refused");
    }
#endif

    printf("All SDC dispatch tests passed.\n");
    return 0;
}
