/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf.h"

#include "TaktOS.h"
#include "TaktOSThread.h"
#include "board.h"
#include "coredev/iopincfg.h"
#include "coredev/system_core_clock.h"
#include "hci_app.h"
#include "hci_trace.h"
#include "iopinctrl.h"
#include "miscdev/led.h"

#define HCI_THREAD_STACK_SIZE    3072U
#define STATUS_THREAD_STACK_SIZE 512U
#define STATUS_UPDATE_MS         100U

#ifdef MCU_OSC_CONFIG
McuOsc_t g_McuOsc = MCU_OSC_CONFIG;
#endif

static HciApp_t s_HciApp;
static const IOPinCfg_t s_LedPins[] = LED_PINS;

alignas(8) static uint8_t s_HciThreadMem[TAKTOS_THREAD_MEM_SIZE(HCI_THREAD_STACK_SIZE)];
alignas(8) static uint8_t s_StatusThreadMem[TAKTOS_THREAD_MEM_SIZE(STATUS_THREAD_STACK_SIZE)];

static void HciLedWrite(uint8_t Port, uint8_t Pin, int Active, bool On)
{
    bool high = Active == LED_LOGIC_HIGH ? On : !On;
    if (high)
    {
        IOPinSet(Port, Pin);
    }
    else
    {
        IOPinClear(Port, Pin);
    }
}

static void HciStatusSet(bool Red, bool Green, bool Blue)
{
    HciLedWrite(HCI_LED_RED_PORT, HCI_LED_RED_PIN, HCI_LED_RED_ACTIVE, Red);
    HciLedWrite(HCI_LED_GREEN_PORT, HCI_LED_GREEN_PIN, HCI_LED_GREEN_ACTIVE, Green);
    HciLedWrite(HCI_LED_BLUE_PORT, HCI_LED_BLUE_PIN, HCI_LED_BLUE_ACTIVE, Blue);
}

static void HciStatusThread(void *)
{
    for (;;)
    {
        if (!s_HciApp.Runtime.Started)
        {
            HciStatusSet(true, false, false);
        }
        else if (!HciAppHostIsOpen(&s_HciApp))
        {
            HciStatusSet(false, false, true);
        }
        else
        {
            HciStatusSet(false, true, false);
        }

        (void)TaktOSThreadSleep(TaktOSCurrentThread(), STATUS_UPDATE_MS);
    }
}

static void HciFatal(void)
{
    HciStatusSet(true, false, false);
    for (;;)
    {
        __WFE();
    }
}

static HciAppHost_t HciSelectHost(void)
{
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U ?
           HCI_APP_HOST_USB : HCI_APP_HOST_UART;
}

int main(void)
{
    IOPinCfg(s_LedPins, sizeof(s_LedPins) / sizeof(s_LedPins[0]));
    HciStatusSet(false, false, false);

    HciTraceInit();

    uint32_t usbReg = NRF_POWER->USBREGSTATUS;
    HciAppHost_t host = HciSelectHost();
    HciTrace("boot: usbregstatus=0x%08lX vbus=%u outrdy=%u host=%s\r\n",
             (unsigned long)usbReg,
             (unsigned)((usbReg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U),
             (unsigned)((usbReg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) != 0U),
             host == HCI_APP_HOST_USB ? "usb" : "uart");

    TaktOSCfg_t kernelCfg = {};
    kernelCfg.KernClockHz = SystemCoreClockGet();
    kernelCfg.TickHz = 1000U;
    HciTrace("boot: coreclk=%lu\r\n", (unsigned long)kernelCfg.KernClockHz);
    if (TaktOSInit(&kernelCfg) != TAKTOS_OK)
    {
        HciTrace("fatal: TaktOSInit\r\n");
        HciFatal();
    }

    if (!HciAppInit(&s_HciApp, host))
    {
        HciTrace("fatal: HciAppInit err=%d target=%ld\r\n",
                 s_HciApp.LastError, (long)s_HciApp.Target.LastError);
        HciFatal();
    }

    if (TaktOSThreadCreate(s_HciThreadMem,
                           sizeof(s_HciThreadMem),
                           HciAppThread,
                           &s_HciApp,
                           TAKTOS_PRIORITY_HIGHEST) == nullptr)
    {
        HciTrace("fatal: hci thread create\r\n");
        HciFatal();
    }

    if (TaktOSThreadCreate(s_StatusThreadMem,
                           sizeof(s_StatusThreadMem),
                           HciStatusThread,
                           nullptr,
                           TAKTOS_PRIORITY_LOW) == nullptr)
    {
        HciTrace("fatal: status thread create\r\n");
        HciFatal();
    }

    HciTrace("boot: starting scheduler\r\n");
    TaktOSStart();
}
