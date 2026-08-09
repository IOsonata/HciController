/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * nRF52840 TinyUSB DCD integration used only for post-mortem diagnostics.
 *
 * The actual DCD below is the installed TinyUSB nRF5x implementation without
 * any scheduling or EasyDMA behavior changed. The only wrapped function is
 * dcd_int_handler(), so the last hardware state seen on entry and exit remains
 * readable over SWD after the USB port stops responding.
 *
 * In particular, Dma199Entry/Dma199Exit expose the nRF52840 anomaly-199 latch
 * at 0x40027C1C. If the last completed ISR exits with it at 0x82 and no later
 * USBD interrupt occurs, the EasyDMA operation that ISR started never reached
 * an END event. If it is zero, the failure is elsewhere in the USBD event or
 * stack state and the DMA scheduler should not be blamed.
 */

#include <stdint.h>

#include "tusb_option.h"

#if CFG_TUD_ENABLED && CFG_TUSB_MCU == OPT_MCU_NRF5X

typedef struct
{
    volatile uint32_t IrqCount;
    volatile uint32_t Dma199Entry;
    volatile uint32_t Dma199Exit;
    volatile uint32_t PendingEntry;
    volatile uint32_t PendingExit;
    volatile uint32_t Inten;
    volatile uint32_t EventCause;
    volatile uint32_t EpStatus;
    volatile uint32_t EpDataStatus;
    volatile uint32_t EpInEnable;
    volatile uint32_t EpOutEnable;
    volatile uint32_t EpIn2Amount;
    volatile uint32_t EpOut2Amount;
    volatile uint32_t EpIn4Amount;
    volatile uint32_t EpOut4Amount;
    volatile uint32_t UsbEnable;
    volatile uint32_t UsbPullup;
    volatile uint32_t UsbRegStatus;
    volatile uint32_t HfclkStat;
} HciTinyUsbDcdDiag_t;

/* Intentionally global and volatile: inspect this symbol directly over SWD. */
volatile HciTinyUsbDcdDiag_t g_HciTinyUsbDcdDiag;

#define dcd_int_handler HciTinyUsbDcdIntHandler
#include "portable/nordic/nrf5x/dcd_nrf5x.c"
#undef dcd_int_handler

#define HCI_DCD_ERRATA_199_REG (*((volatile uint32_t *)0x40027C1CUL))

static uint32_t HciTinyUsbDcdPendingEvents(void)
{
    uint32_t pending = 0U;
    volatile uint32_t *event = &NRF_USBD->EVENTS_USBRESET;

    for (uint32_t i = 0U; i <= USBD_INTEN_EPDATA_Pos; i++)
    {
        if (event[i] != 0U)
        {
            pending |= 1UL << i;
        }
    }

    return pending;
}

static void HciTinyUsbDcdSnapshot(void)
{
    g_HciTinyUsbDcdDiag.Inten = NRF_USBD->INTEN;
    g_HciTinyUsbDcdDiag.EventCause = NRF_USBD->EVENTCAUSE;
    g_HciTinyUsbDcdDiag.EpStatus = NRF_USBD->EPSTATUS;
    g_HciTinyUsbDcdDiag.EpDataStatus = NRF_USBD->EPDATASTATUS;
    g_HciTinyUsbDcdDiag.EpInEnable = NRF_USBD->EPINEN;
    g_HciTinyUsbDcdDiag.EpOutEnable = NRF_USBD->EPOUTEN;
    g_HciTinyUsbDcdDiag.EpIn2Amount = NRF_USBD->EPIN[2].AMOUNT;
    g_HciTinyUsbDcdDiag.EpOut2Amount = NRF_USBD->EPOUT[2].AMOUNT;
    g_HciTinyUsbDcdDiag.EpIn4Amount = NRF_USBD->EPIN[4].AMOUNT;
    g_HciTinyUsbDcdDiag.EpOut4Amount = NRF_USBD->EPOUT[4].AMOUNT;
    g_HciTinyUsbDcdDiag.UsbEnable = NRF_USBD->ENABLE;
    g_HciTinyUsbDcdDiag.UsbPullup = NRF_USBD->USBPULLUP;
    g_HciTinyUsbDcdDiag.UsbRegStatus = NRF_POWER->USBREGSTATUS;
    g_HciTinyUsbDcdDiag.HfclkStat = NRF_CLOCK->HFCLKSTAT;
}

void dcd_int_handler(uint8_t rhport)
{
    g_HciTinyUsbDcdDiag.IrqCount++;
    g_HciTinyUsbDcdDiag.Dma199Entry = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingEntry = HciTinyUsbDcdPendingEvents();

    HciTinyUsbDcdIntHandler(rhport);

    g_HciTinyUsbDcdDiag.Dma199Exit = HCI_DCD_ERRATA_199_REG;
    g_HciTinyUsbDcdDiag.PendingExit = HciTinyUsbDcdPendingEvents();
    HciTinyUsbDcdSnapshot();
}

#endif
