/**-------------------------------------------------------------------------
@file	hci_nrf5340_net.cpp

@brief	nRF5340 network core MPSL/SDC target for HciController.

		Same job as the other targets: bring up MPSL, then the SoftDevice
		Controller, hand the low priority work to the HCI thread, and take
		them down in order. The network core has no USB and no UART in use
		here, so there is no clock request or host diagnostic.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_nrf5340_net.h"

#include "hci_sdc_resources.h"

#include <stdint.h>
#include <string.h>

#include "nrf.h"

#if defined(__cplusplus) && !defined(restrict)
#define restrict __restrict
#include "mpsl.h"
#undef restrict
#else
#include "mpsl.h"
#endif

#include "sdc.h"
#include "sdc_soc.h"
#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"

#ifndef HCI_NRF5340_NET_LOW_IRQ_PRIORITY
#define HCI_NRF5340_NET_LOW_IRQ_PRIORITY (MPSL_HIGH_IRQ_PRIORITY + 4U)
#endif

#ifndef HCI_NRF5340_NET_RAND_PROBE_LOOPS
#define HCI_NRF5340_NET_RAND_PROBE_LOOPS 1000U
#endif

#define HCI_NRF5340_NET_ERR_NO_ENTROPY   (-1005)
#define HCI_NRF5340_NET_ERR_SDC_MEM      (-1002)

static HciNrf5340Net_t s_Target;
static HciNrf5340Net_t *s_pTarget;

/* Called from a time critical fault context: no tracing, no allocation. Count
 * it and reset the network core.
 */
static void HciRadioAssert(uint32_t Reason)
{
    if (s_pTarget != nullptr)
    {
        s_pTarget->FaultCount++;
        s_pTarget->LastError = (int32_t)Reason;
    }
    __DSB();
    NVIC_SystemReset();
    for (;;)
    {
        __WFE();
    }
}

static void HciMpslAssert(const char *, uint32_t)
{
    HciRadioAssert(0x1003U);
}

static void HciSdcAssert(const char *, uint32_t)
{
    HciRadioAssert(0x1004U);
}

/* SDC asks for entropy and must get it, see sdc_soc.h. The source was proven
 * at start up, so this only waits out a busy moment.
 */
static void HciRandPoll(uint8_t *pBuffer, uint8_t Length)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();

    while (rng == nullptr)
    {
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

static bool HciRandSourceReady(HciNrf5340Net_t *pTarget)
{
    CryptoRngNrf *rng = CryptoRngNrfInstance();

    if (rng == nullptr)
    {
        pTarget->LastError = HCI_NRF5340_NET_ERR_NO_ENTROPY;
        return false;
    }

    uint8_t probe[8];
    for (uint32_t loop = 0U; loop < HCI_NRF5340_NET_RAND_PROBE_LOOPS; loop++)
    {
        if (rng->Random(probe, sizeof(probe)) == CRYPTO_STATUS_OK)
        {
            return true;
        }
    }

    pTarget->LastError = HCI_NRF5340_NET_ERR_NO_ENTROPY;
    return false;
}

static void HciSdcCallback(void)
{
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_SDC);
    }
}

static bool HciMpslInit(HciNrf5340Net_t *pTarget)
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

    /* The low priority line is a real NVIC interrupt, MPSL pends it through
     * the NVIC. It only wakes the HCI thread, see SWI0_IRQHandler.
     */
    const int32_t result = mpsl_init(&lfclk, SWI0_IRQn, HciMpslAssert);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->MpslInitialized = true;

    NVIC_SetPriority(RADIO_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(RTC0_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(TIMER0_IRQn, MPSL_HIGH_IRQ_PRIORITY);
    NVIC_SetPriority(CLOCK_POWER_IRQn, HCI_NRF5340_NET_LOW_IRQ_PRIORITY);
    NVIC_SetPriority(SWI0_IRQn, HCI_NRF5340_NET_LOW_IRQ_PRIORITY);

    NVIC_EnableIRQ(RADIO_IRQn);
    NVIC_EnableIRQ(RTC0_IRQn);
    NVIC_EnableIRQ(TIMER0_IRQn);
    NVIC_EnableIRQ(CLOCK_POWER_IRQn);
    NVIC_EnableIRQ(SWI0_IRQn);

    return true;
}

static bool HciSdcInit(HciNrf5340Net_t *pTarget)
{
    int32_t result = sdc_init(HciSdcAssert);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->SdcInitialized = true;

    if (!HciRandSourceReady(pTarget))
    {
        return false;
    }

    sdc_rand_source_t randomSource = { HciRandPoll };
    result = sdc_rand_source_register(&randomSource);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    result = HciSdcResourcesApply();
    if (result < 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->RequiredSdcMem = result;

    if ((size_t)result > pTarget->SdcMemCapacity ||
        (((uintptr_t)pTarget->pSdcMem & 7U) != 0U))
    {
        pTarget->LastError = HCI_NRF5340_NET_ERR_SDC_MEM;
        return false;
    }

    result = sdc_enable(HciSdcCallback, pTarget->pSdcMem);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }

    pTarget->SdcEnabled = true;
    return true;
}

void HciTargetStop(void)
{
    HciNrf5340Net_t *pTarget = s_pTarget;

    if (pTarget == nullptr)
    {
        return;
    }

    if (pTarget->SdcEnabled)
    {
        (void)sdc_disable();
        pTarget->SdcEnabled = false;
    }
    pTarget->SdcInitialized = false;

    if (pTarget->MpslInitialized)
    {
        mpsl_uninit();
        pTarget->MpslInitialized = false;
    }

    NVIC_DisableIRQ(SWI0_IRQn);
    NVIC_DisableIRQ(RADIO_IRQn);
    NVIC_DisableIRQ(RTC0_IRQn);
    NVIC_DisableIRQ(TIMER0_IRQn);
    NVIC_DisableIRQ(CLOCK_POWER_IRQn);

    s_pTarget = nullptr;
}

bool HciTargetStart(void)
{
    if (s_pTarget != nullptr)
    {
        return false;
    }

    s_pTarget = &s_Target;
    if (!HciMpslInit(&s_Target) || !HciSdcInit(&s_Target))
    {
        HciTargetStop();
        return false;
    }

    return true;
}

void HciTargetProcessMpsl(void)
{
    mpsl_low_priority_process();
}

void HciTargetFault(int Error)
{
    s_Target.FaultCount++;
    if (s_Target.LastError == 0)
    {
        s_Target.LastError = Error;
    }
}

bool HciTargetInit(HciTaktOs_t *pRuntime, uint8_t *pSdcMem, size_t SdcMemCapacity)
{
    if (pRuntime == nullptr || pSdcMem == nullptr ||
        SdcMemCapacity == 0U || (((uintptr_t)pSdcMem & 7U) != 0U))
    {
        return false;
    }

    memset(&s_Target, 0, sizeof(s_Target));
    s_Target.pRuntime = pRuntime;
    s_Target.pSdcMem = pSdcMem;
    s_Target.SdcMemCapacity = SdcMemCapacity;
    return true;
}

void HciTargetGetSdcMem(uint32_t *pRequired, uint32_t *pCapacity)
{
    if (pRequired != nullptr)
    {
        *pRequired = s_Target.RequiredSdcMem > 0 ?
                     (uint32_t)s_Target.RequiredSdcMem : 0U;
    }

    if (pCapacity != nullptr)
    {
        *pCapacity = (uint32_t)s_Target.SdcMemCapacity;
    }
}

int32_t HciTargetLastError(void)
{
    return s_Target.LastError;
}

/* MPSL hardware vectors. The low priority work is always deferred to the HCI
 * thread, the vector only wakes it.
 */
extern "C" void SWI0_IRQHandler(void)
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

extern "C" void CLOCK_POWER_IRQHandler(void)
{
    MPSL_IRQ_CLOCK_Handler();
}

#define HCI_NET_MODE_TAG 0xA0U

bool HciNrf5340NetModeLoad(uint8_t *pMode)
{
    const uint32_t tag = NRF_POWER->GPREGRET[0];
    const uint32_t check = NRF_POWER->GPREGRET[1];

    if (pMode == nullptr || (tag & 0xF0U) != HCI_NET_MODE_TAG ||
        check != ((~tag) & 0xFFU))
    {
        return false;
    }

    *pMode = (uint8_t)(tag & 0x0FU);
    return true;
}

bool HciNrf5340NetModeStore(uint8_t Mode)
{
    const uint32_t tag = HCI_NET_MODE_TAG | ((uint32_t)Mode & 0x0FU);

    NRF_POWER->GPREGRET[0] = tag;
    NRF_POWER->GPREGRET[1] = (~tag) & 0xFFU;

    return NRF_POWER->GPREGRET[0] == tag;
}
