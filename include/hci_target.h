/**-------------------------------------------------------------------------
@file	hci_target.h

@brief	Portable HciController target lifecycle interface.

		Defines part-specific initialization, UART diagnostics, runtime
		hooks, shutdown, SDC memory reporting, and target error access.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_TARGET_H
#define HCI_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Target-specific lifecycle and diagnostic operations.
 * Hardware initialization, MPSL integration, and interrupt ownership belong
 * to the target implementation. SDC feature configuration is shared.
 * Each target implementation owns the state referenced by HciTarget_t.
 */
typedef struct
{
    bool (*Init)(void *pContext,
                 HciTaktOs_t *pRuntime,
                 uint8_t *pSdcMem,
                 size_t SdcMemCapacity);

    void (*GetTaktOsOps)(void *pContext, HciTaktOsOps_t *pOps);

    /*
     * Optional UART host-link diagnostics: peripheral state, pin mapping,
     * error status, and hardware flow-control signals.
     */
    void (*UartTrace)(const void *pContext, uint8_t DevNo);

    void (*Stop)(void *pContext);

    /* Report required and allocated SDC memory in bytes when available. */
    void (*GetSdcMem)(const void *pContext,
                      uint32_t *pRequired,
                      uint32_t *pCapacity);

    /* Return the most recent target error. */
    int32_t (*LastError)(const void *pContext);
} HciTargetOps_t;

typedef struct
{
    const HciTargetOps_t *pOps;
    void *pContext;
} HciTarget_t;

static inline bool HciTargetValid(const HciTarget_t *pTarget)
{
    return pTarget != NULL && pTarget->pOps != NULL &&
           pTarget->pOps->Init != NULL &&
           pTarget->pOps->GetTaktOsOps != NULL &&
           pTarget->pOps->Stop != NULL;
}

static inline void HciTargetUartTrace(const HciTarget_t *pTarget, uint8_t DevNo)
{
    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->UartTrace == NULL)
    {
        return;
    }

    pTarget->pOps->UartTrace(pTarget->pContext, DevNo);
}

static inline void HciTargetGetSdcMem(const HciTarget_t *pTarget,
                                      uint32_t *pRequired,
                                      uint32_t *pCapacity)
{
    if (pRequired != NULL)
    {
        *pRequired = 0U;
    }

    if (pCapacity != NULL)
    {
        *pCapacity = 0U;
    }

    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->GetSdcMem == NULL)
    {
        return;
    }

    pTarget->pOps->GetSdcMem(pTarget->pContext, pRequired, pCapacity);
}

static inline int32_t HciTargetLastError(const HciTarget_t *pTarget)
{
    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->LastError == NULL)
    {
        return 0;
    }

    return pTarget->pOps->LastError(pTarget->pContext);
}

#ifdef __cplusplus
}
#endif

#endif /* HCI_TARGET_H */
