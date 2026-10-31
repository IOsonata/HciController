/**-------------------------------------------------------------------------
@file	hci_nrf54lm20.cpp

@brief	nRF54LM20 MPSL/SDC target for HciController.

@author	Nguyen Hoan Hoang
@date	October 2026

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
static HciNrf54lm20_t s_Target;
static HciNrf54lm20_t *s_pTarget;

static volatile uint32_t s_ConstLatRefs;
static uint32_t s_RramLowPowerConfig;

#if HCI_LM20_SYNC_TRACE
/* Each counter has one writer. Status snapshots are approximate: do not mask
 * radio interrupts merely to make diagnostic fields mutually consistent.
 */
static volatile uint32_t s_TraceRadio;
static volatile uint32_t s_TraceGrtc;
static volatile uint32_t s_TraceTimer;
static volatile uint32_t s_TraceSwi;
static volatile uint32_t s_TraceClock;
static volatile uint32_t s_TraceMpslEnter;
static volatile uint32_t s_TraceMpslExit;
static volatile uint32_t s_TraceSdc;
static volatile uint32_t s_TraceExtReports;
static volatile uint32_t s_TracePeriodicReports;

void HciNrf54lm20TraceStatus(void)
{
	HciTrace("lm20 irq radio=%lu grtc=%lu timer=%lu swi=%lu clock=%lu\r\n",
		(unsigned long)s_TraceRadio, (unsigned long)s_TraceGrtc,
		(unsigned long)s_TraceTimer, (unsigned long)s_TraceSwi,
		(unsigned long)s_TraceClock);
	HciTrace("lm20 work in=%lu out=%lu sdc=%lu ext=%lu periodic=%lu refs=%lu rram=%lx\r\n",
		(unsigned long)s_TraceMpslEnter, (unsigned long)s_TraceMpslExit,
		(unsigned long)s_TraceSdc, (unsigned long)s_TraceExtReports,
		(unsigned long)s_TracePeriodicReports, (unsigned long)s_ConstLatRefs,
		(unsigned long)NRF_RRAMC->POWER.LOWPOWERCONFIG);
}

void HciNrf54lm20TraceCommand(const uint8_t *pPacket, size_t Length)
{
	unsigned opcode;

	if (Length < 3U)
	{
		return;
	}
	opcode = pPacket[0] | ((unsigned)pPacket[1] << 8);
	if (opcode == 0x2044U || opcode == 0x2045U || opcode == 0x2046U ||
		opcode == 0x2041U || opcode == 0x2042U || opcode == 0x2084U)
	{
		HciTrace("lm20 dispatch op=%04x len=%u\r\n", opcode, (unsigned)Length);
	}
}

void HciNrf54lm20TraceEvent(const uint8_t *pPacket, size_t Length)
{
	unsigned opcode;
	unsigned status;

	if (Length < 3U)
	{
		return;
	}
	if (pPacket[0] == 0x3eU)
	{
		if (pPacket[2] == 0x0dU)
		{
			s_TraceExtReports = s_TraceExtReports + 1U;
		}
		else if (pPacket[2] == 0x0fU || pPacket[2] == 0x25U)
		{
			s_TracePeriodicReports = s_TracePeriodicReports + 1U;
		}
		else if ((pPacket[2] == 0x0eU || pPacket[2] == 0x24U) && Length >= 6U)
		{
			HciTrace("lm20 SDC sync sub=%02x status=%02x handle=%02x%02x\r\n",
				(unsigned)pPacket[2], (unsigned)pPacket[3],
				(unsigned)pPacket[5], (unsigned)pPacket[4]);
		}
		else if (pPacket[2] == 0x10U && Length >= 5U)
		{
			HciTrace("lm20 SDC sync lost handle=%02x%02x\r\n",
				(unsigned)pPacket[4], (unsigned)pPacket[3]);
		}
		return;
	}
	if (Length < 6U || (pPacket[0] != 0x0eU && pPacket[0] != 0x0fU))
	{
		return;
	}
	if (pPacket[0] == 0x0eU)
	{
		opcode = pPacket[3] | ((unsigned)pPacket[4] << 8);
		status = pPacket[5];
	}
	else
	{
		opcode = pPacket[4] | ((unsigned)pPacket[5] << 8);
		status = pPacket[2];
	}
	if (opcode == 0x2044U || opcode == 0x2045U || opcode == 0x2046U ||
		opcode == 0x2041U || opcode == 0x2042U || opcode == 0x2084U)
	{
		HciTrace("lm20 reply op=%04x status=%02x evt=%02x\r\n",
			opcode, status, (unsigned)pPacket[0]);
	}
}
#endif

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
    for (;;)
    {
        __WFE();
    }
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
#if HCI_LM20_SYNC_TRACE
	s_TraceSdc = s_TraceSdc + 1U;
#endif
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
        pTarget->LastError = -1002;
        return false;
    }
    result = sdc_enable(HciSdcCallback, pTarget->pSdcMem);
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    pTarget->SdcEnabled = true;
    result = HciSdcTimingApply();
    if (result != 0)
    {
        pTarget->LastError = result;
        return false;
    }
    return true;
}
void HciTargetStop(void)
{
    HciNrf54lm20_t *pTarget = s_pTarget;
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
    NVIC_DisableIRQ(SWI00_IRQn);
    NVIC_DisableIRQ(GRTC_3_IRQn);
    NVIC_DisableIRQ(TIMER10_IRQn);
    NVIC_DisableIRQ(RADIO_0_IRQn);
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
#if HCI_LM20_SYNC_TRACE
	s_TraceMpslEnter = s_TraceMpslEnter + 1U;
#endif
    mpsl_low_priority_process();
#if HCI_LM20_SYNC_TRACE
	s_TraceMpslExit = s_TraceMpslExit + 1U;
#endif
}
void HciTargetFault(int error)
{
    s_Target.FaultCount++;
    if (s_Target.LastError == 0)
    {
        s_Target.LastError = error;
    }
}
bool HciTargetInit(HciTaktOs_t *pRuntime, uint8_t *pSdcMem, size_t capacity)
{
    if (pRuntime == nullptr || pSdcMem == nullptr ||
        capacity == 0U || (((uintptr_t)pSdcMem & 7U) != 0U))
    {
        return false;
    }
    memset(&s_Target, 0, sizeof(s_Target));
    s_Target.pRuntime = pRuntime;
    s_Target.pSdcMem = pSdcMem;
    s_Target.SdcMemCapacity = capacity;
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

/* MPSL hardware vectors. Low-priority work is always deferred to TaktOS. */
extern "C" void SWI00_IRQHandler(void)
{
#if HCI_LM20_SYNC_TRACE
	s_TraceSwi = s_TraceSwi + 1U;
#endif
    if (s_pTarget != nullptr && s_pTarget->pRuntime != nullptr)
    {
        HciTaktOsWake(s_pTarget->pRuntime, HCI_TAKTOS_EVENT_MPSL);
    }
}
extern "C" void RADIO_0_IRQHandler(void)
{
#if HCI_LM20_SYNC_TRACE
	s_TraceRadio = s_TraceRadio + 1U;
#endif
    MPSL_IRQ_RADIO_Handler();
}
extern "C" void GRTC_3_IRQHandler(void)
{
#if HCI_LM20_SYNC_TRACE
	s_TraceGrtc = s_TraceGrtc + 1U;
#endif
    MPSL_IRQ_RTC0_Handler();
}
extern "C" void TIMER10_IRQHandler(void)
{
#if HCI_LM20_SYNC_TRACE
	s_TraceTimer = s_TraceTimer + 1U;
#endif
    MPSL_IRQ_TIMER0_Handler();
}
extern "C" void CLOCK_POWER_IRQHandler(void)
{
#if HCI_LM20_SYNC_TRACE
	s_TraceClock = s_TraceClock + 1U;
#endif
    MPSL_IRQ_CLOCK_Handler();
}

/* nRF54L MPSL low-latency window callbacks. */
static void HciConstLatAcquire(void)
{
	if (s_ConstLatRefs++ == 0U)
	{
		/* MPSL needs bounded instruction-fetch latency across radio events.
		 * SystemInit allows RRAM power-off while idle. Preserve that policy
		 * and restore it only after the last low-latency request is released.
		 */
		s_RramLowPowerConfig = NRF_RRAMC->POWER.LOWPOWERCONFIG;
		NRF_RRAMC->POWER.LOWPOWERCONFIG =
			RRAMC_POWER_LOWPOWERCONFIG_MODE_Standby <<
			RRAMC_POWER_LOWPOWERCONFIG_MODE_Pos;
#if NRF_POWER_HAS_CONST_LATENCY
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
#endif
	}
}
static void HciConstLatRelease(void)
{
	if (s_ConstLatRefs > 0U && --s_ConstLatRefs == 0U)
	{
#if NRF_POWER_HAS_CONST_LATENCY
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_LOWPWR);
#endif
		NRF_RRAMC->POWER.LOWPOWERCONFIG = s_RramLowPowerConfig;
	}
}
extern "C" void mpsl_low_latency_acquire_callback(void)
{
    HciConstLatAcquire();
}
extern "C" void mpsl_low_latency_release_callback(void)
{
    HciConstLatRelease();
}
extern "C" void mpsl_constlat_request_callback(void)
{
    HciConstLatAcquire();
}
extern "C" void mpsl_lowpower_request_callback(void)
{
    HciConstLatRelease();
}
