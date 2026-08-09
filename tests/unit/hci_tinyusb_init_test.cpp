/*
 * Host test for the device stack bring up.
 *
 * tud_init is a static inline that calls tud_rhport_init directly and never
 * reaches tusb.c, so the roothub port role is left at TUSB_ROLE_INVALID.
 * tusb_int_handler dispatches on that role, so with tud_init the interrupt
 * handler silently does nothing: no USBD event is ever cleared and no setup
 * packet is ever seen. This test pins the correct entry point.
 */

#include "hci_tinyusb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"

static unsigned gRhportInitCalls;
static uint8_t gRhportInitPort;
static tusb_role_t gRhportInitRole;
static tusb_speed_t gRhportInitSpeed;
static bool gRhportInitResult = true;

static unsigned gTaskCalls;
static uint8_t gTaskPort;
static bool gMounted;
static uint8_t gLineState;
static unsigned gWakeCalls;

/*
 * Models the role table in tusb.c. tusb_int_handler only reaches
 * dcd_int_handler when the role was recorded by tusb_rhport_init.
 */
static tusb_role_t gRhportRole[1];
static unsigned gDcdHandlerCalls;

extern "C" bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t *pInit)
{
    gRhportInitCalls++;
    gRhportInitPort = rhport;
    gRhportInitRole = pInit->role;
    gRhportInitSpeed = pInit->speed;

    if (!gRhportInitResult)
    {
        return false;
    }

    gRhportRole[rhport] = pInit->role;
    return true;
}

extern "C" bool tusb_inited(void) { return gRhportRole[0] != TUSB_ROLE_INVALID; }

extern "C" void tusb_int_handler(uint8_t rhport, bool)
{
    if (gRhportRole[rhport] == TUSB_ROLE_DEVICE)
    {
        gDcdHandlerCalls++;
    }
}

extern "C" void tud_task_ext(uint8_t rhport, bool) { gTaskCalls++; gTaskPort = rhport; }
extern "C" bool tud_mounted(void) { return gMounted; }
extern "C" uint8_t tud_cdc_n_get_line_state(uint8_t) { return gLineState; }
extern "C" uint32_t tud_cdc_n_available(uint8_t) { return 0U; }
extern "C" uint32_t tud_cdc_n_read(uint8_t, void *, uint32_t) { return 0U; }
extern "C" uint32_t tud_cdc_n_write_available(uint8_t) { return 0U; }
extern "C" uint32_t tud_cdc_n_write(uint8_t, const void *, uint32_t) { return 0U; }
extern "C" uint32_t tud_cdc_n_write_flush(uint8_t) { return 0U; }

/* One flag per watched endpoint, so a run of turns busy can be driven. */
static bool gEpBusyHciIn;
static bool gEpBusyHciOut;
static bool gEpBusyLogIn;

static unsigned gDcdConnect;
static unsigned gDcdDisconnect;

extern "C" void dcd_connect(uint8_t rhport) { assert(rhport == 0U); gDcdConnect++; }
extern "C" void dcd_disconnect(uint8_t rhport) { assert(rhport == 0U); gDcdDisconnect++; }

extern "C" bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr)
{
    assert(rhport == 0U);
    switch (ep_addr)
    {
    case HCI_USB_EP_CDC_IN:  return gEpBusyHciIn;
    case HCI_USB_EP_CDC_OUT: return gEpBusyHciOut;
    case HCI_USB_EP_LOG_IN:  return gEpBusyLogIn;
    default:                 return false;
    }
}

extern "C" int CFifoAvail(hCFifo_t) { return 0; }
extern "C" int CFifoUsed(hCFifo_t) { return 0; }
extern "C" int CFifoRead(hCFifo_t, uint8_t *, int) { return 0; }
extern "C" int CFifoWrite(hCFifo_t, uint8_t *, int) { return 0; }

static void Wake(void *) { gWakeCalls++; }

static struct __CFifo gRxFifo;
static struct __CFifo gTxFifo;

static void Reset(UsbdCdcDevIntrf_t *pIntrf)
{
    gRhportInitCalls = 0U;
    gRhportInitPort = 0xFFU;
    gRhportInitRole = TUSB_ROLE_INVALID;
    gRhportInitSpeed = TUSB_SPEED_AUTO;
    gRhportInitResult = true;
    gTaskCalls = 0U;
    gTaskPort = 0xFFU;
    gMounted = false;
    gLineState = 0U;
    gWakeCalls = 0U;
    gDcdHandlerCalls = 0U;
    gRhportRole[0] = TUSB_ROLE_INVALID;
    gEpBusyHciIn = false;
    gEpBusyHciOut = false;
    gEpBusyLogIn = false;
    gDcdConnect = 0U;
    gDcdDisconnect = 0U;

    memset(pIntrf, 0, sizeof(*pIntrf));
    pIntrf->hRxFifo = &gRxFifo;
    pIntrf->hTxFifo = &gTxFifo;
}

int main(void)
{
    UsbdCdcDevIntrf_t intrf;
    HciTinyUsb_t usb;

    Reset(&intrf);
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));

    /* Before bring up the interrupt handler must reach nothing. */
    tusb_int_handler(0U, true);
    assert(gDcdHandlerCalls == 0U);

    assert(HciTinyUsbStart(&usb));

    /* The roothub port role has to be recorded, that is the whole point. */
    assert(gRhportInitCalls == 1U);
    assert(gRhportInitPort == 0U);
    assert(gRhportInitRole == TUSB_ROLE_DEVICE);
    assert(gRhportInitSpeed == TUSB_SPEED_FULL);
    assert(tusb_inited());

    /* With the role set the interrupt now reaches the controller driver. */
    tusb_int_handler(0U, true);
    assert(gDcdHandlerCalls == 1U);
    printf("[ok] bring up records the device role so the isr dispatches\n");

    /* The pump has to drive the same port that was initialised. */
    HciTinyUsbProcess(&usb);
    assert(gTaskCalls == 1U);
    assert(gTaskPort == gRhportInitPort);
    assert(usb.TaskCount == 1U);

    assert(!HciTinyUsbIsMounted(&usb));
    gMounted = true;
    assert(HciTinyUsbIsMounted(&usb));
    printf("[ok] pump drives the initialised port and reports mounted\n");

    /* A failed bring up must not mark the layer started. */
    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    gRhportInitResult = false;
    assert(!HciTinyUsbStart(&usb));
    assert(!usb.Started);
    HciTinyUsbProcess(&usb);
    assert(gTaskCalls == 0U);
    printf("[ok] failed bring up leaves the layer stopped\n");

    /*
     * The endpoint reading is a run of turns busy, not a total. It has to
     * fall back to zero the moment a transfer finishes, and the worst run has
     * to survive that, because the worst run is the one that says a port
     * stopped rather than paused.
     */
    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    assert(HciTinyUsbStart(&usb));
    gMounted = true;

    HciTinyUsbProcess(&usb);
    assert(usb.EpBusyTurns[0] == 0U);
    assert(usb.EpBusyWorst[0] == 0U);

    gEpBusyHciIn = true;
    for (unsigned i = 0U; i < 5U; i++)
    {
        HciTinyUsbProcess(&usb);
    }
    assert(usb.EpBusyTurns[0] == 5U);
    assert(usb.EpBusyWorst[0] == 5U);

    /* The other two are idle and must stay at zero. */
    assert(usb.EpBusyWorst[1] == 0U);
    assert(usb.EpBusyWorst[2] == 0U);

    gEpBusyHciIn = false;
    HciTinyUsbProcess(&usb);
    assert(usb.EpBusyTurns[0] == 0U);
    assert(usb.EpBusyWorst[0] == 5U);

    /* A shorter run afterwards must not pull the worst down. */
    gEpBusyHciIn = true;
    HciTinyUsbProcess(&usb);
    HciTinyUsbProcess(&usb);
    assert(usb.EpBusyTurns[0] == 2U);
    assert(usb.EpBusyWorst[0] == 5U);

    /* Nothing mounted is nothing waiting. */
    gMounted = false;
    HciTinyUsbProcess(&usb);
    assert(usb.EpBusyTurns[0] == 0U);
    printf("[ok] a busy endpoint is counted in turns, and the worst is kept\n");

    /*
     * An endpoint that stays busy past the limit takes the port down and puts
     * it back up. Once, not once a turn, and the detach is held long enough
     * for a host to see the port go.
     */
    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    assert(HciTinyUsbStart(&usb));
    gMounted = true;
    gEpBusyHciOut = true;

    for (unsigned i = 0U; i < HCI_TINYUSB_EP_STUCK_TURNS; i++)
    {
        HciTinyUsbProcess(&usb);
    }
    assert(usb.RestartCount == 1U);
    assert(gDcdDisconnect == 1U);
    assert(gDcdConnect == 0U);
    assert(usb.EpBusyWorst[1] == HCI_TINYUSB_EP_STUCK_TURNS);

    /* Down for the whole stretch, and not taken down again while it is. */
    for (unsigned i = 0U; i < HCI_TINYUSB_DETACH_TURNS - 1U; i++)
    {
        HciTinyUsbProcess(&usb);
    }
    assert(gDcdConnect == 0U);
    assert(gDcdDisconnect == 1U);
    assert(usb.RestartCount == 1U);

    HciTinyUsbProcess(&usb);
    assert(gDcdConnect == 1U);
    assert(usb.RestartCount == 1U);
    printf("[ok] an endpoint stuck past the limit restarts the port, once\n");

    printf("All device stack bring up tests passed.\n");
    return 0;
}
