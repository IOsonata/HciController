/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_TAKTOS_H
#define HCI_TAKTOS_H

#include <stdbool.h>
#include <stdint.h>

#include "TaktOSSem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_TAKTOS_EVENT_HOST  (1UL << 0)
#define HCI_TAKTOS_EVENT_MPSL  (1UL << 1)
#define HCI_TAKTOS_EVENT_SDC   (1UL << 2)
#define HCI_TAKTOS_EVENT_STOP  (1UL << 31)

typedef bool (*HciTaktOsStart_t)(void *pContext);
typedef void (*HciTaktOsProcess_t)(void *pContext);
typedef void (*HciTaktOsFault_t)(void *pContext, int Error);

typedef struct {
    HciTaktOsStart_t Start;
    HciTaktOsProcess_t ProcessMpsl;
    HciTaktOsFault_t Fault;
    void *pContext;
} HciTaktOsOps_t;

typedef struct {
    HciTaktOsStart_t Start;
    HciTaktOsProcess_t Process;
    void *pContext;

    /*
     * Milliseconds between forced Process calls when no wake arrives. Zero
     * waits for a wake only. A USB host stack has to be pumped even if a wake
     * is lost, otherwise enumeration stalls with no way to recover.
     */
    uint32_t PollIntervalMs;
} HciTaktOsHostOps_t;

typedef struct {
    TaktOSSem_t WakeSem;
    HciTaktOsOps_t Ops;
    HciTaktOsHostOps_t HostOps;

    volatile uint32_t PendingEvents;
    volatile bool Started;
    volatile bool Running;
    volatile bool StopRequested;

    uint32_t WakeCount;
    uint32_t MpslProcessCount;
    uint32_t LoopCount;
    uint32_t EmptyWakeCount;
    uint32_t SemaphoreFullCount;
    uint32_t StartErrorCount;
    uint32_t PollWakeCount;
} HciTaktOs_t;

bool HciTaktOsInit(HciTaktOs_t *pRuntime,
                   const HciTaktOsOps_t *pOps,
                   const HciTaktOsHostOps_t *pHostOps);

void HciTaktOsWake(HciTaktOs_t *pRuntime, uint32_t Events);
void HciTaktOsStop(HciTaktOs_t *pRuntime);
void HciTaktOsThread(void *pContext);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TAKTOS_H */
