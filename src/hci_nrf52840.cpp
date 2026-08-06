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

/*
 * The radio resource configuration lives in hci_nrf52840.h, next to the pool
 * size that is computed from it, so that raising a count and sizing the memory
 * cannot be done separately.
 */

static HciNrf52840_t *s_pTarget;

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

static bool HciNrf52840CfgSet(HciNrf52840_t *pTarget,
                              uint8_t Type,
                              const sdc_cfg_t *pCfg)
{
    int32_t required = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, Type, pCfg);
    HciTrace("sdc: cfg type=%u result=%ld\r\n", (unsigned)Type, (long)required);
    if (required < 0)
    {
        pTarget->LastError = required;
        return false;
    }

    pTarget->RequiredSdcMem = required;
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

    sdc_support_ext_adv();
    sdc_support_peripheral();
    sdc_support_ext_central();
    sdc_support_le_2m_phy();
    sdc_support_le_coded_phy();
    sdc_support_dle_peripheral();
    sdc_support_dle_central();
    sdc_support_phy_update_peripheral();
    sdc_support_phy_update_central();
    sdc_support_direct_test_mode();

    /*
     * Address resolution in the controller. Without it the resolving list
     * commands are rejected and a bonded peer arriving under a new resolvable
     * private address is a stranger, which is what a phone looks like on every
     * reconnection. Must be called before sdc_cfg_set and sdc_enable, which is
     * why it sits here with the rest.
     */
    sdc_support_le_privacy();

#if HCI_NRF52840_QOS_CHANNEL_SURVEY
    /*
     * The channel survey module, which the vendor command at 0xFD0E turns on
     * and off. Without this call that command is rejected, so the two have to
     * agree, and the pool in hci_nrf52840.h carries the matching 40 octets.
     */
    sdc_support_qos_channel_survey();
#endif

#if HCI_NRF52840_LE_POWER_CONTROL
    /*
     * Both roles, because this image supports both and sdk-nrfxlib asks for a
     * call per role rather than one for the pair. Path loss monitoring has to
     * come after at least one of them, which is why the order here is not
     * alphabetical.
     */
    sdc_support_le_power_control_central();
    sdc_support_le_power_control_peripheral();
    sdc_support_le_path_loss_monitoring();
#endif

#if HCI_NRF52840_SCA_UPDATE
    sdc_support_sca_central();
    sdc_support_sca_peripheral();
#endif

#if HCI_NRF52840_CONNECTION_SUBRATING
    sdc_support_connection_subrating_central();
    sdc_support_connection_subrating_peripheral();
#endif

#if HCI_NRF52840_EXTENDED_FEATURE_SET
    sdc_support_extended_feature_set_central();
    sdc_support_extended_feature_set_peripheral();
#endif

#if HCI_NRF52840_PARALLEL_SCAN_INIT
    /*
     * sdk-nrfxlib asks for a central role before this, which
     * sdc_support_ext_central() above provides.
     */
    sdc_support_parallel_scanning_and_initiating();
#endif

#if HCI_NRF52840_PERIODIC_ADV
    sdc_support_le_periodic_adv();
#endif

#if HCI_NRF52840_PERIODIC_SYNC
    sdc_support_le_periodic_sync();
#endif

#if HCI_NRF52840_PERIODIC_SYNC_TRANSFER
    /*
     * Four calls rather than two. Sending and receiving a sync are separate
     * capabilities and each is per role, so a build that only ever hands a
     * sync out still needs both sender calls, and this image supports both
     * roles.
     */
    sdc_support_periodic_adv_sync_transfer_sender_central();
    sdc_support_periodic_adv_sync_transfer_sender_peripheral();
    sdc_support_periodic_adv_sync_transfer_receiver_central();
    sdc_support_periodic_adv_sync_transfer_receiver_peripheral();
#endif

    /*
     * Periodic advertising with responses, last because sdk-nrfxlib requires
     * extended advertising, the matching plain periodic half, and a sync
     * transfer sender or receiver, all of which are above.
     */
#if HCI_NRF52840_PERIODIC_ADV_RSP
    sdc_support_le_periodic_adv_with_rsp();
#endif

#if HCI_NRF52840_PERIODIC_SYNC_RSP
    sdc_support_le_periodic_sync_with_rsp();
#endif

    sdc_cfg_t cfg = {};
    cfg.buffer_cfg.rx_packet_size = HCI_NRF52840_ACL_PACKET_SIZE;
    cfg.buffer_cfg.tx_packet_size = HCI_NRF52840_ACL_PACKET_SIZE;
    cfg.buffer_cfg.rx_packet_count = HCI_NRF52840_ACL_PACKET_COUNT;
    cfg.buffer_cfg.tx_packet_count = HCI_NRF52840_ACL_PACKET_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_BUFFER_CFG, &cfg)) return false;

    cfg = {};
    cfg.peripheral_count.count = HCI_NRF52840_PERIPHERAL_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIPHERAL_COUNT, &cfg)) return false;

    cfg = {};
    cfg.central_count.count = HCI_NRF52840_CENTRAL_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_CENTRAL_COUNT, &cfg)) return false;

    cfg = {};
    cfg.adv_count.count = HCI_NRF52840_ADV_SET_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_ADV_COUNT, &cfg)) return false;

    cfg = {};
    cfg.adv_buffer_cfg.max_adv_data = HCI_NRF52840_MAX_ADV_DATA;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_ADV_BUFFER_CFG, &cfg)) return false;

    cfg = {};
    cfg.scan_buffer_cfg.count = HCI_NRF52840_SCAN_BUFFER_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_SCAN_BUFFER_CFG, &cfg)) return false;

    /*
     * The filter accept list. Left unset the controller takes its own default,
     * which is eight, and a host that reads the size gets a number this build
     * never chose.
     */
    cfg = {};
    cfg.fal_size = HCI_NRF52840_FAL_SIZE;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_FAL_SIZE, &cfg)) return false;

#if HCI_NRF52840_EXTENDED_FEATURE_SET
    /*
     * How many feature pages the controller keeps per link. The pool in
     * hci_nrf52840.h is computed from the same macro, so the two cannot
     * disagree about what was reserved.
     */
    cfg = {};
    cfg.extended_feature_page_count = HCI_NRF52840_EXTENDED_FEATURE_PAGES;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_EXTENDED_FEATURE_PAGE_COUNT,
                           &cfg))
    {
        return false;
    }
#endif

#if HCI_NRF52840_PERIODIC_ADV
    /*
     * Periodic advertisers. Each takes one of the advertising sets configured
     * above, which is why hci_nrf52840.h refuses a count larger than that one
     * at build time rather than letting sdc_cfg_set refuse it here.
     */
    cfg = {};
    cfg.periodic_adv_count.count = HCI_NRF52840_PERIODIC_ADV_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_ADV_COUNT, &cfg))
    {
        return false;
    }
#endif

#if HCI_NRF52840_PERIODIC_SYNC
    cfg = {};
    cfg.periodic_sync_count.count = HCI_NRF52840_PERIODIC_SYNC_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_SYNC_COUNT, &cfg))
    {
        return false;
    }

    cfg = {};
    cfg.periodic_sync_buffer_cfg.count =
        HCI_NRF52840_PERIODIC_SYNC_BUFFER_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_SYNC_BUFFER_CFG,
                           &cfg))
    {
        return false;
    }

    /*
     * The periodic advertiser list. The controller default is zero, so left
     * unset a host that reads the size is told the list does not work, and
     * every train has to be named by address instead.
     */
    cfg = {};
    cfg.periodic_adv_list_size = HCI_NRF52840_PERIODIC_ADV_LIST_SIZE;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_ADV_LIST_SIZE, &cfg))
    {
        return false;
    }
#endif

#if HCI_NRF52840_PERIODIC_ADV_RSP
    cfg = {};
    cfg.periodic_adv_rsp_count.count = HCI_NRF52840_PERIODIC_ADV_RSP_COUNT;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_ADV_RSP_COUNT, &cfg))
    {
        return false;
    }

    /*
     * All three buffer numbers together, because the vendor structure carries
     * them in one member and the pool macro takes all three. Setting one and
     * leaving the others to their defaults would compute a pool for numbers the
     * controller was never given.
     */
    cfg = {};
    cfg.periodic_adv_rsp_buffer_cfg.tx_buffer_count =
        HCI_NRF52840_PERIODIC_ADV_RSP_TX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.rx_buffer_count =
        HCI_NRF52840_PERIODIC_ADV_RSP_RX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.max_tx_data_size =
        HCI_NRF52840_PERIODIC_ADV_RSP_MAX_TX_DATA;
    if (!HciNrf52840CfgSet(pTarget, SDC_CFG_TYPE_PERIODIC_ADV_RSP_BUFFER_CFG,
                           &cfg))
    {
        return false;
    }

    cfg = {};
    cfg.periodic_adv_rsp_failure_reporting_cfg =
        HCI_NRF52840_PERIODIC_ADV_RSP_FAILURE_REPORTING;
    if (!HciNrf52840CfgSet(
            pTarget, SDC_CFG_TYPE_PERIODIC_ADV_RSP_FAILURE_REPORTING_CFG,
            &cfg))
    {
        return false;
    }
#endif

#if HCI_NRF52840_PERIODIC_SYNC_RSP
    cfg = {};
    cfg.periodic_sync_rsp_tx_buffer_cfg.count =
        HCI_NRF52840_PERIODIC_SYNC_RSP_TX_BUFFERS;
    if (!HciNrf52840CfgSet(pTarget,
                           SDC_CFG_TYPE_PERIODIC_SYNC_RSP_TX_BUFFER_CFG, &cfg))
    {
        return false;
    }
#endif

    result = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, SDC_CFG_TYPE_NONE, nullptr);
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
            NRF_USBD->EVENTS_USBEVENT = 0U;
            __ISB();
            __DSB();
        }

        tusb_int_handler(0U, true);
    }
}
