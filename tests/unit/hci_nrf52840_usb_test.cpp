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
static NRF_USBD_Type gUsbd;
NRF_USBD_Type *NRF_USBD = &gUsbd;
static bool gUsbdReadyRaises = true;

UsbdEnableReg &UsbdEnableReg::operator=(uint32_t Setting)
{
    Value = Setting;
    if (Setting != 0U && gUsbdReadyRaises)
    {
        gUsbd.EVENTCAUSE.Value |= USBD_EVENTCAUSE_READY_Msk;
    }
    return *this;
}

#define FAKE_REG_COUNT 8
static uint32_t gFakeAddr[FAKE_REG_COUNT];
static uint32_t gFakeVal[FAKE_REG_COUNT];
static unsigned gFakeUsed;

extern "C" uint32_t *HciTestReg(uint32_t Addr)
{
    for (unsigned i = 0U; i < gFakeUsed; i++)
    {
        if (gFakeAddr[i] == Addr)
        {
            return &gFakeVal[i];
        }
    }

    assert(gFakeUsed < FAKE_REG_COUNT);
    gFakeAddr[gFakeUsed] = Addr;
    gFakeVal[gFakeUsed] = 0U;
    return &gFakeVal[gFakeUsed++];
}

static uint32_t FakeRegPeek(uint32_t Addr)
{
    for (unsigned i = 0U; i < gFakeUsed; i++)
    {
        if (gFakeAddr[i] == Addr)
        {
            return gFakeVal[i];
        }
    }
    return 0xFFFFFFFFU;
}

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

static unsigned gUsbPowerEvents[3];
static unsigned gDcdConnect;
static unsigned gDcdDisconnect;
static unsigned gClockRegisterWrites;
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

extern "C" void tusb_hal_nrf_power_event(uint32_t Event)
{
    assert(Event < 3U);
    gUsbPowerEvents[Event]++;
    gClockRegisterWrites++;
}

extern "C" void dcd_connect(uint8_t rhport) { assert(rhport == 0U); gDcdConnect++; }
extern "C" void dcd_disconnect(uint8_t rhport) { assert(rhport == 0U); gDcdDisconnect++; }

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

static void ResetCounters(void)
{
    memset(gUsbPowerEvents, 0, sizeof(gUsbPowerEvents));
    gDcdConnect = 0U;
    gDcdDisconnect = 0U;
    gClockRegisterWrites = 0U;
    gHfclkRequests = 0U;
    gHfclkReleases = 0U;
    gLowPrioProcess = 0U;
    gHfclkRuns = false;
    gHfclkStartAfter = 0U;
    gUsbdReadyRaises = true;
    gXtalSelected = true;
    gSdcAssert = nullptr;
    gFakeUsed = 0U;
    memset(&gPower, 0, sizeof(gPower));
    memset(&gUsbd, 0, sizeof(gUsbd));
    memset(&gClock, 0, sizeof(gClock));
    memset(&gUarte0, 0, sizeof(gUarte0));
    memset(&gUarte1, 0, sizeof(gUarte1));
    memset(&gP0, 0, sizeof(gP0));
    memset(&gP1, 0, sizeof(gP1));
}

static void TestBringUpOrder(void)
{
    ResetCounters();
    gHfclkStartAfter = 3U;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(gUsbPowerEvents[0] == 0U);
    assert(gUsbPowerEvents[2] == 0U);
    assert(gHfclkRequests == 0U);

    /* The DCD may see an early IRQ, but target accounting stays inactive. */
    assert(HciUsbPlatformIrqEnter() == 0U);

    assert(HciNrf52840UsbStart(&target));
    assert(gHfclkRequests == 1U);
    assert(target.HfclkRequested);
    assert(target.UsbStarted && target.UsbReadyDone);
    assert(gUsbPowerEvents[0] == 0U);
    assert(gUsbPowerEvents[1] == 0U);
    assert(gUsbPowerEvents[2] == 0U);

    assert(gUsbd.ENABLE == 1U);
    assert(gUsbd.USBPULLUP == 1U);
    assert(gUsbd.ISOSPLIT == USBD_ISOSPLIT_SPLIT_HalfIN);
    assert((gUsbd.INTENSET & USBD_INTEN_USBRESET_Msk) != 0U);

    assert(FakeRegPeek(0x4006ED14UL) == 0x00000000UL);
    assert(FakeRegPeek(0x4006EC14UL) == 0x00000000UL);
    assert(FakeRegPeek(NRF_USBD_BASE + 0x800UL) == 0x7E3UL);
    assert(FakeRegPeek(NRF_USBD_BASE + 0x804UL) == 0x40UL);
    assert(gClockRegisterWrites == 0U);
    assert((gPower.INTENSET & POWER_INTENSET_USBPWRRDY_Msk) != 0U);

    gPower.INTENSET = 0U;
    ops.ProcessMpsl(ops.pContext);
    assert((gPower.INTENSET & POWER_INTENSET_USBDETECTED_Msk) != 0U);
    assert((gPower.INTENSET & POWER_INTENSET_USBREMOVED_Msk) != 0U);
    assert((gPower.INTENSET & POWER_INTENSET_USBPWRRDY_Msk) != 0U);

    HciNrf52840UsbPassMark(&target);
    assert(HciUsbPlatformIrqEnter() == 1U);
    assert(target.UsbIrqCount == 1U);

    gPower.EVENTS_USBREMOVED = 1U;
    POWER_CLOCK_IRQHandler();
    assert(gUsbPowerEvents[1] == 0U);
    assert(gClockRegisterWrites == 0U);
    assert(target.UsbDetachPending);
    assert(gDcdDisconnect == 0U);

    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdDisconnect == 1U);
    assert(!target.UsbDetachPending);

    gPower.EVENTS_USBDETECTED = 1U;
    POWER_CLOCK_IRQHandler();
    assert(target.UsbAttachPending);
    assert(gDcdConnect == 0U);

    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdConnect == 1U);
    assert(gUsbPowerEvents[0] == 0U);

    gPower.EVENTS_USBREMOVED = 1U;
    POWER_CLOCK_IRQHandler();
    gPower.EVENTS_USBDETECTED = 1U;
    POWER_CLOCK_IRQHandler();
    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdDisconnect == 2U && gDcdConnect == 2U);

    HciNrf52840Stop(&target);
    assert(gHfclkReleases == 1U);
    assert(!target.HfclkRequested && !target.UsbStarted);
    assert(!target.SdcInitialized);
    assert(gUsbd.USBPULLUP == 0U);
    assert(gUsbd.ENABLE == 0U);
    assert(gUsbd.INTEN == 0U);

    assert(ops.Start(ops.pContext));
    HciNrf52840Stop(&target);

    printf("[ok] bring up order and clock ownership\n");
}

static void TestHfclkTimeoutDoesNotHang(void)
{
    ResetCounters();
    gHfclkStartAfter = 0U;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(target.LastError == -1000);
    assert(!target.UsbStarted);
    assert(gHfclkRequests == 1U);
    assert(gHfclkReleases == 1U);
    assert(!target.HfclkRequested);
    assert(gUsbPowerEvents[0] == 0U && gUsbPowerEvents[2] == 0U);

    gHfclkStartAfter = gLowPrioProcess + 1U;
    assert(HciNrf52840UsbStart(&target));
    assert(gHfclkRequests == 2U);
    assert(target.HfclkRequested);

    ops.Fault(ops.pContext, -1);
    assert(target.LastError == -1000);
    assert(target.FaultCount == 1U);

    HciNrf52840Stop(&target);
    assert(gHfclkReleases == 2U);
    printf("[ok] crystal timeout releases its request and retry re-requests it\n");
}

static void TestUsbRegulatorTimeout(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(target.LastError == -1001);
    assert(!target.UsbReadyDone);
    assert(!target.UsbStarted);
    assert(gUsbd.ENABLE == 0U);
    assert(gUsbd.USBPULLUP == 0U);
    assert(gUsbd.INTEN == 0U);
    assert(gHfclkReleases == 1U);
    assert(!target.HfclkRequested);
    assert(gUsbPowerEvents[0] == 0U);

    gPower.USBREGSTATUS = 0U;
    assert(!HciNrf52840UsbStart(&target));

    HciNrf52840Stop(&target);
    printf("[ok] regulator wait is bounded and reports its own code\n");
}

static void TestNoVbusIsRejected(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gPower.USBREGSTATUS = 0U;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(gUsbd.ENABLE == 0U);
    assert(gUsbd.USBPULLUP == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] no vbus is rejected without enabling the controller\n");
}

static void TestUsbdReadyTimeout(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gUsbdReadyRaises = false;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(target.LastError == -1002);
    assert(gUsbd.USBPULLUP == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] controller ready wait is bounded\n");
}

static void TestHfxoNotOnCrystal(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gXtalSelected = false;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(target.LastError == -1003);
    assert(gUsbd.USBPULLUP == 0U);
    assert(gClockRegisterWrites == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] crystal select mismatch is caught, not spun on\n");
}

static void TestUsbIrqBookkeeping(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), true));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));
    assert(HciNrf52840UsbStart(&target));

    HciNrf52840UsbPassMark(&target);
    assert(HciUsbPlatformIrqEnter() == 1U);
    assert(HciUsbPlatformIrqEnter() == 2U);
    assert(target.UsbIrqCount == 2U);

    HciUsbPlatformIrqUnexpectedCause(USBD_EVENTCAUSE_READY_Msk);
    assert(target.UsbStuckCauseCount == 1U);
    assert((target.UsbEventCause & USBD_EVENTCAUSE_READY_Msk) != 0U);

    HciUsbPlatformIrqStorm(0x00000001U,
                           USBD_EVENTCAUSE_READY_Msk,
                           0x00000001U);
    assert(target.UsbStormEvents == 0x00000001U);
    assert(target.UsbStormInten == 0x00000001U);
    assert(target.UsbStormCause == USBD_EVENTCAUSE_READY_Msk);
    assert(target.LastError == -1006);
    assert(!target.UsbStarted);
    assert(!target.UsbReadyDone);

    HciNrf52840UsbPowerProcess(&target);
    assert(!target.HfclkRequested);
    assert(gUsbd.ENABLE == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] usb irq bookkeeping stays outside the register dispatcher\n");
}

static void TestUartModeLeavesUsbAlone(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    gPower.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk |
                          POWER_USBREGSTATUS_OUTPUTRDY_Msk;

    alignas(8) static uint8_t mem[10000];
    HciTaktOs_t runtime = {};
    HciNrf52840_t target;
    HciTaktOsOps_t ops = {};

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), false));
    HciNrf52840GetTaktOsOps(&target, &ops);
    assert(ops.Start(ops.pContext));

    assert(!HciNrf52840UsbStart(&target));
    assert(gHfclkRequests == 0U);
    assert(gUsbPowerEvents[0] == 0U);

    gPower.EVENTS_USBREMOVED = 1U;
    POWER_CLOCK_IRQHandler();
    assert(gDcdDisconnect == 0U);
    assert(HciUsbPlatformIrqEnter() == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] uart mode leaves the usb hardware alone\n");
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

    assert(HciNrf52840Init(&target, &runtime, mem, sizeof(mem), false));
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
    TestBringUpOrder();
    TestHfclkTimeoutDoesNotHang();
    TestUsbRegulatorTimeout();
    TestNoVbusIsRejected();
    TestUsbdReadyTimeout();
    TestHfxoNotOnCrystal();
    TestUsbIrqBookkeeping();
    TestUartModeLeavesUsbAlone();
    TestUartTraceUsesDatasheetPinNames();
    TestResetTraceKeepsSdcAssert();
    printf("All nRF52840 USB bring up tests passed.\n");
    return 0;
}
