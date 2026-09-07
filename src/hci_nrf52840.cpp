/**-------------------------------------------------------------------------
@file	hci_nrf52840.cpp

@brief	nRF52840 MPSL, SDC, USB, and target lifecycle implementation.

		Initializes MPSL and SDC resources, manages nRF52840 USB power and
		clock sequencing, handles target interrupts, entropy, reset diagnostics,
		and platform-specific runtime hooks.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

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
#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"
#include "hci_trace.h"

#ifndef HCI_NRF52840_LOW_IRQ_PRIORITY
#define HCI_NRF52840_LOW_IRQ_PRIORITY 7U
#endif

#ifndef HCI_NRF52840_CLOCK_IRQ_PRIORITY
#define HCI_NRF52840_CLOCK_IRQ_PRIORITY 4U
#endif

/* Bounded wait for the crystal. Worst case ramp-up is 1400 us. */
#ifndef HCI_NRF52840_HFCLK_WAIT_LOOPS
#define HCI_NRF52840_HFCLK_WAIT_LOOPS 1000000U
#endif

/*
 * Attempts allowed when proving the entropy source at start up. This bounds a
 * start up check, not the SDC callback, which must block per sdc_soc.h.
 */
#ifndef HCI_NRF52840_RAND_PROBE_LOOPS
#define HCI_NRF52840_RAND_PROBE_LOOPS 1000U
#endif

#define HCI_NRF52840_ERR_HFCLK_TIMEOUT      (-1000)
/* The entropy source SDC requires is absent or produces nothing. */
#define HCI_NRF52840_ERR_NO_ENTROPY         (-1005)

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

/* The IOsonata USB controller owns USBD; MPSL owns its crystal. */
extern "C" bool UsbdXtalRequest(void)
{
    return s_pTarget != nullptr && s_pTarget->MpslInitialized &&
           HciNrf52840HfclkStart(s_pTarget);
}

extern "C" void UsbdXtalRelease(void)
{
    if (s_pTarget != nullptr && s_pTarget->HfclkRequested)
    {
        (void)HciNrf52840HfclkRelease();
        s_pTarget->HfclkRequested = false;
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
        HciNrf52840Stop(pTarget);
        return false;
    }

    return true;
}

static void HciNrf52840ProcessMpsl(void *pContext)
{
    mpsl_low_priority_process();
    (void)pContext;
}

static void HciNrf52840Fault(void *pContext, int Error)
{
    HciNrf52840_t *pTarget = static_cast<HciNrf52840_t *>(pContext);
    if (pTarget != nullptr)
    {
        pTarget->FaultCount++;
        if (pTarget->LastError == 0)
        {
            pTarget->LastError = Error;
        }
    }
}


bool HciNrf52840Init(HciNrf52840_t *pTarget,
                     HciTaktOs_t *pRuntime,
                     uint8_t *pSdcMem,
                     size_t SdcMemCapacity)
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
}


/*
 * The target interface. Thin wrappers rather than casting the function
 * pointers, so the compiler checks each signature rather than trusting a cast.
 */
static bool HciNrf52840TargetInit(void *pContext,
                                  HciTaktOs_t *pRuntime,
                                  uint8_t *pSdcMem,
                                  size_t SdcMemCapacity)
{
    return HciNrf52840Init(static_cast<HciNrf52840_t *>(pContext), pRuntime,
                           pSdcMem, SdcMemCapacity);
}

static void HciNrf52840TargetGetTaktOsOps(void *pContext,
                                          HciTaktOsOps_t *pOps)
{
    HciNrf52840GetTaktOsOps(static_cast<HciNrf52840_t *>(pContext), pOps);
}


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
    HciNrf52840TargetUartTrace,
    HciNrf52840TargetStop,
    HciNrf52840TargetGetSdcMem,
    HciNrf52840TargetLastError,
};

static HciNrf52840_t s_Nrf52840;

HciTarget_t HciNrf52840Target(void)
{
    HciTarget_t target = { &s_Nrf52840Ops, &s_Nrf52840 };
    return target;
}
