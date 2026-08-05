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

typedef struct {
    HciCmdDispatch_t Commands;
    HciSdcOps_t Ops;
    HciControllerOps_t ControllerOps;

    /*
     * Two sources share one outgoing packet slot: command events from the
     * dispatch table, and everything the controller queues. Set after a
     * command event goes out so the controller queue is asked first on the
     * next call, which stops a busy command stream from starving it.
     */
    bool CommandEventLast;

    uint32_t AclPutErrorCount;
    uint32_t IsoPutErrorCount;
    uint32_t PutRetryCount;
    uint32_t GetErrorCount;
    uint32_t InvalidOutputTypeCount;
    uint32_t InvalidOutputLengthCount;
    uint32_t CommandEventDeferredCount;
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
