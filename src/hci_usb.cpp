/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_usb_priv.h"

#include <string.h>

enum
{
    HCI_USB_TX_VALIDATION_VERSION = 1U,
    HCI_USB_TX_VALIDATION_DEPTH = 8U,
    HCI_USB_TX_VALIDATION_FIRST_LEN = 8U,
    HCI_USB_TX_VALIDATION_LAST_LEN = 8U,
    HCI_USB_TX_VALIDATION_RECORD_LEN = 28U,
    HCI_USB_TX_VALIDATION_HEADER_LEN = 2U
};

#define HCI_USB_TX_VALIDATION_LENGTH_VALID     0x01U
#define HCI_USB_TX_VALIDATION_EVENT_CODE_VALID 0x02U

typedef struct
{
    uint32_t Sequence;
    uint16_t Length;
    uint8_t Flags;
    uint8_t EventCode;
    uint32_t Crc32;
    uint8_t First[HCI_USB_TX_VALIDATION_FIRST_LEN];
    uint8_t Last[HCI_USB_TX_VALIDATION_LAST_LEN];
} HciUsbTxValidationRecord_t;

static HciUsbTxValidationRecord_t
    s_TxValidation[HCI_USB_TX_VALIDATION_DEPTH];
static uint32_t s_TxValidationCount;

static void HciUsbWriteLe16(uint8_t *pData, uint16_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
}

static void HciUsbWriteLe32(uint8_t *pData, uint32_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
    pData[2] = (uint8_t)(Value >> 16);
    pData[3] = (uint8_t)(Value >> 24);
}

static uint32_t HciUsbCrc32(const uint8_t *pData, size_t Len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < Len; ++i)
    {
        crc ^= pData[i];
        for (unsigned bit = 0U; bit < 8U; ++bit)
        {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static bool HciUsbEventCodeValid(uint8_t Code)
{
    return Code != 0U && (Code <= 0x3EU || Code == 0xFFU);
}

static void HciUsbResetTxValidation(void)
{
    memset(s_TxValidation, 0, sizeof(s_TxValidation));
    s_TxValidationCount = 0U;
}

static void HciUsbRecordTxValidation(const uint8_t *pData, size_t Len)
{
    if (pData == nullptr || Len == 0U || Len > UINT16_MAX)
    {
        return;
    }

    const uint32_t sequence = s_TxValidationCount + 1U;
    HciUsbTxValidationRecord_t *pRecord =
        &s_TxValidation[s_TxValidationCount % HCI_USB_TX_VALIDATION_DEPTH];

    memset(pRecord, 0, sizeof(*pRecord));
    pRecord->Sequence = sequence;
    pRecord->Length = (uint16_t)Len;
    pRecord->EventCode = pData[0];
    pRecord->Crc32 = HciUsbCrc32(pData, Len);

    if (Len >= 2U && Len == (size_t)pData[1] + 2U)
    {
        pRecord->Flags |= HCI_USB_TX_VALIDATION_LENGTH_VALID;
    }
    if (HciUsbEventCodeValid(pData[0]))
    {
        pRecord->Flags |= HCI_USB_TX_VALIDATION_EVENT_CODE_VALID;
    }

    size_t keep = Len < HCI_USB_TX_VALIDATION_FIRST_LEN ?
                  Len : HCI_USB_TX_VALIDATION_FIRST_LEN;
    memcpy(pRecord->First, pData, keep);

    keep = Len < HCI_USB_TX_VALIDATION_LAST_LEN ?
           Len : HCI_USB_TX_VALIDATION_LAST_LEN;
    memcpy(pRecord->Last, &pData[Len - keep], keep);

    s_TxValidationCount = sequence;
}

/*
 * Local HCI opcode 0xFFF2 calls this before its own Command Complete is handed
 * back to USB. The returned history therefore ends at the packets that existed
 * before the diagnostic request and cannot be overwritten by the response that
 * carries the snapshot.
 */
extern "C" size_t HciUsbPlatformReadTxValidation(uint8_t *pData,
                                                  size_t Capacity)
{
    const uint32_t count = s_TxValidationCount;
    const size_t available = count < HCI_USB_TX_VALIDATION_DEPTH ?
                             (size_t)count : HCI_USB_TX_VALIDATION_DEPTH;
    const size_t required = HCI_USB_TX_VALIDATION_HEADER_LEN +
                            available * HCI_USB_TX_VALIDATION_RECORD_LEN;

    if (pData == nullptr || Capacity < required)
    {
        return 0U;
    }

    pData[0] = HCI_USB_TX_VALIDATION_VERSION;
    pData[1] = (uint8_t)available;

    const uint32_t firstSequence = count - (uint32_t)available + 1U;
    size_t offset = HCI_USB_TX_VALIDATION_HEADER_LEN;

    for (size_t i = 0U; i < available; ++i)
    {
        const uint32_t sequence = firstSequence + (uint32_t)i;
        const HciUsbTxValidationRecord_t *pRecord =
            &s_TxValidation[(sequence - 1U) % HCI_USB_TX_VALIDATION_DEPTH];

        HciUsbWriteLe32(&pData[offset], pRecord->Sequence);
        HciUsbWriteLe16(&pData[offset + 4U], pRecord->Length);
        pData[offset + 6U] = pRecord->Flags;
        pData[offset + 7U] = pRecord->EventCode;
        HciUsbWriteLe32(&pData[offset + 8U], pRecord->Crc32);
        memcpy(&pData[offset + 12U], pRecord->First,
               HCI_USB_TX_VALIDATION_FIRST_LEN);
        memcpy(&pData[offset + 20U], pRecord->Last,
               HCI_USB_TX_VALIDATION_LAST_LEN);
        offset += HCI_USB_TX_VALIDATION_RECORD_LEN;
    }

    return required;
}

static bool HciUsbOutputTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_EVENT ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_SCO ||
           Type == HCI_H4_PACKET_ISO;
}

static bool HciUsbHostTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_COMMAND ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_SCO ||
           Type == HCI_H4_PACKET_ISO;
}

static uint8_t HciUsbTxEndpoint(const HciUsb_t *pUsb, HciH4PacketType_t Type)
{
    if (pUsb == nullptr)
    {
        return 0U;
    }

    if (pUsb->BulkSerialization)
    {
        return pUsb->BulkInEp;
    }

    switch (Type)
    {
        case HCI_H4_PACKET_EVENT:
            return pUsb->EventEp;

        case HCI_H4_PACKET_ACL:
            return pUsb->BulkInEp;

        case HCI_H4_PACKET_SCO:
            return pUsb->SyncAlt != 0U ? pUsb->SyncInEp : 0U;

        case HCI_H4_PACKET_ISO:
        default:
            return 0U;
    }
}

static uint16_t HciUsbTxPacketSize(const HciUsb_t *pUsb, uint8_t EpAddr)
{
    if (pUsb == nullptr)
    {
        return 0U;
    }

    if (EpAddr == pUsb->BulkInEp)
    {
        return pUsb->BulkInMps;
    }

    if (EpAddr == pUsb->EventEp)
    {
        tusb_desc_endpoint_t endpoint;
        memcpy(&endpoint, pUsb->EventDesc, sizeof(endpoint));
        return tu_edpt_packet_size(&endpoint);
    }

    return 0U;
}

static void HciUsbClearTx(HciUsb_t *pUsb)
{
    if (pUsb == nullptr)
    {
        return;
    }

    pUsb->TxPending = false;
    pUsb->TxActive = false;
    pUsb->TxPayloadComplete = false;
    pUsb->TxZlpActive = false;
    pUsb->TxBufferBulkSerialization = false;
    pUsb->TxType = HCI_H4_PACKET_NONE;
    pUsb->TxLen = 0U;
    pUsb->TxWireLen = 0U;
}

static void HciUsbReframeTxBuffer(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->TxPending ||
        pUsb->TxBufferBulkSerialization == pUsb->BulkSerialization)
    {
        return;
    }

    /*
     * SET_INTERFACE can cancel an active IN request while the HCI packet is
     * still owned by this transport. Preserve that pending packet while
     * converting the same aligned buffer to the new alternate setting's wire
     * layout. Byte 0 stays dedicated to Bulk Serialization's H4 indicator;
     * legacy payload lives at the next word-aligned offset.
     */
    const size_t oldOffset = pUsb->TxBufferBulkSerialization
                                 ? 1U
                                 : HCI_USB_TX_LEGACY_OFFSET;
    const size_t newOffset = pUsb->BulkSerialization
                                 ? 1U
                                 : HCI_USB_TX_LEGACY_OFFSET;
    memmove(&pUsb->TxBuffer[newOffset],
            &pUsb->TxBuffer[oldOffset],
            pUsb->TxLen);
    pUsb->TxBuffer[0] = (uint8_t)pUsb->TxType;
    pUsb->TxBufferBulkSerialization = pUsb->BulkSerialization;
}

bool HciUsbKickTx(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || !pUsb->Configured || !pUsb->TxPending ||
        pUsb->TxActive || pUsb->TxZlpActive)
    {
        return false;
    }

    const uint8_t EpAddr = HciUsbTxEndpoint(pUsb, pUsb->TxType);
    if (EpAddr == 0U || usbd_edpt_busy(0U, EpAddr))
    {
        return false;
    }

    if (pUsb->TxPayloadComplete)
    {
        const uint16_t packetSize = HciUsbTxPacketSize(pUsb, EpAddr);
        if (packetSize == 0U || pUsb->TxWireLen == 0U ||
            (pUsb->TxWireLen % packetSize) != 0U)
        {
            HciUsbClearTx(pUsb);
            return true;
        }

        /*
         * Keep the zero-length transfer's data pointer in DMA-visible RAM.
         * TinyUSB's nRF5x DCD writes the supplied pointer to EPIN[n].PTR even
         * when MAXCNT is zero. TxBuffer is aligned RAM owned for the lifetime
         * of this transfer; no byte is read because the transfer length is 0.
         */
        if (!HciUsbEdptXfer(0U, EpAddr, pUsb->TxBuffer, 0U))
        {
            pUsb->TxErrorCount++;
            return false;
        }

        pUsb->TxActive = true;
        pUsb->TxZlpActive = true;
        return true;
    }

    HciUsbReframeTxBuffer(pUsb);

    uint8_t *pWire = pUsb->BulkSerialization
                         ? pUsb->TxBuffer
                         : &pUsb->TxBuffer[HCI_USB_TX_LEGACY_OFFSET];
    size_t WireLen = pUsb->TxLen;
    if (pUsb->BulkSerialization)
    {
        WireLen++;
    }

    if (WireLen > UINT16_MAX ||
        !HciUsbEdptXfer(0U, EpAddr, pWire, (uint16_t)WireLen))
    {
        pUsb->TxErrorCount++;
        return false;
    }

    /*
     * Validate the exact RAM bytes successfully handed to native Event-IN.
     * This is deliberately above the DCD: it says what the host-side bridge
     * supplied to USB, independent of how USB later packetizes or acknowledges
     * it. Bulk Serialization uses a different wire format and endpoint, so it
     * is not mixed into this legacy Event-IN history.
     */
    if (!pUsb->BulkSerialization &&
        pUsb->TxType == HCI_H4_PACKET_EVENT &&
        EpAddr == pUsb->EventEp)
    {
        HciUsbRecordTxValidation(pWire, WireLen);
    }

    pUsb->TxWireLen = WireLen;
    pUsb->TxActive = true;
    return true;
}

void HciUsbTxComplete(HciUsb_t *pUsb,
                      uint8_t EpAddr,
                      uint32_t Transferred)
{
    if (pUsb == nullptr || !pUsb->TxPending || !pUsb->TxActive)
    {
        return;
    }

    if (pUsb->TxZlpActive)
    {
        pUsb->TxActive = false;
        pUsb->TxZlpActive = false;
        if (Transferred != 0U)
        {
            pUsb->TxErrorCount++;
        }
        HciUsbClearTx(pUsb);
        return;
    }

    pUsb->TxActive = false;
    if ((size_t)Transferred != pUsb->TxWireLen)
    {
        /*
         * Some part of a successful IN request may already have reached the
         * host. Never resend the whole HCI packet after a short completion.
         */
        pUsb->TxErrorCount++;
        HciUsbClearTx(pUsb);
        return;
    }

    pUsb->TxPayloadComplete = true;
    const uint16_t packetSize = HciUsbTxPacketSize(pUsb, EpAddr);
    if (packetSize != 0U && pUsb->TxWireLen != 0U &&
        (pUsb->TxWireLen % packetSize) == 0U)
    {
        /*
         * A host read larger than an HCI packet needs a short transaction to
         * terminate the USB transfer when the packet ends exactly on the IN
         * endpoint max-packet boundary. This applies to legacy Event IN as
         * well as Bulk IN. A failed ZLP leaves only the ZLP pending, so
         * Process() retries it without retransmitting the completed payload.
         */
        (void)HciUsbKickTx(pUsb);
        return;
    }

    HciUsbClearTx(pUsb);
}

void HciUsbTxFailed(HciUsb_t *pUsb, uint8_t EpAddr)
{
    (void)EpAddr;
    if (pUsb == nullptr || !pUsb->TxPending)
    {
        return;
    }

    pUsb->TxErrorCount++;
    if (pUsb->TxZlpActive || pUsb->TxPayloadComplete)
    {
        /* Payload is already complete: retry only the terminating ZLP. */
        pUsb->TxActive = false;
        pUsb->TxZlpActive = false;
        pUsb->TxPayloadComplete = true;
        return;
    }

    /* Do not duplicate an IN packet after an aborted/partial payload transfer. */
    HciUsbClearTx(pUsb);
}

static HciUsb_t *HciUsbFromDev(DevIntrf_t *pDevIntrf)
{
    if (pDevIntrf == nullptr || pDevIntrf->pDevData == nullptr)
    {
        return nullptr;
    }
    return static_cast<HciUsb_t *>(pDevIntrf->pDevData);
}

static void HciUsbDevDisable(DevIntrf_t * const)
{
}

static void HciUsbDevEnable(DevIntrf_t * const)
{
}

static uint32_t HciUsbDevGetRate(DevIntrf_t * const)
{
    return 12000000U;
}

static uint32_t HciUsbDevSetRate(DevIntrf_t * const, uint32_t)
{
    return 12000000U;
}

static bool HciUsbRxAvailable(const HciUsb_t *pUsb, HciH4PacketType_t Type)
{
    if (pUsb == nullptr || !pUsb->Configured || !HciUsbHostTypeValid(Type))
    {
        return false;
    }

    if (Type == HCI_H4_PACKET_COMMAND && pUsb->CommandPending)
    {
        return true;
    }
    if (pUsb->BulkRxPending && pUsb->BulkRxType == Type)
    {
        return true;
    }
    return Type == HCI_H4_PACKET_SCO && pUsb->SyncRxPending;
}

static bool HciUsbDevStartRx(DevIntrf_t * const pDevIntrf, uint32_t DevAddr)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    const HciH4PacketType_t Type = (HciH4PacketType_t)DevAddr;
    if (!HciUsbRxAvailable(pUsb, Type))
    {
        return false;
    }

    pUsb->RxSelect = Type;
    return true;
}

static int HciUsbDevRxData(DevIntrf_t * const pDevIntrf,
                           uint8_t *pBuffer,
                           int BufferLen)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    if (pUsb == nullptr || pBuffer == nullptr || BufferLen <= 0)
    {
        return 0;
    }

    const uint8_t *pSource = nullptr;
    size_t Len = 0U;
    enum { RX_NONE, RX_COMMAND, RX_BULK, RX_SYNC } Source = RX_NONE;

    if (pUsb->RxSelect == HCI_H4_PACKET_COMMAND && pUsb->CommandPending)
    {
        pSource = pUsb->CommandBuffer;
        Len = pUsb->CommandLen;
        Source = RX_COMMAND;
    }
    else if (pUsb->BulkRxPending && pUsb->BulkRxType == pUsb->RxSelect)
    {
        pSource = &pUsb->BulkRxBuffer[pUsb->BulkSerialization ? 1U : 0U];
        Len = pUsb->BulkRxLen;
        Source = RX_BULK;
    }
    else if (pUsb->RxSelect == HCI_H4_PACKET_SCO && pUsb->SyncRxPending)
    {
        pSource = pUsb->SyncRxBuffer;
        Len = pUsb->SyncRxLen;
        Source = RX_SYNC;
    }

    if (Source == RX_NONE || Len == 0U || Len > (size_t)BufferLen)
    {
        return 0;
    }

    memcpy(pBuffer, pSource, Len);

    if (Source == RX_COMMAND)
    {
        pUsb->CommandPending = false;
        pUsb->CommandLen = 0U;
        pUsb->CommandCount++;
    }
    else if (Source == RX_BULK)
    {
        switch (pUsb->BulkRxType)
        {
            case HCI_H4_PACKET_COMMAND:
                pUsb->CommandCount++;
                break;
            case HCI_H4_PACKET_ACL:
                pUsb->AclOutCount++;
                break;
            case HCI_H4_PACKET_SCO:
                pUsb->ScoOutCount++;
                break;
            case HCI_H4_PACKET_ISO:
                pUsb->IsoOutCount++;
                break;
            default:
                break;
        }
        HciUsbResetBulkRx(pUsb);
        (void)HciUsbArmBulkOut(pUsb);
    }
    else
    {
        pUsb->SyncRxPending = false;
        pUsb->SyncRxLen = 0U;
        pUsb->ScoOutCount++;
        (void)HciUsbArmSyncOut(pUsb);
    }

    return (int)Len;
}

static void HciUsbDevStopRx(DevIntrf_t * const pDevIntrf)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    if (pUsb != nullptr)
    {
        pUsb->RxSelect = HCI_H4_PACKET_NONE;
    }
}

static bool HciUsbDevStartTx(DevIntrf_t * const pDevIntrf, uint32_t DevAddr)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    const HciH4PacketType_t Type = (HciH4PacketType_t)DevAddr;
    if (pUsb == nullptr || !pUsb->Configured || pUsb->TxPending ||
        !HciUsbOutputTypeValid(Type))
    {
        if (pUsb != nullptr)
        {
            pUsb->TxBusyCount++;
        }
        return false;
    }

    const uint8_t EpAddr = HciUsbTxEndpoint(pUsb, Type);
    if (EpAddr == 0U || usbd_edpt_busy(0U, EpAddr))
    {
        pUsb->TxBusyCount++;
        return false;
    }

    pUsb->TxSelect = Type;
    return true;
}

static int HciUsbDevTxData(DevIntrf_t * const pDevIntrf,
                           const uint8_t *pData,
                           int DataLen)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    if (pUsb == nullptr || pData == nullptr || DataLen <= 0 ||
        (size_t)DataLen > HCI_USB_PACKET_SIZE ||
        !HciUsbOutputTypeValid(pUsb->TxSelect) || pUsb->TxPending)
    {
        return 0;
    }

    const size_t payloadOffset = pUsb->BulkSerialization
                                     ? 1U
                                     : HCI_USB_TX_LEGACY_OFFSET;
    pUsb->TxBuffer[0] = (uint8_t)pUsb->TxSelect;
    memcpy(&pUsb->TxBuffer[payloadOffset], pData, (size_t)DataLen);

    pUsb->TxType = pUsb->TxSelect;
    pUsb->TxLen = (size_t)DataLen;
    pUsb->TxWireLen = 0U;
    pUsb->TxPending = true;
    pUsb->TxActive = false;
    pUsb->TxPayloadComplete = false;
    pUsb->TxZlpActive = false;
    pUsb->TxBufferBulkSerialization = pUsb->BulkSerialization;

    if (!HciUsbKickTx(pUsb))
    {
        HciUsbClearTx(pUsb);
        pUsb->TxBusyCount++;
        return 0;
    }

    switch (pUsb->TxType)
    {
        case HCI_H4_PACKET_EVENT:
            pUsb->EventInCount++;
            break;
        case HCI_H4_PACKET_ACL:
            pUsb->AclInCount++;
            break;
        case HCI_H4_PACKET_SCO:
            pUsb->ScoInCount++;
            break;
        case HCI_H4_PACKET_ISO:
            pUsb->IsoInCount++;
            break;
        default:
            break;
    }

    return DataLen;
}

static int HciUsbDevTxSrData(DevIntrf_t * const pDevIntrf,
                             const uint8_t *pData,
                             int DataLen)
{
    return HciUsbDevTxData(pDevIntrf, pData, DataLen);
}

static void HciUsbDevStopTx(DevIntrf_t * const pDevIntrf)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    if (pUsb != nullptr)
    {
        pUsb->TxSelect = HCI_H4_PACKET_NONE;
    }
}

static void HciUsbDevReset(DevIntrf_t * const pDevIntrf)
{
    HciUsb_t *pUsb = HciUsbFromDev(pDevIntrf);
    if (pUsb == nullptr)
    {
        return;
    }

    pUsb->CommandPending = false;
    pUsb->CommandLen = 0U;
    HciUsbResetBulkRx(pUsb);
    pUsb->SyncRxPending = false;
    pUsb->SyncRxLen = 0U;
    pUsb->RxSelect = HCI_H4_PACKET_NONE;
    pUsb->TxSelect = HCI_H4_PACKET_NONE;
    HciUsbClearTx(pUsb);
    HciUsbRearmRx(pUsb);
}

static void HciUsbDevPowerOff(DevIntrf_t * const pDevIntrf)
{
    HciUsbDevDisable(pDevIntrf);
}

static void *HciUsbDevGetHandle(DevIntrf_t * const pDevIntrf)
{
    return HciUsbFromDev(pDevIntrf);
}

bool HciUsbInit(HciUsb_t *pUsb, DevIntrfEvtHandler_t EvtCB)
{
    if (pUsb == nullptr || (g_HciUsb != nullptr && g_HciUsb != pUsb))
    {
        return false;
    }

    memset(pUsb, 0, sizeof(*pUsb));
    HciUsbResetTxValidation();

    pUsb->DevIntrf.pDevData = pUsb;
    pUsb->DevIntrf.IntPrio = 0;
    pUsb->DevIntrf.EvtCB = EvtCB;
    atomic_flag_clear(&pUsb->DevIntrf.bBusy);
    pUsb->DevIntrf.MaxRetry = 0;
    atomic_store(&pUsb->DevIntrf.EnCnt, 0);
    pUsb->DevIntrf.Type = DEVINTRF_TYPE_USB;
    pUsb->DevIntrf.bDma = false;
    pUsb->DevIntrf.bIntEn = true;
    atomic_store(&pUsb->DevIntrf.bTxReady, true);
    atomic_store(&pUsb->DevIntrf.bNoStop, false);
    pUsb->DevIntrf.Disable = HciUsbDevDisable;
    pUsb->DevIntrf.Enable = HciUsbDevEnable;
    pUsb->DevIntrf.GetRate = HciUsbDevGetRate;
    pUsb->DevIntrf.SetRate = HciUsbDevSetRate;
    pUsb->DevIntrf.StartRx = HciUsbDevStartRx;
    pUsb->DevIntrf.RxData = HciUsbDevRxData;
    pUsb->DevIntrf.StopRx = HciUsbDevStopRx;
    pUsb->DevIntrf.StartTx = HciUsbDevStartTx;
    pUsb->DevIntrf.TxData = HciUsbDevTxData;
    pUsb->DevIntrf.TxSrData = HciUsbDevTxSrData;
    pUsb->DevIntrf.StopTx = HciUsbDevStopTx;
    pUsb->DevIntrf.Reset = HciUsbDevReset;
    pUsb->DevIntrf.PowerOff = HciUsbDevPowerOff;
    pUsb->DevIntrf.GetHandle = HciUsbDevGetHandle;

    pUsb->RxSelect = HCI_H4_PACKET_NONE;
    pUsb->TxSelect = HCI_H4_PACKET_NONE;
    g_HciUsb = pUsb;
    return true;
}

void HciUsbDeinit(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || pUsb != g_HciUsb)
    {
        return;
    }

    memset(pUsb, 0, sizeof(*pUsb));
    HciUsbResetTxValidation();
    g_HciUsb = nullptr;
}

DevIntrf_t *HciUsbGetDeviceIntrf(HciUsb_t *pUsb)
{
    return pUsb != nullptr ? &pUsb->DevIntrf : nullptr;
}

void HciUsbProcess(HciUsb_t *pUsb)
{
    if (pUsb == nullptr || pUsb != g_HciUsb || !pUsb->Configured)
    {
        return;
    }

    HciUsbRearmRx(pUsb);
    if (pUsb->TxPending && !pUsb->TxActive)
    {
        (void)HciUsbKickTx(pUsb);
    }
}

bool HciUsbIsOpen(const HciUsb_t *pUsb)
{
    return pUsb != nullptr && pUsb == g_HciUsb &&
           pUsb->Configured && tud_mounted();
}

bool HciUsbBulkSerialization(const HciUsb_t *pUsb)
{
    return pUsb != nullptr && pUsb->BulkSerialization;
}
