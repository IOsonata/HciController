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
     * marked busy without that mark clearing, and the worst run of that so
     * far. Read in the order the report prints them: HCI in, HCI out, log in.
     *
     * The mark is set when a transfer is handed to the driver and cleared in
     * exactly one place, the device stack task handling that transfer's
     * completion. If the completion is lost, and the queue that holds it
     * drops silently when it fills, the mark is never cleared and the
     * endpoint refuses every later transfer with nothing faulting, printing
     * or counting.
     *
     * Read the two directions differently, because the mark does not mean the
     * same thing on each.
     *
     * On an IN endpoint it means a transfer this device asked for is still
     * outstanding, so a long run really is a stall. On an OUT endpoint it
     * means a read is armed and waiting for the host to send something, which
     * is the ordinary resting state of a healthy idle endpoint: the stack
     * arms the next read as soon as one completes and there is room, so an
     * OUT endpoint with an idle host sits marked busy for as long as the host
     * stays quiet. A long run there says nothing on its own, and taking it
     * for a stall is a mistake this reading was written into once already.
     *
     * What a long OUT run does say, when the host is known to be sending, is
     * that the octets are not arriving. That comparison is the reader's, and
     * rx on the link line is the other half of it.
     */
    uint16_t EpBusyTurns[HCI_TINYUSB_EP_COUNT];
    uint16_t EpBusyWorst[HCI_TINYUSB_EP_COUNT];

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
