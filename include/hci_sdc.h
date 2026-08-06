/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_SDC_H
#define HCI_SDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_cmd_dispatch.h"
#include "hci_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_SDC_MSG_TYPE_NONE   0x00U
#define HCI_SDC_MSG_TYPE_ACL    0x02U
#define HCI_SDC_MSG_TYPE_EVENT  0x04U
#define HCI_SDC_MSG_TYPE_ISO    0x08U

#define HCI_SDC_RETRY_ERROR     (-11)

typedef int32_t (*HciSdcDataPut_t)(void *pContext, const uint8_t *pPacket);
typedef int32_t (*HciSdcGet_t)(void *pContext, uint8_t *pPacket, uint8_t *pType);
typedef void (*HciSdcProcess_t)(void *pContext);

typedef struct {
    HciSdcDataPut_t AclPut;
    HciSdcDataPut_t IsoPut;
    HciSdcGet_t Get;
    HciSdcProcess_t Process;
    void *pContext;
    int32_t RetryError;
} HciSdcOps_t;

/*
 * Connection handles that can owe a flow control credit at the same time. The
 * controller supports far fewer links than this.
 */
#define HCI_SDC_CREDIT_HANDLES 4U

/*
 * Largest ACL payload the controller will take, Vol 4 Part E 7.8.2. A host
 * that respects LE Read Buffer Size never exceeds it; one that does not must
 * be refused here rather than handed to the controller.
 */
/* Handle and flags, then Data_Total_Length. Vol 4 Part E 5.4.2. */
#define HCI_SDC_ACL_HEADER_SIZE 4U

#ifndef HCI_SDC_ACL_MAX_PAYLOAD
#define HCI_SDC_ACL_MAX_PAYLOAD 251U
#endif

/* Vol 4 Part E 7.7.19. */
#define HCI_SDC_EVENT_NUM_COMPLETED_PACKETS 0x13U

/* Vol 4 Part E 7.7.5. */
#define HCI_SDC_EVENT_DISCONNECTION_COMPLETE 0x05U

/*
 * Links whose outstanding ACL packets are tracked at once. This has to be at
 * least the number of connections the controller is configured for, because a
 * link the table has no room for is not counted, and what is not counted
 * cannot be enforced or handed back.
 */
#ifndef HCI_SDC_ACL_TRACK_HANDLES
#define HCI_SDC_ACL_TRACK_HANDLES 8U
#endif

/*
 * Hold the host to the buffer count the controller advertised in LE Read
 * Buffer Size.
 *
 * Measured on an nRF52840 with the SoftDevice Controller: send one packet more
 * than the advertised count and sdc_hci_data_put answers 0 for it, the packet
 * never goes out, and no Number Of Completed Packets event ever names it. The
 * packet and the host's buffer both vanish. Four buffers, five packets, four
 * transmitted, every error code zero.
 *
 * Vol 4 Part E 4.1.1 puts the obligation on the host not to exceed what it was
 * told, so the controller is within its rights. But losing the credit as well
 * as the packet means a host that slips once loses that buffer for the life of
 * the connection. Refusing here loses the same packet and hands the buffer
 * back, and counts it, which a host can at least see.
 *
 * Set to 0 to keep the counter and let the packets through to SDC.
 */
#ifndef HCI_SDC_ENFORCE_ACL_CREDITS
#define HCI_SDC_ENFORCE_ACL_CREDITS 1
#endif

typedef struct {
    HciCmdDispatch_t Commands;
    HciSdcOps_t Ops;
    HciControllerOps_t ControllerOps;

    /*
     * Two sources share one outgoing packet slot: command events from the
     * dispatch table, and everything the controller queues. Set after a
     * command event goes out, and cleared once the controller queue has been
     * asked. While set, a new command is refused, so a busy command stream
     * cannot starve the controller queue and no queued event can overtake the
     * response of the command that produced it.
     */
    bool CommandEventLast;

    /*
     * Host flow control credits owed back for ACL packets the controller
     * refused. The host spends a credit when it sends a packet and only gets
     * it back in a Number Of Completed Packets event, so a packet dropped
     * without one costs the host a buffer permanently, Vol 4 Part E 4.1.1.
     * One entry per connection handle, counts aggregated.
     */
    uint16_t CreditHandle[HCI_SDC_CREDIT_HANDLES];
    uint16_t CreditCount[HCI_SDC_CREDIT_HANDLES];
    uint8_t CreditEntries;
    uint32_t CreditOverflowCount;

    uint32_t AclPutErrorCount;
    uint32_t IsoPutErrorCount;
    uint32_t PutRetryCount;
    uint32_t GetErrorCount;
    uint32_t InvalidOutputTypeCount;
    uint32_t InvalidOutputLengthCount;
    uint32_t CommandDeferredCount;
    uint32_t AclOversizeCount;
    uint32_t IsoDropCount;

    /*
     * Packets the controller took. Every counter above this point records a
     * refusal, and a block of refusal counters that all read zero cannot be
     * told apart from a path nothing ever reached. These are what make silence
     * mean something.
     */
    uint32_t AclPutCount;
    uint32_t IsoPutCount;

    /*
     * What the host was told in LE Read Buffer Size, and how many packets it
     * has in flight against that. Zero means the host has not asked yet, and
     * nothing is enforced until it has, so the limit can never be a guess.
     * Enforcement also stands down for a link the table has no room for:
     * refusing traffic a host is entitled to send would be worse than the loss
     * this guards against.
     *
     * The budget is a total, not an allowance per link. Vol 4 Part E 4.1.1
     * gives the host one pool of buffers to spend across every connection it
     * has, and LE Read Buffer Size reports that one number. Testing each link
     * against it separately let N links hold N times what the controller owns,
     * which is the overrun this exists to refuse. It was invisible while the
     * controller was built for a single link, where the two counts are the
     * same number.
     *
     * So AclOutstandingTotal is what the limit is tested against, and the per
     * link counts stay because the bookkeeping needs them: Number Of Completed
     * Packets names a handle, and a disconnection takes that link's share of
     * the total with it. The invariant is that the total is the sum of the
     * entries, and every path that moves one moves the other.
     */
    uint16_t AclLimit;
    uint16_t AclTrackHandle[HCI_SDC_ACL_TRACK_HANDLES];
    uint16_t AclOutstanding[HCI_SDC_ACL_TRACK_HANDLES];
    uint16_t AclOutstandingTotal;
    uint8_t AclTrackEntries;
    uint32_t AclCreditOverrunCount;
    uint32_t AclTrackOverflowCount;
} HciSdc_t;

/*
 * The buffer count the controller answers with. The backend records what SDC
 * actually reported rather than what the build configured, so the two cannot
 * drift.
 */
void HciSdcSetAclLimit(HciSdc_t *pSdc, uint16_t Limit);

/*
 * Forget every link's in flight count and every credit still owed. HCI_Reset
 * drops all connections and reports none of them, Vol 4 Part E 7.3.2, so
 * nothing else clears this and a handle handed out again after a reset would
 * inherit a stale count.
 */
void HciSdcResetFlowControl(HciSdc_t *pSdc);

bool HciSdcInit(HciSdc_t *pSdc,
                const HciSdcOps_t *pOps,
                const HciCmdEntry_t *pCommands,
                size_t CommandCount,
                void *pCommandContext,
                uint8_t *pCommandEvent,
                size_t CommandEventCapacity);

const HciControllerOps_t *HciSdcGetControllerOps(HciSdc_t *pSdc);

#ifdef __cplusplus
}
#endif

#endif /* HCI_SDC_H */
