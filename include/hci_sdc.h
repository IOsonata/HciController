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
} HciSdc_t;

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
