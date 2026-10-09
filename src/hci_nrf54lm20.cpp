/**-------------------------------------------------------------------------
@file hci_nrf54lm20.cpp
@brief nRF54LM20 MPSL/SDC target for HciController.
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_nrf54lm20.h"
#include "hci_sdc_resources.h"
#include "hci_trace.h"
#include <stdint.h>
#include <string.h>
#include "nrf.h"
#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"
#if defined(__cplusplus) && !defined(restrict)
#define restrict __restrict
#include "mpsl.h"
#undef restrict
#else
#include "mpsl.h"
#endif
#include "sdc.h"
#include "sdc_soc.h"
#if defined(NRF54LM20B_XXAA) || defined(NRF54LM20A_XXAA)
#include "hal/nrf_power.h"
#else
#error "HciNrf54lm20 target requires the nRF54LM20 application MCU"
#endif

#ifndef HCI_NRF54LM20_RAND_PROBE_LOOPS
#define HCI_NRF54LM20_RAND_PROBE_LOOPS 1000U
#endif
#ifndef HCI_NRF54LM20_MPSL_LOW_PRIORITY
#define HCI_NRF54LM20_MPSL_LOW_PRIORITY (MPSL_HIGH_IRQ_PRIORITY + 4U)
#endif
static HciNrf54lm20_t *s_pTarget;

/* Called from a time-critical fault context: never perform blocking
 * UART/USB tracing or allocate before resetting the controller.
 */
static void HciRadioAssert(uint32_t reason)
{
    if (s_pTarget != nullptr)
    {
        s_pTarget->FaultCount++;
        s_pTarget->LastError = (int32_t)reason;
    }
    __DSB();
    NVIC_SystemReset();
    for (;;) { __WFE(); }
}
static void HciMpslAssert(const char *file, uint32_t line)
{
    (void)file;
    (void)line;
    HciRadioAssert(0x1003U);
}
static void HciSdcAssert(const char *file, uint32_t line)
{
    (void)file;
    (void)line;
    HciRadioAssert(0x1004U);
}
static void HciRandPoll(uint8_t *pBuffer, uint8_t Length)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();
    while (rng == nullptr)
    {
        if (s_pTarget != nullptr) { s_pTarget->RandRetryCount++; }
        rng = CryptoRngNrfInstance();
    }
    while (rng->Random(pBuffer, Length) != CRYPTO_STATUS_OK)
    {
        if (s_pTarget != nullptr) { s_pTarget->RandRetryCount++; }
    }
}
static bool HciRandSourceReady(HciNrf54lm20_t *pTarget)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();
    if (rng == nullptr)
    {
        pTarget->LastError = -1005;
        return false;
    }
    uint8_t probe[8];
    for (uint32_t i = 0; i < HCI_NRF54LM20_RAND_PROBE_LOOPS; i++)
    {
        if (rng->Random(probe, sizeof(probe)) == CRYPTO_STATUS_OK)
        {
            return true;
        }
    }
    pTarget->LastError = -1005;
    return false;
}
static void HciSdcCallback(void)
{
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_SDC);
    }
}
static bool HciMpslInit(HciNrf54lm20_t *pTarget)
{
    /* Nordic MPSL nRF54L contract: GRTC and SYSCOUNTER before mpsl_init.
     * IOsonata ARM/Nordic/src/nrf_mpsl.cpp uses the same sequence.
     */
    if (SystemCoreClockGet() != 128000000U)
    {
        pTarget->LastError = -1001;
        HciTrace("nRF54L: MPSL requires 128 MHz core clock\r\n");
        return false;
    }
    NRF_GRTC->MODE |= 2U;
    NRF_GRTC->TASKS_START = 1U;

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
    const int32_t result = mpsl_init(&lfclk, SWI00_IRQn, HciMpslAssert);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->MpslInitialized = true;

    NVIC_SetPriority(SWI00_IRQn, HCI_NRF54LM20_MPSL_LOW_PRIORITY);
    NVIC_EnableIRQ(SWI00_IRQn);
    NVIC_SetPriority(RADIO_0_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_EnableIRQ(RADIO_0_IRQn);
    NVIC_SetPriority(GRTC_3_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_EnableIRQ(GRTC_3_IRQn);
    NVIC_SetPriority(TIMER10_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_EnableIRQ(TIMER10_IRQn);
    NVIC_SetPriority(CLOCK_POWER_IRQn, HCI_NRF54LM20_MPSL_LOW_PRIORITY);
    NVIC_EnableIRQ(CLOCK_POWER_IRQn);
    return true;
}
static bool HciSdcInit(HciNrf54lm20_t *pTarget)
{
    int32_t result = sdc_init(HciSdcAssert);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->SdcInitialized = true;
    if (!HciRandSourceReady(pTarget)) { return false; }

    sdc_rand_source_t randomSource = { HciRandPoll };
    result = sdc_rand_source_register(&randomSource);
    if (result != 0) { pTarget->LastError = result; return false; }

    result = HciSdcResourcesApply();
    if (result < 0) { pTarget->LastError = result; return false; }
    pTarget->RequiredSdcMem = result;
    if ((size_t)result > pTarget->SdcMemCapacity ||
        (((uintptr_t)pTarget->pSdcMem & 7U) != 0U))
    {
        pTarget->LastError = -1002;
        return false;
    }
    result = sdc_enable(HciSdcCallback, pTarget->pSdcMem);
    if (result != 0) { pTarget->LastError = result; return false; }
    pTarget->SdcEnabled = true;
    return true;
}
static void HciStop(void *pContext)
{
    HciNrf54lm20_t *pTarget = static_cast<HciNrf54lm20_t *>(pContext);
    if (pTarget == nullptr || s_pTarget != pTarget) { return; }
    if (pTarget->SdcInitialized)
    {
        (void)sdc_disable();
        pTarget->SdcEnabled = false;
        pTarget->SdcInitialized = false;
    }
    if (pTarget->MpslInitialized)
    {
        mpsl_uninit();
        pTarget->MpslInitialized = false;
    }
    NVIC_DisableIRQ(SWI00_IRQn);
    NVIC_DisableIRQ(GRTC_3_IRQn);
    NVIC_DisableIRQ(TIMER10_IRQn);
    NVIC_DisableIRQ(RADIO_0_IRQn);
    NVIC_DisableIRQ(CLOCK_POWER_IRQn);
    s_pTarget = nullptr;
}
static bool HciStart(void *pContext)
{
    HciNrf54lm20_t *pTarget = static_cast<HciNrf54lm20_t *>(pContext);
    if (pTarget == nullptr || s_pTarget != nullptr) { return false; }
    s_pTarget = pTarget;
    if (!HciMpslInit(pTarget) || !HciSdcInit(pTarget))
    {
        HciStop(pContext);
        return false;
    }
    return true;
}
static void HciProcessMpsl(void *)
{
    mpsl_low_priority_process();
}
static void HciFault(void *pContext, int error)
{
    HciNrf54lm20_t *pTarget = static_cast<HciNrf54lm20_t *>(pContext);
    if (pTarget != nullptr)
    {
        pTarget->FaultCount++;
        if (pTarget->LastError == 0) { pTarget->LastError = error; }
    }
}
static bool HciInit(void *pContext, HciTaktOs_t *pRuntime,
                    uint8_t *pSdcMem, size_t capacity)
{
    HciNrf54lm20_t *pTarget = static_cast<HciNrf54lm20_t *>(pContext);
    if (pTarget == nullptr || pRuntime == nullptr || pSdcMem == nullptr ||
        capacity == 0U || (((uintptr_t)pSdcMem & 7U) != 0U))
    {
        return false;
    }
    memset(pTarget, 0, sizeof(*pTarget));
    pTarget->pRuntime = pRuntime;
    pTarget->pSdcMem = pSdcMem;
    pTarget->SdcMemCapacity = capacity;
    return true;
}
static void HciGetTaktOsOps(void *pContext, HciTaktOsOps_t *pOps)
{
    if (pOps == nullptr) { return; }
    pOps->Start = HciStart;
    pOps->ProcessMpsl = HciProcessMpsl;
    pOps->Fault = HciFault;
    pOps->pContext = pContext;
}
static void HciGetSdcMem(const void *pContext,
                         uint32_t *pRequired, uint32_t *pCapacity)
{
    const HciNrf54lm20_t *pTarget =
        static_cast<const HciNrf54lm20_t *>(pContext);
    if (pRequired != nullptr)
    {
        *pRequired = pTarget != nullptr && pTarget->RequiredSdcMem > 0 ?
                     (uint32_t)pTarget->RequiredSdcMem : 0U;
    }
    if (pCapacity != nullptr)
    {
        *pCapacity = pTarget != nullptr ? (uint32_t)pTarget->SdcMemCapacity : 0U;
    }
}
static int32_t HciLastError(const void *pContext)
{
    const HciNrf54lm20_t *pTarget =
        static_cast<const HciNrf54lm20_t *>(pContext);
    return pTarget != nullptr ? pTarget->LastError : 0;
}
static const HciTargetOps_t s_TargetOps = {
    HciInit, HciGetTaktOsOps, nullptr, HciStop, HciGetSdcMem, HciLastError
};
static HciNrf54lm20_t s_Target;
HciTarget_t HciNrf54lm20Target(void)
{
    HciTarget_t t = { &s_TargetOps, &s_Target };
    return t;
}

/* MPSL hardware vectors. Low-priority work is always deferred to TaktOS. */
extern "C" void SWI00_IRQHandler(void)
{
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_MPSL);
    }
}
extern "C" void RADIO_0_IRQHandler(void) { MPSL_IRQ_RADIO_Handler(); }
extern "C" void GRTC_3_IRQHandler(void) { MPSL_IRQ_RTC0_Handler(); }
extern "C" void TIMER10_IRQHandler(void) { MPSL_IRQ_TIMER0_Handler(); }
extern "C" void CLOCK_POWER_IRQHandler(void) { MPSL_IRQ_CLOCK_Handler(); }

/* nRF54L MPSL low-latency window callbacks. */
#if NRF_POWER_HAS_CONST_LATENCY
static volatile uint32_t s_ConstLatRefs;
static void HciConstLatAcquire(void)
{
    if (s_ConstLatRefs++ == 0U)
        nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
}
static void HciConstLatRelease(void)
{
    if (s_ConstLatRefs > 0U && --s_ConstLatRefs == 0U)
        nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_LOWPWR);
}
#else
static void HciConstLatAcquire(void) {}
static void HciConstLatRelease(void) {}
#endif
extern "C" void mpsl_low_latency_acquire_callback(void)
{ HciConstLatAcquire(); }
extern "C" void mpsl_low_latency_release_callback(void)
{ HciConstLatRelease(); }
extern "C" void mpsl_constlat_request_callback(void)
{ HciConstLatAcquire(); }
extern "C" void mpsl_lowpower_request_callback(void)
{ HciConstLatRelease(); }
