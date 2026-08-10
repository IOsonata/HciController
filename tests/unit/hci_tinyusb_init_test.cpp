/*
 * Host test for TinyUSB bring up, retry semantics and TX byte preservation.
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

static tusb_role_t gRhportRole[1];
static unsigned gDcdHandlerCalls;

static uint8_t gTxData[32];
static size_t gTxLen;
static size_t gTxPos;
static unsigned gTxFifoReadCalls;
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
extern "C" uint32_t tud_cdc_n_available(uint8_t) { return 0U; }
extern "C" uint32_t tud_cdc_n_read(uint8_t, void *, uint32_t) { return 0U; }
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

static struct __CFifo gRxFifo;
static struct __CFifo gTxFifo;

extern "C" int CFifoAvail(hCFifo_t) { return 0; }
extern "C" int CFifoUsed(hCFifo_t Fifo)
{
    if (Fifo != &gTxFifo)
    {
        return 0;
    }
    return (int)(gTxLen - gTxPos);
}
extern "C" int CFifoRead(hCFifo_t Fifo, uint8_t *pData, int Len)
{
    if (Fifo != &gTxFifo || Len <= 0)
    {
        return 0;
    }

    size_t remaining = gTxLen - gTxPos;
    size_t count = (size_t)Len < remaining ? (size_t)Len : remaining;
    memcpy(pData, &gTxData[gTxPos], count);
    gTxPos += count;
    gTxFifoReadCalls++;
    return (int)count;
}
extern "C" int CFifoWrite(hCFifo_t, uint8_t *, int) { return 0; }

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
    gTxLen = 0U;
    gTxPos = 0U;
    gTxFifoReadCalls = 0U;
    gUsbOutLen = 0U;
    gWriteAvailable = 64U;
    gWriteLimit = 64U;

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

    const uint8_t packet[] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U};
    memcpy(gTxData, packet, sizeof(packet));
    gTxLen = sizeof(packet);
    gWriteLimit = 2U;

    HciTinyUsbProcess(&usb);
    assert(gTxPos == sizeof(packet));
    assert(gTxFifoReadCalls == 1U);
    assert(gUsbOutLen == 2U);
    assert(usb.TxPendingLen == sizeof(packet));
    assert(usb.TxPendingOffset == 2U);
    assert(usb.WriteErrorCount == 1U);

    gWriteLimit = 64U;
    HciTinyUsbProcess(&usb);
    assert(gTxFifoReadCalls == 1U);
    assert(gUsbOutLen == sizeof(packet));
    assert(memcmp(gUsbOut, packet, sizeof(packet)) == 0);
    assert(usb.TxPendingLen == 0U);
    assert(usb.TxPendingOffset == 0U);
    printf("[ok] short USB write preserves every byte across pumps\n");

    printf("All device stack bring up tests passed.\n");
    return 0;
}
