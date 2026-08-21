/*
 * Focused regression for native USB HCI alternate-setting TX recovery.
 *
 * Reuse the complete USB transport test fixture so this case exercises the
 * same TinyUSB endpoint model as hci_usb_test.cpp without maintaining a second
 * copy of the fake DCD and control-transfer plumbing.
 */
#define main hci_usb_existing_main
#include "hci_usb_test.cpp"
#undef main

int main(void)
{
    CheckRealNativeDescriptors();

    memset(s_Xfer, 0, sizeof(s_Xfer));
    memset(s_Open, 0, sizeof(s_Open));
    s_pControlBuffer = nullptr;
    s_ControlLen = 0U;
    s_StatusCount = 0U;
    s_FailOpenEp = 0U;
    s_FailOpenOnce = false;
    s_FailZlpOnce = false;

    HciUsb_t Usb = {};
    assert(HciUsbInit(&Usb, nullptr));
    DevIntrf_t *pDev = HciUsbGetDeviceIntrf(&Usb);
    assert(pDev == &Usb.DevIntrf);

    uint8_t DriverCount = 0U;
    const usbd_class_driver_t *pDriver = usbd_app_driver_get_cb(&DriverCount);
    assert(pDriver != nullptr && DriverCount == 1U);
    pDriver->init();

    const uint8_t *pConfig = tud_descriptor_configuration_cb(0U);
    const uint16_t totalLen =
        (uint16_t)pConfig[2] | ((uint16_t)pConfig[3] << 8);
    const tusb_desc_interface_t *pInterface =
        FindNativeHciInterface(pConfig, totalLen);
    assert(pInterface != nullptr);
    const uint16_t remaining =
        (uint16_t)(totalLen -
                   (reinterpret_cast<const uint8_t *>(pInterface) - pConfig));
    assert(pDriver->open(0U, pInterface, remaining) != 0U);
    assert(Usb.Configured && !Usb.BulkSerialization);

    tusb_control_request_t SetBulk =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 1U, 0U, 0U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetBulk));
    assert(Usb.HciAlt == 1U && Usb.BulkSerialization);

    /*
     * Leave a Controller-to-Host ISO packet active on Bulk IN. Switching to
     * legacy HCI removes ISO transport entirely. The packet must therefore be
     * released instead of remaining TxPending forever and blocking all later
     * Event/ACL traffic.
     */
    const uint8_t IsoEmpty[] = {0x01U, 0x00U, 0x00U, 0x00U};
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_ISO,
                         IsoEmpty,
                         sizeof(IsoEmpty)) == (int)sizeof(IsoEmpty));
    assert(Usb.TxPending && s_Xfer[0x82U].Busy);
    assert(s_Xfer[0x82U].pBuffer == Usb.TxBuffer);
    assert((((uintptr_t)s_Xfer[0x82U].pBuffer) & 3U) == 0U);
    assert(s_Xfer[0x82U].pBuffer[0] == HCI_H4_PACKET_ISO);
    assert(memcmp(&s_Xfer[0x82U].pBuffer[1], IsoEmpty, sizeof(IsoEmpty)) == 0);

    const uint32_t txErrorsBefore = Usb.TxErrorCount;
    tusb_control_request_t SetLegacy =
        MakeRequest(0x01U, TUSB_REQ_SET_INTERFACE, 0U, 0U, 0U);
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetLegacy));
    assert(Usb.HciAlt == 0U && !Usb.BulkSerialization);
    assert(!Usb.TxPending);
    assert(Usb.TxErrorCount == txErrorsBefore + 1U);

    /*
     * Legacy IN starts at byte 4: word aligned, while byte 0 remains reserved
     * for the H4 indicator used by Bulk Serialization.
     */
    const uint8_t Event[] = {0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U};
    assert(DeviceIntrfTx(pDev,
                         HCI_H4_PACKET_EVENT,
                         Event,
                         sizeof(Event)) == (int)sizeof(Event));
    assert(Usb.TxPending && s_Xfer[0x81U].Busy);
    assert(s_Xfer[0x81U].pBuffer ==
           &Usb.TxBuffer[HCI_USB_TX_LEGACY_OFFSET]);
    assert((((uintptr_t)s_Xfer[0x81U].pBuffer) & 3U) == 0U);
    assert(Usb.TxBuffer[0] == HCI_H4_PACKET_EVENT);
    assert(s_Xfer[0x81U].Len == sizeof(Event));
    assert(memcmp(s_Xfer[0x81U].pBuffer, Event, sizeof(Event)) == 0);

    /*
     * A pending packet survives HCI alternate-setting changes. Its in-buffer
     * wire layout must move with the mode while both DMA pointers stay aligned
     * and byte 0 keeps the H4 indicator ownership.
     */
    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetBulk));
    assert(Usb.HciAlt == 1U && Usb.BulkSerialization && Usb.TxPending);
    assert(s_Xfer[0x82U].Busy && s_Xfer[0x82U].pBuffer == Usb.TxBuffer);
    assert((((uintptr_t)s_Xfer[0x82U].pBuffer) & 3U) == 0U);
    assert(s_Xfer[0x82U].Len == sizeof(Event) + 1U);
    assert(s_Xfer[0x82U].pBuffer[0] == HCI_H4_PACKET_EVENT);
    assert(memcmp(&s_Xfer[0x82U].pBuffer[1], Event, sizeof(Event)) == 0);

    assert(pDriver->control_xfer_cb(0U, CONTROL_STAGE_SETUP, &SetLegacy));
    assert(Usb.HciAlt == 0U && !Usb.BulkSerialization && Usb.TxPending);
    assert(s_Xfer[0x81U].Busy &&
           s_Xfer[0x81U].pBuffer == &Usb.TxBuffer[HCI_USB_TX_LEGACY_OFFSET]);
    assert((((uintptr_t)s_Xfer[0x81U].pBuffer) & 3U) == 0U);
    assert(Usb.TxBuffer[0] == HCI_H4_PACKET_EVENT);
    assert(s_Xfer[0x81U].Len == sizeof(Event));
    assert(memcmp(s_Xfer[0x81U].pBuffer, Event, sizeof(Event)) == 0);
    CompleteTransfer(pDriver, 0x81U, sizeof(Event));
    assert(!Usb.TxPending);

    assert(s_StatusCount == 4U);
    pDriver->reset(0U);
    HciUsbDeinit(&Usb);

    puts("hci_usb mode-switch TX alignment regression passed");
    return 0;
}
