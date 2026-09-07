/*
 * Host test for the nRF52840 USB bring up ordering and clock ownership.
 */

#include "hci_nrf52840.h"
#include "hci_sdc_resources.h"
#include "hci_target.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mpsl.h"
#include "mpsl_clock.h"
#include "sdc.h"
#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"

#include "hci_sdc_expected_resources.h"

extern "C" void HciTraceTestReset(void);
extern "C" size_t HciTraceTestTake(char *pOut, size_t Capacity);

static_assert(HCI_SDC_MEM_REQUIRED == EXPECT_REQUIRED,
              "the fake sdc.h and the vendor one disagree about the pool");

static NRF_POWER_Type gPower;
NRF_POWER_Type *NRF_POWER = &gPower;

extern "C" bool nrf52_errata_166(void) { return true; }
extern "C" bool nrf52_errata_171(void) { return true; }
extern "C" bool nrf52_errata_187(void) { return true; }
static NRF_CLOCK_Type gClock;
NRF_CLOCK_Type *NRF_CLOCK = &gClock;

static NRF_UARTE_Type gUarte0;
static NRF_UARTE_Type gUarte1;
NRF_UARTE_Type *NRF_UARTE0 = &gUarte0;
NRF_UARTE_Type *NRF_UARTE1 = &gUarte1;

static NRF_GPIO_Type gP0;
static NRF_GPIO_Type gP1;
NRF_GPIO_Type *NRF_P0 = &gP0;
NRF_GPIO_Type *NRF_P1 = &gP1;

static CryptoRngNrf gRng;
static OscDesc_t gLfOsc = { OSC_TYPE_XTAL, 32768U, 20U, 0U };

static unsigned gHfclkRequests;
static unsigned gHfclkReleases;
static unsigned gLowPrioProcess;
static bool gHfclkRuns;
static unsigned gHfclkStartAfter;
static bool gXtalSelected = true;
static sdc_fault_handler_t gSdcAssert;

extern "C" const OscDesc_t *GetLowFreqOscDesc(void) { return &gLfOsc; }
extern "C" uint32_t SystemCoreClockGet(void) { return 64000000U; }
CRYPTO_STATUS CryptoRngNrf::Random(uint8_t *p, size_t n) { memset(p, 0x5A, n); return CRYPTO_STATUS_OK; }
CryptoRngNrf *CryptoRngNrfInstance(void) { return &gRng; }

extern "C" void NVIC_SetPriority(IRQn_Type, uint32_t) {}
extern "C" void NVIC_EnableIRQ(IRQn_Type) {}
extern "C" void NVIC_DisableIRQ(IRQn_Type) {}
extern "C" void NVIC_ClearPendingIRQ(IRQn_Type) {}

extern "C" int32_t mpsl_init(const mpsl_clock_lfclk_cfg_t *pCfg, IRQn_Type Irq, mpsl_assert_handler_t)
{
    assert(pCfg->source == MPSL_CLOCK_LF_SRC_XTAL);
    assert(Irq == SWI5_EGU5_IRQn);
    return 0;
}
extern "C" void mpsl_uninit(void) {}
extern "C" void mpsl_low_priority_process(void)
{
    gLowPrioProcess++;
    if (gHfclkStartAfter != 0U && gLowPrioProcess >= gHfclkStartAfter)
    {
        gHfclkRuns = true;
        if (gXtalSelected)
        {
            gClock.HFCLKSTAT = CLOCK_HFCLKSTAT_STATE_Msk |
                               (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos);
        }
    }
}
extern "C" void MPSL_IRQ_RADIO_Handler(void) {}
extern "C" void MPSL_IRQ_RTC0_Handler(void) {}
extern "C" void MPSL_IRQ_TIMER0_Handler(void) {}
extern "C" void MPSL_IRQ_CLOCK_Handler(void) {}

extern "C" int32_t mpsl_clock_hfclk_request(mpsl_clock_hfclk_callback_t) { return 0; }
extern "C" int32_t mpsl_clock_hfclk_release(void) { return 0; }
extern "C" int32_t mpsl_clock_hfclk_is_running(uint32_t *p) { *p = gHfclkRuns; return 0; }

extern "C" int32_t mpsl_clock_hfclk_src_request(mpsl_clock_hfclk_src_t Src,
                                                mpsl_clock_hfclk_request_callback_t)
{
    assert(Src == MPSL_CLOCK_HF_SRC_XO);
    gHfclkRequests++;
    return 0;
}
extern "C" int32_t mpsl_clock_hfclk_src_release(mpsl_clock_hfclk_src_t) { gHfclkReleases++; return 0; }
extern "C" int32_t mpsl_clock_hfclk_src_is_running(mpsl_clock_hfclk_src_t, uint32_t *p)
{
    *p = gHfclkRuns ? 1U : 0U;
    return 0;
}

extern "C" int32_t sdc_init(sdc_fault_handler_t Handler)
{
    gSdcAssert = Handler;
    return 0;
}
extern "C" int32_t sdc_rand_source_register(const sdc_rand_source_t *) { return 0; }
extern "C" int32_t sdc_cfg_set(uint8_t, uint8_t, const sdc_cfg_t *) { return 4096; }
extern "C" int32_t sdc_enable(sdc_callback_t, uint8_t *) { return 0; }
extern "C" int32_t sdc_disable(void) { return 0; }
extern "C" void sdc_support_ext_adv(void) {}
extern "C" void sdc_support_peripheral(void) {}
extern "C" void sdc_support_ext_central(void) {}
extern "C" void sdc_support_le_2m_phy(void) {}
extern "C" void sdc_support_le_coded_phy(void) {}
extern "C" void sdc_support_dle_peripheral(void) {}
extern "C" void sdc_support_dle_central(void) {}
extern "C" void sdc_support_phy_update_peripheral(void) {}
extern "C" void sdc_support_phy_update_central(void) {}
extern "C" void sdc_support_direct_test_mode(void) {}
extern "C" void sdc_support_le_privacy(void) {}
extern "C" void sdc_support_qos_channel_survey(void) {}
extern "C" void sdc_support_le_power_control_central(void) {}
extern "C" void sdc_support_le_power_control_peripheral(void) {}
extern "C" void sdc_support_le_path_loss_monitoring(void) {}
extern "C" void sdc_support_sca_central(void) {}
extern "C" void sdc_support_sca_peripheral(void) {}
extern "C" void sdc_support_connection_subrating_central(void) {}
extern "C" void sdc_support_connection_subrating_peripheral(void) {}
extern "C" void sdc_support_extended_feature_set_central(void) {}
extern "C" void sdc_support_extended_feature_set_peripheral(void) {}
extern "C" void sdc_support_parallel_scanning_and_initiating(void) {}
extern "C" void sdc_support_le_periodic_adv(void) {}
extern "C" void sdc_support_le_periodic_sync(void) {}
extern "C" void sdc_support_periodic_adv_sync_transfer_sender_central(void) {}
extern "C" void sdc_support_periodic_adv_sync_transfer_sender_peripheral(void) {}
extern "C" void sdc_support_periodic_adv_sync_transfer_receiver_central(void) {}
extern "C" void sdc_support_periodic_adv_sync_transfer_receiver_peripheral(void) {}
extern "C" void sdc_support_le_periodic_adv_with_rsp(void) {}
extern "C" void sdc_support_le_periodic_sync_with_rsp(void) {}
extern "C" void sdc_support_cis_central(void) {}
extern "C" void sdc_support_cis_peripheral(void) {}
extern "C" void sdc_support_bis_source(void) {}
extern "C" void sdc_support_bis_sink(void) {}

extern "C" void HciTaktOsWake(HciTaktOs_t *, uint32_t) {}

extern "C" void POWER_CLOCK_IRQHandler(void);
extern "C" uint32_t HciUsbPlatformIrqEnter(void);
extern "C" void HciUsbPlatformIrqUnexpectedCause(uint32_t Cause);
extern "C" void HciUsbPlatformIrqStorm(uint32_t Inten,
                                       uint32_t Cause,
                                       uint32_t Events);
extern "C" bool UsbdXtalRequest(void);
extern "C" void UsbdXtalRelease(void);

static void ResetCounters(void)
{
    gHfclkRequests = 0U;
    gHfclkReleases = 0U;
    gLowPrioProcess = 0U;
    gHfclkRuns = false;
    gHfclkStartAfter = 0U;
    gXtalSelected = true;
    gSdcAssert = nullptr;
    memset(&gPower, 0, sizeof(gPower));
    memset(&gClock, 0, sizeof(gClock));
    memset(&gUarte0, 0, sizeof(gUarte0));
    memset(&gUarte1, 0, sizeof(gUarte1));
    memset(&gP0, 0, sizeof(gP0));
    memset(&gP1, 0, sizeof(gP1));
}


static void TestUsbClockOwnership(void)
{
    ResetCounters();
    gHfclkStartAfter = 3U;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem)));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(UsbdXtalRequest());
    assert(target.HfclkRequested);
    assert(gHfclkRequests == 1U);
    assert(UsbdXtalRequest());
    assert(gHfclkRequests == 1U);

    UsbdXtalRelease();
    assert(!target.HfclkRequested);
    assert(gHfclkReleases == 1U);

    gHfclkRuns = false;
    gHfclkStartAfter = 0U;
    assert(!UsbdXtalRequest());
    assert(!target.HfclkRequested);
    assert(gHfclkRequests == 2U);
    assert(gHfclkReleases == 2U);

    HciNrf52840Stop(&target);
    printf("[ok] IOsonata USB crystal ownership is routed through MPSL\n");
}

static void TestUartTraceUsesDatasheetPinNames(void)
{
    char text[512];
    const HciTarget_t target = HciNrf52840Target();

    gUarte0.ENABLE = 8U;
    gUarte0.BAUDRATE = 0x10000000UL;
    gUarte0.ERRORSRC = 0U;
    gUarte0.CONFIG = 1U;
    gUarte0.PSEL.RXD = 32U;
    gUarte0.PSEL.TXD = 25U;
    gUarte0.PSEL.CTS = 22U;
    gUarte0.PSEL.RTS = 19U;
    gP0.IN = 0U;
    gP1.IN = 0xFFFFFFFFU;

    HciTraceTestReset();
    HciTargetUartTrace(&target, 0U);
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "uart0: enabled=yes") != NULL);
    assert(strstr(text, "hwfc=on") != NULL);
    assert(strstr(text, "baud=1000000") != NULL);
    assert(strstr(text, "errors=none") != NULL);
    assert(strstr(text, "cts=low(peer-ready)") != NULL);
    assert(strstr(text, "RXD=P1.00") != NULL);
    assert(strstr(text, "TXD=P0.25") != NULL);
    assert(strstr(text, "CTS=P0.22") != NULL);
    assert(strstr(text, "RTS=P0.19") != NULL);

    gP0.IN = 1UL << 22;
    HciTraceTestReset();
    HciTargetUartTrace(&target, 0U);
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "cts=high(peer-not-ready)") != NULL);

    gUarte0.PSEL.CTS = 32U;
    gP0.IN = 0xFFFFFFFFU;
    gP1.IN = 0U;
    HciTraceTestReset();
    HciTargetUartTrace(&target, 0U);
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "cts=low(peer-ready)") != NULL);
    assert(strstr(text, "CTS=P1.00") != NULL);

    gUarte0.PSEL.CTS = 0x80000000UL;
    HciTraceTestReset();
    HciTargetUartTrace(&target, 0U);
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "cts=not-connected") != NULL);
    assert(strstr(text, "CTS=NC") != NULL);

    gUarte1.ENABLE = 8U;
    gUarte1.PSEL.CTS = 0x80000000UL;
    HciTraceTestReset();
    HciTargetUartTrace(&target, 1U);
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "uart1: enabled=yes") != NULL);

    printf("[ok] uart trace uses datasheet pin names and decoded settings\n");
}

static void TestResetTraceKeepsSdcAssert(void)
{
    ResetCounters();

    char text[512];
    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem)));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));
    assert(gSdcAssert != nullptr);

    gSdcAssert("controller_fault.c", 321U);
    gPower.RESETREAS = POWER_RESETREAS_SREQ_Msk;

    HciTraceTestReset();
    HciNrf52840ResetTrace();
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "cause=software-reset") != NULL);
    assert(strstr(text, "previous=SDC-assert") != NULL);
    assert(strstr(text, "file=controller_fault.c") != NULL);
    assert(strstr(text, "line=321") != NULL);

    HciTraceTestReset();
    HciNrf52840ResetTrace();
    (void)HciTraceTestTake(text, sizeof(text));
    assert(strstr(text, "previous=SDC-assert") == NULL);

    HciNrf52840Stop(&target);
    printf("[ok] reset trace keeps the SDC assertion across reset state\n");
}

int main(void)
{
    TestUsbClockOwnership();
    TestUartTraceUsesDatasheetPinNames();
    TestResetTraceKeepsSdcAssert();
    printf("All nRF52840 target tests passed.\n");
    return 0;
}
