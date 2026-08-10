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

typedef struct {
    UsbdCdcDevIntrf_t *pIntrf;
    uint8_t Interface;

    HciTinyUsbWake_t Wake;
    void *pWakeContext;

    volatile bool Started;
    volatile bool LineStatePending;
    volatile bool RequestedOpen;

    /*
     * A FIFO read is destructive, while tud_cdc_n_write is allowed to accept
     * fewer octets than requested. Keep the unread tail here until the device
     * stack accepts it, otherwise one short write removes bytes from the H:4
     * stream permanently and every packet after it is misframed.
     */
    size_t TxPendingOffset;
    size_t TxPendingLen;

    uint32_t TaskCount;
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

/*
 * Idempotent for the same object. The device stack is initialized before the
 * part-specific USB hardware is started, so a target failure must be retryable
 * without trying to initialize TinyUSB a second time.
 */
bool HciTinyUsbStart(HciTinyUsb_t *pUsb);
void HciTinyUsbProcess(HciTinyUsb_t *pUsb);
bool HciTinyUsbIsOpen(const HciTinyUsb_t *pUsb);
bool HciTinyUsbIsMounted(const HciTinyUsb_t *pUsb);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TINYUSB_H */
