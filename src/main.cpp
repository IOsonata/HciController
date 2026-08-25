/**-------------------------------------------------------------------------
@file	main.cpp

@brief	HciController firmware entry point and board runtime selection.

		Initializes board status, persistent transport mode selection, TaktOS,
		the nRF52840 target, HCI application runtime, mode-switch handling,
		and the firmware worker threads.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf.h"

#include "istddef.h"
#include "TaktOS.h"
#include "TaktOSThread.h"
#include "board.h"
#include "coredev/iopincfg.h"
#include "coredev/system_core_clock.h"
#include "hci_app.h"
#include "hci_nrf52840.h"
#include "hci_trace.h"
#include "hci_version.h"
#include "iopinctrl.h"
#include "miscdev/led.h"

#if HCI_MODE_SWITCH
#include "storage/nvm.h"
#include "storage/nvm_intrf.h"
#include "storage/nvm_region.h"
#endif

#define DEVICE_NAME               "HciController"
#define HCI_THREAD_STACK_SIZE      3072U
/* Mode changes stop HCI/MPSL/SDC before the NVM erase/write/verify path. */
#define STATUS_THREAD_STACK_SIZE   3072U
#define STATUS_UPDATE_MS           20U
#define HCI_MODE_DEBOUNCE_PASSES   2U

#define HCI_THREAD_PRIORITY        TAKTOS_PRIORITY_HIGHEST
#if HCI_MODE_SWITCH
/*
 * TaktOS is strict fixed-priority. Native USB can keep the HCI thread ready
 * continuously, so a lower-priority polling thread may never execute. Give
 * the mode-control thread the reserved critical level so its 20 ms poll can
 * preempt HCI briefly. A confirmed button press stops the HCI runtime, stores
 * the next mode with MPSL/SDC down, then resets.
 */
#define STATUS_THREAD_PRIORITY     TAKTOS_PRIORITY_CRITICAL
#else
#define STATUS_THREAD_PRIORITY     TAKTOS_PRIORITY_LOW
#endif

static_assert(!HCI_MODE_SWITCH || STATUS_THREAD_PRIORITY > HCI_THREAD_PRIORITY,
              "selectable-mode control thread must preempt the HCI thread");

#ifdef MCU_OSC
McuOsc_t g_McuOsc = MCU_OSC;
#endif

__attribute__ ((section(".Version"), used))
const AppInfo_t g_AppInfo = {
    DEVICE_NAME,
    {FIRMWARE_VERSION, 0U, BUILDN},
    {0},
};

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT HCI_HOST_SELECT_AUTO
#endif

#if HCI_HOST_SELECT != HCI_HOST_SELECT_AUTO && \
    HCI_HOST_SELECT != HCI_HOST_SELECT_USB && \
    HCI_HOST_SELECT != HCI_HOST_SELECT_UART
#error "HCI_HOST_SELECT must be HCI_HOST_SELECT_AUTO, _USB or _UART"
#endif

#if BOARD == UDG_NRF52840 && HCI_HOST_SELECT == HCI_HOST_SELECT_UART
#error "UDG_NRF52840 supports USB-H4 and USB-HCI only; UART-HCI is not supported"
#endif

#if (BOARD == THINGY91_NRF52840 || BOARD == WILDTHING51 || BOARD == WILDTHING91) && \
    HCI_HOST_SELECT != HCI_HOST_SELECT_UART
#error "This board is HCI UART-only"
#endif

static HciApp_t s_HciApp;
static HciAppMode_t s_HciMode;

#if HCI_STATUS_LEDS
static const IOPinCfg_t s_LedPins[] = LED_PINS;
#endif

#if HCI_MODE_SWITCH
static const IOPinCfg_t s_ModeButtonPin = {
    HCI_MODE_BUTTON_PORT,
    HCI_MODE_BUTTON_PIN,
    HCI_MODE_BUTTON_PINOP,
    IOPINDIR_INPUT,
    IOPINRES_PULLUP,
    IOPINTYPE_NORMAL,
};

static NvmIntrf s_ModeNvmIntrf;
static Nvm s_ModeNvm;
static bool s_ModeButtonLatched;
static uint8_t s_ModeButtonDebounce;
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

static void HciFatal(void)
{
    HciStatusSet(true, false, false);
    for (;;)
    {
        __WFE();
    }
}

static const char *HciModeName(HciAppMode_t Mode)
{
    switch (Mode)
    {
        case HCI_APP_MODE_UART_H4:
            return "uart-h4";
        case HCI_APP_MODE_USB_H4:
            return "usb-h4";
        case HCI_APP_MODE_USB_NATIVE:
            return "usb-native";
        default:
            return "invalid";
    }
}

static HciAppMode_t HciUsbBuildDefaultMode(void)
{
#if HCI_USB_HCI_TRANSPORT == HCI_USB_HCI_TRANSPORT_NATIVE
    return HCI_APP_MODE_USB_NATIVE;
#else
    return HCI_APP_MODE_USB_H4;
#endif
}

static HciAppMode_t HciBoardDefaultMode(void)
{
#if BOARD == UDG_NRF52840
    return HciUsbBuildDefaultMode();
#elif BOARD == IBK_NRF52840
#if HCI_HOST_SELECT == HCI_HOST_SELECT_UART
    return HCI_APP_MODE_UART_H4;
#elif HCI_HOST_SELECT == HCI_HOST_SELECT_USB
    return HciUsbBuildDefaultMode();
#else
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U ?
           HciUsbBuildDefaultMode() : HCI_APP_MODE_UART_H4;
#endif
#else
    return HCI_APP_MODE_UART_H4;
#endif
}

static bool HciModeAllowed(HciAppMode_t Mode)
{
#if BOARD == UDG_NRF52840
    return Mode == HCI_APP_MODE_USB_H4 || Mode == HCI_APP_MODE_USB_NATIVE;
#elif BOARD == IBK_NRF52840
    return Mode == HCI_APP_MODE_UART_H4 || Mode == HCI_APP_MODE_USB_H4 ||
           Mode == HCI_APP_MODE_USB_NATIVE;
#else
    return Mode == HCI_APP_MODE_UART_H4;
#endif
}

#if HCI_MODE_SWITCH

#define HCI_MODE_NVM_MAGIC          0x4843494DU /* HCIM */
#define HCI_MODE_NVM_VERSION        1U
#define HCI_MODE_NVM_REGION         0

typedef struct
{
    uint32_t Magic;
    uint32_t Version;
    uint32_t Board;
    uint32_t Mode;
    uint32_t Check;
} HciModeRecord_t;

static uint32_t HciModeRecordCheck(const HciModeRecord_t *pRecord)
{
    return ~(pRecord->Magic ^ pRecord->Version ^ pRecord->Board ^ pRecord->Mode);
}

static bool HciModeNvmSetup(void)
{
    NvmCfg_t cfg = {};
    NvmMcuCfg(cfg);

    const uintptr_t addr = NvmRegionAddr(HCI_MODE_NVM_REGION);
    const size_t size = NvmRegionSize(HCI_MODE_NVM_REGION);

    HciTrace("mode: nvm addr=0x%08lX size=%lu\r\n",
             (unsigned long)addr, (unsigned long)size);

    if (addr == 0U || cfg.EraseSize == 0U || size < cfg.EraseSize ||
        (addr % cfg.EraseSize) != 0U)
    {
        HciTrace("mode: nvm geometry invalid\r\n");
        return false;
    }

    if (!s_ModeNvmIntrf.Init(nullptr, false))
    {
        HciTrace("mode: nvm interface init failed\r\n");
        return false;
    }

    /* Use one erase page only; the rest of NVM0 remains available. */
    return s_ModeNvm.Init(cfg, &s_ModeNvmIntrf, addr, cfg.EraseSize);
}

static HciAppMode_t HciModeNvmLoad(HciAppMode_t Default, bool *pStored)
{
    HciModeRecord_t record = {};
    const int rd = s_ModeNvm.Read(0U, &record, sizeof(record));

    const bool valid = rd == (int)sizeof(record) &&
                       record.Magic == HCI_MODE_NVM_MAGIC &&
                       record.Version == HCI_MODE_NVM_VERSION &&
                       record.Board == (uint32_t)BOARD &&
                       record.Check == HciModeRecordCheck(&record) &&
                       record.Mode <= (uint32_t)HCI_APP_MODE_USB_NATIVE &&
                       HciModeAllowed((HciAppMode_t)record.Mode);

    if (pStored != nullptr)
    {
        *pStored = valid;
    }

    return valid ? (HciAppMode_t)record.Mode : Default;
}

static bool HciModeNvmStore(HciAppMode_t Mode)
{
    if (!HciModeAllowed(Mode))
    {
        return false;
    }

    HciModeRecord_t record = {
        HCI_MODE_NVM_MAGIC,
        HCI_MODE_NVM_VERSION,
        (uint32_t)BOARD,
        (uint32_t)Mode,
        0U,
    };
    record.Check = HciModeRecordCheck(&record);

    const int erase = s_ModeNvm.Erase(0U, s_ModeNvm.EraseSize());
    if (erase != 0)
    {
        return false;
    }

    const int wr = s_ModeNvm.Write(0U, &record, sizeof(record));
    if (wr != (int)sizeof(record))
    {
        return false;
    }

    HciModeRecord_t verify = {};
    const int rd = s_ModeNvm.Read(0U, &verify, sizeof(verify));
    return rd == (int)sizeof(verify) &&
           verify.Magic == record.Magic &&
           verify.Version == record.Version &&
           verify.Board == record.Board &&
           verify.Mode == record.Mode &&
           verify.Check == record.Check;
}

static HciAppMode_t HciNextMode(HciAppMode_t Mode)
{
#if BOARD == UDG_NRF52840
    return Mode == HCI_APP_MODE_USB_H4 ? HCI_APP_MODE_USB_NATIVE
                                       : HCI_APP_MODE_USB_H4;
#else
    switch (Mode)
    {
        case HCI_APP_MODE_UART_H4:
            return HCI_APP_MODE_USB_H4;
        case HCI_APP_MODE_USB_H4:
            return HCI_APP_MODE_USB_NATIVE;
        default:
            return HCI_APP_MODE_UART_H4;
    }
#endif
}

static uint32_t HciModeButtonRaw(void)
{
    return (uint32_t)IOPinRead(HCI_MODE_BUTTON_PORT, HCI_MODE_BUTTON_PIN);
}

static bool HciModeButtonDown(void)
{
    return HciModeButtonRaw() == 0U;
}

static void HciModeButtonProcess(void)
{
    const bool down = HciModeButtonDown();

    if (!down)
    {
        s_ModeButtonLatched = false;
        s_ModeButtonDebounce = 0U;
        return;
    }

    if (s_ModeButtonLatched)
    {
        return;
    }

    s_ModeButtonDebounce++;
    if (s_ModeButtonDebounce < HCI_MODE_DEBOUNCE_PASSES)
    {
        return;
    }

    s_ModeButtonLatched = true;
    const HciAppMode_t next = HciNextMode(s_HciMode);

    /*
     * Internal flash is not touched while MPSL/SDC owns the radio. Stop the
     * runtime first; HciAppStop waits for the HCI thread to leave, then stops
     * USB, SDC and MPSL. With the target down the NVM interface uses NVMC
     * directly, so the new mode can be committed without a retained-register
     * handoff through the USB DFU bootloader.
     */
    HciAppStop(&s_HciApp);
    if (s_HciApp.Initialized || !HciModeNvmStore(next))
    {
        HciFatal();
    }

    __disable_irq();
    __DSB();
    NVIC_SystemReset();
}

#endif /* HCI_MODE_SWITCH */

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

#if HCI_MODE_SWITCH
        HciModeButtonProcess();
#endif

        (void)TaktOSThreadSleep(TaktOSCurrentThread(), STATUS_UPDATE_MS);
    }
}

#if HCI_UART_EARLY_STARTUP
#if UART_RTS_PORT != 0 && UART_RTS_PORT != 1
#error "early UART RTS must be on nRF52840 P0 or P1"
#endif

static void HciUartHostNotReady(void)
{
    NRF_GPIO_Type *pPort = UART_RTS_PORT == 0 ? NRF_P0 : NRF_P1;
    const uint32_t mask = 1UL << UART_RTS_PIN;

    /* High first, then output: no transient active-low READY pulse. */
    pPort->OUTSET = mask;
    pPort->DIRSET = mask;
    __DSB();
}
#endif

int main(void)
{
#if HCI_UART_EARLY_STARTUP
    /* Fixed UART boards that are reset-coupled must arm receive immediately. */
    HciUartHostNotReady();
    HciTarget_t target = HciNrf52840Target();
    const bool earlyUartReady = HciAppUartEarlyInit(&s_HciApp, target);
#else
    HciTarget_t target = HciNrf52840Target();
#endif

#if HCI_STATUS_LEDS
    IOPinCfg(s_LedPins, sizeof(s_LedPins) / sizeof(s_LedPins[0]));
#endif
    HciStatusSet(false, false, false);

#if HCI_MODE_SWITCH
    IOPinCfg(&s_ModeButtonPin, 1U);
    s_ModeButtonLatched = HciModeButtonDown();
    s_ModeButtonDebounce = 0U;
#endif

    HciTraceInit();
    HciNrf52840ResetTrace();

#if HCI_UART_EARLY_STARTUP
    if (!earlyUartReady)
    {
        HciTrace("fatal: early UART init err=%d target=%ld\r\n",
                 s_HciApp.LastError,
                 (long)HciTargetLastError(&s_HciApp.Target));
        HciFatal();
    }
#endif

    bool storedMode = false;
    const HciAppMode_t defaultMode = HciBoardDefaultMode();

#if HCI_MODE_SWITCH
    if (!HciModeNvmSetup())
    {
        HciTrace("fatal: mode NVM region unavailable\r\n");
        HciFatal();
    }
    s_HciMode = HciModeNvmLoad(defaultMode, &storedMode);
#else
    s_HciMode = defaultMode;
#endif

    if (!HciModeAllowed(s_HciMode))
    {
        HciTrace("fatal: mode %u not allowed on BOARD=%u\r\n",
                 (unsigned)s_HciMode, (unsigned)BOARD);
        HciFatal();
    }

    const uint32_t usbReg = NRF_POWER->USBREGSTATUS;
    HciTrace("boot: board=%s usbregstatus=0x%08lX vbus=%u outrdy=%u mode=%s stored=%u\r\n",
             BOARD_NAME,
             (unsigned long)usbReg,
             (unsigned)((usbReg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U),
             (unsigned)((usbReg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) != 0U),
             HciModeName(s_HciMode),
             (unsigned)storedMode);

    TaktOSCfg_t kernelCfg = {};
    kernelCfg.KernClockHz = SystemCoreClockGet();
    kernelCfg.TickHz = 1000U;
    kernelCfg.TickPriority = TAKTOS_TICK_PRIORITY_DEFAULT;

    HciTrace("boot: coreclk=%lu\r\n", (unsigned long)kernelCfg.KernClockHz);
    if (TaktOSInit(&kernelCfg) != TAKTOS_OK)
    {
        HciTrace("fatal: TaktOSInit\r\n");
        HciFatal();
    }

    if (!HciAppInitMode(&s_HciApp, s_HciMode, target))
    {
        HciTrace("fatal: HciAppInitMode err=%d target=%ld\r\n",
                 s_HciApp.LastError,
                 (long)HciTargetLastError(&s_HciApp.Target));
        HciFatal();
    }

    HciTaktOsThreadArm(&s_HciApp.Runtime);
    if (TaktOSThreadCreate(s_HciThreadMem,
                           sizeof(s_HciThreadMem),
                           HciAppThread,
                           &s_HciApp,
                           HCI_THREAD_PRIORITY) == nullptr)
    {
        HciTaktOsThreadDisarm(&s_HciApp.Runtime);
        HciTrace("fatal: hci thread create\r\n");
        HciFatal();
    }

    if (TaktOSThreadCreate(s_StatusThreadMem,
                           sizeof(s_StatusThreadMem),
                           HciStatusThread,
                           nullptr,
                           STATUS_THREAD_PRIORITY) == nullptr)
    {
        HciTrace("fatal: status thread create\r\n");
        HciFatal();
    }

    HciTrace("boot: starting scheduler\r\n");
    TaktOSStart();
}
