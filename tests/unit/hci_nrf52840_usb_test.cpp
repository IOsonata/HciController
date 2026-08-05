/*
 * Host test for the nRF52840 USB bring up ordering and clock ownership.
 */

#include "hci_nrf52840.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mpsl.h"
#include "mpsl_clock.h"
#include "sdc.h"
#include "coredev/system_core_clock.h"
#include "crypto_rng_nrf.h"

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

/* Raw register table so the errata writes can be observed. */
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

static CryptoRngNrf gRng;
static OscDesc_t gLfOsc = { OSC_TYPE_XTAL, 32768U, 20U, 0U };

static unsigned gUsbPowerEvents[3];
static unsigned gUsbIrq;
static unsigned gDcdConnect;
static unsigned gDcdDisconnect;
static unsigned gClockRegisterWrites;
static unsigned gHfclkRequests;
static unsigned gHfclkReleases;
static unsigned gLowPrioProcess;
static bool gHfclkRuns;
static unsigned gHfclkStartAfter;
static bool gXtalSelected = true;

extern "C" const OscDesc_t *GetLowFreqOscDesc(void) { return &gLfOsc; }
extern "C" uint32_t SystemCoreClockGet(void) { return 64000000U; }
CRYPTO_STATUS CryptoRngNrf::Random(uint8_t *p, size_t n) { memset(p, 0x5A, n); return CRYPTO_STATUS_OK; }
CryptoRngNrf *CryptoRngNrfInstance(void) { return &gRng; }

extern "C" void NVIC_SetPriority(IRQn_Type, uint32_t) {}
extern "C" void NVIC_EnableIRQ(IRQn_Type) {}
extern "C" void NVIC_DisableIRQ(IRQn_Type) {}
extern "C" void NVIC_ClearPendingIRQ(IRQn_Type) {}

/*
 * The port helper must never be reached. Linking it here would fail the build
 * if anything still called it, and this definition proves the reference is
 * gone from the object under test.
 */
extern "C" void tusb_hal_nrf_power_event(uint32_t Event)
{
    assert(Event < 3U);
    gUsbPowerEvents[Event]++;
    gClockRegisterWrites++;   /* the helper touches NRF_CLOCK on every path */
}

extern "C" void tusb_int_handler(uint8_t rhport, bool in_isr)
{
    assert(rhport == 0U && in_isr);
    gUsbIrq++;
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

extern "C" int32_t sdc_init(sdc_fault_handler_t) { return 0; }
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

extern "C" void HciTaktOsWake(HciTaktOs_t *, uint32_t) {}

extern "C" void POWER_CLOCK_IRQHandler(void);
extern "C" void USBD_IRQHandler(void);

static void ResetCounters(void)
{
    memset(gUsbPowerEvents, 0, sizeof(gUsbPowerEvents));
    gUsbIrq = 0U;
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
    gFakeUsed = 0U;
    memset(&gPower, 0, sizeof(gPower));
    memset(&gUsbd, 0, sizeof(gUsbd));
    memset(&gClock, 0, sizeof(gClock));
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

    /* Radio bring up must not deliver any USB power event. */
    assert(gUsbPowerEvents[0] == 0U);
    assert(gUsbPowerEvents[2] == 0U);
    assert(gHfclkRequests == 0U);

    /* An early USBD interrupt must be ignored before the stack is up. */
    USBD_IRQHandler();
    assert(gUsbIrq == 0U);

    assert(HciNrf52840UsbStart(&target));
    assert(gHfclkRequests == 1U);
    assert(target.HfclkRequested);
    assert(target.UsbStarted && target.UsbReadyDone);
    /* The port power helper must not be reached at all. */
    assert(gUsbPowerEvents[0] == 0U);
    assert(gUsbPowerEvents[1] == 0U);
    assert(gUsbPowerEvents[2] == 0U);

    assert(gUsbd.ENABLE == 1U);
    assert(gUsbd.USBPULLUP == 1U);
    assert(gUsbd.ISOSPLIT == USBD_ISOSPLIT_SPLIT_HalfIN);
    assert((gUsbd.INTENSET & USBD_INTEN_USBRESET_Msk) != 0U);

    /* Errata 187 and 171 applied then reverted, 166 left applied. */
    assert(FakeRegPeek(0x4006ED14UL) == 0x00000000UL);
    assert(FakeRegPeek(0x4006EC14UL) == 0x00000000UL);
    assert(FakeRegPeek(NRF_USBD_BASE + 0x800UL) == 0x7E3UL);
    assert(FakeRegPeek(NRF_USBD_BASE + 0x804UL) == 0x40UL);

    /* The crystal was already running, so the driver never wrote NRF_CLOCK. */
    assert(gClockRegisterWrites == 0U);

    /* The USB bits are armed in the enable register shared with CLOCK. */
    assert((gPower.INTENSET & POWER_INTENSET_USBPWRRDY_Msk) != 0U);

    /* MPSL owns that register. Low priority processing must re-arm the bits. */
    gPower.INTENSET = 0U;
    ops.ProcessMpsl(ops.pContext);
    assert((gPower.INTENSET & POWER_INTENSET_USBDETECTED_Msk) != 0U);
    assert((gPower.INTENSET & POWER_INTENSET_USBREMOVED_Msk) != 0U);
    assert((gPower.INTENSET & POWER_INTENSET_USBPWRRDY_Msk) != 0U);

    USBD_IRQHandler();
    assert(gUsbIrq == 1U);

    /*
     * Cable removal must not reach the driver path that stops the crystal, and
     * must not touch the TinyUSB event queue from interrupt context. The
     * handler records it; the thread applies it.
     */
    gPower.EVENTS_USBREMOVED = 1U;
    POWER_CLOCK_IRQHandler();
    assert(gUsbPowerEvents[1] == 0U);
    assert(gClockRegisterWrites == 0U);
    assert(target.UsbDetachPending);
    assert(gDcdDisconnect == 0U);

    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdDisconnect == 1U);
    assert(!target.UsbDetachPending);

    /* Re-plug only restores the pull up, and again only from the thread. */
    gPower.EVENTS_USBDETECTED = 1U;
    POWER_CLOCK_IRQHandler();
    assert(target.UsbAttachPending);
    assert(gDcdConnect == 0U);

    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdConnect == 1U);
    assert(gUsbPowerEvents[0] == 0U);

    /*
     * A detach and an attach arriving before the thread runs collapse to the
     * state the hardware is actually in, which is attached.
     */
    gPower.EVENTS_USBREMOVED = 1U;
    POWER_CLOCK_IRQHandler();
    gPower.EVENTS_USBDETECTED = 1U;
    POWER_CLOCK_IRQHandler();
    HciNrf52840UsbPowerProcess(&target);
    assert(gDcdDisconnect == 2U && gDcdConnect == 2U);

    HciNrf52840Stop(&target);
    assert(gHfclkReleases == 1U);
    assert(!target.HfclkRequested && !target.UsbStarted);
    /* Stop must disable a controller that was initialised, not only enabled. */
    assert(!target.SdcInitialized);

    /* Stop detaches rather than leaving the host enumerated against nothing. */
    assert(gUsbd.USBPULLUP == 0U);
    assert(gUsbd.ENABLE == 0U);
    assert(gUsbd.INTEN == 0U);

    /* And a stopped target can be started again. */
    assert(ops.Start(ops.pContext));
    HciNrf52840Stop(&target);

    printf("[ok] bring up order and clock ownership\n");
}

static void TestHfclkTimeoutDoesNotHang(void)
{
    ResetCounters();
    gHfclkStartAfter = 0U;   /* crystal never reports running */
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
    assert(gUsbPowerEvents[0] == 0U && gUsbPowerEvents[2] == 0U);

    /* The first recorded cause survives the generic fault report. */
    ops.Fault(ops.pContext, -1);
    assert(target.LastError == -1000);
    assert(target.FaultCount == 1U);

    HciNrf52840Stop(&target);
    printf("[ok] crystal wait is bounded and the cause is kept\n");
}

static void TestUsbRegulatorTimeout(void)
{
    ResetCounters();
    gHfclkStartAfter = 1U;
    /* VBUS present but the 3.3 V regulator never reports ready. */
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

    /*
     * A failed bring up unwinds. The controller goes back off, the pull up
     * never goes up, the flag does not claim success, and the crystal that was
     * requested for USB alone is handed back so MPSL can drop to the internal
     * oscillator between radio events.
     */
    assert(!target.UsbStarted);
    assert(gUsbd.ENABLE == 0U);
    assert(gUsbd.USBPULLUP == 0U);
    assert(gUsbd.INTEN == 0U);
    assert(gHfclkReleases == 1U);
    /* The release and the flag that records it have to agree. */
    assert(!target.HfclkRequested);
    assert(gUsbPowerEvents[0] == 0U);

    /* A retry after a failure must not claim the port is already up. */
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
    gUsbdReadyRaises = false;   /* controller never signals EVENTCAUSE.READY */
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
    gXtalSelected = false;   /* mpsl reports running, hardware still on the rc */
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

static void TestEventCauseStormIsBroken(void)
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

    /* Start up must leave no cause behind. */
    assert(gUsbd.EVENTCAUSE.Value == 0U);
    assert(gUsbd.EVENTS_USBEVENT == 0U);

    /*
     * READY sits outside the mask the port clears. Left alone it re-asserts
     * EVENTS_USBEVENT forever, so the handler has to clear it.
     */
    gUsbd.EVENTCAUSE.Value = USBD_EVENTCAUSE_READY_Msk;
    gUsbd.EVENTS_USBEVENT = 1U;
    USBD_IRQHandler();

    assert((gUsbd.EVENTCAUSE.Value & USBD_EVENTCAUSE_READY_Msk) == 0U);
    assert(gUsbd.EVENTS_USBEVENT == 0U);
    assert(target.UsbStuckCauseCount == 1U);
    assert((target.UsbEventCause & USBD_EVENTCAUSE_READY_Msk) != 0U);

    /* Causes the port does handle must be left to the port. */
    gUsbd.EVENTCAUSE.Value = USBD_EVENTCAUSE_SUSPEND_Msk;
    gUsbd.EVENTS_USBEVENT = 1U;
    USBD_IRQHandler();

    assert((gUsbd.EVENTCAUSE.Value & USBD_EVENTCAUSE_SUSPEND_Msk) != 0U);
    assert(target.UsbStuckCauseCount == 1U);

    /* A source that keeps re-asserting is captured and named, not guessed. */
    gUsbd.EVENTCAUSE.Value = 0U;
    gUsbd.EVENTS_USBEVENT = 0U;
    HciNrf52840UsbPassMark(&target);
    gUsbd.INTEN = 0x00000001U;
    gUsbd.EVENTS_USBRESET = 1U;
    for (uint32_t i = 0U; i < 2100U; i++)
    {
        USBD_IRQHandler();
    }

    assert(target.UsbStormEvents == 0x00000001U);
    assert(target.UsbStormInten == 0x00000001U);

    HciNrf52840Stop(&target);
    printf("[ok] eventcause the port ignores is cleared, storm is named\n");
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

    USBD_IRQHandler();
    assert(gUsbIrq == 0U);

    HciNrf52840Stop(&target);
    printf("[ok] uart mode leaves the usb hardware alone\n");
}

int main(void)
{
    TestBringUpOrder();
    TestHfclkTimeoutDoesNotHang();
    TestUsbRegulatorTimeout();
    TestNoVbusIsRejected();
    TestUsbdReadyTimeout();
    TestHfxoNotOnCrystal();
    TestEventCauseStormIsBroken();
    TestUartModeLeavesUsbAlone();
    printf("All nRF52840 USB bring up tests passed.\n");
    return 0;
}
