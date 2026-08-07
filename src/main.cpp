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
#include "hci_nrf52840.h"
#include "hci_trace.h"
#include "iopinctrl.h"
#include "miscdev/led.h"

#define HCI_THREAD_STACK_SIZE    3072U
#define STATUS_THREAD_STACK_SIZE 512U
#define STATUS_UPDATE_MS         100U

/*
 * board.h names this MCU_OSC. Defining it overrides the __WEAK default in
 * IOsonata, which is HFXO 32 MHz and LFXO 32768 at 20 ppm. Leave it undefined
 * and that default is used, which is what the dongle has. It matters because
 * HciNrf52840MpslInit chooses MPSL_CLOCK_LF_SRC_XTAL or _RC from this, and
 * SystemCoreClockGet feeds the TaktOS tick rate below.
 */
#ifdef MCU_OSC
McuOsc_t g_McuOsc = MCU_OSC;
#endif

/*
 * A board without a status LED reachable from this part, which is any board
 * where the LED belongs to something else. Driving the pins another board uses
 * would put a signal on whatever is actually wired there.
 */
#ifndef HCI_STATUS_LEDS
#define HCI_STATUS_LEDS 1
#endif

static HciApp_t s_HciApp;

#if HCI_STATUS_LEDS
static const IOPinCfg_t s_LedPins[] = LED_PINS;
#endif

alignas(8) static uint8_t s_HciThreadMem[TAKTOS_THREAD_MEM_SIZE(HCI_THREAD_STACK_SIZE)];
alignas(8) static uint8_t s_StatusThreadMem[TAKTOS_THREAD_MEM_SIZE(STATUS_THREAD_STACK_SIZE)];

#if HCI_STATUS_LEDS
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
#else
static void HciStatusSet(bool, bool, bool)
{
}
#endif

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

/*
 * board.h names the host port for the board it describes. A -D on the command
 * line overrides it, which is how the same tree builds a dongle image and a
 * UART controller for a board whose host is another part on the same PCB.
 * hci_app.h documents the three values.
 */
#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT HCI_HOST_SELECT_AUTO
#endif

#if HCI_HOST_SELECT != HCI_HOST_SELECT_AUTO && \
    HCI_HOST_SELECT != HCI_HOST_SELECT_USB && \
    HCI_HOST_SELECT != HCI_HOST_SELECT_UART
#error "HCI_HOST_SELECT must be HCI_HOST_SELECT_AUTO, _USB or _UART"
#endif

/*
 * A UART host with no pins to reach it on would otherwise fail deep inside the
 * UART_PINS expansion on an undeclared identifier, which does not say what is
 * missing. Testing UART_PINS itself proves nothing: board.h defines it for
 * every board, from these two. So test what it is built out of.
 */
#if HCI_HOST_SELECT != HCI_HOST_SELECT_USB && \
    (!defined(UART_RX_PORT) || !defined(UART_TX_PORT))
#error "the selected host needs UART_RX_PORT/PIN and UART_TX_PORT/PIN from board.h"
#endif

static HciAppHost_t HciSelectHost(void)
{
#if HCI_HOST_SELECT == HCI_HOST_SELECT_USB
    return HCI_APP_HOST_USB;
#elif HCI_HOST_SELECT == HCI_HOST_SELECT_UART
    return HCI_APP_HOST_UART;
#elif HCI_HOST_SELECT == HCI_HOST_SELECT_AUTO
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U ?
           HCI_APP_HOST_USB : HCI_APP_HOST_UART;
#else
#error "unreachable, the selection was checked above"
#endif
}

/*
 * Hold the host off until the port exists.
 *
 * RTS is an output and it means "ready to receive", asserted low. Out of
 * reset it is not an output at all: it is a plain input with no pull, so the
 * peer's CTS floats and reads whatever the board leaks. A peer that reads it
 * as asserted starts transmitting into a part that has no UART yet, and those
 * octets are gone. Nothing in HCI retries a command.
 *
 * That is not hypothetical. A Nordic Thingy:91 holds this part in reset while
 * it opens its HCI transport, releases it, and sends HCI Reset at once. This
 * firmware cannot come out of reset, start TaktOS, the radio and the port in
 * the ten milliseconds that host allows, so the first command it ever sends
 * is lost and it asserts ten seconds later on a command this part never saw.
 *
 * A host that can be configured has an answer for this: Zephyr's
 * CONFIG_BT_WAIT_NOP makes it wait for the controller's startup No Operation
 * before sending anything. A host that cannot be changed has none, so the
 * answer has to be here.
 *
 * So the pin is driven high, not ready, in the first instruction of main,
 * before anything that takes time. The peer then holds its transmission until
 * UARTInit hands the pin to the peripheral, which asserts it when the
 * receiver is genuinely listening. Costs one pin write and closes the window
 * completely rather than making it smaller.
 *
 * Only for a board that asked for flow control. Without it the peer is not
 * watching this wire and driving it would put a signal on a pin the board may
 * be using for something else.
 */
#if defined(UART_HW_FLOWCTRL) && UART_HW_FLOWCTRL
static const IOPinCfg_t s_RtsHoldOff = {
    UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP,
    IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL
};

static void HciHoldHostOff(void)
{
    IOPinCfg(&s_RtsHoldOff, 1);
    IOPinSet(UART_RTS_PORT, UART_RTS_PIN);
}
#else
static void HciHoldHostOff(void) {}
#endif

int main(void)
{
    HciHoldHostOff();

#if HCI_STATUS_LEDS
    IOPinCfg(s_LedPins, sizeof(s_LedPins) / sizeof(s_LedPins[0]));
#endif
    HciStatusSet(false, false, false);

    HciTraceInit();

    uint32_t usbReg = NRF_POWER->USBREGSTATUS;
    HciAppHost_t host = HciSelectHost();

    /*
     * Naming the build option next to the port it produced separates a board
     * built for the wrong host from one that read VBUS and got it wrong.
     */
#if HCI_HOST_SELECT == HCI_HOST_SELECT_USB
    static const char *pSelect = "usb";
#elif HCI_HOST_SELECT == HCI_HOST_SELECT_UART
    static const char *pSelect = "uart";
#else
    static const char *pSelect = "auto";
#endif


    HciTrace("boot: usbregstatus=0x%08lX vbus=%u outrdy=%u select=%s host=%s\r\n",
             (unsigned long)usbReg,
             (unsigned)((usbReg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U),
             (unsigned)((usbReg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) != 0U),
             pSelect,
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

    if (!HciAppInit(&s_HciApp, host, HciNrf52840Target()))
    {
        HciTrace("fatal: HciAppInit err=%d target=%ld\r\n",
                 s_HciApp.LastError,
                 (long)HciTargetLastError(&s_HciApp.Target));
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
