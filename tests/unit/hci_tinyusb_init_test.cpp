/*
 * Host test for TinyUSB bring up, retry semantics and TX byte preservation.
 */

#include "hci_tinyusb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cfifo.h"
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

static tusb_role_t gRhportRole[1];
static unsigned gDcdHandlerCalls;

/*
 * Real IOsonata FIFOs, not fakes. hci_tinyusb.cpp asks the RX FIFO how much
 * room it has, clamps its USB read to that, and counts a short write as a
 * drop; a fake that always answered zero left every one of those lines
 * unreachable. Byte FIFOs, so the block size is one.
 */
#define TEST_FIFO_BYTES 64U
static uint8_t gRxFifoMem[CFIFO_MEMSIZE(TEST_FIFO_BYTES)];
static uint8_t gTxFifoMem[CFIFO_MEMSIZE(TEST_FIFO_BYTES)];
static uint8_t gUsbOut[64];
static size_t gUsbOutLen;
static uint32_t gWriteAvailable;
static uint32_t gWriteLimit;

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

extern "C" void tud_task_ext(uint8_t rhport, bool)
{
    gTaskCalls++;
    gTaskPort = rhport;
}
extern "C" bool tud_mounted(void) { return gMounted; }
extern "C" uint8_t tud_cdc_n_get_line_state(uint8_t) { return gLineState; }
/* What the host has queued for the device to read, and how far it has got. */
static uint8_t gUsbIn[128];
static size_t gUsbInLen;
static size_t gUsbInPos;

extern "C" uint32_t tud_cdc_n_available(uint8_t)
{
    return (uint32_t)(gUsbInLen - gUsbInPos);
}
extern "C" uint32_t tud_cdc_n_read(uint8_t, void *pData, uint32_t Len)
{
    uint32_t remaining = (uint32_t)(gUsbInLen - gUsbInPos);
    uint32_t actual = Len < remaining ? Len : remaining;
    memcpy(pData, &gUsbIn[gUsbInPos], actual);
    gUsbInPos += actual;
    return actual;
}
extern "C" uint32_t tud_cdc_n_write_available(uint8_t) { return gWriteAvailable; }
extern "C" uint32_t tud_cdc_n_write(uint8_t, const void *pData, uint32_t Len)
{
    uint32_t actual = Len;
    if (actual > gWriteLimit)
    {
        actual = gWriteLimit;
    }
    assert(gUsbOutLen + actual <= sizeof(gUsbOut));
    memcpy(&gUsbOut[gUsbOutLen], pData, actual);
    gUsbOutLen += actual;
    return actual;
}
extern "C" uint32_t tud_cdc_n_write_flush(uint8_t) { return 0U; }

static hCFifo_t gRxFifo;
static hCFifo_t gTxFifo;

static void Wake(void *) { gWakeCalls++; }

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
    gUsbOutLen = 0U;
    gUsbInLen = 0U;
    gUsbInPos = 0U;
    gWriteAvailable = 64U;
    gWriteLimit = 64U;

    gRxFifo = CFifoInit(gRxFifoMem, sizeof(gRxFifoMem), 1U, true);
    gTxFifo = CFifoInit(gTxFifoMem, sizeof(gTxFifoMem), 1U, true);
    assert(gRxFifo != nullptr && gTxFifo != nullptr);

    memset(pIntrf, 0, sizeof(*pIntrf));
    pIntrf->hRxFifo = gRxFifo;
    pIntrf->hTxFifo = gTxFifo;
}

int main(void)
{
    UsbdCdcDevIntrf_t intrf;
    HciTinyUsb_t usb;

    Reset(&intrf);
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));

    tusb_int_handler(0U, true);
    assert(gDcdHandlerCalls == 0U);

    assert(HciTinyUsbStart(&usb));
    assert(gRhportInitCalls == 1U);
    assert(gRhportInitPort == 0U);
    assert(gRhportInitRole == TUSB_ROLE_DEVICE);
    assert(gRhportInitSpeed == TUSB_SPEED_FULL);
    assert(tusb_inited());

    tusb_int_handler(0U, true);
    assert(gDcdHandlerCalls == 1U);
    printf("[ok] bring up records the device role so the isr dispatches\n");

    assert(HciTinyUsbStart(&usb));
    assert(gRhportInitCalls == 1U);
    printf("[ok] repeated start is idempotent for hardware retry\n");

    HciTinyUsbProcess(&usb);
    assert(gTaskCalls == 1U);
    assert(gTaskPort == gRhportInitPort);
    assert(usb.TaskCount == 1U);

    assert(!HciTinyUsbIsMounted(&usb));
    gMounted = true;
    assert(HciTinyUsbIsMounted(&usb));
    printf("[ok] pump drives the initialised port and reports mounted\n");

    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    gRhportInitResult = false;
    assert(!HciTinyUsbStart(&usb));
    assert(!usb.Started);
    HciTinyUsbProcess(&usb);
    assert(gTaskCalls == 0U);
    printf("[ok] failed bring up leaves the layer stopped\n");

    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    gLineState = 1U;
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    assert(HciTinyUsbStart(&usb));

    uint8_t packet[] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U};
    assert(CFifoWrite(gTxFifo, packet, (int)sizeof(packet)) ==
           (int)sizeof(packet));
    gWriteLimit = 2U;

    HciTinyUsbProcess(&usb);
    /* The whole packet left the FIFO in one read even though USB took two. */
    assert(CFifoUsed(gTxFifo) == 0);
    assert(gUsbOutLen == 2U);
    assert(usb.TxPendingLen == sizeof(packet));
    assert(usb.TxPendingOffset == 2U);
    assert(usb.WriteErrorCount == 1U);

    gWriteLimit = 64U;
    HciTinyUsbProcess(&usb);
    assert(CFifoUsed(gTxFifo) == 0);
    assert(gUsbOutLen == sizeof(packet));
    assert(memcmp(gUsbOut, packet, sizeof(packet)) == 0);
    assert(usb.TxPendingLen == 0U);
    assert(usb.TxPendingOffset == 0U);
    printf("[ok] short USB write preserves every byte across pumps\n");

    /*
     * Receive. The loop reads only as much as the RX FIFO will take, so a host
     * sending more than the FIFO holds must leave the surplus in the USB
     * endpoint rather than dropping it or overrunning. None of this was
     * reachable while CFifoAvail was faked to zero.
     */
    Reset(&intrf);
    memset(&usb, 0, sizeof(usb));
    gLineState = 1U;
    assert(HciTinyUsbInit(&usb, &intrf, 0U, Wake, nullptr));
    assert(HciTinyUsbStart(&usb));

    for (size_t i = 0U; i < sizeof(gUsbIn); i++)
    {
        gUsbIn[i] = (uint8_t)i;
    }
    gUsbInLen = sizeof(gUsbIn);

    HciTinyUsbProcess(&usb);
    assert(CFifoUsed(gRxFifo) == (int)TEST_FIFO_BYTES);
    assert(CFifoAvail(gRxFifo) == 0);
    assert(gUsbInPos == TEST_FIFO_BYTES);
    assert(usb.RxDropCount == 0U);
    assert(usb.ReadErrorCount == 0U);

    uint8_t got[TEST_FIFO_BYTES];
    assert(CFifoRead(gRxFifo, got, (int)sizeof(got)) == (int)sizeof(got));
    assert(memcmp(got, gUsbIn, sizeof(got)) == 0);
    printf("[ok] USB read is clamped to FIFO room, surplus stays queued\n");

    /* Room again, so the pump takes the rest without losing its place. */
    HciTinyUsbProcess(&usb);
    assert(gUsbInPos == sizeof(gUsbIn));
    assert(CFifoUsed(gRxFifo) == (int)(sizeof(gUsbIn) - TEST_FIFO_BYTES));
    assert(usb.RxDropCount == 0U);

    uint8_t rest[sizeof(gUsbIn) - TEST_FIFO_BYTES];
    assert(CFifoRead(gRxFifo, rest, (int)sizeof(rest)) == (int)sizeof(rest));
    assert(memcmp(rest, &gUsbIn[TEST_FIFO_BYTES], sizeof(rest)) == 0);
    printf("[ok] the rest arrives on the next pump, in order\n");

    printf("All device stack bring up tests passed.\n");
    return 0;
}
