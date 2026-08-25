/**-------------------------------------------------------------------------
@file	hci_taktos.h

@brief	TaktOS runtime integration for HciController.

		Defines controller wake events, Host and MPSL processing hooks, runtime
		state, thread lifecycle control, and stop synchronization.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

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
    /* Given by the runtime thread as it leaves, so a stop can wait for it. */
    TaktOSSem_t StoppedSem;
    HciTaktOsOps_t Ops;
    HciTaktOsHostOps_t HostOps;

    volatile uint32_t PendingEvents;
    volatile bool Started;
    volatile bool Running;
    volatile bool StopRequested;
    bool HostStarted;

    /*
     * Armed is set before the kernel thread is created. Live is set on entry
     * to HciTaktOsThread. They are deliberately separate: between thread
     * creation and the first instruction of the thread body there is a real
     * task that a stop must wait for even though ThreadLive is still false.
     */
    volatile bool ThreadArmed;
    volatile bool ThreadLive;

    uint32_t WakeCount;
    uint32_t MpslProcessCount;
    uint32_t LoopCount;
    uint32_t EmptyWakeCount;
    uint32_t SemaphoreFullCount;

    /*
     * Wakes that asked for an event already pending, so the semaphore was not
     * touched. Under load this is most of them.
     */
    uint32_t WakeFoldCount;
    uint32_t StartErrorCount;
    uint32_t HostRetryCount;
    uint32_t PollWakeCount;
} HciTaktOs_t;

bool HciTaktOsInit(HciTaktOs_t *pRuntime,
                   const HciTaktOsOps_t *pOps,
                   const HciTaktOsHostOps_t *pHostOps);

/*
 * Arm before creating the kernel thread. If creation itself fails, disarm it.
 * This closes the interval where a stop could otherwise mistake a scheduled
 * but not-yet-entered thread for no thread at all and tear down SDC/MPSL below
 * it.
 */
void HciTaktOsThreadArm(HciTaktOs_t *pRuntime);
void HciTaktOsThreadDisarm(HciTaktOs_t *pRuntime);

void HciTaktOsWake(HciTaktOs_t *pRuntime, uint32_t Events);
void HciTaktOsStop(HciTaktOs_t *pRuntime);

/*
 * Called only from the runtime thread when the host transport has become
 * unusable after it was started. The next service pass runs HostOps.Start
 * again instead of continuing to pump a failed transport.
 */
void HciTaktOsHostDown(HciTaktOs_t *pRuntime);

/*
 * Wait for the runtime thread to leave its loop after a stop. Returns false on
 * timeout, in which case the caller must not tear down anything the thread
 * uses. Milliseconds.
 */
bool HciTaktOsWaitStopped(HciTaktOs_t *pRuntime, uint32_t TimeoutMs);
void HciTaktOsThread(void *pContext);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TAKTOS_H */
