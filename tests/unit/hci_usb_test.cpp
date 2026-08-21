/*
 * Native Bluetooth USB transport tests.
 *
 * The subject is a packet DeviceIntrf: DevAddr is the HCI packet type and the
 * buffers never contain an H:4 indicator. The USB class below that interface
 * adds/removes Bulk Serialization indicators and maps legacy packet types to
 * their standard endpoints/control transfer.
 */

#include "device/usbd_pvt.h"
#include "hci_usb.h"
#include "nrfficr/nrf.h"
#include "tusb.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int DeviceIntrfRx(DevIntrf_t * const pDev,
                  uint32_t DevAddr,
                  uint8_t *pBuff,
                  int BuffLen)
{
    if (!DeviceIntrfStartRx(pDev, DevAddr))
    {
        return 0;
    }

    const int result = DeviceIntrfRxData(pDev, pBuff, BuffLen);
    if (result >= 0)
    {
        DeviceIntrfStopRx(pDev);
    }
    return result;
}

int DeviceIntrfTx(DevIntrf_t * const pDev,
                  uint32_t DevAddr,
                  const uint8_t *pData,
                  int DataLen)
{
    if (!DeviceIntrfStartTx(pDev, DevAddr))
    {
        return 0;
    }

    const int result = DeviceIntrfTxData(pDev, pData, DataLen);
    if (result >= 0)
    {
        DeviceIntrfStopTx(pDev);
    }
    return result;
}

static NRF_FICR_Type s_Ficr = {{0x01234567U, 0x89ABCDEFU}};
NRF_FICR_Type *NRF_FICR = &s_Ficr;

typedef struct
{
    bool Busy;
    uint8_t *pBuffer;
    uint16_t Len;
    unsigned SubmitCount;
} UsbXfer_t;

static UsbXfer_t s_Xfer[256];
static bool s_Open[256];
static bool s_Mounted = true;
static void *s_pControlBuffer;
static uint16_t s_ControlLen;
static unsigned s_StatusCount;
static uint8_t s_FailOpenEp;
static bool s_FailOpenOnce;
static bool s_FailZlpOnce;

bool usbd_edpt_open(uint8_t RhPort, const tusb_desc_endpoint_t *pEndpoint)
{
    (void)RhPort;
    if (s_FailOpenOnce && pEndpoint->bEndpointAddress == s_FailOpenEp)
    {
        s_FailOpenOnce = false;
        return false;
    }

    s_Open[pEndpoint->bEndpointAddress] = true;
    return true;
}

void usbd_edpt_close(uint8_t RhPort, uint8_t EpAddr)
{
    (void)RhPort;
    s_Open[EpAddr] = false;
    s_Xfer[EpAddr].Busy = false;
}

bool usbd_edpt_xfer(uint8_t RhPort,
                    uint8_t EpAddr,
                    uint8_t *pBuffer,
                    uint16_t Len)
{
    (void)RhPort;

    if (!s_Open[EpAddr] || s_Xfer[EpAddr].Busy)
    {
        return false;
    }

    if (s_FailZlpOnce && Len == 0U)
    {
        s_FailZlpOnce = false;
        return false;
    }

    s_Xfer[EpAddr].Busy = true;
    s_Xfer[EpAddr].pBuffer = pBuffer;
    s_Xfer[EpAddr].Len = Len;
    s_Xfer[EpAddr].SubmitCount++;
    return true;
}

bool usbd_edpt_busy(uint8_t RhPort, uint8_t EpAddr)
{
    (void)RhPort;
    return s_Xfer[EpAddr].Busy;
}

bool tud_control_xfer(uint8_t RhPort,
                      const tusb_control_request_t *pRequest,
                      void *pBuffer,
                      uint16_t Len)
{
    (void)RhPort;
    (void)pRequest;
    s_pControlBuffer = pBuffer;
    s_ControlLen = Len;
    return true;
}

bool tud_control_status(uint8_t RhPort, const tusb_control_request_t *pRequest)
{
    (void)RhPort;
    (void)pRequest;
    s_StatusCount++;
    return true;
}

bool tud_mounted(void)
{
    return s_Mounted;
}

static void CompleteTransfer(const usbd_class_driver_t *pDriver,
                             uint8_t EpAddr,
                             uint32_t Len,
                             xfer_result_t Result = XFER_RESULT_SUCCESS)
{
    /* TinyUSB clears BUSY/CLAIMED before calling the class ISR hook. */
    s_Xfer[EpAddr].Busy = false;
    if (pDriver->xfer_isr != nullptr &&
        pDriver->xfer_isr(0U, EpAddr, Result, Len))
    {
        return;
    }
    assert(pDriver->xfer_cb(0U, EpAddr, Result, Len));
}

static tusb_control_request_t MakeRequest(uint8_t RequestType,
                                          uint8_t Request,
                                          uint16_t Value,
                                          uint16_t Index,
                                          uint16_t Len)
{
    tusb_control_request_t Result = {};
    Result.bmRequestType = RequestType;
    Result.bRequest = Request;
    Result.wValue = Value;
    Result.wIndex = Index;
    Result.wLength = Len;
    return Result;
}

static const tusb_desc_interface_t *FindNativeHciInterface(const uint8_t *pConfig,
                                                           uint16_t TotalLen)
{
    const uint8_t *p = pConfig;
    const uint8_t *pEnd = pConfig + TotalLen;
    while (p < pEnd && p[0] != 0U && p + p[0] <= pEnd)
    {
        if (p[1] == TUSB_DESC_INTERFACE && p[0] >= sizeof(tusb_desc_interface_t))
        {
            const tusb_desc_interface_t *pItf =
                reinterpret_cast<const tusb_desc_interface_t *>(p);
            if (pItf->bInterfaceClass == 0xE0U &&
                pItf->bInterfaceSubClass == 0x01U &&
                pItf->bInterfaceProtocol == 0x01U &&
                pItf->bAlternateSetting == 0U &&
                pItf->bNumEndpoints == 3U)
            {
                return pItf;
            }
        }
        p += p[0];
    }
    return nullptr;
}

static void CheckRealNativeDescriptors(void)
{
    assert(HciUsbDescriptorSetMode(HCI_USB_DESCRIPTOR_NATIVE_HCI));

    const tusb_desc_device_t *pDevice =
        reinterpret_cast<const tusb_desc_device_t *>(tud_descriptor_device_cb());
    assert(pDevice != nullptr);
    assert(pDevice->bDeviceClass == 0xEFU);
    assert(pDevice->bDeviceSubClass == 0x02U);
    assert(pDevice->bDeviceProtocol == 0x01U);

    const uint8_t *pConfig = tud_descriptor_configuration_cb(0U);
    assert(pConfig != nullptr && pConfig[0] == TUD_CONFIG_DESC_LEN);
    const uint16_t totalLen = (uint16_t)pConfig[2] | ((uint16_t)pConfig[3] << 8);

    bool HciIad = false;
    bool HciAlt0 = false;
    bool HciAlt1 = false;
    bool SyncAlt0 = false;
    bool SyncNonZero = false;

    const uint8_t *p = pConfig;
    const uint8_t *pEnd = pConfig + totalLen;
    while (p < pEnd && p[0] != 0U && p + p[0] <= pEnd)
    {
        if (p[1] == TUSB_DESC_INTERFACE_ASSOCIATION && p[0] >= 8U)
        {
            HciIad |= p[2] == 0U && p[3] == 2U &&
                      p[4] == 0xE0U && p[5] == 0x01U && p[6] == 0x01U;
        }
        else if (p[1] == TUSB_DESC_INTERFACE &&
                 p[0] >= sizeof(tusb_desc_interface_t))
        {
            const tusb_desc_interface_t *pItf =
                reinterpret_cast<const tusb_desc_interface_t *>(p);
            if (pItf->bInterfaceClass == 0xE0U &&
                pItf->bInterfaceSubClass == 0x01U &&
                pItf->bInterfaceProtocol == 0x01U)
            {
                if (pItf->bInterfaceNumber == 0U)
                {
                    HciAlt0 |= pItf->bAlternateSetting == 0U &&
                               pItf->bNumEndpoints == 3U;
                    HciAlt1 |= pItf->bAlternateSetting == 1U &&
                               pItf->bNumEndpoints == 2U;
                }
                else if (pItf->bInterfaceNumber == 1U)
                {
                    SyncAlt0 |= pItf->bAlternateSetting == 0U &&
                                pItf->bNumEndpoints == 0U;
                    SyncNonZero |= pItf->bAlternateSetting != 0U;
                }
            }
        }
        p += p[0];
    }

    assert(p == pEnd);
    assert(HciIad && HciAlt0 && HciAlt1 && SyncAlt0 && !SyncNonZero);
}

static const uint8_t s_SyncDescriptor[] = {
    9U, 4U, 0U, 0U, 3U, 0xE0U, 1U, 1U, 0U,
    7U, 5U, 0x81U, 3U, 16U, 0U, 1U,
    7U, 5U, 0x02U, 2U, 64U, 0U, 0U,
    7U, 5U, 0x82U, 2U, 64U, 0U, 0U,
    9U, 4U, 0U, 1U, 2U, 0xE0U, 1U, 1U, 0U,
    7U, 5U, 0x02U, 2U, 64U, 0U, 0U,
    7U, 5U, 0x82U, 2U, 64U, 0U, 0U,
    9U, 4U, 1U, 0U, 0U, 0xE0U, 1U, 1U, 0U,
    9U, 4U, 1U, 1U, 2U, 0xE0U, 1U, 1U, 0U,
    7U, 5U, 0x08U, 1U, 9U, 0U, 1U,
    7U, 5U, 0x88U, 1U, 9U, 0U, 1U,
    9U, 4U, 1U, 2U, 2U, 0xE0U, 1U, 1U, 0U,
    7U, 5U, 0x08U, 1U, 17U, 0U, 1U,
    7U, 5U, 0x88U, 1U, 17U, 0U, 1U,
    8U, 11U, 2U, 2U, 2U, 2U, 1U, 0U,
};

int main(void)
{
    CheckRealNativeDescriptors();

    HciUsb_t Usb = {};
    assert(HciUsbInit(&Usb, nullptr));
    DevIntrf_t *pDev = HciUsbGetDeviceIntrf(&Usb);
    assert(pDev == &Usb.DevIntrf);

    uint8_t DriverCount = 0U;
    const usbd_class_driver_t *pDriver = usbd_app_driver_get_cb(&DriverCount);
    assert(pDriver != nullptr && DriverCount == 1U);
    pDriver->init();

    const uint8_t *pConfig = tud_descriptor_configuration_cb(0U);
    const uint16_t totalLen = (uint16_t)pConfig[2] | ((uint16_t)pConfig[3] << 8);
    const tusb_desc_interface_t *pInterface =
        FindNativeHciInterface(pConfig, totalLen);
    assert(pInterface != nullptr);
    const uint16_t remaining =
        (uint16_t)(totalLen - (reinterpret_cast<const uint8_t *>(pInterface) - pConfig));
    assert(pDriver->open(0U, pInterface, remaining) != 0U);
    assert(Usb.Configured && Usb.BulkSerializationSupported && !Usb.BulkSerialization);
    assert(Usb.EventEp == 0x81U && Usb.BulkOutEp == 0x02U && Usb.BulkInEp == 0x82U);
    assert(s_Xfer[0x02U].Busy && s_Xfer[0x02U].Len == 64U);

    /* EP0 command stays intact across an unrelated GET_INTERFACE request. */
    tusb_control_request_t CommandRequest = MakeRequest(0x21U, 0U, 0U, 0U, 3U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &CommandRequest));
    const uint8_t ResetCommand[] = {0x03U, 0x0CU, 0x00U};
    memcpy(s_pControlBuffer, ResetCommand, sizeof(ResetCommand));
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_DATA, &CommandRequest));
    assert(Usb.CommandPending);

    tusb_control_request_t GetInterface =
        MakeRequest(0x81U, TUSB_REQ_GET_INTERFACE, 0U, 0U, 1U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &GetInterface));
    assert(s_ControlLen == 1U && *(uint8_t *)s_pControlBuffer == 0U);

    uint8_t Rx[1200] = {};
    int received = DeviceIntrfRx(pDev,
                                 HCI_H4_PACKET_COMMAND,
                                 Rx,
                                 (int)sizeof(Rx));
    assert(received == 3 && memcmp(Rx, ResetCommand, 3U) == 0);
    assert(!Usb.CommandPending && Usb.CommandCount == 1U);

    /*
     * Composite hosts are supposed to target the HCI interface, but the
     * Bluetooth USB transport also requires device-targeted HCI commands to
     * work. Historical hosts may use bRequest 0xE0 and nonzero setup fields.
     */
    tusb_control_request_t DeviceCommand =
        MakeRequest(0x20U, 0U, 0U, 0U, 3U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &DeviceCommand));
    memcpy(s_pControlBuffer, ResetCommand, sizeof(ResetCommand));
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_DATA, &DeviceCommand));
    received = DeviceIntrfRx(pDev,
                             HCI_H4_PACKET_COMMAND,
                             Rx,
                             (int)sizeof(Rx));
    assert(received == 3 && memcmp(Rx, ResetCommand, 3U) == 0);

    tusb_control_request_t HistoricalDeviceCommand =
        MakeRequest(0x20U, 0xE0U, 0x1234U, 0x5678U, 3U);
    assert(pDriver->control_xfer_cb(0U,
                                    CONTROL_STAGE_SETUP,
                                    &HistoricalDeviceCommand));
    memcpy(s_pControlBuffer, ResetCommand, sizeof(ResetCommand));
    assert(pDriver->control_xfer_cb(0U,
                                    CONTROL_STAGE_DATA,
                                    &HistoricalDeviceCommand));
    received = DeviceIntrfRx(pDev,
                             HCI_H4_PACKET_COMMAND,
                             Rx,
                             (int)sizeof(Rx));
    assert(received == 3 && memcmp(Rx, ResetCommand, 3U) == 0);

    /* An interface-targeted command must name the Bluetooth HCI interface. */
    tusb_control_request_t WrongInterfaceCommand =
        MakeRequest(0x21U, 0U, 0U, 2U, 3U);
    assert(!pDriver->control_xfer_cb(0U,
                                     CONTROL_STAGE_SETUP,
                                     &WrongInterfaceCommand));

    /*
     * Bulk OUT uses one 64-byte DMA scratch buffer. Every physical USB packet
     * is copied into CFifo and the same scratch buffer is rearmed from
     * xfer_isr before task context runs. Four completions therefore queue the
     * complete 256-byte ACL packet without waiting for HciUsbProcess().
     */
    uint8_t Acl256[256] = {};
    Acl256[0] = 0x01U;
    Acl256[2] = 252U;
    const unsigned aclRxBefore = s_Xfer[0x02U].SubmitCount;
    assert(s_Xfer[0x02U].Busy && s_Xfer[0x02U].Len == 64U);
    uint8_t *pBulkScratch = s_Xfer[0x02U].pBuffer;
    assert(pBulkScratch != Usb.BulkRxBuffer);

    for (unsigned Offset = 0U; Offset < sizeof(Acl256); Offset += 64U)
    {
        assert(s_Xfer[0x02U].Busy && s_Xfer[0x02U].Len == 64U);
        assert(s_Xfer[0x02U].pBuffer == pBulkScratch);
        memcpy(s_Xfer[0x02U].pBuffer, &Acl256[Offset], 64U);
        CompleteTransfer(pDriver, 0x02U, 64U);
        assert(s_Xfer[0x02U].Busy && s_Xfer[0x02U].Len == 64U);
        assert(s_Xfer[0x02U].pBuffer == pBulkScratch);
        assert(!Usb.BulkRxPending);
    }
    assert(s_Xfer[0x02U].SubmitCount == aclRxBefore + 4U);

    HciUsbProcess(&Usb);
    assert(Usb.BulkRxPending);
    received = DeviceIntrfRx(pDev, HCI_H4_PACKET_ACL, Rx, (int)sizeof(Rx));
    assert(received == 256 && memcmp(Rx, Acl256, sizeof(Acl256)) == 0);

    /* Event output is raw event bytes on the interrupt endpoint. */
    const uint8_t Event[] = {0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U};
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_EVENT,
                         Event,
                         (int)sizeof(Event)) == (int)sizeof(Event));
    assert(s_Xfer[0x81U].Len == sizeof(Event));
    assert(s_Xfer[0x81U].pBuffer[0] == 0x0EU);
    CompleteTransfer(pDriver, 0x81U, sizeof(Event));
    assert(!Usb.TxPending);

    /*
     * LE Read All Remote Features Complete is 256 bytes: event code, parameter
     * length 254, and 254 parameters. That lands exactly on sixteen 16-byte
     * interrupt transactions, so legacy Event IN needs a terminating ZLP when
     * the host requested room for a larger event.
     */
    uint8_t Event256[256] = {};
    Event256[0] = 0x3EU;
    Event256[1] = 254U;
    const unsigned event256Before = s_Xfer[0x81U].SubmitCount;
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_EVENT,
                         Event256,
                         sizeof(Event256)) == (int)sizeof(Event256));
    assert(s_Xfer[0x81U].Len == sizeof(Event256));
    CompleteTransfer(pDriver, 0x81U, sizeof(Event256));
    assert(Usb.TxPending && Usb.TxPayloadComplete && Usb.TxZlpActive);
    assert(s_Xfer[0x81U].Busy && s_Xfer[0x81U].Len == 0U);
    assert(s_Xfer[0x81U].SubmitCount == event256Before + 2U);
    CompleteTransfer(pDriver, 0x81U, 0U);
    assert(!Usb.TxPending);

    /* A ZLP submission failure retries only the ZLP, never the payload. */
    uint8_t Acl64[64] = {};
    Acl64[2] = 60U;
    const unsigned submitBefore = s_Xfer[0x82U].SubmitCount;
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_ACL,
                         Acl64,
                         sizeof(Acl64)) == (int)sizeof(Acl64));
    assert(s_Xfer[0x82U].Len == 64U);
    s_FailZlpOnce = true;
    CompleteTransfer(pDriver, 0x82U, 64U);
    assert(Usb.TxPending && Usb.TxPayloadComplete && !Usb.TxActive);
    assert(s_Xfer[0x82U].SubmitCount == submitBefore + 1U);

    HciUsbProcess(&Usb);
    assert(Usb.TxPending && Usb.TxZlpActive && s_Xfer[0x82U].Busy);
    assert(s_Xfer[0x82U].Len == 0U);
    assert(s_Xfer[0x82U].SubmitCount == submitBefore + 2U);
    CompleteTransfer(pDriver, 0x82U, 0U);
    assert(!Usb.TxPending);

    /* A short successful IN completion is not retransmitted. */
    const uint32_t txErrorsBefore = Usb.TxErrorCount;
    const unsigned shortBefore = s_Xfer[0x82U].SubmitCount;
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_ACL,
                         Acl64,
                         sizeof(Acl64)) == (int)sizeof(Acl64));
    CompleteTransfer(pDriver, 0x82U, 32U);
    assert(!Usb.TxPending && Usb.TxErrorCount == txErrorsBefore + 1U);
    HciUsbProcess(&Usb);
    assert(s_Xfer[0x82U].SubmitCount == shortBefore + 1U);

    /* Failed SET_INTERFACE rolls back to the host-visible legacy setting. */
    tusb_control_request_t SetBulk =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 1U, 0U, 0U);
    s_FailOpenEp = 0x02U;
    s_FailOpenOnce = true;
    assert(!pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetBulk));
    assert(Usb.HciAlt == 0U && !Usb.BulkSerialization && Usb.Configured);
    assert(s_Open[0x81U] && s_Open[0x02U] && s_Open[0x82U]);

    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetBulk));
    assert(Usb.HciAlt == 1U && Usb.BulkSerialization && !s_Open[0x81U]);

    /*
     * A complete host-to-controller packet belongs to the endpoint session in
     * which it arrived. Leave it queued in CFifo, re-select the alternate, and
     * verify the new endpoint session is armed without exposing the stale
     * packet to HciUsb.
     */
    const uint8_t StaleReset[] = {0x01U, 0x03U, 0x0CU, 0x00U};
    assert(s_Xfer[0x02U].Busy);
    memcpy(s_Xfer[0x02U].pBuffer, StaleReset, sizeof(StaleReset));
    CompleteTransfer(pDriver, 0x02U, sizeof(StaleReset));
    assert(!Usb.BulkRxPending && s_Xfer[0x02U].Busy);
    const unsigned staleBulkBefore = s_Xfer[0x02U].SubmitCount;
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetBulk));
    assert(Usb.HciAlt == 1U && Usb.BulkSerialization);
    assert(!Usb.BulkRxPending && s_Xfer[0x02U].Busy);
    assert(s_Xfer[0x02U].SubmitCount == staleBulkBefore + 1U);
    HciUsbProcess(&Usb);
    assert(!Usb.BulkRxPending);

    /* Bulk Serialization carries the packet type on the bulk wire. */
    uint8_t WireIso[] = {0x05U, 0x01U, 0x00U, 0x00U, 0x00U};
    memcpy(s_Xfer[0x02U].pBuffer, WireIso, sizeof(WireIso));
    CompleteTransfer(pDriver, 0x02U, sizeof(WireIso));
    assert(s_Xfer[0x02U].Busy);
    HciUsbProcess(&Usb);
    received = DeviceIntrfRx(pDev, HCI_H4_PACKET_ISO, Rx, (int)sizeof(Rx));
    assert(received == 4 && Rx[0] == 0x01U);

    const uint8_t IsoEmpty[] = {0x01U, 0x00U, 0x00U, 0x00U};
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_ISO,
                         IsoEmpty,
                         sizeof(IsoEmpty)) == (int)sizeof(IsoEmpty));
    assert(s_Xfer[0x82U].Len == 5U);
    assert(s_Xfer[0x82U].pBuffer[0] == 0x05U);
    CompleteTransfer(pDriver, 0x82U, 5U);

    /* EP0 HCI commands are disabled while Bulk Serialization is active. */
    assert(!pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &CommandRequest));

    /* Pending output is reframed when the HCI alternate setting changes. */
    const uint8_t Event2[] = {0x13U, 0x00U};
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_EVENT,
                         Event2,
                         sizeof(Event2)) == (int)sizeof(Event2));
    assert(s_Xfer[0x82U].pBuffer[0] == 0x04U);

    tusb_control_request_t SetLegacy =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 0U, 0U, 0U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetLegacy));
    assert(!Usb.BulkSerialization && Usb.TxPending && s_Xfer[0x81U].Busy);
    assert(s_Xfer[0x81U].Len == sizeof(Event2));
    assert(s_Xfer[0x81U].pBuffer[0] == Event2[0]);
    CompleteTransfer(pDriver, 0x81U, sizeof(Event2));

    /*
     * Re-selecting the current HCI alternate must rebuild endpoint state.
     * Model a cancelled host request that left no completion callback by
     * clearing the mock OUT busy state while alt 0 remains selected.
     */
    const unsigned sameAltBefore = s_Xfer[0x02U].SubmitCount;
    s_Xfer[0x02U].Busy = false;
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetLegacy));
    assert(Usb.HciAlt == 0U && !Usb.BulkSerialization);
    assert(s_Open[0x81U] && s_Open[0x02U] && s_Open[0x82U]);
    assert(s_Xfer[0x02U].Busy);
    assert(s_Xfer[0x02U].SubmitCount == sameAltBefore + 1U);

    /* Truncated legacy bulk transfer is rejected and OUT stays rearmed. */
    assert(s_Xfer[0x02U].Busy);
    s_Xfer[0x02U].pBuffer[0] = 0x01U;
    s_Xfer[0x02U].pBuffer[1] = 0x00U;
    const uint32_t invalidBefore = Usb.InvalidRxCount;
    CompleteTransfer(pDriver, 0x02U, 2U);
    assert(s_Xfer[0x02U].Busy);
    HciUsbProcess(&Usb);
    assert(Usb.InvalidRxCount == invalidBefore + 1U && s_Xfer[0x02U].Busy);

    /* The generic class driver also rolls back a failed synchronous switch. */
    pDriver->reset(0U);
    memset(s_Xfer, 0, sizeof(s_Xfer));
    memset(s_Open, 0, sizeof(s_Open));
    const tusb_desc_interface_t *pSyncTest =
        reinterpret_cast<const tusb_desc_interface_t *>(s_SyncDescriptor);
    assert(pDriver->open(0U, pSyncTest, sizeof(s_SyncDescriptor)) ==
           sizeof(s_SyncDescriptor) - 8U);

    tusb_control_request_t SetSync1 =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 1U, 1U, 0U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetSync1));
    assert(Usb.SyncAlt == 1U && s_Open[0x08U] && s_Open[0x88U]);

    tusb_control_request_t SetSync2 =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 2U, 1U, 0U);
    s_FailOpenEp = 0x88U;
    s_FailOpenOnce = true;
    assert(!pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetSync2));
    assert(Usb.SyncAlt == 1U && s_Open[0x08U] && s_Open[0x88U]);

    assert(s_StatusCount == 5U);
    pDriver->reset(0U);
    HciUsbDeinit(&Usb);

    puts("hci_usb native DeviceIntrf tests passed");
    return 0;
}
