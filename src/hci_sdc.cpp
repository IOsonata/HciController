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
#include "hci_core_profile.h"

#include <stddef.h>
#include <string.h>

#include "sdc_hci_cmd_controller_baseband.h"
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
#include "sdc_hci_cmd_le.h"
#endif

#define HCI_SDC_COMPAT_OPCODE_READ_CONN_ACCEPT_TIMEOUT  0x0C15U
#define HCI_SDC_COMPAT_OPCODE_WRITE_CONN_ACCEPT_TIMEOUT 0x0C16U
#define HCI_SDC_OPCODE_LE_SET_PERIODIC_ADV_RESPONSE_DATA 0x2083U
#define HCI_SDC_PAW_RESPONSE_FIXED_PARAM_LEN             8U
#define HCI_SDC_PAW_RESPONSE_DATA_LEN_OFFSET             7U
#define HCI_SDC_PAW_RESPONSE_RETURN_LEN                  2U

#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
#define HCI_SDC_SUPP_OPCODE_LE_FRAME_SPACE_UPDATE                  0x209DU
#define HCI_SDC_SUPP_OPCODE_LE_CONNECTION_RATE_REQUEST              0x20A1U
#define HCI_SDC_SUPP_OPCODE_LE_SET_DEFAULT_RATE_PARAMETERS          0x20A2U
#define HCI_SDC_SUPP_OPCODE_LE_READ_MIN_SUPPORTED_CONN_INTERVAL     0x20A3U
#define HCI_SDC_MIN_INTERVAL_MAX_GROUPS                             41U
#endif

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
 * Supplemental Core HCI commands.
 * ------------------------------------------------------------------------- */

static HciCmdResult_t HciSdcCompatReadConnAcceptTimeout(void *,
                                                         const uint8_t *,
                                                         size_t,
                                                         uint8_t *pReturn,
                                                         size_t ReturnCapacity)
{
    sdc_hci_cmd_cb_read_conn_accept_timeout_return_t value;

    if (ReturnCapacity < sizeof(value))
    {
        HciCmdResult_t error = {
            HCI_STATUS_MEMORY_CAPACITY_EXCEEDED,
            HCI_CMD_RESPONSE_COMPLETE,
            0U,
        };
        return error;
    }

    const uint8_t status = sdc_hci_cmd_cb_read_conn_accept_timeout(&value);
    if (status == HCI_STATUS_SUCCESS)
    {
        memcpy(pReturn, &value, sizeof(value));
    }

    HciCmdResult_t result = {
        status,
        HCI_CMD_RESPONSE_COMPLETE,
        status == HCI_STATUS_SUCCESS ? sizeof(value) : 0U,
    };
    return result;
}

static HciCmdResult_t HciSdcCompatWriteConnAcceptTimeout(void *,
                                                          const uint8_t *pParams,
                                                          size_t,
                                                          uint8_t *,
                                                          size_t)
{
    sdc_hci_cmd_cb_write_conn_accept_timeout_t value;
    memcpy(&value, pParams, sizeof(value));

    HciCmdResult_t result = {
        sdc_hci_cmd_cb_write_conn_accept_timeout(&value),
        HCI_CMD_RESPONSE_COMPLETE,
        0U,
    };
    return result;
}

static HciCmdResult_t HciSdcCompatReadSupportedStates(void *,
                                                       const uint8_t *,
                                                       size_t,
                                                       uint8_t *pReturn,
                                                       size_t ReturnCapacity)
{
    /*
     * Core Vol 4 Part E 7.8.27. States 0 to 41 set, 42 to 63 reserved and
     * zero. This is a product-level concurrency claim rather than a vendor
     * return value. The multirole profile reserves multiple Central and
     * Peripheral links, advertising and scanning resources, and explicitly
     * enables parallel scanning and initiating. tests/command_coverage.py
     * verifies those prerequisites independently, and the hardware profile
     * test pins this exact value on the wire.
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

#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
static HciCmdResult_t HciSdcLeFrameSpaceUpdate(void *,
                                                const uint8_t *pParams,
                                                size_t,
                                                uint8_t *,
                                                size_t)
{
    sdc_hci_cmd_le_frame_space_update_t params;
    memcpy(&params, pParams, sizeof(params));
    HciCmdResult_t result = {
        sdc_hci_cmd_le_frame_space_update(&params),
        HCI_CMD_RESPONSE_STATUS,
        0U,
    };
    return result;
}

static HciCmdResult_t HciSdcLeConnectionRateRequest(void *,
                                                     const uint8_t *pParams,
                                                     size_t,
                                                     uint8_t *,
                                                     size_t)
{
    sdc_hci_cmd_le_conn_rate_request_t params;
    memcpy(&params, pParams, sizeof(params));
    HciCmdResult_t result = {
        sdc_hci_cmd_le_conn_rate_request(&params),
        HCI_CMD_RESPONSE_STATUS,
        0U,
    };
    return result;
}

static HciCmdResult_t HciSdcLeSetDefaultRateParameters(void *,
                                                        const uint8_t *pParams,
                                                        size_t,
                                                        uint8_t *,
                                                        size_t)
{
    sdc_hci_cmd_le_set_default_rate_params_t params;
    memcpy(&params, pParams, sizeof(params));
    HciCmdResult_t result = {
        sdc_hci_cmd_le_set_default_rate_params(&params),
        HCI_CMD_RESPONSE_COMPLETE,
        0U,
    };
    return result;
}

static HciCmdResult_t HciSdcLeReadMinSupportedConnInterval(
    void *,
    const uint8_t *,
    size_t,
    uint8_t *pReturn,
    size_t ReturnCapacity)
{
    static const size_t fixed =
        offsetof(sdc_hci_cmd_le_read_min_supported_conn_interval_return_t,
                 groups);
    static const size_t group =
        sizeof(sdc_hci_le_read_min_supported_conn_interval_group_t);
    static const size_t maximum =
        fixed + HCI_SDC_MIN_INTERVAL_MAX_GROUPS * group;

    static_assert(
        offsetof(sdc_hci_cmd_le_read_min_supported_conn_interval_return_t,
                 groups) == 2U,
        "LE Read Minimum Supported Connection Interval header is not 2 octets");
    static_assert(sizeof(sdc_hci_le_read_min_supported_conn_interval_group_t) ==
                      6U,
                  "LE minimum interval group is not 6 octets");

    if (ReturnCapacity < maximum)
    {
        HciCmdResult_t error = {
            HCI_STATUS_MEMORY_CAPACITY_EXCEEDED,
            HCI_CMD_RESPONSE_COMPLETE,
            0U,
        };
        return error;
    }

    sdc_hci_cmd_le_read_min_supported_conn_interval_return_t *result =
        reinterpret_cast<
            sdc_hci_cmd_le_read_min_supported_conn_interval_return_t *>(
                pReturn);
    const uint8_t status =
        sdc_hci_cmd_le_read_min_supported_conn_interval(result);

    if (status != HCI_STATUS_SUCCESS)
    {
        HciCmdResult_t error = {status, HCI_CMD_RESPONSE_COMPLETE, 0U};
        return error;
    }

    if (result->num_groups > HCI_SDC_MIN_INTERVAL_MAX_GROUPS)
    {
        HciCmdResult_t error = {
            HCI_STATUS_MEMORY_CAPACITY_EXCEEDED,
            HCI_CMD_RESPONSE_COMPLETE,
            0U,
        };
        return error;
    }

    HciCmdResult_t ok = {
        status,
        HCI_CMD_RESPONSE_COMPLETE,
        fixed + (size_t)result->num_groups * group,
    };
    return ok;
}
#endif

static const HciCmdEntry_t s_HciSdcCompatCommands[] = {
    {HCI_SDC_COMPAT_OPCODE_READ_CONN_ACCEPT_TIMEOUT,
     0U,
     2U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcCompatReadConnAcceptTimeout},
    {HCI_SDC_COMPAT_OPCODE_WRITE_CONN_ACCEPT_TIMEOUT,
     2U,
     0U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcCompatWriteConnAcceptTimeout},
    {HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES,
     0U,
     8U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcCompatReadSupportedStates},
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
    {HCI_SDC_SUPP_OPCODE_LE_FRAME_SPACE_UPDATE,
     (uint16_t)sizeof(sdc_hci_cmd_le_frame_space_update_t),
     0U,
     HCI_CMD_RESPONSE_STATUS,
     HciSdcLeFrameSpaceUpdate},
    {HCI_SDC_SUPP_OPCODE_LE_CONNECTION_RATE_REQUEST,
     (uint16_t)sizeof(sdc_hci_cmd_le_conn_rate_request_t),
     0U,
     HCI_CMD_RESPONSE_STATUS,
     HciSdcLeConnectionRateRequest},
    {HCI_SDC_SUPP_OPCODE_LE_SET_DEFAULT_RATE_PARAMETERS,
     (uint16_t)sizeof(sdc_hci_cmd_le_set_default_rate_params_t),
     0U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcLeSetDefaultRateParameters},
    {HCI_SDC_SUPP_OPCODE_LE_READ_MIN_SUPPORTED_CONN_INTERVAL,
     0U,
     0U,
     HCI_CMD_RESPONSE_COMPLETE,
     HciSdcLeReadMinSupportedConnInterval},
#endif
};

static bool HciSdcSupplementalOpcode(uint16_t Opcode)
{
    switch (Opcode)
    {
        case HCI_SDC_COMPAT_OPCODE_READ_CONN_ACCEPT_TIMEOUT:
        case HCI_SDC_COMPAT_OPCODE_WRITE_CONN_ACCEPT_TIMEOUT:
        case HCI_SDC_COMPAT_OPCODE_LE_READ_SUPPORTED_STATES:
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
        case HCI_SDC_SUPP_OPCODE_LE_FRAME_SPACE_UPDATE:
        case HCI_SDC_SUPP_OPCODE_LE_CONNECTION_RATE_REQUEST:
        case HCI_SDC_SUPP_OPCODE_LE_SET_DEFAULT_RATE_PARAMETERS:
        case HCI_SDC_SUPP_OPCODE_LE_READ_MIN_SUPPORTED_CONN_INTERVAL:
#endif
            return true;

        default:
            return false;
    }
}

/*
 * Read Local Supported Commands is supplied by nrfxlib, but supplemental
 * commands must describe the complete HCI controller, not merely the vendor
 * table. The compatibility rows add the timeout pair and LE Read Supported
 * States. Core 6.2 adds Frame Space Update and the three Shorter Connection
 * Intervals commands in octet 48.
 */
static void HciSdcPatchSupportedCommands(uint8_t *pEvent, size_t EventLen)
{
    const uint16_t readSupportedCommands = 0x1002U;
    const size_t timeoutByte = HCI_COMMAND_COMPLETE_BASE_SIZE + 7U;
    const size_t statesByte = HCI_COMMAND_COMPLETE_BASE_SIZE + 28U;

    if (EventLen <= statesByte || pEvent[0] != HCI_EVENT_COMMAND_COMPLETE ||
        HciSdcReadLe16(&pEvent[3]) != readSupportedCommands ||
        pEvent[5] != HCI_STATUS_SUCCESS)
    {
        return;
    }

    pEvent[timeoutByte] |= (uint8_t)((1U << 2) | (1U << 3));
    pEvent[statesByte] |= (1U << 3);

#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
    const size_t core62Byte = HCI_COMMAND_COMPLETE_BASE_SIZE + 48U;
    if (EventLen > core62Byte)
    {
        pEvent[core62Byte] |=
            (uint8_t)((1U << 1) | (1U << 5) | (1U << 6) | (1U << 7));
    }
#endif
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

/*
 * Remember a flow control credit owed back for an ACL packet the controller
 * would not take. The connection handle is the low 12 bits of the first two
 * octets of the ACL header, Vol 4 Part E 5.4.2.
 */
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
        /*
         * More links owing credits than the table holds. Dropping the credit
         * costs the host one buffer on that link, which is the same failure
         * this function exists to avoid, so it is counted rather than hidden.
         */
        pSdc->CreditOverflowCount++;
        return;
    }

    pSdc->CreditHandle[pSdc->CreditEntries] = handle;
    pSdc->CreditCount[pSdc->CreditEntries] = 1U;
    pSdc->CreditEntries++;
}

/*
 * Build the Number Of Completed Packets event for everything owed, Vol 4 Part
 * E 7.7.19. One event covers every handle, so the table empties in one go.
 */
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

    /*
     * Vol 4 Part E 7.3.2 puts the link layer in standby and drops every
     * connection, and no event is reported for them, so no Disconnection
     * Complete arrives to clear this the usual way. Left alone, a handle the
     * controller hands out again after a reset inherits the old in flight
     * count and is throttled, or stalled outright once it reads as full, for
     * the life of the board.
     *
     * The credits owed go with it. Emitting them after a reset would name
     * connection handles the same section has already made meaningless.
     */
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

/* Index of the link, or -1 when it is not tracked and cannot be. */
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

/*
 * Off by default, and HCI_SDC_ENFORCE_ACL_CREDITS in the header says why: the
 * link lifetime this depends on comes from Disconnection Complete on the event
 * stream, which a Host is allowed to mask. What follows is what the guard does
 * when a build turns it on.
 *
 * True when the host already has as many packets in flight, across every link
 * together, as it was told it could. Unknown limit answers false, and so does
 * a link the table cannot hold, because a packet that will not be counted must
 * not be refused either: this refuses only what it can prove is over.
 *
 * The slot is taken here rather than after the decision so that the answer and
 * the counting agree about which links are tracked.
 */
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

    /* The link takes its share of the total with it. */
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

/*
 * Watches what goes out to the host for the two events that change how many
 * packets a link has in flight. Number Of Completed Packets frees them, and a
 * disconnection discards whatever the controller still held for that link
 * without ever counting those back, so the entry goes with the link.
 *
 * The counting runs whether or not the guard above is compiled in: the figures
 * are read back through the vendor counter block, and a disabled guard must
 * still not leave stale ones behind.
 */
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

            /*
             * Take off what the link actually had, not what the event claims,
             * so a count larger than anything outstanding cannot drive the
             * total below the sum of the other links.
             */
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
        /*
         * Only a successful one. Vol 4 Part E 7.7.5 gives the status octet the
         * meaning that a non zero value is a disconnection that did not
         * happen, so the handle is still live and what is in flight on it is
         * still in flight. Forgetting it there hands the host a fresh full
         * allowance on top of the packets the controller has not returned yet,
         * which is the overrun this tracking exists to refuse.
         */
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

/*
 * LE Set Periodic Advertising Response Data is variable length. Its direct SDC
 * entry point is reached only after the fixed eight-octet head is present and
 * Response_Data_Length agrees exactly with the bytes that follow. Mirror that
 * small structural check here so HandlerCallCount can distinguish an SDC call
 * from a guard/local rejection without teaching the generic dispatcher about
 * this opcode.
 */
static bool HciSdcDelayedCommandCandidate(const HciCmdDispatch_t *pDispatch,
                                          const uint8_t *pPacket,
                                          size_t PacketLen)
{
    if (pDispatch == NULL || pPacket == NULL ||
        PacketLen < HCI_DISPATCH_COMMAND_HEADER_SIZE)
    {
        return false;
    }

    const uint16_t opcode = HciSdcReadLe16(pPacket);
    const size_t paramLen = pPacket[2];
    if (opcode != HCI_SDC_OPCODE_LE_SET_PERIODIC_ADV_RESPONSE_DATA ||
        PacketLen != HCI_DISPATCH_COMMAND_HEADER_SIZE + paramLen ||
        paramLen < HCI_SDC_PAW_RESPONSE_FIXED_PARAM_LEN ||
        pDispatch->EventCapacity <
            HCI_COMMAND_COMPLETE_BASE_SIZE + HCI_SDC_PAW_RESPONSE_RETURN_LEN)
    {
        return false;
    }

    const uint8_t dataLen =
        pPacket[HCI_DISPATCH_COMMAND_HEADER_SIZE +
                HCI_SDC_PAW_RESPONSE_DATA_LEN_OFFSET];
    return paramLen - HCI_SDC_PAW_RESPONSE_FIXED_PARAM_LEN ==
           (size_t)dataLen;
}

/*
 * SDC raises the real Command Complete for the command above later through
 * sdc_hci_get. Once the structurally valid command actually entered its
 * handler, suppress the dispatcher's temporary completion and wait for that
 * controller event. Unknown HCI Command is the exception documented by Nordic:
 * an SDC build that does not support the command has no delayed event coming.
 */
static bool HciSdcDelayCommandComplete(HciSdc_t *pSdc,
                                       uint16_t Opcode,
                                       uint8_t RspMarkLenBefore)
{
    HciCmdDispatch_t *pDispatch = &pSdc->Commands;

    if (Opcode != HCI_SDC_OPCODE_LE_SET_PERIODIC_ADV_RESPONSE_DATA ||
        !pDispatch->EventPending ||
        pDispatch->EventLen < HCI_COMMAND_COMPLETE_BASE_SIZE ||
        pDispatch->pEvent[0] != HCI_EVENT_COMMAND_COMPLETE ||
        HciSdcReadLe16(&pDispatch->pEvent[3]) != Opcode ||
        pDispatch->pEvent[5] == HCI_STATUS_UNKNOWN_HCI_COMMAND)
    {
        return false;
    }

    /* The local event never reaches the Host, so it is not a response mark. */
    pDispatch->RspMarkLen = RspMarkLenBefore;
    pDispatch->EventLen = 0U;
    pDispatch->EventPending = false;
    pSdc->DelayedCommandPending = true;
    pSdc->DelayedCommandOpcode = Opcode;
    pSdc->PawrSyntheticSuppressedCount++;
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
        {
            /*
             * Hold the next command until the controller queue has had the
             * outgoing slot. Without this a host that keeps a command in
             * flight, which it is entitled to do because every Command
             * Complete hands back a credit, produces a fresh command event on
             * every pass and sdc_hci_get is never reached. Refusing here is
             * ordinary backpressure: the parser keeps the packet and offers it
             * again next pass.
             *
             * There are two command dispatchers, so the pending check spans
             * both. Otherwise a vendor command could occupy one event slot
             * while a supplemental command occupies the other, granting a
             * second command credit the controller never intended to grant.
             *
             * A delayed SDC completion is the same command-credit condition,
             * except there is no dispatcher event to expose it. Keep the next
             * command at the parser until that real completion is forwarded.
             */
            if (pSdc->DelayedCommandPending ||
                pSdc->CommandEventLast ||
                HciCmdDispatchEventPending(&pSdc->Commands) ||
                HciCmdDispatchEventPending(&pSdc->CompatCommands))
            {
                pSdc->CommandDeferredCount++;
                return false;
            }

            if (PacketLen >= HCI_DISPATCH_COMMAND_HEADER_SIZE)
            {
                const uint16_t opcode = HciSdcReadLe16(pPacket);
                if (HciSdcSupplementalOpcode(opcode))
                {
                    return HciCmdDispatchPut(&pSdc->CompatCommands,
                                             pPacket, PacketLen);
                }

                const bool delayedCandidate =
                    HciSdcDelayedCommandCandidate(&pSdc->Commands,
                                                  pPacket, PacketLen);
                if (delayedCandidate)
                {
                    pSdc->PawrDelayedCandidateCount++;
                }

                const uint32_t handlerCallsBefore =
                    pSdc->Commands.HandlerCallCount;
                const uint8_t rspMarkLenBefore = pSdc->Commands.RspMarkLen;

                const bool accepted =
                    HciCmdDispatchPut(&pSdc->Commands, pPacket, PacketLen);
                if (accepted && delayedCandidate &&
                    pSdc->Commands.HandlerCallCount != handlerCallsBefore)
                {
                    pSdc->PawrDelayedHandlerCallCount++;
                    (void)HciSdcDelayCommandComplete(pSdc, opcode,
                                                     rspMarkLenBefore);
                }
                return accepted;
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

    /*
     * The controller queue has had its turn, even when it was empty or
     * returned an error. A new command and a new synthetic credit event may
     * now use their respective turns. A delayed command is intentionally not
     * released here: an empty poll is not its Command Complete.
     */
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
        /*
         * Only the real Command Complete for the outstanding opcode returns
         * the Host's command credit. LE Meta events, ACL completions, and any
         * other controller traffic are forwarded without disturbing it.
         */
        if (pSdc->DelayedCommandPending &&
            *pPacketLen >= HCI_COMMAND_COMPLETE_BASE_SIZE &&
            pPacket[0] == HCI_EVENT_COMMAND_COMPLETE &&
            HciSdcReadLe16(&pPacket[3]) == pSdc->DelayedCommandOpcode)
        {
            pSdc->PawrSdcCompleteCount++;
            pSdc->DelayedCommandPending = false;
            pSdc->DelayedCommandOpcode = 0U;
        }

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
