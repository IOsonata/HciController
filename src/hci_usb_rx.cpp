/**-------------------------------------------------------------------------
@file	hci_usb_rx.cpp

@brief	Native Bluetooth USB HCI receive path and Bulk OUT queue.

		Implements native USB receive buffering, packet-length validation,
		Bulk Serialization parsing, ISR-side Bulk OUT rearming, task-side
		packet assembly, and synchronous SCO receive rearming.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_usb_priv.h"

#include "cfifo.h"

#include <stdint.h>
#include <string.h>

#ifndef TAKT_INLINE_OPTIMIZATION
#define HCI_USB_LOCAL_TAKT_INLINE_OPTIMIZATION 1
#define TAKT_INLINE_OPTIMIZATION
#endif
#include "TaktOSQueue.h"
#ifdef HCI_USB_LOCAL_TAKT_INLINE_OPTIMIZATION
#undef TAKT_INLINE_OPTIMIZATION
#undef HCI_USB_LOCAL_TAKT_INLINE_OPTIMIZATION
#endif

#ifndef HCI_USB_BULK_RX_FIFO_BLOCKS
#define HCI_USB_BULK_RX_FIFO_BLOCKS 64U
#endif

typedef struct
{
    uint32_t Length;
    uint8_t Data[HCI_USB_FS_BULK_MAX_PACKET];
} HciUsbBulkUsbPacket_t;

#define HCI_USB_BULK_RX_FIFO_MEM_SIZE \
    CFIFO_TOTAL_MEMSIZE(HCI_USB_BULK_RX_FIFO_BLOCKS, \
                        sizeof(HciUsbBulkUsbPacket_t))

typedef struct
{
    hCFifo_t Fifo;
    bool Held;
    uint16_t HeldLen;
    uint32_t Session;
    uint32_t ArmedSession;

    alignas(4) uint8_t DmaBuffer[HCI_USB_FS_BULK_MAX_PACKET];
    alignas(4) uint8_t FifoMem[HCI_USB_BULK_RX_FIFO_MEM_SIZE];
} HciUsbBulkRxQueue_t;

static HciUsbBulkRxQueue_t s_BulkRx;

static_assert((sizeof(HciUsbBulkUsbPacket_t) & 3U) == 0U,
              "USB Bulk OUT FIFO blocks must remain word aligned");

HciUsb_t *g_HciUsb;

static bool HciUsbBulkRxEnsure(void)
{
    if (s_BulkRx.Fifo != nullptr)
    {
        return true;
    }

    s_BulkRx.Fifo = CFifoInit(s_BulkRx.FifoMem,
                              sizeof(s_BulkRx.FifoMem),
                              sizeof(HciUsbBulkUsbPacket_t),
                              true);
    return s_BulkRx.Fifo != nullptr;
}

static void HciUsbFastCopy(void *pDst, const void *pSrc, size_t Len)
{
    if (Len == HCI_USB_FS_BULK_MAX_PACKET &&
        ((((uintptr_t)pDst | (uintptr_t)pSrc) & 3U) == 0U))
    {
        TaktQueueFastCopy(pDst, pSrc, 32U);
        TaktQueueFastCopy((uint8_t *)pDst + 32U,
                          (const uint8_t *)pSrc + 32U,
                          32U);
        return;
    }

    memcpy(pDst, pSrc, Len);
}

static uint16_t HciUsbReadLe16(const uint8_t *pData)
{
    return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

void HciUsbWake(HciUsb_t *pUsb)
{
    if (pUsb != nullptr && pUsb->DevIntrf.EvtCB != nullptr)
    {
        pUsb->DevIntrf.EvtCB(&pUsb->DevIntrf,
                             DEVINTRF_EVT_STATECHG,
                             nullptr,
                             0);
    }
}

bool HciUsbInterfaceMatches(const tusb_desc_interface_t *pInterface)
{
    return pInterface != nullptr &&
           pInterface->bDescriptorType == TUSB_DESC_INTERFACE &&
           pInterface->bInterfaceClass == HCI_USB_CLASS_WIRELESS_CONTROLLER &&
           pInterface->bInterfaceSubClass == HCI_USB_SUBCLASS_RF_CONTROLLER &&
           pInterface->bInterfaceProtocol == HCI_USB_PROTOCOL_BLUETOOTH;
}

bool HciUsbEndpointMatches(const tusb_desc_endpoint_t *pEndpoint,
                           uint8_t Direction,
                           uint8_t TransferType)
{
    return pEndpoint != nullptr &&
           pEndpoint->bDescriptorType == TUSB_DESC_ENDPOINT &&
           tu_edpt_dir(pEndpoint->bEndpointAddress) == Direction &&
           pEndpoint->bmAttributes.xfer == TransferType;
}

size_t HciUsbPacketLength(HciH4PacketType_t Type,
                          const uint8_t *pPacket,
                          size_t Available)
{
    if (pPacket == nullptr)
    {
        return 0U;
    }

    switch (Type)
    {
        case HCI_H4_PACKET_COMMAND:
            if (Available < HCI_USB_COMMAND_HEADER_SIZE)
            {
                return 0U;
            }
            return HCI_USB_COMMAND_HEADER_SIZE + (size_t)pPacket[2];

        case HCI_H4_PACKET_EVENT:
            if (Available < HCI_USB_EVENT_HEADER_SIZE)
            {
                return 0U;
            }
            return HCI_USB_EVENT_HEADER_SIZE + (size_t)pPacket[1];

        case HCI_H4_PACKET_ACL:
            if (Available < HCI_USB_ACL_HEADER_SIZE)
            {
                return 0U;
            }
            return HCI_USB_ACL_HEADER_SIZE + (size_t)HciUsbReadLe16(&pPacket[2]);

        case HCI_H4_PACKET_SCO:
            if (Available < HCI_USB_SCO_HEADER_SIZE)
            {
                return 0U;
            }
            return HCI_USB_SCO_HEADER_SIZE + (size_t)pPacket[2];

        case HCI_H4_PACKET_ISO:
            if (Available < HCI_USB_ISO_HEADER_SIZE)
            {
                return 0U;
            }
            return HCI_USB_ISO_HEADER_SIZE +
                   (size_t)(HciUsbReadLe16(&pPacket[2]) & 0x3FFFU);

        default:
            return 0U;
    }
}

static bool HciUsbHostTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_COMMAND ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_SCO ||
           Type == HCI_H4_PACKET_ISO;
}

static size_t HciUsbHeaderSize(HciH4PacketType_t Type)
{
    switch (Type)
    {
        case HCI_H4_PACKET_COMMAND:
            return HCI_USB_COMMAND_HEADER_SIZE;
        case HCI_H4_PACKET_ACL:
            return HCI_USB_ACL_HEADER_SIZE;
        case HCI_H4_PACKET_SCO:
            return HCI_USB_SCO_HEADER_SIZE;
        case HCI_H4_PACKET_ISO:
            return HCI_USB_ISO_HEADER_SIZE;
        default:
            return 0U;
    }
}

void HciUsbResetBulkRx(HciUsb_t *pUsb)
{
    if (pUsb == nullptr)
    {
        return;
    }

    pUsb->BulkRxPending = false;
    pUsb->BulkRxType = HCI_H4_PACKET_NONE;
    pUsb->BulkRxLen = 0U;
    pUsb->BulkRxReceived = 0U;
    pUsb->BulkRxExpected = 0U;
    pUsb->BulkRxModeSwitchMark = pUsb->ModeSwitchCount;
}

void HciUsbResetBulkRxSession(HciUsb_t *pUsb)
{
    HciUsbResetBulkRx(pUsb);

    if (!HciUsbBulkRxEnsure())
    {
        return;
    }

    ++s_BulkRx.Session;
    s_BulkRx.Held = false;
    s_BulkRx.HeldLen = 0U;
    CFifoFlush(s_BulkRx.Fifo);
}

static bool HciUsbArmBulkOutContext(HciUsb_t *pUsb, bool IsIsr)
{
    if (pUsb == nullptr || !HciUsbBulkRxEnsure() ||
        !pUsb->Configured || s_BulkRx.Held ||
        pUsb->BulkOutEp == 0U || pUsb->BulkOutMps == 0U ||
        pUsb->BulkOutMps > sizeof(s_BulkRx.DmaBuffer) ||
        usbd_edpt_busy(0U, pUsb->BulkOutEp))
    {
        return false;
    }

    s_BulkRx.ArmedSession = s_BulkRx.Session;
    return IsIsr ?
        HciUsbEdptXferIsr(0U,
                          pUsb->BulkOutEp,
                          s_BulkRx.DmaBuffer,
                          pUsb->BulkOutMps) :
        HciUsbEdptXfer(0U,
                       pUsb->BulkOutEp,
                       s_BulkRx.DmaBuffer,
                       pUsb->BulkOutMps);
}

static bool HciUsbBulkPacketReady(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || pUsb->BulkRxReceived == 0U)
    {
        return false;
    }

    size_t Offset = 0U;
    HciH4PacketType_t Type = HCI_H4_PACKET_ACL;
    if (pUsb->BulkSerialization)
    {
        Type = (HciH4PacketType_t)pUsb->BulkRxBuffer[0];
        if (!HciUsbHostTypeValid(Type))
        {
            pUsb->InvalidRxCount++;
            HciUsbResetBulkRx(pUsb);
            return false;
        }
        Offset = 1U;
    }

    const size_t HeaderSize = HciUsbHeaderSize(Type);
    if (HeaderSize == 0U)
    {
        pUsb->InvalidRxCount++;
        HciUsbResetBulkRx(pUsb);
        return false;
    }

    if (pUsb->BulkRxReceived < Offset + HeaderSize)
    {
        return false;
    }

    const size_t PacketLen =
        HciUsbPacketLength(Type,
                           &pUsb->BulkRxBuffer[Offset],
                           pUsb->BulkRxReceived - Offset);
    if (PacketLen == 0U || PacketLen > HCI_USB_PACKET_SIZE)
    {
        pUsb->InvalidRxCount++;
        HciUsbResetBulkRx(pUsb);
        return false;
    }

    const size_t Expected = Offset + PacketLen;
    pUsb->BulkRxExpected = Expected;
    if (pUsb->BulkRxReceived < Expected)
    {
        return false;
    }

    if (pUsb->BulkRxReceived > Expected)
    {
        pUsb->InvalidRxCount++;
        HciUsbResetBulkRx(pUsb);
        return false;
    }

#if defined(HCI_USB_BENCHMARK) && HCI_USB_BENCHMARK
    HciUsbResetBulkRx(pUsb);
    return false;
#else
    pUsb->BulkRxType = Type;
    pUsb->BulkRxLen = PacketLen;
    pUsb->BulkRxPending = true;
    HciUsbWake(pUsb);
    return true;
#endif
}

static bool HciUsbConsumePhysicalPacket(HciUsb_t *pUsb,
                                        const uint8_t *pData,
                                        size_t TransferLen)
{
    if (pUsb == nullptr || pData == nullptr || pUsb->BulkOutMps == 0U)
    {
        return false;
    }

    if (pUsb->BulkRxReceived != 0U &&
        pUsb->BulkRxModeSwitchMark != pUsb->ModeSwitchCount)
    {
        HciUsbResetBulkRx(pUsb);
    }

    if (pUsb->BulkRxPending || TransferLen == 0U ||
        TransferLen > pUsb->BulkOutMps ||
        pUsb->BulkRxReceived + TransferLen > sizeof(pUsb->BulkRxBuffer))
    {
        pUsb->InvalidRxCount++;
        if (!pUsb->BulkRxPending)
        {
            HciUsbResetBulkRx(pUsb);
        }
        return false;
    }

    if (pUsb->BulkRxReceived == 0U)
    {
        pUsb->BulkRxModeSwitchMark = pUsb->ModeSwitchCount;
    }

    HciUsbFastCopy(&pUsb->BulkRxBuffer[pUsb->BulkRxReceived],
                   pData,
                   TransferLen);
    pUsb->BulkRxReceived += TransferLen;

    if (HciUsbBulkPacketReady(pUsb))
    {
        return true;
    }

    if (pUsb->BulkRxReceived != 0U && TransferLen < pUsb->BulkOutMps)
    {
        pUsb->InvalidRxCount++;
        HciUsbResetBulkRx(pUsb);
    }

    return false;
}

static bool HciUsbBulkRxPublishHeld(HciUsb_t *pUsb)
{
    if (!s_BulkRx.Held || s_BulkRx.Fifo == nullptr ||
        CFifoAvail(s_BulkRx.Fifo) <= 0)
    {
        return false;
    }

    HciUsbBulkUsbPacket_t *pPacket =
        (HciUsbBulkUsbPacket_t *)CFifoPut(s_BulkRx.Fifo);
    if (pPacket == nullptr)
    {
        return false;
    }

    pPacket->Length = s_BulkRx.HeldLen;
    HciUsbFastCopy(pPacket->Data,
                   s_BulkRx.DmaBuffer,
                   s_BulkRx.HeldLen);
    s_BulkRx.Held = false;
    s_BulkRx.HeldLen = 0U;

    (void)HciUsbArmBulkOutContext(pUsb, false);
    return true;
}

static void HciUsbBulkRxProcess(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !HciUsbBulkRxEnsure())
    {
        return;
    }

    while (!pUsb->BulkRxPending)
    {
        if (s_BulkRx.Held && CFifoAvail(s_BulkRx.Fifo) > 0)
        {
            (void)HciUsbBulkRxPublishHeld(pUsb);
        }

        HciUsbBulkUsbPacket_t *pPacket =
            (HciUsbBulkUsbPacket_t *)CFifoPeek(s_BulkRx.Fifo);
        if (pPacket == nullptr)
        {
            break;
        }

        const size_t PacketLen = pPacket->Length;
        const bool Ready =
            HciUsbConsumePhysicalPacket(pUsb, pPacket->Data, PacketLen);

        uint8_t *pReleased = CFifoGet(s_BulkRx.Fifo);
        if (pReleased != (uint8_t *)pPacket)
        {
            pUsb->InvalidRxCount++;
            CFifoFlush(s_BulkRx.Fifo);
            HciUsbResetBulkRx(pUsb);
            break;
        }

        if (Ready)
        {
            break;
        }
    }

    if (s_BulkRx.Held && CFifoAvail(s_BulkRx.Fifo) > 0)
    {
        (void)HciUsbBulkRxPublishHeld(pUsb);
    }
}

bool HciUsbArmBulkOut(HciUsb_t *pUsb)
{
    HciUsbBulkRxProcess(pUsb);
    return HciUsbArmBulkOutContext(pUsb, false);
}

bool HciUsbBulkOutXferIsr(HciUsb_t *pUsb, size_t TransferLen)
{
    if (pUsb == nullptr || !HciUsbBulkRxEnsure() ||
        pUsb->BulkOutMps == 0U ||
        TransferLen == 0U || TransferLen > pUsb->BulkOutMps)
    {
        if (pUsb != nullptr)
        {
            pUsb->InvalidRxCount++;
            (void)HciUsbArmBulkOutContext(pUsb, true);
            HciUsbWake(pUsb);
        }
        return true;
    }

    if (s_BulkRx.ArmedSession != s_BulkRx.Session)
    {
        if (!HciUsbArmBulkOutContext(pUsb, true))
        {
            HciUsbWake(pUsb);
        }
        return true;
    }

    const bool WasEmpty = CFifoUsed(s_BulkRx.Fifo) == 0;
    HciUsbBulkUsbPacket_t *pPacket =
        (HciUsbBulkUsbPacket_t *)CFifoPut(s_BulkRx.Fifo);

    if (pPacket == nullptr)
    {
        /*
         * Keep the completed packet in the one DMA scratch buffer. The OUT
         * endpoint is deliberately not armed again until task context frees a
         * FIFO block and moves this packet into it. No overwrite and no drop.
         */
        s_BulkRx.Held = true;
        s_BulkRx.HeldLen = (uint16_t)TransferLen;
        HciUsbWake(pUsb);
        return true;
    }

    pPacket->Length = (uint32_t)TransferLen;
    HciUsbFastCopy(pPacket->Data, s_BulkRx.DmaBuffer, TransferLen);

    /*
     * TinyUSB clears BUSY/CLAIMED before xfer_isr. Submit the same DMA scratch
     * immediately, before waking the consumer. On nRF52840 the direct DCD has
     * already written SIZE.EPOUT2 before this callback, so the hardware can
     * accept the next host packet while this copy/rearm path is still running.
     */
    const bool Rearmed = HciUsbArmBulkOutContext(pUsb, true);
    if (WasEmpty || !Rearmed)
    {
        HciUsbWake(pUsb);
    }
    return true;
}

bool HciUsbArmSyncOut(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->Configured || pUsb->SyncRxPending ||
        pUsb->BulkSerialization || pUsb->SyncAlt == 0U ||
        pUsb->SyncOutEp == 0U || usbd_edpt_busy(0U, pUsb->SyncOutEp))
    {
        return false;
    }

    return HciUsbEdptXfer(0U,
                          pUsb->SyncOutEp,
                          pUsb->SyncRxBuffer,
                          (uint16_t)sizeof(pUsb->SyncRxBuffer));
}

void HciUsbRearmRx(HciUsb_t *pUsb)
{
    (void)HciUsbArmBulkOut(pUsb);
    (void)HciUsbArmSyncOut(pUsb);
}

bool HciUsbParseBulkOut(HciUsb_t *pUsb, size_t TransferLen)
{
    if (pUsb == nullptr || TransferLen > sizeof(s_BulkRx.DmaBuffer))
    {
        return false;
    }

    return HciUsbConsumePhysicalPacket(pUsb,
                                       s_BulkRx.DmaBuffer,
                                       TransferLen);
}
