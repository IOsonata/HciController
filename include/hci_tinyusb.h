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

    uint32_t TaskCount;
    uint32_t RxDropCount;
    uint32_t ReadErrorCount;
    uint32_t WriteBusyCount;
    uint32_t WriteErrorCount;
    uint32_t CallbackInterfaceErrorCount;
} HciTinyUsb_t;

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
