/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_TINYUSB_H
#define HCI_TINYUSB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfifo.h"
#include "usb/usbd_cdc_intrf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*HciTinyUsbWake_t)(void *pContext);

/*
 * The endpoints worth watching: the HCI stream both ways and the log's IN.
 * The log has no OUT traffic and the two notification endpoints only see a
 * serial state change, so neither can be the one that stops a transfer.
 *
 * The addresses themselves are in tusb_config.h beside the rest of the device
 * configuration, so the descriptors and this agree by construction.
 */
#define HCI_TINYUSB_EP_COUNT 3

/*
 * Turns of the device stack an endpoint may stay busy before the port is
 * taken down and put back up.
 *
 * A wedged endpoint stops producing events, so the only thing still turning
 * the stack over is the poll, twice a turn every few milliseconds. Four
 * hundred is on the order of a second of that, which no transfer on a bulk
 * endpoint comes close to and which is well inside the time a host takes to
 * give up and reset the port itself. Doing it here is the safer of the two,
 * because a wedged endpoint has nothing in flight, where a host reset can
 * arrive in the middle of a transfer.
 */
#define HCI_TINYUSB_EP_STUCK_TURNS 400U

/* Turns spent detached, long enough for a host to see the port go. */
#define HCI_TINYUSB_DETACH_TURNS 40U

typedef struct {
    UsbdCdcDevIntrf_t *pIntrf;
    uint8_t Interface;

    HciTinyUsbWake_t Wake;
    void *pWakeContext;

    volatile bool Started;
    volatile bool LineStatePending;
    volatile bool RequestedOpen;

    uint32_t TaskCount;

    /*
     * How many turns of the device stack each watched endpoint has been
     * waiting on a transfer without finishing one, and the worst run of that
     * seen so far.
     *
     * This is the one reading that says a port has stopped rather than paused.
     * An endpoint is marked busy when a transfer is handed to the driver and
     * the mark is cleared in exactly one place, the device stack task handling
     * that transfer's completion event. If the event is lost, and the queue
     * that holds it drops silently when it fills, the mark is never cleared
     * and the endpoint refuses every later transfer. Nothing faults, nothing
     * prints, and no other counter here moves.
     *
     * A few turns busy is ordinary, that is a transfer in flight. Hundreds is
     * the failure.
     */
    uint16_t EpBusyTurns[HCI_TINYUSB_EP_COUNT];
    uint16_t EpBusyWorst[HCI_TINYUSB_EP_COUNT];

    /*
     * How many times the port has been taken down and put back up because an
     * endpoint stayed busy past the limit above, and where the current one is
     * in its detached stretch. Zero when nothing is being restarted.
     *
     * The alternative to doing this is what has been happening: the endpoint
     * stops, the host keeps sending into a port that will not take it, and
     * some time later the host resets the port itself. That reset lands
     * wherever it lands, and if a transfer is in flight when it does, the
     * peripheral is left holding off every endpoint with nothing able to
     * release it, which is a stop only a replug clears. Restarting from here
     * happens when the wedged endpoint has nothing in flight.
     */
    uint32_t RestartCount;
    uint16_t DetachTurns;

    uint32_t RxDropCount;
    uint32_t ReadErrorCount;
    uint32_t WriteBusyCount;
    uint32_t WriteErrorCount;
    uint32_t CallbackInterfaceErrorCount;
} HciTinyUsb_t;

/*
 * Write to a CDC interface directly, for the log.
 *
 * The HCI stream goes through an IOsonata UsbdCdcIntrf with its own buffers
 * in both directions, because it is a transport. The log is one direction,
 * already buffered by its own ring, and nothing reads from it, so it needs
 * none of that and takes the device stack call instead.
 *
 * Returns how many octets the endpoint took, which is zero when the device
 * is not mounted or nothing has opened the port. A log with nobody listening
 * queues rather than blocks, which is the caller's business, not this one's.
 */
size_t HciTinyUsbWrite(uint8_t Interface, const uint8_t *pData, size_t Len);

/*
 * Whether something has opened a given function, by the same test the write
 * above makes: mounted and DTR asserted. For a caller that wants to know the
 * moment a terminal arrives rather than finding out by having a write refused.
 */
bool HciTinyUsbPortIsOpen(uint8_t Interface);

bool HciTinyUsbInit(HciTinyUsb_t *pUsb,
                    UsbdCdcDevIntrf_t *pIntrf,
                    uint8_t Interface,
                    HciTinyUsbWake_t Wake,
                    void *pWakeContext);

bool HciTinyUsbStart(HciTinyUsb_t *pUsb);
void HciTinyUsbProcess(HciTinyUsb_t *pUsb);
bool HciTinyUsbIsOpen(const HciTinyUsb_t *pUsb);
bool HciTinyUsbIsMounted(const HciTinyUsb_t *pUsb);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TINYUSB_H */
