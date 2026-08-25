/**-------------------------------------------------------------------------
@file	hci_usb_tinyusb.cpp

@brief	TinyUSB class-driver integration for native Bluetooth USB HCI.

		Implements the HciController application class driver registered with
		TinyUSB, including descriptor parsing, endpoint ownership, alternate
		settings, control requests, and transfer completion dispatch.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_usb_priv.h"

#include <string.h>

/*
 * Most TinyUSB DCDs implement per-endpoint close directly. The current nRF5x
 * port does not: it uses the ISO allocate/activate model and leaves CBI close
 * as a device-stack no-op. A target may therefore supply this hook to finish
 * cancelling the hardware/DCD endpoint state after usbd_edpt_close().
 */
extern "C" __attribute__((weak))
void HciUsbPlatformEndpointClosed(uint8_t RhPort, uint8_t EpAddr)
{
    (void)RhPort;
    (void)EpAddr;
}

static void HciUsbDriverInit(void)
{
}

static bool HciUsbDriverDeinit(void)
{
    return true;
}

static void HciUsbDriverReset(uint8_t RhPort)
{
    (void)RhPort;

    HciUsb_t *pUsb = g_HciUsb;
    if (pUsb == nullptr)
    {
        return;
    }

    pUsb->Configured = false;
    pUsb->BulkSerializationSupported = false;
    pUsb->BulkSerialization = false;
    pUsb->HciInterface = 0U;
    pUsb->SyncInterface = 0U;
    pUsb->EventEp = 0U;
    pUsb->BulkOutEp = 0U;
    pUsb->BulkInEp = 0U;
    pUsb->BulkOutMps = 0U;
    pUsb->BulkInMps = 0U;
    pUsb->SyncOutEp = 0U;
    pUsb->SyncInEp = 0U;
    pUsb->HciAlt = 0U;
    pUsb->SyncAlt = 0U;
    pUsb->RxSelect = HCI_H4_PACKET_NONE;
    pUsb->TxSelect = HCI_H4_PACKET_NONE;
    pUsb->CommandPending = false;
    pUsb->CommandLen = 0U;
    HciUsbResetBulkRxSession(pUsb);
    pUsb->SyncRxPending = false;
    pUsb->SyncRxLen = 0U;
    pUsb->TxPending = false;
    pUsb->TxActive = false;
    pUsb->TxPayloadComplete = false;
    pUsb->TxZlpActive = false;
    pUsb->TxType = HCI_H4_PACKET_NONE;
    pUsb->TxLen = 0U;
    pUsb->TxWireLen = 0U;
    HciUsbWake(pUsb);
}

static bool HciUsbCopyEndpoint(uint8_t *pDest,
                               const tusb_desc_endpoint_t *pEndpoint)
{
    if (pDest == nullptr || pEndpoint == nullptr ||
        sizeof(tusb_desc_endpoint_t) != HCI_USB_ENDPOINT_DESC_SIZE)
    {
        return false;
    }

    memcpy(pDest, pEndpoint, sizeof(tusb_desc_endpoint_t));
    return true;
}

static bool HciUsbOpenHciEndpoints(HciUsb_t *pUsb,
                                   uint8_t RhPort,
                                   uint8_t Alt);

static uint16_t HciUsbDriverOpen(uint8_t RhPort,
                                 const tusb_desc_interface_t *pInterface,
                                 uint16_t MaxLen)
{
    HciUsb_t *pUsb = g_HciUsb;
    if (pUsb == nullptr || !HciUsbInterfaceMatches(pInterface) ||
        pInterface->bAlternateSetting != HCI_USB_HCI_ALT_LEGACY ||
        pInterface->bNumEndpoints != 3U)
    {
        return 0U;
    }

    const uint8_t *pStart = reinterpret_cast<const uint8_t *>(pInterface);
    const uint8_t *pDesc = pStart;
    const uint8_t *pEnd = pStart + MaxLen;

    memset(pUsb->SyncAltPresent, 0, sizeof(pUsb->SyncAltPresent));
    pUsb->BulkSerializationSupported = false;
    pUsb->HciInterface = pInterface->bInterfaceNumber;
    pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pDesc));

    if ((size_t)(pEnd - pDesc) < sizeof(tusb_desc_endpoint_t))
    {
        return 0U;
    }

    const tusb_desc_endpoint_t *pEvent =
        reinterpret_cast<const tusb_desc_endpoint_t *>(pDesc);
    if (!HciUsbEndpointMatches(pEvent, TUSB_DIR_IN, TUSB_XFER_INTERRUPT) ||
        !HciUsbCopyEndpoint(pUsb->EventDesc, pEvent))
    {
        return 0U;
    }
    pUsb->EventEp = pEvent->bEndpointAddress;
    pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pDesc));

    const tusb_desc_endpoint_t *pBulkOut = nullptr;
    const tusb_desc_endpoint_t *pBulkIn = nullptr;
    for (unsigned Index = 0U; Index < 2U; ++Index)
    {
        if ((size_t)(pEnd - pDesc) < sizeof(tusb_desc_endpoint_t))
        {
            return 0U;
        }

        const tusb_desc_endpoint_t *pEndpoint =
            reinterpret_cast<const tusb_desc_endpoint_t *>(pDesc);
        if (pEndpoint->bDescriptorType != TUSB_DESC_ENDPOINT ||
            pEndpoint->bmAttributes.xfer != TUSB_XFER_BULK)
        {
            return 0U;
        }

        if (tu_edpt_dir(pEndpoint->bEndpointAddress) == TUSB_DIR_IN)
        {
            pBulkIn = pEndpoint;
        }
        else
        {
            pBulkOut = pEndpoint;
        }
        pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pDesc));
    }

    if (pBulkOut == nullptr || pBulkIn == nullptr)
    {
        return 0U;
    }

    const uint16_t BulkOutMps = tu_edpt_packet_size(pBulkOut);
    const uint16_t BulkInMps = tu_edpt_packet_size(pBulkIn);
    if (BulkOutMps == 0U || BulkOutMps > HCI_USB_FS_BULK_MAX_PACKET ||
        BulkInMps == 0U ||
        !HciUsbCopyEndpoint(pUsb->BulkOutDesc, pBulkOut) ||
        !HciUsbCopyEndpoint(pUsb->BulkInDesc, pBulkIn))
    {
        return 0U;
    }

    pUsb->BulkOutEp = pBulkOut->bEndpointAddress;
    pUsb->BulkInEp = pBulkIn->bEndpointAddress;
    pUsb->BulkOutMps = BulkOutMps;
    pUsb->BulkInMps = BulkInMps;

    if ((size_t)(pEnd - pDesc) >= sizeof(tusb_desc_interface_t))
    {
        const tusb_desc_interface_t *pAlt =
            reinterpret_cast<const tusb_desc_interface_t *>(pDesc);
        if (HciUsbInterfaceMatches(pAlt) &&
            pAlt->bInterfaceNumber == pUsb->HciInterface &&
            pAlt->bAlternateSetting == HCI_USB_HCI_ALT_BULK_SERIALIZATION &&
            pAlt->bNumEndpoints == 2U)
        {
            const uint8_t *pAltDesc =
                reinterpret_cast<const uint8_t *>(tu_desc_next(pAlt));
            uint8_t BulkOutEp = 0U;
            uint8_t BulkInEp = 0U;
            uint16_t AltBulkOutMps = 0U;
            uint16_t AltBulkInMps = 0U;

            for (unsigned Index = 0U; Index < 2U; ++Index)
            {
                if ((size_t)(pEnd - pAltDesc) < sizeof(tusb_desc_endpoint_t))
                {
                    return 0U;
                }

                const tusb_desc_endpoint_t *pEndpoint =
                    reinterpret_cast<const tusb_desc_endpoint_t *>(pAltDesc);
                if (pEndpoint->bDescriptorType != TUSB_DESC_ENDPOINT ||
                    pEndpoint->bmAttributes.xfer != TUSB_XFER_BULK)
                {
                    return 0U;
                }

                if (tu_edpt_dir(pEndpoint->bEndpointAddress) == TUSB_DIR_IN)
                {
                    BulkInEp = pEndpoint->bEndpointAddress;
                    AltBulkInMps = tu_edpt_packet_size(pEndpoint);
                }
                else
                {
                    BulkOutEp = pEndpoint->bEndpointAddress;
                    AltBulkOutMps = tu_edpt_packet_size(pEndpoint);
                }
                pAltDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pAltDesc));
            }

            if (BulkOutEp != pUsb->BulkOutEp || BulkInEp != pUsb->BulkInEp ||
                AltBulkOutMps != pUsb->BulkOutMps ||
                AltBulkInMps != pUsb->BulkInMps)
            {
                return 0U;
            }

            pUsb->BulkSerializationSupported = true;
            pDesc = pAltDesc;
        }
    }

    if ((size_t)(pEnd - pDesc) < sizeof(tusb_desc_interface_t))
    {
        return 0U;
    }

    const tusb_desc_interface_t *pSync =
        reinterpret_cast<const tusb_desc_interface_t *>(pDesc);
    if (!HciUsbInterfaceMatches(pSync) ||
        pSync->bInterfaceNumber == pUsb->HciInterface ||
        pSync->bAlternateSetting != 0U || pSync->bNumEndpoints != 0U)
    {
        return 0U;
    }
    pUsb->SyncInterface = pSync->bInterfaceNumber;
    pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pDesc));

    while ((size_t)(pEnd - pDesc) >= sizeof(tusb_desc_interface_t))
    {
        const tusb_desc_interface_t *pAlt =
            reinterpret_cast<const tusb_desc_interface_t *>(pDesc);
        if (!HciUsbInterfaceMatches(pAlt) ||
            pAlt->bInterfaceNumber != pUsb->SyncInterface ||
            pAlt->bAlternateSetting == 0U)
        {
            break;
        }

        if (pAlt->bAlternateSetting >= HCI_USB_SYNC_ALT_COUNT ||
            pAlt->bNumEndpoints != 2U)
        {
            return 0U;
        }

        const uint8_t Alt = pAlt->bAlternateSetting;
        pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pAlt));
        const tusb_desc_endpoint_t *pOut = nullptr;
        const tusb_desc_endpoint_t *pIn = nullptr;

        for (unsigned Index = 0U; Index < 2U; ++Index)
        {
            if ((size_t)(pEnd - pDesc) < sizeof(tusb_desc_endpoint_t))
            {
                return 0U;
            }

            const tusb_desc_endpoint_t *pEndpoint =
                reinterpret_cast<const tusb_desc_endpoint_t *>(pDesc);
            if (pEndpoint->bDescriptorType != TUSB_DESC_ENDPOINT ||
                pEndpoint->bmAttributes.xfer != TUSB_XFER_ISOCHRONOUS)
            {
                return 0U;
            }

            if (tu_edpt_dir(pEndpoint->bEndpointAddress) == TUSB_DIR_IN)
            {
                pIn = pEndpoint;
            }
            else
            {
                pOut = pEndpoint;
            }
            pDesc = reinterpret_cast<const uint8_t *>(tu_desc_next(pDesc));
        }

        if (pOut == nullptr || pIn == nullptr ||
            !HciUsbCopyEndpoint(pUsb->SyncAltOutDesc[Alt], pOut) ||
            !HciUsbCopyEndpoint(pUsb->SyncAltInDesc[Alt], pIn))
        {
            return 0U;
        }
        pUsb->SyncAltPresent[Alt] = 1U;
    }

    /*
     * Parse the complete Bluetooth function before claiming any endpoint.
     * A malformed later alternate/interface must not leave an Event or Bulk
     * endpoint open after the class driver's open callback reports failure.
     * HciUsbOpenHciEndpoints is transactional and closes partial opens if the
     * DCD refuses one of the three legacy endpoints.
     */
    if (!HciUsbOpenHciEndpoints(pUsb, RhPort, HCI_USB_HCI_ALT_LEGACY))
    {
        return 0U;
    }

    pUsb->Configured = true;
    pUsb->HciAlt = HCI_USB_HCI_ALT_LEGACY;
    pUsb->SyncAlt = 0U;
    pUsb->BulkSerialization = false;
    pUsb->RxSelect = HCI_H4_PACKET_NONE;
    pUsb->TxSelect = HCI_H4_PACKET_NONE;
    pUsb->CommandPending = false;
    pUsb->CommandLen = 0U;
    HciUsbResetBulkRxSession(pUsb);
    pUsb->SyncRxPending = false;
    pUsb->SyncRxLen = 0U;
    pUsb->TxPending = false;
    pUsb->TxActive = false;
    pUsb->TxPayloadComplete = false;
    pUsb->TxZlpActive = false;
    pUsb->TxType = HCI_H4_PACKET_NONE;
    pUsb->TxLen = 0U;
    pUsb->TxWireLen = 0U;
    HciUsbRearmRx(pUsb);
    HciUsbWake(pUsb);
    return (uint16_t)(pDesc - pStart);
}

static bool HciUsbOpenStoredEndpoint(uint8_t RhPort, const uint8_t *pStored)
{
    if (pStored == nullptr)
    {
        return false;
    }

    tusb_desc_endpoint_t Endpoint;
    memcpy(&Endpoint, pStored, sizeof(Endpoint));
    return usbd_edpt_open(RhPort, &Endpoint);
}

static void HciUsbCloseEndpoint(uint8_t RhPort, uint8_t EpAddr)
{
    if (EpAddr == 0U)
    {
        return;
    }

    usbd_edpt_close(RhPort, EpAddr);
    HciUsbPlatformEndpointClosed(RhPort, EpAddr);
}

static void HciUsbCloseHciEndpoints(const HciUsb_t *pUsb, uint8_t RhPort)
{
    if (pUsb == nullptr)
    {
        return;
    }
    HciUsbCloseEndpoint(RhPort, pUsb->EventEp);
    HciUsbCloseEndpoint(RhPort, pUsb->BulkOutEp);
    HciUsbCloseEndpoint(RhPort, pUsb->BulkInEp);
}

static bool HciUsbOpenHciEndpoints(HciUsb_t *pUsb,
                                   uint8_t RhPort,
                                   uint8_t Alt)
{
    bool EventOpen = false;
    bool BulkOutOpen = false;

    if (Alt == HCI_USB_HCI_ALT_LEGACY)
    {
        if (!HciUsbOpenStoredEndpoint(RhPort, pUsb->EventDesc))
        {
            return false;
        }
        EventOpen = true;
    }

    if (!HciUsbOpenStoredEndpoint(RhPort, pUsb->BulkOutDesc))
    {
        if (EventOpen)
        {
            HciUsbCloseEndpoint(RhPort, pUsb->EventEp);
        }
        return false;
    }
    BulkOutOpen = true;

    if (!HciUsbOpenStoredEndpoint(RhPort, pUsb->BulkInDesc))
    {
        if (BulkOutOpen)
        {
            HciUsbCloseEndpoint(RhPort, pUsb->BulkOutEp);
        }
        if (EventOpen)
        {
            HciUsbCloseEndpoint(RhPort, pUsb->EventEp);
        }
        return false;
    }

    return true;
}

static void HciUsbRestartTxAfterEndpointChange(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->TxPending)
    {
        return;
    }

    pUsb->TxActive = false;
    pUsb->TxPayloadComplete = false;
    pUsb->TxZlpActive = false;
    pUsb->TxWireLen = 0U;
}

static bool HciUsbPendingTxHasEndpoint(const HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->TxPending)
    {
        return true;
    }

    if (pUsb->BulkSerialization)
    {
        return true;
    }

    if (pUsb->TxType == HCI_H4_PACKET_EVENT ||
        pUsb->TxType == HCI_H4_PACKET_ACL)
    {
        return true;
    }

    if (pUsb->TxType == HCI_H4_PACKET_SCO)
    {
        return pUsb->SyncAlt != 0U;
    }

    return false;
}

static void HciUsbDropUnsendableTx(HciUsb_t *pUsb)
{
    if (pUsb != nullptr && pUsb->TxPending &&
        !HciUsbPendingTxHasEndpoint(pUsb))
    {
        /*
         * The previous USB mode accepted this controller packet, but the new
         * mode has no endpoint that can carry it. Keeping TxPending set would
         * block every later Event/ACL response indefinitely. The endpoint
         * switch already cancelled the old transfer, so record the transport
         * loss and release the packet rather than wedging the controller.
         */
        HciUsbTxFailed(pUsb, 0U);
    }
}

static bool HciUsbSwitchHciAlt(HciUsb_t *pUsb,
                               uint8_t RhPort,
                               uint8_t Alt)
{
    if (pUsb == nullptr || Alt > HCI_USB_HCI_ALT_BULK_SERIALIZATION ||
        (Alt == HCI_USB_HCI_ALT_BULK_SERIALIZATION &&
         !pUsb->BulkSerializationSupported) ||
        (pUsb->SyncAlt != 0U && Alt != pUsb->HciAlt))
    {
        return false;
    }

    /*
     * Re-selecting the current alternate setting is also an endpoint recovery
     * point. A host can close a pending interrupt/bulk request and then reopen
     * the same alternate setting; rebuilding the endpoints clears stale DCD
     * state instead of carrying it into the new host session.
     */
    const uint8_t OldAlt = pUsb->HciAlt;
    const bool OldBulkSerialization = pUsb->BulkSerialization;

    HciUsbCloseHciEndpoints(pUsb, RhPort);
    HciUsbRestartTxAfterEndpointChange(pUsb);

    if (!HciUsbOpenHciEndpoints(pUsb, RhPort, Alt))
    {
        /* SET_INTERFACE failed: restore the exact mode the Host still owns. */
        HciUsbCloseHciEndpoints(pUsb, RhPort);
        if (!HciUsbOpenHciEndpoints(pUsb, RhPort, OldAlt))
        {
            pUsb->Configured = false;
            pUsb->UsbTransferErrorCount++;
            HciUsbResetBulkRxSession(pUsb);
            HciUsbWake(pUsb);
            return false;
        }

        pUsb->HciAlt = OldAlt;
        pUsb->BulkSerialization = OldBulkSerialization;
        HciUsbResetBulkRxSession(pUsb);
        HciUsbRearmRx(pUsb);
        (void)HciUsbKickTx(pUsb);
        HciUsbWake(pUsb);
        return false;
    }

    pUsb->HciAlt = Alt;
    pUsb->BulkSerialization = Alt == HCI_USB_HCI_ALT_BULK_SERIALIZATION;
    HciUsbDropUnsendableTx(pUsb);
    pUsb->ModeSwitchCount++;
    HciUsbResetBulkRxSession(pUsb);
    HciUsbRearmRx(pUsb);
    (void)HciUsbKickTx(pUsb);
    HciUsbWake(pUsb);
    return true;
}

static bool HciUsbOpenSyncAlt(HciUsb_t *pUsb, uint8_t RhPort, uint8_t Alt)
{
    if (Alt == 0U)
    {
        pUsb->SyncOutEp = 0U;
        pUsb->SyncInEp = 0U;
        return true;
    }

    tusb_desc_endpoint_t OutDesc;
    tusb_desc_endpoint_t InDesc;
    memcpy(&OutDesc, pUsb->SyncAltOutDesc[Alt], sizeof(OutDesc));
    memcpy(&InDesc, pUsb->SyncAltInDesc[Alt], sizeof(InDesc));

    if (!usbd_edpt_open(RhPort, &OutDesc))
    {
        return false;
    }
    if (!usbd_edpt_open(RhPort, &InDesc))
    {
        HciUsbCloseEndpoint(RhPort, OutDesc.bEndpointAddress);
        return false;
    }

    pUsb->SyncOutEp = OutDesc.bEndpointAddress;
    pUsb->SyncInEp = InDesc.bEndpointAddress;
    return true;
}

static bool HciUsbSwitchSyncAlt(HciUsb_t *pUsb,
                                uint8_t RhPort,
                                uint8_t Alt)
{
    if (pUsb == nullptr || pUsb->BulkSerialization ||
        Alt >= HCI_USB_SYNC_ALT_COUNT ||
        (Alt != 0U && pUsb->SyncAltPresent[Alt] == 0U))
    {
        return false;
    }

    if (Alt == pUsb->SyncAlt)
    {
        return true;
    }

    const uint8_t OldAlt = pUsb->SyncAlt;
    const uint8_t OldOut = pUsb->SyncOutEp;
    const uint8_t OldIn = pUsb->SyncInEp;

    if (OldAlt != 0U)
    {
        HciUsbCloseEndpoint(RhPort, OldOut);
        HciUsbCloseEndpoint(RhPort, OldIn);
    }

    if (pUsb->TxPending && pUsb->TxType == HCI_H4_PACKET_SCO)
    {
        HciUsbRestartTxAfterEndpointChange(pUsb);
    }

    pUsb->SyncOutEp = 0U;
    pUsb->SyncInEp = 0U;

    if (!HciUsbOpenSyncAlt(pUsb, RhPort, Alt))
    {
        if (!HciUsbOpenSyncAlt(pUsb, RhPort, OldAlt))
        {
            pUsb->Configured = false;
            pUsb->UsbTransferErrorCount++;
            HciUsbWake(pUsb);
            return false;
        }
        pUsb->SyncAlt = OldAlt;
        HciUsbRearmRx(pUsb);
        (void)HciUsbKickTx(pUsb);
        HciUsbWake(pUsb);
        return false;
    }

    pUsb->SyncAlt = Alt;
    HciUsbDropUnsendableTx(pUsb);
    HciUsbRearmRx(pUsb);
    (void)HciUsbKickTx(pUsb);
    HciUsbWake(pUsb);
    return true;
}

static bool HciUsbDriverControl(uint8_t RhPort,
                                uint8_t Stage,
                                const tusb_control_request_t *pRequest)
{
    HciUsb_t *pUsb = g_HciUsb;
    if (pUsb == nullptr || pRequest == nullptr || !pUsb->Configured)
    {
        return false;
    }

    if (Stage == CONTROL_STAGE_SETUP &&
        pRequest->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        pRequest->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE)
    {
        const uint8_t InterfaceNo = (uint8_t)pRequest->wIndex;
        if (pRequest->bRequest == TUSB_REQ_SET_INTERFACE)
        {
            if (pRequest->bmRequestType_bit.direction != TUSB_DIR_OUT ||
                pRequest->wLength != 0U || (pRequest->wValue & 0xFF00U) != 0U ||
                (pRequest->wIndex & 0xFF00U) != 0U)
            {
                return false;
            }

            const uint8_t Alt = (uint8_t)pRequest->wValue;
            bool Success = false;
            if (InterfaceNo == pUsb->HciInterface)
            {
                Success = HciUsbSwitchHciAlt(pUsb, RhPort, Alt);
            }
            else if (InterfaceNo == pUsb->SyncInterface)
            {
                Success = HciUsbSwitchSyncAlt(pUsb, RhPort, Alt);
            }

            if (!Success)
            {
                return false;
            }
            return tud_control_status(RhPort, pRequest);
        }

        if (pRequest->bRequest == TUSB_REQ_GET_INTERFACE)
        {
            if (pRequest->bmRequestType_bit.direction != TUSB_DIR_IN ||
                pRequest->wValue != 0U || pRequest->wLength != 1U ||
                (pRequest->wIndex & 0xFF00U) != 0U)
            {
                return false;
            }

            if (InterfaceNo == pUsb->HciInterface)
            {
                pUsb->ControlReply = pUsb->HciAlt;
            }
            else if (InterfaceNo == pUsb->SyncInterface)
            {
                pUsb->ControlReply = pUsb->SyncAlt;
            }
            else
            {
                return false;
            }
            return tud_control_xfer(RhPort,
                                    pRequest,
                                    &pUsb->ControlReply,
                                    1U);
        }

        return false;
    }

    if (pRequest->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS ||
        pRequest->bmRequestType_bit.direction != TUSB_DIR_OUT ||
        pUsb->BulkSerialization)
    {
        return false;
    }

    const uint8_t Recipient = pRequest->bmRequestType_bit.recipient;
    if (Recipient == TUSB_REQ_RCPT_INTERFACE)
    {
        if (pRequest->bRequest != 0U || pRequest->wValue != 0U ||
            pRequest->wIndex != pUsb->HciInterface)
        {
            return false;
        }
    }
    else if (Recipient != TUSB_REQ_RCPT_DEVICE)
    {
        return false;
    }

    if (Stage == CONTROL_STAGE_SETUP)
    {
        if (pUsb->CommandPending || pRequest->wLength == 0U ||
            pRequest->wLength > sizeof(pUsb->CommandBuffer))
        {
            return false;
        }

        memset(pUsb->CommandBuffer, 0, sizeof(pUsb->CommandBuffer));
        return tud_control_xfer(RhPort,
                                pRequest,
                                pUsb->CommandBuffer,
                                pRequest->wLength);
    }

    if (Stage == CONTROL_STAGE_DATA)
    {
        const size_t CommandLen = pRequest->wLength;
        if (HciUsbPacketLength(HCI_H4_PACKET_COMMAND,
                               pUsb->CommandBuffer,
                               CommandLen) != CommandLen)
        {
            pUsb->InvalidRxCount++;
            return false;
        }

        pUsb->CommandLen = CommandLen;
        pUsb->CommandPending = true;
        HciUsbWake(pUsb);
        return true;
    }

    return true;
}

static bool HciUsbDriverXferIsr(uint8_t RhPort,
                                uint8_t EpAddr,
                                xfer_result_t Result,
                                uint32_t Transferred)
{
    HciUsb_t *pUsb = g_HciUsb;
    if (pUsb == nullptr || Result != XFER_RESULT_SUCCESS ||
        EpAddr != pUsb->BulkOutEp)
    {
        return false;
    }

    (void)RhPort;
    return HciUsbBulkOutXferIsr(pUsb, (size_t)Transferred);
}

static bool HciUsbDriverXfer(uint8_t RhPort,
                             uint8_t EpAddr,
                             xfer_result_t Result,
                             uint32_t Transferred)
{
    HciUsb_t *pUsb = g_HciUsb;
    if (pUsb == nullptr)
    {
        return false;
    }

    if (Result != XFER_RESULT_SUCCESS)
    {
        pUsb->UsbTransferErrorCount++;
        if (EpAddr == pUsb->BulkOutEp)
        {
            HciUsbResetBulkRxSession(pUsb);
            (void)HciUsbArmBulkOut(pUsb);
        }
        else if (EpAddr == pUsb->SyncOutEp)
        {
            pUsb->SyncRxPending = false;
            pUsb->SyncRxLen = 0U;
            (void)HciUsbArmSyncOut(pUsb);
        }
        else if (EpAddr == pUsb->EventEp || EpAddr == pUsb->BulkInEp ||
                 EpAddr == pUsb->SyncInEp)
        {
            HciUsbTxFailed(pUsb, EpAddr);
        }

        HciUsbWake(pUsb);
        return true;
    }

    if (EpAddr == pUsb->BulkOutEp)
    {
        if (!HciUsbParseBulkOut(pUsb, (size_t)Transferred) ||
            !pUsb->BulkRxPending)
        {
            (void)HciUsbArmBulkOut(pUsb);
        }
        HciUsbWake(pUsb);
        return true;
    }

    if (EpAddr == pUsb->SyncOutEp && pUsb->SyncAlt != 0U)
    {
        if (Transferred == 0U || Transferred > sizeof(pUsb->SyncRxBuffer) ||
            HciUsbPacketLength(HCI_H4_PACKET_SCO,
                               pUsb->SyncRxBuffer,
                               (size_t)Transferred) != (size_t)Transferred)
        {
            pUsb->InvalidRxCount++;
            (void)HciUsbArmSyncOut(pUsb);
        }
        else
        {
            pUsb->SyncRxLen = (size_t)Transferred;
            pUsb->SyncRxPending = true;
        }
        HciUsbWake(pUsb);
        return true;
    }

    if (EpAddr == pUsb->EventEp || EpAddr == pUsb->BulkInEp ||
        EpAddr == pUsb->SyncInEp)
    {
        HciUsbTxComplete(pUsb, EpAddr, Transferred);
        HciUsbWake(pUsb);
        return true;
    }

    (void)RhPort;
    return false;
}

static const usbd_class_driver_t s_HciUsbDriver = {
    "HCI-USB",
    HciUsbDriverInit,
    HciUsbDriverDeinit,
    HciUsbDriverReset,
    HciUsbDriverOpen,
    HciUsbDriverControl,
    HciUsbDriverXfer,
    HciUsbDriverXferIsr,
    nullptr,
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *pDriverCount)
{
    if (pDriverCount == nullptr)
    {
        return nullptr;
    }

    *pDriverCount = 1U;
    return &s_HciUsbDriver;
}
