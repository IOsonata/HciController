/**-------------------------------------------------------------------------
@file	hci_target.h

@brief	Portable HciController target lifecycle interface.

		Defines part-specific initialization, USB and UART diagnostics, runtime
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
 * What the application needs from a part, and nothing else.
 *
 * Everything here is hardware that differs between parts: the low frequency
 * clock and how MPSL is told about it, the USB device peripheral if there is
 * one, the errata, and the interrupt wiring. What the SoftDevice Controller is
 * configured to do is not in this interface, because it does not vary. That
 * lives in hci_sdc_resources.h and every port calls the same code.
 *
 * The application held an HciNrf52840_t directly before this existed, which
 * meant the whole stack above the radio named one part in nine places for no
 * reason. Adding a second part meant editing all of them.
 *
 * A port publishes one HciTarget_t. It owns its own instance, because a board
 * has one radio and there is nothing to allocate.
 */
typedef struct {
    bool (*Init)(void *pContext,
                 HciTaktOs_t *pRuntime,
                 uint8_t *pSdcMem,
                 size_t SdcMemCapacity,
                 bool UsbEnabled);

    void (*GetTaktOsOps)(void *pContext, HciTaktOsOps_t *pOps);

    /*
     * The USB entries are optional. A part with no USB device peripheral, or a
     * board that only ever talks over its UART, leaves them null and the
     * application skips them rather than testing for a part.
     */
    bool (*UsbStart)(void *pContext);
    void (*UsbPassMark)(void *pContext);
    void (*UsbPowerProcess)(void *pContext);

    /*
     * True when the USB peripheral has reached a state that no further
     * settling pass will leave, so the application stops waiting instead of
     * spending its whole budget on a port that will never enumerate. What that
     * state is differs per part. Optional; a port that cannot tell leaves it
     * null and the loop runs to its limit.
     */
    bool (*UsbStuck)(const void *pContext);

    /*
     * Report what the port knows about USB bring up, under a label the
     * application supplies. The detail is peripheral register state, which is
     * why it is printed here and not by the caller: the application decides
     * when a report is worth making, the port decides what it says. Pass is
     * the settling pass number, or zero when the report is not from the loop.
     */
    void (*UsbTrace)(const void *pContext, const char *pLabel, uint32_t Pass);

    /*
     * Report what the UART hardware says about the host link, for the
     * instance the board put the host on.
     *
     * Same division as UsbTrace: the application knows when a report is worth
     * making, the port knows what there is to say. What there is to say here
     * is whether the peripheral is enabled, which pins it actually ended up
     * on, what the error source holds, and whether the peer is asserting the
     * clear to send line. A link that moves no octets in either direction
     * looks identical from above whether the peer is silent, the framing is
     * wrong, or nothing ever told the peer it could send, and only the
     * hardware can tell those apart.
     *
     * Optional; a port with no UART leaves it null.
     */
    void (*UartTrace)(const void *pContext, uint8_t DevNo);

    void (*Stop)(void *pContext);

    /*
     * The two SoftDevice Controller pool figures once Init has run: the octets
     * the controller asked for, and the octets it was given. Both zero from a
     * port that does not track them, which is not the same as a controller
     * that wanted no memory.
     */
    void (*GetSdcMem)(const void *pContext,
                      uint32_t *pRequired,
                      uint32_t *pCapacity);

    /* Whatever the port last failed with, for the application to report. */
    int32_t (*LastError)(const void *pContext);
} HciTargetOps_t;

typedef struct {
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

static inline bool HciTargetHasUsb(const HciTarget_t *pTarget)
{
    return pTarget != NULL && pTarget->pOps != NULL &&
           pTarget->pOps->UsbStart != NULL &&
           pTarget->pOps->UsbPassMark != NULL &&
           pTarget->pOps->UsbPowerProcess != NULL;
}

static inline bool HciTargetUsbStuck(const HciTarget_t *pTarget)
{
    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->UsbStuck == NULL)
    {
        return false;
    }

    return pTarget->pOps->UsbStuck(pTarget->pContext);
}

static inline void HciTargetUsbTrace(const HciTarget_t *pTarget,
                                     const char *pLabel,
                                     uint32_t Pass)
{
    if (pTarget == NULL || pTarget->pOps == NULL ||
        pTarget->pOps->UsbTrace == NULL)
    {
        return;
    }

    pTarget->pOps->UsbTrace(pTarget->pContext, pLabel, Pass);
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
