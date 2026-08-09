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

#include "coredev/iopincfg.h"
#include "coredev/system_core_clock.h"
#include "iopinctrl.h"
#include "crypto_rng_nrf.h"
#include "hci_trace.h"

#ifndef HCI_NRF52840_LOW_IRQ_PRIORITY
#define HCI_NRF52840_LOW_IRQ_PRIORITY 7U
#endif

#ifndef HCI_NRF52840_CLOCK_IRQ_PRIORITY
#define HCI_NRF52840_CLOCK_IRQ_PRIORITY 4U
#endif

#ifndef HCI_NRF52840_USB_IRQ_PRIORITY
#define HCI_NRF52840_USB_IRQ_PRIORITY 6U
#endif

/* Set to 1 when the nrfxlib in use predates mpsl_clock_hfclk_src_request. */
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

/*
 * Attempts at the random source before giving up. It is polled from low
 * priority processing, so this bounds a stall rather than a busy wait.
 */
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

static void HciNrf52840CycleCounterStart(void);

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
            pTarget->LastError = result;
            return false;
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
    pTarget->LastError = HCI_NRF52840_ERR_HFCLK_TIMEOUT;
    return false;
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
    if (pTarget == nullptr || !pTarget->UsbReadyDone)
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

/*
 * mpsl.h and sdc.h both state that the library "will reset the chip if the
 * application returns from this function", and that all interrupts are already
 * disabled on entry. Returning is therefore the documented recovery, and
 * spinning here forfeits it: the device hangs with interrupts off and nothing
 * to show for it.
 *
 * The file and line identify the fault. mpsl_asserts.h and sdc_asserts.h
 * decode the line value, so it is kept where a debugger or a later read of the
 * target state can find it rather than discarded.
 */
static void HciNrf52840RecordAssert(HciNrf52840_t *pTarget,
                                    const char *pFile,
                                    uint32_t Line,
                                    bool FromSdc)
{
    if (pTarget == nullptr)
    {
        return;
    }

    pTarget->AssertFile = pFile;
    pTarget->AssertLine = Line;
    pTarget->AssertFromSdc = FromSdc;
    pTarget->AssertCount++;
}

static void HciNrf52840MpslAssert(const char *file, uint32_t line)
{
    HciNrf52840RecordAssert(s_pTarget, file, line, false);
    /* Return, so MPSL resets the chip. */
}

static void HciNrf52840SdcAssert(const char *file, uint32_t line)
{
    HciNrf52840RecordAssert(s_pTarget, file, line, true);
    /* Return, so the controller resets the chip. */
}

/*
 * The entropy source SDC uses for pairing and for resolvable addresses.
 *
 * sdc_soc.h states the contract without an escape: "This function must block
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
     * USB bits from the shared enable register. Put them back each pass.
     */
    HciNrf52840_t *pTarget = static_cast<HciNrf52840_t *>(pContext);
    if (pTarget != nullptr && pTarget->UsbStarted)
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

    /*
     * Before anything can raise an interrupt, so the first one is timed like
     * every other one.
     */
    HciNrf52840CycleCounterStart();
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

/*
 * Start the core's cycle counter, so the USB handler can time itself.
 *
 * It is free running and reading it is one load, which is what makes it
 * usable from inside the handler it measures. Nothing else here uses it, and
 * leaving it running costs nothing.
 */
static void HciNrf52840CycleCounterStart(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
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
        const uint32_t entryCycle = DWT->CYCCNT;

        s_pTarget->UsbIrqCount++;

        /*
         * Too many entries inside one pump pass means an event is re-asserting
         * faster than it is being consumed. Record what is pending once, so
         * the offending source can be named instead of guessed at.
         */
        if (s_pTarget->UsbStormEvents == 0U &&
            (s_pTarget->UsbIrqCount - s_pTarget->UsbIrqMark) >
                HCI_NRF52840_USB_STORM_LIMIT)
        {
            s_pTarget->UsbStormInten = NRF_USBD->INTEN;
            s_pTarget->UsbStormCause = NRF_USBD->EVENTCAUSE;
            s_pTarget->UsbStormEvents = HciNrf52840UsbdPendingEvents();
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
             * Take the event away only when nothing the port reads is left.
             * SUSPEND, RESUME and USBWUALLOWED can be set in the same word as
             * the cause cleared above, and the port learns about them from
             * EVENTS_USBEVENT alone: it never reads EVENTCAUSE unless the
             * event is there. Clearing it whatever is left loses a suspend or
             * a resume, and a lost resume leaves the peripheral in low power
             * with nothing to bring it out.
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

        const uint32_t took = DWT->CYCCNT - entryCycle;
        s_pTarget->UsbIrqCycles = took;
        if (took > s_pTarget->UsbIrqCyclesWorst)
        {
            s_pTarget->UsbIrqCyclesWorst = took;
        }
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

    /*
     * And the peripheral itself, which is the layer under everything the
     * firmware counts. EPINEN and EPOUTEN say which endpoints the port still
     * has open, INTEN says which end of transfer interrupts it is still
     * listening for, and pend says which events are raised right now. An end
     * of transfer raised with its INTEN bit clear is a transfer whose
     * completion nobody will ever deliver, which is the one shape that stops
     * an endpoint with every counter above it at zero.
     */
    HciTrace("usbd: epstat=0x%08lX epdata=0x%08lX epin=0x%02lX epout=0x%02lX "
             "inten=0x%08lX cause=0x%08lX pend=0x%08lX sizeout2=%lu "
             "pullup=%lu irq=%lu cyc=%lu worst=%lu\r\n",
             (unsigned long)NRF_USBD->EPSTATUS,
             (unsigned long)NRF_USBD->EPDATASTATUS,
             (unsigned long)NRF_USBD->EPINEN,
             (unsigned long)NRF_USBD->EPOUTEN,
             (unsigned long)NRF_USBD->INTEN,
             (unsigned long)NRF_USBD->EVENTCAUSE,
             (unsigned long)HciNrf52840UsbdPendingEvents(),
             (unsigned long)NRF_USBD->SIZE.EPOUT[2],
             (unsigned long)NRF_USBD->USBPULLUP,
             (unsigned long)pTarget->UsbIrqCount,
             (unsigned long)pTarget->UsbIrqCycles,
             (unsigned long)pTarget->UsbIrqCyclesWorst);

    /* HciTrace discards its arguments when tracing is off. */
    (void)pLabel;
    (void)Pass;
}

/*
 * A PSEL register holds the pin a peripheral function ended up on, or a
 * disconnected marker in the top bit. Bit 5 is the port and the low five bits
 * are the pin, which is the encoding the pin map has to get right and the one
 * place to read back what the driver actually programmed rather than what the
 * board meant.
 */
#define HCI_NRF52840_PSEL_DISCONNECTED 0x80000000UL

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

static void HciNrf52840TargetUartTrace(const void *, uint8_t DevNo)
{
    NRF_UARTE_Type *pReg = DevNo == 0U ? NRF_UARTE0 : NRF_UARTE1;

    /*
     * Clear to send is active low and driven by the peer. Asserted means the
     * peer is willing to receive; deasserted means this part will not
     * transmit however much it has to say, which looks from above exactly like
     * a controller that never answered.
     */
    bool ctsHigh = false;
    const bool ctsKnown = HciNrf52840PselLevel(pReg->PSEL.CTS, &ctsHigh);

    HciTrace("uart%u: enable=%lu baud=0x%08lX errsrc=0x%08lX cts=%s "
             "psel rxd=0x%08lX txd=0x%08lX cts=0x%08lX rts=0x%08lX\r\n",
             (unsigned)DevNo,
             (unsigned long)pReg->ENABLE,
             (unsigned long)pReg->BAUDRATE,
             (unsigned long)pReg->ERRORSRC,
             ctsKnown ? (ctsHigh ? "high, peer not ready" : "low, peer ready")
                      : "not connected",
             (unsigned long)pReg->PSEL.RXD,
             (unsigned long)pReg->PSEL.TXD,
             (unsigned long)pReg->PSEL.CTS,
             (unsigned long)pReg->PSEL.RTS);

    /* HciTrace discards its arguments when tracing is off. */
    (void)DevNo;
}

/*
 * Long enough for a pin to reach the level its pull is asking for.
 *
 * The internal pull is tens of kilohms against a few tens of picofarads, so
 * the time constant is well under a microsecond. This is a few microseconds at
 * 64 MHz, which is generous for that and still nothing next to the rest of
 * bring up. A loop rather than a timer because this runs before any clock this
 * layer owns is running.
 */
#define HCI_NRF52840_PIN_SETTLE_LOOPS 200U

static void HciNrf52840PinSettle(void)
{
    for (volatile uint32_t i = 0U; i < HCI_NRF52840_PIN_SETTLE_LOOPS; i++)
    {
    }
}

static bool HciNrf52840PinIsDriven(const void *,
                                   uint8_t Port,
                                   uint8_t Pin,
                                   bool *pLevel)
{
    IOPinConfig(Port, Pin, 0, IOPINDIR_INPUT, IOPINRES_PULLUP,
                IOPINTYPE_NORMAL);
    HciNrf52840PinSettle();
    const int high = IOPinRead(Port, Pin);

    IOPinConfig(Port, Pin, 0, IOPINDIR_INPUT, IOPINRES_PULLDOWN,
                IOPINTYPE_NORMAL);
    HciNrf52840PinSettle();
    const int low = IOPinRead(Port, Pin);

    /*
     * Left with no pull rather than with whichever one was tried last, so a
     * pin about to be handed to the UART goes to it in a neutral state.
     */
    IOPinConfig(Port, Pin, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL);

    if (pLevel != nullptr)
    {
        *pLevel = high != 0;
    }

    /* The pull moved it, so nothing outside is holding it. */
    return high == low;
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
    HciNrf52840PinIsDriven,
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
