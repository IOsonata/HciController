/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_nrf52840.h"

#include "hci_sdc_resources.h"

#include <stdint.h>
#include <string.h>

#include "nrf.h"

#if defined(__cplusplus) && !defined(restrict)
#define restrict __restrict
#include "mpsl.h"
#include "mpsl_clock.h"
#undef restrict
#else
#include "mpsl.h"
#include "mpsl_clock.h"
#endif

#include "sdc.h"
#include "sdc_soc.h"
#include "tusb.h"
#include "device/dcd.h"
#include "nrf_erratas.h"

#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"
#include "hci_trace.h"

#ifndef HCI_NRF52840_LOW_IRQ_PRIORITY
#define HCI_NRF52840_LOW_IRQ_PRIORITY 7U
#endif

#ifndef HCI_NRF52840_CLOCK_IRQ_PRIORITY
#define HCI_NRF52840_CLOCK_IRQ_PRIORITY 4U
#endif

#ifndef HCI_NRF52840_USB_IRQ_PRIORITY
#define HCI_NRF52840_USB_IRQ_PRIORITY 7U
#endif

/* Bounded wait for the crystal. Worst case ramp-up is 1400 us. */
#ifndef HCI_NRF52840_HFCLK_WAIT_LOOPS
#define HCI_NRF52840_HFCLK_WAIT_LOOPS 1000000U
#endif

/*
 * Bounded wait for the USB 3.3 V regulator. POWER and CLOCK are one peripheral
 * on this part, NRF_POWER_BASE and NRF_CLOCK_BASE are both 0x40000000, so they
 * share one INTENSET and one interrupt line that MPSL owns. USBPWRRDY cannot be
 * relied on, so the status is polled instead. Settling is a few milliseconds.
 */
#ifndef HCI_NRF52840_USBREG_WAIT_LOOPS
#define HCI_NRF52840_USBREG_WAIT_LOOPS 2000000U
#endif

#define HCI_NRF52840_USB_INT_MASK (POWER_INTENSET_USBDETECTED_Msk | \
                                   POWER_INTENSET_USBREMOVED_Msk | \
                                   POWER_INTENSET_USBPWRRDY_Msk)

/*
 * EVENTS_USBEVENT is driven from EVENTCAUSE, so it re-asserts as soon as it is
 * cleared while any cause bit is still set. The TinyUSB nRF5x port clears only
 * these four, so every other cause, READY in particular, produces an interrupt
 * that never stops.
 */
#define HCI_NRF52840_USBD_PORT_EVENTCAUSE (USBD_EVENTCAUSE_SUSPEND_Msk | \
                                           USBD_EVENTCAUSE_RESUME_Msk | \
                                           USBD_EVENTCAUSE_USBWUALLOWED_Msk | \
                                           USBD_EVENTCAUSE_ISOOUTCRC_Msk)

/*
 * EVENTS_USBRESET through EVENTS_EPDATA are contiguous and match the INTEN bit
 * order, so a pending mask can be built by walking them. Used to name whatever
 * event is re-asserting when the handler will not stop.
 */
#define HCI_NRF52840_USBD_EVT_COUNT 25U

/* Interrupts within one pump pass before the state is captured. */
#ifndef HCI_NRF52840_USB_STORM_LIMIT
#define HCI_NRF52840_USB_STORM_LIMIT 2000U
#endif

/* A nonzero latch even when the peripheral exposes no individual event bit. */
#define HCI_NRF52840_USB_STORM_UNKNOWN 0x80000000UL

/*
 * Attempts allowed when proving the entropy source at start up. This bounds a
 * start up check, not the SDC callback, which must block per sdc_soc.h.
 */
#ifndef HCI_NRF52840_RAND_PROBE_LOOPS
#define HCI_NRF52840_RAND_PROBE_LOOPS 1000U
#endif

#define HCI_NRF52840_ERR_HFCLK_TIMEOUT      (-1000)
#define HCI_NRF52840_ERR_USBREG_TIMEOUT     (-1001)
#define HCI_NRF52840_ERR_USBD_READY_TIMEOUT (-1002)
#define HCI_NRF52840_ERR_HFXO_NOT_XTAL      (-1003)
/* Cable absent is not the same fault as a regulator that never came up. */
#define HCI_NRF52840_ERR_NO_VBUS            (-1004)
/* The entropy source SDC requires is absent or produces nothing. */
#define HCI_NRF52840_ERR_NO_ENTROPY         (-1005)
/* An interrupt source re-asserted faster than the runtime could consume it. */
#define HCI_NRF52840_ERR_USB_STORM          (-1006)

/*
 * The crystal check the TinyUSB nRF5x port spins on. MPSL reporting the clock
 * as running is its own bookkeeping, which is not the same as the hardware
 * having switched HFCLKSTAT.SRC to the crystal.
 */
static bool HciNrf52840HfxoOnXtal(void)
{
    uint32_t stat = NRF_CLOCK->HFCLKSTAT;

    return (stat & CLOCK_HFCLKSTAT_STATE_Msk) != 0U &&
           ((stat & CLOCK_HFCLKSTAT_SRC_Msk) >> CLOCK_HFCLKSTAT_SRC_Pos) ==
               CLOCK_HFCLKSTAT_SRC_Xtal;
}

static HciNrf52840_t *s_pTarget;

/*
 * MPSL/SDC assert callbacks return to the library, which then resets the part.
 * Ordinary target state is in .bss and disappears during ResetEntry, so keep a
 * small assertion record outside .bss. The IOsonata reset code clears only the
 * .bss range; this NOLOAD/NOBITS input section therefore survives a software
 * reset and is reported once on the next boot.
 *
 * A magic and its inverse make random SRAM after a power-on extraordinarily
 * unlikely to look like a retained assertion. The magic is written last, after
 * the payload and a data barrier, so an interrupted write is rejected too.
 */
#define HCI_NRF52840_ASSERT_MAGIC     0x48434941U
#define HCI_NRF52840_ASSERT_FILE_SIZE 48U

typedef struct {
    uint32_t Magic;
    uint32_t MagicInverse;
    uint32_t Line;
    uint8_t FromSdc;
    char File[HCI_NRF52840_ASSERT_FILE_SIZE];
} HciNrf52840AssertRecord_t;

/*
 * The target linker/startup uses an ELF NOLOAD/NOBITS input section. Native
 * macOS tests use Mach-O, where the ELF section spelling is invalid and reset
 * retention is not part of the host test. Keep the object alive there without
 * changing the nRF52840 image layout.
 */
#if defined(__APPLE__)
#define HCI_NRF52840_ASSERT_STORAGE __attribute__((used))
#else
#define HCI_NRF52840_ASSERT_STORAGE \
    __attribute__((section(".noinit.hci_reset"), used))
#endif

alignas(4) HCI_NRF52840_ASSERT_STORAGE
static volatile HciNrf52840AssertRecord_t s_AssertRecord;

/*
 * The USBD startup sequence is driven here rather than through
 * tusb_hal_nrf_power_event. That helper calls hfclk_enable() on both its
 * detected and its ready path, and the clock belongs to MPSL on this target.
 *
 * The sequence and the errata workarounds follow the nRF52840 product
 * specification USBD startup sequence and errata 187, 171 and 166.
 */
#ifndef HCI_NRF52840_REG32
#define HCI_NRF52840_REG32(Addr) (*(volatile uint32_t *)(uintptr_t)(Addr))
#endif

#define HCI_NRF52840_ERRATA_UNLOCK_REG 0x4006EC00UL
#define HCI_NRF52840_ERRATA_UNLOCK_KEY 0x00009375UL
#define HCI_NRF52840_ERRATA_171_REG    0x4006EC14UL
#define HCI_NRF52840_ERRATA_187_REG    0x4006ED14UL
#define HCI_NRF52840_ERRATA_166_REG_A  (NRF_USBD_BASE + 0x800UL)
#define HCI_NRF52840_ERRATA_166_REG_B  (NRF_USBD_BASE + 0x804UL)

static void HciNrf52840ErrataWrite(uint32_t Reg, uint32_t Value)
{
    if (HCI_NRF52840_REG32(HCI_NRF52840_ERRATA_UNLOCK_REG) == 0x00000000UL)
    {
        HCI_NRF52840_REG32(HCI_NRF52840_ERRATA_UNLOCK_REG) =
            HCI_NRF52840_ERRATA_UNLOCK_KEY;
        HCI_NRF52840_REG32(Reg) = Value;
        HCI_NRF52840_REG32(HCI_NRF52840_ERRATA_UNLOCK_REG) =
            HCI_NRF52840_ERRATA_UNLOCK_KEY;
    }
    else
    {
        HCI_NRF52840_REG32(Reg) = Value;
    }
}

/*
 * The cable event flags are written by POWER_CLOCK and read by the runtime
 * thread, so the read and clear must be one operation. PRIMASK is used rather
 * than the TaktOS critical section because this file must stay usable before
 * the runtime exists.
 */
static uint32_t HciNrf52840EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void HciNrf52840ExitCritical(uint32_t State)
{
    __set_PRIMASK(State);
}

static void HciNrf52840UsbdErrataApply(void)
{
    if (nrf52_errata_187())
    {
        HciNrf52840ErrataWrite(HCI_NRF52840_ERRATA_187_REG, 0x00000003UL);
    }

    if (nrf52_errata_171())
    {
        HciNrf52840ErrataWrite(HCI_NRF52840_ERRATA_171_REG, 0x000000C0UL);
    }
}

static void HciNrf52840UsbdErrataRevert(void)
{
    if (nrf52_errata_171())
    {
        HciNrf52840ErrataWrite(HCI_NRF52840_ERRATA_171_REG, 0x00000000UL);
    }

    if (nrf52_errata_187())
    {
        HciNrf52840ErrataWrite(HCI_NRF52840_ERRATA_187_REG, 0x00000000UL);
    }

    if (nrf52_errata_166())
    {
        HCI_NRF52840_REG32(HCI_NRF52840_ERRATA_166_REG_A) = 0x7E3UL;
        HCI_NRF52840_REG32(HCI_NRF52840_ERRATA_166_REG_B) = 0x40UL;
        __ISB();
        __DSB();
    }
}

/*
 * MPSL owns NRF_CLOCK on this target, so the crystal is taken from MPSL and
 * held for as long as USB is up. Nothing here starts or stops it.
 */
/*
 * mpsl_clock_hfclk_request, _is_running and _release carry
 * __attribute__((deprecated)) and "This function will be removed in a future
 * release", so only the _src_ API is used here.
 */
static void HciNrf52840HfclkStarted(mpsl_clock_evt_type_t EvtType)
{
    (void)EvtType;
}

static int32_t HciNrf52840HfclkRequest(void)
{
    return mpsl_clock_hfclk_src_request(MPSL_CLOCK_HF_SRC_XO,
                                        HciNrf52840HfclkStarted);
}

static int32_t HciNrf52840HfclkIsRunning(uint32_t *pRunning)
{
    return mpsl_clock_hfclk_src_is_running(MPSL_CLOCK_HF_SRC_XO, pRunning);
}

static int32_t HciNrf52840HfclkRelease(void)
{
    return mpsl_clock_hfclk_src_release(MPSL_CLOCK_HF_SRC_XO);
}

/*
 * A successful request belongs to this target until USB starts or the request
 * is explicitly released. An error while waiting for the clock must therefore
 * unwind the request as well as report the error; otherwise a retry sees
 * HfclkRequested and incorrectly assumes the crystal is still owned.
 */
static bool HciNrf52840HfclkStartFailed(HciNrf52840_t *pTarget,
                                        int32_t Error)
{
    (void)HciNrf52840HfclkRelease();
    pTarget->HfclkRequested = false;
    pTarget->LastError = Error;
    return false;
}

static bool HciNrf52840HfclkStart(HciNrf52840_t *pTarget)
{
    if (pTarget->HfclkRequested)
    {
        return true;
    }

    int32_t result = HciNrf52840HfclkRequest();
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    pTarget->HfclkRequested = true;
    HciTrace("hfclk: requested\r\n");

    for (uint32_t loop = 0U; loop < HCI_NRF52840_HFCLK_WAIT_LOOPS; loop++)
    {
        /*
         * The started callback runs from low priority processing, so pump it
         * here while polling. This runs in the same context that later calls
         * HciNrf52840ProcessMpsl.
         */
        mpsl_low_priority_process();

        uint32_t running = 0U;
        result = HciNrf52840HfclkIsRunning(&running);
        if (result != 0)
        {
            return HciNrf52840HfclkStartFailed(pTarget, result);
        }

        if (running != 0U)
        {
            HciTrace("hfclk: mpsl running after %lu polls hfclkstat=0x%08lX\r\n",
                     (unsigned long)loop + 1UL,
                     (unsigned long)NRF_CLOCK->HFCLKSTAT);
            return true;
        }
    }

    HciTrace("hfclk: timeout\r\n");
    return HciNrf52840HfclkStartFailed(
        pTarget, HCI_NRF52840_ERR_HFCLK_TIMEOUT);
}

/*
 * Finish shutting down a USB port that the ISR already contained. Clock
 * ownership belongs to MPSL and is therefore unwound from thread context, not
 * from USBD_IRQHandler.
 */
static void HciNrf52840UsbAbort(HciNrf52840_t *pTarget, int32_t Error)
{
    NVIC_DisableIRQ(USBD_IRQn);
    NRF_USBD->INTEN = 0U;
    NRF_USBD->USBPULLUP = 0U;
    NRF_USBD->ENABLE = 0U;
    __ISB();
    __DSB();

    NRF_POWER->INTENCLR = HCI_NRF52840_USB_INT_MASK;

    if (pTarget->HfclkRequested)
    {
        (void)HciNrf52840HfclkRelease();
        pTarget->HfclkRequested = false;
    }

    pTarget->UsbStarted = false;
    pTarget->UsbReadyDone = false;
    pTarget->UsbAttachPending = false;
    pTarget->UsbDetachPending = false;
    pTarget->LastError = Error;
}

/*
 * Record the cable event and leave. dcd_connect and dcd_disconnect must not
 * run from here.
 *
 * dcd_disconnect pushes into the TinyUSB event queue, and under CFG_TUSB_OS
 * OPT_OS_NONE the only mutual exclusion on that queue is masking USBD_IRQn.
 * This handler shares POWER_CLOCK with MPSL and runs above the thread that
 * calls tud_task, so that mask does not hold it off: it would write the queue
 * while tud_task is reading it, and on the way out it would re-enable an
 * interrupt the thread had deliberately masked.
 *
 * The flags are acted on in HciNrf52840UsbPowerProcess, in the same context as
 * tud_task, where the mask means what TinyUSB expects it to mean.
 */
static void HciNrf52840UsbPowerIrq(void)
{
    bool wake = false;

    if (NRF_POWER->EVENTS_USBDETECTED != 0U)
    {
        NRF_POWER->EVENTS_USBDETECTED = 0U;
        s_pTarget->UsbAttachPending = true;
        s_pTarget->UsbAttachCount++;
        wake = true;
    }

    if (NRF_POWER->EVENTS_USBREMOVED != 0U)
    {
        NRF_POWER->EVENTS_USBREMOVED = 0U;
        s_pTarget->UsbDetachPending = true;
        s_pTarget->UsbDetachCount++;
        wake = true;
    }

    if (NRF_POWER->EVENTS_USBPWRRDY != 0U)
    {
        NRF_POWER->EVENTS_USBPWRRDY = 0U;
    }

    if (wake && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_HOST);
    }
}

/*
 * Apply a cable event recorded by the interrupt handler. Runs in the thread
 * that pumps tud_task, so the TinyUSB queue is protected the way TinyUSB
 * assumes. A detach followed by an attach before this runs collapses to an
 * attach, which is the state the hardware is actually in.
 */
void HciNrf52840UsbPowerProcess(HciNrf52840_t *pTarget)
{
    if (pTarget == nullptr)
    {
        return;
    }

    /*
     * A storm was already made harmless in the ISR by taking the pull-up and
     * interrupt away. Complete the shutdown here so the MPSL-owned crystal is
     * released and the failed port cannot be mistaken for a usable one.
     */
    if (pTarget->UsbStormEvents != 0U)
    {
        HciNrf52840UsbAbort(pTarget, HCI_NRF52840_ERR_USB_STORM);
        return;
    }

    if (!pTarget->UsbReadyDone)
    {
        return;
    }

    uint32_t state = HciNrf52840EnterCritical();
    const bool attach = pTarget->UsbAttachPending;
    const bool detach = pTarget->UsbDetachPending;
    pTarget->UsbAttachPending = false;
    pTarget->UsbDetachPending = false;
    HciNrf52840ExitCritical(state);

    if (detach && !attach)
    {
        dcd_disconnect(0U);
    }
    else if (attach)
    {
        if (detach)
        {
            dcd_disconnect(0U);
        }
        dcd_connect(0U);
    }
}

static void HciNrf52840AssertFileCopy(const char *pFile)
{
    const char *pBase = pFile != nullptr ? pFile : "?";
    if (pFile != nullptr)
    {
        for (const char *p = pFile; *p != '\0'; p++)
        {
            if (*p == '/' || *p == '\\')
            {
                pBase = p + 1;
            }
        }
    }

    size_t i = 0U;
    while (pBase[i] != '\0' && i + 1U < HCI_NRF52840_ASSERT_FILE_SIZE)
    {
        s_AssertRecord.File[i] = pBase[i];
        i++;
    }
    s_AssertRecord.File[i] = '\0';
}

/*
 * mpsl.h and sdc.h both state that the library resets the chip after the
 * application returns from this callback. Store enough information outside
 * .bss to explain that next reset instead of losing the only useful evidence.
 */
static void HciNrf52840RecordAssert(const char *pFile,
                                    uint32_t Line,
                                    bool FromSdc)
{
    s_AssertRecord.Magic = 0U;
    s_AssertRecord.MagicInverse = 0U;
    s_AssertRecord.Line = Line;
    s_AssertRecord.FromSdc = FromSdc ? 1U : 0U;
    HciNrf52840AssertFileCopy(pFile);
    __DSB();
    s_AssertRecord.MagicInverse = ~HCI_NRF52840_ASSERT_MAGIC;
    s_AssertRecord.Magic = HCI_NRF52840_ASSERT_MAGIC;
    __DSB();
}

static void HciNrf52840MpslAssert(const char *file, uint32_t line)
{
    HciNrf52840RecordAssert(file, line, false);
    /* Return, so MPSL resets the chip. */
}

static void HciNrf52840SdcAssert(const char *file, uint32_t line)
{
    HciNrf52840RecordAssert(file, line, true);
    /* Return, so the controller resets the chip. */
}

static void HciNrf52840ResetCauseAppend(char *pText,
                                        size_t Capacity,
                                        const char *pCause)
{
    size_t at = strlen(pText);
    if (at != 0U && at + 1U < Capacity)
    {
        pText[at++] = '|';
        pText[at] = '\0';
    }

    while (*pCause != '\0' && at + 1U < Capacity)
    {
        pText[at++] = *pCause++;
    }
    pText[at] = '\0';
}

void HciNrf52840ResetTrace(void)
{
    const uint32_t reason = NRF_POWER->RESETREAS;
    char causes[96] = {};

    /*
     * nRF52840 anomaly 136 can set unrelated RESETREAS bits after a pin reset.
     * If RESETPIN is present, report that as the cause and leave the raw value
     * alongside it rather than presenting spurious secondary causes as fact.
     */
    if ((reason & POWER_RESETREAS_RESETPIN_Msk) != 0U)
    {
        HciNrf52840ResetCauseAppend(causes, sizeof(causes), "pin-reset");
    }
    else
    {
        if ((reason & POWER_RESETREAS_DOG_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "watchdog");
        if ((reason & POWER_RESETREAS_SREQ_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "software-reset");
        if ((reason & POWER_RESETREAS_LOCKUP_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "CPU-lockup");
        if ((reason & POWER_RESETREAS_OFF_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "GPIO-wake-from-off");
        if ((reason & POWER_RESETREAS_LPCOMP_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "LPCOMP-wake-from-off");
        if ((reason & POWER_RESETREAS_DIF_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "debug-wake-from-off");
        if ((reason & POWER_RESETREAS_NFC_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "NFC-wake-from-off");
        if ((reason & POWER_RESETREAS_VBUS_Msk) != 0U)
            HciNrf52840ResetCauseAppend(causes, sizeof(causes), "VBUS-wake-from-off");
    }

    if (causes[0] == '\0')
    {
        HciTrace("reset: cause=power-on/brownout-or-unlatched raw=0x%08lX\r\n",
                 (unsigned long)reason);
    }
    else
    {
        HciTrace("reset: cause=%s raw=0x%08lX\r\n",
                 causes, (unsigned long)reason);
    }

    /* RESETREAS is write-one-to-clear. Start the next reset with a clean slate. */
    NRF_POWER->RESETREAS = reason;

    const bool retained =
        s_AssertRecord.Magic == HCI_NRF52840_ASSERT_MAGIC &&
        s_AssertRecord.MagicInverse == ~HCI_NRF52840_ASSERT_MAGIC;

    if (retained)
    {
        char file[HCI_NRF52840_ASSERT_FILE_SIZE];
        size_t i = 0U;
        while (i + 1U < sizeof(file) && s_AssertRecord.File[i] != '\0')
        {
            file[i] = s_AssertRecord.File[i];
            i++;
        }
        file[i] = '\0';

        HciTrace("reset: previous=%s-assert file=%s line=%lu\r\n",
                 s_AssertRecord.FromSdc != 0U ? "SDC" : "MPSL",
                 file,
                 (unsigned long)s_AssertRecord.Line);

        s_AssertRecord.Magic = 0U;
        s_AssertRecord.MagicInverse = 0U;
        __DSB();
    }
}

/*
 * The entropy source SDC uses for pairing and for resolvable addresses.
 *
 * sdc_soc.h states the requirement without an escape: "This function must block
 * until length bytes of random numbers were written to p_buff", and the source
 * "must conform to Core Spec Vol 2, Part H, Section 2". The signature returns
 * void because failure is not a permitted outcome. Handing back zeros would
 * give SDC a predictable session diversifier or a predictable resolvable
 * address with no way to detect it, so this blocks as documented.
 *
 * The case that used to justify bounding this, a source that is absent or
 * permanently broken, is caught in HciNrf52840RandSourceReady before the
 * controller is ever enabled. By the time SDC calls this, the source has
 * already produced bytes once.
 */
static void HciNrf52840RandPoll(uint8_t *pBuffer, uint8_t Length)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();

    while (rng == nullptr)
    {
        /*
         * Unreachable unless the instance disappeared after start up. There is
         * no legal way to return, so retry rather than fabricate entropy.
         */
        if (s_pTarget != nullptr)
        {
            s_pTarget->RandRetryCount++;
        }
        rng = CryptoRngNrfInstance();
    }

    while (rng->Random(pBuffer, Length) != CRYPTO_STATUS_OK)
    {
        if (s_pTarget != nullptr)
        {
            s_pTarget->RandRetryCount++;
        }
    }
}

/*
 * Prove the entropy source works before the controller is enabled, so a broken
 * source is a clean start up failure rather than a stall inside SDC later.
 */
static bool HciNrf52840RandSourceReady(HciNrf52840_t *pTarget)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();

    if (rng == nullptr)
    {
        HciTrace("rand: no source instance\r\n");
        pTarget->LastError = HCI_NRF52840_ERR_NO_ENTROPY;
        return false;
    }

    uint8_t probe[8];
    for (uint32_t loop = 0U; loop < HCI_NRF52840_RAND_PROBE_LOOPS; loop++)
    {
        if (rng->Random(probe, sizeof(probe)) == CRYPTO_STATUS_OK)
        {
            return true;
        }
    }

    HciTrace("rand: source did not produce bytes\r\n");
    pTarget->LastError = HCI_NRF52840_ERR_NO_ENTROPY;
    return false;
}

static void HciNrf52840SdcCallback(void)
{
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_SDC);
    }
}

static bool HciNrf52840MpslInit(HciNrf52840_t *pTarget)
{
    mpsl_clock_lfclk_cfg_t lfclk = {};
    const OscDesc_t *lfosc = GetLowFreqOscDesc();

    if (lfosc->Type == OSC_TYPE_RC)
    {
        lfclk.source = MPSL_CLOCK_LF_SRC_RC;
        lfclk.rc_ctiv = MPSL_RECOMMENDED_RC_CTIV;
        lfclk.rc_temp_ctiv = MPSL_RECOMMENDED_RC_TEMP_CTIV;
        lfclk.accuracy_ppm = MPSL_DEFAULT_CLOCK_ACCURACY_PPM;
    }
    else
    {
        lfclk.source = MPSL_CLOCK_LF_SRC_XTAL;
        lfclk.accuracy_ppm = (uint16_t)lfosc->Accuracy;
    }
    lfclk.skip_wait_lfclk_started = MPSL_DEFAULT_SKIP_WAIT_LFCLK_STARTED;

    HciTrace("mpsl: lfsrc=%u ppm=%u\r\n",
             (unsigned)lfclk.source, (unsigned)lfclk.accuracy_ppm);

    int32_t result = mpsl_init(&lfclk, SWI5_EGU5_IRQn, HciNrf52840MpslAssert);
    HciTrace("mpsl: init=%ld\r\n", (long)result);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    NVIC_SetPriority(RADIO_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(RTC0_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(TIMER0_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(POWER_CLOCK_IRQn, HCI_NRF52840_CLOCK_IRQ_PRIORITY);
    NVIC_SetPriority(SWI5_EGU5_IRQn, HCI_NRF52840_LOW_IRQ_PRIORITY);

    NVIC_EnableIRQ(RADIO_IRQn);
    NVIC_EnableIRQ(RTC0_IRQn);
    NVIC_EnableIRQ(TIMER0_IRQn);
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);
    NVIC_EnableIRQ(SWI5_EGU5_IRQn);

    pTarget->MpslInitialized = true;
    /*
     * USB bring up is deliberately not done here. It has to run after the USB
     * device stack is initialised, see HciNrf52840UsbStart.
     */
    return true;
}

static bool HciNrf52840SdcInit(HciNrf52840_t *pTarget)
{
    int32_t result = sdc_init(HciNrf52840SdcAssert);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->SdcInitialized = true;

    if (!HciNrf52840RandSourceReady(pTarget))
    {
        return false;
    }

    sdc_rand_source_t randomSource = { HciNrf52840RandPoll };
    result = sdc_rand_source_register(&randomSource);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    /*
     * What the controller is configured for is not a property of this part,
     * so it lives in hci_sdc_resources.cpp with the pool that is computed from
     * it. This layer supplies the clock, the entropy and the host interface.
     */
    result = HciSdcResourcesApply();
    if (result < 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->RequiredSdcMem = result;

    HciTrace("sdc: required=%ld capacity=%lu align=%lu\r\n",
             (long)result,
             (unsigned long)pTarget->SdcMemCapacity,
             (unsigned long)((uintptr_t)pTarget->pSdcMem & 7U));

    if ((size_t)result > pTarget->SdcMemCapacity ||
        (((uintptr_t)pTarget->pSdcMem & 7U) != 0U))
    {
        HciTrace("sdc: memory pool too small or misaligned\r\n");
        pTarget->LastError = -1;
        return false;
    }

    result = sdc_enable(HciNrf52840SdcCallback, pTarget->pSdcMem);
    HciTrace("sdc: enable=%ld\r\n", (long)result);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    pTarget->SdcEnabled = true;
    return true;
}

static bool HciNrf52840Start(void *pContext)
{
    HciNrf52840_t *pTarget = static_cast<HciNrf52840_t *>(pContext);
    if (pTarget == nullptr || pTarget->MpslInitialized || pTarget->SdcInitialized ||
        pTarget->SdcEnabled ||
        (s_pTarget != nullptr && s_pTarget != pTarget))
    {
        return false;
    }

    s_pTarget = pTarget;
    if (!HciNrf52840MpslInit(pTarget))
    {
        s_pTarget = nullptr;
        return false;
    }

    if (!HciNrf52840SdcInit(pTarget))
    {
        /*
         * MPSL is up with five interrupts enabled at this point. Returning
         * without undoing it leaves the radio subsystem live and nothing
         * servicing it, because the runtime treats a failed start as fatal.
         */
        HciNrf52840Stop(pTarget);
        return false;
    }

    return true;
}

static void HciNrf52840ProcessMpsl(void *pContext)
{
    mpsl_low_priority_process();

    /*
     * POWER and CLOCK are one peripheral and MPSL owns it, so it can drop the
     * USB bits from the shared enable register. Put them back each pass, unless
     * a storm deliberately quenched the port.
     */
    HciNrf52840_t *pTarget = static_cast<HciNrf52840_t *>(pContext);
    if (pTarget != nullptr && pTarget->UsbStarted &&
        pTarget->UsbStormEvents == 0U)
    {
        NRF_POWER->INTENSET = HCI_NRF52840_USB_INT_MASK;
    }
}

static void HciNrf52840Fault(void *pContext, int Error)
{
    HciNrf52840_t *pTarget = static_cast<HciNrf52840_t *>(pContext);
    if (pTarget != nullptr)
    {
        pTarget->FaultCount++;
        /* Keep the first recorded cause. The generic layer reports -1 only. */
        if (pTarget->LastError == 0)
        {
            pTarget->LastError = Error;
        }
    }
}

/*
 * Undo everything HciNrf52840UsbStart put in place, for a failure between the
 * crystal request and the point where the port is live. Every step is
 * conditional on having been taken, so this is safe from any failure point.
 */
static bool HciNrf52840UsbStartFailed(HciNrf52840_t *pTarget,
                                      int32_t Error,
                                      bool ErrataApplied)
{
    if (ErrataApplied)
    {
        HciNrf52840UsbdErrataRevert();
    }

    NRF_USBD->INTEN = 0U;
    NRF_USBD->USBPULLUP = 0U;
    NRF_USBD->ENABLE = 0U;
    __ISB();
    __DSB();

    NVIC_DisableIRQ(USBD_IRQn);
    NRF_POWER->INTENCLR = HCI_NRF52840_USB_INT_MASK;

    /*
     * The crystal was requested for USB alone. Holding it after a failed bring
     * up keeps MPSL from ever dropping to the internal oscillator between
     * radio events, for the rest of the boot.
     */
    (void)HciNrf52840HfclkRelease();
    pTarget->HfclkRequested = false;

    pTarget->UsbStarted = false;
    pTarget->UsbReadyDone = false;
    pTarget->LastError = Error;
    return false;
}

bool HciNrf52840UsbStart(HciNrf52840_t *pTarget)
{
    if (pTarget == nullptr || pTarget != s_pTarget || !pTarget->UsbEnabled ||
        !pTarget->MpslInitialized)
    {
        return false;
    }

    /*
     * A storm is latched until target reinitialization. Re-enabling the same
     * peripheral immediately would reproduce the interrupt starvation before
     * the runtime had a chance to report or contain anything.
     */
    if (pTarget->UsbStormEvents != 0U)
    {
        pTarget->LastError = HCI_NRF52840_ERR_USB_STORM;
        return false;
    }

    if (pTarget->UsbStarted)
    {
        return true;
    }

    if (!HciNrf52840HfclkStart(pTarget))
    {
        return false;
    }

    NRF_POWER->EVENTS_USBDETECTED = 0U;
    NRF_POWER->EVENTS_USBREMOVED = 0U;
    NRF_POWER->EVENTS_USBPWRRDY = 0U;
    NRF_POWER->INTENSET = HCI_NRF52840_USB_INT_MASK;

    NVIC_SetPriority(USBD_IRQn, HCI_NRF52840_USB_IRQ_PRIORITY);

    uint32_t status = NRF_POWER->USBREGSTATUS;
    HciTrace("usb: start usbregstatus=0x%08lX inten=0x%08lX\r\n",
             (unsigned long)status, (unsigned long)NRF_POWER->INTENSET);

    if ((status & POWER_USBREGSTATUS_VBUSDETECT_Msk) == 0U)
    {
        HciTrace("usb: no vbus\r\n");
        return HciNrf52840UsbStartFailed(pTarget, HCI_NRF52840_ERR_NO_VBUS,
                                         false);
    }

    /*
     * Controller enable. The errata 187 and 171 workarounds go on before
     * ENABLE and come back off once the controller reports ready.
     */
    NRF_USBD->EVENTCAUSE = USBD_EVENTCAUSE_READY_Msk;
    __ISB();
    __DSB();

    HciNrf52840UsbdErrataApply();

    NRF_USBD->ENABLE = 1U;
    __ISB();
    __DSB();

    HciTrace("usb: enable set hfclkstat=0x%08lX\r\n",
             (unsigned long)NRF_CLOCK->HFCLKSTAT);

    uint32_t loop = 0U;
    while ((NRF_USBD->EVENTCAUSE & USBD_EVENTCAUSE_READY_Msk) == 0U &&
           loop < HCI_NRF52840_USBREG_WAIT_LOOPS)
    {
        /*
         * MPSL and the radio are already running by this point, and this wait
         * can take its full budget on a bad supply. Low priority processing
         * has a deadline of a few hundred milliseconds, so it is pumped here
         * exactly as in the crystal wait above.
         */
        mpsl_low_priority_process();
        loop++;
    }

    if ((NRF_USBD->EVENTCAUSE & USBD_EVENTCAUSE_READY_Msk) == 0U)
    {
        HciTrace("usb: controller ready timeout eventcause=0x%08lX\r\n",
                 (unsigned long)NRF_USBD->EVENTCAUSE);
        return HciNrf52840UsbStartFailed(pTarget,
                                         HCI_NRF52840_ERR_USBD_READY_TIMEOUT,
                                         true);
    }

    NRF_USBD->EVENTCAUSE = USBD_EVENTCAUSE_READY_Msk;
    __ISB();
    __DSB();

    HciTrace("usb: controller ready after %lu polls\r\n", (unsigned long)loop);

    HciNrf52840UsbdErrataRevert();

    NRF_USBD->ISOSPLIT = USBD_ISOSPLIT_SPLIT_HalfIN;

    /*
     * Leave no cause behind. Anything still set here would re-assert
     * EVENTS_USBEVENT the moment the port enables that interrupt.
     */
    NRF_USBD->EVENTCAUSE = NRF_USBD->EVENTCAUSE;
    NRF_USBD->EVENTS_USBEVENT = 0U;
    __ISB();
    __DSB();

    NRF_USBD->INTENSET = USBD_INTEN_USBRESET_Msk;

    NVIC_ClearPendingIRQ(USBD_IRQn);
    NVIC_EnableIRQ(USBD_IRQn);

    /*
     * Poll the 3.3 V regulator rather than wait for USBPWRRDY. That event
     * shares its enable bit and its interrupt line with CLOCK, which MPSL
     * owns. The pull up must not go up before the supply is ready.
     */
    loop = 0U;
    while ((status & POWER_USBREGSTATUS_OUTPUTRDY_Msk) == 0U &&
           loop < HCI_NRF52840_USBREG_WAIT_LOOPS)
    {
        mpsl_low_priority_process();
        status = NRF_POWER->USBREGSTATUS;
        loop++;
    }

    if ((status & POWER_USBREGSTATUS_OUTPUTRDY_Msk) == 0U)
    {
        HciTrace("usb: regulator timeout usbregstatus=0x%08lX\r\n",
                 (unsigned long)status);
        return HciNrf52840UsbStartFailed(pTarget,
                                         HCI_NRF52840_ERR_USBREG_TIMEOUT,
                                         false);
    }

    HciTrace("usb: outrdy after %lu polls\r\n", (unsigned long)loop);

    /*
     * The crystal has to be up before the pull up and it comes from MPSL.
     * Report a mismatch rather than starting it here.
     */
    if (!HciNrf52840HfxoOnXtal())
    {
        HciTrace("usb: hfxo not on crystal hfclkstat=0x%08lX\r\n",
                 (unsigned long)NRF_CLOCK->HFCLKSTAT);
        return HciNrf52840UsbStartFailed(pTarget,
                                         HCI_NRF52840_ERR_HFXO_NOT_XTAL,
                                         false);
    }

    NRF_USBD->USBPULLUP = 1U;
    __ISB();
    __DSB();

    /*
     * Set only now that the port is genuinely up. Anything earlier makes a
     * failed bring up look like a success to the interrupt handler, to
     * HciNrf52840Stop and to a caller that retries.
     */
    pTarget->UsbStarted = true;
    pTarget->UsbReadyDone = true;

    /* MPSL shares the enable register, so put the USB bits back. */
    NRF_POWER->INTENSET = HCI_NRF52840_USB_INT_MASK;

    HciTrace("usb: started pullup=%lu enable=%lu hfclkstat=0x%08lX inten=0x%08lX\r\n",
             (unsigned long)NRF_USBD->USBPULLUP,
             (unsigned long)NRF_USBD->ENABLE,
             (unsigned long)NRF_CLOCK->HFCLKSTAT,
             (unsigned long)NRF_POWER->INTENSET);

    return true;
}

bool HciNrf52840Init(HciNrf52840_t *pTarget,
                     HciTaktOs_t *pRuntime,
                     uint8_t *pSdcMem,
                     size_t SdcMemCapacity,
                     bool UsbEnabled)
{
    if (pTarget == nullptr || pRuntime == nullptr || pSdcMem == nullptr ||
        SdcMemCapacity == 0U || (((uintptr_t)pSdcMem & 7U) != 0U))
    {
        return false;
    }

    memset(pTarget, 0, sizeof(*pTarget));
    pTarget->pRuntime = pRuntime;
    pTarget->pSdcMem = pSdcMem;
    pTarget->SdcMemCapacity = SdcMemCapacity;
    pTarget->UsbEnabled = UsbEnabled;
    return true;
}

void HciNrf52840GetTaktOsOps(HciNrf52840_t *pTarget,
                             HciTaktOsOps_t *pOps)
{
    if (pOps == nullptr)
    {
        return;
    }

    pOps->Start = HciNrf52840Start;
    pOps->ProcessMpsl = HciNrf52840ProcessMpsl;
    pOps->Fault = HciNrf52840Fault;
    pOps->pContext = pTarget;
}

void HciNrf52840Stop(HciNrf52840_t *pTarget)
{
    if (pTarget == nullptr || pTarget != s_pTarget)
    {
        return;
    }

    /*
     * Detach cleanly first. Leaving the controller enabled with the pull up
     * asserted keeps the host enumerated against a device that answers
     * nothing, and the crystal it needs is released further down. This is the
     * sequence the vendor port uses on cable removal.
     */
    if (pTarget->UsbEnabled)
    {
        NVIC_DisableIRQ(USBD_IRQn);
        NRF_POWER->INTENCLR = POWER_INTENCLR_USBDETECTED_Msk |
                              POWER_INTENCLR_USBREMOVED_Msk |
                              POWER_INTENCLR_USBPWRRDY_Msk;

        NRF_USBD->USBPULLUP = 0U;
        NRF_USBD->INTEN = 0U;
        NRF_USBD->ENABLE = 0U;
        __ISB();
        __DSB();

        pTarget->UsbStarted = false;
        pTarget->UsbReadyDone = false;
        pTarget->UsbAttachPending = false;
        pTarget->UsbDetachPending = false;
    }

    /*
     * The radio has to be stopped before MPSL is torn down, and sdc_disable
     * unwinds an in flight timeslot, which needs MPSL's RADIO and TIMER0
     * handlers to run. Masking them first makes it block or trip the MPSL
     * assert, so the interrupts stay live until both are down.
     */
    /*
     * mpsl.h: "All initialized protocol stacks need to be stopped before
     * calling this function. Failing to do so will lead to undefined
     * behavior." Initialized, not enabled. sdc_init can succeed and sdc_enable
     * fail afterwards, and that path used to reach mpsl_uninit with the
     * controller still initialised.
     */
    if (pTarget->SdcInitialized || pTarget->SdcEnabled)
    {
        (void)sdc_disable();
        pTarget->SdcEnabled = false;
    }

    if (pTarget->HfclkRequested)
    {
        (void)HciNrf52840HfclkRelease();
        pTarget->HfclkRequested = false;
    }

    if (pTarget->MpslInitialized)
    {
        mpsl_uninit();
        pTarget->MpslInitialized = false;
    }

    NVIC_DisableIRQ(SWI5_EGU5_IRQn);
    NVIC_DisableIRQ(RADIO_IRQn);
    NVIC_DisableIRQ(RTC0_IRQn);
    NVIC_DisableIRQ(TIMER0_IRQn);
    NVIC_DisableIRQ(POWER_CLOCK_IRQn);

    /*
     * Clear everything Start checks, so a target that was stopped can be
     * started again without going back through HciNrf52840Init.
     */
    pTarget->SdcInitialized = false;
    s_pTarget = nullptr;
}

extern "C" void SWI5_EGU5_IRQHandler(void)
{
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_MPSL);
    }
}

extern "C" void RADIO_IRQHandler(void)
{
    MPSL_IRQ_RADIO_Handler();
}

extern "C" void RTC0_IRQHandler(void)
{
    MPSL_IRQ_RTC0_Handler();
}

extern "C" void TIMER0_IRQHandler(void)
{
    MPSL_IRQ_TIMER0_Handler();
}

extern "C" void POWER_CLOCK_IRQHandler(void)
{
    MPSL_IRQ_CLOCK_Handler();
    if (s_pTarget != nullptr && s_pTarget->UsbEnabled)
    {
        HciNrf52840UsbPowerIrq();
    }
}

static uint32_t HciNrf52840UsbdPendingEvents(void)
{
    volatile uint32_t *pEvt = &NRF_USBD->EVENTS_USBRESET;
    uint32_t pending = 0U;

    for (uint32_t i = 0U; i < HCI_NRF52840_USBD_EVT_COUNT; i++)
    {
        if (pEvt[i] != 0U)
        {
            pending |= 1UL << i;
        }
    }

    return pending;
}

void HciNrf52840UsbPassMark(HciNrf52840_t *pTarget)
{
    if (pTarget != nullptr)
    {
        pTarget->UsbIrqMark = pTarget->UsbIrqCount;
    }
}

extern "C" void USBD_IRQHandler(void)
{
    if (s_pTarget != nullptr && s_pTarget->UsbStarted)
    {
        s_pTarget->UsbIrqCount++;

        /*
         * Too many entries inside one pump pass means an event is re-asserting
         * faster than it is being consumed. Capture what is pending, then
         * contain the source immediately. Logging a storm while leaving a
         * level-sensitive source enabled only guarantees this handler keeps
         * starving the thread that was supposed to report it.
         */
        if (s_pTarget->UsbStormEvents == 0U &&
            (s_pTarget->UsbIrqCount - s_pTarget->UsbIrqMark) >
                HCI_NRF52840_USB_STORM_LIMIT)
        {
            s_pTarget->UsbStormInten = NRF_USBD->INTEN;
            s_pTarget->UsbStormCause = NRF_USBD->EVENTCAUSE;
            uint32_t pending = HciNrf52840UsbdPendingEvents();
            s_pTarget->UsbStormEvents = pending != 0U ? pending :
                                        HCI_NRF52840_USB_STORM_UNKNOWN;
            s_pTarget->LastError = HCI_NRF52840_ERR_USB_STORM;

            NRF_USBD->INTEN = 0U;
            NRF_USBD->USBPULLUP = 0U;
            NRF_POWER->INTENCLR = HCI_NRF52840_USB_INT_MASK;
            NVIC_DisableIRQ(USBD_IRQn);
            s_pTarget->UsbStarted = false;
            s_pTarget->UsbReadyDone = false;
            __ISB();
            __DSB();

            if (s_pTarget->pRuntime != nullptr)
            {
                HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_HOST);
            }
            return;
        }

        /*
         * Clear whatever the port leaves behind in EVENTCAUSE before handing
         * over, otherwise EVENTS_USBEVENT re-asserts immediately and this
         * handler is re-entered without end.
         */
        uint32_t cause = NRF_USBD->EVENTCAUSE &
                         ~(uint32_t)HCI_NRF52840_USBD_PORT_EVENTCAUSE;
        if (cause != 0U)
        {
            s_pTarget->UsbEventCause |= cause;
            s_pTarget->UsbStuckCauseCount++;
            NRF_USBD->EVENTCAUSE = cause;
            __ISB();
            __DSB();

            /*
             * Take the umbrella event away only when no cause that TinyUSB
             * consumes remains. Clearing EVENTS_USBEVENT while SUSPEND,
             * RESUME or USBWUALLOWED is still set prevents the port from ever
             * seeing that state transition.
             */
            if ((NRF_USBD->EVENTCAUSE &
                 (uint32_t)HCI_NRF52840_USBD_PORT_EVENTCAUSE) == 0U)
            {
                NRF_USBD->EVENTS_USBEVENT = 0U;
                __ISB();
                __DSB();
            }
        }

        tusb_int_handler(0U, true);
    }
}

/*
 * The target interface. Thin wrappers rather than casting the function
 * pointers, so the compiler checks each signature rather than trusting a cast.
 */
static bool HciNrf52840TargetInit(void *pContext,
                                  HciTaktOs_t *pRuntime,
                                  uint8_t *pSdcMem,
                                  size_t SdcMemCapacity,
                                  bool UsbEnabled)
{
    return HciNrf52840Init(static_cast<HciNrf52840_t *>(pContext), pRuntime,
                           pSdcMem, SdcMemCapacity, UsbEnabled);
}

static void HciNrf52840TargetGetTaktOsOps(void *pContext,
                                          HciTaktOsOps_t *pOps)
{
    HciNrf52840GetTaktOsOps(static_cast<HciNrf52840_t *>(pContext), pOps);
}

static bool HciNrf52840TargetUsbStart(void *pContext)
{
    return HciNrf52840UsbStart(static_cast<HciNrf52840_t *>(pContext));
}

static void HciNrf52840TargetUsbPassMark(void *pContext)
{
    HciNrf52840UsbPassMark(static_cast<HciNrf52840_t *>(pContext));
}

static void HciNrf52840TargetUsbPowerProcess(void *pContext)
{
    HciNrf52840UsbPowerProcess(static_cast<HciNrf52840_t *>(pContext));
}

/*
 * A storm is the USBD peripheral raising events with no interrupt source the
 * driver recognises. Nothing later in the settling loop clears it, so once it
 * is seen there is no point waiting out the remaining passes.
 */
static bool HciNrf52840TargetUsbStuck(const void *pContext)
{
    const HciNrf52840_t *pTarget =
        static_cast<const HciNrf52840_t *>(pContext);

    return pTarget != nullptr && pTarget->UsbStormEvents != 0U;
}

static void HciNrf52840TargetUsbTrace(const void *pContext,
                                      const char *pLabel,
                                      uint32_t Pass)
{
    const HciNrf52840_t *pTarget =
        static_cast<const HciNrf52840_t *>(pContext);

    if (pTarget == nullptr)
    {
        return;
    }

    HciTrace("host: %s pass=%lu irq=%lu stuck=%lu evtcause=0x%08lX "
             "inten=0x%08lX storm=0x%08lX stormcause=0x%08lX\r\n",
             pLabel,
             (unsigned long)Pass,
             (unsigned long)pTarget->UsbIrqCount,
             (unsigned long)pTarget->UsbStuckCauseCount,
             (unsigned long)pTarget->UsbEventCause,
             (unsigned long)pTarget->UsbStormInten,
             (unsigned long)pTarget->UsbStormEvents,
             (unsigned long)pTarget->UsbStormCause);

    /* HciTrace discards its arguments when tracing is off. */
    (void)pLabel;
    (void)Pass;
}

/*
 * A PSEL register holds the pin a peripheral function ended up on, or a
 * disconnected marker in the top bit. Bit 5 is the port and the low five bits
 * are the pin.
 */
#define HCI_NRF52840_PSEL_DISCONNECTED 0x80000000UL
#define HCI_NRF52840_UARTE_HWFC_MASK   0x00000001UL

static bool HciNrf52840PselLevel(uint32_t Psel, bool *pHigh)
{
    if ((Psel & HCI_NRF52840_PSEL_DISCONNECTED) != 0U)
    {
        return false;
    }

    const uint32_t port = (Psel >> 5) & 0x01U;
    const uint32_t pin = Psel & 0x1FU;
    const NRF_GPIO_Type *pGpio = port == 0U ? NRF_P0 : NRF_P1;

    *pHigh = (pGpio->IN & (1UL << pin)) != 0U;
    return true;
}

static void HciNrf52840PselName(uint32_t Psel, char Name[6])
{
    if ((Psel & HCI_NRF52840_PSEL_DISCONNECTED) != 0U)
    {
        Name[0] = 'N';
        Name[1] = 'C';
        Name[2] = '\0';
        return;
    }

    const uint32_t port = (Psel >> 5) & 0x01U;
    const uint32_t pin = Psel & 0x1FU;
    Name[0] = 'P';
    Name[1] = (char)('0' + port);
    Name[2] = '.';
    Name[3] = (char)('0' + (pin / 10U));
    Name[4] = (char)('0' + (pin % 10U));
    Name[5] = '\0';
}

static uint32_t HciNrf52840Baud(uint32_t RegisterValue)
{
    /* BAUDRATE = baud * 2^32 / 16 MHz. Round back to the nearest baud. */
    return (uint32_t)((((uint64_t)RegisterValue * 16000000ULL) +
                       0x80000000ULL) >> 32);
}

static void HciNrf52840TargetUartTrace(const void *, uint8_t DevNo)
{
    NRF_UARTE_Type *pReg = DevNo == 0U ? NRF_UARTE0 : NRF_UARTE1;
    char rxd[6];
    char txd[6];
    char cts[6];
    char rts[6];
    HciNrf52840PselName(pReg->PSEL.RXD, rxd);
    HciNrf52840PselName(pReg->PSEL.TXD, txd);
    HciNrf52840PselName(pReg->PSEL.CTS, cts);
    HciNrf52840PselName(pReg->PSEL.RTS, rts);

    bool ctsHigh = false;
    const bool ctsKnown = HciNrf52840PselLevel(pReg->PSEL.CTS, &ctsHigh);
    const bool hwfc = (pReg->CONFIG & HCI_NRF52840_UARTE_HWFC_MASK) != 0U;

    if (pReg->ERRORSRC == 0U)
    {
        HciTrace("uart%u: enabled=%s hwfc=%s baud=%lu errors=none "
                 "cts=%s RXD=%s TXD=%s CTS=%s RTS=%s\r\n",
                 (unsigned)DevNo,
                 pReg->ENABLE != 0U ? "yes" : "no",
                 hwfc ? "on" : "off",
                 (unsigned long)HciNrf52840Baud(pReg->BAUDRATE),
                 ctsKnown ? (ctsHigh ? "high(peer-not-ready)" :
                                       "low(peer-ready)") : "not-connected",
                 rxd, txd, cts, rts);
    }
    else
    {
        HciTrace("uart%u: enabled=%s hwfc=%s baud=%lu errors=0x%08lX "
                 "cts=%s RXD=%s TXD=%s CTS=%s RTS=%s\r\n",
                 (unsigned)DevNo,
                 pReg->ENABLE != 0U ? "yes" : "no",
                 hwfc ? "on" : "off",
                 (unsigned long)HciNrf52840Baud(pReg->BAUDRATE),
                 (unsigned long)pReg->ERRORSRC,
                 ctsKnown ? (ctsHigh ? "high(peer-not-ready)" :
                                       "low(peer-ready)") : "not-connected",
                 rxd, txd, cts, rts);
    }

    /* HciTrace discards its arguments when tracing is off. */
    (void)DevNo;
}

static void HciNrf52840TargetStop(void *pContext)
{
    HciNrf52840Stop(static_cast<HciNrf52840_t *>(pContext));
}

static void HciNrf52840TargetGetSdcMem(const void *pContext,
                                       uint32_t *pRequired,
                                       uint32_t *pCapacity)
{
    const HciNrf52840_t *pTarget =
        static_cast<const HciNrf52840_t *>(pContext);

    if (pTarget == nullptr)
    {
        return;
    }

    if (pRequired != nullptr)
    {
        *pRequired = pTarget->RequiredSdcMem > 0 ?
                     (uint32_t)pTarget->RequiredSdcMem : 0U;
    }

    if (pCapacity != nullptr)
    {
        *pCapacity = (uint32_t)pTarget->SdcMemCapacity;
    }
}

static int32_t HciNrf52840TargetLastError(const void *pContext)
{
    const HciNrf52840_t *pTarget =
        static_cast<const HciNrf52840_t *>(pContext);

    return pTarget != nullptr ? pTarget->LastError : 0;
}

static const HciTargetOps_t s_Nrf52840Ops = {
    HciNrf52840TargetInit,
    HciNrf52840TargetGetTaktOsOps,
    HciNrf52840TargetUsbStart,
    HciNrf52840TargetUsbPassMark,
    HciNrf52840TargetUsbPowerProcess,
    HciNrf52840TargetUsbStuck,
    HciNrf52840TargetUsbTrace,
    HciNrf52840TargetUartTrace,
    HciNrf52840TargetStop,
    HciNrf52840TargetGetSdcMem,
    HciNrf52840TargetLastError,
};

/*
 * One radio, so one instance, owned here. The application holds the pair and
 * never has to know how large this is.
 */
static HciNrf52840_t s_Nrf52840;

HciTarget_t HciNrf52840Target(void)
{
    HciTarget_t target = { &s_Nrf52840Ops, &s_Nrf52840 };
    return target;
}
